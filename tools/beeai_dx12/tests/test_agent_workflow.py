from __future__ import annotations

from collections import deque
from pathlib import Path
import tempfile
import unittest

from mgif_beeai_dx12.actions import ActionResult
from mgif_beeai_dx12.agent_models import (
    Dx12Task,
    FileChange,
    IterationReview,
    PatchProposal,
    ReviewStatus,
    TextEdit,
)
from mgif_beeai_dx12.agent_workflow import AgentLoop
from mgif_beeai_dx12.security import SecurityViolation
from mgif_beeai_dx12.state_store import StateStore


def _proposal(content: str) -> PatchProposal:
    return PatchProposal(
        summary="DX12 increment",
        changes=[FileChange(path="rhi/sample.cpp", content=content, rationale="test change")],
        verification_actions=["build-dx12"],
    )


class _ScriptedDriver:
    def __init__(self, proposal: PatchProposal, reviews: list[IterationReview]) -> None:
        self.proposal = proposal
        self.reviews = deque(reviews)
        self.propose_calls = 0
        self.feedback: list[dict[str, object]] = []

    async def propose(self, task: Dx12Task) -> PatchProposal:
        self.propose_calls += 1
        return self.proposal

    async def review(self, task, proposal, build_feedback):
        self.feedback.append(build_feedback)
        return self.reviews.popleft()


class _ScriptedActions:
    def __init__(self, exit_codes: list[int]) -> None:
        self.exit_codes = deque(exit_codes)

    def run(self, run_id: str, action: str) -> ActionResult:
        code = self.exit_codes.popleft()
        return ActionResult(
            action=action,
            command=["fixed-action", action],
            exit_code=code,
            stdout="build output",
            stderr="compile failure" if code else "",
            timed_out=False,
            started_at="start",
            finished_at="finish",
        )


class AgentLoopTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory(prefix="beeai-loop-")
        self.addCleanup(self.temp_dir.cleanup)
        self.root = Path(self.temp_dir.name).resolve()
        (self.root / "rhi").mkdir()
        self.source = self.root / "rhi" / "sample.cpp"
        self.source.write_text("original", encoding="utf-8")
        self.store = StateStore(self.root / "state.sqlite3")
        self.run_id = self.store.create_run("agent", str(self.root))
        self.task = Dx12Task(objective="Implement one safe DX12 resource increment")

    def _loop(self, driver, actions, *, limit: int = 3) -> AgentLoop:
        return AgentLoop(
            self.root,
            self.store,
            self.root / "evidence",
            driver,
            max_correction_iterations=limit,
            action_runner=actions,
        )

    async def test_plan_invokes_driver_and_write_requires_both_approvals(self) -> None:
        proposal = _proposal("changed")
        driver = _ScriptedDriver(
            proposal, [IterationReview(status=ReviewStatus.passed, analysis="all actions passed")]
        )
        loop = self._loop(driver, _ScriptedActions([0]))
        await loop.plan(self.run_id, self.task)
        self.assertEqual(driver.propose_calls, 1)
        with self.assertRaises(SecurityViolation):
            await loop.execute(self.run_id, self.task, proposal)
        self.assertEqual(self.source.read_text(encoding="utf-8"), "original")

        self.store.approve(self.run_id, "dx12-source", "user")
        self.store.approve(self.run_id, "build", "user")
        result = await loop.execute(self.run_id, self.task, proposal)
        self.assertEqual(result["status"], "agent_verified")
        self.assertEqual(self.source.read_text(encoding="utf-8"), "changed")

    async def test_build_failure_is_returned_to_agent_and_revised(self) -> None:
        first = _proposal("broken")
        revised = _proposal("fixed")
        driver = _ScriptedDriver(
            first,
            [
                IterationReview(
                    status=ReviewStatus.revise,
                    analysis="compiler rejected it",
                    revised_proposal=revised,
                ),
                IterationReview(status=ReviewStatus.passed, analysis="build passed"),
            ],
        )
        loop = self._loop(driver, _ScriptedActions([1, 0]))
        await loop.plan(self.run_id, self.task)
        self.store.approve(self.run_id, "dx12-source", "user")
        self.store.approve(self.run_id, "build", "user")
        result = await loop.execute(self.run_id, self.task, first)
        self.assertEqual(result["iterations"], 2)
        self.assertEqual(driver.feedback[0]["actions"][0]["exit_code"], 1)
        self.assertIn("compile failure", driver.feedback[0]["actions"][0]["stderr_tail"])
        self.assertEqual(self.source.read_text(encoding="utf-8"), "fixed")

    async def test_identical_revised_proposal_triggers_loop_guard(self) -> None:
        proposal = _proposal("same")
        driver = _ScriptedDriver(
            proposal,
            [
                IterationReview(
                    status=ReviewStatus.revise,
                    analysis="retry",
                    revised_proposal=proposal,
                )
            ],
        )
        loop = self._loop(driver, _ScriptedActions([1]))
        await loop.plan(self.run_id, self.task)
        self.store.approve(self.run_id, "dx12-source", "user")
        self.store.approve(self.run_id, "build", "user")
        with self.assertRaisesRegex(RuntimeError, "identical patch"):
            await loop.execute(self.run_id, self.task, proposal)
        self.assertEqual(self.store.get_run(self.run_id)["status"], "agent_loop_detected")

    async def test_iteration_limit_stops_further_edits(self) -> None:
        first = _proposal("one")
        second = _proposal("two")
        driver = _ScriptedDriver(
            first,
            [
                IterationReview(
                    status=ReviewStatus.revise,
                    analysis="one more fix",
                    revised_proposal=second,
                )
            ],
        )
        loop = self._loop(driver, _ScriptedActions([1]), limit=1)
        await loop.plan(self.run_id, self.task)
        self.store.approve(self.run_id, "dx12-source", "user")
        self.store.approve(self.run_id, "build", "user")
        result = await loop.execute(self.run_id, self.task, first)
        self.assertEqual(result["status"], "agent_iteration_limit")
        self.assertEqual(self.source.read_text(encoding="utf-8"), "one")

    async def test_exact_text_edit_is_applied_after_manifest_validation(self) -> None:
        proposal = PatchProposal(
            summary="Apply one exact edit",
            changes=[
                FileChange(
                    path="rhi/sample.cpp",
                    edits=[TextEdit(old="original", new="changed exactly once")],
                    rationale="exercise the existing-file edit path",
                )
            ],
            verification_actions=["build-dx12"],
        )
        driver = _ScriptedDriver(
            proposal, [IterationReview(status=ReviewStatus.passed, analysis="build passed")]
        )
        loop = self._loop(driver, _ScriptedActions([0]))
        await loop.plan(self.run_id, self.task)
        self.store.approve(self.run_id, "dx12-source", "user")
        self.store.approve(self.run_id, "build", "user")

        result = await loop.execute(self.run_id, self.task, proposal)

        self.assertEqual(result["status"], "agent_verified")
        self.assertEqual(self.source.read_text(encoding="utf-8"), "changed exactly once")

    async def test_exact_text_edit_rejects_non_unique_match(self) -> None:
        self.source.write_text("repeat repeat", encoding="utf-8")
        proposal = PatchProposal(
            summary="Ambiguous edit",
            changes=[
                FileChange(
                    path="rhi/sample.cpp",
                    edits=[TextEdit(old="repeat", new="changed")],
                    rationale="must be rejected before evidence is accepted",
                )
            ],
            verification_actions=["build-dx12"],
        )
        loop = self._loop(_ScriptedDriver(proposal, []), _ScriptedActions([]))

        with self.assertRaisesRegex(SecurityViolation, "match exactly once"):
            await loop.plan(self.run_id, self.task)

        self.assertEqual(self.source.read_text(encoding="utf-8"), "repeat repeat")
        self.assertEqual(self.store.get_run(self.run_id)["status"], "agent_plan_failed")


if __name__ == "__main__":
    unittest.main()
