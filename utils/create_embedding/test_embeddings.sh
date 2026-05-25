#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ -z "${TEST_EMBEDDINGS_PG_VIRTUALENV:-}" &&
      "${TEST_EMBEDDINGS_USE_PG_VIRTUALENV:-1}" == "1" &&
      "${1:-}" != "-h" &&
      "${1:-}" != "--help" &&
      -z "${PGHOST:-}" &&
      -z "${POSTGRES_HOST:-}" &&
      -z "${LOCAL_PGDATA:-${PGDATA:-}}" ]] &&
   command -v pg_virtualenv >/dev/null 2>&1; then
  status_file="$(mktemp)"
  export TEST_EMBEDDINGS_PG_VIRTUALENV=1
  export TEST_EMBEDDINGS_STATUS_FILE="$status_file"

  set +e
  pg_virtualenv bash "$0" "$@"
  exit_code=$?
  set -e

  if [[ -s "$status_file" ]]; then
    status="$(cat "$status_file")"
    rm -f "$status_file"
    if [[ "$status" == "missing_pgvector" ]]; then
      exit 1
    fi
  fi

  rm -f "$status_file"
  exit "$exit_code"
fi

PGHOST="${PGHOST:-${POSTGRES_HOST:-127.0.0.1}}"
PGPORT="${PGPORT:-${POSTGRES_PORT:-5432}}"
PGUSER="${PGUSER:-${POSTGRES_USER:-postgres}}"
PGMAINTENANCE_DB="${PGMAINTENANCE_DB:-postgres}"
LOCAL_PGDATA="${LOCAL_PGDATA:-${PGDATA:-}}"
export PGPASSWORD="${PGPASSWORD:-${POSTGRES_PASSWORD:-}}"
if [[ -n "${TEST_EMBEDDINGS_DB:-}" ]]; then
  PGTESTDATABASE="$TEST_EMBEDDINGS_DB"
elif [[ -n "${POSTGRES_DB:-}" ]]; then
  PGTESTDATABASE="$POSTGRES_DB"
elif [[ -n "${TEST_EMBEDDINGS_PG_VIRTUALENV:-}" ]]; then
  PGTESTDATABASE="embeddings_test"
else
  PGTESTDATABASE="${PGDATABASE:-embeddings_test}"
fi
EMBEDDING_DIMENSIONS="${EMBEDDING_DIMENSIONS:-32}"
if [[ -z "${PYTHON:-}" ]]; then
  if command -v python >/dev/null 2>&1; then
    PYTHON="python"
  elif command -v python3 >/dev/null 2>&1; then
    PYTHON="python3"
  elif command -v py >/dev/null 2>&1; then
    PYTHON="py"
  else
    PYTHON="python"
  fi
fi
COPILOT_COMMAND="${COPILOT_COMMAND:-cat}"
STARTED_LOCAL_POSTGRES=0

declare -A TestCases=(
  [Test1StringEmbeddingPgvectorInsert]=test_string_embedding_pgvector_insert
  [Test2FileEmbeddingPgvectorInsert]=test_file_embedding_pgvector_insert
  [Test3SimilaritySearch]=test_similarity_search
  [Test4JsonOutputPgvectorInsert]=test_json_output_pgvector_insert
)

usage() {
  cat <<USAGE
Usage:
  ./test_embeddings.sh init
  ./test_embeddings.sh [TestCasePattern ...]

Examples:
  ./test_embeddings.sh
  ./test_embeddings.sh 'Test1*'
  ./test_embeddings.sh 'Test*Pgvector*' Test3SimilaritySearch

By default, this uses pg_virtualenv when available to run a throwaway local
Postgres test cluster without requiring you to enter a password.
The target Postgres must have the pgvector extension installed; init exits if
pgvector is unavailable.
Any Postgres instance started by this script is torn down on exit.

Environment:
  PGHOST               Local Postgres host (default: ${PGHOST})
  PGPORT               Local Postgres port (default: ${PGPORT})
  TEST_EMBEDDINGS_DB   Test database name (default: ${PGTESTDATABASE})
  PGUSER               Postgres user (default: ${PGUSER})
  TEST_EMBEDDINGS_USE_PG_VIRTUALENV
                       Set to 0 to use an existing Postgres instead
  PGPASSWORD           Existing Postgres password, if needed
  LOCAL_PGDATA         Optional local data dir to start with pg_ctl when Postgres is down
  EMBEDDING_DIMENSIONS Vector dimensions used by tests (default: ${EMBEDDING_DIMENSIONS})
  PYTHON               Python executable (default: ${PYTHON})
  COPILOT_COMMAND      Command passed to create_embedding.py (default: ${COPILOT_COMMAND})
USAGE
}

require_command() {
  local command_name="$1"
  if ! command -v "$command_name" >/dev/null 2>&1; then
    echo "Missing required command: $command_name" >&2
    exit 1
  fi
}

