// Minimal HTTP/3 server using ngtcp2 + nghttp3 + OpenSSL 3.5 QUIC API
// (via ngtcp2_crypto_ossl). NOT production-ready: single-threaded, no
// migration handling, no 0-RTT, no retry tokens, minimal error recovery.
// Intended as a starting skeleton; reference ngtcp2/examples/server.cc
// for the canonical, fully-featured implementation.
//
// Usage: dist_cache.exe <host> <port> <cert.pem> <key.pem>
//   curl --http3-only -k https://<host>:<port>/

#include "server.h"

#include "redis_client.h"
#include "rest.h"

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>
#include <nghttp3/nghttp3.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr size_t kMaxUdpPayload = 1452;
constexpr std::string_view kAlpnH3 = "\x02h3";  // length-prefixed wire form

ngtcp2_tstamp now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void rand_cb(uint8_t *dest, size_t destlen, const ngtcp2_rand_ctx *) {
    RAND_bytes(dest, static_cast<int>(destlen));
}

int get_new_cid_cb(ngtcp2_conn *, ngtcp2_cid *cid, uint8_t *token,
                   size_t cidlen, void *) {
    if (RAND_bytes(cid->data, static_cast<int>(cidlen)) != 1) return NGTCP2_ERR_CALLBACK_FAILURE;
    cid->datalen = cidlen;
    if (RAND_bytes(token, NGTCP2_STATELESS_RESET_TOKENLEN) != 1) return NGTCP2_ERR_CALLBACK_FAILURE;
    return 0;
}

// Forward declarations
class Connection;
class Server;

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
int ng_extend_max_local_streams_bidi(ngtcp2_conn *, uint64_t, void *);
int ng_handshake_completed(ngtcp2_conn *, void *);
int ng_stream_stop_sending(ngtcp2_conn *, int64_t, uint64_t, void *, void *);
int ng_stream_reset(ngtcp2_conn *, int64_t, uint64_t, uint64_t, void *, void *);

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
};

// ---------- Redis-backed rest::Store --------------------------------------
//
// Adapter that satisfies the transport-agnostic rest::Store interface by
// delegating every op to a hiredis client. Used by the HTTP/3 server below;
// rest_test keeps using the in-memory base class.
//
// All JSON ops share a single Redis key so the spec's "single root JSON
// document" semantics are preserved. Path arguments ("$", "$.user.age", ...)
// flow straight to Redis 8's native JSON commands, which understand them.
class RedisStore : public dist_cache::rest::Store {
public:
    explicit RedisStore(dist_cache::redis::Client *client) : client_(client) {}

    void kv_set(const std::string &key, std::string value,
                std::optional<int64_t> expiry_sec) override {
        client_->kv_set(key, value, expiry_sec);
    }
    std::optional<std::string> kv_get(const std::string &key) override {
        return client_->kv_get(key);
    }
    bool kv_delete(const std::string &key) override {
        return client_->kv_delete(key);
    }

    std::optional<nlohmann::json> json_get(const std::string &path) const override {
        try {
            auto v = client_->json_get(kJsonKey, path);
            if (!v) return std::nullopt;
            // JSON.GET on a non-existent path returns an empty array; treat
            // that as "not found" so the REST layer can emit 404.
            if (v->is_array() && v->empty()) return std::nullopt;
            return v;
        } catch (const dist_cache::redis::RedisError &) {
            return std::nullopt;
        }
    }
    bool json_set(const std::string &path, nlohmann::json value,
                  std::optional<int64_t> expiry_sec) override {
        // Spec: expiry_sec only allowed at root. Matches the in-memory rule.
        if (expiry_sec && path != "$") return false;
        try {
            client_->json_set(kJsonKey, path, value, expiry_sec);
            return true;
        } catch (const dist_cache::redis::RedisError &) {
            // Most commonly: parent path missing on a sub-path SET.
            return false;
        }
    }
    bool json_delete(const std::string &path) override {
        try { return client_->json_delete(kJsonKey, path); }
        catch (const dist_cache::redis::RedisError &) { return false; }
    }

private:
    static constexpr const char *kJsonKey = "dc:json";
    dist_cache::redis::Client *client_;
};

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

    //
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
    ~Server();

    int run(const char *host, const char *port, const char *cert, const char *key);

    // REST router store, populated by run() once the redis connection is up.
    // Connections call this from submit_response() to dispatch requests.
    dist_cache::rest::Store *store() { return store_.get(); }

