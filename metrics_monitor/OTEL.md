# Opentelemetry 
An open standard for all signals - logs, metrics and traces.

## Client 
Clients should use opentelemetry client for all telemetry - logs, metrics and tracing. All telemetry is exported using OTLP protocol.

## Collector 
Otel collector is optional component, it is used to unify telemetry and then send to respective backends.


