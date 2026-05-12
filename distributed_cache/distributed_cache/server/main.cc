// Process entry point. All HTTP/3 server logic lives in server.cc; this file
// only parses argv and delegates.

#include "server.h"

#include <cstdio>

int main(int argc, char **argv) {
    if (argc != 5) {
        std::fprintf(stderr,
                     "usage: %s <host> <port> <cert.pem> <key.pem>\n", argv[0]);
        return 1;
    }
    return dist_cache::run_server(argv[1], argv[2], argv[3], argv[4]);
}
