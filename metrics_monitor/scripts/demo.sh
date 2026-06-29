#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

source "${SCRIPT_DIR}/loki-demo.sh"
source "${SCRIPT_DIR}/mimir-demo.sh"
source "${SCRIPT_DIR}/tempo-demo.sh"

WORK_DIR="${DEMO_HOME:-${ROOT_DIR}/.local/demo}"
TOOLS_DIR="${WORK_DIR}/tools"
DOWNLOAD_DIR="${WORK_DIR}/downloads"
BIN_DIR="${WORK_DIR}/bin"
CONFIG_DIR="${WORK_DIR}/config"
DATA_DIR="${WORK_DIR}/data"
LOG_DIR="${ROOT_DIR}"
PID_DIR="${WORK_DIR}/pids"

LOCAL_HOST="${LOCAL_HOST:-127.0.0.1}"
LOKI_PORT="${LOKI_PORT:-3100}"
LOKI_GRPC_PORT="${LOKI_GRPC_PORT:-9095}"
MIMIR_PORT="${MIMIR_PORT:-9009}"
MIMIR_GRPC_PORT="${MIMIR_GRPC_PORT:-9096}"
TEMPO_PORT="${TEMPO_PORT:-3200}"
TEMPO_GRPC_PORT="${TEMPO_GRPC_PORT:-9097}"
TEMPO_OTLP_GRPC_PORT="${TEMPO_OTLP_GRPC_PORT:-4327}"
TEMPO_OTLP_HTTP_PORT="${TEMPO_OTLP_HTTP_PORT:-4328}"
GRAFANA_PORT="${GRAFANA_PORT:-3000}"
OTLP_GRPC_PORT="${OTLP_GRPC_PORT:-4317}"
OTLP_HTTP_PORT="${OTLP_HTTP_PORT:-4318}"
OTELCOL_METRICS_PORT="${OTELCOL_METRICS_PORT:-8889}"

LOKI_VERSION="${LOKI_VERSION:-3.0.0}"
MIMIR_VERSION="${MIMIR_VERSION:-3.1.2}"
TEMPO_VERSION="${TEMPO_VERSION:-2.10.7}"
GRAFANA_VERSION="${GRAFANA_VERSION:-11.0.0}"
OTELCOL_VERSION="${OTELCOL_VERSION:-0.103.0}"

LOKI_CONFIG="${CONFIG_DIR}/loki.yaml"
MIMIR_CONFIG="${CONFIG_DIR}/mimir.yaml"
TEMPO_CONFIG="${CONFIG_DIR}/tempo.yaml"
OTEL_CONFIG="${CONFIG_DIR}/otelcol.yaml"
EXAMPLE_BIN="${BIN_DIR}/example-pod"
GRAFANA_DASHBOARD_DIR="${ROOT_DIR}/grafana"

usage() {
  cat <<'EOF'
Usage: scripts/demo.sh [command]

No command starts Loki, Mimir, Tempo, Grafana, the OpenTelemetry Collector, and the example binary.

Commands:
  up            Download tools, build the example binary, and start the full demo
  down          Stop demo processes started by this script
  status        Show whether demo processes are running and where they listen
  logs [name]   Tail logs for all processes, or one of: loki, mimir, tempo, otel/otelcol, grafana, example-pod
  query         Query Loki logs, Mimir metrics, and Tempo traces
  urls          Print local demo URLs
  clean         Stop the demo and remove generated .local state

Environment overrides:
  DEMO_HOME, LOKI_VERSION, MIMIR_VERSION, TEMPO_VERSION, GRAFANA_VERSION, OTELCOL_VERSION,
  LOKI_BIN, MIMIR_BIN, TEMPO_BIN, OTELCOL_BIN, GRAFANA_HOME,
  LOKI_PORT, LOKI_GRPC_PORT, MIMIR_PORT, MIMIR_GRPC_PORT,
  TEMPO_PORT, TEMPO_GRPC_PORT, TEMPO_OTLP_GRPC_PORT, TEMPO_OTLP_HTTP_PORT,
  GRAFANA_PORT, OTLP_GRPC_PORT, OTLP_HTTP_PORT, OTELCOL_METRICS_PORT
EOF
}

ensure_dirs() {
  mkdir -p "$TOOLS_DIR" "$DOWNLOAD_DIR" "$BIN_DIR" "$CONFIG_DIR" "$DATA_DIR" "$LOG_DIR" "$PID_DIR"
  mkdir -p "$DATA_DIR/grafana" "$DATA_DIR/grafana/plugins" "$DATA_DIR/grafana/provisioning"
  mkdir -p "$DATA_DIR/loki/chunks" "$DATA_DIR/loki/rules"
  mkdir -p "$DATA_DIR/mimir/blocks" "$DATA_DIR/mimir/tsdb" "$DATA_DIR/mimir/bucket-sync"
  mkdir -p "$DATA_DIR/mimir/compactor" "$DATA_DIR/mimir/ruler" "$DATA_DIR/mimir/ruler-storage"
  mkdir -p "$DATA_DIR/tempo/blocks" "$DATA_DIR/tempo/wal"
  mkdir -p "$DATA_DIR/tempo/generator/wal" "$DATA_DIR/tempo/generator/traces"
}

