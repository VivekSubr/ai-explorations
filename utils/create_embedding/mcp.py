#!/usr/bin/env python3
"""MCP server for loading Istio embedding context from local Postgres."""

from __future__ import annotations

import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any
from urllib.parse import quote, unquote


SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent


def is_wsl() -> bool:
    try:
        return "microsoft" in Path("/proc/version").read_text(encoding="utf-8", errors="ignore").lower()
    except OSError:
        return False


def normalize_path(path: str | Path) -> Path:
    raw_path = str(path)
    if os.name != "nt":
        match = re.match(r"^([A-Za-z]):[\\/](.*)$", raw_path)
        if match:
            drive = match.group(1).lower()
            rest = match.group(2).replace("\\", "/")
            return Path("/mnt") / drive / rest
    return Path(raw_path)


def env_path(name: str, default: str | Path) -> Path:
    return normalize_path(os.environ.get(name, str(default))).resolve()


def env_bool(name: str, default: bool) -> bool:
    value = os.environ.get(name)
    if value is None:
        return default
    return value.strip().lower() not in {"0", "false", "no", "off"}


def env_int(name: str, default: int) -> int:
    value = os.environ.get(name)
    if value is None:
        return default
    try:
        return int(value)
    except ValueError:
        return default


WORKDIR = env_path("WORKDIR", PROJECT_ROOT / "istio_embedding_workspace")
SOURCE_DOCS_DIR = env_path("SOURCE_DOCS_DIR", WORKDIR / "source_docs")
CREATE_EMBEDDING_PY = env_path("CREATE_EMBEDDING_PY", WORKDIR / "tools" / "create_embedding.py")
EXPORT_DIR = env_path("EXPORT_DIR", WORKDIR / "exports")

POSTGRES_HOST = os.environ.get("POSTGRES_HOST", "localhost")
POSTGRES_PORT = os.environ.get("POSTGRES_PORT", "55432")
POSTGRES_DB = os.environ.get("POSTGRES_DB", "istio_embeddings")
POSTGRES_USER = os.environ.get("POSTGRES_USER", "postgres")
POSTGRES_PASSWORD = os.environ.get("POSTGRES_PASSWORD", "")
POSTGRES_AUTO_INIT = env_bool("POSTGRES_AUTO_INIT", True)
POSTGRES_DATA_DIR = env_path(
    "POSTGRES_DATA_DIR",
    Path.home() / ".local" / "share" / "cna_explain" / "istio_embeddings_postgres" if is_wsl() else WORKDIR / "postgres_data",
)
POSTGRES_LOG = env_path("POSTGRES_LOG", POSTGRES_DATA_DIR / "postgres.log")
POSTGRES_SOCKET_DIR = env_path("POSTGRES_SOCKET_DIR", POSTGRES_DATA_DIR / "socket")
POSTGRES_INIT_TIMEOUT = max(1, env_int("POSTGRES_INIT_TIMEOUT", 60))

DIMENSIONS = os.environ.get("DIMENSIONS", "1536")
COPILOT_TIMEOUT = os.environ.get("COPILOT_TIMEOUT", "300")
COPILOT_COMMAND = os.environ.get("COPILOT_COMMAND", "")
QUERY_EMBEDDING_USE_COPILOT = env_bool("QUERY_EMBEDDING_USE_COPILOT", False)

_DATABASE_READY = False
_BOOTSTRAPPING_DATABASE = False
_POSTGRES_EXECUTABLES: dict[str, str] = {}
_CREATE_EMBEDDING_MODULE: Any | None = None


def log(message: str) -> None:
    print(f"istio-embeddings-mcp: {message}", file=sys.stderr, flush=True)


