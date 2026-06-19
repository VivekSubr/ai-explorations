#pragma once

#include "rest.h"

#include <cstdint>
#include <optional>
#include <string>

namespace dist_cache::redis {
class Client;
}

namespace dist_cache::server_internal {

// ---------- Redis-backed rest::Store --------------------------------------
//
// Adapter that satisfies the transport-agnostic rest::Store interface by
// delegating every op to a hiredis client. Used by the HTTP/3 server below;
// rest_test keeps using the in-memory base class.
//
// All JSON ops share a single Redis key so the spec's "single root JSON
// document" semantics are preserved. Path arguments ("$", "$.user.age", ...)
// flow straight to Redis 8's native JSON commands, which understand them.
class RedisStore : public dist_cache::rest::Store {
public:
    explicit RedisStore(dist_cache::redis::Client *client);

    void kv_set(const std::string &key, std::string value,
                std::optional<int64_t> expiry_sec) override;
    std::optional<std::string> kv_get(const std::string &key) override;
    bool kv_delete(const std::string &key) override;

    std::optional<nlohmann::json> json_get(const std::string &path) const override;
    bool json_set(const std::string &path, nlohmann::json value,
                  std::optional<int64_t> expiry_sec) override;
    bool json_delete(const std::string &path) override;

private:
    static constexpr const char *kJsonKey = "dc:json";
    dist_cache::redis::Client *client_;
};

}  // namespace dist_cache::server_internal