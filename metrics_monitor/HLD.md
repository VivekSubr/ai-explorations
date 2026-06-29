# High Level Design 
We are going for the grafana stack for this project - Loki for logs, Mimir for metrics. We will use Tempo for traces. (LGTM stack)

All logging, metrics, traces are expected to be unified in an opentelemetry collector stage before being sent to respective solution for storage.

## Components

**Opentelemetry** 
Collector and operator. This is optional actually, since Loki, Mimir and Jaeger accept OTLP directly... but still useful to have a unification step for all signals.

**Loki** 
Loki is Grafana's log aggregation system designed around a "logs as metrics" philosophy — index minimally, store cheaply, query with labels. 

**Mimir** 


**Tempo**