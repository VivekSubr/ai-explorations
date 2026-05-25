# Retrieval Augmented Generation (RAG)



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