// hiredis-backed Redis client for the cache server. See redis_client.h for
// the contract. Kept synchronous and single-connection; the caller (the HTTP
// stream handler in server.cc) is itself single-threaded per connection.

#include "redis_client.h"

#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <hiredis/hiredis.h>

namespace dist_cache::redis {

namespace {

// RAII wrapper for redisReply* so command paths stay exception-safe without
// boilerplate freeReplyObject() at every error return.
struct ReplyDeleter {
    void operator()(redisReply* r) const noexcept {
        if (r) freeReplyObject(r);
    }
};
using ReplyPtr = std::unique_ptr<redisReply, ReplyDeleter>;

// Run a redisCommandArgv() with a vector<string_view> of arguments. Throws
// RedisError on transport failure or REDIS_REPLY_ERROR. Returns the reply.
ReplyPtr command(redisContext* ctx,
                 const std::vector<std::string_view>& argv) {
    std::vector<const char*> a;
    std::vector<size_t>      lens;
    a.reserve(argv.size());
    lens.reserve(argv.size());
    for (auto sv : argv) {
        a.push_back(sv.data());
        lens.push_back(sv.size());
    }
    auto* raw = static_cast<redisReply*>(redisCommandArgv(
        ctx, static_cast<int>(a.size()), a.data(), lens.data()));
    if (!raw) {
        // Connection-level failure. ctx->errstr holds the detail.
        throw RedisError(std::string("redis transport error: ") +
                         (ctx->errstr[0] ? ctx->errstr : "unknown"));
    }
    ReplyPtr reply(raw);
    if (reply->type == REDIS_REPLY_ERROR) {
        throw RedisError(std::string("redis error: ") +
                         std::string(reply->str, reply->len));
    }
    return reply;
}

}  // namespace

Client::Client(std::string_view host, int port) {
    // hiredis wants a NUL-terminated host string.
    std::string h(host);
    ctx_ = redisConnect(h.c_str(), port);
    if (!ctx_) {
        throw RedisError("redisConnect returned null (allocation failure)");
    }
    if (ctx_->err) {
        std::string msg = ctx_->errstr;
        redisFree(ctx_);
        ctx_ = nullptr;
        throw RedisError("redis connect failed: " + msg);
    }
}

Client::~Client() {
    if (ctx_) redisFree(ctx_);
}

bool Client::ping() {
    auto r = command(ctx_, {"PING"});
    return (r->type == REDIS_REPLY_STATUS || r->type == REDIS_REPLY_STRING) &&
           r->len == 4 && std::memcmp(r->str, "PONG", 4) == 0;
}

void Client::kv_set(std::string_view key, std::string_view value,
                    std::optional<int64_t> expiry_sec) {
    std::string ttl;
    std::vector<std::string_view> argv{"SET", key, value};
    if (expiry_sec) {
        ttl = std::to_string(*expiry_sec);
        argv.push_back("EX");
        argv.push_back(ttl);
    }
    (void)command(ctx_, argv);
}

std::optional<std::string> Client::kv_get(std::string_view key) {
    auto r = command(ctx_, {"GET", key});
    if (r->type == REDIS_REPLY_NIL) return std::nullopt;
    if (r->type != REDIS_REPLY_STRING) {
        throw RedisError("GET: unexpected reply type");
    }
    return std::string(r->str, r->len);
}

bool Client::kv_delete(std::string_view key) {
    auto r = command(ctx_, {"DEL", key});
    return r->type == REDIS_REPLY_INTEGER && r->integer > 0;
}

void Client::json_set(std::string_view key, std::string_view path,
                      const nlohmann::json& value,
                      std::optional<int64_t> expiry_sec) {
    std::string serialized = value.dump();
    (void)command(ctx_, {"JSON.SET", key, path, serialized});
    if (expiry_sec) {
        // Redis JSON inherits its TTL from the surrounding key, so a separate
        // EXPIRE is required. Only meaningful when assigning the root.
        std::string ttl = std::to_string(*expiry_sec);
        (void)command(ctx_, {"EXPIRE", key, ttl});
    }
}

std::optional<nlohmann::json> Client::json_get(std::string_view key,
                                               std::string_view path) {
    auto r = command(ctx_, {"JSON.GET", key, path});
    if (r->type == REDIS_REPLY_NIL) return std::nullopt;
    if (r->type != REDIS_REPLY_STRING) {
        throw RedisError("JSON.GET: unexpected reply type");
    }
    // JSON.GET with a single path returns a JSON-encoded array of matches
    // (e.g. `[42]` or `["world"]`). Unwrap when there's exactly one match so
    // callers see the value at `path` directly.
    auto j = nlohmann::json::parse(std::string_view(r->str, r->len));
    if (j.is_array() && j.size() == 1) return j[0];
    return j;
}

bool Client::json_delete(std::string_view key, std::string_view path) {
    auto r = command(ctx_, {"JSON.DEL", key, path});
    return r->type == REDIS_REPLY_INTEGER && r->integer > 0;
}

}  // namespace dist_cache::redis
