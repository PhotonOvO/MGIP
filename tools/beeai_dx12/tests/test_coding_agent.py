from __future__ import annotations

from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from mgif_beeai_dx12.agent_models import Dx12Task, FileChange, PatchProposal
from mgif_beeai_dx12.coding_agent import BeeAICodingAgent


class _Gateway:
    def __init__(self) -> None:
        self.model_calls = 0

    def create_chat_model(self):
        self.model_calls += 1
        return object()


class _Result:
    def __init__(self, value) -> None:
        self.output_structured = value


class _RequirementAgent:
    calls = 0

    def __init__(self, **kwargs) -> None:
        self.kwargs = kwargs

    async def run(self, prompt, **kwargs):
        type(self).calls += 1
        return _Result(
            PatchProposal(
                summary="one slice",
                changes=[FileChange(path="rhi/new.cpp", content="content", rationale="needed")],
            )
        )


class BeeAICodingAgentTests(unittest.IsolatedAsyncioTestCase):
    async def test_propose_invokes_gateway_model_and_requirement_agent(self) -> None:
        with tempfile.TemporaryDirectory(prefix="beeai-coding-") as directory:
            root = Path(directory)
            (root / "rhi").mkdir()
            gateway = _Gateway()
            _RequirementAgent.calls = 0
            with patch("mgif_beeai_dx12.coding_agent.RequirementAgent", _RequirementAgent):
                proposal = await BeeAICodingAgent(gateway, str(root)).propose(
                    Dx12Task(objective="Implement a small DX12 resource slice")
                )
        self.assertEqual(proposal.summary, "one slice")
        self.assertEqual(gateway.model_calls, 1)
        self.assertEqual(_RequirementAgent.calls, 1)


if __name__ == "__main__":
    unittest.main()
