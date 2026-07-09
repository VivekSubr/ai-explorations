# Client Spec

This document specifies the Dropbox-like client described in `HLD.md`. The client is a cross-platform Qt application that lets users sign in, browse folders, upload and download large files, delete files and folders, edit file metadata, and keep a local folder synced across devices.

## Goals

**Functional goals**
1. Users can sign in from desktop, Android, and iOS with Google SSO.
2. Users can upload files from any supported device.
3. Users can download files from any supported device.
4. Users can browse their folders and file metadata without downloading file bytes.
5. Users can delete files and folders.
6. Users can automatically sync local files across devices.
7. Users can eventually share files with other users and view shared files.

**Non-functional goals**
1. Support files up to 50GB.
2. Prefer availability and forward progress over strict consistency.
3. Keep uploads, downloads, and sync low-latency where the network allows.
4. Recover cleanly from process restarts, dropped connections, and device sleep.
5. Keep user tokens and local file data secure.

## Platforms

The client is implemented with Qt and rebuilt per platform.

| Platform | UI/runtime | Sign-in mechanism |
| --- | --- | --- |
| Windows/Linux/macOS | Qt desktop application | Google OAuth2 browser flow |
| Android | Qt Android application | Google Sign-In SDK |
| iOS | Qt iOS application | Google Sign-In SDK |

Platform-specific code should be isolated behind narrow interfaces for authentication, secure token storage, filesystem watching, and OS notifications.

## User Experience

The client has these primary surfaces:

1. **Sign in**: starts Google SSO and stores the returned access token/refresh token securely.
2. **File browser**: lists folders and files using metadata only.
3. **Upload queue**: shows active uploads, progress, retries, and paused/resumable state.
4. **Download queue**: shows active downloads, progress, retries, and local target path.
5. **Sync status**: shows whether the local sync folder is up to date, syncing, paused, offline, or conflicted.
6. **Metadata/details panel**: shows name, size, content type, updated time, permissions, and any client-display metadata.
7. **Settings**: account, sync folder, bandwidth limits, retry policy, sign out, and cache cleanup.

The client should never require the user to understand the backend storage layout. Files and folders should behave the same across devices.

## Client Components

```text
Qt UI
	-> Auth Manager
	-> API Client
	-> Transfer Manager
	-> Sync Engine
	-> Local Metadata Store
	-> Secure Token Store
	-> Filesystem Adapter
```

**Auth Manager**
Handles Google SSO, token refresh, sign out, and bearer-token injection for API calls.

**API Client**
Wraps the object-store HTTP API. It owns request construction, response parsing, redirects, retries, timeouts, and API errors.

**Transfer Manager**
Owns upload/download jobs, progress reporting, chunking, resume state, cancellation, and retry backoff.

**Sync Engine**
Watches local filesystem changes, compares them with remote metadata, schedules upload/download/delete operations, and resolves conflicts.

**Local Metadata Store**
Persists file metadata, sync cursor/state, upload session IDs, local checksums if available, and conflict records. SQLite is a good default for desktop and mobile.

**Secure Token Store**
Stores OAuth tokens using the platform keychain/keystore when available.

**Filesystem Adapter**
Provides platform-specific file watching, atomic file writes, temp files, and path normalization.

## Authentication

All API requests include:

```http
Authorization: Bearer <token>
```

Desktop clients use Google OAuth2 via browser redirect or device authorization flow. Android and iOS clients use the Google Sign-In SDK. The client exchanges or receives tokens according to the backend authentication contract, then stores tokens in the secure token store.

Auth behavior:
1. Refresh tokens before expiry when possible.
2. On `403`, attempt one token refresh if the token may be expired.
3. If refresh fails, mark the account signed out and ask the user to sign in again.
4. Do not log bearer tokens, refresh tokens, or full signed URLs.

## API Usage

The client uses the backend API described in `HLD.md` and `openapi/openapi.yaml`.

### List Folder

```http
GET /files?folderpath=/path/to/folder
Authorization: Bearer <token>
```

Expected responses:
1. `200 OK`: nested JSON array of files and folders.
2. `403 Forbidden`: auth failed or user is not allowed to read the folder.
3. `404 Not Found`: folder does not exist.

Client behavior:
1. Render metadata from the JSON response without downloading file bytes.
2. Cache the folder listing in the local metadata store.
3. Preserve the current cached view if a refresh fails while offline.

### Get File Metadata

```http
HEAD /file?filepath=/path/to/file
Authorization: Bearer <token>
```

Expected responses:
1. `200 OK`: file metadata in headers.
2. `403 Forbidden`: auth failed or user is not allowed to read the file.
3. `404 Not Found`: file does not exist.