protected:
    int setup_socket(const char *host, const char *port);
    int setup_ssl(const char *cert, const char *key);
    int read_socket();
    void poll_loop();
    void on_packet(const sockaddr *remote, socklen_t remote_len,
                   const uint8_t *data, size_t datalen);
    void sweep_closed();

    int fd_ = -1;
    SSL_CTX *ssl_ctx_ = nullptr;
    sockaddr_storage local_addr_{};
    socklen_t local_addrlen_ = 0;
    // Map DCID bytes -> Connection. Real impl needs multi-CID tracking.
    std::map<std::string, std::unique_ptr<Connection>> conns_;

    // Backing storage. Owned by Server so its lifetime spans every Connection
    // and every in-flight stream. Both are null until run() initializes them.
    std::unique_ptr<dist_cache::redis::Client> redis_;
    std::unique_ptr<dist_cache::rest::Store>   store_;
};

// ngtcp2 needs an SSL* lookup from a conn ref
ngtcp2_conn *get_conn_from_ref(ngtcp2_crypto_conn_ref *ref) {
    return static_cast<Connection *>(ref->user_data)->conn();
}

// ---------- Connection impl ----------
Connection::Connection(Server *server, const ngtcp2_cid &dcid,
                       const ngtcp2_cid &scid, const ngtcp2_cid &ocid,
                       const sockaddr *local, socklen_t local_len,
                       const sockaddr *remote, socklen_t remote_len,
                       uint32_t version, SSL_CTX *ssl_ctx)
    : server_(server) {
    std::memcpy(&local_addr_, local, local_len);
    local_addrlen_ = local_len;
    std::memcpy(&remote_addr_, remote, remote_len);
    remote_addrlen_ = remote_len;

    // Pick our own server-side SCID
    ngtcp2_cid our_scid;
    our_scid.datalen = NGTCP2_MIN_INITIAL_DCIDLEN;
    RAND_bytes(our_scid.data, static_cast<int>(our_scid.datalen));

    ngtcp2_path path;
    path.local.addr = reinterpret_cast<sockaddr *>(&local_addr_);
    path.local.addrlen = local_addrlen_;
    path.remote.addr = reinterpret_cast<sockaddr *>(&remote_addr_);
    path.remote.addrlen = remote_addrlen_;
    path.user_data = nullptr;

    ngtcp2_callbacks callbacks{};
    callbacks.recv_client_initial         = ngtcp2_crypto_recv_client_initial_cb;
    callbacks.recv_crypto_data            = ngtcp2_crypto_recv_crypto_data_cb;
    callbacks.encrypt                     = ngtcp2_crypto_encrypt_cb;
    callbacks.decrypt                     = ngtcp2_crypto_decrypt_cb;
    callbacks.hp_mask                     = ngtcp2_crypto_hp_mask_cb;
    callbacks.update_key                  = ngtcp2_crypto_update_key_cb;
    callbacks.delete_crypto_aead_ctx      = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
    callbacks.delete_crypto_cipher_ctx    = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
    callbacks.get_path_challenge_data     = ngtcp2_crypto_get_path_challenge_data_cb;
    callbacks.version_negotiation         = ngtcp2_crypto_version_negotiation_cb;

    callbacks.handshake_completed         = ng_handshake_completed;
    callbacks.recv_stream_data            = ng_recv_stream_data;
    callbacks.acked_stream_data_offset    = ng_acked_stream_data;
    callbacks.stream_open                 = ng_stream_open;
    callbacks.stream_close                = ng_stream_close;
    callbacks.extend_max_local_streams_bidi = ng_extend_max_local_streams_bidi;
    callbacks.rand                        = rand_cb;
    callbacks.get_new_connection_id       = get_new_cid_cb;
    callbacks.stream_stop_sending         = ng_stream_stop_sending;
    callbacks.stream_reset                = ng_stream_reset;

    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    settings.initial_ts = now_ns();

    ngtcp2_transport_params params;
    ngtcp2_transport_params_default(&params);
    params.initial_max_stream_data_bidi_local  = 256 * 1024;
    params.initial_max_stream_data_bidi_remote = 256 * 1024;
    params.initial_max_stream_data_uni         = 256 * 1024;
    params.initial_max_data                    = 1024 * 1024;
    params.initial_max_streams_bidi            = 100;
    params.initial_max_streams_uni             = 3;
    params.max_idle_timeout                    = 30 * NGTCP2_SECONDS;
    params.original_dcid                       = ocid;
    params.original_dcid_present               = 1;

    // ngtcp2_conn_server_new wants the peer's SCID (i.e. the client's SCID)
    // as its `dcid` argument; our local `scid` parameter holds that value.
    if (ngtcp2_conn_server_new(&conn_, &scid, &our_scid, &path, version,
                               &callbacks, &settings, &params, nullptr,
                               this) != 0) {
        std::fprintf(stderr, "ngtcp2_conn_server_new failed\n");
        return;
    }

    ssl_ = SSL_new(ssl_ctx);
    SSL_set_accept_state(ssl_);
    SSL_set_quic_tls_early_data_enabled(ssl_, 0);

    conn_ref_.get_conn = get_conn_from_ref;
    conn_ref_.user_data = this;
    SSL_set_app_data(ssl_, &conn_ref_);
    if (ngtcp2_crypto_ossl_configure_server_session(ssl_) != 0) {
        std::fprintf(stderr, "ngtcp2_crypto_ossl_configure_server_session failed\n");
    }
    ngtcp2_conn_set_tls_native_handle(conn_, ssl_);
}

