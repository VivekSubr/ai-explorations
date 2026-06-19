#include "server_redis.h"

#include "redis_client.h"

namespace dist_cache::server_internal {

RedisStore::RedisStore(dist_cache::redis::Client *client) : client_(client) {}

void RedisStore::kv_set(const std::string &key, std::string value,
                        std::optional<int64_t> expiry_sec) {
    client_->kv_set(key, value, expiry_sec);
}

std::optional<std::string> RedisStore::kv_get(const std::string &key) {
    return client_->kv_get(key);
}

bool RedisStore::kv_delete(const std::string &key) {
    return client_->kv_delete(key);
}

std::optional<nlohmann::json> RedisStore::json_get(const std::string &path) const {
    try {
        auto v = client_->json_get(kJsonKey, path);
        if (!v) return std::nullopt;
        // JSON.GET on a non-existent path returns an empty array; treat
        // that as "not found" so the REST layer can emit 404.
        if (v->is_array() && v->empty()) return std::nullopt;
        return v;
    } catch (const dist_cache::redis::RedisError &) {
        return std::nullopt;
    }
}

bool RedisStore::json_set(const std::string &path, nlohmann::json value,
                          std::optional<int64_t> expiry_sec) {
    // Spec: expiry_sec only allowed at root. Matches the in-memory rule.
    if (expiry_sec && path != "$") return false;
    try {
        client_->json_set(kJsonKey, path, value, expiry_sec);
        return true;
    } catch (const dist_cache::redis::RedisError &) {
        // Most commonly: parent path missing on a sub-path SET.
        return false;
    }
}

bool RedisStore::json_delete(const std::string &path) {
    try { return client_->json_delete(kJsonKey, path); }
    catch (const dist_cache::redis::RedisError &) { return false; }
}

}  // namespace dist_cache::server_internal