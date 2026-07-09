# REST Api Design
Running though major concepts of REST api design.

## Nouns and Verbs
First step to decide on **nouns** of the system, eg: user, file, login ect. 

Then, decide on what **verbs** each noun should have - GET, POST, PUT ect.

## Idempotency 
Idempotency: performing the same operation multiple times produces the same end state as performing it once. Formally, if f is idempotent, f(f(x)) = f(x) — applying it again after the first time doesn't change anything further.

**What it means in REST**
Idempotent ≠ same response every time. It means same resulting state every time.

eg: DELETE /user?123 -> 200 OK first time and 404 second time... but same state for both.

'Safe' generally means - no state change at all, like GET. PUT is idempotent (same patch) but not safe... if the patching triggers some operation on backend, PUT may not even be idempotent.

**Table**
| Verb | Idempotent? | Safe? | Typical use |
| --- | --- | --- | --- |
| GET | Yes | Yes | Read |
| PUT | Yes | No | Full replace (client supplies full representation) |
| PATCH | Not by default (can be designed to be) | No | Partial update |
| DELETE | Yes | No | Remove |
| POST | **No** | No | Create, or non-idempotent actions |


**Idempotency Key**
It's a UUID header. Backends use this UUID to know if api calls are duplicate, eg: same fields and greater UUID - discard. 

Useful for making POST safe over unreliable networks (which is all of them). 

Special case - two requests with same UUID - one needs to get a 409 response.

## Status Codes 

| Code | Meaning | When to reach for it |
| --- | --- | --- |
| 200 | OK | Successful GET/PUT/PATCH |
| 201 | Created | Successful POST that creates a resource; include `Location` header |
| 202 | Accepted | Async processing started, not yet complete |
| 204 | No Content | Successful DELETE, or PUT with no body to return |
| 400 | Bad Request | Malformed syntax/validation failure |
| 401 | Unauthorized | Missing/invalid credentials |
| 403 | Forbidden | Authenticated but not authorized |
| 404 | Not Found | Resource doesn't exist |
| 409 | Conflict | Version/state conflict (e.g. optimistic lock failure) |
| 422 | Unprocessable Entity | Syntactically valid, semantically invalid |
| 429 | Too Many Requests | Rate limited — include `Retry-After` |
| 500 / 503 | Server error / Unavailable | Distinguish transient (retryable) from permanent |

## E-Tags
An ETag (Entity Tag) is a header the server attaches to a response as a fingerprint of that resource's current state — usually a hash of the content, or a version number. It's how HTTP does cheap *"has this changed since I last saw it?"* 

## Versioning 
Basically two ways - 
1. URL way, eg: /v1/users
2. Header way, eg: have version header

## Pagination 
Basically, two ways to handle pagination in REST.

**Offset**
GET /orders?offset=100&limit=20

Simple and maps directly to SQL, eg:
```SELECT * FROM orders ORDER BY id LIMIT 20 OFFSET 100```

**Cursor**
GET /feed?cursor=eyJpZCI6MTIzfQ&limit=20

Basically, backend tracks of cursors of where user is.

## Errors 
RFC 7807 is the standard for returing errors in REST

```json
{
  "type": "https://api.example.com/errors/insufficient-funds",
  "title": "Insufficient funds",
  "status": 402,
  "detail": "Account balance is $12.00, charge requires $50.00",
  "instance": "/payments/abc123"
}
```

## Observability
**traceparent** header is the standard way for REST observablity. It's a number split into version, trace-id, parent-id and trace-flags.