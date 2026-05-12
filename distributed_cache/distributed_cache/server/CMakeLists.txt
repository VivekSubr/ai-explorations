cmake_minimum_required(VERSION 3.16)
project(dist_cache VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Prefer the in-tree OpenSSL 3.5 (third_party/openssl/install/) over the
# system OpenSSL if it has been built. This avoids the OpenSSL 3.0/3.5
# version mismatch on Ubuntu where libssl-dev is still 3.0.x.
set(LOCAL_OPENSSL_ROOT "${CMAKE_SOURCE_DIR}/../../third_party/openssl/install")
if(EXISTS "${LOCAL_OPENSSL_ROOT}/include/openssl/ssl.h")
    set(OPENSSL_ROOT_DIR "${LOCAL_OPENSSL_ROOT}" CACHE PATH "" FORCE)
    set(OPENSSL_USE_STATIC_LIBS FALSE CACHE BOOL "" FORCE)
    # Ensure runtime resolves libssl.so.3 / libcrypto.so.3 from our prefix
    # rather than /usr/lib (which on Ubuntu is 3.0.x).
    list(APPEND CMAKE_BUILD_RPATH "${LOCAL_OPENSSL_ROOT}/lib")
    list(APPEND CMAKE_INSTALL_RPATH "${LOCAL_OPENSSL_ROOT}/lib")
    message(STATUS "Using local OpenSSL at ${LOCAL_OPENSSL_ROOT}")
endif()

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

add_executable(dist_cache.exe main.cc server.cc)
target_link_libraries(dist_cache.exe
    PRIVATE
        hiredis::hiredis
        PkgConfig::NGTCP2
        PkgConfig::NGTCP2_CRYPTO
        PkgConfig::NGHTTP3
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads)

add_executable(server_test server_test.cc)
target_link_libraries(server_test
    PRIVATE
        hiredis::hiredis
        PkgConfig::NGTCP2
        PkgConfig::NGTCP2_CRYPTO
        PkgConfig::NGHTTP3
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
        GTest::gtest
        GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(server_test)