def sql_literal(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def quote_identifier(identifier: str) -> str:
    return '"' + identifier.replace('"', '""') + '"'


def clamp_int(value: Any, default: int, minimum: int, maximum: int) -> int:
    try:
        parsed = int(value)
    except (TypeError, ValueError):
        parsed = default
    return max(minimum, min(maximum, parsed))


def postgres_executable(name: str) -> str:
    if name in _POSTGRES_EXECUTABLES:
        return _POSTGRES_EXECUTABLES[name]

    bin_dir = os.environ.get("POSTGRES_BIN_DIR")
    if bin_dir:
        candidate = normalize_path(bin_dir).resolve() / name
        if candidate.is_file():
            _POSTGRES_EXECUTABLES[name] = str(candidate)
            return _POSTGRES_EXECUTABLES[name]

    found = shutil.which(name)
    if found:
        _POSTGRES_EXECUTABLES[name] = found
        return found

    for candidate in sorted(Path("/usr/lib/postgresql").glob(f"*/bin/{name}"), reverse=True):
        if candidate.is_file():
            _POSTGRES_EXECUTABLES[name] = str(candidate)
            return _POSTGRES_EXECUTABLES[name]

    raise RuntimeError(f"Required PostgreSQL command not found: {name}")


def run_command(cmd: list[str], input_text: str | None = None, timeout: int = POSTGRES_INIT_TIMEOUT) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    env["PGPASSWORD"] = POSTGRES_PASSWORD
    return subprocess.run(cmd, input=input_text, text=True, capture_output=True, env=env, timeout=timeout, check=False)


def run_psql(database: str, sql: str, variables: dict[str, str] | None = None) -> str:
    cmd = [
        postgres_executable("psql"),
        "-X",
        "-q",
        "-A",
        "-t",
        "-w",
        "-v",
        "ON_ERROR_STOP=1",
        "-h",
        POSTGRES_HOST,
        "-p",
        POSTGRES_PORT,
        "-U",
        POSTGRES_USER,
    ]
    for key, value in (variables or {}).items():
        cmd.append(f"--set={key}={value}")
    cmd.extend(["-d", database])

    result = run_command(cmd, sql)
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip() or "psql failed")
    return result.stdout.strip()


def run_psql_file(database: str, path: Path) -> None:
    cmd = [
        postgres_executable("psql"),
        "-X",
        "-q",
        "-w",
        "-v",
        "ON_ERROR_STOP=1",
        "-h",
        POSTGRES_HOST,
        "-p",
        POSTGRES_PORT,
        "-U",
        POSTGRES_USER,
        "-d",
        database,
        "-f",
        str(path),
    ]
    result = run_command(cmd, timeout=max(POSTGRES_INIT_TIMEOUT, 300))
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip() or f"psql failed loading {path}")


def postgres_accepting_connections() -> bool:
    result = run_command(
        [
            postgres_executable("pg_isready"),
            "-h",
            POSTGRES_HOST,
            "-p",
            POSTGRES_PORT,
            "-U",
            POSTGRES_USER,
            "-d",
            "postgres",
        ]
    )
    return result.returncode == 0


def can_connect(database: str) -> tuple[bool, str]:
    try:
        run_psql(database, "SELECT 1;")
        return True, ""
    except Exception as exc:
        return False, str(exc)


def database_has_embeddings() -> bool:
    sql = """
SELECT count(*)
FROM information_schema.columns
WHERE table_schema = 'public'
  AND column_name = 'embedding'
  AND udt_name = 'vector';
"""
    try:
        return int(run_psql(POSTGRES_DB, sql) or "0") > 0
    except Exception:
        return False


def latest_export_path() -> Path:
    exports = sorted(EXPORT_DIR.glob(f"{POSTGRES_DB}_*.sql"), key=lambda path: path.stat().st_mtime, reverse=True)
    if not exports:
        raise RuntimeError(f"No embedding export found in {EXPORT_DIR} matching {POSTGRES_DB}_*.sql")
    return exports[0]


def initialize_postgres_data_dir() -> None:
    if (POSTGRES_DATA_DIR / "PG_VERSION").is_file():
        return
    if POSTGRES_DATA_DIR.exists() and any(POSTGRES_DATA_DIR.iterdir()):
        raise RuntimeError(f"Postgres data directory exists but is not initialized: {POSTGRES_DATA_DIR}")

    POSTGRES_DATA_DIR.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        postgres_executable("initdb"),
        "-D",
        str(POSTGRES_DATA_DIR),
        "-U",
        POSTGRES_USER,
        "--auth-local=trust",
        "--auth-host=trust",
        "--encoding=UTF8",
        "--no-locale",
    ]
    log(f"initializing local Postgres data directory at {POSTGRES_DATA_DIR}")
    result = run_command(cmd)
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip() or "initdb failed")


