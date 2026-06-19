# Server Implementation Notes

This document describes the HTTP/3 server implementation in this directory. The
public entry point is small; most of the behavior lives in the private server
implementation files.

## Files

- [server.h](server.h) exposes `dist_cache::run_server(...)` to the rest of the
  program.
- [main.cc](main.cc) parses process arguments and calls `run_server`.
- [server_impl.h](server_impl.h) contains private transport/server declarations:
  `StreamCtx`, `Connection`, `Server`, and ngtcp2/nghttp3 callback prototypes.
- [server_redis.h](server_redis.h) and [server_redis.cc](server_redis.cc)
  contain the private Redis-backed `dist_cache::rest::Store` adapter.
- [server.cc](server.cc) contains the implementation of the HTTP/3 server,
  QUIC/TLS setup, socket loop, callbacks, request routing, and response writing.
- [hello.cc](hello.cc) is a tiny example HTTP/3 server built on the same private
  transport implementation. It exposes only `GET /hello`, which returns `hi`.
- [rest.h](rest.h) and [rest.cc](rest.cc) contain the transport-independent REST
  router.
- [redis_client.h](redis_client.h) and [redis_client.cc](redis_client.cc) contain
  the Redis client used by the server-backed store.
- [server_test.cc](server_test.cc), [rest_test.cc](rest_test.cc), and
  [redis_client_test.cc](redis_client_test.cc) cover server internals, REST
  behavior, and Redis client behavior.

`server_impl.h` is intentionally private. It uses an unnamed namespace so it
should only be included by translation units that intentionally need the server
internals, such as [server.cc](server.cc) and the current internal tests.

## Runtime Dependencies

The server uses:

- ngtcp2 for QUIC packet parsing, state, stream flow control, and packet writing.
- ngtcp2 crypto OpenSSL integration for TLS 1.3 over QUIC.
- nghttp3 for HTTP/3 frame parsing, QPACK, request callbacks, and response
  submission.
- OpenSSL 3.5+ for the QUIC-capable TLS API.
- hiredis for Redis access.
- nlohmann::json for JSON request and response handling.

The CMake file prefers locally built dependencies under `../../third_party`, in
particular OpenSSL 3.5, ngtcp2, and nghttp3.

## Build And Test

The normal build path is:

```bash
make build
```

That target builds third-party dependencies through the top-level Makefile,
generates OpenAPI-derived sources, configures CMake, and builds the targets.

For an already configured tree:

```bash
cmake --build build -j
```

Run all tests with:

```bash
cd build && ctest --output-on-failure
```

The Redis tests use CTest fixtures. `redis_up` starts a Redis fixture before the
Redis client tests, and `redis_down` cleans it up afterward.

The end-to-end HTTP/3 shell test requires a curl binary built with HTTP/3
support. If the system curl does not list `HTTP3` in `curl -V`, build the
repo-local curl from the project root:

```bash
make curl
```

The shell test prefers `third_party/curl/install/bin/curl` when it exists, and
also honors `CURL=/path/to/curl` for explicit overrides.

## Running The Server

The executable expects:

```bash
dist_cache.exe <host> <port> <cert.pem> <key.pem>
```

Example client call:

```bash
curl --http3-only -k https://<host>:<port>/
```

Redis defaults to `127.0.0.1:6379`. Override it with:

```bash
DC_REDIS_HOST=<host> DC_REDIS_PORT=<port> dist_cache.exe <host> <port> <cert.pem> <key.pem>
```

The example `hello.exe` accepts the same arguments but does not connect to
Redis:

```bash
hello.exe <host> <port> <cert.pem> <key.pem>
curl --http3-only -k https://<host>:<port>/hello
```

## Startup Flow

`dist_cache::run_server(...)` creates a `Server` and calls `Server::run(...)`.
Startup then proceeds as follows:

1. Initialize ngtcp2 OpenSSL crypto integration with `ngtcp2_crypto_ossl_init()`.
2. Connect to Redis and construct `RedisStore`.
3. Create and bind a UDP socket with `Server::setup_socket(...)`.
4. Create an OpenSSL server context with `Server::setup_ssl(...)`.
5. Enter `Server::poll_loop()`.

The socket is configured non-blocking in `poll_loop()`. The loop waits for UDP
read readiness, QUIC timer expiry, or short drain polling when a connection is
waiting to close after HTTP/3 GOAWAY.

## Socket And Packet Flow

Incoming UDP data follows this path:

```text
Server::poll_loop()
  -> Server::read_socket()
  -> recvfrom(...)
  -> Server::on_packet(...)
  -> Connection::read_pkt(...)
  -> ngtcp2_conn_read_pkt(...)
```

`read_socket()` only receives datagrams. It does not parse QUIC, HTTP/3, or REST
requests. It passes each datagram plus the peer address to `on_packet()`.

