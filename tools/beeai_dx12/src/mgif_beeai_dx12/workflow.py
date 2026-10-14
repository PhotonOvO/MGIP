from __future__ import annotations

from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from typing import Any

from pydantic import BaseModel, Field

from beeai_framework.workflows import Workflow

from .state_store import StateStore


EXPECTED_BEEAI_COMMIT = "14882f0a16d08439dab919d9b05028464d304fc5"
EXPECTED_BEEAI_VERSION = "0.1.82"
SOURCE_APPROVAL_SCOPE = "dx12-source"


class Dx12WorkflowState(BaseModel):
    run_id: str
    workspace: str
    beeai_root: str
    evidence_root: str
    status: str = "created"
    baseline_commit: str = ""
    beeai_commit: str = ""
    dirty_paths: list[str] = Field(default_factory=list)
    checks: dict[str, Any] = Field(default_factory=dict)
    source_write_enabled: bool = False


def _run(command: list[str], cwd: Path) -> str:
    result = subprocess.run(command, cwd=cwd, check=False, capture_output=True, text=True, encoding="utf-8")
    if result.returncode != 0:
        raise RuntimeError(f"command failed ({result.returncode}): {' '.join(command)}\n{result.stderr.strip()}")
    return result.stdout.strip()


def _write_json_atomic(path: Path, payload: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    encoded = json.dumps(payload, indent=2, sort_keys=True) + "\n"
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(encoded, encoding="utf-8")
    temporary.replace(path)


class Dx12Orchestrator:
    def __init__(self, store: StateStore) -> None:
        self.store = store

    def create_workflow(self) -> Workflow[Dx12WorkflowState, str]:
        workflow: Workflow[Dx12WorkflowState, str] = Workflow(Dx12WorkflowState, name="mgif-dx12-foundation")
        workflow.add_step("preflight", self._preflight)
        workflow.add_step("approval_gate", self._approval_gate)
        workflow.add_step("ready", self._ready)
        return workflow

    async def run(self, state: Dx12WorkflowState) -> Dx12WorkflowState:
        result = await self.create_workflow().run(state)
        if result.result is None:
            raise RuntimeError("BeeAI workflow returned no result state")
        return result.result

    async def _preflight(self, state: Dx12WorkflowState) -> str | None:
        attempt = self.store.start_step(state.run_id, "preflight", state.model_dump())
        try:
            workspace = Path(state.workspace).resolve(strict=True)
            beeai_root = Path(state.beeai_root).resolve(strict=True)
            beeai_python = beeai_root / "python" / "pyproject.toml"
            if not (workspace / ".git").exists():
                raise RuntimeError(f"workspace is not an MGIF git checkout: {workspace}")
            if not beeai_python.exists():
                raise RuntimeError(f"BeeAI Python package is missing: {beeai_python}")

            safe_directory = f"safe.directory={beeai_root.as_posix()}"
            beeai_commit = _run(["git", "-c", safe_directory, "rev-parse", "HEAD"], beeai_root)
            baseline_commit = _run(["git", "rev-parse", "HEAD"], workspace)
            dirty_output = _run(["git", "status", "--porcelain=v1", "--untracked-files=all"], workspace)
            dirty_paths = [line for line in dirty_output.splitlines() if line]
            pyproject_text = beeai_python.read_text(encoding="utf-8")

            checks = {
                "python_supported": (3, 11) <= sys.version_info[:2] < (3, 14),
                "beeai_commit_matches": beeai_commit == EXPECTED_BEEAI_COMMIT,
                "beeai_version_matches": f'version="{EXPECTED_BEEAI_VERSION}"' in pyproject_text.replace(" ", ""),
                "workspace_is_git": True,
                "dirty_worktree_preserved": True,
            }
            failed = [name for name, passed in checks.items() if not passed]
            if failed:
                raise RuntimeError(f"preflight checks failed: {', '.join(failed)}")

            state.status = "preflight_passed"
            state.baseline_commit = baseline_commit
            state.beeai_commit = beeai_commit
            state.dirty_paths = dirty_paths
            state.checks = checks
            self.store.update_run(
                state.run_id,
                state.status,
                baseline_commit=baseline_commit,
                beeai_commit=beeai_commit,
            )
            evidence = {
                "schema": "mgif-beeai-dx12-preflight-v1",
                "created_at": datetime.now(timezone.utc).isoformat(),
                "state": state.model_dump(),
            }
            evidence_path = Path(state.evidence_root) / state.run_id / "preflight.json"
            _write_json_atomic(evidence_path, evidence)
            digest = hashlib.sha256(evidence_path.read_bytes()).hexdigest()
            self.store.finish_step(
                state.run_id,
                "preflight",
                attempt,
                {"evidence": str(evidence_path), "sha256": digest, "checks": checks},
            )
            return None
        except Exception as exc:
            state.status = "preflight_failed"
            self.store.update_run(state.run_id, state.status)
            self.store.fail_step(state.run_id, "preflight", attempt, str(exc))
            raise

    async def _approval_gate(self, state: Dx12WorkflowState) -> str:
        attempt = self.store.start_step(state.run_id, "approval_gate", {"scope": SOURCE_APPROVAL_SCOPE})
        approved = self.store.has_approval(state.run_id, SOURCE_APPROVAL_SCOPE)
        state.source_write_enabled = approved
        if approved:
            state.status = "source_approved"
            self.store.update_run(state.run_id, state.status)
            self.store.finish_step(state.run_id, "approval_gate", attempt, {"approved": True})
            return Workflow.NEXT
        state.status = "waiting_for_approval"
        self.store.update_run(state.run_id, state.status)
        self.store.finish_step(state.run_id, "approval_gate", attempt, {"approved": False})
        return Workflow.END

    async def _ready(self, state: Dx12WorkflowState) -> str:
        attempt = self.store.start_step(state.run_id, "ready")
        state.status = "ready_for_dx12_source"
        self.store.update_run(state.run_id, state.status)
        self.store.finish_step(state.run_id, "ready", attempt, {"source_write_enabled": True})
        return Workflow.END