Connection::~Connection() {
    if (h3_) nghttp3_conn_del(h3_);
    if (conn_) ngtcp2_conn_del(conn_);
    if (ssl_) SSL_free(ssl_);
}

int Connection::init_h3() {
    nghttp3_callbacks cb{};
    cb.recv_data         = h3_recv_data;
    cb.deferred_consume  = h3_deferred_consume;
    cb.acked_stream_data = h3_acked_stream_data;
    cb.stream_close      = [](nghttp3_conn *, int64_t, uint64_t,
                              void *, void *) -> int { return 0; };
    cb.recv_header       = h3_recv_header;
    cb.end_headers       = h3_end_headers;
    cb.stop_sending      = h3_stop_sending;
    cb.reset_stream      = h3_reset_stream;
    cb.end_stream        = h3_end_stream;
    cb.shutdown          = h3_go_away;

    nghttp3_settings settings;
    nghttp3_settings_default(&settings);
    settings.qpack_max_dtable_capacity = 4096;
    settings.qpack_blocked_streams     = 100;

    const nghttp3_mem *mem = nghttp3_mem_default();
    if (nghttp3_conn_server_new(&h3_, &cb, &settings, mem, this) != 0)
        return -1;

    int64_t ctrl, qenc, qdec;
    if (ngtcp2_conn_open_uni_stream(conn_, &ctrl, nullptr) != 0) return -1;
    if (ngtcp2_conn_open_uni_stream(conn_, &qenc, nullptr) != 0) return -1;
    if (ngtcp2_conn_open_uni_stream(conn_, &qdec, nullptr) != 0) return -1;
    if (nghttp3_conn_bind_control_stream(h3_, ctrl) != 0) return -1;
    if (nghttp3_conn_bind_qpack_streams(h3_, qenc, qdec) != 0) return -1;
    return 0;
}

int Connection::read_pkt(const sockaddr *remote, socklen_t remote_len,
                         const uint8_t *data, size_t datalen) {
    ngtcp2_path path;
    path.local.addr = reinterpret_cast<sockaddr *>(&local_addr_);
    path.local.addrlen = local_addrlen_;
    path.remote.addr = const_cast<sockaddr *>(remote);
    path.remote.addrlen = remote_len;
    path.user_data = nullptr;

    ngtcp2_pkt_info pi{};
    int rv = ngtcp2_conn_read_pkt(conn_, &path, &pi, data, datalen, now_ns());
    if (rv != 0) {
        std::fprintf(stderr, "ngtcp2_conn_read_pkt: %s\n", ngtcp2_strerror(rv));
        if (rv == NGTCP2_ERR_DRAINING || rv == NGTCP2_ERR_DROP_CONN ||
            rv == NGTCP2_ERR_CRYPTO) {
            closed_ = true;
        }
        return rv;
    }
    return 0;
}