`on_packet()` decodes the QUIC version and connection IDs. Long-header packets
carry explicit CID lengths, but short-header packets do not; for those packets
the server decodes the destination CID using the fixed server CID length it
generated during connection setup.

`Server::conns_` owns each `Connection` by the first client Initial DCID.
Packet routing uses `Server::cid_index_`, which maps every active destination
CID to the owned `Connection`. After a connection is created or receives a
packet, the server associates all CIDs returned by `ngtcp2_conn_get_scid2(...)`
with that connection.

- If no connection exists, it creates a new `Connection`, inserts it into the
  owner map and CID index, reads the packet, then calls `write_pending()`.
- If a connection exists, it reads the packet on that connection and then calls
  `write_pending()`.

## Connection Setup

`Connection` owns one QUIC connection and one TLS session:

- `ngtcp2_conn *conn_`
- `SSL *ssl_`
- `ngtcp2_crypto_ossl_ctx *ossl_ctx_`
- `nghttp3_conn *h3_` once HTTP/3 is initialized
- local and remote socket addresses
- `streams_`, the per-stream request/response state map

During construction, `Connection` configures ngtcp2 callbacks, transport
parameters, OpenSSL state, and the ngtcp2/OpenSSL crypto bridge.

The OpenSSL backend requires an `ngtcp2_crypto_ossl_ctx` as the ngtcp2 TLS
native handle. The raw `SSL*` is stored inside that context with
`ngtcp2_crypto_ossl_ctx_set_ssl(...)`, while the `SSL*` app data points at
`ngtcp2_crypto_conn_ref` so ngtcp2 crypto callbacks can recover the owning
`ngtcp2_conn`. Before freeing the `SSL*`, the destructor clears its app data and
then frees the TLS context separately.

Important transport parameters include:

- bidirectional stream data limits of 256 KiB
- unidirectional stream data limit of 256 KiB
- connection-level data limit of 1 MiB
- 100 bidirectional streams
- 3 unidirectional streams
- 30 second max idle timeout

## ngtcp2 Callbacks

ngtcp2 callbacks are installed in the `Connection` constructor. They handle QUIC
events and bridge stream bytes into HTTP/3.

Important callbacks:

- `handshake_completed` -> `ng_handshake_completed(...)`
- `recv_stream_data` -> `ng_recv_stream_data(...)`
- `acked_stream_data_offset` -> `ng_acked_stream_data(...)`
- `stream_open` -> `ng_stream_open(...)`
- `stream_close` -> `ng_stream_close(...)`
- `extend_max_remote_streams_bidi` -> `ng_extend_max_remote_streams_bidi(...)`
- `stream_stop_sending` -> `ng_stream_stop_sending(...)`
- `stream_reset` -> `ng_stream_reset(...)`

Crypto-related callbacks are supplied by ngtcp2 crypto OpenSSL helpers.

The remote-stream-limit callback bridges QUIC stream limits into nghttp3 by
calling `nghttp3_conn_set_max_client_streams_bidi(...)`. Without that bridge,
nghttp3 sees only HTTP/3 control/QPACK streams and rejects client request
streams.

When `ngtcp2_conn_read_pkt(...)` processes stream data, ngtcp2 invokes
`ng_recv_stream_data(...)`. That callback forwards the stream bytes to nghttp3:

```text
ng_recv_stream_data(...)
  -> nghttp3_conn_read_stream(...)
```

After nghttp3 consumes bytes, the callback extends QUIC stream and connection
flow-control windows with:

```text
ngtcp2_conn_extend_max_stream_offset(...)
ngtcp2_conn_extend_max_offset(...)
```

## HTTP/3 Initialization

HTTP/3 is initialized only after the QUIC/TLS handshake completes:

```text
ng_handshake_completed(...)
  -> Connection::on_handshake_completed()
  -> Connection::init_h3()
```

`init_h3()` creates the nghttp3 server connection, installs nghttp3 callbacks,
seeds nghttp3 with the server's initial bidirectional stream limit, opens
unidirectional QUIC streams for HTTP/3 control and QPACK, and binds them to
nghttp3.

The server opens three unidirectional streams:

- HTTP/3 control stream
- QPACK encoder stream
- QPACK decoder stream

## nghttp3 Callbacks

nghttp3 callbacks parse HTTP/3 frames and populate per-stream request state.

Important callbacks:

- `h3_recv_header(...)` captures `:method` and `:path`.
- `h3_recv_data(...)` appends request body bytes.
- `h3_end_headers(...)` submits a response immediately for headers-only
  requests when nghttp3 reports `fin` on the header section.
- `h3_end_stream(...)` triggers response generation.
- `h3_stop_sending(...)` shuts down QUIC stream reads.
- `h3_reset_stream(...)` shuts down QUIC stream writes.
- `h3_deferred_consume(...)` extends QUIC flow-control windows.
- `h3_go_away(...)` marks the connection draining.