Client behavior:
1. Issue `HEAD` before large downloads.
2. Use size, content type, ETag, and modified time to decide whether local data is current.
3. Store metadata locally for browsing and sync comparisons.

### Download File

```http
GET /file?filepath=/path/to/file
Authorization: Bearer <token>
```

Client behavior:
1. Follow redirects.
2. Download to a temporary file beside the final target when possible.
3. Resume interrupted downloads using byte ranges when the server supports them.
4. Atomically rename the temporary file after the download completes and metadata checks pass.
5. Retry transient network failures with exponential backoff.
6. Surface permanent `403` and `404` failures to the user.

### Create Folder

```http
POST /files?folderpath=/path/to/folder
Authorization: Bearer <token>
```

Expected responses:
1. `200 OK`: folder created.
2. `201 Created`: folder already exists, treated as success for idempotent sync.
3. `403 Forbidden`: auth failed or user is not allowed to create the folder.

Client behavior:
1. Treat existing folders as successful during sync.
2. Refresh the parent folder listing after a successful create.

### Upload Small File

Files smaller than 5MB use a single request.

```http
POST /file?filepath=/path/to/file
Authorization: Bearer <token>
Content-Type: <file media type>

<file bytes>
```

Expected responses:
1. `200 OK`: upload succeeded.
2. `201 Created`: file already exists, handled according to conflict policy.
3. `403 Forbidden`: auth failed or user is not allowed to upload the file.

Client behavior:
1. Infer `Content-Type` from the OS or file extension, defaulting to `application/octet-stream`.
2. Save remote metadata after success.
3. If the file already exists and differs from the local file, create a conflict copy or prompt according to sync settings.

### Upload Large File

Files 5MB or larger use resumable upload.

Step 1: initiate a session.

```http
POST /file?uploadType=resumable&filepath=/path/to/file
Authorization: Bearer <token>
Content-Type: application/json; charset=UTF-8
X-Upload-Content-Type: <file media type>
X-Upload-Content-Length: <total bytes>

{"name":"filename.ext"}
```

Step 2: upload the whole file or upload chunks.

```http
PUT /file?uploadType=resumable&upload_id=<upload id>
Authorization: Bearer <token>
Content-Length: <chunk bytes>
Content-Range: bytes <start>-<end>/<total bytes>

<chunk bytes>
```

Client behavior:
1. Use 256KB-aligned chunks for resumable chunk uploads.
2. Persist upload session ID, file path, file size, chunk size, and last confirmed byte range.
3. On process restart, resume the session if it has not expired.
4. On dropped chunks or `308 Resume Incomplete`, query progress with `Content-Range: bytes */<total>`.
5. Retry transient errors with exponential backoff and jitter.
6. Do not restart a 50GB upload from byte zero unless the session is invalid or the file changed locally.

### Patch File Metadata

```http
PATCH /file?filepath=/path/to/file
Authorization: Bearer <token>
Content-Type: application/json

{"customMetadata":{"key":"value"}}
```

Client behavior:
1. Use metadata patches for client-display metadata only.
2. Refresh local metadata after a successful patch.
3. Treat `404` as a remote delete during sync reconciliation.

### Delete Folder

```http
DELETE /files?folderpath=/path/to/folder
Authorization: Bearer <token>
```

Client behavior:
1. Confirm destructive folder deletes in the UI unless the delete was generated by sync policy.
2. Remove local cached metadata after success.
3. For sync deletes, preserve recoverability through local trash/history when platform support exists.

### Delete File

```http
DELETE /file?filepath=/path/to/file
Authorization: Bearer <token>
```

Client behavior:
1. Confirm destructive manual deletes in the UI.
2. Treat successful remote delete as a sync event and update local metadata.
3. Treat `404` as already deleted for idempotent sync operations.

## Transfer Policy

**Upload threshold**: files under 5MB use single-request upload; files 5MB and larger use resumable upload.

**Chunk size**: default to 256KB-aligned chunks. The implementation may use larger multiples such as 4MB or 8MB for high-throughput networks, while preserving 256KB alignment.

**Retries**:
1. Retry network timeouts, connection resets, HTTP `408`, `429`, and `5xx` responses.
2. Do not retry permanent `403` unless a token refresh succeeds.
3. Treat `404` on upload session as session expired and restart session creation.
4. Use exponential backoff with jitter and a maximum retry window visible to the user.

**Timeouts**:
1. Use a short connect timeout.
2. Use longer transfer timeouts for large files.
3. Pause transfers when the OS reports no network connectivity.