int Connection::write_pending(int fd) {
    std::array<uint8_t, kMaxUdpPayload> buf;

    //In QUIC, a "path" consists of a local address and a remote address, ngtcp2_path_storage is handle to 
    //buffer for path info. ngtcp2_pkt_info is used to get info about the packet being written, such as the stream id and whether it's the end of the stream.
    ngtcp2_path_storage ps;
    ngtcp2_path_storage_zero(&ps);

    //handle to buffer for meta data about a pkt
    ngtcp2_pkt_info pi{};

    for (;;) {
        int64_t stream_id = -1;
        int fin = 0;
        nghttp3_vec vec[16];
        nghttp3_ssize sveccnt = 0;

        if (h3_) { //if handshake is done
            /*
                Ask nghttp3 for stream payload
                nghttp3 returns:
                    stream_id — which QUIC stream this data belongs to (e.g. an HTTP/3 response or a control/QPACK stream)
                    AND
                    fin — whether this chunk ends the stream,
                    AND
                    [vecvec[0..sveccnt) — iovecs of QPACK-encoded headers / body bytes / control frames.
            */
            sveccnt = nghttp3_conn_writev_stream(h3_, &stream_id, &fin, vec, 16);
            if (sveccnt < 0) {
                std::fprintf(stderr, "nghttp3_conn_writev_stream: %s\n",
                             nghttp3_strerror(static_cast<int>(sveccnt)));
                closed_ = true;
                return -1;
            }
        }

        uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_MORE;
        if (fin) flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;

        ngtcp2_ssize ndatalen = 0;

        // ngtcp2_conn_writev_stream creates the actual QUIC STREAM frame. 
        // If h3_ not done, nghttp3_conn_writev_stream didn't fill out sveccnt - so create handshake / ACK-only packet.
        ngtcp2_ssize n = ngtcp2_conn_writev_stream(
            conn_, &ps.path, &pi, buf.data(), buf.size(), &ndatalen, flags,
            stream_id, reinterpret_cast<const ngtcp2_vec *>(vec),
            static_cast<size_t>(sveccnt), now_ns());

        if (n < 0) {
            if (n == NGTCP2_ERR_WRITE_MORE) {
                if (h3_ && ndatalen >= 0) {
                    /*
                        If ngtcp2_conn_writev_stream returns NGTCP2_ERR_WRITE_MORE, 
                        it means the packet is not fully filled with the data we provided (vec), 
                        and we need to call it again to get the next chunk of data for the same stream. 
                        
                        This is a flow control mechanism: ngtcp2 is telling us 
                        "I can't fit all the data you want to send in one packet, 
                        so give me the next chunk for the same stream". 
                        
                        We should call nghttp3_conn_writev_stream again to get the next chunk of data 
                        for the same stream_id, and then call ngtcp2_conn_writev_stream again with 
                        that new data. 

                        ie, nghttp3_conn_add_write_offset is adding offset to the stream's write cursor,
                        in the *internal buffers of nghttp3*... so that the next call to nghttp3_conn_writev_stream 
                        will give us the next chunk of data for that stream.
                        
                        This loop continues until ngtcp2_conn_writev_stream returns 0, which means all data has been sent.
                    */
                    nghttp3_conn_add_write_offset(h3_, stream_id, ndatalen);
                }
                continue;
            }
            if (n == NGTCP2_ERR_STREAM_DATA_BLOCKED ||
                n == NGTCP2_ERR_STREAM_SHUT_WR) {
                /*
                    NGTCP2_ERR_STREAM_DATA_BLOCKED - QUIC stream is flow controlled and rejecting writes.
                    NGTCP2_ERR_STREAM_SHUT_WR - QUIC stream is closed for writing, mostly because of RESET_STREAM from client.
                    
                    In this case, we should inform nghttp3 that the stream is blocked... ie, 
                    nghttp3_conn_writev_stream will not generate data from this stream untill it is marked
                    unblocked
                */
                if (h3_) nghttp3_conn_block_stream(h3_, stream_id);
                continue;
            }
            std::fprintf(stderr, "ngtcp2_conn_writev_stream: %s\n",
                         ngtcp2_strerror(static_cast<int>(n)));
            closed_ = true;
            return -1;
        }
        if (n == 0) break; // No more data to send at the moment
        if (h3_ && ndatalen >= 0) { // Mark the data as sent in nghttp3's internal buffers, so it can update its state and provide the next chunk on the next call.
            nghttp3_conn_add_write_offset(h3_, stream_id, ndatalen);
        }

        //send the frame created as UDP packet to client
        ssize_t sent = sendto(fd, buf.data(), static_cast<size_t>(n), 0,
                              ps.path.remote.addr, ps.path.remote.addrlen);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            std::perror("sendto");
            closed_ = true;
            return -1;
        }
    }
    return 0;
}

int Connection::on_handshake_completed() {
    if (h3_) return 0;
    if (init_h3() != 0) {
        std::fprintf(stderr, "init_h3 failed\n");
        closed_ = true;
        return -1;
    }
    handshake_done_ = true;
    return 0;
}

int Connection::handle_expiry() {
    int rv = ngtcp2_conn_handle_expiry(conn_, now_ns());
    if (rv != 0) {
        std::fprintf(stderr, "handle_expiry: %s\n", ngtcp2_strerror(rv));
        closed_ = true;
    }
    return rv;
}

void Connection::on_peer_goaway(int64_t last_stream_id) {
    peer_goaway_id_ = last_stream_id;
    draining_ = true;
}

int Connection::write_connection_close(int fd) {
    if (closed_ || !conn_) {
        closed_ = true;
        return 0;
    }
    std::array<uint8_t, kMaxUdpPayload> buf;
    ngtcp2_path_storage ps;
    ngtcp2_path_storage_zero(&ps);
    ngtcp2_pkt_info pi{};
    ngtcp2_ccerr ccerr;
    ngtcp2_ccerr_default(&ccerr);
    ngtcp2_ssize n = ngtcp2_conn_write_connection_close(
        conn_, &ps.path, &pi, buf.data(), buf.size(), &ccerr, now_ns());
    if (n > 0) {
        ssize_t sent = sendto(fd, buf.data(), static_cast<size_t>(n), 0,
                              ps.path.remote.addr, ps.path.remote.addrlen);
        if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            std::perror("sendto (CONNECTION_CLOSE)");
        }
    }
    closed_ = true;
    return 0;
}

