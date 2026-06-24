# Components 

## Kubernetes 
The k8s model is as follows 

* **Web server** is a Deployment of nginx. It has a service with external-ip and use the kubernetes gateway resource to define URLs it serves as per the openapi spec.
   ** Gateway - HTTP listener on port 80

* **Timeline Service** 
    * Deployment + Service, service listening on port 8080. 
    * HTTPRoute to it's service for everything in read and write apis.

* **Fan-out Service**
    * Deployment + Service, service listening on port 8080. 
    * HTTPRoute to it's service for everything in write api.

* **Search Service**
    * Deployment + Service, service listening on port 8080. 
    * HTTPRoute to it's service for everything in search api.

* **Notification Service**
    * Deployment + Service, service listening on port 8080. 

* **Cache Syncing Service**
    * Daemonset, no service - each instance is responsible for syncing it's local cache instance with DB.

## Web-Server
An nginx deployment and uses the k8s Gateway resource. The ngnix config should configure routing based on
    * openapi/read_api.yaml
    * openapi/write_api.yaml
    * openapi/search_api.yaml

## Read Api
Defined in openapi/read_api.yaml

Apis are as follows 
  * GET /user?<id>
      Return code 200 Success, 404 Not Found
      Data: {name, bio, created_at }

  * GET /user/name?<id>
      Return code 200 Success, 404 Not Found
      Data: {name, created_at }

  * GET /user/bio?<id>
      Return code 200 Success, 404 Not Found
      Data: {bio}

  * GET /tweet?id=<user_id>&number=<num>
      Return code 200 Success, 203 data truncated, 204 no content, 404 user not found
      Data: Array of { tweet, timestamp} till execeeding num, if no  arg is passed send 500 tweets.

  * GET /user/followed?<id>
      Return code 200 Success, 204 no content, 404 user no found
      Data: Array of {id, name} of all accounts user is following

  * GET /timeline?user_id=<id>&tweets=<number_of_tweets>
      Return code 200 Success, 203 data truncated, 204 no content, 404 user not found
      Data: Returns tweets of all users <id> follows, ordered by timestamp (newest first), upto number_of_tweets (200 by default)

  * GET /home?user_id=<id>&tweets=<number_of_tweets>
        Return code 200 Success, 203 data truncated, 204 no content, 404 user not found
        Data: Returns tweets of all tweets posted by <id>, ordered by timestamp (newest first), upto number_of_tweets (200 by default)

### Usage in work-flows
* UI calls /timeline and /home apis to get info to dispay timeline or home timeline respectively.

## Write Api
Defined in openapi/write_api.yaml

  * PUT /user
      Return Code 200 create user success, 201 if updating existing user, 400 user forbidden
      Return user_id on 200 or 201, null on 400
      PayLoad : {name, bio}

  * POST /user/profile-pic?<id>
      Return Code 201 update profile pic success, 400 forbidden, 404 user not found
      Return none
      Payload : image bytes

  * POST /user/bio?<id>
      Return Code 201 update bio pic success, 400 forbidden, 404 user not found
      Return none
      Payload : bio text

  * POST /tweet?<id>
      Return Code 201 post tweet success, 400 forbidden, 404 user not found
      Return none
      Payload : tweet text

  * POST /user/follow?<id>
      Return Code 200 sucess
      Return None
      Payload: Array of user_id for $id to follow

### Usage in work-flows 
* /user and /user/profile-pic apis together are called when creating new user.
* /tweet api used when user posts a tweet

## Search Api
  * GET /search/user?<id>
      Return Code 200 or 404
      Returns (user_id, user_name)

  * GET /search/tweet?<search-phrase>
      Return Code 200 or 404
      Returns {<user_id>, <tweet_id>, <created_at>} or null for 404

### Usage in work-flows 
Called by search queries on the UI.

