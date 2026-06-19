#pragma once

#include "rest.h"

#include <nghttp3/nghttp3.h>
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <openssl/ssl.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/types.h>

namespace dist_cache::redis {
class Client;
}

namespace {

// ---------- Per-stream request/response buffer ----------
struct StreamCtx {
    int64_t stream_id;
    // Request side, populated by h3 callbacks before on_stream_end fires.
    std::string method;     // captured from :method pseudo-header
    std::string path;       // captured from :path  pseudo-header
    std::string req_body;   // accumulated from h3_recv_data

    // Response side, populated by submit_response.
    std::string resp_status;  // e.g. "200" (stable storage for nghttp3_nv)
    std::string resp_ctype;   // e.g. "application/json" (empty => omit header)
    std::string body;         // response body
    size_t offset = 0;        // bytes already handed to nghttp3
    bool response_submitted = false;
};

class Server;

// ---------- Connection ----------
class Connection {
public:
    //Setups nghttp2 callbacks, and initializes ngtcp2
    Connection(Server *server, const ngtcp2_cid &dcid, const ngtcp2_cid &scid,
               const ngtcp2_cid &ocid, const sockaddr *local, socklen_t local_len,
               const sockaddr *remote, socklen_t remote_len, uint32_t version,
               SSL_CTX *ssl_ctx);
    ~Connection();

    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;

    //reads a single packet using ngtcp2_conn_read_pkt
    int read_pkt(const sockaddr *remote, socklen_t remote_len,
                 const uint8_t *data, size_t datalen);

    //generate stream frame and call sendto()
    int write_pending(int fd);
    ngtcp2_tstamp expiry() const { return ngtcp2_conn_get_expiry(conn_); }
    int handle_expiry();
    bool closed() const { return closed_; }
    ngtcp2_conn *conn() { return conn_; }
    nghttp3_conn *h3() { return h3_; }
    Server *server() { return server_; }

    // h3 hooks
    int on_stream_data(int64_t stream_id, std::string_view data);
    int on_stream_end(int64_t stream_id);

    int submit_response(int64_t stream_id);

    StreamCtx *stream(int64_t id) {
        auto it = streams_.find(id);
        return it == streams_.end() ? nullptr : &it->second;
    }

    //check if stream with id exists, if not create an empty one and return reference to it
    StreamCtx &ensure_stream(int64_t id) { return streams_[id]; }
    void erase_stream(int64_t id) { streams_.erase(id); }

    // Called from the ngtcp2 handshake_completed callback so that h3 control /
    // qpack streams exist before any request bytes are delivered.
    int on_handshake_completed();

    // Peer (client) sent an HTTP/3 GOAWAY. Mark the connection as draining;
    // poll_loop will emit CONNECTION_CLOSE once all in-flight streams finish.
    void on_peer_goaway(int64_t last_stream_id);
    bool draining() const { return draining_; }
    bool idle_for_close() const { return draining_ && streams_.empty(); }
    int write_connection_close(int fd);

protected:
    // Test-only default constructor. Leaves all QUIC/TLS handles null so that
    // entry points which dereference them are not safe to call, but lets unit
    // tests exercise pure-C++ bookkeeping (streams_, draining_, etc.) without
    // driving a real handshake. Production code must use the parameterized
    // ctor above.
    Connection() = default;

    int init_h3();

    Server *server_ = nullptr;
    ngtcp2_conn *conn_ = nullptr;
    SSL *ssl_ = nullptr;
    ngtcp2_crypto_ossl_ctx *ossl_ctx_ = nullptr;
    nghttp3_conn *h3_ = nullptr;
    ngtcp2_crypto_conn_ref conn_ref_{};
    sockaddr_storage remote_addr_{};
    socklen_t remote_addrlen_ = 0;
    sockaddr_storage local_addr_{};
    socklen_t local_addrlen_ = 0;
    std::map<int64_t, StreamCtx> streams_;
    bool handshake_done_ = false;
    bool closed_ = false;
    bool draining_ = false;
    int64_t peer_goaway_id_ = -1;
};

// ---------- Server ----------
class Server {
public:
    Server() = default;
    virtual ~Server();

    int run(const char *host, const char *port, const char *cert, const char *key);

    virtual dist_cache::rest::Response handle_request(std::string_view method,
                                                      std::string_view target,
                                                      std::string_view body);

    // REST router store, populated by run() once the redis connection is up.
    // Connections call this from submit_response() to dispatch requests.
    dist_cache::rest::Store *store() { return store_.get(); }

protected:
    virtual int init_backend();

    int setup_socket(const char *host, const char *port);
    int setup_ssl(const char *cert, const char *key);
    int read_socket();
    void poll_loop();
    void on_packet(const sockaddr *remote, socklen_t remote_len,
                   const uint8_t *data, size_t datalen);
    void associate_cids(Connection *conn);
    void dissociate_cids(const Connection *conn);
    void sweep_closed();

    int fd_ = -1;
    SSL_CTX *ssl_ctx_ = nullptr;
    sockaddr_storage local_addr_{};
    socklen_t local_addrlen_ = 0;
    // Owns each connection by its first client Initial DCID. cid_index_ maps
    // every active destination CID to the owned Connection for packet routing.
    std::map<std::string, std::unique_ptr<Connection>> conns_;
    std::map<std::string, Connection *> cid_index_;

    // Backing storage. Owned by Server so its lifetime spans every Connection
    // and every in-flight stream. Both are null until run() initializes them.
    std::unique_ptr<dist_cache::redis::Client> redis_;
    std::unique_ptr<dist_cache::rest::Store>   store_;
    std::string backend_detail_;
};

// ---------- nghttp3 callbacks ----------
int h3_recv_header(nghttp3_conn *, int64_t stream_id, int32_t,
                   nghttp3_rcbuf *, nghttp3_rcbuf *, uint8_t, void *, void *);
int h3_end_headers(nghttp3_conn *, int64_t stream_id, int, void *, void *);
int h3_recv_data(nghttp3_conn *, int64_t, const uint8_t *, size_t, void *, void *);
int h3_end_stream(nghttp3_conn *, int64_t stream_id, void *, void *);
int h3_stop_sending(nghttp3_conn *, int64_t, uint64_t, void *, void *);
int h3_reset_stream(nghttp3_conn *, int64_t, uint64_t, void *, void *);
int h3_deferred_consume(nghttp3_conn *, int64_t, size_t, void *, void *);
int h3_acked_stream_data(nghttp3_conn *, int64_t, uint64_t, void *, void *);
int h3_go_away(nghttp3_conn *, int64_t, void *);

// ---------- ngtcp2 callbacks ----------
int ng_recv_stream_data(ngtcp2_conn *, uint32_t flags, int64_t stream_id,
                        uint64_t offset, const uint8_t *data, size_t datalen,
                        void *user_data, void *stream_user_data);
int ng_acked_stream_data(ngtcp2_conn *, int64_t, uint64_t, uint64_t, void *, void *);
int ng_stream_open(ngtcp2_conn *, int64_t, void *);
int ng_stream_close(ngtcp2_conn *, uint32_t, int64_t, uint64_t, void *, void *);
int ng_extend_max_remote_streams_bidi(ngtcp2_conn *, uint64_t, void *);
int ng_handshake_completed(ngtcp2_conn *, void *);
int ng_stream_stop_sending(ngtcp2_conn *, int64_t, uint64_t, void *, void *);
int ng_stream_reset(ngtcp2_conn *, int64_t, uint64_t, uint64_t, void *, void *);

}  // namespace