int Connection::on_stream_data(int64_t stream_id, std::string_view data) {
    // Accumulate the request body so submit_response can hand it to the REST
    // router once the peer signals end-of-stream.
    auto &s = ensure_stream(stream_id);
    s.req_body.append(data);
    return 0;
}

int Connection::on_stream_end(int64_t stream_id) {
    return submit_response(stream_id);
}

int Connection::submit_response(int64_t stream_id) {
    auto &s = ensure_stream(stream_id);
    s.stream_id = stream_id;

    // Route through the OpenAPI-spec REST handler. The store backing this
    // call is Redis when running under dist_cache.exe (see Server::run).
    auto resp = dist_cache::rest::handle(
        *server_->store(), s.method, s.path, s.req_body);

    // Stable storage for header values referenced by the data reader below.
    s.resp_status = std::to_string(resp.status);
    s.resp_ctype  = std::move(resp.content_type);
    s.body        = std::move(resp.body);
    s.offset      = 0;

    /*
        associate stream_id from this stream context,
        so that when read_data callback is called with this stream_id, we can
        get the response body and offset from the stream context
    */
    nghttp3_conn_set_stream_user_data(h3_, stream_id, &s);

    /*
        typedef struct {
            uint8_t *name;    // Pointer to the header name (e.g., "content-type")
            uint8_t *value;   // Pointer to the header value (e.g., "application/json")
            size_t namelen;   // Length of the name string
            size_t valuelen;  // Length of the value string
            uint8_t flags;    // Configuration flags
        } nghttp3_nv;
    */
    std::vector<nghttp3_nv> nva;
    nva.reserve(3);
    nva.push_back({(uint8_t *)":status",
                   (uint8_t *)s.resp_status.data(),
                   7, s.resp_status.size(), NGHTTP3_NV_FLAG_NONE});
    nva.push_back({(uint8_t *)"server",
                   (uint8_t *)"dist_cache/0.1",
                   6, 14, NGHTTP3_NV_FLAG_NONE});
    if (!s.resp_ctype.empty()) {
        nva.push_back({(uint8_t *)"content-type",
                       (uint8_t *)s.resp_ctype.data(),
                       12, s.resp_ctype.size(), NGHTTP3_NV_FLAG_NONE});
    }

    /*
        Because nghttp3 is designed to be extremely memory-efficient, it doesn't copy the entire payload 
        into its own internal memory. Instead, it uses this "reader" to pull data only when it’s actually ready to send it out over the wire.
    
        read_data callback is invoked whenever it pulls.
    */
    nghttp3_data_reader dr{};
    dr.read_data = [](nghttp3_conn *, int64_t /*sid*/, nghttp3_vec *vec,
                      size_t veccnt, uint32_t *pflags, void *,
                      void *stream_user_data) -> nghttp3_ssize {
        auto *ctx = static_cast<StreamCtx *>(stream_user_data);
        if (!ctx || veccnt == 0) return 0;
        const size_t remaining = ctx->body.size() - ctx->offset;
        if (remaining == 0) {
            *pflags = NGHTTP3_DATA_FLAG_EOF;
            return 0;
        }
        vec[0].base = reinterpret_cast<uint8_t *>(ctx->body.data() + ctx->offset);
        vec[0].len = remaining;
        ctx->offset += remaining;
        if (ctx->offset == ctx->body.size()) {
            *pflags = NGHTTP3_DATA_FLAG_EOF;
        }
        return 1;
    };

    //call initiates response from server, read_data called from stream_id
    int rv = nghttp3_conn_submit_response(h3_, stream_id, nva.data(),
                                          nva.size(), &dr);
    if (rv != 0) {
        std::fprintf(stderr, "submit_response: %s\n", nghttp3_strerror(rv));

        // If submission fails, consider clearing the user data
        nghttp3_conn_set_stream_user_data(h3_, stream_id, nullptr);
        return rv;
    }

    return 0;
}

