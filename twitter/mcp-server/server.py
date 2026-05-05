"""twitter-gen MCP server.

Exposes project markdown docs as resources and a 'generate' tool that returns
SYSTEM_PROMPT.md + AGENCY.md as code-generation instructions.
"""

import os
import sys
from pathlib import Path

from mcp.server.fastmcp import FastMCP

SERVER_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SERVER_DIR.parent

mcp = FastMCP("twitter-gen")

def _read_project_doc(name: str) -> str:
    path = PROJECT_ROOT / name
    return path.read_text(encoding="utf-8")


def _list_design_docs() -> list[str]:
    return sorted(
        f.name for f in PROJECT_ROOT.iterdir()
        if f.is_file() and f.suffix.lower() == ".md"
    )


# --- Resources: expose every markdown doc in the project root ---
for doc_name in _list_design_docs():
    uri = "docs://" + doc_name.lower().removesuffix(".md")

    # Use a factory to capture doc_name in each closure
    def _make_reader(name: str, resource_uri: str):
        @mcp.resource(resource_uri, name=name, description=f"Project design document: {name}", mime_type="text/markdown")
        def _read() -> str:
            return _read_project_doc(name)
    _make_reader(doc_name, uri)


# --- Tool: generate ---
@mcp.tool(description=(
    "Kick off code generation for the Twitter clone project. "
    "Returns SYSTEM_PROMPT.md followed by AGENCY.md as the generation instructions."
))
def generate() -> str:
    """Return the system prompt and agency doc concatenated."""
    system_prompt_path = SERVER_DIR / "SYSTEM_PROMPT.md"
    system_prompt = system_prompt_path.read_text(encoding="utf-8").strip()

    agency = _read_project_doc("AGENCY.md").strip()

    return (
        f"<system>\n{system_prompt}\n</system>\n\n"
        f"<agency>\n{agency}\n</agency>\n"
    )


if __name__ == "__main__":
    mcp.run(transport="stdio")
