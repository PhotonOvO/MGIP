from __future__ import annotations

import asyncio
import os
from pathlib import Path
import stat
import tempfile
from typing import Any


class SecurityViolation(RuntimeError):
    """Raised when an agent operation escapes the configured policy."""


_WRITE_SUFFIXES = {
    ".bat",
    ".cmake",
    ".cmd",
    ".cpp",
    ".h",
    ".hpp",
    ".json",
    ".md",
    ".ps1",
    ".py",
    ".slang",
    ".txt",
    ".toml",
    ".yaml",
    ".yml",
}
_WRITE_NAMES = {"CMakeLists.txt", "CMakePresets.json"}
_DENIED_PARTS = {".git", ".vs", "build", "captures", "out", "resources"}
_DENIED_READ_NAMES = {".env", "id_rsa", "id_ed25519"}
_DENIED_READ_SUFFIXES = {".key", ".p12", ".pfx", ".pem"}


def _is_reparse_point(path: Path) -> bool:
    try:
        info = path.lstat()
    except FileNotFoundError:
        return False
    attributes = getattr(info, "st_file_attributes", 0)
    reparse_flag = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
    return path.is_symlink() or bool(attributes & reparse_flag)


class WorkspacePolicy:
    def __init__(self, root: str | Path) -> None:
        root_path = Path(root)
        if not root_path.is_absolute():
            raise ValueError("workspace root must be absolute")
        self.root = root_path.resolve(strict=True)

    def resolve(self, path: str | Path, *, for_write: bool = False) -> Path:
        candidate = Path(path)
        if not candidate.is_absolute():
            raise SecurityViolation(f"absolute path required: {path}")

        absolute = Path(os.path.abspath(candidate))
        try:
            relative = absolute.relative_to(self.root)
        except ValueError as exc:
            raise SecurityViolation(f"path is outside workspace: {path}") from exc

        cursor = self.root
        for part in relative.parts:
            cursor = cursor / part
            if cursor.exists() and _is_reparse_point(cursor):
                raise SecurityViolation(f"reparse points are not allowed: {cursor}")

        resolved = absolute.resolve(strict=False)
        try:
            resolved.relative_to(self.root)
        except ValueError as exc:
            raise SecurityViolation(f"resolved path is outside workspace: {path}") from exc

        lowered_parts = {part.lower() for part in relative.parts}
        if ".git" in lowered_parts:
            raise SecurityViolation("direct .git access is not allowed")

        if for_write:
            if lowered_parts & _DENIED_PARTS:
                raise SecurityViolation(f"writes to generated or protected paths are not allowed: {path}")
            if resolved.name not in _WRITE_NAMES and resolved.suffix.lower() not in _WRITE_SUFFIXES:
                raise SecurityViolation(f"file type is not writable by the agent: {path}")
        elif resolved.name.lower() in _DENIED_READ_NAMES or resolved.suffix.lower() in _DENIED_READ_SUFFIXES:
            raise SecurityViolation(f"secret-like file is not readable by the agent: {path}")

        return resolved


class WorkspaceFileBackend:
    """BeeAI FileBackend implementation confined to one MGIF worktree."""

    def __init__(
        self,
        root: str | Path,
        *,
        writable: bool = False,
        max_read_bytes: int = 4 * 1024 * 1024,
        max_write_bytes: int = 2 * 1024 * 1024,
    ) -> None:
        self.policy = WorkspacePolicy(root)
        self.writable = writable
        self.max_read_bytes = max_read_bytes
        self.max_write_bytes = max_write_bytes

    async def read_text(self, path: str, *, line: int | None = None, limit: int | None = None) -> str:
        resolved = self.policy.resolve(path)
        if resolved.stat().st_size > self.max_read_bytes:
            raise SecurityViolation(f"file exceeds read limit: {resolved}")
        content = resolved.read_text(encoding="utf-8")
        if line is None and limit is None:
            return content
        lines = content.splitlines(keepends=True)
        start = max(0, (line or 1) - 1)
        end = start + limit if limit is not None else len(lines)
        return "".join(lines[start:end])

    async def write_text(self, path: str, content: str) -> None:
        if not self.writable:
            raise SecurityViolation("source writes require an approved writable backend")
        encoded = content.encode("utf-8")
        if len(encoded) > self.max_write_bytes:
            raise SecurityViolation("write exceeds configured size limit")
        resolved = self.policy.resolve(path, for_write=True)
        resolved.parent.mkdir(parents=True, exist_ok=True)
        temp_name: str | None = None
        try:
            with tempfile.NamedTemporaryFile(
                mode="w", encoding="utf-8", newline="", delete=False, dir=resolved.parent
            ) as stream:
                temp_name = stream.name
                stream.write(content)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temp_name, resolved)
        finally:
            if temp_name and os.path.exists(temp_name):
                os.unlink(temp_name)


