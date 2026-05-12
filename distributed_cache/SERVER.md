# Server 


## LDD 
* main() runs Server poll_loop... which does in loop
    1. poll fd (blocking call)
    2. read socket --> recvfrom and update connection map
    3. clean up connection map

### Connection Class


### ngtcp2 and nghttp2 flows
http/3 needs TLS as a hard requirement, so first setup TLS: 
* ngtcp2_crypto_ossl_init to libngtcp2_crypto_ossl 
* ngtcp2_crypto_ossl_configure_server_context --> give a SSL context and setup SSL using openssl

Packet reading flows, 
* Get QUIC connection ID using ngtcp2_pkt_decode_version_cid, and use as key in connection map.
* 