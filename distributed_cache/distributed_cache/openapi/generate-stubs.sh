#!/usr/bin/env bash
# Generate model stubs from openapi/distributed_cache.yaml using
# openapi-generator-cli. By default ONLY data-model classes are emitted
# (no HTTP server framework, no main, no CMakeLists) so the existing
# server.cc can include them as plain request/response types.
#
# Usage:
#   ./generate-stubs.sh [-g <generator>] [-o <output-dir>] [-s <spec>]
#                       [--with-apis] [--full]
#                       [-- <extra openapi-generator args>]
#
# Flags:
#   --with-apis  Also generate the api/ interface classes (still skips
#                main, CMakeLists, helpers, etc.).
#   --full       Generate everything the generator produces (server framework,
#                main, CMakeLists, helpers, ...).
#
# Defaults:
#   generator   = cpp-pistache-server  (used only for its model templates)
#   spec        = <script dir>/distributed_cache.yaml
#   output dir  = <caller cwd>/generated/<generator>
#
# Examples:
#   ./generate-stubs.sh                              # models only (default)
#   ./generate-stubs.sh --with-apis                  # models + api/ interfaces
#   ./generate-stubs.sh --full -g go-server          # full server scaffold
#
# NOTE: the default no longer emits the api/ folder. Pass --with-apis if you
# need the generated KvApi/JsonApi interface classes.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CALLER_DIR="$PWD"
SPEC_DEFAULT="${SCRIPT_DIR}/distributed_cache.yaml"
GENERATOR="cpp-pistache-server"
SPEC=""
OUT=""
MODE="models"   # one of: models | apis | full
EXTRA_ARGS=()

usage() {
    sed -n '2,29p' "$0"
    exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -g|--generator) GENERATOR="$2"; shift 2 ;;
        -o|--output)    OUT="$2"; shift 2 ;;
        -s|--spec)      SPEC="$2"; shift 2 ;;
        --with-apis)    MODE="apis"; shift ;;
        --full)         MODE="full"; shift ;;
        -h|--help)      usage 0 ;;
        --)             shift; EXTRA_ARGS=("$@"); break ;;
        *) echo "Unknown argument: $1" >&2; usage 1 ;;
    esac
done

SPEC="${SPEC:-$SPEC_DEFAULT}"
OUT="${OUT:-${CALLER_DIR}/generated/${GENERATOR}}"

if [[ ! -f "$SPEC" ]]; then
    echo "OpenAPI spec not found: $SPEC" >&2
    exit 1
fi

mkdir -p "$OUT"

# Resolve openapi-generator invocation: prefer a system binary, otherwise
# fall back to the official Docker image so the script works on a clean box.
run_generator() {
    if command -v openapi-generator-cli >/dev/null 2>&1; then
        echo "Using openapi-generator-cli from PATH"
        openapi-generator-cli generate "$@"
    elif command -v openapi-generator >/dev/null 2>&1; then
        echo "Using openapi-generator from PATH"
        openapi-generator generate "$@"
    elif command -v npx >/dev/null 2>&1; then
        echo "Using npx @openapitools/openapi-generator-cli"
        npx --yes @openapitools/openapi-generator-cli generate "$@"
    elif command -v docker >/dev/null 2>&1; then
        echo "Using openapitools/openapi-generator-cli docker image"
        docker run --rm \
            -u "$(id -u):$(id -g)" \
            -v "${SCRIPT_DIR}:/local/openapi" \
            -v "${OUT}:/local/out" \
            openapitools/openapi-generator-cli generate "$@"
    else
        echo "ERROR: need one of openapi-generator-cli, openapi-generator, npx, or docker on PATH." >&2
        exit 1
    fi
}

# Build argument list. When using docker we must use container-internal paths.
if ! command -v openapi-generator-cli >/dev/null 2>&1 \
   && ! command -v openapi-generator >/dev/null 2>&1 \
   && ! command -v npx >/dev/null 2>&1 \
   && command -v docker >/dev/null 2>&1; then
    SPEC_REL="$(basename "$SPEC")"
    ARGS=(-i "/local/openapi/${SPEC_REL}" -g "$GENERATOR" -o "/local/out")
else
    ARGS=(-i "$SPEC" -g "$GENERATOR" -o "$OUT")
fi

# Restrict what the generator emits. --global-property selects which template
# groups run; setting a group to an empty value disables it.
case "$MODE" in
    models)
        # supportingFiles whitelist keeps Helpers.{h,cpp} (required by model
        # .cpp files for ValidationException) while dropping CMakeLists, main,
        # README, etc.
        ARGS+=(--global-property "models,modelDocs=false,modelTests=false,supportingFiles=Helpers.h,Helpers.cpp,apis=false,apiDocs=false,apiTests=false")
        ;;
    apis)
        ARGS+=(--global-property "models,modelDocs=false,modelTests=false,apis,apiDocs=false,apiTests=false,supportingFiles=Helpers.h,Helpers.cpp,ApiBase.h,ApiBase.cpp")
        ;;
    full)
        : # no restriction
        ;;
esac

ARGS+=("${EXTRA_ARGS[@]}")

echo "Generator : $GENERATOR"
echo "Spec      : $SPEC"
echo "Output    : $OUT"
echo "Mode      : $MODE"
echo

run_generator "${ARGS[@]}"

echo
echo "Stubs generated under: $OUT"
