from __future__ import annotations

from dataclasses import asdict, dataclass
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
from typing import Callable

from .security import SecurityViolation, WorkspacePolicy
from .state_store import StateStore


@dataclass(frozen=True)
class ActionSpec:
    name: str
    approval_scope: str | None
    command: Callable[[Path], list[str]]
    timeout_seconds: float


@dataclass(frozen=True)
class ActionResult:
    action: str
    command: list[str]
    exit_code: int
    stdout: str
    stderr: str
    timed_out: bool
    started_at: str
    finished_at: str


def default_action_specs() -> dict[str, ActionSpec]:
    return {
        "git-status": ActionSpec(
            "git-status", None, lambda root: ["git", "status", "--short", "--branch"], 30.0
        ),
        "rhi-boundary": ActionSpec(
            "rhi-boundary",
            None,
            lambda root: [
                sys.executable,
                str(root / "tools" / "check_rhi_boundary.py"),
                "--root",
                str(root),
                "--baseline",
                str(root / ".rhi-boundary-baseline.json"),
                "--allowlist",
                str(root / ".rhi-boundary-allow"),
            ],
            120.0,
        ),
        "configure-dx12": ActionSpec(
            "configure-dx12",
            "build",
            lambda root: [
                "cmake",
                "-S",
                str(root),
                "-B",
                str(root / "out" / "build" / "beeai-dx12-foundation"),
                "-DDEMO_ENABLE_D3D12=ON",
                "-DBUILD_TESTING=ON",
                "-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
                "-DUSE_SLANG=ON",
            ],
            300.0,
        ),
        "build-dx12": ActionSpec(
            "build-dx12",
            "build",
            lambda root: [
                "cmake",
                "--build",
                str(root / "out" / "build" / "beeai-dx12-foundation"),
                "--target",
                "Demo",
                "rhi_backend_factory_tests",
                "d3d12_device_smoke_tests",
                "--parallel",
                "2",
            ],
            1800.0,
        ),
        "test-dx12": ActionSpec(
            "test-dx12",
            "build",
            lambda root: [
                "ctest",
                "--test-dir",
                str(root / "out" / "build" / "beeai-dx12-foundation"),
                "--output-on-failure",
                "-R",
                "rhi_backend_factory|d3d12_device_smoke",
            ],
            900.0,
        ),
    }


class NamedActionRunner:
    """Runs fixed command templates and records immutable evidence."""

    def __init__(
        self,
        workspace: str | Path,
        store: StateStore,
        evidence_root: str | Path,
        specs: dict[str, ActionSpec] | None = None,
        *,
        output_limit: int = 4 * 1024 * 1024,
    ) -> None:
        self.policy = WorkspacePolicy(workspace)
        self.store = store
        self.evidence_root = self.policy.resolve(evidence_root)
        self.specs = specs or default_action_specs()
        self.output_limit = output_limit

    def run(self, run_id: str, action: str) -> ActionResult:
        spec = self.specs.get(action)
        if spec is None:
            raise SecurityViolation(f"unknown named action: {action}")
        if spec.approval_scope and not self.store.has_approval(run_id, spec.approval_scope):
            raise SecurityViolation(
                f"action '{action}' requires approval scope '{spec.approval_scope}' for run {run_id}"
            )

        command = spec.command(self.policy.root)
        if not command or any(not isinstance(part, str) or not part for part in command):
            raise RuntimeError(f"invalid command template for action: {action}")
        started_at = datetime.now(timezone.utc).isoformat()
        timed_out = False
        try:
            completed = subprocess.run(
                command,
                cwd=self.policy.root,
                check=False,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=spec.timeout_seconds,
            )
            exit_code = completed.returncode
            stdout = completed.stdout
            stderr = completed.stderr
        except subprocess.TimeoutExpired as exc:
            timed_out = True
            exit_code = -1
            stdout = exc.stdout if isinstance(exc.stdout, str) else ""
            stderr = exc.stderr if isinstance(exc.stderr, str) else ""

        stdout = stdout[: self.output_limit]
        stderr = stderr[: self.output_limit]
        result = ActionResult(
            action=action,
            command=command,
            exit_code=exit_code,
            stdout=stdout,
            stderr=stderr,
            timed_out=timed_out,
            started_at=started_at,
            finished_at=datetime.now(timezone.utc).isoformat(),
        )
        evidence_path = self.evidence_root / run_id / "actions" / f"{action}.json"
        evidence_path.parent.mkdir(parents=True, exist_ok=True)
        temporary = evidence_path.with_suffix(".json.tmp")
        temporary.write_text(json.dumps(asdict(result), indent=2, sort_keys=True) + "\n", encoding="utf-8")
        temporary.replace(evidence_path)
        digest = hashlib.sha256(evidence_path.read_bytes()).hexdigest()
        self._record_artifact(run_id, action, evidence_path, digest)
        return result

    def _record_artifact(self, run_id: str, action: str, path: Path, digest: str) -> None:
        if not self.store.get_run(run_id):
            raise KeyError(f"unknown run: {run_id}")
        connection = sqlite3.connect(self.store.path)
        try:
            with connection:
                connection.execute(
                    "INSERT OR REPLACE INTO artifacts(run_id, kind, path, sha256, created_at) "
                    "VALUES(?, ?, ?, ?, ?)",
                    (
                        run_id,
                        f"action:{action}",
                        str(path),
                        digest,
                        datetime.now(timezone.utc).isoformat(),
                    ),
                )
        finally:
            connection.close()
