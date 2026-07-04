#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK_DIR="${MIMIR_DEMO_HOME:-${ROOT_DIR}/.local/mimir-demo}"
TOOLS_DIR="${WORK_DIR}/tools"
DOWNLOAD_DIR="${WORK_DIR}/downloads"
BIN_DIR="${WORK_DIR}/bin"
CONFIG_DIR="${WORK_DIR}/config"
DATA_DIR="${WORK_DIR}/data"
LOG_DIR="${ROOT_DIR}"
PID_DIR="${WORK_DIR}/pids"

LOCAL_HOST="${LOCAL_HOST:-127.0.0.1}"
MIMIR_PORT="${MIMIR_PORT:-9009}"
MIMIR_GRPC_PORT="${MIMIR_GRPC_PORT:-9096}"
GRAFANA_PORT="${GRAFANA_PORT:-3000}"
OTLP_GRPC_PORT="${OTLP_GRPC_PORT:-4317}"
OTLP_HTTP_PORT="${OTLP_HTTP_PORT:-4318}"
OTELCOL_METRICS_PORT="${OTELCOL_METRICS_PORT:-8889}"

MIMIR_VERSION="${MIMIR_VERSION:-3.1.2}"
GRAFANA_VERSION="${GRAFANA_VERSION:-11.0.0}"
OTELCOL_VERSION="${OTELCOL_VERSION:-0.103.0}"

MIMIR_CONFIG="${CONFIG_DIR}/mimir.yaml"
OTEL_CONFIG="${CONFIG_DIR}/otelcol.yaml"
EXAMPLE_BIN="${BIN_DIR}/example-pod"

die() {
	printf 'error: %s\n' "$*" >&2
	exit 1
}

usage() {
	cat <<'EOF'
Usage: scripts/mimir-demo.sh [command]

No command starts Mimir, Grafana, the OpenTelemetry Collector, and the example binary.

Commands:
  up            Download tools, build the example binary, and start the demo
  down          Stop demo processes started by this script
  status        Show whether demo processes are running and where they listen
	logs [name]   Tail logs for all processes, or one of: mimir, otel/otelcol, grafana, example-pod
  query         Query Mimir for the example request counter
  labels        List labels known to Mimir through the Prometheus API
  urls          Print local demo URLs
  clean         Stop the demo and remove generated .local state

Environment overrides:
  MIMIR_DEMO_HOME, MIMIR_VERSION, GRAFANA_VERSION, OTELCOL_VERSION,
  MIMIR_BIN, OTELCOL_BIN, GRAFANA_HOME,
	MIMIR_PORT, MIMIR_GRPC_PORT, GRAFANA_PORT,
	OTLP_GRPC_PORT, OTLP_HTTP_PORT, OTELCOL_METRICS_PORT
EOF
}

ensure_dirs() {
	mkdir -p "$TOOLS_DIR" "$DOWNLOAD_DIR" "$BIN_DIR" "$CONFIG_DIR" "$DATA_DIR" "$LOG_DIR" "$PID_DIR"
	mkdir -p "$DATA_DIR/grafana" "$DATA_DIR/grafana/plugins" "$DATA_DIR/grafana/provisioning"
	mkdir -p "$DATA_DIR/mimir/blocks" "$DATA_DIR/mimir/tsdb" "$DATA_DIR/mimir/bucket-sync"
	mkdir -p "$DATA_DIR/mimir/compactor" "$DATA_DIR/mimir/ruler" "$DATA_DIR/mimir/ruler-storage"
}

require_command() {
	command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

detect_arch() {
	case "$(uname -m)" in
		x86_64|amd64)
			printf 'amd64'
			;;
		aarch64|arm64)
			printf 'arm64'
			;;
		*)
			die "unsupported CPU architecture: $(uname -m)"
			;;
	esac
}

ensure_dependencies() {
	require_command curl
	require_command tar
	require_command go
}

