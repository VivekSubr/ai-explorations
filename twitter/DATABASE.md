# Database and Cache
Database is postgres, Cache is Redis. 

The full datamodel and all the data lives in postgres. We also need postgres to support full text search for Search api. 

Redis is used as a cache and mq-lite for rapid fan-out and notifications.

## Database model
The core elements that come out from HLD are 
    * USER, table of user profiles, with seperate table for profile pics and for relation - "who follows who"

    * TWEET, upto 255 character message, keep in a seperate table with user id as foreign key

    * TIMELINE of a user - no need for seperate table, just query the TWEET table for user.

### User
```
-- 1. Main users table
CREATE TABLE users (
    id BIGSERIAL PRIMARY KEY,
    name VARCHAR(255) NOT NULL,
    bio TEXT,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);

-- 2. Profile pictures table
CREATE TABLE profile_pictures (
    id BIGSERIAL PRIMARY KEY,
    user_id BIGINT NOT NULL UNIQUE REFERENCES users(id) ON DELETE CASCADE, #UNIQUE enforces 1:1 with users
    image_data BYTEA NOT NULL,
    mime_type VARCHAR(50) NOT NULL,
    file_size INTEGER,
    width INTEGER,
    height INTEGER,
    uploaded_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    CONSTRAINT valid_mime_type CHECK (mime_type IN ('image/jpeg', 'image/png', 'image/gif', 'image/webp'))
);

-- 3. Follows table
CREATE TABLE follows (
    id BIGSERIAL PRIMARY KEY,
    follower_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    following_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    CONSTRAINT no_self_follow CHECK (follower_id != following_id),
    UNIQUE(follower_id, following_id)
);

-- Indexes
CREATE INDEX idx_profile_pictures_user_id ON profile_pictures(user_id);
CREATE INDEX idx_follows_follower_id ON follows(follower_id);
CREATE INDEX idx_follows_following_id ON follows(following_id);
CREATE INDEX idx_follows_both ON follows(follower_id, following_id);

-- Triggers
CREATE OR REPLACE FUNCTION update_updated_at_column()
RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = CURRENT_TIMESTAMP;
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER update_users_updated_at
    BEFORE UPDATE ON users
    FOR EACH ROW
    EXECUTE FUNCTION update_updated_at_column();
```

### Tweet
```
CREATE TABLE tweets (
    id BIGSERIAL PRIMARY KEY,
    user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    content VARCHAR(255) NOT NULL,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    
    -- Ensure tweet is not empty or just whitespace
    CONSTRAINT content_not_empty CHECK (char_length(trim(content)) > 0)
);

-- Index for fetching user's tweets
CREATE INDEX idx_tweets_user_id ON tweets(user_id);

-- Index for chronological queries
CREATE INDEX idx_tweets_created_at ON tweets(created_at DESC);

-- Composite index for user timeline queries
CREATE INDEX idx_tweets_user_created ON tweets(user_id, created_at DESC);

-- Auto-update trigger for updated_at
CREATE TRIGGER update_tweets_updated_at
    BEFORE UPDATE ON tweets
    FOR EACH ROW
    EXECUTE FUNCTION update_updated_at_column();
```

## Cache Model 
The cache is there to support two operaions
    * Fan-Out --> New tweet by user should quickly be visible in all relevant timelines.
    * Notifications --> Based on threshold, notifications should be sent out for new tweets.

To support this, we have the following structures in the cache
    * HSET of tweets to user_id, 
        tweet:{tweet_id} -> Hash
            - user_id: "123"
            - text: "Hello world"

    And a ZSET of user_id to tweets,
        user:{user_id}:tweets -> Sorted Set
            - Members: tweet_id
            - Score: timestamp (for chronological ordering)

    So, adding tweet becomes
    1. Store a tweet ```HSET tweet-$tweet_id user_id $user_id text "My first tweet"```
    2. Add to user   ```ZADD user-$user_id-tweets $TIMESTAMP $user_id```

    When adding to ZSET, fan out service will also XADD to a stream, 
    ```XADD tweets:events * user_id $id tweet_id $tweet_id timestamp $TIMESTAMP content "..."```

    Timeline and Notification service are consumers of the stream. They will create a group and wait for input from it
    ```
        #Create 'fanout' consumer group
        XGROUP CREATE tweets:events fanout-tweets $ MKSTREAM 

        #Read entries upto 100 messages, with 5000 msec of wait before new poll
        XREADGROUP GROUP fanout-tweets consumer-name COUNT 100 BLOCK 5000 STREAMS tweets:events >
    ```

    * ZSET of user_id : followed_users... whenever new user is followed, write_api should hit fanout service.
      ```ZADD $user_id $TIMESTAMP $followed_user_id```

      And XADD to a stream as well,
      ```XADD followed:events * user_id $id followed_id $followed_user_id timestamp $TIMESTAMP```

      Again Timeline and Notification services should consume and update themselves
      ```
        XGROUP CREATE  followed:events fanout-followed $ MKSTREAM 
        XREADGROUP GROUP fanout-followed consumer-name COUNT 100 BLOCK 5000 STREAMS tweets:events >
      ```

    * ZSET of user_id : followers,  whenever new user is followed, write_api should hit fanout service.
      ```ZADD $user_id $TIMESTAMP $follower_user_id```

      And XADD to a stream as well,
      ```XADD follower:events * user_id $id follower_id $follower_user_id timestamp $TIMESTAMP```

      Again Timeline and Notification services should consume and update themselves
      ```
        XGROUP CREATE  follower:events fanout-follower $ MKSTREAM 
        XREADGROUP GROUP fanout-follower consumer-name COUNT 100 BLOCK 5000 STREAMS tweets:events >
      ```

