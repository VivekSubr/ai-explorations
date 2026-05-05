# Test Plan
Each component is responsible for having a 'make test' target that executes unit tests with 90+ % coverage.

We also have minikube based test environent for bringing up the actual components and running integration tests, as described below.

## Create k8s full deploy script
Create test folder. Create a bash script, *k8s_full_bring_up.sh*, in test folder... this script should 
1. Go through backend, database and observability folders and build all 'make docker targets' and generate all docker images.
2. Bring up minikube test enviroment, ```minikube start --driver=docker ```
3. Bring up all resources in K8S_MANIFEST using generated images.

## Integration Tests
