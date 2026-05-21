# Distributed Cache
Redis is used as a distributed cache here.

Distributed cache stores:
* Simple Key : Value strings, with TTL 
    SET key value EX ttl 
    GET key
    DEL key

* Key : Json, with JsonPaths accepted as queries
    JSON.SET key path json_serialized EXPIRE ttl
    JSON.GET key path
    JSON.DEL key path