def start_local_postgres() -> None:
    if POSTGRES_HOST not in {"localhost", "127.0.0.1", "::1"}:
        raise RuntimeError(f"POSTGRES_AUTO_INIT can only start a local database, not host {POSTGRES_HOST}")

    initialize_postgres_data_dir()
    POSTGRES_LOG.parent.mkdir(parents=True, exist_ok=True)
    POSTGRES_SOCKET_DIR.mkdir(parents=True, exist_ok=True)
    listen_address = "localhost" if POSTGRES_HOST in {"localhost", "::1"} else POSTGRES_HOST
    cmd = [
        postgres_executable("pg_ctl"),
        "-D",
        str(POSTGRES_DATA_DIR),
        "-l",
        str(POSTGRES_LOG),
        "-o",
        f"-p {POSTGRES_PORT} -c listen_addresses={listen_address} -c unix_socket_directories={POSTGRES_SOCKET_DIR}",
        "-w",
        "-t",
        str(POSTGRES_INIT_TIMEOUT),
        "start",
    ]
    log(f"starting local Postgres on {POSTGRES_HOST}:{POSTGRES_PORT}")
    result = run_command(cmd, timeout=POSTGRES_INIT_TIMEOUT + 10)
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip() or "pg_ctl start failed")


def ensure_postgres_running() -> None:
    connected, error = can_connect("postgres")
    if connected:
        return

    if postgres_accepting_connections():
        raise RuntimeError(
            f"Postgres is accepting connections on {POSTGRES_HOST}:{POSTGRES_PORT}, "
            f"but psql cannot connect as user '{POSTGRES_USER}': {error}"
        )

    if not POSTGRES_AUTO_INIT:
        raise RuntimeError(
            f"Postgres is not available on {POSTGRES_HOST}:{POSTGRES_PORT} and POSTGRES_AUTO_INIT is disabled"
        )

    start_local_postgres()
    connected, error = can_connect("postgres")
    if not connected:
        raise RuntimeError(f"Started Postgres on {POSTGRES_HOST}:{POSTGRES_PORT}, but psql cannot connect: {error}")


def ensure_database_exists() -> None:
    run_psql(
        "postgres",
        f"""
SELECT 'CREATE DATABASE ' || quote_ident({sql_literal(POSTGRES_DB)})
WHERE NOT EXISTS (SELECT 1 FROM pg_database WHERE datname = {sql_literal(POSTGRES_DB)})\\gexec
""",
    )


def load_embeddings_export() -> None:
    export_path = latest_export_path()
    log(f"loading embeddings from {export_path}")
    run_psql_file(POSTGRES_DB, export_path)


def ensure_database_ready() -> None:
    global _DATABASE_READY, _BOOTSTRAPPING_DATABASE
    if _DATABASE_READY:
        return
    if _BOOTSTRAPPING_DATABASE:
        return

    _BOOTSTRAPPING_DATABASE = True
    try:
        if database_has_embeddings():
            _DATABASE_READY = True
            return

        ensure_postgres_running()
        ensure_database_exists()
        if not database_has_embeddings():
            load_embeddings_export()
        if not database_has_embeddings():
            raise RuntimeError(f"No embedding tables were loaded into database '{POSTGRES_DB}'")
        _DATABASE_READY = True
    finally:
        _BOOTSTRAPPING_DATABASE = False


def load_json_from_psql(database: str, sql: str, variables: dict[str, str] | None = None) -> Any:
    output = run_psql(database, sql, variables)
    return json.loads(output or "[]")


def embedding_tables() -> list[str]:
    ensure_database_ready()
    sql = """
SELECT COALESCE(jsonb_agg(table_name ORDER BY table_name), '[]'::jsonb)
FROM (
  SELECT DISTINCT table_name
  FROM information_schema.columns
  WHERE table_schema = 'public'
    AND column_name = 'embedding'
    AND udt_name = 'vector'
) tables;
"""
    return list(load_json_from_psql(POSTGRES_DB, sql))


def all_source_rows() -> list[dict[str, Any]]:
    tables = embedding_tables()
    if not tables:
        return []

    selects = []
    for table in tables:
        selects.append(
            "SELECT "
            f"{sql_literal(table)} AS table_name, "
            "repo, source_path, source_kind, created_at::text "
            f"FROM {quote_identifier(table)}"
        )

    sql = f"""
SELECT COALESCE(jsonb_agg(to_jsonb(rows) ORDER BY table_name, repo, source_path), '[]'::jsonb)
FROM (
  {' UNION ALL '.join(selects)}
) rows;
"""
    return list(load_json_from_psql(POSTGRES_DB, sql))