// ---------- ngtcp2 callbacks impl ----------
int ng_recv_stream_data(ngtcp2_conn *, uint32_t flags, int64_t stream_id,
                        uint64_t, const uint8_t *data, size_t datalen,
                        void *user_data, void *) {
    auto *c = static_cast<Connection *>(user_data);
    if (!c->h3()) return 0;
    nghttp3_ssize n = nghttp3_conn_read_stream(
        c->h3(), stream_id, data, datalen,
        (flags & NGTCP2_STREAM_DATA_FLAG_FIN) ? 1 : 0);
    if (n < 0) {
        std::fprintf(stderr, "nghttp3_conn_read_stream: %s\n",
                     nghttp3_strerror(static_cast<int>(n)));
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    ngtcp2_conn_extend_max_stream_offset(c->conn(), stream_id, n);
    ngtcp2_conn_extend_max_offset(c->conn(), n);
    return 0;
}

int ng_acked_stream_data(ngtcp2_conn *, int64_t stream_id, uint64_t,
                         uint64_t datalen, void *user_data, void *) {
    auto *c = static_cast<Connection *>(user_data);
    if (c->h3()) {
        nghttp3_conn_add_ack_offset(c->h3(), stream_id, datalen);
    }
    return 0;
}

int ng_stream_open(ngtcp2_conn *, int64_t, void *) { return 0; }

int ng_stream_close(ngtcp2_conn *, uint32_t flags, int64_t stream_id,
                    uint64_t app_error_code, void *user_data, void *) {
    auto *c = static_cast<Connection *>(user_data);
    if (c->h3()) {
        if (!(flags & NGTCP2_STREAM_CLOSE_FLAG_APP_ERROR_CODE_SET))
            app_error_code = NGHTTP3_H3_NO_ERROR;
        int rv = nghttp3_conn_close_stream(c->h3(), stream_id, app_error_code);
        if (rv != 0 && rv != NGHTTP3_ERR_STREAM_NOT_FOUND) {
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }
    }
    c->erase_stream(stream_id);
    return 0;
}

int ng_extend_max_local_streams_bidi(ngtcp2_conn *, uint64_t, void *) {
    return 0;
}

int ng_handshake_completed(ngtcp2_conn *, void *user_data) {
    auto *c = static_cast<Connection *>(user_data);
    return c->on_handshake_completed() == 0 ? 0 : NGTCP2_ERR_CALLBACK_FAILURE;
}

int ng_stream_stop_sending(ngtcp2_conn *, int64_t stream_id, uint64_t,
                           void *user_data, void *) {
    auto *c = static_cast<Connection *>(user_data);
    if (c->h3()) nghttp3_conn_shutdown_stream_read(c->h3(), stream_id);
    return 0;
}

int ng_stream_reset(ngtcp2_conn *, int64_t stream_id, uint64_t, uint64_t,
                    void *user_data, void *) {
    auto *c = static_cast<Connection *>(user_data);
    if (c->h3()) nghttp3_conn_shutdown_stream_read(c->h3(), stream_id);
    return 0;
}

// ---------- nghttp3 callbacks impl ----------
int h3_recv_header(nghttp3_conn *, int64_t stream_id, int32_t,
                   nghttp3_rcbuf *name, nghttp3_rcbuf *value, uint8_t,
                   void *conn_user_data, void *) {
    // Capture the request pseudo-headers (:method, :path) into the per-stream
    // context so submit_response can hand them to the REST router. Everything
    // else is dropped on the floor (we don't honor request content-type, etc).
    auto *c = static_cast<Connection *>(conn_user_data);
    auto nbuf = nghttp3_rcbuf_get_buf(name);
    auto vbuf = nghttp3_rcbuf_get_buf(value);
    std::string_view n(reinterpret_cast<const char *>(nbuf.base), nbuf.len);
    std::string_view v(reinterpret_cast<const char *>(vbuf.base), vbuf.len);
    auto &s = c->ensure_stream(stream_id);
    if (n == ":method") s.method.assign(v);
    else if (n == ":path") s.path.assign(v);
    return 0;
}

int h3_end_headers(nghttp3_conn *, int64_t, int, void *, void *) { return 0; }

int h3_recv_data(nghttp3_conn *, int64_t stream_id, const uint8_t *data,
                 size_t datalen, void *conn_user_data, void *) {
    auto *c = static_cast<Connection *>(conn_user_data);
    return c->on_stream_data(
        stream_id,
        std::string_view(reinterpret_cast<const char *>(data), datalen));
}

int h3_end_stream(nghttp3_conn *, int64_t stream_id, void *conn_user_data, void *) {
    auto *c = static_cast<Connection *>(conn_user_data);
    return c->on_stream_end(stream_id);
}

int h3_stop_sending(nghttp3_conn *, int64_t stream_id, uint64_t code,
                    void *conn_user_data, void *) {
    auto *c = static_cast<Connection *>(conn_user_data);
    ngtcp2_conn_shutdown_stream_read(c->conn(), 0, stream_id, code);
    return 0;
}

int h3_reset_stream(nghttp3_conn *, int64_t stream_id, uint64_t code,
                    void *conn_user_data, void *) {
    auto *c = static_cast<Connection *>(conn_user_data);
    ngtcp2_conn_shutdown_stream_write(c->conn(), 0, stream_id, code);
    return 0;
}

int h3_deferred_consume(nghttp3_conn *, int64_t stream_id, size_t consumed,
                        void *conn_user_data, void *) {
    auto *c = static_cast<Connection *>(conn_user_data);
    ngtcp2_conn_extend_max_stream_offset(c->conn(), stream_id, consumed);
    ngtcp2_conn_extend_max_offset(c->conn(), consumed);
    return 0;
}

int h3_acked_stream_data(nghttp3_conn *, int64_t, uint64_t, void *, void *) {
    return 0;
}

// Fired by nghttp3 when the peer (client) sends an HTTP/3 GOAWAY frame on
// its control stream. We mark the Connection as draining; once all in-flight
// streams finish, poll_loop will emit CONNECTION_CLOSE for this connection.
int h3_go_away(nghttp3_conn *, int64_t id, void *conn_user_data) {
    auto *c = static_cast<Connection *>(conn_user_data);
    std::fprintf(stderr, "h3 GOAWAY from peer: last id=%lld\n",
                 static_cast<long long>(id));
    c->on_peer_goaway(id);
    return 0;
}

// ---------- Server impl ----------
Server::~Server() {
    if (fd_ >= 0) ::close(fd_);
    if (ssl_ctx_) SSL_CTX_free(ssl_ctx_);
}

int Server::setup_socket(const char *host, const char *port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;
    addrinfo *res = nullptr;
    if (getaddrinfo(host, port, &hints, &res) != 0) {
        std::perror("getaddrinfo");
        return -1;
    }
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> guard(res, &freeaddrinfo);

    fd_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd_ < 0) { std::perror("socket"); return -1; }
    int on = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    if (bind(fd_, res->ai_addr, res->ai_addrlen) < 0) {
        std::perror("bind"); return -1;
    }
    std::memcpy(&local_addr_, res->ai_addr, res->ai_addrlen);
    local_addrlen_ = res->ai_addrlen;
    return 0;
}

