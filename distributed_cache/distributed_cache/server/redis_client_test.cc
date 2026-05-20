// Integration tests for the hiredis-backed Client wrapper in redis_client.cc.
//
// These talk to a real redis-server on REDIS_HOST:REDIS_PORT (defaults
// 127.0.0.1:6379). The test harness dist_cache_test.sh already spins one up;
// when running ctest standalone, point the env vars at any reachable
// redis-server with the JSON module loaded. If the connection (or the JSON
// module) is unavailable, every test SKIPs cleanly rather than failing so
// CI without a redis sidecar still goes green.

#include "redis_client.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

namespace {

using dist_cache::redis::Client;
using dist_cache::redis::RedisError;
using nlohmann::json;

// Per-process unique key prefix so concurrent test runs (and the e2e harness)
// don't trample one another's state.
std::string unique_key(const char* tag) {
    static const std::string pid = std::to_string(::getpid());
    static int counter = 0;
    return std::string("dc:test:") + tag + ":" + pid + ":" +
           std::to_string(++counter);
}

// Shared fixture: tries to connect once, skips every test if the local
// redis-server isn't reachable.
class RedisClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* host = std::getenv("REDIS_HOST");
        const char* port = std::getenv("REDIS_PORT");
        int port_n = port ? std::atoi(port) : 6379;
        try {
            client_ = std::make_unique<Client>(
                host ? host : "127.0.0.1", port_n);
        } catch (const RedisError& e) {
            GTEST_SKIP() << "redis unreachable: " << e.what();
        }
        if (!client_->ping()) GTEST_SKIP() << "redis PING did not return PONG";
    }

    std::unique_ptr<Client> client_;
};

// JSON tests need the RedisJSON module loaded. We probe in SetUp so the
// GTEST_SKIP (which only short-circuits the function it's called from)
// actually halts the test body rather than letting an unguarded JSON.*
// call throw later. Subclasses inherit Skipped state from the base.
class RedisJsonClientTest : public RedisClientTest {
protected:
    void SetUp() override {
        RedisClientTest::SetUp();
        if (IsSkipped()) return;
        const auto k = unique_key("probe");
        try {
            client_->json_set(k, "$", json{{"x", 1}});
            client_->json_delete(k, "$");
        } catch (const RedisError& e) {
            GTEST_SKIP() << "redis JSON module unavailable: " << e.what();
        }
    }
};

// -------------------- connection / liveness --------------------

TEST_F(RedisClientTest, PingReturnsTrue) {
    EXPECT_TRUE(client_->ping());
}

TEST(RedisClientCtor, ConnectFailureThrows) {
    // Port 1 is privileged + almost certainly closed; even if something is
    // listening it won't speak RESP. Either way the ctor must throw rather
    // than silently produce an unusable Client.
    EXPECT_THROW(Client("127.0.0.1", 1), RedisError);
}

// -------------------- KV ops --------------------

TEST_F(RedisClientTest, KvSetGetDeleteRoundTrip) {
    const auto k = unique_key("kv_rt");
    client_->kv_set(k, "hello");
    auto v = client_->kv_get(k);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "hello");
    EXPECT_TRUE(client_->kv_delete(k));
    EXPECT_FALSE(client_->kv_get(k).has_value());
}

TEST_F(RedisClientTest, KvGetMissingReturnsNullopt) {
    const auto k = unique_key("kv_missing");
    EXPECT_FALSE(client_->kv_get(k).has_value());
}

TEST_F(RedisClientTest, KvDeleteMissingReturnsFalse) {
    const auto k = unique_key("kv_del_missing");
    EXPECT_FALSE(client_->kv_delete(k));
}

TEST_F(RedisClientTest, KvOverwriteReplacesValue) {
    const auto k = unique_key("kv_overwrite");
    client_->kv_set(k, "first");
    client_->kv_set(k, "second");
    auto v = client_->kv_get(k);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "second");
    client_->kv_delete(k);
}

TEST_F(RedisClientTest, KvSetWithTtlExpiresKey) {
    const auto k = unique_key("kv_ttl");
    client_->kv_set(k, "ephemeral", /*expiry_sec=*/1);
    ASSERT_TRUE(client_->kv_get(k).has_value());
    // Give the server a moment past the TTL; 1.2s leaves slack for jitter.
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    EXPECT_FALSE(client_->kv_get(k).has_value());
}

