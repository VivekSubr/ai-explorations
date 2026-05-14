#!/usr/bin/env bash
# Helpers for running dist_cache.exe (HTTP/3 server) in tests.
set -euo pipefail

# generate_self_signed_cert <out_dir> [cn] [days]
#
# Generates a self-signed TLS cert + key suitable for local HTTP/3 testing.
# Writes <out_dir>/cert.pem and <out_dir>/key.pem.
#
# Echoes the absolute paths of the cert and key (space separated) on stdout
# so callers can capture them, e.g.:
#     read -r CERT KEY < <(generate_self_signed_cert /tmp/dc-certs)
generate_self_signed_cert() {
    local out_dir="${1:?out_dir required}"
    local cn="${2:-localhost}"
    local days="${3:-1}"

    mkdir -p "$out_dir"
    local cert="$out_dir/cert.pem"
    local key="$out_dir/key.pem"

    if ! command -v openssl >/dev/null 2>&1; then
        echo "openssl not found; cannot generate self-signed cert" >&2
        return 1
    fi

    # SAN config so curl --resolve / IP-based clients are happy.
    local san_cfg
    san_cfg=$(mktemp)
    cat >"$san_cfg" <<EOF
[req]
distinguished_name = dn
x509_extensions = v3_req
prompt = no
[dn]
CN = ${cn}
[v3_req]
subjectAltName = @alt
basicConstraints = CA:FALSE
keyUsage = digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
[alt]
DNS.1 = ${cn}
DNS.2 = localhost
IP.1  = 127.0.0.1
IP.2  = ::1
EOF

    openssl req -x509 -newkey rsa:2048 -nodes \
        -keyout "$key" -out "$cert" \
        -days "$days" \
        -config "$san_cfg" >/dev/null 2>&1

    rm -f "$san_cfg"
    chmod 600 "$key"
    echo "$cert $key"
}

# ---------------------------------------------------------------------------
# setup / teardown
# ---------------------------------------------------------------------------
#
# setup() launches:
#   1. redis-server, using redis/redis.conf next to this script
#   2. dist_cache.exe, from server/build/
# teardown() (registered via trap) kills both and removes the temp dir.
# Both PIDs and a scratch dir are exported so tests can reach them:
#   $DC_HOST, $DC_PORT       — distributed-cache server bind address
#   $DC_CERT, $DC_KEY        — TLS material the server is using
#   $REDIS_PORT              — redis-server port
#   $DC_TMPDIR               — per-run scratch dir (auto-cleaned)

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DC_BIN_DEFAULT="$REPO_ROOT/server/build/dist_cache.exe"
REDIS_CONF_DEFAULT="$REPO_ROOT/redis/redis.conf"

DC_HOST="${DC_HOST:-127.0.0.1}"
DC_PORT="${DC_PORT:-4433}"
REDIS_PORT="${REDIS_PORT:-6379}"
DC_BIN="${DC_BIN:-$DC_BIN_DEFAULT}"
REDIS_CONF="${REDIS_CONF:-$REDIS_CONF_DEFAULT}"

DC_PID=""
REDIS_PID=""
DC_TMPDIR=""

# wait_for_port <port> [host] [timeout_sec]
# Returns 0 once <host>:<port> accepts a TCP connection, 1 on timeout.
wait_for_port() {
    local port="$1" host="${2:-127.0.0.1}" timeout="${3:-5}"
    local deadline=$(( SECONDS + timeout ))
    while (( SECONDS < deadline )); do
        if (exec 3<>"/dev/tcp/${host}/${port}") 2>/dev/null; then
            exec 3<&- 3>&-
            return 0
        fi
        sleep 0.1
    done
    return 1
}

# curl_h3_supported  -> 0 if the local curl can speak HTTP/3, else 1.
curl_h3_supported() {
    command -v curl >/dev/null 2>&1 || return 1
    curl --version 2>/dev/null | grep -qiE 'Features:.*HTTP3'
}

