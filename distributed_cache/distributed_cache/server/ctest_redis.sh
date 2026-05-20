#!/usr/bin/env bash
# CTest fixture helper for redis_client_test.
#
# Usage:
#   ctest_redis.sh up   <state_dir> <conf>
#   ctest_redis.sh down <state_dir>
#
# Host/port are read from the REDIS_HOST / REDIS_PORT environment variables
# (defaults: 127.0.0.1 / 6379), which CTest inherits from its own environment
# and forwards to fixture commands and dependent tests alike.
#
# `up` first probes <host>:<port> with redis-cli ping; if a server is already
# answering, it leaves it alone (no pid file is written, so `down` is a
# no-op). Otherwise it launches `redis-server <conf> --port <port>` in the
# background, records the pid, and waits until the port accepts connections.
#
# `down` reads <state_dir>/redis.pid (if present) and TERM-then-KILLs that
# process. Never errors out -- ctest cleanup must be idempotent so reruns of
# a failed ctest don't get stuck.
set -euo pipefail

cmd="${1:?up|down required}"
state_dir="${2:?state_dir required}"
mkdir -p "$state_dir"
pid_file="$state_dir/redis.pid"
log_file="$state_dir/redis.log"

host="${REDIS_HOST:-127.0.0.1}"
port="${REDIS_PORT:-6379}"

ping_ok() {
    command -v redis-cli >/dev/null 2>&1 || return 1
    redis-cli -h "$host" -p "$port" ping 2>/dev/null | grep -q '^PONG$'
}

case "$cmd" in
    up)
        conf="${3:?conf required}"

        if ping_ok; then
            echo "redis already running on $host:$port -- reusing"
            # No pid file => down() won't try to kill someone else's server.
            exit 0
        fi

        if ! command -v redis-server >/dev/null 2>&1; then
            echo "redis-server not on PATH; cannot start fixture" >&2
            exit 1
        fi
        if [[ ! -f "$conf" ]]; then
            echo "redis conf missing at $conf" >&2
            exit 1
        fi

        # --daemonize no keeps the process in the foreground; we background
        # it via & and disown so it survives this script exiting.
        #
        # Autoload the RedisJSON module if we can find it: the system conf
        # at /etc/redis/redis.conf usually loads it, but our local conf
        # doesn't, and the JSON.* commands are required by the cache tests.
        # Common install paths covered: Debian/Ubuntu package, redis-stack.
        loadmodule_args=()
        for so in /usr/lib/redis/modules/rejson.so \
                  /opt/redis-stack/lib/rejson.so; do
            if [[ -f "$so" ]]; then
                loadmodule_args=(--loadmodule "$so")
                break
            fi
        done

        redis-server "$conf" --port "$port" --daemonize no \
            "${loadmodule_args[@]}" \
            >"$log_file" 2>&1 &
        pid=$!
        disown "$pid" 2>/dev/null || true
        echo "$pid" >"$pid_file"

        # Wait up to ~5s for the port to come live.
        for _ in $(seq 1 50); do
            if ping_ok; then
                echo "started redis pid=$pid on $host:$port"
                exit 0
            fi
            if ! kill -0 "$pid" 2>/dev/null; then
                echo "redis-server exited during startup; log:" >&2
                sed 's/^/  /' "$log_file" >&2
                rm -f "$pid_file"
                exit 1
            fi
            sleep 0.1
        done
        echo "redis-server did not become ready on $host:$port" >&2
        kill "$pid" 2>/dev/null || true
        rm -f "$pid_file"
        exit 1
        ;;

    down)
        if [[ ! -f "$pid_file" ]]; then
            # Nothing we started; reusing a pre-existing server.
            exit 0
        fi
        pid=$(cat "$pid_file" 2>/dev/null || true)
        rm -f "$pid_file"
        [[ -n "$pid" ]] || exit 0
        kill "$pid" 2>/dev/null || true
        # Give it a beat to exit cleanly, then SIGKILL if still alive.
        for _ in $(seq 1 20); do
            kill -0 "$pid" 2>/dev/null || exit 0
            sleep 0.1
        done
        kill -9 "$pid" 2>/dev/null || true
        exit 0
        ;;

    *)
        echo "unknown subcommand: $cmd" >&2
        exit 2
        ;;
esac
