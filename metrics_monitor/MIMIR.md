# Mimir

`make mimir` brings up Mimir, the OpenTelemetry Collector, Grafana, and the `example_pod` workload without Docker.

```sh
bash scripts/mimir-demo.sh urls

Grafana:       http://localhost:3000  (admin/admin)
Mimir ready:   http://localhost:9009/ready
Mimir labels:  http://localhost:9009/prometheus/api/v1/labels
Mimir query:   http://localhost:9009/prometheus/api/v1/query?query=sum(demo_requests_total)
OTLP gRPC endpoint for the example binary: 127.0.0.1:4317
```

The demo downloads local binaries into `.local/mimir-demo`, builds `example_pod`, starts the collector on OTLP gRPC port `4317`, and sends metrics to Mimir's native OTLP HTTP endpoint at `http://127.0.0.1:9009/otlp/v1/metrics`.

Process logs are written to the repository root as `mimir.log`, `otel.log`, `grafana.log`, and `example-pod.log`; `*.log` is ignored by git.

Operational helpers:

```sh
bash scripts/mimir-demo.sh status
bash scripts/mimir-demo.sh query
bash scripts/mimir-demo.sh labels
bash scripts/mimir-demo.sh logs
bash scripts/mimir-demo.sh down
```

The shell query uses PromQL through Mimir's Prometheus-compatible API:

```sh
curl --max-time 15 -fsS -G "http://127.0.0.1:9009/prometheus/api/v1/query" \
  --data-urlencode 'query=sum(demo_requests_total)'
```

OpenTelemetry metric names are normalized into Prometheus names, so the example counter `demo.requests.total` is queried as `demo_requests_total`.

The Kubernetes manifests live at `yamls/mimir_yaml` and `yamls/otel.yaml`:

```sh
kubectl apply -f yamls/mimir_yaml
kubectl apply -f yamls/otel.yaml
kubectl -n mimir-example rollout status deploy/mimir
kubectl -n otel-demo rollout status deploy/otel-collector
kubectl -n otel-demo rollout status deploy/example-otel-pod
```

## Architechure
Mimir is basically - Prometheus, but sharded across many machines with object store backing. It implements Prometheus Remote Write and PromQL query APIs.

The problems with prometheus mimir tries to solve -
**Vertical scaling** - in prometheus all active series are in RAM, with the TSDB in pod local storage. 
**Retention** - since TSDB is on disk, retention is limited by disk storage.
**No multi-tenancy** 
**No native HA** - two prometheus instances on same target leads to duplication.


Remote Write
┌──────────────────┐         │
│  Prometheus      │─────────┤
│  (scraper/agent) │         │
└──────────────────┘         ▼
                      ┌─────────────┐
                      │ Distributor  │  ← validate, rate-limit, fan-out
                      └──────┬──────┘
                             │  consistent hash ring
                    ┌────────┴────────┐
                    ▼                 ▼
             ┌───────────┐    ┌───────────┐
             │ Ingester  │    │ Ingester  │  ← in-memory TSDB + WAL
             └─────┬─────┘    └─────┬─────┘
                   │                │
                   ▼                ▼
            ┌─────────────────────────────┐
            │     Object Store (S3/Blob)  │  ← TSDB blocks (Parquet-like)
            └─────────────────────────────┘
                         ▲
          ┌──────────────┼──────────────┐
          ▼              ▼              ▼
    ┌──────────┐  ┌──────────┐  ┌──────────────┐
    │ Querier  │  │  Store   │  │    Ruler      │
    │          │  │ Gateway  │  │  (alerts/     │
    └────┬─────┘  └────┬─────┘  │   recording) │
         │              │        └──────────────┘
         └──────┬───────┘
                ▼
       ┌─────────────────┐
       │  Query Frontend │  ← sharding, caching, query scheduling
       └────────┬────────┘
                ▼
           Grafana / API



## Otel integration
Mimir accepts OpenTelemetry metrics through its native OTLP HTTP endpoint. The OpenTelemetry Collector receives OTLP from applications, batches the data, and exports the metrics to Mimir.

```yaml
exporters:
  otlphttp/mimir:
    endpoint: http://mimir:9009/otlp

service:
  pipelines:
    metrics:
      receivers: [otlp]
      processors: [batch]
      exporters: [otlphttp/mimir, debug]
```

For the local demo, the same exporter points at `http://127.0.0.1:9009/otlp`. In Kubernetes, `yamls/otel.yaml` points at the Mimir service across namespaces:

```yaml
exporters:
  otlphttp/mimir:
    endpoint: http://mimir.mimir-example.svc.cluster.local:9009/otlp
```

Mimir translates OpenTelemetry metric names and labels into Prometheus-compatible names. Dots and other unsupported characters become underscores, so:

```text
demo.requests.total -> demo_requests_total
demo.request.duration -> demo_request_duration
```

Resource attributes such as `service.name` are not ordinary metric labels by default. This demo promotes the useful resource attributes at Mimir ingestion time:

```yaml
limits:
  promote_otel_resource_attributes: service.name,service.version,deployment.environment,k8s.namespace.name,k8s.pod.name,k8s.node.name
```

That makes labels such as `service_name`, `deployment_environment`, and `k8s_pod_name` available in PromQL while Mimir stores the original resource metadata in `target_info` as well.

You can query the generated request counter like this:

```sh
curl --max-time 15 -fsS -G "http://127.0.0.1:9009/prometheus/api/v1/query" \
  --data-urlencode 'query=sum by (service_name) (demo_requests_total)'
```
