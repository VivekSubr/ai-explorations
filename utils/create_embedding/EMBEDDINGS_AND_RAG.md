# Vector Embeddings
Vector embeddings are how LLMs represent any data, like text, pics ect. The key operation done on vectors by LLMs is **similarity**.

Consider this example,
```
    "king"    → [0.2, 0.5, -0.1, 0.8, ...]  
    "queen"   → [0.3, 0.4, -0.2, 0.7, ...]  (close to "king")
    "car"     → [-0.5, 0.1, 0.9, -0.3, ...]  (far from "king")


    cosine_similarity("king", "queen") → high (similar)
    cosine_similarity("king", "car") → low (different)
```

Vector operations also encode **semantic relationships**, eg: 
``` vector("king") - vector("man") + vector("woman") ≈ vector("queen") ```

## pgvector
Vectors are unique form of data, in that they are *high dimensioned*. Compare with standard SQL data - a table will typically have a few indices/dimensions, but vector operations need to indice every element in it to do operations affectively... hence entire length of vector is it's dimentsion. 

In SQL terms, vectors are array, where each element is indexed by it's position.

Queried a vector DB is typically done for similarity, hence the data storage needs to be organized by k-NN (k Nearest Neighbor)... vector DBs guarantee getting all nearest neighbors of an element in O log(n)

pgvector adds the 'vector' datatype, and 'cosine similarity' operator <=> to postgres, eg:

```
CREATE EXTENSION vector;

CREATE TABLE documents (
    id        SERIAL PRIMARY KEY,
    content   TEXT,
    embedding vector(1536)   -- dimension must match your model
);

INSERT INTO documents (content, embedding)
VALUES ('Kubernetes uses etcd for state', '[0.12, -0.34, ...]');

SELECT content, 1 - (embedding <=> '[0.11, -0.32, ...]') AS similarity
FROM documents
ORDER BY embedding <=> query_vector   -- <=> = cosine distance
LIMIT 5;
```

## Debugging embeddings 



# Retrieval Augmented Generation (RAG)
RAG is a way to inject context into an LLM. Note that LLMs *do not* understand vector embeddings natively, the vectorDB provides a way to get similar* data to query and inject to LLM context as plain-text.

## mcp.py
The mcp server exposes three tools:
   * list_embedding_sources to see available paths.
   * get_embedding_context for exact path context.
   * semantic_search_embedding_context for conceptual questions.

list and get are basically SELECT queries to check the data.

**semantic_search_embedding_context** is the interesting operation. It takes the user input, converts it to a vector embedding, queries similarity (<=>) to find if there is any data in DB that is relevant, and if there is, fetches and returns it to context.