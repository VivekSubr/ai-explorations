// REST handler implementing the OpenAPI spec at openapi/distributed_cache.yaml.
//
// Transport-agnostic: the caller (e.g. the HTTP/3 stream handler in server.cc)
// passes in (method, target, body) and gets back (status, content_type, body).
// The handler holds an in-memory Store with two flavors of storage:
//   * KV   : opaque string values under arbitrary keys      (paths /<key>)
//   * JSON : a single root JSON document, addressed by      (paths /json)
//            JSONPath expressions supplied via the query string.
//
// JSONPath support is intentionally a minimal subset:
//   $                  root
//   $.a.b              dot-separated object property access
//   $.a[0]             integer array indices
// This is enough to exercise the spec; richer queries are out of scope.
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace dist_cache::rest {

struct Response {
    int status = 200;
    std::string content_type;  // empty => no Content-Type header
    std::string body;
};

// In-memory backing store. Caller owns lifetime. Not thread-safe; protect
// externally if shared across threads.
class Store {
public:
    using Clock = std::chrono::steady_clock;

    // KV ------------------------------------------------------------------
    void kv_set(const std::string& key, std::string value,
                std::optional<int64_t> expiry_sec = std::nullopt);
    std::optional<std::string> kv_get(const std::string& key);
    bool kv_delete(const std::string& key);

    // JSON ----------------------------------------------------------------
    // Returns the value at `path` (or std::nullopt if absent).
    std::optional<nlohmann::json> json_get(const std::string& path) const;
    // Returns false if `path` cannot be created (e.g. parent missing).
    // `expiry_sec` is only honored when `path` is the root ("$").
    bool json_set(const std::string& path, nlohmann::json value,
                  std::optional<int64_t> expiry_sec = std::nullopt);
    bool json_delete(const std::string& path);

    // Test hook: force-expire entries whose TTL has lapsed. Normally called
    // implicitly by kv_get / json_get.
    void sweep_expired();

private:
    struct KvEntry {
        std::string value;
        std::optional<Clock::time_point> expiry;
    };
    struct JsonState {
        nlohmann::json root = nullptr;
        std::optional<Clock::time_point> expiry;
    };

    std::map<std::string, KvEntry> kv_;
    JsonState json_;

    static std::optional<Clock::time_point> to_expiry(
        std::optional<int64_t> expiry_sec);
    bool kv_expired(const KvEntry& e) const;
};

// Single entry point. `method` is "GET", "POST", or "DELETE". `target` is the
// HTTP :path pseudo-header (e.g. "/json?$.user.name" or "/mykey"). `body` is
// the raw request body (may be empty).
Response handle(Store& store,
                std::string_view method,
                std::string_view target,
                std::string_view body);

}  // namespace dist_cache::rest
