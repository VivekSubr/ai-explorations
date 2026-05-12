# Distributed Cache System Design
Cache runs as a daemonset on Kubernetes, ensuring that each node in the cluster has a local cache instance. This design allows for low-latency access to cached data while also providing high availability and scalability.

## Requirements
**Core functional Requirements**
1. Users should be able to set, get, and delete key-value pairs.
2. Users should be able to configure the expiration time for key-value pairs.
3. Data should be evicted according to Least Recently Used (LRU) policy.
4. User should be able set, get and delete JSON objects as values, with a maximum size of 1MB per object.
5. User should be query and update JSON objects using JSONPath expressions.

**Core non-functional requirements**
1. The system should be highly available. Eventual consistency is acceptable.
2. The system should support low latency operations (< 10ms for get and set requests).
3. The system should be scalable to support the expected 1TB of data and 100k requests per second.

**High Level Design Requirements**
1. Cache should be highly available and fault tolerant
Use redis cluster api's replication to setup master-replicas: https://redis.io/docs/latest/operate/oss_and_stack/management/replication/

2. Cache must be scalable
Horizontal scaling using redis cluster: https://redis.io/docs/latest/operate/oss_and_stack/management/scaling/

3. Key distribution should be even

4. 'hot-keys' should be explicitly handled, using read-replicas.

5. Cache should be performant

## Components 
**Redis** - The actual cache, details in REDIS.md

**Redis Operator** - Manage redis cluster, details in REDIS_OPERATOR.md

**Server** - serve the apis, details in SERVER.md

## Api
**Key Value apis**
1. GET KEY, eg curl http://localhost:80/<key>
2. SET KEY, eg curl -X POST http://localhost:80/<key> -d <value>
   alternatively, -d {"data": {}, "expiry_sec": 60 }
3. DELETE KEY, eg curl -X DELETE http://localhost:80/<key>

**Json apis**
1. GET JSONPATH, eg curl http://localhost:80/json?<jsonpath>
2. SET JSONPATH, eg curl -X POST http://localhost:80/json?<jsonpath> -d <json>
                 Note that expiry_key can be set only if jsonpath points to root
3. DELETE JSONPATH, eg curl -X DELETE http://localhost:80/json??<jsonpath>