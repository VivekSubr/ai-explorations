// Thin RAII wrapper around hiredis for the operations the cache server
// actually needs: KV get/set/del with optional TTL and Redis 8's native
// JSON.SET / JSON.GET / JSON.DEL commands.
//
// Kept deliberately small: synchronous, single connection, no pooling.
// Not thread-safe -- guard externally if shared across threads.
#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

struct redisContext;

namespace dist_cache::redis {

// Thrown for any hiredis-level failure (connect error, protocol error, server
// returned an ERROR reply). The message includes the underlying detail.
class RedisError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Client {
public:
    // Connects synchronously. Throws RedisError on failure.
    Client(std::string_view host = "127.0.0.1", int port = 6379);
    ~Client();

    Client(const Client&)            = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&)                 = delete;
    Client& operator=(Client&&)      = delete;

    // Liveness probe. Returns true on PONG.
    bool ping();

    // -- KV ----------------------------------------------------------------
    // SET key value [EX expiry_sec]. Throws on protocol/server error.
    void kv_set(std::string_view key, std::string_view value,
                std::optional<int64_t> expiry_sec = std::nullopt);
    // GET key. nullopt iff the key is absent.
    std::optional<std::string> kv_get(std::string_view key);
    // DEL key. Returns true iff a key was actually removed.
    bool kv_delete(std::string_view key);

    // -- JSON (Redis 8 native JSON commands) -------------------------------
    // JSON.SET key path value [EX expiry_sec via separate EXPIRE].
    // `expiry_sec` is only honored when path == "$" (root assignment); the
    // server-side semantics enforced in rest.cc match this.
    void json_set(std::string_view key, std::string_view path,
                  const nlohmann::json& value,
                  std::optional<int64_t> expiry_sec = std::nullopt);
    // JSON.GET key path. Returns nullopt iff key/path is absent.
    std::optional<nlohmann::json> json_get(std::string_view key,
                                           std::string_view path);
    // JSON.DEL key path. Returns true iff at least one element was removed.
    bool json_delete(std::string_view key, std::string_view path);

private:
    redisContext* ctx_ = nullptr;
};

}  // namespace dist_cache::redis