int Server::setup_ssl(const char *cert, const char *key) {
    ssl_ctx_ = SSL_CTX_new(TLS_server_method());
    if (!ssl_ctx_) return -1;
    // ngtcp2 1.16+: per-context configure helper was removed; configuration
    // now happens per-session via ngtcp2_crypto_ossl_configure_server_session.
    SSL_CTX_set_min_proto_version(ssl_ctx_, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(ssl_ctx_, TLS1_3_VERSION);

    SSL_CTX_set_alpn_select_cb(
        ssl_ctx_,
        [](SSL *, const unsigned char **out, unsigned char *outlen,
           const unsigned char *in, unsigned int inlen, void *) -> int {
            const unsigned char *alpn = reinterpret_cast<const unsigned char *>(kAlpnH3.data());
            unsigned int alpnlen = static_cast<unsigned int>(kAlpnH3.size());
            for (unsigned int i = 0; i + 1 + in[i] <= inlen; i += 1 + in[i]) {
                if (in[i] == alpn[0] && std::memcmp(&in[i + 1], alpn + 1, alpn[0]) == 0) {
                    *out = &in[i + 1];
                    *outlen = in[i];
                    return SSL_TLSEXT_ERR_OK;
                }
            }
            return SSL_TLSEXT_ERR_ALERT_FATAL;
        },
        nullptr);

    if (SSL_CTX_use_certificate_chain_file(ssl_ctx_, cert) != 1) {
        ERR_print_errors_fp(stderr); return -1;
    }
    if (SSL_CTX_use_PrivateKey_file(ssl_ctx_, key, SSL_FILETYPE_PEM) != 1) {
        ERR_print_errors_fp(stderr); return -1;
    }
    return 0;
}

void Server::on_packet(const sockaddr *remote, socklen_t remote_len,
                       const uint8_t *data, size_t datalen) {
    ngtcp2_version_cid vc;
    int rv = ngtcp2_pkt_decode_version_cid(&vc, data, datalen,
                                           NGTCP2_MAX_CIDLEN);
    if (rv == NGTCP2_ERR_VERSION_NEGOTIATION) {
        // Skip version negotiation handling for brevity.
        return;
    }
    if (rv != 0) return;

    std::string key(reinterpret_cast<const char *>(vc.dcid), vc.dcidlen);
    auto it = conns_.find(key);
    if (it == conns_.end()) {
        // New connection: client's first Initial. Only handle long-header packets.
        ngtcp2_cid dcid, scid, ocid;
        std::memcpy(dcid.data, vc.dcid, vc.dcidlen); dcid.datalen = vc.dcidlen;
        std::memcpy(scid.data, vc.scid, vc.scidlen); scid.datalen = vc.scidlen;
        ocid = dcid;  // original DCID == what client sent
        auto conn = std::make_unique<Connection>(
            this, dcid, scid, ocid,
            reinterpret_cast<sockaddr *>(&local_addr_), local_addrlen_,
            remote, remote_len, vc.version, ssl_ctx_);
        if (!conn->conn()) return;
        auto *raw = conn.get();
        conns_.emplace(key, std::move(conn));
        if (raw->read_pkt(remote, remote_len, data, datalen) != 0) {
            if (raw->closed()) { conns_.erase(key); return; }
        }
        raw->write_pending(fd_);
    } else {
        it->second->read_pkt(remote, remote_len, data, datalen);
        it->second->write_pending(fd_);
    }
}

int Server::read_socket() {
    for (;;) {
        std::array<uint8_t, 64 * 1024> buf;
        sockaddr_storage from{};
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(fd_, buf.data(), buf.size(), 0,
                             reinterpret_cast<sockaddr *>(&from), &fromlen);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            std::perror("recvfrom");
            return -1;
        }
        on_packet(reinterpret_cast<sockaddr *>(&from), fromlen, buf.data(),
                  static_cast<size_t>(n));
    }
}

