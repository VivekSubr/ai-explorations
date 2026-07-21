This project is, basically, a scalable object store + load balancer.

![High level design](high_level_design.svg)

# Goals
**Core functional Requirements**
1. Users should be able to upload a file from any device
2. Users should be able to download a file from any device
3. Users should be able to share a file with other users and view the files shared with them
4. Users can automatically sync files across devices

**Core non-functional requirements**
1. The system should be highly available (prioritizing availability over consistency).
2. The system should support files as large as 50GB.
3. The system should be secure and reliable. We should be able to recover files if they are lost or corrupted.
4. The system should make upload, download, and sync times as fast as possible (low latency).

# Elements 
We have User, Device and Files.

**User**
* Each User will have login credentials.
* Users can login easily from multiple devices (SSO is a must)
* Users can upload files, view what they have uploaded, delete files.

**Device**
Devices should be transparent to User - ie, regardless of what Device it should work exactly same, with minimum friction.

**File**
Files are just bytes with metadata. The client is responsible for understanding the meta data to be able to display it reasonably (text, picture, pdfs ect)


# Client 
A Qt based client, recompile for each client but will work same. 

**SSO** needs to use google sign in SDK for android, iOS and OAuth2 vis google in desktop.

# Backend
Client connect, after SSO, to load balancers which distribute to backend server pod running accross nodes. Backend manages storing to object-store and DB.

* Load Balancer
* Server 
* Postgres DB
* Object Store (Just node NFS storage)

## API

![API design](api_design.svg)

Our api mimicks exisiting apis for cloud services like Azure Blob. Annother option is have the client use sftp, but that doesn't scale.

All APIs have a bearer token header for auth and identifying the user.

```http
Authorization: Bearer <token>
```

### Nouns And Verbs

| Noun | Verbs |
| --- | --- |
| `file` | `GET`, `POST`, `PUT`, `HEAD`, `PATCH`, `DELETE` |
| `files` | `GET`, `POST`, `DELETE` |
| `user` | `GET`, `POST`, `DELETE` |

### Endpoint Summary

| Method | Path | Args | Success | Errors | Description |
| --- | --- | --- | --- | --- | --- |
| `GET` | `/files` | `folderpath` | `200 OK` | `403`, `404` | Return a nested JSON array of files and folders, not complete file bytes. |
| `HEAD` | `/file` | `filepath` | `200 OK` | `403`, `404` | Return file metadata. |
| `GET` | `/file` | `filepath` | `200 OK` | `403`, `404` | Return file bytes. |
| `POST` | `/files` | `folderpath` | `200 OK`, `201 already exist` | `403` | Create a folder in NFS. |
| `POST` | `/file` | `filepath` | `200 OK`, `201 already exist` | `403` | Upload a file or initiate a resumable upload. |
| `PUT` | `/file` | `uploadType`, `upload_id` | `200 OK`, `308 Resume Incomplete` | `403`, `404` | Upload bytes to a resumable upload session. |
| `PATCH` | `/file` | `filepath` | `200 OK` | `403`, `404` | Patch file metadata. |
| `DELETE` | `/files` | `folderpath` | `200 OK` | `403`, `404` | Delete an entire folder. |
| `DELETE` | `/file` | `filepath` | `200 OK` | `403`, `404` | Delete a file. |
| `GET` | `/user/<user-email>` | `user-email` | `200 OK` | `404` | Return user details. |
| `POST` | `/user` | none | `200 OK` | `403` | Create a user in DB and assign the user to a pod. |
| `DELETE` | `/user/<user-email>` | `user-email` | `200 OK` | `403`, `404` | Delete a user in DB and remove the user from NFS. |
| `POST` | `/user/share?<user-email>` | `user-email` | `200 OK`, `207 partial failure` | `404 user NF` | Share folders with other users. |

### List Folder

```http
GET /files?folderpath=<folderpath>
```

Returns `200 OK`, `403 forbidden`, or `404 not found`.

The response is a nested JSON array of files and folders, not the complete files.

### Get File Metadata

```http
HEAD /file?filepath=<filepath>
```

Returns `200 OK`, `403 forbidden`, or `404 not found`.

The response returns file metadata.

### Download File

```http
GET /file?filepath=<filepath>
```

Returns `200 OK`, `403 forbidden`, or `404 not found`.

The response returns file bytes.

Since files can be huge, `GET /file` only returns file bytes, so it must be paired with a `HEAD /file` request first. This API may also return redirects, so curl must be done with `-L`.

Example usage:

