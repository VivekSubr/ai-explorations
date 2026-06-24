# Authentication, Authorization and Accounting
This doc details AAA aspects of this project. 

**Authentication**
Authentication, as in proving client call is from real and correct user. The open-api spec for read, write, search specifies that apis must have bearer and identification token.

**Authorization**
Is this user authorized to do this? In this project no much to do here - user allowed for all apis.

**Accounting**
Tracking of what user did what when. This is taken care by audit logs that should be maintained by backend services.

## Session Api
Defined in openapi/session_api.yaml

Apis are as follows 
    * POST /register 
      Return code 200 Success, 422 bad data
      Data: { user_name, password, bio }

    * POST /login?<user_id>
      Return code 200 Success, 404 Not Found
      Return JWT token and ID token
      Data: password (in base64)

    * POST /logout?<ID token> 
      Return 200

## Session Service 
Service running on cluster that issues JWT and OIDC tokens. Session apis are supposed to land on this service, and it will also update DB as required.

Use Dex: https://github.com/dexidp/dex


## Kubernetes RBAC
Kubernetes RBAC is for defense in depth inside the cluster - what service should have access to what?

First up, in this project no pod needs k8s api permission at all. Next we need to ask which services need access to Redis or Postgres.

List the services in the cluster -
* Webserver        Deployment      None
* Timeline-svc     Deployment      Postgres and Redis
* Fan-out-svc      Deployment      Redis and Postgres for outbox table only 
* Search-svc       Deployment      Postgres only 
* Notification-svc Deployment      Redis only 
* Cache-sync-svc   Deployment      Redis only 
* Redis            Daemonset 
* Postgres         Statefulset

Create service accounts for all of these, (web-sa, time-sa, fan-sa, search-sa, notif-sa, cache-sync-sa, redis-sa, postgres-sa). Disable service token mounting for all of these -
```
spec:
  template:
    spec:
      serviceAccountName: web-sa
      automountServiceAccountToken: false
```