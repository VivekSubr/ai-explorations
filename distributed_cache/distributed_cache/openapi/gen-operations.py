#!/usr/bin/env python3
"""Generate generated_operations.h from distributed_cache.yaml.

Emits a C++ header listing every (path, method) operation defined in the
spec. The header is consumed by rest.cc so that adding/removing an
operation in the YAML produces a compile error (non-exhaustive switch on
the OpId enum) rather than silent drift.

Usage:
    gen-operations.py [--spec PATH] [--out PATH]

Defaults to the spec sitting alongside this script and writes the header
to <caller_cwd>/generated_operations.h .
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

try:
    import yaml
except ImportError:
    sys.exit("PyYAML is required: pip install pyyaml")

HERE = Path(__file__).resolve().parent
DEFAULT_SPEC = HERE / "distributed_cache.yaml"


def to_camel(s: str) -> str:
    parts = re.split(r"[^A-Za-z0-9]+", s)
    return "".join(p[:1].upper() + p[1:] for p in parts if p)


def op_id(path: str, method: str) -> str:
    # /json -> Json, /{key} -> Key, /a/{b}/c -> AByC
    cleaned = re.sub(r"[{}]", "", path).strip("/")
    base = to_camel(cleaned) if cleaned else "Root"
    return f"{base}{method.title()}"


def collect_operations(spec: dict) -> list[dict]:
    ops = []
    methods = ("get", "put", "post", "delete", "patch", "options", "head")
    for path, item in (spec.get("paths") or {}).items():
        for m in methods:
            entry = item.get(m)
            if entry is None:
                continue
            req = entry.get("requestBody") or {}
            ops.append({
                "id": op_id(path, m),
                "method": m.upper(),
                "path": path,
                "has_body": bool(req),
                "summary": entry.get("summary", ""),
            })
    ops.sort(key=lambda o: (o["path"], o["method"]))
    return ops


def collect_error_codes(spec: dict) -> list[str]:
    schemas = ((spec.get("components") or {}).get("schemas") or {})
    ec = schemas.get("ErrorCode") or {}
    return list(ec.get("enum") or [])


HEADER_TMPL = """\
// AUTO-GENERATED from openapi/distributed_cache.yaml by gen-operations.py.
// DO NOT EDIT. Run `make generate` to regenerate.
//
// This header binds rest.cc to the OpenAPI spec at the operation level:
// every (path, method) pair defined in the YAML appears here as an OpId
// enumerator. rest.cc dispatches via `switch(OpId)`; with -Werror=switch
// any added or removed operation in the spec breaks the build at exactly
// the call site that needs updating.
#pragma once

#include <cstddef>
#include <string_view>

#include "ErrorCode.h"

namespace dist_cache::spec {{

enum class OpId {{
{op_enum}
}};

struct OpDecl {{
    OpId             id;
    std::string_view method;        // e.g. "GET"
    std::string_view path_template; // e.g. "/{{key}}"
    bool             has_body;
}};

inline constexpr OpDecl kOperations[] = {{
{op_table}
}};
inline constexpr std::size_t kOperationCount = sizeof(kOperations) / sizeof(kOperations[0]);

// Stable, machine-readable error identifiers. Mirrors the ErrorCode enum in
// the spec: each constant is the typed enumerator from the generated
// `ErrorCode` model, so passing a stale code to `Error::setError` is a
// compile error rather than a runtime mismatch.
namespace err {{
using EC = ::org::openapitools::server::model::ErrorCode::eErrorCode;
{err_constants}
}}

}}  // namespace dist_cache::spec
"""


def render(ops: list[dict], errs: list[str]) -> str:
    op_enum = "\n".join(f"    {o['id']}," for o in ops)
    op_table = "\n".join(
        f'    {{OpId::{o["id"]}, "{o["method"]}", "{o["path"]}", {"true" if o["has_body"] else "false"}}},'
        for o in ops
    )
    err_constants = "\n".join(
        f'inline constexpr EC k{to_camel(e)} = EC::{e.upper()};'
        for e in errs
    )
    return HEADER_TMPL.format(op_enum=op_enum, op_table=op_table,
                              err_constants=err_constants)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--spec", type=Path, default=DEFAULT_SPEC)
    ap.add_argument("--out",  type=Path,
                    default=Path.cwd() / "generated_operations.h")
    args = ap.parse_args()

    spec = yaml.safe_load(args.spec.read_text())
    ops = collect_operations(spec)
    errs = collect_error_codes(spec)
    if not ops:
        sys.exit(f"no operations found in {args.spec}")
    if not errs:
        sys.exit(f"no ErrorCode enum found in {args.spec}")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(render(ops, errs))
    print(f"wrote {args.out}  ({len(ops)} ops, {len(errs)} error codes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
