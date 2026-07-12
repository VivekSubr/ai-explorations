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