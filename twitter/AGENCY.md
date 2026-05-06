# Goals
This project aims to showcase a twitter clone generated with AI. As such, only the markdown files are checked in. The files are as follows

* **HLD.md**
The high level requirements and design.

* **BACKEND.md**
Details relating to backend code.

* **FRONTEND.md**
Detailing api calls from frontend on workflows.

* **DATABASE.md** 
Details relating to database design.

* **OBSERVABILITY.md**
Details relating to observability strategy.

* **SCALE.md**
Scaling calculations, and design relating to auto-scaling

* **K8S_MANIFEST.md**
Fully lists all kubernetes resources expected to be generated.

* **TEST_PLAN.md**
Instructions on how to test and qualify the final project.

## Assumptions
The backend runs on kubernetes. All backend code should have a makefile target to build a docker image using a Dockerfile.

Frontend runs on a browser.

## Folder structure
Input files/folders:
   * mcp-server/
   * openapi/
   * AGENCY.md, BACKEND.md, DATABASE.md, FRONTEND.md, HLD.md SCALE.md OBSERVABILITY.md K8S_MANIFEST.md TEST_PLAN.md
NEVER write anything to this files! All are read only.

Output, create these folders
   * frontend, folder for all frontend code
   * backend, folder for all backend code
      - timeline-svc
      - fanout-svc
      - search-svc
      - notification-svc
   * database, folder for all database related code
   * observability, folder for observality resources.
   * utils, folder for common utilities
   * test, folder for all test related output
Do not add any of these output folders to git. In fact - NEVER do any git commands at all.

## Scaling
This document provides sample calculations for scaling considerations and suggestions for auto-scaling.

## Generation
For generating this project, do the following, in this order : 

1. **Read** HLL.md, understand the high level design, generate a diagram for this, HLL.svg

2. **Read** HLD.md and FRONTEND.md and generate a UX_UI model, UI_UX.md

3. Spin up four sub-agents
   2.1. **Backend Agent** - read BACKEND.md, OBSERVABILITY.md and SCALE.md (sections relevate to services in BACKEND.md) create backend folder and generate code in golang according to the documents. Add unit tests for all functionality, have a Makefile with go build, go clean and go test targets.

   2.2. **Database Agent** - read DATABASE.md, OBSERVABILITY.md and SCALE.md (sections relevant to redis and postgres), create database folder and generate resources to bring up cache and database services. 

   2.3. **UI Agent** - read UX_UI.md and FRONTEND.md, create frontend folder and generate frontend using react.

   2.4 **Observability Agent** - read OBSERVABILITY.md, create observability folder and generate resources for observability

4. Wait for the sub agents in step 3 to complete.

5. **Critique step**. Sub up four sub-agents to scan though the generated code and critique it.
   5.1. **Backend Review Agent** - read BACKEND.md, OBSERVABILITY.md and SCALE.md, and review whether code in backend, util folder conforms to it... if not, edit and justify why.

   5.2. **Database Review Agent** - read DATABASE.md, OBSERVABILITY.md and SCALE.md, and review whether code in database folder conforms to it... if not, edit and justify why.

   5.3. **UI Review Agent** -  read UX_UI.md and FRONTEND.md, and review whether code in frontend folder conforms to it... if not, edit and justify why.

   5.4. **Observability Review Agent** - read OBSERVABILITY.md and review whether code in observability folder conforms to it... if not, edit and justify why.

6. Wait for sub-agent in step 5 to complete, then **test** code as per TEST_PLAN.md

7. Review test results, fix code as per any failures and re-test

## Testing
Testing is two levels ->
   * **Unit Tests** for each component, after generating code make sure to generate enough unit tests for 90+ coverage.

   * **Integration Tests**, expounded in TEST_SPAN.md... agent must bring up test environment described in TEST_PLAN.md and run all the tests.

