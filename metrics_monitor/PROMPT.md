Generate a MVP of a observability monitory platform, using golang and react.js. The platform pulls in logs, metrics and traces from opentelemetry collector and presents it in a UI.

** Core Requirements **
1. Users should be able to query and visualize metrics on dashboards with filters, aggregations, and time ranges

2. Users should be able to define alert rules with thresholds over time windows (e.g., "alert if p99 latency > 500ms for 5 minutes")

3. Users should receive notifications when alerts fire (email, Slack, PagerDuty)

** Non-Functional Requirements **
1. The system should scale to ingest 5M metrics per second from 500k servers

2. Dashboard queries should return within seconds, even for queries spanning days or weeks

3. Alerts should evaluate with low latency (< 1 minute from metric emission to alert firing)

4. The system should be highly available. We can tolerate eventual consistency for dashboards, but alert evaluation should be reliable.

5. The system should handle late or out-of-order data gracefully (network delays are common)