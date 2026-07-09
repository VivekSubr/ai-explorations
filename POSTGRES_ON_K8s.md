# Postgres on Kubernetes 
We use the CNPG operator resource manage postgres for us, only deploy the operator 'cluster' resource.

## Manifest 

# Example CloudNativePG deployment
# Assumes the CNPG operator is already installed:
#   helm install cnpg --namespace cnpg-system --create-namespace \
#     cnpg/cloudnative-pg

---
apiVersion: v1
kind: Namespace
metadata:
  name: postgres

---
# Superuser credentials (CNPG also auto-generates an 'app' user secret)
apiVersion: v1
kind: Secret
metadata:
  name: pg-superuser-secret
  namespace: postgres
type: kubernetes.io/basic-auth
stringData:
  username: postgres
  password: CHANGE_ME

---
# Credentials for backup target (Azure Blob Storage here)
apiVersion: v1
kind: Secret
metadata:
  name: azure-blob-creds
  namespace: postgres
type: Opaque
stringData:
  AZURE_STORAGE_ACCOUNT: mystorageaccount
  AZURE_STORAGE_KEY: CHANGE_ME

---
apiVersion: postgresql.cnpg.io/v1
kind: Cluster
metadata:
  name: pg-cluster
  namespace: postgres
spec:
  instances: 3
  imageName: ghcr.io/cloudnative-pg/postgresql:16.4

  superuserSecret:
    name: pg-superuser-secret

  storage:
    size: 50Gi
    storageClass: managed-premium

  resources:
    requests:
      cpu: "1"
      memory: 2Gi
    limits:
      cpu: "2"
      memory: 4Gi

  # Spread replicas across zones instead of piling them on one node
  affinity:
    topologySpreadConstraints:
      - maxSkew: 1
        topologyKey: topology.kubernetes.io/zone
        whenUnsatisfiable: DoNotSchedule
        labelSelector:
          matchLabels:
            cnpg.io/cluster: pg-cluster

  postgresql:
    parameters:
      max_connections: "200"
      shared_buffers: "512MB"
      effective_cache_size: "1536MB"

  monitoring:
    enablePodMonitor: true

  # If this namespace is inside the mesh, keep the DB port out of mTLS
  # interception rather than fighting sidecar/probe issues:
  # template:
  #   metadata:
  #     annotations:
  #       traffic.sidecar.istio.io/excludeInboundPorts: "5432"

  backup:
    barmanObjectStore:
      destinationPath: "https://mystorageaccount.blob.core.windows.net/pg-backups"
      azureCredentials:
        storageAccount:
          name: azure-blob-creds
          key: AZURE_STORAGE_ACCOUNT
        storageKey:
          name: azure-blob-creds
          key: AZURE_STORAGE_KEY
      wal:
        compression: gzip
      data:
        compression: gzip
    retentionPolicy: "30d"

---
apiVersion: postgresql.cnpg.io/v1
kind: ScheduledBackup
metadata:
  name: pg-cluster-daily-backup
  namespace: postgres
spec:
  schedule: "0 0 2 * * *"
  backupOwnerReference: self
  cluster:
    name: pg-cluster

---
apiVersion: postgresql.cnpg.io/v1
kind: Pooler
metadata:
  name: pg-cluster-pooler
  namespace: postgres
spec:
  cluster:
    name: pg-cluster
  instances: 2
  type: rw
  pgbouncer:
    poolMode: transaction
    parameters:
      max_client_conn: "1000"
      default_pool_size: "25"