// Tests for the transport-agnostic REST router defined in rest.h / rest.cc.
// These exercise the OpenAPI contract directly (no HTTP/3, no sockets).

#include "rest.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {

using dist_cache::rest::Store;
using dist_cache::rest::handle;
using nlohmann::json;

TEST(RestKv, RoundTripRawValue) {
    Store s;
    auto r = handle(s, "POST", "/foo", "hello world");
    EXPECT_EQ(r.status, 200);

    r = handle(s, "GET", "/foo", "");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.content_type, "text/plain");
    EXPECT_EQ(r.body, "hello world");

    r = handle(s, "DELETE", "/foo", "");
    EXPECT_EQ(r.status, 204);

    r = handle(s, "GET", "/foo", "");
    EXPECT_EQ(r.status, 404);
}

TEST(RestKv, WrappedValueIsUnwrapped) {
    Store s;
    auto r = handle(s, "POST", "/k",
                    R"({"data": {"a": 1}, "expiry_sec": 60})");
    EXPECT_EQ(r.status, 200);

    r = handle(s, "GET", "/k", "");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.content_type, "application/json");
    EXPECT_EQ(json::parse(r.body), json::parse(R"({"a":1})"));
}

TEST(RestKv, ReservedJsonPathIsNotAKey) {
    Store s;
    // /json without ?<jsonpath> is a JSON op missing its required param.
    auto r = handle(s, "GET", "/json", "");
    EXPECT_EQ(r.status, 400);
}

TEST(RestJson, SetRootGetSubpath) {
    Store s;
    auto r = handle(s, "POST", "/json?$",
                    R"({"user": {"name": "alice", "age": 30}})");
    EXPECT_EQ(r.status, 200);

    r = handle(s, "GET", "/json?$.user.name", "");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(json::parse(r.body), "alice");

    r = handle(s, "GET", "/json?$.user.missing", "");
    EXPECT_EQ(r.status, 404);
}

TEST(RestJson, SetSubpathThenDelete) {
    Store s;
    handle(s, "POST", "/json?$", R"({"u": {"n": "a"}})");

    auto r = handle(s, "POST", "/json?$.u.n", R"("bob")");
    EXPECT_EQ(r.status, 200);

    r = handle(s, "GET", "/json?$.u.n", "");
    EXPECT_EQ(json::parse(r.body), "bob");

    r = handle(s, "DELETE", "/json?$.u.n", "");
    EXPECT_EQ(r.status, 204);

    r = handle(s, "GET", "/json?$.u.n", "");
    EXPECT_EQ(r.status, 404);
}

TEST(RestJson, ArrayIndex) {
    Store s;
    handle(s, "POST", "/json?$", R"({"xs": [10, 20, 30]})");
    auto r = handle(s, "GET", "/json?$.xs[1]", "");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(json::parse(r.body), 20);

    r = handle(s, "POST", "/json?$.xs[1]", "99");
    EXPECT_EQ(r.status, 200);

    r = handle(s, "GET", "/json?$.xs[1]", "");
    EXPECT_EQ(json::parse(r.body), 99);
}

TEST(RestJson, ExpirySecOnlyAllowedAtRoot) {
    Store s;
    handle(s, "POST", "/json?$", R"({"u": {"n": "a"}})");
    auto r = handle(s, "POST", "/json?$.u",
                    R"({"data": {"n": "z"}, "expiry_sec": 5})");
    EXPECT_EQ(r.status, 404);  // spec: expiry_sec is root-only
}

TEST(RestJson, BadJsonPathIs400) {
    Store s;
    auto r = handle(s, "GET", "/json?not_a_path", "");
    EXPECT_EQ(r.status, 400);
}

}  // namespace
