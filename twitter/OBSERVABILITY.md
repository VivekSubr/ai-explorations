# Observality strategy 
First off - we need a different DB for logs than our main one - Postgres, one optimized for Logging, which is write heavy, reads being timeseries based.

We are going for the grafana stack for this project - Loki for logs, Mimir for metrics.

## Logging 
This project uses Grafana Loki for log storage and analytics. So the following resources need to run on cluster -
* **Promtail Agent** daemonset, used to collect logs from each pod.
* **Loki** StatefulSet, which stores the logs, along with a load balancer service for it.

Expection is that all pods will write their logs to stdout, which will be scrapped by promtail and sent to Loki.

### Pod level Logging
All services should use a common logging library, so we will generate a library in utils folder, 'twitter-log', and all services MUST import and use that library for logging.

**twitter-log library**
Exposes a enum for logging level : Error, Info, Debug

Exposes a public api Log that writes to stdout, Log(LogLevel, string)
Usage: Log(LogLevel.Info, "Test Log")

This api will write to stdout as, { <id>, <loglevel>, <log-string>}... with id being a monotonic increasing id.

### Promtail labelling
The promtail config will look something like this, each pod should have a label 'app' that'll be labelled in it's logs.

Since twitter-log prints a simple json, the promtail has json processing step. (It also has regex based approach for more free form)

```
scrape_configs:
  - job_name: kubernetes-pods
    kubernetes_sd_configs:
      - role: pod
    
    relabel_configs:
      # Standard Kubernetes metadata
      - source_labels: [__meta_kubernetes_namespace]
        target_label: namespace
      
      - source_labels: [__meta_kubernetes_pod_name]
        target_label: pod
      
      - source_labels: [__meta_kubernetes_pod_label_app]
        target_label: app
      
      - source_labels: [__meta_kubernetes_pod_container_name]
        target_label: container

      # Extract pod labels to Loki labels
      - source_labels: [__meta_kubernetes_pod_label_app]
        target_label: app
    
    # Parse JSON logs
    pipeline_stages:
      - json:
          expressions:
            loglevel: loglevel        # Extract loglevel field
            id: id                    # Extract id field
            message: log-string       # Extract log-string field
      
      - labels:
          loglevel:   # Add loglevel as a label (low cardinality - good!)
      
      # DON'T add 'id' as label - high cardinality will kill performance
      
      - output:
          source: message   # Use log-string as the actual log line
```

### Loki 
Configure to store logs on cluster, mount /logs path as /loki.

## Metrics 
This project uses Grafana Mimir for metrics storage and analytics. So the following resources need to run on cluster -
**Prometheus Agent** daemonset to scrape metrics from each pod on it's worker.
**Mimir** StatefulSet 

### Pod Level Metrics
All services should use a common metrics library, so we will generate a library in utils folder, 'twitter-metrics', and all services MUST import and use that library for metrics.

**twitter-metrics library**
Metrics library spins up a thread that registers and serves /metrics endpoint for prometheus to use. Library provides wrappers for creating and pegging prometheus constructs - counters, gauges ect.

### Prometheus Agent
Prometheus agent daemonset is deployed to scrape metrics from each pod on a worker node, and send to mimir.

### Mimir
Mimir StatefulSet in monolithic mode, with a load balancer model bound to it. Mimir is configured to store on local filesystem.

## Dashboards