ensure_dependencies() {
  require_command curl
  require_command tar
  require_command go
  if ! command -v unzip >/dev/null 2>&1; then
    require_command python3
  fi
}

generate_configs() {
  cat >"$LOKI_CONFIG" <<EOF
auth_enabled: false

server:
  http_listen_address: ${LOCAL_HOST}
  http_listen_port: ${LOKI_PORT}
  grpc_listen_address: ${LOCAL_HOST}
  grpc_listen_port: ${LOKI_GRPC_PORT}

common:
  path_prefix: "${DATA_DIR}/loki"
  replication_factor: 1
  ring:
    kvstore:
      store: inmemory
  storage:
    filesystem:
      chunks_directory: "${DATA_DIR}/loki/chunks"
      rules_directory: "${DATA_DIR}/loki/rules"

schema_config:
  configs:
    - from: 2024-01-01
      store: tsdb
      object_store: filesystem
      schema: v13
      index:
        prefix: index_
        period: 24h

limits_config:
  allow_structured_metadata: true

analytics:
  reporting_enabled: false
EOF

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

  cat >"$TEMPO_CONFIG" <<EOF
stream_over_http_enabled: true

server:
  http_listen_address: ${LOCAL_HOST}
  http_listen_port: ${TEMPO_PORT}
  grpc_listen_address: ${LOCAL_HOST}
  grpc_listen_port: ${TEMPO_GRPC_PORT}
  log_level: warn

distributor:
  receivers:
    otlp:
      protocols:
        grpc:
          endpoint: ${LOCAL_HOST}:${TEMPO_OTLP_GRPC_PORT}
        http:
          endpoint: ${LOCAL_HOST}:${TEMPO_OTLP_HTTP_PORT}

ingester:
  max_block_duration: 5m

compactor:
  compaction:
    block_retention: 24h

metrics_generator:
  registry:
    collection_interval: 2s
    external_labels:
      source: tempo
      cluster: local-demo
  storage:
    path: "${DATA_DIR}/tempo/generator/wal"
    remote_write:
      - url: http://${LOCAL_HOST}:${MIMIR_PORT}/api/v1/push
        send_exemplars: true
  traces_storage:
    path: "${DATA_DIR}/tempo/generator/traces"

storage:
  trace:
    backend: local
    wal:
      path: "${DATA_DIR}/tempo/wal"
    local:
      path: "${DATA_DIR}/tempo/blocks"

usage_report:
  reporting_enabled: false

overrides:
  defaults:
    metrics_generator:
      processors: [service-graphs, span-metrics]
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
  otlphttp/loki:
    endpoint: http://${LOCAL_HOST}:${LOKI_PORT}/otlp
  otlphttp/mimir:
    endpoint: http://${LOCAL_HOST}:${MIMIR_PORT}/otlp
  otlp/tempo:
    endpoint: ${LOCAL_HOST}:${TEMPO_OTLP_GRPC_PORT}
    tls:
      insecure: true
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
      exporters: [otlphttp/loki, debug]
    metrics:
      receivers: [otlp]
      processors: [batch]
      exporters: [otlphttp/mimir, debug]
    traces:
      receivers: [otlp]
      processors: [batch]
      exporters: [otlp/tempo, debug]
EOF
}

upsert_grafana_datasource() {
  local base_url="$1"
  local auth="$2"
  local uid="$3"
  local payload="$4"
  if curl -fsS -u "$auth" "$base_url/api/datasources/uid/$uid" >/dev/null; then
    curl -fsS -u "$auth" -H 'Content-Type: application/json' -X PUT "$base_url/api/datasources/uid/$uid" --data "$payload" >/dev/null
  else
    curl -fsS -u "$auth" -H 'Content-Type: application/json' -X POST "$base_url/api/datasources" --data "$payload" >/dev/null
  fi
}

configure_grafana() {
  local base_url="http://${LOCAL_HOST}:${GRAFANA_PORT}"
  local auth="admin:admin"
  local loki_payload mimir_payload tempo_payload
  loki_payload='{"name":"Loki","uid":"loki","type":"loki","access":"proxy","url":"http://127.0.0.1:'"${LOKI_PORT}"'","isDefault":false,"jsonData":{"timeout":"60"}}'
  mimir_payload='{"name":"Mimir","uid":"mimir","type":"prometheus","access":"proxy","url":"http://127.0.0.1:'"${MIMIR_PORT}"'/prometheus","isDefault":true,"jsonData":{"httpMethod":"POST","timeInterval":"10s"}}'
  tempo_payload='{"name":"Tempo","uid":"tempo","type":"tempo","access":"proxy","url":"http://127.0.0.1:'"${TEMPO_PORT}"'","isDefault":false,"jsonData":{"httpMethod":"GET","nodeGraph":{"enabled":true},"serviceMap":{"datasourceUid":"mimir"}}}'

  upsert_grafana_datasource "$base_url" "$auth" loki "$loki_payload"
  upsert_grafana_datasource "$base_url" "$auth" mimir "$mimir_payload"
  upsert_grafana_datasource "$base_url" "$auth" tempo "$tempo_payload"
  load_grafana_dashboards "$base_url" "$auth"
}