# wait_for_http3 <host> <port> [timeout_sec]
# Probes <host>:<port> with `curl --http3-only` until it gets ANY HTTP reply
# (success OR error -- both prove the server is up and decoding QUIC).
# Returns 1 on timeout, 2 if the local curl lacks HTTP/3 support.
wait_for_http3() {
    local host="$1" port="$2" timeout="${3:-5}"
    curl_h3_supported || return 2
    local url="https://${host}:${port}/__readiness__"
    local deadline=$(( SECONDS + timeout ))
    while (( SECONDS < deadline )); do
        # --http3-only forces QUIC (no Alt-Svc upgrade dance).
        # -k accepts the self-signed cert; --max-time keeps us snappy.
        # Any HTTP status (even 404) means the handler answered.
        if curl -ksS --http3-only --max-time 1 \
                -o /dev/null -w '%{http_code}' "$url" 2>/dev/null \
                | grep -qE '^[1-5][0-9][0-9]$'; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

setup() {
    [[ -x "$DC_BIN"      ]] || { echo "dist_cache.exe not built at $DC_BIN (run 'make -C server cmake')" >&2; return 1; }
    [[ -f "$REDIS_CONF"  ]] || { echo "redis conf missing at $REDIS_CONF" >&2; return 1; }
    command -v redis-server >/dev/null 2>&1 \
        || { echo "redis-server not on PATH" >&2; return 1; }

    DC_TMPDIR=$(mktemp -d)
    trap teardown EXIT INT TERM

    # 1. redis-server. --port on the command line overrides anything in the
    # conf file, keeping the test harness self-contained.
    local redis_log="$DC_TMPDIR/redis.log"
    redis-server "$REDIS_CONF" --port "$REDIS_PORT" --daemonize no \
        >"$redis_log" 2>&1 &
    REDIS_PID=$!
    if ! wait_for_port "$REDIS_PORT" 127.0.0.1 5; then
        echo "redis-server failed to listen on $REDIS_PORT; log:" >&2
        sed 's/^/  /' "$redis_log" >&2
        return 1
    fi

    # 2. dist_cache.exe. Generate per-run TLS material so concurrent runs
    # don't collide on a shared cert/key.
    read -r DC_CERT DC_KEY < <(generate_self_signed_cert "$DC_TMPDIR/certs")
    export DC_CERT DC_KEY
    local dc_log="$DC_TMPDIR/dist_cache.log"
    "$DC_BIN" "$DC_HOST" "$DC_PORT" "$DC_CERT" "$DC_KEY" \
        >"$dc_log" 2>&1 &
    DC_PID=$!
    # Readiness probe: prefer a real HTTP/3 round-trip via curl; fall back to
    # a process-alive check when the local curl lacks HTTP/3 support (older
    # distro builds). Either way, fail fast if the binary exited.
    local probe_rc=0
    wait_for_http3 "$DC_HOST" "$DC_PORT" 5 || probe_rc=$?
    case $probe_rc in
        0) ;;  # got an HTTP reply -- ready
        2) sleep 0.3 ;;  # curl has no HTTP/3; best we can do is alive check
        *) echo "dist_cache.exe did not respond on ${DC_HOST}:${DC_PORT}; log:" >&2
           sed 's/^/  /' "$dc_log" >&2
           return 1 ;;
    esac
    if ! kill -0 "$DC_PID" 2>/dev/null; then
        echo "dist_cache.exe exited during startup; log:" >&2
        sed 's/^/  /' "$dc_log" >&2
        return 1
    fi

    export DC_HOST DC_PORT REDIS_PORT DC_TMPDIR
    echo "setup: redis pid=$REDIS_PID port=$REDIS_PORT; "\
"dist_cache pid=$DC_PID ${DC_HOST}:${DC_PORT}"
}

teardown() {
    local pid
    for pid in "$DC_PID" "$REDIS_PID"; do
        [[ -n "$pid" ]] || continue
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
    DC_PID=""; REDIS_PID=""
    [[ -n "$DC_TMPDIR" && -d "$DC_TMPDIR" ]] && rm -rf "$DC_TMPDIR"
    DC_TMPDIR=""
}

# ---------------------------------------------------------------------------
# Test runner
# ---------------------------------------------------------------------------
#
# Define each test as a function named `tc_<id>` (e.g. tc_TC1) and add the
# id to TESTS below. Run via:
#   ./dist_cache_test.sh                 # all
#   ./dist_cache_test.sh TC1             # exact match
#   ./dist_cache_test.sh 'TC1*'          # glob: TC1, TC11, TC101, ...
#   ./dist_cache_test.sh TC2 'TC1?'      # multiple filters (OR-combined)
# Quote globs so the shell doesn't expand them against the filesystem.

TESTS=(
    TC_KV_SET_GET
    TC_KV_GET_MISSING
    TC_KV_DELETE
    TC_KV_DELETE_MISSING
    TC_KV_TTL_WRAPPED
    TC_KV_EMPTY_BODY
    TC_JSON_SET_GET_ROOT
    TC_JSON_SET_SUBPATH
    TC_JSON_GET_MISSING_PATH
    TC_JSON_DELETE
    TC_JSON_MISSING_QUERY
    TC_JSON_BAD_PATH
)

# ---- individual test cases -------------------------------------------------
# Each tc_* function should `return 0` on success and non-zero on failure.
# Use `fail "msg"` for a clear diagnostic, or `skip "msg"` to mark the test
# as not-applicable in the current environment (e.g. curl lacks HTTP/3).