```bash
curl -L -C - -o bigfile.zip \
  --progress-bar --connect-timeout 10 --max-time 3600 --retry 3 --retry-delay 5 \
  "https://example.com/file?path"
```

### Create Folder

```http
POST /files?folderpath=<folderpath>
```

Returns `200 OK`, `201 already exist`, or `403 forbidden`.

This API creates a folder in NFS.

### Upload File

```http
POST /file?filepath=<filepath>
```

Headers:

| Header | Description |
| --- | --- |
| `Content-Type` | File content type. |
| file metadata headers | File metadata used by the server. |

Returns `200 OK`, `201 already exist`, or `403 forbidden`.

If it is a small file, less than 5MB, use a single POST upload:

```bash
curl -X POST "https://example.com/file?<filepath>" \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: image/jpeg" \
  --data-binary @photo.jpg
```

If the file is large, clients should use resumable uploads.

#### Resumable Uploads

Step 1: initiate a session and get the session URI back in the `Location` header.

```bash
curl -i -X POST "https://www.example.com/file?uploadType=resumable&&filepath=<path>" \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json; charset=UTF-8" \
  -H "X-Upload-Content-Type: video/mp4" \
  -H "X-Upload-Content-Length: 2000000" \
  -d '{"name":"big_video.mp4"}'
```

This returns an upload ID.

Step 2a: upload the whole file in one shot.

```bash
curl -X PUT "https://www.example.com/file?uploadType=resumable&upload_id=xyz123" \
  -H "Content-Length: 2000000" \
  --data-binary @big_video.mp4
```

Step 2b: or upload in 256KB-aligned chunks for progress bars and flaky links.

```bash
curl -X PUT "https://www.example.com/file?uploadType=resumable&upload_id=xyz123" \
  -H "Content-Length: 262144" \
  -H "Content-Range: bytes 0-262143/2000000" \
  --data-binary @chunk1.bin
```

A dropped chunk returns `308 Resume Incomplete`. Query the same URI with `Content-Range: bytes */2000000` to find how many bytes the server already has.

### Patch File Metadata

```http
PATCH /file?filepath=<filepath>
```

Headers:

| Header | Description |
| --- | --- |
| `Content-Type` | Metadata request body content type. |
| file metadata headers | Metadata fields to patch. |

Returns `200 OK`, `403 forbidden`, or `404 NF`.

Patches file metadata.

### Delete Folder

```http
DELETE /files?folderpath=<folderpath>
```

Returns `200 OK`, `403 forbidden`, or `404 NF`.

Deletes the entire folder.

### Delete File

```http
DELETE /file?filepath=<filepath>
```

Returns `200 OK`, `403 forbidden`, or `404 NF`.

Deletes the file.

### Get User

```http
GET /user/<user-email>
```

Returns `200 OK` or `404 NF`.

Response data is JSON user details.

### Create User

```http
POST /user
```

Returns JSON user details.

Creates the user in DB and assigns the user to a pod.

### Delete User

```http
DELETE /user/<user-email>
```

Returns `200 OK`, `403 forbidden`, or `404 NF`.

Deletes the user in DB and removes the user from NFS.

### Share Folders

```http
POST /user/share?<user-email>
```

Args:

| Arg | Description |
| --- | --- |
| `user-email` | Source user email. |

Request data:

```json
[
  {
    "email": "target@example.com",
    "folderpath": "/photos"
  }
]
```

Returns `200 OK`, `404 user NF`, or `207 partial failure`.

For partial failures, the response is JSON shaped like:

```json
[
  {
    "email": "target@example.com",
    "folderpath": "/photos",
    "reject_reason": "user not found"
  }
]
```


## Database 
Have just two tables - **User** and **Files**

**User**
The user table contains details about users. 

```
CREATE TABLE USERS(
  id      BIGSERIAL PRIMARY KEY,
  name    TEXT NOT NULL,
  email   VARCHAR(255) UNIQUE
  volume  VARCHAR(255) UNIQUE #name of volume where user folder is created
)
```

**Files** 
The files table contains meta-data for all files, with users as key

```
CREATE TABLE FILES(
  id          BIGSERIAL PRIMARY KEY,
  user_id     BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  name        TEXT NOT NULL,
  size        INTEGER NOT NULL,
  type        VARCHAR(255) NOT NULL,
  created_at  TIMESTAMP,  
  updated_at  TIMESTAMP,
  permission  VARCHAR(255) NOT NULL
)

CREATE INDEX idx_files_users ON files(user_id);
```