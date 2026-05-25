#!/usr/bin/env python3
"""
Create a single embedding from text, a file, a folder tree, or stdin using
Agency Copilot.

The default output is a pgvector-compatible vector literal that is also a JSON
numeric array, so it can be cast with ::vector in PostgreSQL or consumed by
other embedding-aware tools. The vector is deterministic for the same input,
Agency Copilot output, and dimension count.
"""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
from pathlib import Path
from typing import Any


DEFAULT_DIMENSIONS = 1536
DEFAULT_COPILOT_COMMAND = "agency copilot"
WINDOWS_COPILOT_COMMAND = "cmd.exe /c agency copilot"
WSL_CMD_EXE = Path("/mnt/c/Windows/System32/cmd.exe")
EMBEDDING_MODEL = "agency-copilot-hash-v1"
TEXT_ENCODINGS = ("utf-8-sig", "utf-16", "cp1252")
TOKEN_RE = re.compile(r"[A-Za-z0-9_]+(?:[-'][A-Za-z0-9_]+)?")


def normalize_windows_path(value: Path) -> str:
    return str(value).replace("/", "\\")


def quote_for_command(value: str) -> str:
    return subprocess.list2cmdline([value])


def agency_command(args: argparse.Namespace) -> str:
    configured_command = args.copilot_command or os.getenv("AGENCY_COPILOT_COMMAND")
    if configured_command:
        return configured_command

    if shutil.which("agency"):
        return DEFAULT_COPILOT_COMMAND
    if shutil.which("agency.exe"):
        return "agency.exe copilot"
    if shutil.which("cmd.exe"):
        return WINDOWS_COPILOT_COMMAND
    if WSL_CMD_EXE.exists():
        return f"{WSL_CMD_EXE} /c agency copilot"

    return DEFAULT_COPILOT_COMMAND


def decode_file(path: Path, encoding: str | None) -> str:
    data = path.read_bytes()
    if encoding:
        try:
            return data.decode(encoding)
        except UnicodeDecodeError as exc:
            raise SystemExit(f"Could not decode {path} with {encoding}.") from exc

    for candidate in TEXT_ENCODINGS:
        try:
            return data.decode(candidate)
        except UnicodeDecodeError:
            continue

    raise SystemExit(
        f"Could not decode {path}. Pass --encoding with the file's text encoding."
    )


def iter_files(root: Path) -> list[Path]:
    files: list[Path] = []
    for current_root, dirs, file_names in os.walk(root):
        dirs.sort(key=str.casefold)
        file_names.sort(key=str.casefold)
        current_path = Path(current_root)
        for file_name in file_names:
            path = current_path / file_name
            if path.is_file():
                files.append(path)
    return files


def read_path(path: Path, encoding: str | None) -> str:
    resolved = path.expanduser().resolve()
    if resolved.is_file():
        return decode_file(resolved, encoding)
    if not resolved.is_dir():
        raise SystemExit(f"Path does not exist or is not a file/folder: {resolved}")

    files = iter_files(resolved)
    if not files:
        raise SystemExit(f"No files found under folder: {resolved}")

    sections: list[str] = []
    for file_path in files:
        relative_path = file_path.relative_to(resolved)
        content = decode_file(file_path, encoding)
        sections.append(
            "\n".join(
                [
                    f"--- FILE: {relative_path} ---",
                    content,
                    f"--- END FILE: {relative_path} ---",
                ]
            )
        )
    return "\n\n".join(sections)


def describe_input_source(args: argparse.Namespace) -> str:
    if args.path is not None:
        return normalize_windows_path(args.path.expanduser().resolve())
    if args.input_file is not None:
        return normalize_windows_path(args.input_file.expanduser().resolve())
    if args.text:
        return "command-line text"
    return "stdin"