fail() { echo "  FAIL: $*" >&2; exit 1; }
skip() { echo "  SKIP: $*" >&2; exit 77; }

# Marker for tests that need an HTTP/3-capable curl. Skips the test cleanly
# when curl can't reach the server.
require_h3() {
    curl_h3_supported || skip "local curl lacks HTTP/3 support"
}

# dc_curl <method> <path> [body] [content_type]
# Issues an HTTP/3 request to the server brought up by setup(). Writes the
# response body to stdout and the numeric HTTP status to stderr's last line
# via the trailing `STATUS=NNN` marker. Use `dc_status` / `dc_body` to parse.
#
# Implementation: -w '\nSTATUS=%{http_code}' appends a marker line we strip
# from the body. Keeps the call site dependency-free (no jq/python needed).
dc_curl() {
    local method="$1" path="$2" body="${3:-}" ctype="${4:-application/json}"
    local url="https://${DC_HOST}:${DC_PORT}${path}"
    local args=(-ksS --http3-only --max-time 5
                -X "$method"
                -H "content-type: ${ctype}"
                -w '\nSTATUS=%{http_code}\n'
                "$url")
    if [[ -n "$body" ]]; then
        args+=(--data-binary "$body")
    fi
    curl "${args[@]}" 2>/dev/null
}

# Extract the STATUS=NNN trailer from a dc_curl response.
dc_status() { sed -n 's/^STATUS=\([0-9]\{3\}\)$/\1/p' <<<"$1" | tail -n1; }
# Strip the trailer to leave just the body.
dc_body()   { sed '/^STATUS=[0-9]\{3\}$/d' <<<"$1"; }

# Random suffix so concurrent runs don't collide on the same key.
_DC_RUN_TAG="$$_$RANDOM"

tc_TC_KV_SET_GET() {
    require_h3
    local k="k_set_get_${_DC_RUN_TAG}"
    local r; r=$(dc_curl POST   "/$k" 'hello world' text/plain)
    [[ "$(dc_status "$r")" == 200 ]] || fail "POST status $(dc_status "$r")"
    r=$(dc_curl GET "/$k")
    [[ "$(dc_status "$r")" == 200 ]] || fail "GET status $(dc_status "$r")"
    [[ "$(dc_body "$r")" == "hello world" ]] \
        || fail "GET body mismatch: $(dc_body "$r")"
    dc_curl DELETE "/$k" >/dev/null
}

tc_TC_KV_GET_MISSING() {
    require_h3
    local r; r=$(dc_curl GET "/missing_${_DC_RUN_TAG}")
    [[ "$(dc_status "$r")" == 404 ]] || fail "expected 404, got $(dc_status "$r")"
    dc_body "$r" | grep -q '"error"' \
        || fail "missing error JSON in body"
}

tc_TC_KV_DELETE() {
    require_h3
    local k="k_del_${_DC_RUN_TAG}"
    dc_curl POST "/$k" 'v' text/plain >/dev/null
    local r; r=$(dc_curl DELETE "/$k")
    [[ "$(dc_status "$r")" == 204 ]] || fail "DELETE status $(dc_status "$r")"
    r=$(dc_curl GET "/$k")
    [[ "$(dc_status "$r")" == 404 ]] || fail "expected 404 after delete"
}

tc_TC_KV_DELETE_MISSING() {
    require_h3
    local r; r=$(dc_curl DELETE "/no_such_${_DC_RUN_TAG}")
    [[ "$(dc_status "$r")" == 404 ]] || fail "expected 404, got $(dc_status "$r")"
}

tc_TC_KV_TTL_WRAPPED() {
    require_h3
    local k="k_ttl_${_DC_RUN_TAG}"
    local r; r=$(dc_curl POST "/$k" '{"data":"v","expiry_sec":60}')
    [[ "$(dc_status "$r")" == 200 ]] || fail "POST status $(dc_status "$r")"
    r=$(dc_curl GET "/$k")
    [[ "$(dc_status "$r")" == 200 ]] || fail "GET status $(dc_status "$r")"
    dc_curl DELETE "/$k" >/dev/null
}

tc_TC_KV_EMPTY_BODY() {
    require_h3
    local r; r=$(dc_curl POST "/k_empty_${_DC_RUN_TAG}" '' text/plain)
    [[ "$(dc_status "$r")" == 400 ]] || fail "expected 400, got $(dc_status "$r")"
    dc_body "$r" | grep -q 'empty_body' || fail "expected empty_body error"
}

