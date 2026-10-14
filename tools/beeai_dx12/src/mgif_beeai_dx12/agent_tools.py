from __future__ import annotations

import json
from pathlib import Path

from beeai_framework.tools import StringToolOutput, tool
from pydantic import BaseModel, ConfigDict, Field, field_validator

from .security import SecurityViolation, WorkspaceFileBackend, WorkspacePolicy


_SOURCE_SUFFIXES = {".cpp", ".h", ".hpp", ".cmake", ".slang", ".md", ".txt"}
_SOURCE_NAMES = {"CMakeLists.txt", "CMakePresets.json"}


class _ToolInput(BaseModel):
    model_config = ConfigDict(extra="forbid")


class ReadSourceInput(_ToolInput):
    path: str = Field(description="Workspace-relative source path")
    line: int = Field(default=1, ge=1)
    limit: int = Field(default=240, ge=1, le=500)


class SearchSourceInput(_ToolInput):
    query: str = Field(min_length=1, max_length=160, description="Literal case-insensitive search text")
    root: str = Field(default="rhi", description="Workspace-relative directory or file")
    max_results: int = Field(default=60, ge=1, le=120)

    @field_validator("query")
    @classmethod
    def reject_control_characters(cls, value: str) -> str:
        if any(ord(char) < 32 for char in value):
            raise ValueError("search query cannot contain control characters")
        return value


def create_source_tools(workspace: str | Path, audit: list[dict[str, object]] | None = None):
    workspace_root = Path(workspace).resolve(strict=True)
    policy = WorkspacePolicy(workspace_root)
    backend = WorkspaceFileBackend(workspace_root)
    trace = audit if audit is not None else []

    def resolve_relative(path: str) -> Path:
        candidate = Path(path)
        if candidate.is_absolute() or ".." in candidate.parts:
            raise SecurityViolation("agent tool paths must be workspace-relative and traversal-free")
        return policy.resolve(workspace_root / candidate)

    @tool(name="read_source", input_schema=ReadSourceInput)
    async def read_source(path: str, line: int = 1, limit: int = 240) -> StringToolOutput:
        """Read a bounded line range from one source file in the MGIF workspace."""
        resolved = resolve_relative(path)
        content = await backend.read_text(str(resolved), line=line, limit=limit)
        trace.append(
            {
                "tool": "read_source",
                "path": resolved.relative_to(workspace_root).as_posix(),
                "line": line,
                "limit": limit,
                "returned_chars": len(content),
            }
        )
        return StringToolOutput(
            json.dumps({"path": resolved.relative_to(workspace_root).as_posix(), "line": line, "content": content})
        )

    @tool(name="search_source", input_schema=SearchSourceInput)
    async def search_source(query: str, root: str = "rhi", max_results: int = 60) -> StringToolOutput:
        """Search literal text in bounded MGIF source files; no shell command is executed."""
        search_root = resolve_relative(root)
        candidates = [search_root] if search_root.is_file() else search_root.rglob("*")
        needle = query.casefold()
        matches: list[dict[str, object]] = []
        scanned = 0
        for candidate in candidates:
            if len(matches) >= max_results or scanned >= 2000:
                break
            if not candidate.is_file() or (
                candidate.name not in _SOURCE_NAMES and candidate.suffix.lower() not in _SOURCE_SUFFIXES
            ):
                continue
            try:
                safe_path = policy.resolve(candidate)
                if safe_path.stat().st_size > 2 * 1024 * 1024:
                    continue
                lines = safe_path.read_text(encoding="utf-8", errors="replace").splitlines()
            except (OSError, SecurityViolation):
                continue
            scanned += 1
            for line_number, text in enumerate(lines, 1):
                if needle in text.casefold():
                    matches.append(
                        {
                            "path": safe_path.relative_to(policy.root).as_posix(),
                            "line": line_number,
                            "text": text[:500],
                        }
                    )
                    if len(matches) >= max_results:
                        break
        trace.append(
            {
                "tool": "search_source",
                "root": search_root.relative_to(workspace_root).as_posix(),
                "query_sha256": __import__("hashlib").sha256(query.encode("utf-8")).hexdigest(),
                "match_count": len(matches),
                "scanned_files": scanned,
            }
        )
        return StringToolOutput(json.dumps({"query": query, "matches": matches, "scanned_files": scanned}))

    return [search_source, read_source]