### Cache Syncing
All the cache instances need to be **synced**, so that we get convergence of all the timeline and notification services which depend on the caches.

Postgres should remain the source of truth. Redis should be treated as a derived cache that can be rebuilt from Postgres if it becomes stale or is lost.

We can not rely on direct dual-writes from the API like "write Postgres, then write Redis" as the only sync mechanism. If Postgres commits but the Redis write fails, the cache becomes inconsistent.

For cache syncing, we can maintain a seperate 'outbox' tables,
```
    CREATE TABLE cache_outbox (
        id BIGSERIAL PRIMARY KEY,
        event_type VARCHAR(50) NOT NULL,
        aggregate_type VARCHAR(50) NOT NULL,
        aggregate_id BIGINT NOT NULL,
        payload JSONB NOT NULL,
        created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
        processed_at TIMESTAMP WITH TIME ZONE
    );

    CREATE INDEX idx_cache_outbox_unprocessed ON cache_outbox(id) WHERE processed_at IS NULL;
```

And insert into tweets table should become a transaction
```
    BEGIN;
        INSERT INTO tweets (user_id, content) VALUES (42, 'hello world');
        INSERT INTO cache_outbox (event_type, aggregate_id, payload)
            VALUES ('tweet.created', 42, '{"tweet_id": 99, "content": "hello world"}');
    COMMIT;
```

Cache sync worker is supposed to check cache_outbox for tweets not in cache, and update cache accordingly.

## Search 
The tweets table should have tsvector 

```
CREATE TABLE tweets (
    id BIGSERIAL PRIMARY KEY,
    user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    content VARCHAR(255) NOT NULL,
    content_tsv tsvector,  -- Full-text search vector
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    CONSTRAINT content_not_empty CHECK (char_length(trim(content)) > 0)
);

-- Indexes
CREATE INDEX idx_tweets_user_id ON tweets(user_id);
CREATE INDEX idx_tweets_created_at ON tweets(created_at DESC);
CREATE INDEX idx_tweets_user_created ON tweets(user_id, created_at DESC);
CREATE INDEX idx_tweets_content_tsv ON tweets USING GIN(content_tsv);

-- Triggers
CREATE TRIGGER update_tweets_updated_at
    BEFORE UPDATE ON tweets
    FOR EACH ROW
    EXECUTE FUNCTION update_updated_at_column();

CREATE TRIGGER tweets_content_tsv_trigger
    BEFORE INSERT OR UPDATE OF content ON tweets
    FOR EACH ROW
    EXECUTE FUNCTION tweets_content_tsv_update();
```

Full text search can be done using this tsvector
```
-- Search for tweets containing "kubernetes"
SELECT 
    t.id,
    t.content,
    t.created_at,
    u.name AS author
FROM tweets t
JOIN users u ON t.user_id = u.id
WHERE t.content_tsv @@ to_tsquery('english', 'kubernetes')
ORDER BY t.created_at DESC;
```


## Kubernetes Deployment Model
Deploy postgres as a cluster daemonset using this operator: https://github.com/cloudnative-pg/cloudnative-pg

Deploy redis, same, as a cluster daemonset, using Redis Cluster. 

* **Postgres resources**
    * Daemonset of postgres pods 
    * Deployment of cloudnative-pg operator
    * Service for postgres, port 8080
    * Service for operator, port 8080

* **Redis resources**
    * Daemonset of redis pods 
    * Deployment of redis cluster operator
    * Service for redis, port 8080
    * Service for operator, port 8080

### Resiliency 