teardown() {
  local exit_code=$?

  if [[ "$STARTED_LOCAL_POSTGRES" == "1" && -n "$LOCAL_PGDATA" ]]; then
    echo "Stopping local Postgres..."
    if ! pg_ctl -D "$LOCAL_PGDATA" -w stop >/dev/null; then
      echo "Failed to stop local Postgres at $LOCAL_PGDATA" >&2
    fi
  fi

  return "$exit_code"
}

trap teardown EXIT

psql_exec() {
  psql -v ON_ERROR_STOP=1 \
    -w \
    -h "$PGHOST" \
    -p "$PGPORT" \
    -U "$PGUSER" \
    -d "$PGTESTDATABASE" \
    "$@"
}

psql_scalar() {
  psql -v ON_ERROR_STOP=1 -At \
    -w \
    -h "$PGHOST" \
    -p "$PGPORT" \
    -U "$PGUSER" \
    -d "$PGTESTDATABASE" \
    "$@"
}

psql_admin() {
  psql -v ON_ERROR_STOP=1 \
    -w \
    -h "$PGHOST" \
    -p "$PGPORT" \
    -U "$PGUSER" \
    -d "$PGMAINTENANCE_DB" \
    "$@"
}

postgres_is_ready() {
  pg_isready \
    -h "$PGHOST" \
    -p "$PGPORT" \
    -U "$PGUSER" \
    -d "$PGMAINTENANCE_DB" >/dev/null 2>&1
}

start_local_postgres() {
  if [[ -z "$LOCAL_PGDATA" ]]; then
    echo "Local Postgres is not accepting connections at ${PGHOST}:${PGPORT}." >&2
    echo "Start Postgres locally, or set LOCAL_PGDATA/PGDATA so this script can start it with pg_ctl." >&2
    exit 1
  fi

  require_command pg_ctl

  if [[ ! -s "$LOCAL_PGDATA/PG_VERSION" ]]; then
    require_command initdb
    mkdir -p "$LOCAL_PGDATA"
    initdb -D "$LOCAL_PGDATA" -U "$PGUSER" -A trust >/dev/null
  fi

  pg_ctl -D "$LOCAL_PGDATA" -o "-p $PGPORT" -w start >/dev/null
  STARTED_LOCAL_POSTGRES=1
}

ensure_database() {
  local exists
  exists="$(psql_admin -At -v dbname="$PGTESTDATABASE" <<'SQL'
SELECT 1 FROM pg_database WHERE datname = :'dbname';
SQL
)"
  if [[ "$exists" == "1" ]]; then
    return 0
  fi

  psql_admin -v dbname="$PGTESTDATABASE" <<'SQL' >/dev/null
CREATE DATABASE :"dbname";
SQL
}

pgvector_available() {
  local available
  available="$(psql_exec -At <<'SQL'
SELECT 1 FROM pg_available_extensions WHERE name = 'vector';
SQL
)"
  [[ "$available" == "1" ]]
}

fail_missing_pgvector() {
  echo "pgvector is not installed for this PostgreSQL installation." >&2
  echo "Install the matching pgvector extension/package, then rerun this script." >&2

  if [[ -n "${TEST_EMBEDDINGS_STATUS_FILE:-}" ]]; then
    printf 'missing_pgvector' >"$TEST_EMBEDDINGS_STATUS_FILE"
    exit 0
  fi

  exit 1
}

setup_pgvector() {
  psql_exec <<SQL
CREATE EXTENSION IF NOT EXISTS vector;
DROP TABLE IF EXISTS embedding_test_documents;
CREATE TABLE embedding_test_documents (
  id text PRIMARY KEY,
  content text NOT NULL,
  embedding vector(${EMBEDDING_DIMENSIONS}) NOT NULL
);
SQL
}

init() {
  require_command "$PYTHON"
  require_command psql
  require_command pg_isready

  if ! postgres_is_ready; then
    echo "Starting local Postgres with pg_ctl..."
    start_local_postgres
  fi

  echo "Waiting for Postgres to accept connections..."
  for _ in $(seq 1 60); do
    if postgres_is_ready; then
      break
    fi
    sleep 1
  done

  postgres_is_ready
  ensure_database
  if pgvector_available; then
    setup_pgvector
  else
    fail_missing_pgvector
  fi
}

create_embedding() {
  "$PYTHON" "$SCRIPT_DIR/create_embedding.py" \
    "$@" \
    --dimensions "$EMBEDDING_DIMENSIONS" \
    --copilot-command "$COPILOT_COMMAND"
}

insert_document() {
  local id="$1"
  local content="$2"
  local embedding="$3"

  psql_exec \
    -v id="$id" \
    -v content="$content" \
    -v embedding="$embedding" <<'SQL' >/dev/null
INSERT INTO embedding_test_documents (id, content, embedding)
VALUES (:'id', :'content', :'embedding'::vector)
ON CONFLICT (id) DO UPDATE
SET content = EXCLUDED.content,
    embedding = EXCLUDED.embedding;
SQL
}

