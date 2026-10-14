from __future__ import annotations

from dataclasses import dataclass, field
import json
from pathlib import Path
from typing import Protocol

from beeai_framework.agents.requirement import RequirementAgent
from beeai_framework.agents.requirement.requirements.conditional import ConditionalRequirement
from beeai_framework.agents.tool_calling.utils import ToolCallCheckerConfig

from .agent_models import Dx12Task, IterationReview, PatchProposal
from .agent_tools import create_source_tools
from .gateway import GatewayConfig
from .security import WorkspaceFileBackend


class AgentDriver(Protocol):
    async def propose(self, task: Dx12Task) -> PatchProposal: ...

    async def review(
        self, task: Dx12Task, proposal: PatchProposal, build_feedback: dict[str, object]
    ) -> IterationReview: ...


_INSTRUCTIONS = [
    "You are implementing the Direct3D 12 backend of the MGIF renderer.",
    "Use the supplied repository snapshot and inspect additional real sources with tools when needed.",
    "Keep the public RHI contract backend-neutral and preserve the existing Vulkan backend.",
    "Only propose changes under rhi/, tests/, shaders/, or the root CMakeLists.txt.",
    "Never request shell commands, credentials, network access, git writes, or generated-output edits.",
    "For existing files, return minimal exact text edits whose old text matches once; do not return whole-file content.",
    "Use complete content only for new files, and never return placeholders or prose as source content.",
    "Keep each iteration narrowly scoped and list only fixed named verification actions.",
]

_CONTEXT_RANGES = (
    ("rhi/RHIDevice.h", 190, 80),
    ("rhi/RHITypes.h", 495, 95),
    ("rhi/d3d12/D3D12Device.h", 1, 500),
    ("rhi/d3d12/D3D12Device.cpp", 1, 700),
    ("tests/d3d12_device_smoke_tests.cpp", 1, 500),
    ("tests/rhi_backend_factory_tests.cpp", 1, 500),
)


@dataclass
class BeeAICodingAgent:
    gateway: GatewayConfig
    workspace: str
    max_agent_iterations: int = 8
    last_trace: list[dict[str, object]] = field(default_factory=list, init=False)

    def _agent(self, audit: list[dict[str, object]]) -> RequirementAgent:
        tools = create_source_tools(self.workspace, audit)
        requirements = [
            ConditionalRequirement(tools[0], max_invocations=2),
            ConditionalRequirement(tools[1], max_invocations=4),
        ]
        return RequirementAgent(
            llm=self.gateway.create_chat_model(),
            tools=tools,
            requirements=requirements,
            name="mgif-dx12-coding-agent",
            role="Senior graphics engineer working on MGIF's D3D12 RHI backend",
            instructions=_INSTRUCTIONS,
            tool_call_checker=ToolCallCheckerConfig(
                max_strike_length=1, max_total_occurrences=4, window_size=8
            ),
            final_answer_as_tool=False,
            save_intermediate_steps=True,
        )

    async def _source_snapshot(self) -> str:
        root = Path(self.workspace).resolve(strict=True)
        backend = WorkspaceFileBackend(root)
        sections = []
        for relative, line, limit in _CONTEXT_RANGES:
            path = root / relative
            if not path.is_file():
                continue
            content = await backend.read_text(str(path), line=line, limit=limit)
            sections.append(f"===== {relative}:{line} =====\n{content}")
        return "\n".join(sections)

    async def propose(self, task: Dx12Task) -> PatchProposal:
        audit: list[dict[str, object]] = []
        agent = self._agent(audit)
        prompt = (
            "Analyze the current implementation and propose one buildable DX12 increment. "
            "The snapshot below is authoritative. Use read_source/search_source for any missing definitions. "
            "If the task cannot be completed, fail the response instead of inventing a placeholder patch.\n"
            f"Task:\n{task.model_dump_json(indent=2)}\n"
            f"Repository snapshot:\n{await self._source_snapshot()}"
        )
        try:
            result = await agent.run(
                prompt,
                expected_output=PatchProposal,
                max_iterations=self.max_agent_iterations,
                total_max_retries=3,
                max_retries_per_step=1,
            )
            return PatchProposal.model_validate(result.output_structured)
        finally:
            self.last_trace = audit

    async def review(
        self,
        task: Dx12Task,
        proposal: PatchProposal,
        build_feedback: dict[str, object],
    ) -> IterationReview:
        audit: list[dict[str, object]] = []
        agent = self._agent(audit)
        prompt = (
            "Review the applied DX12 patch against the build and test feedback. "
            "Return passed only when all requested actions succeeded. On revise, inspect relevant sources and return "
            "a complete corrected proposal. On blocked, explain the external blocker.\n"
            f"Task:\n{task.model_dump_json(indent=2)}\n"
            f"Applied proposal:\n{proposal.model_dump_json(indent=2)}\n"
            f"Build feedback:\n{json.dumps(build_feedback, indent=2, sort_keys=True)}"
        )
        try:
            result = await agent.run(
                prompt,
                expected_output=IterationReview,
                max_iterations=self.max_agent_iterations,
                total_max_retries=3,
                max_retries_per_step=1,
            )
            return IterationReview.model_validate(result.output_structured)
        finally:
            self.last_trace = audit
