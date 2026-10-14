from __future__ import annotations

from enum import Enum
from pathlib import PurePath
from typing import Literal

from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator


ALLOWED_SOURCE_ROOTS = ("rhi/", "tests/", "shaders/")
ALLOWED_ROOT_FILES = {"CMakeLists.txt"}


class StrictModel(BaseModel):
    model_config = ConfigDict(extra="forbid")


class Dx12Task(StrictModel):
    objective: str = Field(min_length=8, max_length=4000)
    constraints: list[str] = Field(default_factory=list, max_length=20)


class TextEdit(StrictModel):
    old: str = Field(min_length=1, max_length=128 * 1024)
    new: str = Field(max_length=256 * 1024)

    @model_validator(mode="after")
    def must_change_text(self):
        if self.old == self.new:
            raise ValueError("text edit must change the matched source")
        return self


class FileChange(StrictModel):
    path: str = Field(min_length=1, max_length=260)
    content: str | None = Field(default=None, max_length=2 * 1024 * 1024)
    edits: list[TextEdit] = Field(default_factory=list, max_length=20)
    rationale: str = Field(min_length=1, max_length=2000)

    @field_validator("path")
    @classmethod
    def validate_path(cls, value: str) -> str:
        normalized = value.replace("\\", "/").removeprefix("./")
        path = PurePath(normalized)
        if path.is_absolute() or ".." in path.parts or ":" in normalized:
            raise ValueError("change paths must be workspace-relative and traversal-free")
        if normalized not in ALLOWED_ROOT_FILES and not normalized.startswith(ALLOWED_SOURCE_ROOTS):
            raise ValueError("change path is outside the DX12 source allowlist")
        return normalized

    @model_validator(mode="after")
    def exactly_one_change_mode(self):
        if (self.content is None) == (not self.edits):
            raise ValueError("provide either complete content or one or more exact text edits")
        return self


class PatchProposal(StrictModel):
    summary: str = Field(min_length=1, max_length=4000)
    changes: list[FileChange] = Field(min_length=1, max_length=12)
    verification_actions: list[Literal["rhi-boundary", "configure-dx12", "build-dx12", "test-dx12"]] = Field(
        default_factory=lambda: ["build-dx12", "test-dx12"], max_length=4
    )
    risks: list[str] = Field(default_factory=list, max_length=20)

    @field_validator("changes")
    @classmethod
    def unique_paths(cls, changes: list[FileChange]) -> list[FileChange]:
        paths = [change.path.lower() for change in changes]
        if len(paths) != len(set(paths)):
            raise ValueError("a proposal cannot modify the same path twice")
        return changes


class ReviewStatus(str, Enum):
    passed = "passed"
    revise = "revise"
    blocked = "blocked"


class IterationReview(StrictModel):
    status: ReviewStatus
    analysis: str = Field(min_length=1, max_length=6000)
    revised_proposal: PatchProposal | None = None

    @model_validator(mode="after")
    def proposal_matches_status(self):
        if self.status == ReviewStatus.revise and self.revised_proposal is None:
            raise ValueError("revise status requires a revised proposal")
        if self.status != ReviewStatus.revise and self.revised_proposal is not None:
            raise ValueError("only revise status may include a revised proposal")
        return self