def source_doc_path(repo: str, source_path: str) -> Path:
    filename = source_path.replace("/", "__")
    if filename in {"", "."}:
        filename = "__repo_root__"
    return SOURCE_DOCS_DIR / repo / f"{filename}.txt"


def read_source_doc(row: dict[str, Any], max_chars: int) -> str:
    path = source_doc_path(str(row["repo"]), str(row["source_path"]))
    if not path.is_file():
        return f"[source document not found: {path}]"

    text = path.read_text(encoding="utf-8", errors="replace")
    if len(text) <= max_chars:
        return text
    return text[:max_chars] + f"\n\n[truncated at {max_chars} characters]"


def context_for_rows(rows: list[dict[str, Any]], max_chars_per_source: int, include_scores: bool = False) -> str:
    if not rows:
        return "No embedding context rows matched."

    parts = [
        "Istio embedding context loaded from local Postgres.",
        f"Database: {POSTGRES_DB} on {POSTGRES_HOST}:{POSTGRES_PORT}",
        f"Source docs: {SOURCE_DOCS_DIR}",
    ]
    for index, row in enumerate(rows, start=1):
        header = [
            f"## {index}. {row.get('repo')}/{row.get('source_path')}",
            f"table: {row.get('table_name')}",
            f"source_kind: {row.get('source_kind')}",
            f"created_at: {row.get('created_at')}",
        ]
        if include_scores and "distance" in row:
            header.append(f"distance: {row.get('distance')}")
        parts.append("\n".join(header))
        parts.append(read_source_doc(row, max_chars_per_source))

    return "\n\n".join(parts)


def filter_rows(args: dict[str, Any]) -> list[dict[str, Any]]:
    rows = all_source_rows()
    table = args.get("table")
    repo = args.get("repo")
    source_path = args.get("source_path")

    if table:
        rows = [row for row in rows if row.get("table_name") == table]
    if repo:
        rows = [row for row in rows if row.get("repo") == repo]
    if source_path:
        rows = [row for row in rows if row.get("source_path") == source_path]

    limit = clamp_int(args.get("limit"), 20, 1, 100)
    return rows[:limit]


def find_copilot_command() -> str:
    if COPILOT_COMMAND:
        return COPILOT_COMMAND
    if shutil.which("agency"):
        return "agency copilot"
    if shutil.which("agency.exe"):
        return "agency.exe copilot"
    raise RuntimeError("COPILOT_COMMAND is not set and agency/agency.exe was not found")


def load_create_embedding_module() -> Any:
    global _CREATE_EMBEDDING_MODULE
    if _CREATE_EMBEDDING_MODULE is not None:
        return _CREATE_EMBEDDING_MODULE
    if not CREATE_EMBEDDING_PY.is_file():
        raise RuntimeError(f"create_embedding.py not found at {CREATE_EMBEDDING_PY}")

    spec = importlib.util.spec_from_file_location("istio_create_embedding", CREATE_EMBEDDING_PY)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Could not load create_embedding.py from {CREATE_EMBEDDING_PY}")

    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    _CREATE_EMBEDDING_MODULE = module
    return module


def dimensions() -> int:
    try:
        parsed = int(DIMENSIONS)
    except ValueError as exc:
        raise RuntimeError(f"DIMENSIONS must be an integer, got {DIMENSIONS!r}") from exc
    if parsed <= 0:
        raise RuntimeError(f"DIMENSIONS must be greater than zero, got {parsed}")
    return parsed


def create_query_embedding_locally(query: str) -> str:
    module = load_create_embedding_module()
    vector = module.create_embedding(query, query, dimensions())
    return module.vector_literal(vector)


