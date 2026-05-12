// Public entry point for the HTTP/3 server defined in server.cc.
#pragma once

namespace dist_cache {

// Starts the HTTP/3 server, blocks in its poll loop, and returns a process
// exit code (0 on graceful shutdown, non-zero on setup failure).
int run_server(const char *host, const char *port,
               const char *cert, const char *key);

}  // namespace dist_cache
