# Loki

'make loki' will bring up loki, otel collector and grafana.

```sh
bash scripts/loki-demo.sh urls

Grafana:     http://localhost:3000  (admin/admin)
Dashboard:   http://localhost:3000/d/example-process/example-process-overview
Loki ready:  http://localhost:3100/ready
Loki labels: http://localhost:3100/loki/api/v1/labels
OTLP gRPC endpoint for the example binary: 127.0.0.1:4317

```

You can query Loki for logs using it's query language.
```sh
curl --max-time 15 -fsS -G "http://127.0.0.1:3100/loki/api/v1/query" \
  --data-urlencode 'query={service_name="example-otel-pod"}'
```

The Kubernetes manifests live at `yamls/loki_yaml` and `yamls/otel.yaml`:

```sh
kubectl apply -f yamls/loki_yaml
kubectl apply -f yamls/otel.yaml
kubectl -n loki-example rollout status deploy/loki
kubectl -n otel-demo rollout status deploy/otel-collector
kubectl -n otel-demo rollout status deploy/example-otel-pod
```

## Architechure 
Loki is a horizontally scalable, highly available log aggregation system inspired by Prometheus. Core design principle: *index labels, not log content.*

┌──────────────┐     ┌───────────────┐     ┌──────────────┐
│  Log Sources │────▶│  Promtail /   │────▶│  Distributor │
│  (pods, VMs) │     │  OTEL / Fluentd│     │  (ingest)   │
└──────────────┘     └───────────────┘     └──────┬───────┘
                                                   │
                                          ┌────────▼────────┐
                                          │    Ingester      │
                                          │  (WAL + chunks)  │
                                          └────────┬────────┘
                                                   │
                                    ┌──────────────┼──────────────┐
                                    ▼              ▼              ▼
                             ┌────────────┐ ┌──────────┐ ┌────────────┐
                             │  Object    │ │  Index   │ │  Cache     │
                             │  Store (S3)│ │  (TSDB)  │ │  (memcached│
                             └────────────┘ └──────────┘ └────────────┘
                                                   │
                                          ┌────────▼────────┐
                                          │    Querier       │
                                          │  (query engine) │
                                          └────────┬────────┘
                                                   │
                                          ┌────────▼────────┐
                                          │  Query Frontend  │
                                          │ (sharding, cache)│
                                          └─────────────────┘


The **Distributor** is a load balancer that fans out (using consistent hashing) to **Ingestors** which store logs. 

Logs labels are KV pairs stored in a time series DB, which log body stored in simple object storage.

## LogQL
Two basic types - 

1. Log Queries that'll return logs
```sh
{namespace="istio", pod=~"istiod-.*"}
  |= "ERROR"
  | json
  | line_format "{{.level}} {{.msg}}"
```

2. Metric queries similar to promQL
```sh
rate({namespace="istio"} |= "timeout" [5m])

sum by (pod) (
  count_over_time({namespace="istio", level="error"}[1m])
)
```

Inbuilt stages
```sh
|= / != / |~ — line filter (grep)

| json / | logfmt / | pattern — log parser

| label_filter, | line_format, | label_format — transforms

| unwrap — extract numeric field for metric queries
```


## OTEL Integration 
Loki exporter will convert otel log streams to loki streams

```yaml
exporters:
  loki:
    endpoint: http://loki-distributor:3100/loki/api/v1/push
    default_labels_enabled:
      exporter: false
      job: true
    labels:
      resource:
        - k8s.namespace.name
        - k8s.pod.name
        - service.name
```