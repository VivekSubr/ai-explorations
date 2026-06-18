// Implementation of the REST router declared in rest.h. See the OpenAPI spec
// at openapi/distributed_cache.yaml for the contract this implements.

#include "rest.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <stdexcept>
#include <utility>
#include <vector>

// Generated from openapi/distributed_cache.yaml. Including these ties the
// REST handler to the spec: regenerating after a schema change (e.g. a
// renamed field) yields a compile error here rather than silent breakage.
#include "Error.h"
#include "ErrorCode.h"
#include "Helpers.h"            // ValidationException
#include "WrappedJsonValue.h"
// Operation manifest emitted by openapi/gen-operations.py. Adding/removing
// an operation in the YAML changes the OpId enum, which makes the dispatch
// switch below non-exhaustive and breaks the build (with -Wswitch-enum /
// -Werror=switch) at the exact call sites that need updating.
#include "operations.h"

namespace dist_cache::rest {

using nlohmann::json;
using json_pointer = nlohmann::json::json_pointer;
using org::openapitools::server::model::Error;
using org::openapitools::server::model::ErrorCode;
using org::openapitools::server::model::WrappedJsonValue;
using org::openapitools::server::helpers::ValidationException;
using dist_cache::spec::OpId;
namespace err = dist_cache::spec::err;

namespace {

// ---------- small URL / path helpers ----------------------------------------

// Split "/json?$.foo" into ("/json", "$.foo"). The query is returned exactly
// as it appears on the wire (no percent-decoding) because the spec describes
// the JSONPath as "the raw query string".
std::pair<std::string_view, std::string_view> split_target(
    std::string_view target) {
    auto q = target.find('?');
    if (q == std::string_view::npos) return {target, {}};
    return {target.substr(0, q), target.substr(q + 1)};
}

Response error(int status, ErrorCode::eErrorCode code,
               std::string_view msg = {}) {
    // Build the response body through the generated `Error` model so the
    // wire shape is governed by the OpenAPI spec, not by ad-hoc strings.
    Error e;
    ErrorCode ec;
    ec.setValue(code);
    e.setError(ec);
    if (!msg.empty()) e.setMessage(std::string(msg));
    json j;
    to_json(j, e);
    return {status, "application/json", j.dump()};
}

Response ok_empty(int status = 200) { return {status, "", ""}; }

Response ok_json(const json& j) {
    return {200, "application/json", j.dump()};
}

// Extract (data, expiry_sec) from a request body that may either be the raw
// JSON value or a wrapped {"data": ..., "expiry_sec": N} object. The wrapped
// form is recognized by attempting to parse the body as the generated
// `WrappedJsonValue`; if that fails we treat the body as a raw JSON value.
// Either way, the generated `validate()` enforces spec-defined constraints
// (e.g. expiry_sec >= 1) before we touch the store.
struct ParsedBody {
    json data;
    std::optional<int64_t> expiry_sec;
};
ParsedBody unwrap_body(const json& parsed) {
    ParsedBody pb;
    if (parsed.is_object() && parsed.contains("data")) {
        WrappedJsonValue w;
        from_json(parsed, w);   // throws nlohmann exception if shape mismatches
        w.validate();           // throws ValidationException on constraint fail
        pb.data = w.getData();
        if (w.expirySecIsSet()) pb.expiry_sec = w.getExpirySec();
    } else {
        pb.data = parsed;
    }
    return pb;
}

// ---------- JSONPath ---------------------------------------------------------
//
// We support a tiny JSONPath subset and translate it to an RFC 6901 JSON
// Pointer so that nlohmann::json's built-in pointer machinery (contains/at/
// erase/operator[]) does the actual traversal and mutation. Supported:
//   "$"                  -> ""           (root)
//   "$.a.b.c"            -> "/a/b/c"
//   "$.a[0].b[12]"       -> "/a/0/b/12"
// Throws std::invalid_argument on malformed input.

// Append `tok` to `out` with RFC 6901 escaping (~ -> ~0, / -> ~1).
void append_escaped(std::string& out, std::string_view tok) {
    out.push_back('/');
    for (char c : tok) {
        if (c == '~')      out += "~0";
        else if (c == '/') out += "~1";
        else               out.push_back(c);
    }
}

json_pointer jsonpath_to_pointer(std::string_view p) {
    if (p.empty() || p[0] != '$') {
        throw std::invalid_argument("JSONPath must start with '$'");
    }
    std::string ptr;          // RFC 6901 string; "" means root
    size_t i = 1;
    while (i < p.size()) {
        if (p[i] == '.') {
            ++i;
            size_t start = i;
            while (i < p.size() && p[i] != '.' && p[i] != '[') ++i;
            if (start == i) throw std::invalid_argument("empty path segment");
            append_escaped(ptr, p.substr(start, i - start));
        } else if (p[i] == '[') {
            ++i;
            size_t start = i;
            while (i < p.size() && p[i] != ']') ++i;
            if (i == p.size() || start == i) {
                throw std::invalid_argument("malformed array index");
            }
            size_t idx = 0;
            auto [end, ec] = std::from_chars(p.data() + start,
                                             p.data() + i, idx);
            if (ec != std::errc{} || end != p.data() + i) {
                throw std::invalid_argument("non-integer array index");
            }
            ptr.push_back('/');
            ptr.append(std::to_string(idx));
            ++i;  // skip ']'
        } else {
            throw std::invalid_argument("expected '.' or '['");
        }
    }
    try {
        return json_pointer(ptr);
    } catch (const json::exception& e) {
        throw std::invalid_argument(e.what());
    }
}

}  // namespace

// ---------- Store impl -------------------------------------------------------

std::optional<Store::Clock::time_point> Store::to_expiry(
    std::optional<int64_t> expiry_sec) {
    if (!expiry_sec) return std::nullopt;
    return Clock::now() + std::chrono::seconds(*expiry_sec);
}

bool Store::kv_expired(const KvEntry& e) const {
    return e.expiry && Clock::now() >= *e.expiry;
}

void Store::kv_set(const std::string& key, std::string value,
                   std::optional<int64_t> expiry_sec) {
    kv_[key] = KvEntry{std::move(value), to_expiry(expiry_sec)};
}

std::optional<std::string> Store::kv_get(const std::string& key) {
    auto it = kv_.find(key);
    if (it == kv_.end()) return std::nullopt;
    if (kv_expired(it->second)) {
        kv_.erase(it);
        return std::nullopt;
    }
    return it->second.value;
}

bool Store::kv_delete(const std::string& key) {
    return kv_.erase(key) > 0;
}

std::optional<json> Store::json_get(const std::string& path) const {
    if (json_.expiry && Clock::now() >= *json_.expiry) return std::nullopt;
    auto ptr = jsonpath_to_pointer(path);
    if (!json_.root.contains(ptr)) return std::nullopt;
    return json_.root.at(ptr);
}

bool Store::json_set(const std::string& path, json value,
                     std::optional<int64_t> expiry_sec) {
    auto ptr = jsonpath_to_pointer(path);
    if (ptr.empty()) {
        // root assignment: expiry allowed.
        json_.root = std::move(value);
        json_.expiry = to_expiry(expiry_sec);
        return true;
    }
    if (expiry_sec) {
        // Spec: expiry_sec only allowed at root.
        return false;
    }
    // Require parent to exist; the final token addresses the slot to assign.
    auto parent_ptr = ptr.parent_pointer();
    if (!json_.root.contains(parent_ptr)) return false;
    try {
        json_.root[ptr] = std::move(value);
        return true;
    } catch (const json::exception&) {
        return false;
    }
}

bool Store::json_delete(const std::string& path) {
    auto ptr = jsonpath_to_pointer(path);
    if (ptr.empty()) {
        // delete root => reset to null
        json_.root = nullptr;
        json_.expiry = std::nullopt;
        return true;
    }
    if (!json_.root.contains(ptr)) return false;
    try {
        json_.root.at(ptr.parent_pointer()).erase(ptr.back());
        return true;
    } catch (const json::exception&) {
        return false;
    }
}

void Store::sweep_expired() {
    auto now = Clock::now();
    for (auto it = kv_.begin(); it != kv_.end();) {
        if (it->second.expiry && now >= *it->second.expiry) {
            it = kv_.erase(it);
        } else {
            ++it;
        }
    }
    if (json_.expiry && now >= *json_.expiry) {
        json_.root = nullptr;
        json_.expiry = std::nullopt;
    }
}

// ---------- Router -----------------------------------------------------------

namespace {

// Per-operation handlers. Each receives the already-parsed (path-param, body)
// pair so the dispatch loop owns all parsing/lookup. Returning Response keeps
// transport details out.

Response op_json_get(Store& store, std::string_view jsonpath,
                     std::string_view /*body*/) {
    if (jsonpath.empty()) {
        return error(400, err::kMissingJsonpath,
                     "JSONPath must be supplied as the query string");
    }
    (void)jsonpath_to_pointer(jsonpath);
    auto v = store.json_get(std::string(jsonpath));
    if (!v) return error(404, err::kNotFound);
    return ok_json(*v);
}

Response op_json_post(Store& store, std::string_view jsonpath,
                      std::string_view body) {
    if (jsonpath.empty()) {
        return error(400, err::kMissingJsonpath,
                     "JSONPath must be supplied as the query string");
    }
    (void)jsonpath_to_pointer(jsonpath);
    if (body.empty()) return error(400, err::kEmptyBody);
    json parsed = json::parse(body);
    auto pb = unwrap_body(parsed);
    if (!store.json_set(std::string(jsonpath), std::move(pb.data),
                        pb.expiry_sec)) {
        return error(404, err::kNotFound,
                     "parent path does not exist or expiry_sec used off-root");
    }
    return ok_empty(200);
}

Response op_json_delete(Store& store, std::string_view jsonpath,
                        std::string_view /*body*/) {
    if (jsonpath.empty()) {
        return error(400, err::kMissingJsonpath,
                     "JSONPath must be supplied as the query string");
    }
    (void)jsonpath_to_pointer(jsonpath);
    if (!store.json_delete(std::string(jsonpath))) {
        return error(404, err::kNotFound);
    }
    return ok_empty(204);
}

Response op_key_get(Store& store, std::string_view key,
                    std::string_view /*body*/) {
    if (key.empty()) return error(400, err::kMissingKey);
    auto v = store.kv_get(std::string(key));
    if (!v) return error(404, err::kNotFound);
    // Heuristic: if the stored value parses as JSON, advertise it as such;
    // otherwise return text/plain. The spec allows either.
    try {
        auto j = json::parse(*v);
        return ok_json(j);
    } catch (...) {
        return {200, "text/plain", *v};
    }
}

Response op_key_post(Store& store, std::string_view key,
                     std::string_view body) {
    if (key.empty()) return error(400, err::kMissingKey);
    if (body.empty()) return error(400, err::kEmptyBody);
    std::optional<int64_t> expiry;
    std::string value;
    try {
        auto parsed = json::parse(body);
        auto pb = unwrap_body(parsed);
        expiry = pb.expiry_sec;
        value = pb.data.is_string() ? pb.data.get<std::string>()
                                    : pb.data.dump();
    } catch (const json::exception&) {
        value.assign(body);
    }
    store.kv_set(std::string(key), std::move(value), expiry);
    return ok_empty(200);
}

Response op_key_delete(Store& store, std::string_view key,
                       std::string_view /*body*/) {
    if (key.empty()) return error(400, err::kMissingKey);
    if (!store.kv_delete(std::string(key))) return error(404, err::kNotFound);
    return ok_empty(204);
}

// Match (method, request_path) against the spec's operation manifest. On
// success, returns the matched OpId plus the captured path parameter (or the
// query string for /json operations whose JSONPath lives in the query).
struct Match {
    OpId             op;
    std::string_view path_param;  // value of the {key} segment, or jsonpath
};

std::optional<Match> match_op(std::string_view method,
                              std::string_view req_path,
                              std::string_view query) {
    for (const auto& decl : dist_cache::spec::kOperations) {
        if (decl.method != method) continue;
        const auto tmpl = decl.path_template;
        // Template with a single trailing "/{name}" placeholder: capture the
        // remainder. Otherwise require exact match.
        auto open = tmpl.find('{');
        if (open == std::string_view::npos) {
            if (req_path != tmpl) continue;
            // Operations whose JSONPath is supplied via the query string
            // (today: every /json operation) capture the query as their
            // path_param. This keeps op_*_get/post/delete uniform.
            return Match{decl.id, query};
        }
        // Static prefix before "{...}" must match; capture the rest as the
        // single path parameter. (No support for multiple placeholders; the
        // spec doesn't use any.)
        auto prefix = tmpl.substr(0, open);
        if (req_path.size() <= prefix.size()) continue;
        if (req_path.substr(0, prefix.size()) != prefix) continue;
        return Match{decl.id, req_path.substr(prefix.size())};
    }
    return std::nullopt;
}

Response dispatch(Store& store, OpId op, std::string_view path_param,
                  std::string_view body) {
    // Exhaustive switch over the generated OpId enum. With -Wswitch-enum or
    // -Werror=switch this is the compile-time anchor that catches spec drift:
    // adding an operation to the YAML adds an enumerator, missing-case on it
    // becomes a build error here.
    switch (op) {
        case OpId::JsonGet:    return op_json_get(store, path_param, body);
        case OpId::JsonPost:   return op_json_post(store, path_param, body);
        case OpId::JsonDelete: return op_json_delete(store, path_param, body);
        case OpId::KeyGet:     return op_key_get(store, path_param, body);
        case OpId::KeyPost:    return op_key_post(store, path_param, body);
        case OpId::KeyDelete:  return op_key_delete(store, path_param, body);
    }
    // Unreachable when the switch is exhaustive; required to silence
    // compilers that don't recognize the exhaustiveness.
    return error(500, err::kBadRequest, "unhandled operation");
}

}  // namespace

Response handle(Store& store, std::string_view method,
                std::string_view target, std::string_view body) {
    auto [path, query] = split_target(target);

    if (path.empty() || path[0] != '/') {
        return error(400, err::kBadPath);
    }

    auto m = match_op(method, path, query);
    if (!m) {
        // No (method, path) matched. Distinguish "wrong method on a known
        // path" from "unknown path" so clients get useful diagnostics.
        for (const auto& decl : dist_cache::spec::kOperations) {
            if (decl.path_template == path ||
                (decl.path_template.find('{') != std::string_view::npos &&
                 path.rfind(decl.path_template.substr(
                     0, decl.path_template.find('{')), 0) == 0)) {
                return error(405, err::kMethodNotAllowed);
            }
        }
        return error(404, err::kNotFound);
    }

    try {
        return dispatch(store, m->op, m->path_param, body);
    } catch (const ValidationException& e) {
        return error(400, err::kValidationFailed, e.what());
    } catch (const json::exception& e) {
        return error(400, err::kBadJson, e.what());
    } catch (const std::invalid_argument& e) {
        return error(400, err::kBadJsonpath, e.what());
    } catch (const std::exception& e) {
        return error(400, err::kBadRequest, e.what());
    }
}

}  // namespace dist_cache::rest

