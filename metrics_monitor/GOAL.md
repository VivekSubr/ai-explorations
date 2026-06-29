Generate a MVP of a observability monitory platform, using golang and react.js. The platform pulls in logs, metrics and traces from opentelemetry collector and presents it in a UI.

# Metrics Goals 

**Core Requirements**
1. Users should be able to query and visualize metrics on dashboards with filters, aggregations, and time ranges

2. Users should be able to define alert rules with thresholds over time windows (e.g., "alert if p99 latency > 500ms for 5 minutes")

3. Users should receive notifications when alerts fire (email, Slack, PagerDuty)

**Non-Functional Requirements**
1. The system should scale to ingest 5M metrics per second from 500k servers

2. Dashboard queries should return within seconds, even for queries spanning days or weeks

3. Alerts should evaluate with low latency (< 1 minute from metric emission to alert firing)

4. The system should be highly available. We can tolerate eventual consistency for dashboards, but alert evaluation should be reliable.

5. The system should handle late or out-of-order data gracefully (network delays are common)

# Logging Goals 

**Core Requirements**
1. Users should be query and visualize logs on dashboards with filters, regex, and time ranges

2. Users should be able to define alert rules with thresholds over time windows (e.g., "Error log seen thrice in 60s")

3. Users should receive notifications when alerts fire (email, Slack, PagerDuty)


**Non-Functional Requirements**
1. The system should sustain 500k log entries per second from 500k servers as an average target. This corresponds to 1 log entry/server/sec, which is a more realistic planning baseline than matching metrics event-for-event.

2. The system should handle 5M log entries per second as a burst target when workloads become noisy. This corresponds to 10 log entries/server/sec, matching the metrics target only as a worst-case per-server event-density envelope.

3. Log storage and ingestion should be sized by bytes/sec as well as entries/sec. At 1KB/log entry, this is about 500MB/sec average and 5GB/sec at the burst target before compression.

4. Log queries over recent data should return within seconds for high-selectivity filters. Broad regex scans over large time ranges may require asynchronous or paginated query execution.

5. Log alerting should evaluate with low latency (< 1 minute from log emission to alert firing) while handling duplicate and late-arriving log entries gracefully.


# Tracing Goals

**Core Requirements**


**Non-Functional Requirements**
1. The system should sustain 500k sampled spans per second from 500k servers as an average target. This corresponds to 1 sampled span/server/sec.

2. The system should handle 5M sampled spans per second as a burst target. This corresponds to 10 sampled spans/server/sec, matching the metrics target only as a worst-case per-server event-density envelope.

3. Assuming 10 spans/trace, the trace target is about 50k sampled traces/sec average and 500k sampled traces/sec at burst.

4. Trace ingestion should support head or tail sampling before storage. At 1% sampling, 50k sampled traces/sec represents roughly 5M traces/sec before sampling, and 500k sampled traces/sec represents roughly 50M traces/sec before sampling.

5. Trace lookup by trace ID should return within seconds, and service/dependency queries should return within seconds for recent time windows.