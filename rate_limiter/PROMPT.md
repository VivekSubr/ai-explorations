Create a MVP of a rate limiter, using envoy, deployable on kubernetes

** Core functional Requirements **
1. The system should identify clients by user ID, IP address, or API key to apply appropriate limits.
2. The system should limit HTTP requests based on configurable rules (e.g., 100 API requests per minute per user).
3. When limits are exceeded, the system should reject requests with HTTP 429 and include helpful headers (rate limit remaining, reset time).