void Server::sweep_closed() {
    for (auto it = conns_.begin(); it != conns_.end();) {
        if (it->second->closed()) it = conns_.erase(it);
        else ++it;
    }
}

void Server::poll_loop() {
    pollfd pfd{fd_, POLLIN, 0};
    int flags = fcntl(fd_, F_GETFL, 0);
    fcntl(fd_, F_SETFL, flags | O_NONBLOCK);

    for (;;) {
        // Compute earliest expiry across connections
        ngtcp2_tstamp earliest = UINT64_MAX;
        for (auto &kv : conns_) {
            ngtcp2_tstamp e = kv.second->expiry();
            if (e < earliest) earliest = e;
        }
        int timeout_ms = -1;
        ngtcp2_tstamp t = now_ns();
        if (earliest != UINT64_MAX) {
            timeout_ms = earliest > t
                ? static_cast<int>((earliest - t) / 1'000'000ULL)
                : 0;
        }
        // If any connection is draining (peer sent GOAWAY), cap the poll
        // wait so we promptly notice when its last stream finishes and we
        // can emit CONNECTION_CLOSE without waiting for the next packet.
        for (auto &kv : conns_) {
            if (kv.second->draining()) {
                if (timeout_ms < 0 || timeout_ms > 100) timeout_ms = 100;
                break;
            }
        }

        int rv = poll(&pfd, 1, timeout_ms);
        if (rv < 0) {
            if (errno == EINTR) continue;
            std::perror("poll");
            return;
        }
        if (rv > 0 && (pfd.revents & POLLIN)) {
            read_socket();
        }
        // Run timers for all conns
        for (auto &kv : conns_) {
            if (kv.second->expiry() <= now_ns()) {
                kv.second->handle_expiry();
                kv.second->write_pending(fd_);
            }
        }
        // Tear down any draining connection whose in-flight streams have all
        // completed.
        for (auto &kv : conns_) {
            if (!kv.second->closed() && kv.second->idle_for_close()) {
                kv.second->write_connection_close(fd_);
            }
        }
        sweep_closed();
    }
}

int Server::run(const char *host, const char *port, const char *cert,
                const char *key) {
    if (ngtcp2_crypto_ossl_init() != 0) {
        std::fprintf(stderr, "ngtcp2_crypto_ossl_init failed\n");
        return 1;
    }
    // Connect to Redis. Host/port overridable via env so the test harness
    // (dist_cache_test.sh) can point at the redis-server it spins up.
    const char *rhost = std::getenv("DC_REDIS_HOST");
    const char *rport = std::getenv("DC_REDIS_PORT");
    int rport_n = rport ? std::atoi(rport) : 6379;
    try {
        redis_ = std::make_unique<dist_cache::redis::Client>(
            rhost ? rhost : "127.0.0.1", rport_n);
    } catch (const dist_cache::redis::RedisError &e) {
        std::fprintf(stderr, "redis connect failed: %s\n", e.what());
        return 1;
    }
    store_ = std::make_unique<RedisStore>(redis_.get());

    if (setup_socket(host, port) != 0) return 1;
    if (setup_ssl(cert, key) != 0) return 1;
    std::printf("HTTP/3 server listening on %s:%s (redis %s:%d)\n",
                host, port, rhost ? rhost : "127.0.0.1", rport_n);
    poll_loop();
    return 0;
}

}  // namespace

namespace dist_cache {

int run_server(const char *host, const char *port,
               const char *cert, const char *key) {
    Server s;
    return s.run(host, port, cert, key);
}

}  // namespace dist_cache