def read_input_text(args: argparse.Namespace) -> str:
    has_text = bool(args.text)
    has_file = args.input_file is not None
    has_path = args.path is not None
    source_count = sum([has_text, has_file, has_path])
    if source_count > 1:
        raise SystemExit("Provide only one input source: text, --path, or --input-file.")

    if has_path:
        return read_path(args.path, args.encoding)

    if has_file:
        path = args.input_file.expanduser().resolve()
        if not path.is_file():
            raise SystemExit(f"Input file does not exist or is not a file: {path}")
        return decode_file(path, args.encoding)

    if has_text:
        return " ".join(args.text)

    if not sys.stdin.isatty():
        return sys.stdin.read()

    raise SystemExit("Provide text, pass --path/--input-file, or pipe text on stdin.")


def build_copilot_prompt(source_label: str, text: str) -> str:
    return textwrap.dedent(
        f"""
        You are preparing one semantic embedding document for PostgreSQL pgvector.

        Read the source content and return concise, searchable plain text that
        captures the important entities, topics, APIs, settings, decisions,
        relationships, and behavior. Include names and domain terms that should
        affect semantic retrieval. Do not return JSON, markdown code fences, or
        a numeric vector; this script will create the pgvector-compatible vector
        from your semantic text.

        Source: {source_label}

        Source content:
        {text}
        """
    ).strip()


def run_agency_copilot(
    command_template: str,
    prompt: str,
    source_label: str,
    timeout_seconds: int,
) -> str:
    with tempfile.NamedTemporaryFile(
        "w",
        encoding="utf-8",
        suffix=".prompt.txt",
        delete=False,
    ) as prompt_file:
        prompt_file.write(prompt)
        prompt_file_path = prompt_file.name

    try:
        replacements = {
            "{prompt_file}": quote_for_command(prompt_file_path),
            "{source_path}": quote_for_command(source_label),
            "{path}": quote_for_command(source_label),
        }
        command = command_template
        for placeholder, value in replacements.items():
            command = command.replace(placeholder, value)

        uses_prompt_file = "{prompt_file}" in command_template
        result = subprocess.run(
            command,
            input=None if uses_prompt_file else prompt,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            shell=True,
            timeout=timeout_seconds,
            check=False,
        )
    finally:
        Path(prompt_file_path).unlink(missing_ok=True)

    output = result.stdout.strip()
    diagnostics = result.stderr.strip()
    if result.returncode != 0:
        detail = diagnostics or output or "no output"
        if result.returncode == 127 and "agency" in command_template:
            raise RuntimeError(
                "Agency Copilot command was not found. Install the agency CLI, put it "
                "on PATH, set AGENCY_COPILOT_COMMAND, or on WSL run with "
                "--copilot-command \"cmd.exe /c agency copilot\"."
            )
        raise RuntimeError(
            f"Agency Copilot command failed with exit code {result.returncode}: {detail}"
        )
    if not output:
        raise RuntimeError("Agency Copilot command produced no output.")
    return output


def feature_hash_embedding(text: str, dimensions: int) -> list[float]:
    if dimensions <= 0:
        raise SystemExit("--dimensions must be greater than zero.")

    tokens = [match.group(0).lower() for match in TOKEN_RE.finditer(text)]
    if not tokens:
        raise RuntimeError("Could not build an embedding because no tokens were found.")

    features: Counter[str] = Counter(tokens)
    features.update(f"{left} {right}" for left, right in zip(tokens, tokens[1:], strict=False))

    vector = [0.0] * dimensions
    for feature, count in features.items():
        digest = hashlib.blake2b(feature.encode("utf-8"), digest_size=16).digest()
        index = int.from_bytes(digest[:8], "big") % dimensions
        sign = 1.0 if digest[8] & 1 else -1.0
        vector[index] += sign * (1.0 + math.log(count))

    magnitude = math.sqrt(sum(value * value for value in vector))
    if magnitude == 0:
        raise RuntimeError("Could not normalize an empty embedding vector.")

    return [value / magnitude for value in vector]