def create_query_embedding_with_copilot(query: str) -> str:
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", suffix=".txt", delete=False) as temp:
        temp.write(query)
        temp_path = temp.name

    try:
        cmd = [
            sys.executable,
            str(CREATE_EMBEDDING_PY),
            "--path",
            temp_path,
            "--dimensions",
            DIMENSIONS,
            "--copilot-command",
            find_copilot_command(),
            "--copilot-timeout",
            COPILOT_TIMEOUT,
            "--format",
            "pgvector",
        ]
        result = subprocess.run(cmd, text=True, capture_output=True, check=False)
        if result.returncode != 0:
            raise RuntimeError(result.stderr.strip() or result.stdout.strip() or "embedding generation failed")
        vector = result.stdout.strip()
        if not vector.startswith("[") or not vector.endswith("]"):
            raise RuntimeError("embedding generation did not return a pgvector literal")
        return vector
    finally:
        Path(temp_path).unlink(missing_ok=True)


def create_query_embedding(query: str) -> str:
    if QUERY_EMBEDDING_USE_COPILOT:
        return create_query_embedding_with_copilot(query)
    return create_query_embedding_locally(query)


def semantic_search(args: dict[str, Any]) -> list[dict[str, Any]]:
    query = str(args.get("query", "")).strip()
    if not query:
        raise ValueError("query is required")

    tables = embedding_tables()
    if not tables:
        return []

    query_embedding = create_query_embedding(query)
    top_k = clamp_int(args.get("top_k"), 5, 1, 20)
    selects = []
    for table in tables:
        selects.append(
            "SELECT "
            f"{sql_literal(table)} AS table_name, "
            "repo, source_path, source_kind, created_at::text, "
            "(embedding <=> :'query_embedding'::vector) AS distance "
            f"FROM {quote_identifier(table)}"
        )

    sql = f"""
SELECT COALESCE(jsonb_agg(to_jsonb(ranked) ORDER BY distance), '[]'::jsonb)
FROM (
  SELECT *
  FROM (
    {' UNION ALL '.join(selects)}
  ) rows
  ORDER BY distance
  LIMIT {top_k}
) ranked;
"""
    return list(load_json_from_psql(POSTGRES_DB, sql, {"query_embedding": query_embedding}))


def tool_list_sources(_: dict[str, Any]) -> str:
    rows = all_source_rows()
    return json.dumps(
        {
            "database": POSTGRES_DB,
            "host": POSTGRES_HOST,
            "port": POSTGRES_PORT,
            "source_docs_dir": str(SOURCE_DOCS_DIR),
            "count": len(rows),
            "sources": rows,
        },
        indent=2,
    )


def tool_get_context(args: dict[str, Any]) -> str:
    max_chars = clamp_int(args.get("max_chars_per_source"), 6000, 200, 50000)
    return context_for_rows(filter_rows(args), max_chars)


def tool_semantic_search(args: dict[str, Any]) -> str:
    max_chars = clamp_int(args.get("max_chars_per_source"), 6000, 200, 50000)
    rows = semantic_search(args)
    return context_for_rows(rows, max_chars, include_scores=True)


TOOLS = {
    "list_embedding_sources": tool_list_sources,
    "get_embedding_context": tool_get_context,
    "semantic_search_embedding_context": tool_semantic_search,
}


def tool_definitions() -> list[dict[str, Any]]:
    return [
        {
            "name": "list_embedding_sources",
            "description": "List embedding rows available in the local Istio pgvector database.",
            "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
        },
        {
            "name": "get_embedding_context",
            "description": "Load source text associated with embeddings from Postgres into LLM context.",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "table": {"type": "string", "description": "Optional embedding table name."},
                    "repo": {"type": "string", "description": "Optional repository name."},
                    "source_path": {"type": "string", "description": "Optional source path, such as interface or Makefile."},
                    "limit": {"type": "integer", "minimum": 1, "maximum": 100, "default": 20},
                    "max_chars_per_source": {"type": "integer", "minimum": 200, "maximum": 50000, "default": 6000},
                },
                "additionalProperties": False,
            },
        },
        {
            "name": "semantic_search_embedding_context",
            "description": "Embed a query, search pgvector rows by distance, and return matched source text as LLM context.",
            "inputSchema": {
                "type": "object",
                "required": ["query"],
                "properties": {
                    "query": {"type": "string", "description": "Question or topic to search for."},
                    "top_k": {"type": "integer", "minimum": 1, "maximum": 20, "default": 5},
                    "max_chars_per_source": {"type": "integer", "minimum": 200, "maximum": 50000, "default": 6000},
                },
                "additionalProperties": False,
            },
        },
    ]


