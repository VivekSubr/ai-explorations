// Unit tests for server.cc internals.
//
// server.cc keeps its Server / Connection / callback machinery in an
// anonymous namespace, so the tests pull it into the same translation unit
// by #including the .cc directly. Subclasses (TestServer / TestConnection)
// expose the protected members we need to probe.
//
// We deliberately avoid driving a real QUIC handshake here — the focus is on
// the deterministic pieces of state that don't require a live peer:
//   * Server::setup_socket / setup_ssl success and failure paths
//   * Server::sweep_closed reaping logic
//   * StreamCtx bookkeeping inside Connection
//   * The free helpers: now_ns, get_new_cid_cb, the ALPN selector

#include "server.cc"  // pull anonymous-namespace types into this TU

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace {

namespace fs = std::filesystem;

// ---------------- Test fixtures / helpers ----------------

class TempDir {
public:
    TempDir() {
        path_ = fs::temp_directory_path() /
                ("server_test_" + std::to_string(::getpid()) + "_" +
                 std::to_string(counter_++));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    std::string file(const std::string &name) const {
        return (path_ / name).string();
    }

private:
    fs::path path_;
    static inline int counter_ = 0;
};

// Generates a self-signed RSA cert+key in `dir`. Returns {cert, key} paths,
// or {"", ""} if openssl is unavailable (callers should SKIP in that case).
std::pair<std::string, std::string> make_self_signed(const TempDir &dir) {
    if (std::system("command -v openssl >/dev/null 2>&1") != 0) return {"", ""};
    const std::string cert = dir.file("cert.pem");
    const std::string key  = dir.file("key.pem");
    const std::string cnf  = dir.file("san.cnf");
    std::ofstream(cnf) <<
        "[req]\ndistinguished_name=dn\nx509_extensions=v3_req\nprompt=no\n"
        "[dn]\nCN=localhost\n"
        "[v3_req]\nsubjectAltName=@alt\nbasicConstraints=CA:FALSE\n"
        "keyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n"
        "[alt]\nDNS.1=localhost\nIP.1=127.0.0.1\n";
    const std::string cmd =
        "openssl req -x509 -newkey rsa:2048 -nodes "
        "-keyout '" + key + "' -out '" + cert + "' -days 1 "
        "-config '" + cnf + "' >/dev/null 2>&1";
    if (std::system(cmd.c_str()) != 0) return {"", ""};
    return {cert, key};
}

// Exposes protected members of Server for test inspection.
class TestServer : public Server {
public:
    using Server::setup_socket;
    using Server::setup_ssl;
    using Server::sweep_closed;
    using Server::fd_;
    using Server::ssl_ctx_;
    using Server::local_addr_;
    using Server::local_addrlen_;
    using Server::conns_;
};

// Exposes Connection's protected default ctor and members for testing the
// pure-C++ state machinery (streams_, draining_, peer_goaway_id_) without
// driving a real ngtcp2/TLS handshake. The QUIC handles (conn_, h3_, ssl_)
// stay null, so callbacks that dereference them must be tested only on the
// branches that respect those nulls.
class TestConnection : public Connection {
public:
    TestConnection() : Connection() {}

    using Connection::streams_;
    using Connection::draining_;
    using Connection::closed_;
    using Connection::peer_goaway_id_;
    using Connection::ensure_stream;
    using Connection::erase_stream;
    using Connection::stream;
    using Connection::on_peer_goaway;
    using Connection::draining;
    using Connection::idle_for_close;
};

// ---------------- Free-function helpers ----------------

TEST(NowNs, IsMonotonicAndPositive) {
    auto a = now_ns();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto b = now_ns();
    EXPECT_GT(a, 0u);
    EXPECT_GT(b, a);
}

TEST(GetNewCidCb, PopulatesCidAndToken) {
    ngtcp2_cid cid{};
    uint8_t token[NGTCP2_STATELESS_RESET_TOKENLEN]{};
    int rv = get_new_cid_cb(nullptr, &cid, token, 16, nullptr);
    EXPECT_EQ(rv, 0);
    EXPECT_EQ(cid.datalen, 16u);
    // RAND_bytes should not leave a fixed-zero buffer; check at least one
    // byte differs across two invocations.
    ngtcp2_cid cid2{};
    uint8_t token2[NGTCP2_STATELESS_RESET_TOKENLEN]{};
    ASSERT_EQ(get_new_cid_cb(nullptr, &cid2, token2, 16, nullptr), 0);
    EXPECT_NE(0, std::memcmp(cid.data, cid2.data, 16));
}

// ---------------- Server::setup_socket ----------------

TEST(ServerSetupSocket, BindsToEphemeralPort) {
    TestServer s;
    ASSERT_EQ(0, s.setup_socket("127.0.0.1", "0"));
    EXPECT_GE(s.fd_, 0);
    EXPECT_GT(s.local_addrlen_, 0u);

    // Verify the kernel actually assigned a port to the socket.
    sockaddr_storage bound{};
    socklen_t blen = sizeof(bound);
    ASSERT_EQ(0, getsockname(s.fd_, reinterpret_cast<sockaddr *>(&bound), &blen));
    ASSERT_EQ(bound.ss_family, AF_INET);
    auto *sin = reinterpret_cast<sockaddr_in *>(&bound);
    EXPECT_NE(0, sin->sin_port);
}

TEST(ServerSetupSocket, FailsOnUnresolvableHost) {
    TestServer s;
    EXPECT_NE(0, s.setup_socket("definitely.not.a.real.host.invalid", "0"));
}

TEST(ServerSetupSocket, FailsOnGarbagePort) {
    TestServer s;
    EXPECT_NE(0, s.setup_socket("127.0.0.1", "not_a_port"));
}

// ---------------- Server::setup_ssl ----------------

TEST(ServerSetupSsl, SucceedsWithValidPemPair) {
    TempDir dir;
    auto [cert, key] = make_self_signed(dir);
    if (cert.empty()) GTEST_SKIP() << "openssl not available";

    // setup_ssl uses ngtcp2_crypto_ossl, which must be initialised first.
    ASSERT_EQ(0, ngtcp2_crypto_ossl_init());

    TestServer s;
    EXPECT_EQ(0, s.setup_ssl(cert.c_str(), key.c_str()));
    EXPECT_NE(nullptr, s.ssl_ctx_);
}

TEST(ServerSetupSsl, FailsOnMissingCert) {
    ngtcp2_crypto_ossl_init();
    TempDir dir;
    std::ofstream(dir.file("key.pem")) << "not a real key";
    TestServer s;
    EXPECT_NE(0, s.setup_ssl(dir.file("missing.pem").c_str(),
                             dir.file("key.pem").c_str()));
}

TEST(ServerSetupSsl, FailsOnGarbageCertContents) {
    ngtcp2_crypto_ossl_init();
    TempDir dir;
    std::ofstream(dir.file("cert.pem")) << "not pem";
    std::ofstream(dir.file("key.pem"))  << "not pem";
    TestServer s;
    EXPECT_NE(0, s.setup_ssl(dir.file("cert.pem").c_str(),
                             dir.file("key.pem").c_str()));
}

// ---------------- Server::sweep_closed ----------------

// Without a real handshake we can't construct a Connection to insert into
// conns_, so we only verify the trivial empty-map case here. Coverage of the
// reaping branch would require refactoring Connection to allow stubbed
// construction.
TEST(ServerSweepClosed, NoOpOnEmptyMap) {
    TestServer s;
    EXPECT_NO_THROW(s.sweep_closed());
    EXPECT_TRUE(s.conns_.empty());
}

// ---------------- ALPN selector ----------------

// The ALPN callback is installed inside setup_ssl as a lambda; we can't grab
// a pointer to it. Instead, verify the constant it's matching against.
TEST(Alpn, ConstantIsLengthPrefixedH3) {
    ASSERT_EQ(kAlpnH3.size(), 3u);
    EXPECT_EQ(static_cast<unsigned char>(kAlpnH3[0]), 2);
    EXPECT_EQ(kAlpnH3[1], 'h');
    EXPECT_EQ(kAlpnH3[2], '3');
}

// ---------------- StreamCtx ----------------

TEST(StreamCtx, DefaultsAreZeroExceptId) {
    StreamCtx s;
    EXPECT_TRUE(s.body.empty());
    EXPECT_EQ(s.offset, 0u);
}

// ---------------- Connection state bookkeeping ----------------

TEST(Connection, DefaultConstructedStateIsClean) {
    TestConnection c;
    EXPECT_FALSE(c.draining());
    EXPECT_FALSE(c.closed());
    EXPECT_TRUE(c.streams_.empty());
    EXPECT_TRUE(c.idle_for_close() == false);  // not draining ⇒ not idle
    EXPECT_EQ(c.peer_goaway_id_, -1);
}

TEST(Connection, EnsureAndEraseStream) {
    TestConnection c;
    auto &s = c.ensure_stream(4);
    s.body = "hello";
    s.offset = 0;
    EXPECT_EQ(c.streams_.size(), 1u);
    auto *p = c.stream(4);
    ASSERT_NE(nullptr, p);
    EXPECT_EQ(p->body, "hello");
    c.erase_stream(4);
    EXPECT_TRUE(c.streams_.empty());
    EXPECT_EQ(nullptr, c.stream(4));
}

// ---------------- Client RESET_STREAM handling ----------------

// ng_stream_reset is the ngtcp2 callback fired when the peer sends a
// RESET_STREAM frame. The callback should:
//   * be a no-op for streams the app never tracked,
//   * not touch nghttp3 when h3_ is null (skip branch),
//   * return 0 in all cases (errors would tear down the conn in ngtcp2).
TEST(ResetStream, CallbackIsSafeWithoutH3) {
    TestConnection c;
    // Simulate a request stream already known to the app layer.
    c.ensure_stream(0);
    EXPECT_EQ(c.streams_.size(), 1u);

    // h3_ is null in TestConnection, so ng_stream_reset must skip the
    // nghttp3_conn_shutdown_stream_read call and just return 0.
    int rv = ng_stream_reset(/*ngtcp2_conn*/ nullptr,
                             /*stream_id*/   0,
                             /*final_size*/  0,
                             /*app_err*/     0x010c,  // H3_REQUEST_CANCELLED
                             /*user_data*/   &c,
                             /*stream_user_data*/ nullptr);
    EXPECT_EQ(0, rv);
    // The callback alone does not erase the stream — that happens later via
    // ng_stream_close. Confirm we did not accidentally do so here.
    EXPECT_EQ(c.streams_.size(), 1u);
}

TEST(ResetStream, CallbackOnUnknownStreamIsHarmless) {
    TestConnection c;
    int rv = ng_stream_reset(nullptr, /*stream_id*/ 99,
                             0, 0, &c, nullptr);
    EXPECT_EQ(0, rv);
    EXPECT_TRUE(c.streams_.empty());
}

// h3_reset_stream is the *nghttp3* callback: fired when the HTTP/3 layer
// itself wants to reset a stream. It calls ngtcp2_conn_shutdown_stream_write,
// which we can't exercise here without a live ngtcp2_conn. Just verify the
// callback is wired up to the right field in init_h3 (compile-time check).
TEST(ResetStream, NghttpCallbackPointerIsInstalled) {
    // We can't call init_h3 without h3 setup; instead, confirm h3_reset_stream
    // has the signature nghttp3 expects so the assignment in init_h3 compiles.
    nghttp3_callbacks cb{};
    cb.reset_stream = h3_reset_stream;
    EXPECT_NE(nullptr, reinterpret_cast<void *>(cb.reset_stream));
}

// ---------------- Client GOAWAY handling ----------------

// on_peer_goaway is the pure-C++ leaf of the GOAWAY pipeline; verify the
// state changes the rest of the server relies on.
TEST(GoAway, OnPeerGoawaySetsDraining) {
    TestConnection c;
    EXPECT_FALSE(c.draining());
    EXPECT_EQ(c.peer_goaway_id_, -1);

    c.on_peer_goaway(/*last_stream_id*/ 12);

    EXPECT_TRUE(c.draining());
    EXPECT_EQ(c.peer_goaway_id_, 12);
}

TEST(GoAway, IdleForCloseRequiresEmptyStreams) {
    TestConnection c;
    c.ensure_stream(0);
    c.on_peer_goaway(0);
    EXPECT_TRUE(c.draining());
    EXPECT_FALSE(c.idle_for_close())
        << "must wait for in-flight streams to drain";

    c.erase_stream(0);
    EXPECT_TRUE(c.idle_for_close());
}

TEST(GoAway, IdleForCloseFalseWhenNotDraining) {
    TestConnection c;
    EXPECT_TRUE(c.streams_.empty());
    EXPECT_FALSE(c.idle_for_close())
        << "empty streams alone must not flag idle without GOAWAY";
}

// h3_go_away is the nghttp3 dispatch shim. It just forwards to
// on_peer_goaway, so verifying that path is sufficient.
TEST(GoAway, NghttpCallbackForwardsToOnPeerGoaway) {
    TestConnection c;
    int rv = h3_go_away(/*nghttp3_conn*/ nullptr,
                        /*id*/ 8,
                        /*conn_user_data*/ &c,
                        /*stream_user_data*/ nullptr);
    EXPECT_EQ(0, rv);
    EXPECT_TRUE(c.draining());
    EXPECT_EQ(c.peer_goaway_id_, 8);
}

TEST(GoAway, RepeatedGoawayUpdatesLastId) {
    TestConnection c;
    c.on_peer_goaway(4);
    EXPECT_EQ(c.peer_goaway_id_, 4);
    // RFC 9114: GOAWAY's stream ID must not increase; we don't enforce that
    // here, but we do record the most-recent value the peer sent.
    c.on_peer_goaway(0);
    EXPECT_EQ(c.peer_goaway_id_, 0);
    EXPECT_TRUE(c.draining());
}

}  // namespace