def create_embedding(source_text: str, copilot_text: str, dimensions: int) -> list[float]:
    embedding_text = "\n\n".join(
        [
            "Agency Copilot semantic text:",
            copilot_text,
            "Original source text:",
            source_text,
        ]
    )
    return feature_hash_embedding(embedding_text, dimensions)


def format_float(value: float) -> str:
    return repr(value)


def vector_literal(vector: list[float]) -> str:
    return "[" + ",".join(format_float(value) for value in vector) + "]"


def json_dumps(value: Any, pretty: bool) -> str:
    if pretty:
        return json.dumps(value, indent=2)
    return json.dumps(value, separators=(",", ":"))


def write_output(
    text: str,
    copilot_text: str,
    vector: list[float],
    output_format: str,
    pretty: bool,
) -> None:
    if output_format == "pgvector":
        print(vector_literal(vector))
        return

    if output_format == "array":
        print(json_dumps(vector, pretty))
        return

    payload = {
        "model": EMBEDDING_MODEL,
        "dimensions": len(vector),
        "input_sha256": hashlib.sha256(text.encode("utf-8")).hexdigest(),
        "copilot_sha256": hashlib.sha256(copilot_text.encode("utf-8")).hexdigest(),
        "embedding": vector,
        "pgvector": vector_literal(vector),
    }
    print(json_dumps(payload, pretty))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Create an Agency Copilot-assisted embedding from text, a file, a folder tree, or stdin.",
        usage="python create_embedding.py [text ...] [options]",
    )
    parser.add_argument(
        "text",
        nargs="*",
        help="Text to embed. If omitted, use --path, --input-file, or stdin.",
    )
    parser.add_argument(
        "--path",
        type=Path,
        help="Read text from a file or recursively from every file under a folder.",
    )
    parser.add_argument(
        "--input-file",
        type=Path,
        help="Read text to embed from a file. Prefer --path for new usage.",
    )
    parser.add_argument(
        "--encoding",
        help="Encoding for --path/--input-file. Defaults to utf-8-sig, then utf-16, then cp1252.",
    )
    parser.add_argument(
        "--copilot-command",
        help=(
            "Agency Copilot shell command. Defaults to AGENCY_COPILOT_COMMAND. "
            f"If unset, auto-detects '{DEFAULT_COPILOT_COMMAND}', 'agency.exe copilot', "
            f"or '{WINDOWS_COPILOT_COMMAND}' for WSL. The prompt is sent on stdin "
            "unless the command contains {prompt_file}."
        ),
    )
    parser.add_argument(
        "--copilot-timeout",
        "--embedding-timeout",
        dest="copilot_timeout",
        type=int,
        default=300,
        help="Seconds before the Agency Copilot command times out.",
    )
    parser.add_argument(
        "--dimensions",
        "--embedding-dimensions",
        dest="dimensions",
        type=int,
        default=DEFAULT_DIMENSIONS,
        help=f"Embedding vector dimensions. Default: {DEFAULT_DIMENSIONS}.",
    )
    parser.add_argument(
        "--format",
        choices=("pgvector", "array", "json"),
        default="pgvector",
        help=(
            "Output format. pgvector prints only [..] for SQL vector casts; "
            "array prints a JSON numeric array; json includes metadata. Default: pgvector."
        ),
    )
    parser.add_argument("--pretty", action="store_true", help="Pretty-print JSON output.")
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    text = read_input_text(args)
    if not text.strip():
        raise SystemExit("Input text is empty.")

    source_label = describe_input_source(args)
    copilot_text = run_agency_copilot(
        agency_command(args),
        build_copilot_prompt(source_label, text),
        source_label,
        args.copilot_timeout,
    )
    vector = create_embedding(text, copilot_text, args.dimensions)
    write_output(text, copilot_text, vector, args.format, args.pretty)
    return 0


if __name__ == "__main__":
    sys.exit(main())
