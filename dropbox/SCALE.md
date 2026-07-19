Going over scaling aspects - we basically have two things here **users** and their **storage**

# Users
Users use the api - hence load the server and gateway.

DB metadata is proportional to **Total registered users** 

API calls are proportional to **Active Users**

Let's say every active user have 0.1 req/sec, so for 20k active users,
```metadata_rps = 20,000 * 0.1 = 2,000 RPS```

Gateways should be able handle these api load, we can assume DB updates to be also nearly equal to this, 2000 updates per second for database.

# Storage
Assume each user needs on average 16 GB... NFS should have 16 * users GB on hand.