## Timeline Service
Timeline service is the primary backend of all of the read apis. It reads *only* from the cache, for most of the apis.

  * GET /user?<id>
    Reads from DB. 
    ```SELECT name, bio FROM users WHERE id = $id;```

  * GET /user/name?<id>
    ```SELECT name FROM users WHERE id = $id;```

  * GET /user/bio?<id>
    ```SELECT bio FROM users WHERE id = $id;``` 

  * GET /tweet?id=<user_id>&number=<num>
    ```
      #Get first num tweet ids
      ZREVRANGE user-$user_id-tweets 0 $num
    
      #Need to HGETALL for each of these,
      HGETALL tweet-$tweet_id
    ```

  * GET /user/followed?<id>
  ```ZRANGE followers:$id 0 -1```

  * GET /timeline?user_id=<id>&tweets=<number_of_tweets>   
      For this, timeline service should
      1. Get list of all users $user_id follows
         ```ZRANGE followers:$id 0 -1```
      2. Get tweets from all of them, till we hit limit

  * GET /home?user_id=<id>&tweets=<number_of_tweets>
    This is simpler, just get own tweets, equivalent to /tweet?id=<user_id>&number=<num>

## Fan-out Service
Fan-out service is the primary backed for the write api. It updates cache and database.

When creating a new user, UI should show seperate api calls for /user and /user/profile-pic

  * PUT /user
  ```
    INSERT INTO users (id, name, bio) 
    VALUES ($1, $2, $3)
    ON CONFLICT (id) 
    DO UPDATE SET 
      name = EXCLUDED.name,
      bio = EXCLUDED.bio,
      updated_at = CURRENT_TIMESTAMP
    RETURNING id, created_at, updated_at;
  ```

  check if created and updated are equal to find out whether new insert or update.

  * POST /user/profile-pic?<id>
  ```
    INSERT INTO profile_pictures (user_id, image_data, mime_type, file_size, width, height)
    VALUES ($1, $2, $3, $4, $5, $6)
    ON CONFLICT (user_id) 
    DO UPDATE SET 
      image_data = EXCLUDED.image_data,
      mime_type = EXCLUDED.mime_type,
      file_size = EXCLUDED.file_size,
      width = EXCLUDED.width,
      height = EXCLUDED.height,
      uploaded_at = CURRENT_TIMESTAMP
    RETURNING id, user_id, uploaded_at;
  ```

  * POST /user/bio?<id>
  ```
    INSERT INTO users (id, bio) 
    VALUES ($1, $2, $3)
    ON CONFLICT (id) 
    DO UPDATE SET 
      name = EXCLUDED.name,
      bio = EXCLUDED.bio,
      updated_at = CURRENT_TIMESTAMP
    RETURNING id, created_at, updated_at;
  ```

  * POST /tweet?<id>

    First update the cache,
    ```ZADD tweets-$id $TIMESTAMP $TWEET```

    Then, publish the tweet
    ```XADD tweets-$id:events * tweet_id $tweet_id timestamp $TIMESTAMP content $TWEET```

    Insert in postgres
    ```INSERT INTO tweets (user_id, content) VALUES ($user_id, $TWEET);```

  * POST /user/follow?<id>
    
    First update the cache,
    ```
      ZADD followers-$id $TIMESTAMP $follower_id
      ZADD followed-$id  $TIMESTAMP $followed_id
    ```

    Then, publish the tweet

    Insert in postgres

## Search Service
Search service is the backend for the search api.

  * GET /search/user?<id>
  ```SELECT (id, name) FROM users where id=$id```

  * GET /search/tweet?<search-phrase>
  ```SELECT (id, user_id, created_at) FROM tweets WHERE content_tsv @@ to_tsquery($search-phrase) ORDER BY created_at; ```


## Notification Service
The notification service is subscribed to the cache's ZSET, maintains internal map of which user follows whom... when notification comes of addition of tweet from cache, this service's job is to display notification to appropriate users.

ie, Notification service consumes from these two streams and maintains mapping of users
```
  XGROUP CREATE  followed:events fanout-followed $ MKSTREAM 
  XREADGROUP GROUP fanout-followed consumer-name COUNT 100 BLOCK 5000 STREAMS tweets:events >

  XGROUP CREATE  follower:events fanout-follower $ MKSTREAM 
  XREADGROUP GROUP fanout-follower consumer-name COUNT 100 BLOCK 5000 STREAMS tweets:events >
```

And then when tweets come in it should send corresonding notification, acking if it is doing so.

```
  XGROUP CREATE tweets:events fanout-tweets $ MKSTREAM 
  XREADGROUP GROUP fanout-tweets consumer-name COUNT 100 BLOCK 5000 STREAMS tweets:events >
```

Upon recieving notification, the service will send SSE to frontend, frontend responsibility to manage duplicates, flooding ect of SSEs


## Cache Syncing Service
