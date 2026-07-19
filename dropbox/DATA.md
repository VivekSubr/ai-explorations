A distributed system must consider the following things with it's data:

**Availability & partition tolerance**

**Replication**

**Consistency and Integrity**

**Durability**
Once a write is acknowledged to the client, it survives any subsequent failure — crash, power loss, process kill. The data won't disappear or roll back.

**Backup & recovery**


# This project 
We prioritize availiability over strict consistency. We need data to be durable and replicated.

## Durability 
Design splits data to *metadata*, living in postgres and *object bytes* stored in the NFS mounts. Postgres cluster takes care of it's own durability, we just need to enable WAL and backups.

For object bytes durability, the server pod should store file checksum as part of metadata as well, eg:
```
CREATE TABLE FILES(
  id          BIGSERIAL PRIMARY KEY,
  user_id     BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  name        TEXT NOT NULL,
  size        INTEGER NOT NULL,
  type        VARCHAR(255) NOT NULL,
  checksum    BIGINT NOT NULL,
  created_at  TIMESTAMP,  
  updated_at  TIMESTAMP,
  permission  VARCHAR(255) NOT NULL
)
```

And only update this after all file chunks are fully written. Bad Checksum should result in file delete and removal from DB as well.

## Replication 
The NFS backend should handle replication. 

## Access Control
We need finer access control on folders, since dropbox also has a *share api*... so, users should not be able to share unconditionally.

Access control is folder level, so we need a table for folders.
```
CREATE TABLE FOLDERS (
  id          BIGSERIAL PRIMARY KEY,
  user_id     BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  parent_id   BIGINT REFERENCES folders(id) ON DELETE CASCADE,
  name        TEXT NOT NULL,
  path        TEXT NOT NULL,
  created_at  TIMESTAMP,
  updated_at  TIMESTAMP,
  UNIQUE(user_id, path)
);

ALTER TABLE FILES
  ADD COLUMN folder_id BIGINT REFERENCES folders(id) ON DELETE CASCADE;
```

and a table for ACLs
```
CREATE TABLE FOLDER_ACL (
  id                 BIGSERIAL PRIMARY KEY,
  folder_id          BIGINT NOT NULL REFERENCES folders(id) ON DELETE CASCADE,
  principal_user_id  BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  role               VARCHAR(32) NOT NULL, -- viewer | editor | owner
  can_share          BOOLEAN NOT NULL DEFAULT false,
  created_by         BIGINT NOT NULL REFERENCES users(id),
  created_at         TIMESTAMP,
  expires_at         TIMESTAMP,
  revoked_at         TIMESTAMP,
  UNIQUE(folder_id, principal_user_id)
);

CREATE INDEX idx_folder_acl_principal ON folder_acl(principal_user_id);
CREATE INDEX idx_folder_acl_folder ON folder_acl(folder_id);
```