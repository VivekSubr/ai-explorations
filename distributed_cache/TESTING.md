# Testing 

## Server

### Hello Server 
The 'hello' server is a simple server for debug and learning purpose.

Build the server -
```bash
    cd distributed_cache/server
    make hello
```

Check the binary,
```bash
    cd build
    ./hello.exe 0.0.0.0 1234
    usage: ./hello.exe <host> <port> <cert.pem> <key.pem>
```

HTTP/3 cannot run without encryption, so generate a local self-signed certificate.
```bash
# X.509 self-signed cert, 2048-bit private key, no DES.
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout key.pem \
  -out    cert.pem \
  -days 100 \
  -subj "/CN=localhost" \
  -addext "subjectAltName=DNS:localhost,IP:127.0.0.1,IP:::1"

./hello.exe 0.0.0.0 1234 cert.pem key.pem
HTTP/3 server listening on 0.0.0.0:1234 (/hello -> hi)
```

Now with server up, use curl to send /hello and capture pcap. (Might need to use curl from third_party folder if installed curl doesn't support HTTP/3)