OPENAPI_DIR  := $(abspath $(CURDIR)/../openapi)
REPO_ROOT    := $(abspath $(CURDIR)/..)
GENERATOR    ?= cpp-pistache-server

cmake: deps clean generate
	mkdir -p build && cd build && cmake  -DCMAKE_BUILD_TYPE=Debug ..

# Ensure third_party libs (openssl 3.5, nghttp3, ngtcp2) are built. Delegates
# to the top-level Makefile so the recipe lives in one place.
deps:
	$(MAKE) -C $(REPO_ROOT) build

clean:
	rm -fr build generated openapitools.json

test:
	cd build && ctest --output-on-failure

debug:
	gdb -d ${PWD} ${PWD}/build/server_test

# Generate API stubs from the OpenAPI spec. Override generator via:
#   make generate GENERATOR=cpp-restsdk-client
# Also emits generated/operations.h, the dispatch manifest consumed by rest.cc.
generate:
	$(OPENAPI_DIR)/generate-stubs.sh -g $(GENERATOR)
	mkdir -p generated
	$(OPENAPI_DIR)/gen-operations.py --out generated/operations.h

.PHONY: cmake clean test debug generate deps