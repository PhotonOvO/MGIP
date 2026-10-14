from __future__ import annotations

import unittest

from pydantic import ValidationError

from mgif_beeai_dx12.agent_models import FileChange, IterationReview, PatchProposal, ReviewStatus


class AgentModelTests(unittest.TestCase):
    def test_change_paths_are_strictly_scoped(self) -> None:
        for path in ["../secret.txt", "out/generated.cpp", "tools/agent.py", "C:/outside.cpp"]:
            with self.subTest(path=path), self.assertRaises(ValidationError):
                FileChange(path=path, content="x", rationale="test")
        self.assertEqual(
            FileChange(path="rhi/d3d12/Buffer.cpp", content="x", rationale="test").path,
            "rhi/d3d12/Buffer.cpp",
        )

    def test_review_status_and_revised_proposal_must_agree(self) -> None:
        proposal = PatchProposal(
            summary="fix",
            changes=[FileChange(path="rhi/a.cpp", content="x", rationale="test")],
        )
        with self.assertRaises(ValidationError):
            IterationReview(status=ReviewStatus.revise, analysis="retry")
        with self.assertRaises(ValidationError):
            IterationReview(status=ReviewStatus.passed, analysis="ok", revised_proposal=proposal)


if __name__ == "__main__":
    unittest.main()
