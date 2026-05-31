# Postgres

Whatever is using create_embeddings.py is expected to initialize postgres before calling it, eg:
```
SELECT 'CREATE DATABASE ' || quote_ident(:'db')
WHERE NOT EXISTS (SELECT 1 FROM pg_database WHERE datname = :'db')\gexec
```

## Suggested DB structure 
```
CREATE TABLE IF NOT EXISTS <name> (
    id bigserial PRIMARY KEY,
    name text NOT NULL, #name of the source
    path text NOT NULL UNIQUE, #path to the source
    kind text NOT NULL, #what is the source?
    embedding vector(1536) NOT NULL, #Dimensions should match with embedding model
);
```