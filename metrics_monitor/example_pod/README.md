# Example OpenTelemetry Pod

This folder contains a small Go workload that generates logs, metrics, and traces with OpenTelemetry client libraries and exports all three signals over OTLP gRPC.

The default endpoint is `otel-collector:4317`, so deploy it into a namespace where an OpenTelemetry Collector service with that name is reachable, or override `OTEL_EXPORTER_OTLP_ENDPOINT` in `k8s.yaml`.

## Generated Signals

- Traces: a server span named `synthetic.request` plus client dependency spans for PostgreSQL and Redis.
- Metrics: request counters, error counters, latency histogram, queue depth gauge, and active users gauge.
- Logs: structured OpenTelemetry log records correlated with the active request span context.

## Run Locally

Start an OpenTelemetry Collector that accepts OTLP gRPC on port `4317`, then run:

```sh
go mod tidy
OTEL_EXPORTER_OTLP_ENDPOINT=localhost:4317 go run .
```

Use `OTEL_EXPORTER_OTLP_INSECURE=false` if your collector endpoint requires TLS.

## Build And Deploy To Kubernetes

Build the image:

```sh
docker build -t example-otel-pod:local .
```

For kind clusters, load the image before applying the manifest:

```sh
kind load docker-image example-otel-pod:local
kubectl apply -f k8s.yaml
```

For minikube, build inside the minikube Docker daemon or push the image to a registry and update `k8s.yaml`.

## Configuration

| Environment variable | Default | Description |
| --- | --- | --- |
| `OTEL_SERVICE_NAME` | `example-otel-pod` | OpenTelemetry service name. |
| `OTEL_EXPORTER_OTLP_ENDPOINT` | `otel-collector:4317` | OTLP gRPC collector endpoint. |
| `OTEL_EXPORTER_OTLP_INSECURE` | inferred from endpoint scheme | Set to `true` for plaintext gRPC. |
| `DEPLOYMENT_ENVIRONMENT` | `demo` | Resource attribute for environment. |
| `EMIT_INTERVAL` | `2s` | How often to emit a synthetic request. |
| `METRIC_EXPORT_INTERVAL` | `10s` | Periodic metric export interval. |
