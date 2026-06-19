# Server 
Implements http/3 server, uses redis client to talk to actual redis cluster.

## LDD 
* main() runs Server poll_loop... which does in loop
    1. poll fd (blocking call)
    2. read socket --> recvfrom and update connection map
    3. clean up connection map

### Connection Class
Manages the connection via nghttp2 and ngtcp2 apis.

### Callbacks 
Connection class constructor sets up ngtcp2 and nghttp2 callbacks with function pointers.

ngtcp2 callbacks are invoked in three paths -> read, write and timer paths. In our server impl, it's most driven by read,
```
    read_pkt (Initial packet)
        → ng_handshake_completed        # TLS done, h3_conn_ created

    read_pkt (STREAM frame, new stream)
        → ng_stream_open                # StreamCtx allocated
        → ng_recv_stream_data           # bytes → nghttp3
            → h3_recv_header            # :method, :path, etc.
            → h3_end_headers
            → h3_stream_ended           # FIN seen → submit_response()

    read_pkt (ACK frame)
        → ng_acked_stream_data          # nghttp3 releases send buffer
```

Aside from this,  ng_stream_open  --> connection open.
                  ng_stream_close --> called on connection close, triggered by STOP_SENDING or RESET_STREAM frame. 

nghttp2 callbacks are all triggered (conditionally) from nghttp3_conn_read_stream() call. In server call, this is in turn within ng_recv_stream_data callback, so
```
    ngtcp2_conn_read_pkt
        → ng_recv_stream_data
            → nghttp3_conn_read_stream     [nghttp3 internal frame parser]
                → h3_* callbacks
```

### CID
**Connection ID**, UDP packets are identified only by (src-ip, src-port, dest-ip, dest-port) 4-tuple, but QUIC adds a connection id for all packets.

Server extracts using ngtcp2_pkt_decode_version_cid(), and adds every packet to map, so that every packet has an owning Connection object.
```
    std::map<std::string, std::unique_ptr<Connection>> conns_;
    std::map<std::string, Connection *>                cid_index_;
```

### ngtcp2 and nghttp2 flows
http/3 needs TLS as a hard requirement, so first setup TLS: 
* ngtcp2_crypto_ossl_init to libngtcp2_crypto_ossl 
* ngtcp2_crypto_ossl_configure_server_context --> give a SSL context and setup SSL using openssl

Poll Loop,
* call poll on listening socket
* once packet is recieved, start read flow
* Any connection that has timer expired, call write_pending
* Close connections that are finished

Packet reading flow, 
* Get QUIC connection ID using ngtcp2_pkt_decode_version_cid, and use as key in connection map.
* Call ngtcp2_conn_read_pkt on packet (via connection object's read_pkt method)
* ng_recv_stream_data callback is called by ngtcp2_conn_read_pkt on STREAM frame, which calls nghttp3_conn_read_stream, which in turn reads into 'data' pointer.

Write pending flow,


### Redis flows