cmd_up() {
  ensure_dependencies
  ensure_dirs
  local arch loki_bin mimir_bin tempo_bin otel_bin grafana_dir
  arch="$(detect_arch)"
  loki_bin="$(install_loki "$arch")"
  mimir_bin="$(install_mimir "$arch")"
  tempo_bin="$(install_tempo "$arch")"
  otel_bin="$(install_otelcol "$arch")"
  grafana_dir="$(install_grafana "$arch")"
  build_example
  generate_configs
  start_loki "$loki_bin"
  start_mimir "$mimir_bin"
  start_tempo "$tempo_bin"
  start_otelcol "$otel_bin"
  start_grafana "$grafana_dir"
  start_example
  cmd_urls
  printf '\nFull demo is running: logs go to Loki, metrics go to Mimir, traces go to Tempo.\n'
  printf 'Query all stores: bash scripts/demo.sh query\n'
  printf 'Stop the demo:   bash scripts/demo.sh down\n'
}

cmd_down() {
  ensure_dirs
  stop_process example-pod
  stop_process grafana
  stop_process otelcol
  stop_process tempo
  stop_process mimir
  stop_process loki
}

status_endpoint_for() {
  case "$1" in
    loki)
      printf 'http://%s:%s' "$LOCAL_HOST" "$LOKI_PORT"
      ;;
    mimir)
      printf 'http://%s:%s' "$LOCAL_HOST" "$MIMIR_PORT"
      ;;
    tempo)
      printf 'http://%s:%s' "$LOCAL_HOST" "$TEMPO_PORT"
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

cmd_status() {
  ensure_dirs
  for name in loki mimir tempo otelcol grafana example-pod; do
    local endpoint
    endpoint="$(status_endpoint_for "$name")"
    if is_running "$name"; then
      printf '%-12s running pid %-8s %s\n' "$name" "$(cat "$(pid_file_for "$name")")" "$endpoint"
    else
      printf '%-12s stopped              %s\n' "$name" "$endpoint"
    fi
  done
}

cmd_logs() {
  ensure_dirs
  if [[ $# -gt 0 ]]; then
    show_tail "$1"
    return 0
  fi
  for name in loki mimir tempo otelcol grafana example-pod; do
    show_tail "$name"
  done
}

cmd_query() {
  require_command curl
  printf 'Loki logs:\n'
  curl --max-time 15 -fsS -G "http://${LOCAL_HOST}:${LOKI_PORT}/loki/api/v1/query" --data-urlencode 'query={service_name="example-otel-pod"}'
  printf '\n\nMimir metrics:\n'
  curl --max-time 15 -fsS -G "http://${LOCAL_HOST}:${MIMIR_PORT}/prometheus/api/v1/query" --data-urlencode 'query=sum(demo_requests_total)'
  printf '\n\nTempo traces:\n'
  curl --max-time 15 -fsS -G "http://${LOCAL_HOST}:${TEMPO_PORT}/api/search" \
    --data-urlencode 'q={resource.service.name="example-otel-pod"}' \
    --data-urlencode 'limit=20'
  printf '\n'
}

cmd_urls() {
  printf '\nGrafana:       http://localhost:%s  (admin/admin)\n' "$GRAFANA_PORT"
  printf 'Dashboard:     http://localhost:%s/d/example-process/example-process-overview\n' "$GRAFANA_PORT"
  printf 'Loki ready:    http://localhost:%s/ready\n' "$LOKI_PORT"
  printf 'Loki labels:   http://localhost:%s/loki/api/v1/labels\n' "$LOKI_PORT"
  printf 'Mimir ready:   http://localhost:%s/ready\n' "$MIMIR_PORT"
  printf 'Mimir labels:  http://localhost:%s/prometheus/api/v1/labels\n' "$MIMIR_PORT"
  printf 'Mimir query:   http://localhost:%s/prometheus/api/v1/query?query=sum(demo_requests_total)\n' "$MIMIR_PORT"
  printf 'Tempo ready:   http://localhost:%s/ready\n' "$TEMPO_PORT"
  printf 'Tempo search:  http://localhost:%s/api/search?q={resource.service.name="example-otel-pod"}\n' "$TEMPO_PORT"
  printf 'Service graph: http://localhost:%s/explore  (select Tempo -> Service Graph)\n' "$GRAFANA_PORT"
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
    urls) cmd_urls "$@" ;;
    clean) cmd_clean "$@" ;;
    -h|--help|help) usage ;;
    *) usage >&2; exit 1 ;;
  esac
}

main "$@"
