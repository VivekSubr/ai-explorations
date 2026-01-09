Generate a *distributed cache* in golang, with images being deployed onto kubernetes as a daemonset.

** Core functional Requirements **
1. Users should be able to set, get, and delete key-value pairs.
2. Users should be able to configure the expiration time for key-value pairs.
3. Data should be evicted according to Least Recently Used (LRU) policy.

** Core non-functional requirements **
1. The system should be highly available. Eventual consistency is acceptable.
2. The system should support low latency operations (< 10ms for get and set requests).
3. The system should be scalable to support the expected 1TB of data and 100k requests per second.

Use redis as the backend for this cache, implement an api for users.
1. GET KEY, eg curl http://localhost:80/<key>
2. SET KEY, eg curl -X POST http://localhost:80/<key> -d <value>
3. DELETE KEY, eg curl -X DELETE http://localhost:80/<key>

** Suggested folder structure **
- src
  - redis_client.go
  - api.go 
- test
- deploy (contains deployment scripts and kubernetes resources)