download() {
	local url="$1"
	local dest="$2"
	if [[ -f "$dest" ]]; then
		return 0
	fi
	mkdir -p "$(dirname "$dest")"
	printf 'downloading %s\n' "$url" >&2
	if [[ -f "${dest}.tmp" ]]; then
		curl -fL -C - --retry 3 --connect-timeout 20 -o "${dest}.tmp" "$url"
	else
		curl -fL --retry 3 --connect-timeout 20 -o "${dest}.tmp" "$url"
	fi
	mv "${dest}.tmp" "$dest"
}

install_mimir() {
	local arch="$1"
	if [[ -n "${MIMIR_BIN:-}" ]]; then
		[[ -x "$MIMIR_BIN" ]] || die "MIMIR_BIN is not executable: $MIMIR_BIN"
		printf '%s\n' "$MIMIR_BIN"
		return 0
	fi

	local mimir_dir="${TOOLS_DIR}/mimir"
	local mimir_bin="${mimir_dir}/mimir"
	if [[ -x "$mimir_bin" ]]; then
		printf '%s\n' "$mimir_bin"
		return 0
	fi

	rm -rf "$mimir_dir"
	mkdir -p "$mimir_dir"
	local downloaded="${DOWNLOAD_DIR}/mimir-linux-${arch}"
	download "https://github.com/grafana/mimir/releases/download/mimir-${MIMIR_VERSION}/mimir-linux-${arch}" "$downloaded"
	cp "$downloaded" "$mimir_bin"
	chmod +x "$mimir_bin"
	printf '%s\n' "$mimir_bin"
}

install_otelcol() {
	local arch="$1"
	if [[ -n "${OTELCOL_BIN:-}" ]]; then
		[[ -x "$OTELCOL_BIN" ]] || die "OTELCOL_BIN is not executable: $OTELCOL_BIN"
		printf '%s\n' "$OTELCOL_BIN"
		return 0
	fi

	local otel_dir="${TOOLS_DIR}/otelcol"
	local otel_bin="${otel_dir}/otelcol"
	if [[ -x "$otel_bin" ]]; then
		printf '%s\n' "$otel_bin"
		return 0
	fi

	rm -rf "$otel_dir"
	mkdir -p "$otel_dir"
	local archive="${DOWNLOAD_DIR}/otelcol_${OTELCOL_VERSION}_linux_${arch}.tar.gz"
	download "https://github.com/open-telemetry/opentelemetry-collector-releases/releases/download/v${OTELCOL_VERSION}/otelcol_${OTELCOL_VERSION}_linux_${arch}.tar.gz" "$archive"
	tar -xzf "$archive" -C "$otel_dir"
	[[ -x "$otel_bin" ]] || die "otelcol archive did not contain ${otel_bin}"
	printf '%s\n' "$otel_bin"
}

install_grafana() {
	local arch="$1"
	if [[ -n "${GRAFANA_HOME:-}" ]]; then
		[[ -x "${GRAFANA_HOME}/bin/grafana-server" || -x "${GRAFANA_HOME}/bin/grafana" ]] || die "GRAFANA_HOME does not contain bin/grafana-server or bin/grafana: $GRAFANA_HOME"
		printf '%s\n' "$GRAFANA_HOME"
		return 0
	fi

	local grafana_dir="${TOOLS_DIR}/grafana"
	local grafana_bin="${grafana_dir}/bin/grafana-server"
	local grafana_cli="${grafana_dir}/bin/grafana"
	if [[ -x "$grafana_bin" || -x "$grafana_cli" ]]; then
		printf '%s\n' "$grafana_dir"
		return 0
	fi

	rm -rf "$grafana_dir"
	mkdir -p "$grafana_dir"
	local archive="${DOWNLOAD_DIR}/grafana-${GRAFANA_VERSION}.linux-${arch}.tar.gz"
	download "https://dl.grafana.com/oss/release/grafana-${GRAFANA_VERSION}.linux-${arch}.tar.gz" "$archive"
	tar -xzf "$archive" -C "$grafana_dir" --strip-components=1
	[[ -x "$grafana_bin" || -x "$grafana_cli" ]] || die "Grafana archive did not contain a runnable Grafana binary"
	printf '%s\n' "$grafana_dir"
}

