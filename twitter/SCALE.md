# Calculations and Suggestions on auto-scale

## Data stored
Per user,
    * User Name       (upto 50 char, ie 50 bytes)
    * Bio             (255 char, bytes)
    * Profile Picture (1 MB)
    * user_id         (64 bytes)

Each tweet,
    * Tweet (255 char, bytes)
    * tweet_id (64 bytes)

## Rate of api usage
Let's say we have 100 thousand users to begin with, with a peak surge of 40k user signups a day. 
ie, 
    calls to /user and /user-profile pics, lets say peak of 20k/hour, approx 3300 calls per second peak.

each user on average makes 10 tweets a day, so for 100 thousand user, a million tweets per day, again following the 40% peak distribution idea, lets say a peak of 400k tweets a minute, 100k peak a second.

each user on average follows 200 other users, following an average of 10 users a day, 4 users an hour peak.

Rough scaling rules for **x** users,
    * /user and /user-profile -> 0.2 * x max calls per hour, 0.2 * 0.4 * x calls per second peak. 
    
    * 10 * x max tweets per day, 0.4 * 10 * x tweets per hour peak, 0.4 * 0.4 * 10 * x tweets per second peak. This results in : 
        ** 0.16 * x peak calls to /tweet per second.
        ** With each user tweeting on average 10 tweets a day, 10 notifications to refresh home timeline a day, ie calls to /home will result in fresh data.
        ** Given x users have on average 200 followed, 200 * x notifications for tweets per second 

    * 10 * x users being followed a day, peaking at 4 * x an hour and x per second. That results in 
        ** x calls to /user/follow per second
        ** 4 * x notifcations for user follwed per hour at peak. 

Hence notifications are overwhelmning the api-usage... hence we need heuristics to limit notifications, to say x per second. 

### ngnix scaling
ngnix suggested config is 
```
    worker_processes auto;
    events {
        worker_connections 2048;  # start here
        use epoll;  # Linux
    }
```

**Memory Usage**
Each connection consumes:
    File descriptors: 1 per connection (socket)
    Memory: ~10KB per active connection
    For reverse proxy: 2 connections (client → nginx → backend)

With 2048 connections per worker:
    Memory: 2048 × 10KB ≈ 20MB per worker
    File descriptors: 2048 × 2 = 4096 FDs (if proxying)
    Total capacity: worker_processes × worker_connections

Assuming 4 core worker nodes, we arrive at,
    Max concurrent clients: 4 × 2048 = 8,192
    Max proxy connections: 8,192 ÷ 2 = 4,096 simultaneous requests
    Total memory: 4 × 20MB ≈ 80MB for connections

Hence, memory usage is not bottleneck for nginx gateway.

**CPU Usage**
CPU Usage doesn't just depend on connections, it depends on requests, whether it is SSL ect... Rule of thumb is 
```
    RSA 2048-bit:  1,000-2,000 req/s per core (handshakes)
    ECDSA P-256:   3,000-5,000 req/s per core (handshakes)

    With session reuse: ~10,000 req/s per core (mostly symmetric crypto)
```

So, ~2 cores for 20,000 reqs/second per code.

So, for api usage, peak usage is - 0.16x /user and /user-profile + 0.16x /tweets + x notifications + x /user/follows... 2.32*x 

For 100k users, 2.32 * 10k/20k ~ 2 ngnix pods can handle it. 

Number of ngnix pods needed at (2 cores, 1 GB memory) ~ 2.32*x/20k 

## Data usage 
User account data is dominated by the profile picture, with metadata it's ~1.5 MB per user.... *but* profile pics are stored only in postgres, hence it's 1.5 MB in postgres but 350 Bytes per user in Redis. 

Tweet is 255 + 64 ~= 320 bytes per tweet... tweets are stored for a year in postgres, and a week in cache. 

Now, considering overheads, 2 * x MB + x * 365 MB = 367 * x MB is needed for postgres.

Considering following, user and followed ZSET, Stream ect, let's say it's 2 * x KB + 7 * x MB ~= 7 * x MB memory is needed for redis.

**CPU Usage**
Redis is single threaded, with a fork for BGSAVE... hence 2 cores is optimal for redis pods. Hence, 2 cores per redis pod, scale out till 7 * x memory is achieved, have 30% buffer so 10 * x. 

Postgres scales horizontally for number for connections, so can be configured for N cored for N core worker nodes. Accross all it's cores, it needs to have access to approve 400 * x disk storage, have N GB memory per pod.

## Auto scaling
Have hpa resources on every deployment, have it autoscale as per above calculations.

## Test deployment scale 
All these deployments are to run single replica, with the following requests and limits.

* **Gateway**
    Requests: 200 mCPU, 256 MB 
    Limits: 300 mCPU, 512 MB

* **Redis**
    Requests: 200 mCPU, 1 GB 
    Limits: 300 mCPU, 1.5 GB

* **Postgres**
    Requests: 200 mCPU, 256 MB 
    Limits: 300 mCPU, 512 MB
    Additionally, 5 GB PV must be mounted.

* **Timeline Service**
    Requests: 100 mCPU, 128 MB 
    Limits: 100 mCPU, 256 MB

* **Fanout Service**
    Requests: 100 mCPU, 128 MB 
    Limits: 100 mCPU, 256 MB

* **Search Service**
    Requests: 100 mCPU, 128 MB 
    Limits: 100 mCPU, 256 MB

* **Notification Service**
    Requests: 100 mCPU, 128 MB 
    Limits: 100 mCPU, 256 MB