**Progress**:
1. Show per-file progress, total queue progress, speed, and estimated time remaining.
2. Persist progress often enough that app restarts lose at most one chunk of work.

## Sync Engine

The sync engine keeps a configured local folder aligned with remote state.

Inputs:
1. Local filesystem watcher events.
2. Periodic remote folder listing refreshes.
3. Completed upload/download/delete operations.
4. User actions from the file browser.

Sync loop:
1. Scan local changes and normalize paths.
2. Fetch remote metadata for affected folders/files.
3. Compare local metadata, remote metadata, and the last synced state.
4. Schedule uploads, downloads, deletes, or conflict handling.
5. Persist the new sync state after each successful operation.

Conflict policy:
1. If local and remote both changed since the last synced state, keep both versions.
2. Rename the local conflicting file with device name and timestamp.
3. Upload the conflict copy after the original remote path is preserved.
4. Show conflicts in the sync status surface.

Consistency policy:
1. The UI may show cached metadata while offline.
2. Remote metadata refresh is eventually consistent.
3. Sync operations should be idempotent where possible.

## Local Metadata

The client stores local state so it can browse quickly and resume work after restart.

Recommended tables/records:
1. `account`: user identity, account ID, display email, token storage reference.
2. `remote_entry`: path, type, size, content type, ETag, created time, updated time, permissions.
3. `local_entry`: path, size, modified time, optional checksum, sync status.
4. `sync_state`: path, last local version, last remote version, last successful sync time.
5. `transfer`: job ID, type, path, status, bytes completed, total bytes, retry count.
6. `upload_session`: upload ID, path, total bytes, chunk size, last confirmed byte, expiration time.
7. `conflict`: path, conflict copy path, reason, created time, resolved flag.

## Security

1. Store tokens in the platform secure store, not plain text config files.
2. Redact tokens, signed URLs, and file contents from logs.
3. Use HTTPS for all backend calls.
4. Validate local paths to prevent path traversal outside the configured sync folder.
5. Use atomic writes for downloaded files to avoid exposing partial data as complete files.
6. Clear upload session state and cached metadata on sign out unless the user chooses to keep local files.

## Error Handling

| Error | Client behavior |
| --- | --- |
| `403 Forbidden` | Refresh token once if appropriate; otherwise require sign-in or show permission error. |
| `404 Not Found` | Mark remote file/folder missing; reconcile as delete during sync. |
| `308 Resume Incomplete` | Persist accepted byte range and continue upload from the next byte. |
| Redirect on download | Follow `Location` and continue progress tracking. |
| Network offline | Pause transfers and resume when connectivity returns. |
| Local file changed during upload | Cancel current upload session and reschedule with the new file version. |
| Disk full | Pause affected downloads/uploads and show actionable error. |

## Observability

The client should log structured events without secrets:
1. Sign-in success/failure.
2. API request type, status code, duration, and retry count.
3. Transfer start, progress checkpoint, pause, resume, success, and failure.
4. Sync scan start/end and number of changes detected.
5. Conflict creation and resolution.

Useful metrics:
1. Upload/download throughput.
2. Transfer failure rate by status code.
3. Average sync delay.
4. Bytes resumed versus bytes restarted.

## Testing

Minimum test coverage:
1. Auth token injection and refresh-once behavior.
2. Folder listing parsing.
3. HEAD metadata parsing.
4. Small upload request creation.
5. Resumable upload session creation.
6. Chunk upload, `308 Resume Incomplete`, and resume query behavior.
7. Download redirect handling.
8. Range-based download resume.
9. Sync conflict detection.
10. Local path normalization and path traversal rejection.
11. Delete idempotency for sync-generated deletes.

Integration test scenarios:
1. Upload a small file, list the folder, download it, then delete it.
2. Upload a large file, interrupt mid-transfer, restart the client, and resume.
3. Download a large file through a redirect and resume after interruption.
4. Modify the same file locally and remotely, then verify conflict copy creation.
5. Sign out and verify tokens are cleared.

## API Gaps And Open Questions

The HLD includes sharing as a core functional goal, but the current API only covers folders, files, metadata, upload, download, and delete. Sharing needs additional backend endpoints before the client can implement it.

Open questions:
1. What API creates, lists, accepts, rejects, and revokes shared-file permissions?
2. Does the backend provide checksums for upload/download integrity verification?
3. Does `HEAD /file` return metadata in headers or a JSON body despite being HEAD?
4. Should `201` mean "already exists" for both folders and files, or should that be `409 Conflict` in the final API?
5. Does the backend expose a remote change feed, or should sync poll folder listings?
6. What are upload session expiration rules?
7. Are file permissions per user, per path, or per file ID?