build_example() {
	printf 'building example_pod binary\n' >&2
	(cd "$ROOT_DIR/example_pod" && go build -o "$EXAMPLE_BIN" .)
}

generate_configs() {
	cat >"$MIMIR_CONFIG" <<EOF
target: all
multitenancy_enabled: false

server:
  http_listen_address: ${LOCAL_HOST}
  http_listen_port: ${MIMIR_PORT}
  grpc_listen_address: ${LOCAL_HOST}
  grpc_listen_port: ${MIMIR_GRPC_PORT}
  log_level: warn

blocks_storage:
  backend: filesystem
  filesystem:
    dir: "${DATA_DIR}/mimir/blocks"
  bucket_store:
    sync_dir: "${DATA_DIR}/mimir/bucket-sync"
  tsdb:
    dir: "${DATA_DIR}/mimir/tsdb"
    flush_blocks_on_shutdown: true

compactor:
  data_dir: "${DATA_DIR}/mimir/compactor"
  sharding_ring:
    kvstore:
      store: inmemory

distributor:
  ring:
    kvstore:
      store: inmemory

ingester:
  ring:
    replication_factor: 1
		instance_addr: ${LOCAL_HOST}
		instance_port: ${MIMIR_GRPC_PORT}
    kvstore:
      store: inmemory

querier:
  ring:
    kvstore:
      store: inmemory

frontend:
	address: ${LOCAL_HOST}
	port: ${MIMIR_GRPC_PORT}

store_gateway:
  sharding_ring:
    replication_factor: 1
    kvstore:
      store: inmemory

ruler:
  rule_path: "${DATA_DIR}/mimir/ruler"
  ring:
    kvstore:
      store: inmemory

ruler_storage:
  backend: filesystem
  filesystem:
    dir: "${DATA_DIR}/mimir/ruler-storage"

limits:
  ingestion_rate: 100000
  ingestion_burst_size: 200000
  max_global_series_per_user: 0
  promote_otel_resource_attributes: service.name,service.version,deployment.environment,k8s.namespace.name,k8s.pod.name,k8s.node.name

usage_stats:
  enabled: false
EOF

	cat >"$OTEL_CONFIG" <<EOF
receivers:
  otlp:
    protocols:
      grpc:
        endpoint: ${LOCAL_HOST}:${OTLP_GRPC_PORT}
      http:
        endpoint: ${LOCAL_HOST}:${OTLP_HTTP_PORT}

processors:
  batch:
    timeout: 5s

exporters:
  otlphttp/mimir:
    endpoint: http://${LOCAL_HOST}:${MIMIR_PORT}/otlp
  debug:
    verbosity: basic

service:
  telemetry:
    metrics:
      address: ${LOCAL_HOST}:${OTELCOL_METRICS_PORT}
  pipelines:
    logs:
      receivers: [otlp]
      processors: [batch]
      exporters: [debug]
    metrics:
      receivers: [otlp]
      processors: [batch]
      exporters: [otlphttp/mimir, debug]
    traces:
      receivers: [otlp]
      processors: [batch]
      exporters: [debug]
EOF
}

pid_file_for() {
	printf '%s/%s.pid' "$PID_DIR" "$1"
}

log_name_for() {
	case "$1" in
		otelcol)
			printf 'otel'
			;;
		*)
			printf '%s' "$1"
			;;
	esac
}

log_file_for() {
	printf '%s/%s.log' "$LOG_DIR" "$(log_name_for "$1")"
}