tc_TC_JSON_SET_GET_ROOT() {
    require_h3
    local r; r=$(dc_curl POST '/json?$' '{"hello":"world"}')
    [[ "$(dc_status "$r")" == 200 ]] || fail "POST status $(dc_status "$r")"
    r=$(dc_curl GET '/json?$.hello')
    [[ "$(dc_status "$r")" == 200 ]] || fail "GET status $(dc_status "$r")"
    [[ "$(dc_body "$r")" == '"world"' ]] \
        || fail "GET body mismatch: $(dc_body "$r")"
    dc_curl DELETE '/json?$' >/dev/null
}

tc_TC_JSON_SET_SUBPATH() {
    require_h3
    dc_curl POST '/json?$' '{"user":{"name":"alice"}}' >/dev/null
    local r; r=$(dc_curl POST '/json?$.user.age' '30')
    [[ "$(dc_status "$r")" == 200 ]] || fail "POST status $(dc_status "$r")"
    r=$(dc_curl GET '/json?$.user.age')
    [[ "$(dc_body "$r")" == '30' ]] || fail "got $(dc_body "$r")"
    dc_curl DELETE '/json?$' >/dev/null
}

tc_TC_JSON_GET_MISSING_PATH() {
    require_h3
    dc_curl POST '/json?$' '{"a":1}' >/dev/null
    local r; r=$(dc_curl GET '/json?$.no.such.path')
    [[ "$(dc_status "$r")" == 404 ]] || fail "expected 404, got $(dc_status "$r")"
    dc_curl DELETE '/json?$' >/dev/null
}

tc_TC_JSON_DELETE() {
    require_h3
    dc_curl POST '/json?$' '{"a":1,"b":2}' >/dev/null
    local r; r=$(dc_curl DELETE '/json?$.a')
    [[ "$(dc_status "$r")" == 204 ]] || fail "DELETE status $(dc_status "$r")"
    r=$(dc_curl GET '/json?$.a')
    [[ "$(dc_status "$r")" == 404 ]] || fail "expected 404 after delete"
    dc_curl DELETE '/json?$' >/dev/null
}

tc_TC_JSON_MISSING_QUERY() {
    require_h3
    local r; r=$(dc_curl GET '/json')
    [[ "$(dc_status "$r")" == 400 ]] || fail "expected 400, got $(dc_status "$r")"
    dc_body "$r" | grep -q 'missing_jsonpath' \
        || fail "expected missing_jsonpath error"
}

tc_TC_JSON_BAD_PATH() {
    require_h3
    local r; r=$(dc_curl GET '/json?notapath')
    [[ "$(dc_status "$r")" == 400 ]] || fail "expected 400, got $(dc_status "$r")"
    dc_body "$r" | grep -q 'bad_jsonpath' \
        || fail "expected bad_jsonpath error"
}

# ---- dispatcher ------------------------------------------------------------

# match_any <name> <pattern>...  -> 0 if `name` matches any of the patterns,
# 1 otherwise. Empty pattern list => match everything. Patterns use bash
# `[[ == ]]` globbing, so '*' and '?' work as expected.
match_any() {
    local name="$1"; shift
    [[ $# -eq 0 ]] && return 0
    local pat
    for pat in "$@"; do
        [[ "$name" == $pat ]] && return 0
    done
    return 1
}

run_tests() {
    local filters=("$@")
    local passed=0 failed=0 skipped=0 nf_skipped=0
    local failed_names=()
    local id
    for id in "${TESTS[@]}"; do
        if ! match_any "$id" "${filters[@]}"; then
            nf_skipped=$((nf_skipped + 1)); continue
        fi
        echo "[ RUN      ] $id"
        # Run inside a subshell so per-test `trap ... RETURN`, `set -e`
        # exits, and cwd changes don't leak between cases. Exit code 77
        # (de-facto autotools convention) means the test opted to skip.
        local rc=0
        ( set -e; "tc_$id" ) || rc=$?
        case $rc in
            0)  echo "[       OK ] $id"; passed=$((passed + 1)) ;;
            77) echo "[  SKIPPED ] $id"; skipped=$((skipped + 1)) ;;
            *)  echo "[  FAILED  ] $id"; failed=$((failed + 1))
                failed_names+=("$id") ;;
        esac
    done
    echo
    echo "$passed passed, $failed failed, $skipped skipped, "\
"$nf_skipped filtered (of ${#TESTS[@]})"
    if ((failed > 0)); then
        printf '  - %s\n' "${failed_names[@]}" >&2
        return 1
    fi
    return 0
}

# Example usage when this script is run directly:
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    setup
    # Capture run_tests' exit so teardown still runs even when tests fail.
    # The EXIT trap registered inside setup() also covers Ctrl-C / signals.
    rc=0
    run_tests "$@" || rc=$?
    teardown
    trap - EXIT INT TERM
    exit "$rc"
fi