TEST_F(RedisClientTest, KvHandlesBinaryAndEmptyValues) {
    const auto k = unique_key("kv_bin");
    // Embedded NUL must survive the round-trip (hiredis is length-prefixed).
    std::string blob("a\0b\0c", 5);
    client_->kv_set(k, blob);
    auto v = client_->kv_get(k);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, blob);
    EXPECT_EQ(v->size(), 5u);

    client_->kv_set(k, "");
    v = client_->kv_get(k);
    ASSERT_TRUE(v.has_value());
    EXPECT_TRUE(v->empty());

    client_->kv_delete(k);
}

// -------------------- JSON ops --------------------

TEST_F(RedisJsonClientTest, JsonSetGetRoot) {
    const auto k = unique_key("json_root");
    json doc = {{"hello", "world"}, {"n", 42}};
    client_->json_set(k, "$", doc);
    auto got = client_->json_get(k, "$");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, doc);
    EXPECT_TRUE(client_->json_delete(k, "$"));
}

TEST_F(RedisJsonClientTest, JsonGetSubpath) {
    const auto k = unique_key("json_sub");
    client_->json_set(k, "$", json{{"user", {{"name", "alice"},
                                             {"age", 30}}}});
    auto name = client_->json_get(k, "$.user.name");
    ASSERT_TRUE(name.has_value());
    EXPECT_EQ(*name, "alice");

    auto age = client_->json_get(k, "$.user.age");
    ASSERT_TRUE(age.has_value());
    EXPECT_EQ(*age, 30);

    client_->json_delete(k, "$");
}

TEST_F(RedisJsonClientTest, JsonSetSubpathRequiresParent) {
    const auto k = unique_key("json_subset");
    client_->json_set(k, "$", json{{"user", {{"name", "alice"}}}});

    // Existing parent: OK.
    client_->json_set(k, "$.user.age", json(30));
    auto v = client_->json_get(k, "$.user.age");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 30);

    // Missing parent: Redis JSON.SET is a silent no-op (returns nil, not an
    // error). Confirm the value really wasn't written -- this is the
    // observable contract the higher-level RestStore relies on.
    //
    // JSON.GET on a missing JSONPath returns an empty array `[]`, which the
    // wrapper passes through as a json value; treat that as "absent".
    client_->json_set(k, "$.nope.deeper.x", json(1));
    auto missing = client_->json_get(k, "$.nope.deeper.x");
    EXPECT_TRUE(missing && missing->is_array() && missing->empty())
        << "expected empty-array sentinel, got " << missing.value_or(json()).dump();
    auto missing_parent = client_->json_get(k, "$.nope");
    EXPECT_TRUE(missing_parent && missing_parent->is_array() && missing_parent->empty())
        << "expected empty-array sentinel, got " << missing_parent.value_or(json()).dump();

    client_->json_delete(k, "$");
}

TEST_F(RedisJsonClientTest, JsonGetMissingKeyReturnsNullopt) {
    const auto k = unique_key("json_missing");
    auto v = client_->json_get(k, "$");
    EXPECT_FALSE(v.has_value());
}

TEST_F(RedisJsonClientTest, JsonDeleteSubpath) {
    const auto k = unique_key("json_del");
    client_->json_set(k, "$", json{{"a", 1}, {"b", 2}});
    EXPECT_TRUE(client_->json_delete(k, "$.a"));
    auto v = client_->json_get(k, "$");
    ASSERT_TRUE(v.has_value());
    EXPECT_FALSE(v->contains("a"));
    EXPECT_TRUE(v->contains("b"));
    client_->json_delete(k, "$");
}

TEST_F(RedisJsonClientTest, JsonDeleteMissingPathReturnsFalse) {
    const auto k = unique_key("json_del_missing");
    client_->json_set(k, "$", json{{"a", 1}});
    EXPECT_FALSE(client_->json_delete(k, "$.no_such_path"));
    client_->json_delete(k, "$");
}

TEST_F(RedisJsonClientTest, JsonSetRootWithTtlExpires) {
    const auto k = unique_key("json_ttl");
    client_->json_set(k, "$", json{{"v", 1}}, /*expiry_sec=*/1);
    ASSERT_TRUE(client_->json_get(k, "$").has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    EXPECT_FALSE(client_->json_get(k, "$").has_value());
}

}  // namespace