assert_equals() {
  local expected="$1"
  local actual="$2"
  local message="$3"

  if [[ "$expected" != "$actual" ]]; then
    echo "Assertion failed: $message" >&2
    echo "  expected: $expected" >&2
    echo "  actual:   $actual" >&2
    return 1
  fi
}

test_string_embedding_pgvector_insert() {
  local embedding
  embedding="$(create_embedding "Jupiter spaceship mission through the magnetosphere")"
  insert_document "string-jupiter" "Jupiter spaceship mission through the magnetosphere" "$embedding"

  local dimensions
  dimensions="$(psql_scalar -v id="string-jupiter" <<'SQL'
SELECT vector_dims(embedding) FROM embedding_test_documents WHERE id = :'id';
SQL
)"
  assert_equals "$EMBEDDING_DIMENSIONS" "$dimensions" "string embedding should round-trip through pgvector"
}

test_file_embedding_pgvector_insert() {
  local story_path="$SCRIPT_DIR/test_story.txt"
  if [[ ! -f "$story_path" ]]; then
    echo "Missing test fixture: $story_path" >&2
    return 1
  fi

  local embedding
  embedding="$(create_embedding --path "$story_path")"
  insert_document "file-story" "test_story.txt" "$embedding"

  local count
  count="$(psql_scalar -v id="file-story" <<SQL
SELECT count(*) FROM embedding_test_documents WHERE id = :'id' AND vector_dims(embedding) = ${EMBEDDING_DIMENSIONS};
SQL
)"
  assert_equals "1" "$count" "file embedding should insert with the configured vector dimensions"
}

test_similarity_search() {
  local jupiter_embedding garden_embedding query_embedding nearest
  jupiter_embedding="$(create_embedding "spaceship mission to Jupiter and Europa beacon")"
  garden_embedding="$(create_embedding "quiet garden soil tomatoes and summer rain")"
  query_embedding="$(create_embedding "Jupiter spaceship Europa mission")"

  insert_document "similarity-jupiter" "spaceship mission to Jupiter and Europa beacon" "$jupiter_embedding"
  insert_document "similarity-garden" "quiet garden soil tomatoes and summer rain" "$garden_embedding"

  nearest="$(psql_scalar -v query_embedding="$query_embedding" <<'SQL'
SELECT id FROM embedding_test_documents WHERE id LIKE 'similarity-%' ORDER BY embedding <=> :'query_embedding'::vector LIMIT 1;
SQL
)"
  assert_equals "similarity-jupiter" "$nearest" "Jupiter query should rank the Jupiter document first"
}

test_json_output_pgvector_insert() {
  local payload embedding dimensions
  payload="$(create_embedding "JSON payload for pgvector insertion" --format json)"
  embedding="$("$PYTHON" -c 'import json,sys; print(json.load(sys.stdin)["pgvector"])' <<<"$payload")"
  insert_document "json-output" "JSON payload for pgvector insertion" "$embedding"

  dimensions="$(psql_scalar -v id="json-output" <<'SQL'
SELECT vector_dims(embedding) FROM embedding_test_documents WHERE id = :'id';
SQL
)"
  assert_equals "$EMBEDDING_DIMENSIONS" "$dimensions" "JSON pgvector output should insert into Postgres"
}

matches_any_pattern() {
  local test_name="$1"
  shift

  local pattern
  for pattern in "$@"; do
    if [[ "$test_name" == $pattern ]]; then
      return 0
    fi
  done

  return 1
}

run_test_cases() {
  local patterns=("$@")
  local matched=0
  local failed=0
  local test_name function_name

  while IFS= read -r test_name; do
    if ! matches_any_pattern "$test_name" "${patterns[@]}"; then
      continue
    fi

    matched=$((matched + 1))
    function_name="${TestCases[$test_name]}"
    echo "=== RUN   $test_name"
    if "$function_name"; then
      echo "--- PASS: $test_name"
    else
      echo "--- FAIL: $test_name"
      failed=$((failed + 1))
    fi
  done < <(printf '%s\n' "${!TestCases[@]}" | sort)

  if [[ "$matched" -eq 0 ]]; then
    echo "No test cases matched pattern(s): ${patterns[*]}" >&2
    echo "Available test cases:" >&2
    printf '  %s\n' "${!TestCases[@]}" | sort >&2
    return 1
  fi

  if [[ "$failed" -ne 0 ]]; then
    echo "FAIL: ${failed}/${matched} test case(s) failed" >&2
    return 1
  fi

  echo "PASS: ${matched} test case(s)"
}

main() {
  if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    return 0
  fi

  if [[ "${1:-}" == "init" ]]; then
    init
    shift
    if [[ "$#" -eq 0 ]]; then
      return 0
    fi
  else
    init
  fi

  local patterns=("$@")
  if [[ "${#patterns[@]}" -eq 0 ]]; then
    patterns=("*")
  fi

  run_test_cases "${patterns[@]}"
}

main "$@"
