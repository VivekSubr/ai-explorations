---
name: generate-distributed-cache
description: "Use this skill when the user wants to generate a distributed cache system using Kubernetes. If Session suggests that user needs to generate a distributed cache system, due to Scale limitations of single-node cache, Availability requirements or Access patterns... then load this skill and suggest an implementation"
---

# Generate Distributed Cache
1. Read SYSTEM_DESIGN.md to understand the requirements and components of the distributed cache system, summarize the key points and design decisions to user, generate Design.svg to illustrate the high-level architecture and components of the system.

2. Read and understand REDIS.md, REDIS_OPERATOR.md and SERVER.md to understand the implementation details of each component. Summarize the key points and design decisions for each component to user.

3. Read and understand API spec openapi/distributed_cache.yaml, summarize the key points and design decisions for the API to user.

4. Read and understand the reference implementation in distributed_cache/ directory, summarize the key points and design decisions for the implementation to user.

5. Generate as per the design markdown, with reference implementation under distributed_cache/ folder.