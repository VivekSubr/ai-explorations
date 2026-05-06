# High Level Design

## Use cases 
* USER can post a TWEET, and that should be visible to all it's followers on their TIMELINES.
* USER can view own TIMELINE, and 'HOME TIMELINE', ie timeline of tweets from all accounts user follows.
* USER can search for TWEETs using keywords.

## Constraints and Assumptions
**General**
* Traffic is not evenly distributed.
* Posting a tweet should be fast - fan out of tweets must be fast, even if user has millions of followers.

**Timeline**
* Viewing timeline must be fast.
*  Twitter is read heavy, optimize for reads.

  **Search**
  * Must be fast
  * Again, read optimize.

## Components
* **Client** 
    Usually web browser

* **Backend** Web server.
    Backend has three distinct swagger apis defined -> **Read**, **Write** and **Search**

**Services** - logical units which have responsibility for certain work flows.
**Timeline Service** : Responsible for sending Timeline to UI, for both user's home and other accounts.
**Fan Out Service**  : Main user of the write api, responsible for writting new tweets to cache/DB, in such a way that they fan out fast.
**Search Service** : Responsible for responding to search queries using search api
**Notification Service** : Sends out notifications of popular posts by accounts followed by user.

* **Cache**
    An in-memory cache to support fast read and fan out requirements.

* **Database** 
    The actual DB storing data long term.

```mermaid
graph LR
    Client[Client] --> WebServer[Web Server]
    
    WebServer --> ReadAPI[Read API]
    WebServer --> WriteAPI[Write API]
    WebServer --> SearchAPI[Search API]
    
    ReadAPI --> TimelineService[Timeline Service]
    ReadAPI --> NotificationService[Notification Service]
    
    WriteAPI --> FanOutService[Fan Out Service]
    WriteAPI --> NotificationService[Notification Service]

    SearchAPI --> SearchService[Search Service]
    
    TimelineService --> Cache1[Cache]
    Cache1 --> Database[(Database)]
    
    FanOutService --> Cache2[Cache]
    FanOutService --> Database
    
    SearchService --> Cache4[Cache]

    NotificationService[Notification Service] --> TimelineService
    
    style Client fill:#e1f5ff
    style WebServer fill:#fff4e1
    style ReadAPI fill:#f0e1ff
    style WriteAPI fill:#f0e1ff
    style SearchAPI fill:#f0e1ff
    style TimelineService fill:#e1ffe1
    style NotificationService fill:#e1ffe1
    style FanOutService fill:#e1ffe1
    style SearchService fill:#e1ffe1
    style Database fill:#ffe1e1
```

## Core Work Flows

* **User Posts a Tweet**
Web-Server uses write api to send tweet to fan out service, which will update Database and Cache. 

Notification Service should fetch which other users are interested in this ('which accounts follow this user') and send notification to all of them.

Timeline Service should update home timeline and timelines of all accounts following this user.

```mermaid
graph LR
    WebServer[Web Server] -->|Send Tweet| WriteAPI[Write API]
    WriteAPI --> FanOutService[Fan Out Service]
    
    FanOutService -->|Update| Database[(Database)]
    FanOutService -->|Update| Cache[Cache]
    
    FanOutService -->|Trigger| NotificationService[Notification Service]
    FanOutService -->|Trigger| TimelineService[Timeline Service]
    
    NotificationService -->|Fetch Followers| Database
    NotificationService -->|Send Notifications| Followers[Follower Accounts]
    
    TimelineService -->|Update Home Timeline| Cache
    TimelineService -->|Update Follower Timelines| Cache
    TimelineService -->|Persist| Database
    
    style WebServer fill:#fff4e1
    style WriteAPI fill:#f0e1ff
    style FanOutService fill:#e1ffe1
    style NotificationService fill:#ffe1f5
    style TimelineService fill:#ffe1f5
    style Database fill:#ffe1e1
    style Cache fill:#f5f5dc
    style Followers fill:#e1f5ff
```

* **User views home Timeline**
Request lands on timeline service via read api, and it's a simple fetch of user's own posts (top 20 or so), with pagination repeating the request.

* **User views another account's timeline**
Same as home timeline, only for other account.

* **User searches keywords**
Request lands on Search Service, using the search api. Search service queries the DB, which has full text search capability.