is_running() {
	local name="$1"
	local pid_file
	pid_file="$(pid_file_for "$name")"
	[[ -f "$pid_file" ]] || return 1
	local pid
	pid="$(cat "$pid_file" 2>/dev/null || true)"
	[[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null
}

start_process() {
	local name="$1"
	shift
	local pid_file log_file
	pid_file="$(pid_file_for "$name")"
	log_file="$(log_file_for "$name")"

	if is_running "$name"; then
		printf '%s already running with pid %s\n' "$name" "$(cat "$pid_file")"
		return 0
	fi
	rm -f "$pid_file"
	printf 'starting %s\n' "$name"
	"$@" >"$log_file" 2>&1 &
	printf '%s\n' "$!" >"$pid_file"
}

stop_process() {
	local name="$1"
	local pid_file pid
	pid_file="$(pid_file_for "$name")"
	if [[ ! -f "$pid_file" ]]; then
		printf '%s is not running\n' "$name"
		return 0
	fi
	pid="$(cat "$pid_file" 2>/dev/null || true)"
	if [[ -z "$pid" ]] || ! kill -0 "$pid" 2>/dev/null; then
		rm -f "$pid_file"
		printf '%s is not running\n' "$name"
		return 0
	fi

	printf 'stopping %s pid %s\n' "$name" "$pid"
	kill "$pid" 2>/dev/null || true
	for attempt in {1..10}; do
		if ! kill -0 "$pid" 2>/dev/null; then
			rm -f "$pid_file"
			return 0
		fi
		sleep 1
	done
	kill -KILL "$pid" 2>/dev/null || true
	rm -f "$pid_file"
}

show_tail() {
	local name="$1"
	local log_file
	log_file="$(log_file_for "$name")"
	if [[ -f "$log_file" ]]; then
		printf '\n==> %s <==\n' "$log_file"
		tail -n 80 "$log_file"
	else
		printf 'no log file for %s\n' "$name"
	fi
}

wait_for_url() {
	local name="$1"
	local url="$2"
	local attempts="$3"
	for ((attempt = 1; attempt <= attempts; attempt++)); do
		if curl -fsS -o /dev/null "$url"; then
			return 0
		fi
		if ! is_running "$name"; then
			printf '%s exited before becoming ready\n' "$name" >&2
			show_tail "$name" >&2
			return 1
		fi
		sleep 1
	done
	printf 'timed out waiting for %s at %s\n' "$name" "$url" >&2
	show_tail "$name" >&2
	return 1
}

wait_for_tcp() {
	local name="$1"
	local host="$2"
	local port="$3"
	local attempts="$4"
	for ((attempt = 1; attempt <= attempts; attempt++)); do
		if (echo >"/dev/tcp/${host}/${port}") >/dev/null 2>&1; then
			return 0
		fi
		if ! is_running "$name"; then
			printf '%s exited before opening %s:%s\n' "$name" "$host" "$port" >&2
			show_tail "$name" >&2
			return 1
		fi
		sleep 1
	done
	printf 'timed out waiting for %s on %s:%s\n' "$name" "$host" "$port" >&2
	show_tail "$name" >&2
	return 1
}

configure_grafana() {
	local base_url="http://${LOCAL_HOST}:${GRAFANA_PORT}"
	local auth="admin:admin"
	local datasource_payload
	datasource_payload='{"name":"Mimir","uid":"mimir","type":"prometheus","access":"proxy","url":"http://127.0.0.1:'"${MIMIR_PORT}"'/prometheus","isDefault":true,"jsonData":{"httpMethod":"POST","timeInterval":"10s"}}'

	if curl -fsS -u "$auth" "$base_url/api/datasources/uid/mimir" >/dev/null; then
		curl -fsS -u "$auth" -H 'Content-Type: application/json' -X PUT "$base_url/api/datasources/uid/mimir" --data "$datasource_payload" >/dev/null
	else
		curl -fsS -u "$auth" -H 'Content-Type: application/json' -X POST "$base_url/api/datasources" --data "$datasource_payload" >/dev/null
	fi
}

start_mimir() {
	local mimir_bin="$1"
	start_process mimir "$mimir_bin" "-config.file=$MIMIR_CONFIG"
	wait_for_url mimir "http://${LOCAL_HOST}:${MIMIR_PORT}/ready" 60
}

start_otelcol() {
	local otel_bin="$1"
	start_process otelcol "$otel_bin" "--config=$OTEL_CONFIG"
	wait_for_tcp otelcol "$LOCAL_HOST" "$OTLP_GRPC_PORT" 30
}

start_grafana() {
	local grafana_dir="$1"
	if [[ -x "${grafana_dir}/bin/grafana-server" ]]; then
		start_process grafana env \
			GF_SERVER_HTTP_ADDR="$LOCAL_HOST" \
			GF_SERVER_HTTP_PORT="$GRAFANA_PORT" \
			GF_SERVER_ROOT_URL="http://localhost:${GRAFANA_PORT}/" \
			GF_SECURITY_ADMIN_USER=admin \
			GF_SECURITY_ADMIN_PASSWORD=admin \
			GF_AUTH_ANONYMOUS_ENABLED=true \
			GF_AUTH_ANONYMOUS_ORG_ROLE=Viewer \
			GF_PATHS_DATA="${DATA_DIR}/grafana" \
			GF_PATHS_LOGS="${LOG_DIR}" \
			GF_PATHS_PLUGINS="${DATA_DIR}/grafana/plugins" \
			GF_PATHS_PROVISIONING="${DATA_DIR}/grafana/provisioning" \
			GF_ANALYTICS_REPORTING_ENABLED=false \
			GF_ANALYTICS_CHECK_FOR_UPDATES=false \
			"${grafana_dir}/bin/grafana-server" "--homepath=${grafana_dir}"
	else
		start_process grafana env \
			GF_SERVER_HTTP_ADDR="$LOCAL_HOST" \
			GF_SERVER_HTTP_PORT="$GRAFANA_PORT" \
			GF_SERVER_ROOT_URL="http://localhost:${GRAFANA_PORT}/" \
			GF_SECURITY_ADMIN_USER=admin \
			GF_SECURITY_ADMIN_PASSWORD=admin \
			GF_AUTH_ANONYMOUS_ENABLED=true \
			GF_AUTH_ANONYMOUS_ORG_ROLE=Viewer \
			GF_PATHS_DATA="${DATA_DIR}/grafana" \
			GF_PATHS_LOGS="${LOG_DIR}" \
			GF_PATHS_PLUGINS="${DATA_DIR}/grafana/plugins" \
			GF_PATHS_PROVISIONING="${DATA_DIR}/grafana/provisioning" \
			GF_ANALYTICS_REPORTING_ENABLED=false \
			GF_ANALYTICS_CHECK_FOR_UPDATES=false \
			"${grafana_dir}/bin/grafana" server "--homepath=${grafana_dir}"
	fi
	wait_for_url grafana "http://${LOCAL_HOST}:${GRAFANA_PORT}/api/health" 60
	configure_grafana
}

start_example() {
	start_process example-pod env \
		OTEL_SERVICE_NAME=example-otel-pod \
		OTEL_EXPORTER_OTLP_ENDPOINT="${LOCAL_HOST}:${OTLP_GRPC_PORT}" \
		OTEL_EXPORTER_OTLP_INSECURE=true \
		DEPLOYMENT_ENVIRONMENT=local \
		EMIT_INTERVAL=2s \
		METRIC_EXPORT_INTERVAL=10s \
		POD_NAME=example-otel-local \
		POD_NAMESPACE=local \
		NODE_NAME=wsl \
		"$EXAMPLE_BIN"
}

cmd_up() {
	ensure_dependencies
	ensure_dirs
	local arch mimir_bin otel_bin grafana_dir
	arch="$(detect_arch)"
	mimir_bin="$(install_mimir "$arch")"
	otel_bin="$(install_otelcol "$arch")"
	grafana_dir="$(install_grafana "$arch")"
	build_example
	generate_configs
	start_mimir "$mimir_bin"
	start_otelcol "$otel_bin"
	start_grafana "$grafana_dir"
	start_example
	cmd_urls
	printf '\nMimir stores Prometheus-compatible metrics; use Grafana Explore with the Mimir datasource.\n'
	printf 'Query from the shell: bash scripts/mimir-demo.sh query\n'
	printf 'Stop the demo:        bash scripts/mimir-demo.sh down\n'
}

cmd_down() {
	ensure_dirs
	stop_process example-pod
	stop_process grafana
	stop_process otelcol
	stop_process mimir
}

cmd_status() {
	ensure_dirs
	for name in mimir otelcol grafana example-pod; do
		local endpoint
		endpoint="$(status_endpoint_for "$name")"
		if is_running "$name"; then
			printf '%-12s running pid %-8s %s\n' "$name" "$(cat "$(pid_file_for "$name")")" "$endpoint"
		else
			printf '%-12s stopped              %s\n' "$name" "$endpoint"
		fi
	done
}

status_endpoint_for() {
	case "$1" in
		mimir)
			printf 'http://%s:%s' "$LOCAL_HOST" "$MIMIR_PORT"
			;;
		otelcol)
			printf 'otlp grpc %s:%s, http %s:%s' "$LOCAL_HOST" "$OTLP_GRPC_PORT" "$LOCAL_HOST" "$OTLP_HTTP_PORT"
			;;
		grafana)
			printf 'http://localhost:%s' "$GRAFANA_PORT"
			;;
		example-pod)
			printf 'exports otlp grpc to %s:%s' "$LOCAL_HOST" "$OTLP_GRPC_PORT"
			;;
	esac
}

