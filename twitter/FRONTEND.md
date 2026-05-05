# Frontend api calls for workflows

## Creating new user
Frontend should call 
    * POST /user
    * POST /user/profile-pic

## User Posts a Tweet
Frontend should call POST /tweet?<user_id> 

## User views home Timeline
GET /home?user_id=<id>&tweets=<number_of_tweets>

Paginating with number_of_tweets

## User views another account's timeline
GET /timeline?user_id=<id>&tweets=<number_of_tweets>

Paginating with number_of_tweets

## User searches keywords
GET /search/tweet?<search-phrase>