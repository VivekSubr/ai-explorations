This project is, basically, a scalable object store + load balancer.

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
Our api mimicks exisiting apis for cloud services like Azure Blob. Annother option is have the client use sftp, but that doesn't scale.

First, list out the **nouns** and what **verbs** act on them.
* file - get, post, put, head, delete
* user - get, post, put, delete

All these apis will have bearer token header for auth and identifying user.

* GET /files
  args:   folderpath
  return: 200 OK, 403 forbidden, 404 not found
          nested json array of files and folders (NOT the complete files)

* HEAD /file
  args:   filepath
  return: 200 OK, 403 forbidden, 404 not found
          returns json of file metadata

* GET /file
  args:   filepath
  return: 200 OK, 403 forbidden, 404 not found
          returns file bytes

  Since files can be huge, made GET only return file bytes, so it must be paired with a HEAD first. This api also may return *redirects*, so curl must be done with -L. Example usage:
  ```
    curl -L -C - -o bigfile.zip \
        --progress-bar --connect-timeout 10 --max-time 3600 --retry 3 --retry-delay 5 \
        "https://example.com/file?path"
  ```

* POST /files
  args:   folderpath
  return: 200 OK, 201 already exist, 403 forbidden
          This api creates folder in NFS.

* POST /file 
  args:    filepath
  headers: file metadata, content-type header 
  return:  200 OK, 201 already exist, 403 forbidden
           
  If it's a small file, <5MB, single post upload, something like this 
  ```
  curl -X POST "https://example.com/file?<filepath>" \
    -H "Authorization: Bearer $TOKEN" -H "Content-Type: image/jpeg" \
    --data-binary @photo.jpg
  ```

  If the file is large, clients should use *resumable uploads*

  ```
  # Step 1: initiate session, get the session URI back in the Location header
  curl -i -X POST "https://www.example.com/file?uploadType=resumable&&filepath=<path>" \
    -H "Authorization: Bearer $TOKEN" \
    -H "Content-Type: application/json; charset=UTF-8" \
    -H "X-Upload-Content-Type: video/mp4" \
    -H "X-Upload-Content-Length: 2000000" \
    -d '{"name":"big_video.mp4"}'
  # this will return an upload id

  # Step 2a: upload the whole thing in one shot
  curl -X PUT "https://www.example.com/file?uploadType=resumable&upload_id=xyz123" \
    -H "Content-Length: 2000000" \
    --data-binary @big_video.mp4

  # Step 2b: OR upload in 256KB-aligned chunks (for progress bars / flaky links)
  curl -X PUT "https://www.example.com/file?uploadType=resumable&upload_id=xyz123" \
    -H "Content-Length: 262144" \
    -H "Content-Range: bytes 0-262143/2000000" \
    --data-binary @chunk1.bin
  # a dropped chunk returns 308 Resume Incomplete; query the same URI with
  # Content-Range: bytes */2000000 to find how many bytes the server already has
  ```

* PATCH /file
  args:    filepath
  headers: file metadata, content-type header 
  return:  200 OK, 403 forbidden, 404 NF

  Patches file meta-data

* DELETE /files
  args:   folderpath
  return: 200 OK, 403 forbidden, 404 NF

  Delete entire folder

* DELETE /file
  args:   filepath
  return: 200 OK, 403 forbidden, 404 NF

  Delete file.

* GET /user<user-email>
  return: 200 OK, 404 NF
  data: json of user details

* POST /user
  return: json of user details

  results in user being created in DB and assigned to pod.

* DELETE /user<user-email>
  return: 200 OK, 403 forbidden, 404 NF

  results in user being deleted in DB and removed from NFS.

## Database 
Have just two tables - **User** and **Files**

**User**
The user table contains details about users. 

CREATE TABLE USERS(
  id      BIGSERIAL PRIMARY KEY,
  name    TEXT NOT NULL,
  email   VARCHAR(255) UNIQUE
  volume  VARCHAR(255) UNIQUE #name of volume where user folder is created
)

**Files** 
The files table contains meta-data for all files, with users as key

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