cmd_logs() {
	ensure_dirs
	if [[ $# -gt 0 ]]; then
		show_tail "$1"
		return 0
	fi
	for name in mimir otelcol grafana example-pod; do
		show_tail "$name"
	done
}

cmd_query() {
	require_command curl
	local query='sum(demo_requests_total)'
	curl --max-time 15 -fsS -G "http://${LOCAL_HOST}:${MIMIR_PORT}/prometheus/api/v1/query" --data-urlencode "query=${query}"
	printf '\n'
}

cmd_labels() {
	require_command curl
	curl --max-time 15 -fsS "http://${LOCAL_HOST}:${MIMIR_PORT}/prometheus/api/v1/labels"
	printf '\n'
}

cmd_urls() {
	printf '\nGrafana:       http://localhost:%s  (admin/admin)\n' "$GRAFANA_PORT"
	printf 'Mimir ready:   http://localhost:%s/ready\n' "$MIMIR_PORT"
	printf 'Mimir labels:  http://localhost:%s/prometheus/api/v1/labels\n' "$MIMIR_PORT"
	printf 'Mimir query:   http://localhost:%s/prometheus/api/v1/query?query=sum(demo_requests_total)\n' "$MIMIR_PORT"
	printf 'OTLP gRPC endpoint for the example binary: %s:%s\n' "$LOCAL_HOST" "$OTLP_GRPC_PORT"
}

cmd_clean() {
	cmd_down
	rm -rf "$WORK_DIR"
	printf 'removed %s\n' "$WORK_DIR"
}

main() {
	local command="${1:-up}"
	if [[ $# -gt 0 ]]; then
		shift
	fi
	case "$command" in
		up) cmd_up "$@" ;;
		down) cmd_down "$@" ;;
		status) cmd_status "$@" ;;
		logs) cmd_logs "$@" ;;
		query) cmd_query "$@" ;;
		labels) cmd_labels "$@" ;;
		urls) cmd_urls "$@" ;;
		clean) cmd_clean "$@" ;;
		-h|--help|help) usage ;;
		*) usage >&2; exit 1 ;;
	esac
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
	main "$@"
fi