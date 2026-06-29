# Tempo

`make tempo` brings up Tempo, the OpenTelemetry Collector, Grafana, and the `example_pod` workload without Docker.

```sh
bash scripts/tempo-demo.sh urls

Grafana:       http://localhost:3000  (admin/admin)
Tempo ready:   http://localhost:3200/ready
Tempo search:  http://localhost:3200/api/search?q={resource.service.name="example-otel-pod"}
Tempo OTLP gRPC endpoint for the collector: 127.0.0.1:4327
OTLP gRPC endpoint for the example binary: 127.0.0.1:4317
```

The demo downloads local binaries into `.local/tempo-demo`, builds `example_pod`, starts the collector on OTLP gRPC port `4317`, and sends traces to Tempo's OTLP gRPC receiver on `127.0.0.1:4327`.

Process logs are written to the repository root as `tempo.log`, `otel.log`, `grafana.log`, and `example-pod.log`; `*.log` is ignored by git.

Operational helpers:

```sh
bash scripts/tempo-demo.sh status
bash scripts/tempo-demo.sh query
bash scripts/tempo-demo.sh logs
bash scripts/tempo-demo.sh down
```

The shell query searches Tempo with TraceQL:

```sh
curl --max-time 15 -fsS -G "http://127.0.0.1:3200/api/search" \
	--data-urlencode 'q={resource.service.name="example-otel-pod"}' \
	--data-urlencode 'limit=20'
```

## Architechure 
Tempo is different from Jaeger: Jaeger is a **tracing-first** system that optimizes for search and exploration. Tempo is a **storage-first** system that optimizes for scale and cost, relying on other signals (metrics, logs) to drive you to a trace.

Tempo has it's own DB whereas Jaeger is usually backed by ElasticSearch. Jaeger has a rich UI, but Tempo just has TraceQL and depends on Grafana for UI.

┌─────────────────────────────────────────┐
                    │              Write Path                  │
                    │                                         │
  OTLP/Jaeger  ──► │  Distributor                            │
  Zipkin/OC        │      │ (consistent hash by trace_id)    │
                    │      ▼                                  │
                    │  Ingester                               │
                    │      │ WAL (local disk)                 │
                    │      │ buffers ~15min of traces         │
                    │      ▼                                  │
                    │  Object Store (S3/Azure Blob/GCS)       │
                    │      blocks + bloom filters             │
                    └─────────────────────────────────────────┘

                    ┌─────────────────────────────────────────┐
                    │              Read Path                   │
                    │                                         │
  TraceID query ──► │  Query Frontend                         │
                    │      │ (sharding, caching)              │
                    │      ▼                                  │
                    │  Querier                                │
                    │      ├── Ingester (recent traces)       │
                    │      └── Object Store (older traces)    │
                    └─────────────────────────────────────────┘

                    ┌─────────────────────────────────────────┐
                    │          Background Services             │
                    │                                         │
                    │  Compactor — merges blocks, retention   │
                    │  Metrics Generator — RED metrics        │
                    │  Cache (Memcached/Redis) — block index  │
                    └─────────────────────────────────────────┘

## TraceQL
```sh
# Find all traces with an error span
{ status = error }

# Find traces where any span hit a specific service with latency > 500ms
{ .service.name = "order-api" && duration > 500ms }

# Structural query — find traces where a DB call errored inside an HTTP handler
{ span.http.method = "POST" } >> { span.db.system = "postgresql" && status = error }

# Attribute filter on resource
{ resource.k8s.namespace.name = "production" && status = error }

# Count traces by service (aggregate)
{ } | rate()
count_over_time({ status = error }[5m])
```

Syntax:
```
{} — span selector
>> — ancestor/descendant (A contains B somewhere in subtree)
> — direct parent/child
~ — siblings
&&, ||, ! — boolean
```

## Traces always tied with Metrics 
Tempo generates metrics for all traces by default - and these all feed Grafana's default *Service Graph* panel. 

In this repo, the service graph works in the full LGTM demo because Tempo's metrics-generator remote-writes `traces_service_graph_*` and `traces_spanmetrics_*` metrics into Mimir, and Grafana's Tempo datasource points service maps at the `mimir` datasource.

```sh
make demo
```

Open Grafana Explore:

```text
http://localhost:3000/explore
```

Then select `Tempo` as the datasource and `Service Graph` as the query type.

If you run the demo with a custom `GRAFANA_PORT`, replace `3000` in the URL.

You can verify service graph metrics are present in Mimir with:

```sh
curl -fsS 'http://127.0.0.1:9009/prometheus/api/v1/label/__name__/values' |
    tr ',' '\n' |
    grep -E 'traces_service_graph|traces_spanmetrics'
```

## Otel Integration
Tempo accepts OpenTelemetry traces directly through OTLP. In this repo the application sends all signals to the OpenTelemetry Collector on `4317`; the collector then forwards only the trace pipeline to Tempo.

The Tempo receiver is enabled in the Tempo config:

```yaml
distributor:
  receivers:
    otlp:
      protocols:
        grpc:
          endpoint: 127.0.0.1:4327
        http:
          endpoint: 127.0.0.1:4328
```

The collector exports traces to that Tempo OTLP gRPC receiver:

```yaml
exporters:
  otlp/tempo:
    endpoint: 127.0.0.1:4327
    tls:
      insecure: true

service:
  pipelines:
    traces:
      receivers: [otlp]
      processors: [batch]
      exporters: [otlp/tempo, debug]
```

Logs and metrics can still flow through the same collector, but the standalone Tempo demo sends them to the collector `debug` exporter because Tempo stores traces only.

In the full LGTM demo, Tempo also turns traces into metrics for Grafana's Service Graph panel. Those generated metrics are remote-written to Mimir:

```yaml
metrics_generator:
  registry:
    collection_interval: 2s
    external_labels:
      source: tempo
      cluster: local-demo
  storage:
    path: .local/demo/data/tempo/generator/wal
    remote_write:
      - url: http://127.0.0.1:9009/api/v1/push
        send_exemplars: true
  traces_storage:
    path: .local/demo/data/tempo/generator/traces

overrides:
  defaults:
    metrics_generator:
      processors: [service-graphs, span-metrics]
```

Grafana then links the Tempo datasource to the Mimir datasource for service graph queries:

```json
{
  "uid": "tempo",
  "type": "tempo",
  "jsonData": {
    "serviceMap": {
      "datasourceUid": "mimir"
    }
  }
}
```
