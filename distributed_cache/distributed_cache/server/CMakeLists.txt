cmake_minimum_required(VERSION 3.16)
project(dist_cache VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Prefer the in-tree OpenSSL 3.5 (third_party/openssl/install/) over the
# system OpenSSL if it has been built. This avoids the OpenSSL 3.0/3.5
# version mismatch on Ubuntu where libssl-dev is still 3.0.x.
set(THIRD_PARTY_DIR  "${CMAKE_SOURCE_DIR}/../../third_party")
set(LOCAL_OPENSSL_ROOT "${THIRD_PARTY_DIR}/openssl/install")
set(LOCAL_NGHTTP3_ROOT "${THIRD_PARTY_DIR}/nghttp3/install")
set(LOCAL_NGTCP2_ROOT  "${THIRD_PARTY_DIR}/ngtcp2/install")

if(EXISTS "${LOCAL_OPENSSL_ROOT}/include/openssl/ssl.h")
    set(OPENSSL_ROOT_DIR "${LOCAL_OPENSSL_ROOT}" CACHE PATH "" FORCE)
    set(OPENSSL_USE_STATIC_LIBS FALSE CACHE BOOL "" FORCE)
    # Ensure runtime resolves libssl.so.3 / libcrypto.so.3 from our prefix
    # rather than /usr/lib (which on Ubuntu is 3.0.x).
    list(APPEND CMAKE_BUILD_RPATH "${LOCAL_OPENSSL_ROOT}/lib")
    list(APPEND CMAKE_INSTALL_RPATH "${LOCAL_OPENSSL_ROOT}/lib")
    message(STATUS "Using local OpenSSL at ${LOCAL_OPENSSL_ROOT}")
endif()

# Prefer the in-tree ngtcp2 / nghttp3 (built via top-level `make build`).
# Prepend their pkg-config dirs so the pkg_check_modules() calls below find
# the local copies (which were linked against the local OpenSSL 3.5).
foreach(_root IN ITEMS "${LOCAL_NGTCP2_ROOT}" "${LOCAL_NGHTTP3_ROOT}")
    if(EXISTS "${_root}/lib/pkgconfig")
        set(ENV{PKG_CONFIG_PATH} "${_root}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
        list(APPEND CMAKE_BUILD_RPATH "${_root}/lib")
        list(APPEND CMAKE_INSTALL_RPATH "${_root}/lib")
        message(STATUS "Using local library at ${_root}")
    endif()
endforeach()

# Setup testing
enable_testing()
find_package(GTest REQUIRED)
include_directories(${GTEST_INCLUDE_DIR})

# hiredis (Redis C client)
find_package(hiredis CONFIG QUIET)
if(NOT hiredis_FOUND)
    find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h)
    find_library(HIREDIS_LIBRARY NAMES hiredis)
    if(NOT HIREDIS_INCLUDE_DIR OR NOT HIREDIS_LIBRARY)
        message(FATAL_ERROR "hiredis not found. Install libhiredis-dev (or equivalent).")
    endif()
    add_library(hiredis::hiredis UNKNOWN IMPORTED)
    set_target_properties(hiredis::hiredis PROPERTIES
        IMPORTED_LOCATION "${HIREDIS_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${HIREDIS_INCLUDE_DIR}")
endif()

find_package(Threads REQUIRED)

# OpenSSL 3.5+ is required for the QUIC API used by ngtcp2_crypto_ossl.
find_package(OpenSSL 3.5 REQUIRED)

# ngtcp2 / nghttp3 are typically discovered via pkg-config.
find_package(PkgConfig REQUIRED)
pkg_check_modules(NGTCP2          REQUIRED IMPORTED_TARGET libngtcp2)
pkg_check_modules(NGTCP2_CRYPTO   REQUIRED IMPORTED_TARGET libngtcp2_crypto_ossl)
pkg_check_modules(NGHTTP3         REQUIRED IMPORTED_TARGET libnghttp3)

# nlohmann::json (header-only).
find_package(nlohmann_json 3.10 REQUIRED)

# OpenAPI-generated data model classes. Compiled in so that spec changes
# (run via `make generate`) propagate as compile errors into rest.cc.
set(GEN_MODEL_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/cpp-pistache-server/model")
set(GEN_MODEL_SOURCES
    "${GEN_MODEL_DIR}/Error.cpp"
    "${GEN_MODEL_DIR}/ErrorCode.cpp"
    "${GEN_MODEL_DIR}/WrappedJsonValue.cpp")

# operations.h: spec-derived OpId enum and dispatch manifest emitted by
# openapi/gen-operations.py. Adding/removing an operation in the YAML
# breaks the exhaustive switch in rest.cc, hence -Werror=switch below.
set(GEN_OPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated")

# Treat non-exhaustive switch over the generated OpId as a hard error so
# spec drift fails the build.
set(REST_DIAGS -Wall -Wextra -Wswitch-enum -Werror=switch -Werror=switch-enum)

add_executable(dist_cache.exe main.cc server.cc rest.cc redis_client.cc ${GEN_MODEL_SOURCES})
target_include_directories(dist_cache.exe PRIVATE ${GEN_MODEL_DIR} ${GEN_OPS_DIR})
target_compile_options(dist_cache.exe PRIVATE ${REST_DIAGS})
target_link_libraries(dist_cache.exe
    PRIVATE
        hiredis::hiredis
        PkgConfig::NGTCP2
        PkgConfig::NGTCP2_CRYPTO
        PkgConfig::NGHTTP3
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
        nlohmann_json::nlohmann_json)

add_executable(hello.exe hello.cc rest.cc redis_client.cc ${GEN_MODEL_SOURCES})
target_include_directories(hello.exe PRIVATE ${GEN_MODEL_DIR} ${GEN_OPS_DIR})
target_compile_options(hello.exe PRIVATE ${REST_DIAGS})
target_link_libraries(hello.exe
    PRIVATE
        hiredis::hiredis
        PkgConfig::NGTCP2
        PkgConfig::NGTCP2_CRYPTO
        PkgConfig::NGHTTP3
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
        nlohmann_json::nlohmann_json)

add_executable(server_test server_test.cc rest.cc redis_client.cc ${GEN_MODEL_SOURCES})
target_include_directories(server_test PRIVATE ${GEN_MODEL_DIR} ${GEN_OPS_DIR})
target_compile_options(server_test PRIVATE ${REST_DIAGS})
target_link_libraries(server_test
    PRIVATE
        hiredis::hiredis
        PkgConfig::NGTCP2
        PkgConfig::NGTCP2_CRYPTO
        PkgConfig::NGHTTP3
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
        nlohmann_json::nlohmann_json
        GTest::gtest
        GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(server_test)

add_executable(rest_test rest_test.cc rest.cc ${GEN_MODEL_SOURCES})
target_include_directories(rest_test PRIVATE ${GEN_MODEL_DIR} ${GEN_OPS_DIR})
target_compile_options(rest_test PRIVATE ${REST_DIAGS})
target_link_libraries(rest_test
    PRIVATE
        nlohmann_json::nlohmann_json
        GTest::gtest
        GTest::gtest_main)
gtest_discover_tests(rest_test)

# Integration tests for the hiredis wrapper. Talks to a live redis-server on
# REDIS_HOST:REDIS_PORT (defaults 127.0.0.1:6379). CTest fixtures below bring
# a redis-server up on demand (and tear it down after) if one isn't already
# running, so `ctest` works standalone without external orchestration.
add_executable(redis_client_test redis_client_test.cc redis_client.cc)
target_link_libraries(redis_client_test
    PRIVATE
        hiredis::hiredis
        nlohmann_json::nlohmann_json
        GTest::gtest
        GTest::gtest_main)
gtest_discover_tests(redis_client_test
    PROPERTIES FIXTURES_REQUIRED Redis)

# Fixture scaffolding for the Redis dependency. Two ctest entries: redis_up
# (FIXTURES_SETUP) and redis_down (FIXTURES_CLEANUP). Each redis_client_test
# case declares FIXTURES_REQUIRED=Redis (above), so ctest will:
#   1. run redis_up before any of them,
#   2. run them (in any order, possibly in parallel),
#   3. run redis_down once they're all finished.
# If redis_up fails, dependent tests are reported as "Not Run".
set(REDIS_FIXTURE_SH "${CMAKE_CURRENT_SOURCE_DIR}/ctest_redis.sh")
set(REDIS_FIXTURE_CONF "${CMAKE_CURRENT_SOURCE_DIR}/../redis/redis.conf")
set(REDIS_FIXTURE_STATE "${CMAKE_CURRENT_BINARY_DIR}/redis_fixture")

add_test(NAME redis_up
    COMMAND ${REDIS_FIXTURE_SH} up
            ${REDIS_FIXTURE_STATE} ${REDIS_FIXTURE_CONF})
set_tests_properties(redis_up PROPERTIES FIXTURES_SETUP Redis)

add_test(NAME redis_down
    COMMAND ${REDIS_FIXTURE_SH} down ${REDIS_FIXTURE_STATE})
set_tests_properties(redis_down PROPERTIES FIXTURES_CLEANUP Redis)