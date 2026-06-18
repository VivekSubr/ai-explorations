// Tiny HTTP/3 example server built on the same private server implementation
// as dist_cache.exe. It exposes exactly one endpoint:
//   GET /hello -> 200 text/plain "hi"

#include "server.cc"

#include <cstdio>
#include <string_view>

namespace {

class HelloServer : public Server {
public:
    int init_backend() override {
        backend_detail_ = "(/hello -> hi)";
        return 0;
    }

    dist_cache::rest::Response handle_request(std::string_view method,
                                              std::string_view target,
                                              std::string_view) override {
        if (target == "/hello" && method == "GET") {
            return {200, "text/plain", "hi"};
        }
        if (target == "/hello") {
            return {405, "text/plain", "method not allowed"};
        }
        return {404, "text/plain", "not found"};
    }
};

}  // namespace

int main(int argc, char **argv) {
    if (argc != 5) {
        std::fprintf(stderr,
                     "usage: %s <host> <port> <cert.pem> <key.pem>\n",
                     argv[0]);
        return 1;
    }
    HelloServer server;
    return server.run(argv[1], argv[2], argv[3], argv[4]);
}