class RestrictedShellBackend:
    """Read-only ShellBackend for BeeAI's generic ShellTool."""

    _GIT_SUBCOMMANDS = {"diff", "log", "ls-files", "rev-parse", "show", "status"}
    _VERSION_COMMANDS = {"cmake", "python"}
    _DENIED_GIT_OPTIONS = {
        "-c",
        "-C",
        "--config-env",
        "--exec-path",
        "--git-dir",
        "--work-tree",
    }

    def __init__(self, root: str | Path, *, output_limit: int = 1024 * 1024) -> None:
        self.policy = WorkspacePolicy(root)
        self.output_limit = output_limit

    def validate(self, command: list[str], cwd: str | None, env: dict[str, str] | None) -> Path:
        if not command:
            raise SecurityViolation("empty command")
        if env:
            raise SecurityViolation("generic shell environment overrides are not allowed")
        working_dir = self.policy.resolve(cwd or str(self.policy.root))
        executable = Path(command[0]).name.lower()
        if executable in {"git", "git.exe"}:
            if len(command) < 2 or command[1] not in self._GIT_SUBCOMMANDS:
                raise SecurityViolation("git subcommand is not read-only or not allowed")
            if any(arg in self._DENIED_GIT_OPTIONS or arg.startswith("--git-dir=") for arg in command[2:]):
                raise SecurityViolation("git repository redirection is not allowed")
        elif executable in self._VERSION_COMMANDS or executable.removesuffix(".exe") in self._VERSION_COMMANDS:
            if command[1:] != ["--version"]:
                raise SecurityViolation("only version inspection is allowed for this command")
        else:
            raise SecurityViolation(f"generic shell command is not allowed: {command[0]}")
        return working_dir

    async def run(
        self,
        *,
        command: list[str],
        cwd: str | None = None,
        env: dict[str, str] | None = None,
        timeout_seconds: float | None = None,
        input_text: str | None = None,
    ) -> dict[str, Any]:
        if input_text is not None:
            raise SecurityViolation("generic shell stdin is not allowed")
        working_dir = self.validate(command, cwd, env)
        process = await asyncio.create_subprocess_exec(
            *command,
            cwd=working_dir,
            stdin=asyncio.subprocess.DEVNULL,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        timed_out = False
        try:
            stdout, stderr = await asyncio.wait_for(process.communicate(), timeout=timeout_seconds or 60.0)
        except TimeoutError:
            process.kill()
            stdout, stderr = await process.communicate()
            timed_out = True
        stdout_text = stdout.decode(errors="replace")
        stderr_text = stderr.decode(errors="replace")
        truncated = len(stdout_text) + len(stderr_text) > self.output_limit
        if truncated:
            remaining = self.output_limit
            stdout_text = stdout_text[:remaining]
            remaining -= len(stdout_text)
            stderr_text = stderr_text[: max(0, remaining)]
        return {
            "exit_code": process.returncode if process.returncode is not None else -1,
            "stdout": stdout_text,
            "stderr": stderr_text,
            "timed_out": timed_out,
            "truncated": truncated,
        }
