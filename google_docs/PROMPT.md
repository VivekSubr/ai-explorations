Generate a MVP of *google docs*, in golang, deploying into kubernetes. 

** Core functional Requirements **
1. Users should be able to create new documents.
2. Multiple users should be able to edit the same document concurrently.
3. Users should be able to view each other's changes in real-time.
4. Users should be able to see the cursor position and presence of other users.

** Core non-functional requirements **
1. Documents should be eventually consistent (i.e. all users should eventually see the same document state).

2. Updates should be low latency (< 100ms).
3. The system should scale to millions of concurrent users across billions of documents.
4. No more than 100 concurrent editors per document.
5. Documents should be durable and available even if the server restarts.

** Core Entities **
1. Editor (user)
2. Document 
3. Edit
4. Cursor

** High level design **

1. Users should be able to create new documents.
POST /doc?name is required, should return an id.
Corresponding, GET /doc?id or /doc?name apis also needed.

2. Multiple users should be able to edit the same document concurrently.
Each edit done by user should be sent to all viewing the document. 

POST /edit?doc_name -d edit_data

The Edit data struct should be a 'Conflict Free Data Type', CRDT (https://en.wikipedia.org/wiki/Conflict-free_replicated_data_type)

3. Users should be able to view each other's changes in real-time.
Each edit should go to backend and come to every client as a Server Side Event

4. Users should be able to see the cursor position and presence of other users.
Something along the lines of https://www.canva.dev/blog/engineering/realtime-mouse-pointers/