def row_resource_uri(row: dict[str, Any]) -> str:
    return (
        "istio-embeddings://source/"
        + quote(str(row["table_name"]), safe="")
        + "/"
        + quote(str(row["repo"]), safe="")
        + "/"
        + quote(str(row["source_path"]), safe="")
    )


def list_resources() -> list[dict[str, Any]]:
    resources = [
        {
            "uri": "istio-embeddings://catalog",
            "name": "Istio embedding source catalog",
            "description": "JSON catalog of embedding rows loaded from Postgres.",
            "mimeType": "application/json",
        },
        {
            "uri": "istio-embeddings://context/all",
            "name": "All Istio embedding context",
            "description": "All source documents referenced by the embedding rows.",
            "mimeType": "text/plain",
        },
    ]
    for row in all_source_rows():
        resources.append(
            {
                "uri": row_resource_uri(row),
                "name": f"{row['repo']}/{row['source_path']}",
                "description": f"Source context from {row['table_name']}.",
                "mimeType": "text/plain",
            }
        )
    return resources


def read_resource(uri: str) -> tuple[str, str]:
    if uri == "istio-embeddings://catalog":
        return "application/json", tool_list_sources({})
    if uri == "istio-embeddings://context/all":
        return "text/plain", context_for_rows(all_source_rows(), 6000)

    prefix = "istio-embeddings://source/"
    if uri.startswith(prefix):
        parts = uri[len(prefix) :].split("/", 2)
        if len(parts) != 3:
            raise ValueError(f"Invalid source resource URI: {uri}")
        table, repo, source_path = [unquote(part) for part in parts]
        rows = filter_rows({"table": table, "repo": repo, "source_path": source_path, "limit": 1})
        return "text/plain", context_for_rows(rows, 50000)

    raise ValueError(f"Unknown resource URI: {uri}")


def success(message_id: Any, result: Any) -> dict[str, Any]:
    return {"jsonrpc": "2.0", "id": message_id, "result": result}


def failure(message_id: Any, code: int, message: str) -> dict[str, Any]:
    return {"jsonrpc": "2.0", "id": message_id, "error": {"code": code, "message": message}}


def handle_request(message: dict[str, Any]) -> dict[str, Any] | None:
    message_id = message.get("id")
    method = message.get("method")
    params = message.get("params") or {}

    if method and method.startswith("notifications/"):
        return None

    try:
        if method == "initialize":
            return success(
                message_id,
                {
                    "protocolVersion": params.get("protocolVersion", "2024-11-05"),
                    "capabilities": {"tools": {}, "resources": {}},
                    "serverInfo": {"name": "istio-embeddings-mcp", "version": "1.0.0"},
                },
            )
        if method == "ping":
            return success(message_id, {})
        if method == "tools/list":
            return success(message_id, {"tools": tool_definitions()})
        if method == "tools/call":
            name = params.get("name")
            args = params.get("arguments") or {}
            if name not in TOOLS:
                raise ValueError(f"Unknown tool: {name}")
            text = TOOLS[name](args)
            return success(message_id, {"content": [{"type": "text", "text": text}], "isError": False})
        if method == "resources/list":
            return success(message_id, {"resources": list_resources()})
        if method == "resources/read":
            uri = params.get("uri")
            if not isinstance(uri, str):
                raise ValueError("resources/read requires a string uri")
            mime_type, text = read_resource(uri)
            return success(message_id, {"contents": [{"uri": uri, "mimeType": mime_type, "text": text}]})
        return failure(message_id, -32601, f"Method not found: {method}")
    except Exception as exc:
        log(str(exc))
        if method == "tools/call":
            return success(message_id, {"content": [{"type": "text", "text": f"ERROR: {exc}"}], "isError": True})
        return failure(message_id, -32000, str(exc))


def write_message(message: dict[str, Any]) -> None:
    print(json.dumps(message, separators=(",", ":")), flush=True)


def main() -> None:
    log(f"using database {POSTGRES_DB} on {POSTGRES_HOST}:{POSTGRES_PORT}")
    if POSTGRES_AUTO_INIT:
        log(f"auto-init enabled; data dir {POSTGRES_DATA_DIR}")
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            message = json.loads(line)
        except json.JSONDecodeError as exc:
            write_message(failure(None, -32700, f"Parse error: {exc}"))
            continue

        response = handle_request(message)
        if response is not None:
            write_message(response)


if __name__ == "__main__":
    main()
