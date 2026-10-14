#!/usr/bin/env python3
"""Negative and positive tests for acceptance-report semantic validation."""

from __future__ import annotations

import copy
import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools"))

from validate_editor_acceptance_report import (  # noqa: E402
    ValidationError,
    validate_acceptance_report,
)
from json_schema_subset import JsonSchemaError, validate_json_schema  # noqa: E402


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class AcceptanceReportValidatorTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name)
        self.executable = self.root / "Demo.exe"
        self.executable.write_bytes(b"fixed executable")
        self.evidence = self.root / "evidence.log"
        self.evidence.write_bytes(b"fixed evidence")
        self.edge_obligations = self.root / "edge-obligations.json"
        self.edge_obligations.write_text(
            json.dumps({"obligations": [{"id": f"E{index:03d}"} for index in range(1, 111)]}),
            encoding="utf-8",
        )
        self.report_path = self.root / "summary.json"
        self.schema = json.loads(
            (REPO_ROOT / "docs/editor-parity/schemas/acceptance-report.schema.json").read_text(encoding="utf-8")
        )
        self.report = self.make_report()

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def make_result(self, identifier: str) -> dict[str, object]:
        return {"id": identifier, "status": "pass", "evidence": ["evidence.log"]}

    def make_tally(self, total: int) -> dict[str, int]:
        return {
            "total": total,
            "passed": total,
            "failed": 0,
            "blocked": 0,
            "notRun": 0,
            "notApplicable": 0,
        }

    def make_report(self) -> dict[str, object]:
        return {
            "$schema": "acceptance-report.schema.json",
            "schemaVersion": 1,
            "runMode": "final",
            "identity": {
                "startedAt": "2026-08-22T12:00:00+08:00",
                "gitHead": "6c36567625b305286755a734a66188a22df18e77",
                "dirtyDiffSha256": "0" * 64,
                "referenceCommit": "50373c7b0ff58078727af2c2aac70e29d5e74752",
                "executable": {"path": "Demo.exe", "sha256": digest(b"fixed executable")},
                "cmake": {"generator": "Ninja"},
                "compiler": {"id": "MSVC"},
                "os": {"name": "Windows"},
                "vulkan": {"sdk": "test"},
                "gpu": [{"name": "test"}],
                "dpi": {"scale": 1.0},
            },
            "summary": {
                "status": "pass",
                "requirements": self.make_tally(24),
                "features": self.make_tally(42),
                "edges": self.make_tally(110),
                "platforms": self.make_tally(1),
                "unresolvedWaivers": 0,
            },
            "requirements": [self.make_result(f"R{index:02d}") for index in range(1, 25)],
            "features": [self.make_result(f"F{index:02d}") for index in range(1, 43)],
            "edges": [self.make_result(f"E{index:03d}") for index in range(1, 111)],
            "platforms": [self.make_result("windows-vulkan")],
            "artifacts": [
                {
                    "path": "evidence.log",
                    "sha256": digest(b"fixed evidence"),
                    "mediaType": "text/plain",
                    "producer": "test",
                }
            ],
        }

    def validate(self, report: dict[str, object] | None = None) -> None:
        selected_report = report or self.report
        validate_json_schema(selected_report, self.schema)
        self.report_path.write_text(json.dumps(selected_report), encoding="utf-8")
        validate_acceptance_report(self.report_path, self.root, self.edge_obligations)

    def test_accepts_complete_hash_bound_final_report(self) -> None:
        self.validate()

    def test_rejects_noncanonical_result_ids(self) -> None:
        report = copy.deepcopy(self.report)
        report["requirements"][0]["id"] = "R24"
        with self.assertRaisesRegex(ValidationError, "IDs must be (unique|canonical)"):
            self.validate(report)

    def test_rejects_false_summary_tally(self) -> None:
        report = copy.deepcopy(self.report)
        report["summary"]["features"]["passed"] = 41
        with self.assertRaisesRegex(ValidationError, "summary.features"):
            self.validate(report)

    def test_rejects_final_blocked_result(self) -> None:
        report = copy.deepcopy(self.report)
        report["edges"][0].update({"status": "blocked", "message": "gate pending"})
        report["summary"]["edges"] = {
            "total": 110,
            "passed": 109,
            "failed": 0,
            "blocked": 1,
            "notRun": 0,
            "notApplicable": 0,
        }
        report["summary"]["status"] = "blocked"
        with self.assertRaisesRegex(ValidationError, "final report requires every"):
            self.validate(report)

    def test_rejects_undeclared_evidence_reference(self) -> None:
        report = copy.deepcopy(self.report)
        report["features"][0]["evidence"] = ["missing.log"]
        with self.assertRaisesRegex(ValidationError, "undeclared artifact"):
            self.validate(report)

    def test_rejects_artifact_path_traversal(self) -> None:
        report = copy.deepcopy(self.report)
        report["artifacts"][0]["path"] = "../evidence.log"
        with self.assertRaisesRegex(ValidationError, "escapes the evidence root"):
            self.validate(report)

    def test_rejects_artifact_hash_mismatch(self) -> None:
        report = copy.deepcopy(self.report)
        report["artifacts"][0]["sha256"] = "f" * 64
        with self.assertRaisesRegex(ValidationError, "does not match"):
            self.validate(report)

    def test_rejects_naive_started_at(self) -> None:
        report = copy.deepcopy(self.report)
        report["identity"]["startedAt"] = "2026-08-22T12:00:00"
        with self.assertRaisesRegex(JsonSchemaError, "explicit UTC offset"):
            self.validate(report)

    def test_schema_rejects_missing_run_mode(self) -> None:
        report = copy.deepcopy(self.report)
        del report["runMode"]
        with self.assertRaisesRegex(JsonSchemaError, "runMode"):
            self.validate(report)


if __name__ == "__main__":
    unittest.main()
