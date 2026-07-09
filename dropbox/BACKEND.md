# Backend
We have the following services running 

**Gateway**
Nginx running that maps requests to instances of server pod using Consistent Hashing of their ID tokens.

Load balances the first request based on least requests.

**Server**
Sever will serves incoming requests, implements the api. It interacts with the object store, and will mount dynamically the folder paths of users it is server. It also needs to keep database up to date.

**Postgres DB**
Database instance.

## Object Store
The object store is basically just NFS mounted on the nodes. Each user will have it's own root folder created, and all their operations will happen within that folder (For user, /<user-name> will be / in their filepaths)

The cluster will have a pool of PersistentVolumes. Each server mounts a sub-set of PVCs, and creates user folders in them as required.

## K8s manifest 
**Gateway**
1. Daemonset, instance one running per node.
2. Service - exposing external ip

**Server**
1. Deployment - manage topological spread across nodes. 
2. Service - ClusterIP only

**Object Store**
1. csi-nfs deployment
2. StorageClass for NFS
3. A number of PersistentVolumes
4. PVCs created dynamically to bind to server pods 

**Database**
1. CNPG cluster resource


## Workflows
**Create User**
1. Lands on gateway which load balances and sends to least loaded server pod. 
2. Server pod creates entry in DB and creates user folder in NFS.

**Upload file**
1. Gateway CH to correct server pod.
2. Server pod updates files table and fsyncs to NFS under user folder.

**Download file**

**Delete file**