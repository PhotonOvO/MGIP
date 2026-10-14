from __future__ import annotations

from dataclasses import asdict
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
from typing import Any

from .actions import NamedActionRunner
from .agent_models import Dx12Task, PatchProposal, ReviewStatus
from .coding_agent import AgentDriver
from .security import SecurityViolation, WorkspaceFileBackend, WorkspacePolicy
from .state_store import StateStore
from .workflow import SOURCE_APPROVAL_SCOPE


class AgentLoop:
    def __init__(
        self,
        workspace: str | Path,
        store: StateStore,
        evidence_root: str | Path,
        driver: AgentDriver,
        *,
        max_correction_iterations: int = 3,
        action_runner: NamedActionRunner | None = None,
    ) -> None:
        self.policy = WorkspacePolicy(workspace)
        self.store = store
        self.evidence_root = self.policy.resolve(evidence_root)
        self.driver = driver
        self.max_correction_iterations = max_correction_iterations
        self.action_runner = action_runner or NamedActionRunner(
            self.policy.root, store, self.evidence_root
        )

    async def plan(self, run_id: str, task: Dx12Task) -> PatchProposal:
        self._require_run(run_id)
        attempt = self.store.start_step(run_id, "agent_plan", {"task": task.model_dump()})
        try:
            proposal = await self.driver.propose(task)
            manifest = self._proposal_manifest(proposal)
            path, digest = self._write_evidence(run_id, "proposal-1.json", manifest)
            self.store.record_artifact(run_id, "agent:proposal:1", path, digest)
            self.store.update_run(run_id, "agent_waiting_for_approval")
            self.store.finish_step(
                run_id,
                "agent_plan",
                attempt,
                {"proposal": str(path), "sha256": digest, "change_count": len(proposal.changes)},
            )
            return proposal
        except Exception as exc:
            self.store.update_run(run_id, "agent_plan_failed")
            self.store.fail_step(run_id, "agent_plan", attempt, self._safe_error(exc))
            raise

    async def execute(self, run_id: str, task: Dx12Task, proposal: PatchProposal) -> dict[str, Any]:
        self._require_run(run_id)
        self._require_approvals(run_id)
        current = proposal
        seen: set[str] = set()
        for iteration in range(1, self.max_correction_iterations + 1):
            fingerprint = hashlib.sha256(current.model_dump_json().encode("utf-8")).hexdigest()
            if fingerprint in seen:
                self.store.update_run(run_id, "agent_loop_detected")
                raise RuntimeError("agent repeated an identical patch proposal")
            seen.add(fingerprint)

            await self._apply(run_id, iteration, current)
            feedback = self._run_actions(run_id, current)
            review = await self.driver.review(task, current, self._bounded_feedback(feedback))
            review_path, review_digest = self._write_evidence(
                run_id, f"review-{iteration}.json", review.model_dump(mode="json")
            )
            self.store.record_artifact(run_id, f"agent:review:{iteration}", review_path, review_digest)

            actions_passed = all(item["exit_code"] == 0 for item in feedback["actions"])
            if review.status == ReviewStatus.passed:
                if not actions_passed:
                    raise RuntimeError("agent marked the iteration passed despite failing verification actions")
                self.store.update_run(run_id, "agent_verified")
                return {"run_id": run_id, "status": "agent_verified", "iterations": iteration}
            if review.status == ReviewStatus.blocked:
                self.store.update_run(run_id, "agent_blocked")
                return {
                    "run_id": run_id,
                    "status": "agent_blocked",
                    "iterations": iteration,
                    "analysis": review.analysis,
                }
            assert review.revised_proposal is not None
            current = review.revised_proposal
            manifest = self._proposal_manifest(current)
            path, digest = self._write_evidence(run_id, f"proposal-{iteration + 1}.json", manifest)
            self.store.record_artifact(run_id, f"agent:proposal:{iteration + 1}", path, digest)

        self.store.update_run(run_id, "agent_iteration_limit")
        return {
            "run_id": run_id,
            "status": "agent_iteration_limit",
            "iterations": self.max_correction_iterations,
        }

    def load_proposal(self, run_id: str, iteration: int = 1) -> PatchProposal:
        path = self.evidence_root / run_id / "agent" / f"proposal-{iteration}.json"
        payload = json.loads(path.read_text(encoding="utf-8"))
        return PatchProposal.model_validate(payload["proposal"])

    def _render_change(self, change) -> tuple[Path, str]:
        target = self.policy.resolve(self.policy.root / change.path, for_write=True)
        if change.content is not None:
            result = change.content
        elif not target.exists():
            raise SecurityViolation(f"exact text edits require an existing file: {change.path}")
        else:
            result = target.read_text(encoding="utf-8")
            for edit in change.edits:
                occurrences = result.count(edit.old)
                if occurrences != 1:
                    raise SecurityViolation(
                        f"text edit must match exactly once in {change.path}; found {occurrences}"
                    )
                result = result.replace(edit.old, edit.new, 1)
        if not result.strip():
            raise SecurityViolation(f"proposal contains empty source content: {change.path}")
        if target.exists():
            original = target.read_text(encoding="utf-8")
            minimum = max(1, len(original) // 4)
            if len(result) < minimum:
                raise SecurityViolation(
                    f"proposal would replace an existing source with suspiciously short content: {change.path}"
                )
        elif len(result) < 80:
            raise SecurityViolation(f"new source file content is suspiciously short: {change.path}")
        return target, result

    def _proposal_manifest(self, proposal: PatchProposal) -> dict[str, Any]:
        files = []
        for change in proposal.changes:
            target, result = self._render_change(change)
            files.append(
                {
                    "path": change.path,
                    "base_sha256": hashlib.sha256(target.read_bytes()).hexdigest()
                    if target.exists()
                    else None,
                    "result_sha256": hashlib.sha256(result.encode("utf-8")).hexdigest(),
                }
            )
        return {
            "schema": "mgif-beeai-dx12-proposal-v1",
            "created_at": datetime.now(timezone.utc).isoformat(),
            "proposal": proposal.model_dump(mode="json"),
            "base_files": files,
        }

    async def _apply(self, run_id: str, iteration: int, proposal: PatchProposal) -> None:
        manifest_path = self.evidence_root / run_id / "agent" / f"proposal-{iteration}.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        expected = {item["path"]: item["base_sha256"] for item in manifest["base_files"]}
        backend = WorkspaceFileBackend(self.policy.root, writable=True)
        backups: dict[str, str | None] = {}
        rendered: dict[str, str] = {}
        for change in proposal.changes:
            target, result = self._render_change(change)
            current_hash = hashlib.sha256(target.read_bytes()).hexdigest() if target.exists() else None
            if current_hash != expected.get(change.path):
                raise SecurityViolation(f"source changed after proposal was created: {change.path}")
            backups[change.path] = target.read_text(encoding="utf-8") if target.exists() else None
            rendered[change.path] = result
        backup_path, backup_digest = self._write_evidence(
            run_id, f"backup-{iteration}.json", {"files": backups}
        )
        self.store.record_artifact(run_id, f"agent:backup:{iteration}", backup_path, backup_digest)
        written: list[str] = []
        try:
            for change in proposal.changes:
                await backend.write_text(str(self.policy.root / change.path), rendered[change.path])
                written.append(change.path)
        except Exception:
            for path in reversed(written):
                original = backups[path]
                if original is not None:
                    await backend.write_text(str(self.policy.root / path), original)
            raise

    def _run_actions(self, run_id: str, proposal: PatchProposal) -> dict[str, Any]:
        results = []
        for action in proposal.verification_actions:
            result = self.action_runner.run(run_id, action)
            results.append(asdict(result))
            if result.exit_code != 0:
                break
        return {"actions": results}

    @staticmethod
    def _bounded_feedback(feedback: dict[str, Any]) -> dict[str, Any]:
        bounded = {"actions": []}
        for item in feedback["actions"]:
            bounded["actions"].append(
                {
                    "action": item["action"],
                    "exit_code": item["exit_code"],
                    "timed_out": item["timed_out"],
                    "stdout_tail": item["stdout"][-32000:],
                    "stderr_tail": item["stderr"][-32000:],
                }
            )
        return bounded

    def _write_evidence(self, run_id: str, name: str, payload: Any) -> tuple[Path, str]:
        path = self.evidence_root / run_id / "agent" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_suffix(path.suffix + ".tmp")
        temporary.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        temporary.replace(path)
        return path, hashlib.sha256(path.read_bytes()).hexdigest()

    def _require_run(self, run_id: str) -> None:
        if not self.store.get_run(run_id):
            raise KeyError(f"unknown run: {run_id}")

    def _require_approvals(self, run_id: str) -> None:
        missing = [
            scope
            for scope in (SOURCE_APPROVAL_SCOPE, "build")
            if not self.store.has_approval(run_id, scope)
        ]
        if missing:
            raise SecurityViolation(f"agent execution requires approval scopes: {', '.join(missing)}")

    @staticmethod
    def _safe_error(exc: Exception) -> str:
        return f"{type(exc).__name__}: model or validation operation failed"
