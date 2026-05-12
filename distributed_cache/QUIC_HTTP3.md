# Notes on QUIC and HTTP3

## QUIC 
QUIC is a multiplexed, encrypted transport protocol running over UDP. Instead of running through all the details, let's go though it's advantages over TCP.

* QUIC is *user-space* protocol over UDP, it can updated independant of OS.

**Faster Handshake**

Consider the TCP/TLS handshake,
Client                  Server
  |-- SYN -------------->|
  |<-- SYN-ACK ----------|
  |-- ACK -------------->|
  |                      |  (TCP established)
  |-- ClientHello ------>|
  |<-- ServerHello ------|
  |    + Certificate     |
  |    + ServerFinished  |
  |-- ClientFinished --->|
  |                      |  (TLS established)
  |-- HTTP Request ----->|

1-RTT: TCP SYN → SYN-ACK → ACK
1-RTT: TLS ClientHello → ServerHello + Certificate + Finished → Finished
Total: 2-RTT before application data can be sent

QUIC makes *TLS mandatory* and merges the steps, 
Client                  Server
  |-- Initial ---------->|  (ClientHello + Connection ID)
  |<-- Initial/Handshake-|  (ServerHello + Certificate)
  |-- Handshake -------->|  (ClientFinished)
  |-- 1-RTT Data ------->|  (Application data)
  |                      |

QUIC allows to elide the handshake, so that client won't wait on server hello... this is called 0 RTT handshake.
Client                     Server
  |-- ClientHello -------->|
  |    + Early Data ------>|  ← Application data sent immediately!
  |                        |
  |<-- ServerHello --------|
  |    + Response Data ----|


**Connection IDs**
QUIC abandons TCP's four tuple (src_ip, src_port, dst_ip, dst_port), in favor of unique CIDs.

**Streams and Multiplexing**
Every stream has a Stream ID (62-bit integer)
    Bits 0-1: Type (client/server initiated, bidirectional/unidirectional)
    Bits 2-61: Sequence number

```
Stream ID encoding:
  0x00, 0x04, 0x08... = client-initiated bidirectional
  0x01, 0x05, 0x09... = server-initiated bidirectional
  0x02, 0x06, 0x0A... = client-initiated unidirectional
  0x03, 0x07, 0x0B... = server-initiated unidirectional
```

```
STREAM Frame:
  Type (8) = 0x08-0x0f
  Stream ID (i)
  [Offset (i)]       // byte offset in stream
  [Length (i)]       // data length
  Stream Data (..)
```

And if FIN bit is set in TYPE byte, it indicates End of Stream.

The key difference with TCP is how it handles lost packets.
* TCP packets have *order*, if pkt 5 is lost, TCP waits for re-transmission, dropping pkts 5+... each 'Stream' needs to be a different TCP connection.
* QUIC doesn't enforce order, each stream can send it's own packets, order is only enforced within streams.

QUIC doesn't ACK each packet, instead it ACKs ranges of packets recieved... like ACT 0-10, 10-20 ect.


## HTTP/3
HTTP/3 has two major changes over HTTP/2,
* It uses QUIC rather than TCP, removing transport layer waiting on ACKs, lost packets.
* It compresses headers using algo called QPACK

Flow in server.cc,
```
  UDP socket
      ↓
  ngtcp2_conn_read_pkt()
      ↓
  nghttp3_conn_read_stream()
      ↓
  your app logic
      ↓
  nghttp3_conn_submit_response()
      ↓
  ngtcp2_conn_writev_stream()
      ↓
  sendto()
```

### Changes in Frames, HTTP/2 vs HTTP/3

**HEADERS Frame**
Both have HEADERS frame, but since HTTP/3 expects QUIC to handle fragmentation, there are not CONTINUATION frames in HTTP/3, and there's not END_HEADER flag

**PRIORITY**
PRIORITY_UPDATE frame replaces PRIORITY frame from HTTP/2, it sets a value from 0-7 for a stream.

**RST_STREAM**
HTTP/3 doesn't have this frame, instead it relies on QUIC's RESET_STREAM. This means RESET_STREAM hits ngtcp2 layer rather than nghttp layer as was in HTTP/2.

RESET_STREAM causes ngtcp2_callbacks stream_reset callback to be called, as well as nghttp3_callbacks's reset_stream callback.

In server.cc ngtcp2_callbacks stream_reset callback calls nghttp3_conn_shutdown_stream_read, so that server doesn't read anymore... this causes server to send STOP_SENDING frame to client.

nghttp3 layer is writting http/3 frames to ngtcp2 layer... so nghttp3 stream_reset callback calls ngtcp2_conn_shutdown_stream_write to stop the write, resulting server sending RESET_STREAM to client.

*Why?* -> because QUIC streams are bi-directional, hence to tear down both sides should get RESET_STREAM and STOP_SENDING frames.

**PING**
No PING frame in HTTP/3, again handled by QUIC PING.

**GOAWAY**

