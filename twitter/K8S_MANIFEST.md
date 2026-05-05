# Manifest 
This document lists **all** kubernetes resources for this project, organized by area.

## Gateway
* **Deployment** of ngnix
* **Gateway** 
* **Service**

## Backend 
We have 4 services running - timeline, fanout, notification and search. Each of them will have their own:
* **Deployment**
* **Service**
* **HTTP Route**

## Database 
Have Postgres and redis.
* **Redis StatefulSet**
* **Redis Service**
* **Postgres CNPG Cluster**
* **Postgres Service** 

## Observability 
Have Loki and Mimir deployed.
* **Loki StatefulSet**
* **Loki Service**
* **Mimir StatefulSet**
* **Mimir Service**