The request path is:

```text
nghttp3_conn_read_stream(...)
  -> h3_recv_header(...)
  -> h3_end_headers(...) [headers-only request]
  -> h3_recv_data(...)
  -> h3_end_stream(...)
  -> Connection::on_stream_end(...)
  -> Connection::submit_response(...)
```

## Per-Stream State

`StreamCtx` stores both request and response state for one HTTP/3 stream.

Request fields:

- `method`
- `path`
- `req_body`

Response fields:

- `resp_status`
- `resp_ctype`
- `body`
- `offset`
- `response_submitted`

`Connection::ensure_stream(id)` creates a stream context on demand in
`streams_`. `response_submitted` prevents duplicate response submission when a
headers-only request reaches both `h3_end_headers(..., fin=1)` and
`h3_end_stream(...)`. Stream contexts are erased by the ngtcp2 stream close
callback.

## REST Dispatch And Redis Storage

`Connection::submit_response(stream_id)` converts the collected stream request
into a response by calling the owning server's request hook:

```text
Server::handle_request(method, path, req_body)
```

The base `Server` implementation routes that hook through the OpenAPI-spec REST
handler:

```text
dist_cache::rest::handle(*server_->store(), method, path, req_body)
```

The store is a `RedisStore`, created by `Server::init_backend()` after the Redis
client connects. Small example servers can override `init_backend()` and
`handle_request(...)` to reuse the QUIC/HTTP/3 transport without Redis or the
distributed-cache REST routes; `hello.cc` does exactly this for `/hello`.

`RedisStore` implements `dist_cache::rest::Store` by delegating to
`dist_cache::redis::Client`:

- key/value operations map to Redis string operations
- JSON operations map to Redis JSON commands
- all JSON operations share the Redis key `dc:json`

For JSON operations, a Redis JSON empty array for a missing path is treated as
not found so the REST layer can emit a 404.

The REST layer validates JSONPath syntax before calling the store. This keeps
malformed JSONPath handling consistent across the in-memory test store and the
Redis-backed store: malformed paths return 400, while valid but missing paths
return 404.

## Response Writing

`submit_response()` stores stable header/body data in `StreamCtx`, associates
the stream with that context using `nghttp3_conn_set_stream_user_data(...)`, and
queues the response with `nghttp3_conn_submit_response(...)`.

Response body bytes are pulled lazily by nghttp3 through a data reader. The data
reader points nghttp3 at `StreamCtx::body` and advances `StreamCtx::offset` as
bytes are handed over.

Actual UDP writes happen later in `Connection::write_pending(fd)`:

```text
Connection::write_pending(...)
  -> nghttp3_conn_writev_stream(...)
  -> ngtcp2_conn_writev_stream(...)
  -> sendto(...)
```

This same path writes handshake data, ACK-only packets, HTTP/3 control/QPACK
data, and HTTP response data.

## Connection Close And Draining

The server has two related shutdown paths.

For connection-level ngtcp2 errors, `Connection::read_pkt(...)` marks the
connection closed for terminal conditions such as draining, drop-connection, and
crypto errors. `Server::sweep_closed()` removes closed connections from the map.

For HTTP/3 GOAWAY, `h3_go_away(...)` calls `Connection::on_peer_goaway(...)`.
That sets `draining_ = true`. `poll_loop()` caps its poll timeout while draining
so it can promptly notice when all stream contexts are gone. Once
`idle_for_close()` is true, the server sends a QUIC CONNECTION_CLOSE frame with
`Connection::write_connection_close(fd)`.

## Known Limitations

This is a minimal HTTP/3 server skeleton, not a complete production server.
Current limitations include:

- single-threaded event loop
- no connection migration handling
- no 0-RTT support
- no retry token support
- minimal version negotiation handling
- minimal error recovery
- minimal CID lifecycle handling beyond associating active server CIDs returned
  by ngtcp2
- no request content-type validation in the HTTP/3 layer
- request bodies are accumulated fully in memory before dispatch

## Troubleshooting

If CMake cannot find OpenSSL 3.5+, build or install the local third-party
dependencies through the top-level build path before configuring this directory.

If HTTP/3 requests do not complete, inspect the flow in this order:

1. UDP socket readiness in `Server::poll_loop()`.
2. Datagram receive in `Server::read_socket()`.
3. Connection lookup/creation in `Server::on_packet()`.
4. QUIC packet read in `Connection::read_pkt()`.
5. Stream callback entry in `ng_recv_stream_data()`.
6. HTTP/3 callbacks for headers, body, and end stream.
7. Response submission in `Connection::submit_response()`.
8. Packet generation and `sendto()` in `Connection::write_pending()`.

If Redis-backed REST behavior is unexpected, first isolate whether the issue is
in the transport layer or the REST/store layer by running `rest_test` and
`redis_client_test` separately.