#!/usr/bin/env python3
"""Adversarial tests for the post-Orca local progress ledger."""

from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
PUBLISHER_PATH = ROOT / "tools/local_editor_progress_ledger.py"
VALIDATOR_PATH = ROOT / "tools/validate_editor_local_progress.py"


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


PUBLISHER = load_module("local_progress_publisher_under_test", PUBLISHER_PATH)
VALIDATOR = load_module("local_progress_validator_under_test", VALIDATOR_PATH)


def file_sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(PUBLISHER.document(value))


class Fixture:
    # Recovery tests exercise the pre-v2 failed-G6 chain.  The selected local
    # pointer is intentionally allowed to advance as the real ledger grows;
    # tests must therefore bind this historical v1 parent explicitly instead
    # of copying whichever chain happens to be selected in the worktree.
    FAILED_G6_V1_BUNDLE_ID = (
        "f7c9d5377328243fe409d926e32b0575acc33231842317c6a56e58544074b946"
    )
    # This immutable schema-v2 bundle contains the historical ordinary G7
    # failure plus the completed P7 integration prefix.  Pinning the bundle
    # keeps bridge tests independent of the moving checked-out pointer.
    FAILED_G7_V2_BUNDLE_ID = (
        "6563cc92f2053e5cf290c90006858f9e946e63037ed7395b08eae3f7603723e2"
    )
    FAILED_P8_90_V2_BUNDLE_ID = (
        "ff9636881a6ae8b32526dce3d6aac152a2bc503d5556d1e518a8aa281061409a"
    )

    COPY_FILES = (
        "docs/editor-parity/progress-ledger-current.json",
        "docs/editor-parity/progress-ledger-state.json",
        "docs/gltf-scene-editor-parity-progress.md",
        "docs/editor-parity/manifest.json",
        "docs/editor-parity/LICENSE-SOURCES.md",
        "docs/gltf-scene-editor-parity-task.md",
        "docs/editor-parity/schemas/progress-ledger.schema.json",
        "docs/editor-parity/schemas/local-progress-ledger.schema.json",
        "docs/editor-parity/.progress-ledger-local.guard",
        "tools/json_schema_subset.py",
        "tools/validate_editor_progress_ledger.py",
        "tools/validate_editor_local_progress.py",
    )

    def __init__(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="mgif-local-ledger-tests-")
        self.root = Path(self.temp.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()
        for relative in self.COPY_FILES:
            target = self.repo / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, target)
        shutil.copytree(
            ROOT / "docs/editor-parity/progress-ledger",
            self.repo / "docs/editor-parity/progress-ledger",
        )
        self.evidence = self.root / "evidence"
        self.evidence.mkdir()
        self.sequence = 0

    def close(self) -> None:
        self.temp.cleanup()

    def run(self, script: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, "-B", str(script), "--repo-root", str(self.repo), *arguments],
            cwd=self.repo,
            text=True,
            encoding="utf-8",
            capture_output=True,
            check=False,
        )

    def local(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return self.run(PUBLISHER_PATH, *arguments)

    def validator(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return self.run(VALIDATOR_PATH, *arguments)

    def task(self, task_id: str) -> tuple[dict, dict, dict | None]:
        origin = PUBLISHER.legacy_origin(self.repo)
        parent = PUBLISHER.load_local(self.repo, origin)
        tasks = parent["tasks"] if parent else PUBLISHER.effective_tasks(origin, {}, self.repo)
        task = next(row for row in tasks if row["taskId"] == task_id)
        return origin, task, parent

    def receipt(
        self,
        task_id: str,
        *,
        corrupt: str | None = None,
        completed_at: str | None = None,
        outcome: str = "succeeded",
    ) -> Path:
        origin, task, parent = self.task(task_id)
        self.sequence += 1
        label = f"case-{self.sequence}"
        report = self.evidence / f"{label}.md"
        report.write_text(f"# {task['displayName']} local review\n\nBounded local result.\n", encoding="utf-8")
        report_sha = file_sha(report)
        parent_bundle = parent["bundleId"] if parent else origin["bundleId"]
        parent_snapshot = parent["pointer"]["snapshotSha256"] if parent else origin["snapshotSha256"]
        version = parent["snapshot"]["schemaVersion"] if parent else 1
        before = PUBLISHER.task_hash(task)
        verdict = {
            "documentKind": "local-task-verdict",
            "schemaVersion": version,
            "ledgerId": PUBLISHER.LEDGER_ID,
            "parentBundleId": parent_bundle,
            "parentSnapshotSha256": parent_snapshot,
            "taskId": task_id,
            "displayName": task["displayName"],
            "beforeTaskSha256": before,
            "decision": "completed" if outcome == "succeeded" else "failed",
            "outcome": outcome,
            "reportSha256": report_sha,
            "fixedPolicy": copy.deepcopy(PUBLISHER.POLICY),
            "reviewAuthority": "local-automatic-review",
            "reviewExecutionOutcome": "succeeded",
        }
        verdict_path = self.evidence / f"{label}.json"
        write_json(verdict_path, verdict)
        receipt = {
            "documentKind": "local-task-transition-receipt",
            "schemaVersion": version,
            "ledgerId": PUBLISHER.LEDGER_ID,
            "parentBundleId": parent_bundle,
            "parentSnapshotSha256": parent_snapshot,
            "previousTransitionSha256": parent["snapshot"]["transitionHeadSha256"] if parent else None,
            "taskId": task_id,
            "displayName": task["displayName"],
            "beforeTaskSha256": before,
            "fromStatus": "ready",
            "toStatus": "completed" if outcome == "succeeded" else "failed",
            "outcome": outcome,
            "completedAtUtc": completed_at
            or (f"2026-09-01T00:00:{self.sequence:02d}.000Z"
                if version == 2
                else f"2026-08-29T00:00:{self.sequence:02d}.000Z"),
            "reviewAuthority": "local-automatic-review",
            "reviewExecutionOutcome": "succeeded",
            "report": {"path": str(report), "bytes": report.stat().st_size, "sha256": report_sha},
            "verdictArtifact": {"path": str(verdict_path), "bytes": verdict_path.stat().st_size, "sha256": file_sha(verdict_path)},
            "fixedPolicy": copy.deepcopy(PUBLISHER.POLICY),
        }
        if corrupt == "before":
            receipt["beforeTaskSha256"] = "0" * 64
        elif corrupt == "parent":
            receipt["parentBundleId"] = "1" * 64
        elif corrupt == "predecessor":
            receipt["previousTransitionSha256"] = "2" * 64
        draft = self.evidence / f"{label}-receipt.json"
        write_json(draft, receipt)
        if corrupt == "report-bytes":
            report.write_text("changed after sealing\n", encoding="utf-8")
        return draft

    def prepare(
        self, task_id: str, *, completed_at: str | None = None
    ) -> dict[str, object]:
        draft_path = self.receipt(task_id, completed_at=completed_at)
        origin, _, parent = self.task(task_id)
        stage = self.root / f"prepared-{self.sequence}"
        (stage / "evidence").mkdir(parents=True)
        draft = json.loads(draft_path.read_text(encoding="utf-8"))
        sealed, receipt_sha = PUBLISHER.validate_receipt_draft(
            draft,
            origin,
            parent,
            self.repo,
            [self.evidence],
            stage,
        )
        receipt_data = PUBLISHER.document(sealed)
        receipt_record = {
            "path": (PUBLISHER.EVIDENCE_REL / receipt_sha).as_posix(),
            "bytes": len(receipt_data),
            "sha256": receipt_sha,
        }
        snapshot, state, snapshot_data, state_data, markdown = (
            PUBLISHER.build_local(
                origin,
                parent,
                sealed,
                receipt_sha,
                receipt_record,
                self.repo,
            )
        )
        return {
            "origin": origin,
            "parent": parent,
            "snapshot": snapshot,
            "state": state,
            "snapshot_data": snapshot_data,
            "state_data": state_data,
            "markdown": markdown,
            "stage": stage,
            "expected_pointer_sha": PUBLISHER.captured_parent_pointer_sha(parent),
        }

    def publish_prepared(self, prepared: dict[str, object]) -> str:
        return PUBLISHER.publish_local(
            self.repo,
            prepared["origin"],
            prepared["parent"],
            prepared["snapshot"],
            prepared["state"],
            prepared["snapshot_data"],
            prepared["state_data"],
            prepared["markdown"],
            prepared["stage"],
            prepared["expected_pointer_sha"],
        )

    def crash_writer(self, *, with_journal: bool) -> subprocess.CompletedProcess[str]:
        code = r'''
import importlib.util
import os
import sys
from pathlib import Path

publisher_path = Path(sys.argv[1])
repo = Path(sys.argv[2])
spec = importlib.util.spec_from_file_location("crash_local_publisher", publisher_path)
module = importlib.util.module_from_spec(spec)
assert spec and spec.loader
spec.loader.exec_module(module)
lock, token, _, guard = module.acquire_lock(repo)
if sys.argv[3] == "journal":
    paths = module.publication_paths(repo)
    before = {name: module.preimage(path) for name, path in paths.items()}
    replacement = {name: f"crashed-{name}\n".encode("ascii") for name in paths}
    journal = {
        "documentKind": "local-progress-ledger-publication-journal",
        "schemaVersion": 1,
        "transactionId": token,
        "commitPoint": "pointer-replacement",
        "preimages": before,
        "newHashes": {
            name: module.digest(data) for name, data in replacement.items()
        },
    }
    module.write_atomic(
        repo / module.JOURNAL_REL,
        module.document(journal),
        "crash journal",
    )
    module.write_atomic(paths["state"], replacement["state"], "crash state")
    module.write_atomic(paths["ledger"], replacement["ledger"], "crash ledger")
os._exit(73)
'''
        return subprocess.run(
            [
                sys.executable,
                "-B",
                "-c",
                code,
                str(PUBLISHER_PATH),
                str(self.repo),
                "journal" if with_journal else "lock-only",
            ],
            cwd=self.repo,
            text=True,
            encoding="utf-8",
            capture_output=True,
            check=False,
        )

    def start_recovery(self) -> subprocess.Popen[str]:
        code = r'''
import importlib.util
import sys
from pathlib import Path

publisher_path = Path(sys.argv[1])
repo = Path(sys.argv[2])
spec = importlib.util.spec_from_file_location("recovery_local_publisher", publisher_path)
module = importlib.util.module_from_spec(spec)
assert spec and spec.loader
spec.loader.exec_module(module)
try:
    print(module.recover_local_publication(repo))
except module.LocalLedgerError as exc:
    print(str(exc), file=sys.stderr)
    raise SystemExit(2)
'''
        return subprocess.Popen(
            [sys.executable, "-B", "-c", code, str(PUBLISHER_PATH), str(self.repo)],
            cwd=self.repo,
            text=True,
            encoding="utf-8",
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    def publish(
        self, task_id: str, *, outcome: str = "succeeded"
    ) -> subprocess.CompletedProcess[str]:
        receipt = self.receipt(task_id, outcome=outcome)
        return self.local(
            "--receipt", str(receipt), "--evidence-root", str(self.evidence), "--publish"
        )

    def recovery_integration_receipt(
        self, *, supersedes: str = "task_99a11c23bb9a",
        dependencies: list[str] | None = None,
        outcome: str = "succeeded",
    ) -> Path:
        origin = PUBLISHER.legacy_origin(self.repo)
        parent = PUBLISHER.load_local(self.repo, origin)
        assert parent is not None
        self.sequence += 1
        task_id = "task_p8_90_r1"
        definition = {
            "taskId": task_id,
            "displayName": "P8-90-R1",
            "dependencies": sorted(dependencies or [
                "task_469ac0bc570d", "task_c52f54a8f799",
                "task_2c5f762c2290",
            ]),
            "createdAtUtc": f"2026-09-05T05:00:{self.sequence:02d}.000Z",
            "phase": "P8",
            "role": "integration",
            "supersedesTaskId": supersedes,
        }
        authority = {
            "documentKind": "local-recovery-task-authority",
            "schemaVersion": 1,
            "ledgerId": PUBLISHER.LEDGER_ID,
            "parentBundleId": parent["bundleId"],
            "parentSnapshotSha256": parent["pointer"]["snapshotSha256"],
            "taskDefinitionSha256": PUBLISHER.digest(PUBLISHER.payload(definition)),
            "authorizedAtUtc": definition["createdAtUtc"],
            "fixedPolicy": copy.deepcopy(PUBLISHER.POLICY),
        }
        prefix = f"recovery-integration-{self.sequence}"
        authority_path = self.evidence / f"{prefix}-authority.json"
        write_json(authority_path, authority)
        report_path = self.evidence / f"{prefix}.md"
        report_path.write_text("# P8-90-R1\n\nRecovery integration contract.\n", encoding="utf-8")
        report_sha = file_sha(report_path)
        task = PUBLISHER.recovery_task_view(definition)
        task["status"] = "ready"
        before = PUBLISHER.task_hash(task)
        verdict = {
            "documentKind": "local-task-verdict",
            "schemaVersion": 2,
            "ledgerId": PUBLISHER.LEDGER_ID,
            "parentBundleId": parent["bundleId"],
            "parentSnapshotSha256": parent["pointer"]["snapshotSha256"],
            "taskId": task_id,
            "displayName": "P8-90-R1",
            "beforeTaskSha256": before,
            "decision": "completed" if outcome == "succeeded" else "failed",
            "outcome": outcome,
            "reportSha256": report_sha,
            "fixedPolicy": copy.deepcopy(PUBLISHER.POLICY),
            "reviewAuthority": "local-automatic-review",
            "reviewExecutionOutcome": "succeeded",
            "taskDefinitionSha256": PUBLISHER.digest(PUBLISHER.payload(definition)),
            "authoritySha256": file_sha(authority_path),
            "gateEvent": None,
            "dependencyReplacementsSha256": PUBLISHER.digest(PUBLISHER.payload([])),
        }
        verdict_path = self.evidence / f"{prefix}-verdict.json"
        write_json(verdict_path, verdict)
        receipt = {
            "documentKind": "local-task-transition-receipt",
            "schemaVersion": 2,
            "ledgerId": PUBLISHER.LEDGER_ID,
            "parentBundleId": parent["bundleId"],
            "parentSnapshotSha256": parent["pointer"]["snapshotSha256"],
            "previousTransitionSha256": parent["snapshot"]["transitionHeadSha256"],
            "taskId": task_id,
            "displayName": "P8-90-R1",
            "beforeTaskSha256": before,
            "fromStatus": "ready",
            "toStatus": "completed" if outcome == "succeeded" else "failed",
            "outcome": outcome,
            "completedAtUtc": f"2026-09-05T05:01:{self.sequence:02d}.000Z",
            "reviewAuthority": "local-automatic-review",
            "reviewExecutionOutcome": "succeeded",
            "report": {"path": str(report_path), "bytes": report_path.stat().st_size, "sha256": report_sha},
            "verdictArtifact": {"path": str(verdict_path), "bytes": verdict_path.stat().st_size, "sha256": file_sha(verdict_path)},
            "fixedPolicy": copy.deepcopy(PUBLISHER.POLICY),
            "taskDefinition": definition,
            "authorityArtifact": {"path": str(authority_path), "bytes": authority_path.stat().st_size, "sha256": file_sha(authority_path)},
            "gateEvent": None,
            "dependencyReplacements": [],
        }
        receipt_path = self.evidence / f"{prefix}-receipt.json"
        write_json(receipt_path, receipt)
        return receipt_path

    def bootstrap_failed_g6(self) -> None:
        # Do not copy the repository's selected pointer here.  It is a moving
        # integration output (currently schema v2), while these tests require
        # the immutable schema-v1 failed-G6 parent and its exact receipt prefix.
        self.bootstrap_bundle(self.FAILED_G6_V1_BUNDLE_ID)

    def bootstrap_bundle(self, bundle_id: str) -> None:
        """Select one immutable local bundle as the temporary test parent."""
        bundle = self.repo / PUBLISHER.LOCAL_STORE_REL / bundle_id
        descriptor = json.loads(
            (bundle / "bundle.json").read_text(encoding="utf-8")
        )
        if descriptor.get("bundleId") != bundle_id:
            raise AssertionError("pinned local fixture bundle identity changed")
        origin = PUBLISHER.legacy_origin(self.repo)
        pointer = {
            "documentKind": "local-progress-ledger-pointer",
            "schemaVersion": 1,
            "ledgerId": PUBLISHER.LEDGER_ID,
            "basePointerSha256": origin["pointerSha256"],
            "baseBundleId": origin["bundleId"],
            "baseStateSha256": origin["stateSha256"],
            "baseSnapshotSha256": origin["snapshotSha256"],
            "bundleId": bundle_id,
            "bundlePath": (
                PUBLISHER.LOCAL_STORE_REL
                / bundle_id
            ).as_posix(),
            "stateSha256": descriptor["stateSha256"],
            "snapshotSha256": descriptor["snapshotSha256"],
            "markdownSha256": descriptor["markdownSha256"],
        }
        write_json(self.repo / PUBLISHER.LOCAL_POINTER_REL, pointer)
        for source_name, relative in (
            ("state.json", PUBLISHER.LOCAL_STATE_REL),
            ("progress.md", PUBLISHER.LOCAL_MARKDOWN_REL),
        ):
            target = self.repo / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(bundle / source_name, target)

    def recovery_receipt(
        self,
        *,
        outcome: str,
        corruption: str | None = None,
        task_id: str = "task_g6_r1",
    ) -> Path:
        origin = PUBLISHER.legacy_origin(self.repo)
        parent = PUBLISHER.load_local(self.repo, origin)
        assert parent is not None
        self.sequence += 1
        created_at = f"2026-08-31T08:00:{self.sequence:02d}.000Z"
        completed_at = f"2026-08-31T08:01:{self.sequence:02d}.000Z"
        dependencies = ["task_c4958ebe56e4"]
        if corruption == "unknown":
            dependencies = ["task_unknown_dependency"]
        elif corruption == "cycle":
            dependencies = [task_id]
        definition = {
            "taskId": task_id,
            "displayName": "G6-R1",
            "dependencies": dependencies,
            "createdAtUtc": created_at,
            "phase": "P6",
            "role": "gate",
            "supersedesTaskId": "task_4af12cfad98a",
        }
        authority = {
            "documentKind": "local-recovery-task-authority",
            "schemaVersion": 1,
            "ledgerId": PUBLISHER.LEDGER_ID,
            "parentBundleId": parent["bundleId"],
            "parentSnapshotSha256": parent["pointer"]["snapshotSha256"],
            "taskDefinitionSha256": PUBLISHER.digest(
                PUBLISHER.payload(definition)
            ),
            "authorizedAtUtc": created_at,
            "fixedPolicy": copy.deepcopy(PUBLISHER.POLICY),
        }
        if corruption == "authority":
            authority["taskDefinitionSha256"] = "0" * 64
        label = f"recovery-{self.sequence}"
        authority_path = self.evidence / f"{label}-authority.json"
        write_json(authority_path, authority)
        report = self.evidence / f"{label}.md"
        report.write_text("# G6-R1\n\nFresh bounded re-Gate.\n", encoding="utf-8")
        latest = PUBLISHER.latest_gate_events(parent["gateEvents"])["P6"]
        supersedes_event = latest["eventId"]
        if corruption == "supersedes":
            supersedes_event = "1" * 64
        gate_event = {
            "phase": "P6",
            "gateVerdict": PUBLISHER.GATE_PASS
            if outcome == "succeeded" else PUBLISHER.GATE_FAIL,
            "supersedesEventId": supersedes_event,
        }
        replacements = [] if outcome == "failed" else [
            {
                "taskId": "task_1a40c0d302ae",
                "replacedDependencyTaskId": "task_4af12cfad98a",
                "acceptedGate": {
                    "phase": "P6",
                    "gateVerdict": PUBLISHER.GATE_PASS,
                },
            }
        ]
        task = PUBLISHER.recovery_task_view(definition)
        task["status"] = "ready"
        before = PUBLISHER.task_hash(task)
        verdict = {
            "documentKind": "local-task-verdict",
            "schemaVersion": 2,
            "ledgerId": PUBLISHER.LEDGER_ID,
            "parentBundleId": parent["bundleId"],
            "parentSnapshotSha256": parent["pointer"]["snapshotSha256"],
            "taskId": task_id,
            "displayName": "G6-R1",
            "beforeTaskSha256": before,
            "decision": "completed" if outcome == "succeeded" else "failed",
            "outcome": outcome,
            "reportSha256": file_sha(report),
            "fixedPolicy": copy.deepcopy(PUBLISHER.POLICY),
            "reviewAuthority": "local-automatic-review",
            "reviewExecutionOutcome": "succeeded",
            "taskDefinitionSha256": PUBLISHER.digest(
                PUBLISHER.payload(definition)
            ),
            "authoritySha256": file_sha(authority_path),
            "gateEvent": gate_event,
            "dependencyReplacementsSha256": PUBLISHER.digest(
                PUBLISHER.payload(replacements)
            ),
        }
        verdict_path = self.evidence / f"{label}-verdict.json"
        write_json(verdict_path, verdict)
        receipt = {
            "documentKind": "local-task-transition-receipt",
            "schemaVersion": 2,
            "ledgerId": PUBLISHER.LEDGER_ID,
            "parentBundleId": parent["bundleId"],
            "parentSnapshotSha256": parent["pointer"]["snapshotSha256"],
            "previousTransitionSha256": parent["snapshot"]["transitionHeadSha256"],
            "taskId": task_id,
            "displayName": "G6-R1",
            "beforeTaskSha256": before,
            "fromStatus": "ready",
            "toStatus": "completed" if outcome == "succeeded" else "failed",
            "outcome": outcome,
            "completedAtUtc": completed_at,
            "reviewAuthority": "local-automatic-review",
            "reviewExecutionOutcome": "succeeded",
            "report": {
                "path": str(report),
                "bytes": report.stat().st_size,
                "sha256": file_sha(report),
            },
            "verdictArtifact": {
                "path": str(verdict_path),
                "bytes": verdict_path.stat().st_size,
                "sha256": file_sha(verdict_path),
            },
            "fixedPolicy": copy.deepcopy(PUBLISHER.POLICY),
            "taskDefinition": definition,
            "authorityArtifact": {
                "path": str(authority_path),
                "bytes": authority_path.stat().st_size,
                "sha256": file_sha(authority_path),
            },
            "gateEvent": gate_event,
            "dependencyReplacements": replacements,
        }
        if corruption == "extra":
            receipt["unexpected"] = True
        draft = self.evidence / f"{label}-receipt.json"
        write_json(draft, receipt)
        return draft

    def publish_recovery(
        self, *, outcome: str, corruption: str | None = None
    ) -> subprocess.CompletedProcess[str]:
        receipt = self.recovery_receipt(
            outcome=outcome, corruption=corruption)
        return self.local(
            "--receipt", str(receipt), "--evidence-root", str(self.evidence),
            "--publish")

    def g7_bridge_receipt(
        self, *, outcome: str = "succeeded", corruption: str | None = None
    ) -> Path:
        """Build a schema-2 G7 recovery receipt against the pinned v2 prefix."""
        origin = PUBLISHER.legacy_origin(self.repo)
        parent = PUBLISHER.load_local(self.repo, origin)
        assert parent is not None
        created_at = "2026-09-04T07:00:00.000Z"
        completed_at = "2026-09-04T07:01:00.000Z"
        task_id = "task_g7_r1_bridge"
        definition = {
            "taskId": task_id,
            "displayName": "G7-R1",
            "dependencies": ["task_p7_rhi_visual_r3_20260904"],
            "createdAtUtc": created_at,
            "phase": "P7",
            "role": "gate",
            "supersedesTaskId": "task_3f216d250677",
        }
        authority = {
            "documentKind": "local-recovery-task-authority",
            "schemaVersion": 1,
            "ledgerId": PUBLISHER.LEDGER_ID,
            "parentBundleId": parent["bundleId"],
            "parentSnapshotSha256": parent["pointer"]["snapshotSha256"],
            "taskDefinitionSha256": PUBLISHER.digest(
                PUBLISHER.payload(definition)),
            "authorizedAtUtc": created_at,
            "fixedPolicy": copy.deepcopy(PUBLISHER.POLICY),
        }
        authority_path = self.evidence / "g7-bridge-authority.json"
        write_json(authority_path, authority)
        report_path = self.evidence / "g7-bridge-report.md"
        report_path.write_text("# G7-R1\n\nBridge protocol test.\n", encoding="utf-8")
        supersedes_event = (
            "e297bd471f5f7a5441abe7bb9e645736b6085987d9e452f4168de71445aca7cd"
        )
        if corruption == "unknown":
            supersedes_event = "1" * 64
        elif corruption == "mismatched":
            # This is a real earlier ordinary receipt, but it is not the
            # materialized G7 Gate and must not be accepted as its predecessor.
            supersedes_event = (
                "e6b8aa65967b3afb19f8fce76c830f1317a5ecc0fb7ce4c709c14da2053b3701"
            )
        gate_event = {
            "phase": "P7",
            "gateVerdict": PUBLISHER.GATE_PASS
            if outcome == "succeeded" else PUBLISHER.GATE_FAIL,
            "supersedesEventId": supersedes_event,
        }
        replacements = [
            {
                "taskId": "task_15f206f912f6",
                "replacedDependencyTaskId": "task_3f216d250677",
                "acceptedGate": {
                    "phase": "P7", "gateVerdict": PUBLISHER.GATE_PASS,
                },
            }
        ]
        if corruption != "failed-replacement" and outcome == "failed":
            replacements = []
        task = PUBLISHER.recovery_task_view(definition)
        task["status"] = "ready"
        before = PUBLISHER.task_hash(task)
        verdict = {
            "documentKind": "local-task-verdict",
            "schemaVersion": 2,
            "ledgerId": PUBLISHER.LEDGER_ID,
            "parentBundleId": parent["bundleId"],
            "parentSnapshotSha256": parent["pointer"]["snapshotSha256"],
            "taskId": task_id,
            "displayName": "G7-R1",
            "beforeTaskSha256": before,
            "decision": "completed" if outcome == "succeeded" else "failed",
            "outcome": outcome,
            "reportSha256": file_sha(report_path),
            "fixedPolicy": copy.deepcopy(PUBLISHER.POLICY),
            "reviewAuthority": "local-automatic-review",
            "reviewExecutionOutcome": "succeeded",
            "taskDefinitionSha256": PUBLISHER.digest(
                PUBLISHER.payload(definition)),
            "authoritySha256": file_sha(authority_path),
            "gateEvent": gate_event,
            "dependencyReplacementsSha256": PUBLISHER.digest(
                PUBLISHER.payload(replacements)),
        }
        verdict_path = self.evidence / "g7-bridge-verdict.json"
        write_json(verdict_path, verdict)
        receipt = {
            "documentKind": "local-task-transition-receipt",
            "schemaVersion": 2,
            "ledgerId": PUBLISHER.LEDGER_ID,
            "parentBundleId": parent["bundleId"],
            "parentSnapshotSha256": parent["pointer"]["snapshotSha256"],
            "previousTransitionSha256": parent["snapshot"]["transitionHeadSha256"],
            "taskId": task_id,
            "displayName": "G7-R1",
            "beforeTaskSha256": before,
            "fromStatus": "ready",
            "toStatus": "completed" if outcome == "succeeded" else "failed",
            "outcome": outcome,
            "completedAtUtc": completed_at,
            "reviewAuthority": "local-automatic-review",
            "reviewExecutionOutcome": "succeeded",
            "report": {"path": str(report_path), "bytes": report_path.stat().st_size, "sha256": file_sha(report_path)},
            "verdictArtifact": {"path": str(verdict_path), "bytes": verdict_path.stat().st_size, "sha256": file_sha(verdict_path)},
            "fixedPolicy": copy.deepcopy(PUBLISHER.POLICY),
            "taskDefinition": definition,
            "authorityArtifact": {"path": str(authority_path), "bytes": authority_path.stat().st_size, "sha256": file_sha(authority_path)},
            "gateEvent": gate_event,
            "dependencyReplacements": replacements,
        }
        receipt_path = self.evidence / "g7-bridge-receipt.json"
        write_json(receipt_path, receipt)
        return receipt_path


class LocalProgressLedgerTests(unittest.TestCase):
    def fixture(self) -> Fixture:
        value = Fixture()
        self.addCleanup(value.close)
        return value

    def assert_success(self, run: subprocess.CompletedProcess[str]) -> None:
        self.assertEqual(run.returncode, 0, run.stderr or run.stdout)

    def assert_failure(self, run: subprocess.CompletedProcess[str], text: str) -> None:
        self.assertNotEqual(run.returncode, 0, run.stdout)
        self.assertIn(text, run.stderr + run.stdout)

    def test_01_first_receipt_publishes_and_derives_p6_ready(self) -> None:
        fixture = self.fixture()
        legacy_before = (fixture.repo / PUBLISHER.LEGACY_POINTER_REL).read_bytes()
        self.assert_success(fixture.publish("task_2ebaf90139c5"))
        self.assert_success(fixture.validator("--check"))
        self.assertEqual((fixture.repo / PUBLISHER.LEGACY_POINTER_REL).read_bytes(), legacy_before)
        state = json.loads((fixture.repo / PUBLISHER.LOCAL_STATE_REL).read_text(encoding="utf-8"))
        tasks = {row["taskId"]: row for row in state["tasks"]}
        self.assertEqual(tasks["task_2ebaf90139c5"]["status"], "completed")
        self.assertEqual(tasks["task_1254fa47b9b7"]["status"], "ready")
        self.assertEqual(state["nextRunnableTaskIds"], ["task_1254fa47b9b7"])
        wire = (fixture.repo / PUBLISHER.LOCAL_STATE_REL).read_text(encoding="utf-8")
        for name in PUBLISHER.BANNED_LOCAL_KEYS:
            self.assertNotIn(f'"{name}"', wire)

    def test_02_append_receipt_requires_parent_prefix_and_advances_readiness(self) -> None:
        fixture = self.fixture()
        self.assert_success(fixture.publish("task_2ebaf90139c5"))
        first = json.loads((fixture.repo / PUBLISHER.LOCAL_POINTER_REL).read_text(encoding="utf-8"))
        self.assert_success(fixture.publish("task_1254fa47b9b7"))
        pointer = json.loads((fixture.repo / PUBLISHER.LOCAL_POINTER_REL).read_text(encoding="utf-8"))
        snapshot = json.loads((fixture.repo / pointer["bundlePath"] / "snapshot.json").read_text(encoding="utf-8"))
        state = json.loads((fixture.repo / PUBLISHER.LOCAL_STATE_REL).read_text(encoding="utf-8"))
        tasks = {row["taskId"]: row for row in state["tasks"]}
        self.assertEqual(len(snapshot["transitionReceipts"]), 2)
        self.assertEqual(snapshot["parent"]["bundleId"], first["bundleId"])
        self.assertEqual(tasks["task_1040525304c0"]["status"], "ready")
        self.assertEqual(
            state["nextRunnableTaskIds"],
            ["task_1040525304c0", "task_b59fd290ead0"],
        )

    def test_03_stale_task_parent_predecessor_and_unmet_dependency_fail(self) -> None:
        for corruption, expected in (("before", "before-task"), ("parent", "parent")):
            with self.subTest(corruption=corruption):
                fixture = self.fixture()
                draft = fixture.receipt("task_2ebaf90139c5", corrupt=corruption)
                self.assert_failure(fixture.local("--receipt", str(draft), "--evidence-root", str(fixture.evidence), "--candidate-dir", str(fixture.root / "candidate")), expected)
        fixture = self.fixture()
        draft = fixture.receipt("task_1040525304c0")
        self.assert_failure(fixture.local("--receipt", str(draft), "--evidence-root", str(fixture.evidence), "--candidate-dir", str(fixture.root / "candidate")), "not ready")
        fixture = self.fixture()
        self.assert_success(fixture.publish("task_2ebaf90139c5"))
        draft = fixture.receipt("task_1254fa47b9b7", corrupt="predecessor")
        self.assert_failure(fixture.local("--receipt", str(draft), "--evidence-root", str(fixture.evidence), "--candidate-dir", str(fixture.root / "candidate")), "predecessor")

    def test_04_artifact_tamper_and_verdict_binding_fail(self) -> None:
        fixture = self.fixture()
        draft = fixture.receipt("task_2ebaf90139c5", corrupt="report-bytes")
        self.assert_failure(fixture.local("--receipt", str(draft), "--evidence-root", str(fixture.evidence), "--candidate-dir", str(fixture.root / "candidate")), "bytes/hash")
        fixture = self.fixture()
        draft = fixture.receipt("task_2ebaf90139c5")
        receipt = json.loads(draft.read_text(encoding="utf-8"))
        verdict_path = Path(receipt["verdictArtifact"]["path"])
        verdict = json.loads(verdict_path.read_text(encoding="utf-8"))
        verdict["decision"] = "failed"
        write_json(verdict_path, verdict)
        receipt["verdictArtifact"] = {"path": str(verdict_path), "bytes": verdict_path.stat().st_size, "sha256": file_sha(verdict_path)}
        write_json(draft, receipt)
        self.assert_failure(fixture.local("--receipt", str(draft), "--evidence-root", str(fixture.evidence), "--candidate-dir", str(fixture.root / "candidate")), "not bound")

    def test_05_independent_validator_rejects_local_legacy_fields(self) -> None:
        with self.assertRaisesRegex(VALIDATOR.ValidationError, "forbidden legacy field dispatchId"):
            VALIDATOR.reject_banned({"task": {"dispatchId": "ctx_forged"}}, "probe")
        source = PUBLISHER_PATH.read_text(encoding="utf-8")
        self.assertNotIn('["orca"]', source)
        self.assertNotIn('"orchestration"', source)

    def test_06_schema_accepts_selected_local_documents(self) -> None:
        fixture = self.fixture()
        self.assert_success(fixture.publish("task_2ebaf90139c5"))
        schema = json.loads((fixture.repo / "docs/editor-parity/schemas/local-progress-ledger.schema.json").read_text(encoding="utf-8"))
        validator = VALIDATOR.JsonSchemaValidator(schema)
        pointer = json.loads((fixture.repo / PUBLISHER.LOCAL_POINTER_REL).read_text(encoding="utf-8"))
        bundle = fixture.repo / pointer["bundlePath"]
        for path in (fixture.repo / PUBLISHER.LOCAL_POINTER_REL, bundle / "state.json", bundle / "snapshot.json"):
            validator.validate(json.loads(path.read_text(encoding="utf-8")))
        snapshot = json.loads((bundle / "snapshot.json").read_text(encoding="utf-8"))
        receipt_path = fixture.repo / snapshot["transitionReceipts"][0]["path"]
        validator.validate(json.loads(receipt_path.read_text(encoding="utf-8")))

    def test_07_two_writers_from_one_parent_cannot_both_publish(self) -> None:
        fixture = self.fixture()
        self.assert_success(fixture.publish("task_2ebaf90139c5"))
        first = fixture.prepare("task_1254fa47b9b7")
        second = fixture.prepare("task_1254fa47b9b7")
        fixture.publish_prepared(first)
        with self.assertRaisesRegex(
            PUBLISHER.LocalLedgerError,
            "pointer changed since receipt was prepared",
        ):
            fixture.publish_prepared(second)
        second["expected_pointer_sha"] = PUBLISHER.current_hash(
            fixture.repo / PUBLISHER.LOCAL_POINTER_REL
        )
        with self.assertRaisesRegex(
            PUBLISHER.LocalLedgerError,
            "not bound to its parent",
        ):
            fixture.publish_prepared(second)

    def test_08_receipt_time_must_advance_parent_snapshot(self) -> None:
        first = self.fixture()
        with self.assertRaisesRegex(
            PUBLISHER.LocalLedgerError,
            "not newer than its parent snapshot",
        ):
            first.prepare(
                "task_2ebaf90139c5",
                completed_at="2026-08-28T10:56:46.046Z",
            )
        fixture = self.fixture()
        self.assert_success(fixture.publish("task_2ebaf90139c5"))
        with self.assertRaisesRegex(
            PUBLISHER.LocalLedgerError,
            "not newer than its parent snapshot",
        ):
            fixture.prepare(
                "task_1254fa47b9b7",
                completed_at="2026-08-29T00:00:01.000Z",
            )

    def test_09_post_commit_check_cannot_retract_validated_pointer(self) -> None:
        fixture = self.fixture()
        self.assert_success(fixture.publish("task_2ebaf90139c5"))
        prepared = fixture.prepare("task_1254fa47b9b7")
        paths = PUBLISHER.publication_paths(fixture.repo)
        failure = PUBLISHER.LocalLedgerError("forced post-commit validation failure")
        with mock.patch.object(
            PUBLISHER,
            "run_independent_validator",
            side_effect=[None, failure],
        ):
            fixture.publish_prepared(prepared)
        self.assertFalse((fixture.repo / PUBLISHER.JOURNAL_REL).exists())
        self.assert_success(fixture.validator("--check"))
        state = json.loads(paths["state"].read_text(encoding="utf-8"))
        tasks = {row["taskId"]: row for row in state["tasks"]}
        self.assertEqual(tasks["task_1254fa47b9b7"]["status"], "completed")

    def test_10_real_crash_lock_and_journal_roll_back_precommit(self) -> None:
        fixture = self.fixture()
        self.assert_success(fixture.publish("task_2ebaf90139c5"))
        paths = PUBLISHER.publication_paths(fixture.repo)
        before = {name: path.read_bytes() for name, path in paths.items()}
        crashed = fixture.crash_writer(with_journal=True)
        self.assertEqual(crashed.returncode, 73, crashed.stderr)
        self.assertTrue((fixture.repo / PUBLISHER.LOCK_REL).exists())
        self.assertTrue((fixture.repo / PUBLISHER.JOURNAL_REL).exists())
        self.assertTrue(PUBLISHER.recover_local_publication(fixture.repo))
        self.assertEqual(
            {name: path.read_bytes() for name, path in paths.items()}, before
        )
        self.assertFalse((fixture.repo / PUBLISHER.LOCK_REL).exists())
        self.assertFalse((fixture.repo / PUBLISHER.JOURNAL_REL).exists())
        self.assert_success(fixture.validator("--check"))

    def test_11_independent_replay_rejects_time_regression(self) -> None:
        fixture = self.fixture()
        self.assert_success(fixture.publish("task_2ebaf90139c5"))
        self.assert_success(fixture.publish("task_1254fa47b9b7"))
        pointer = json.loads(
            (fixture.repo / PUBLISHER.LOCAL_POINTER_REL).read_text(
                encoding="utf-8"
            )
        )
        snapshot = json.loads(
            (fixture.repo / pointer["bundlePath"] / "snapshot.json").read_text(
                encoding="utf-8"
            )
        )
        first_record, second_record = snapshot["transitionReceipts"]
        first_receipt = json.loads(
            (fixture.repo / first_record["path"]).read_text(encoding="utf-8")
        )
        second_receipt = json.loads(
            (fixture.repo / second_record["path"]).read_text(encoding="utf-8")
        )
        second_receipt["completedAtUtc"] = first_receipt["completedAtUtc"]
        regressed_data = PUBLISHER.document(second_receipt)
        regressed_sha = PUBLISHER.digest(regressed_data)
        regressed_path = fixture.repo / PUBLISHER.EVIDENCE_REL / regressed_sha
        regressed_path.write_bytes(regressed_data)
        snapshot["transitionReceipts"][-1] = {
            "path": (PUBLISHER.EVIDENCE_REL / regressed_sha).as_posix(),
            "bytes": len(regressed_data),
            "sha256": regressed_sha,
        }
        snapshot["transitionHeadSha256"] = regressed_sha
        snapshot["sealedAtUtc"] = first_receipt["completedAtUtc"]
        override = snapshot["taskOverrides"]["task_1254fa47b9b7"]
        override["completedAtUtc"] = first_receipt["completedAtUtc"]
        override["completionEvidence"]["receiptSha256"] = regressed_sha
        legacy = VALIDATOR.load_legacy(fixture.repo)
        bundles = VALIDATOR.local_bundle_map(fixture.repo)
        with self.assertRaisesRegex(
            VALIDATOR.ValidationError,
            "not strictly increasing",
        ):
            VALIDATOR.replay(snapshot, legacy, fixture.repo, bundles)

    def test_12_real_crash_before_journal_reclaims_orphan_lock(self) -> None:
        fixture = self.fixture()
        crashed = fixture.crash_writer(with_journal=False)
        self.assertEqual(crashed.returncode, 73, crashed.stderr)
        self.assertTrue((fixture.repo / PUBLISHER.LOCK_REL).exists())
        self.assertFalse(PUBLISHER.recover_local_publication(fixture.repo))
        self.assertFalse((fixture.repo / PUBLISHER.LOCK_REL).exists())
        self.assert_success(fixture.publish("task_2ebaf90139c5"))

    def test_13_committed_journal_rolls_forward(self) -> None:
        fixture = self.fixture()
        self.assert_success(fixture.publish("task_2ebaf90139c5"))
        paths = PUBLISHER.publication_paths(fixture.repo)
        before = {name: PUBLISHER.preimage(path) for name, path in paths.items()}
        self.assert_success(fixture.publish("task_1254fa47b9b7"))
        committed = {name: path.read_bytes() for name, path in paths.items()}
        journal = {
            "documentKind": "local-progress-ledger-publication-journal",
            "schemaVersion": 1,
            "transactionId": "committed-test",
            "commitPoint": "pointer-replacement",
            "preimages": before,
            "newHashes": {
                name: PUBLISHER.digest(data) for name, data in committed.items()
            },
        }
        PUBLISHER.write_atomic(
            fixture.repo / PUBLISHER.JOURNAL_REL,
            PUBLISHER.document(journal),
            "committed test journal",
        )
        self.assertTrue(PUBLISHER.recover_local_publication(fixture.repo))
        self.assertEqual(
            {name: path.read_bytes() for name, path in paths.items()}, committed
        )
        self.assertFalse((fixture.repo / PUBLISHER.JOURNAL_REL).exists())
        self.assert_success(fixture.validator("--check"))

    def test_14_external_pointer_edit_after_journal_is_preserved(self) -> None:
        fixture = self.fixture()
        self.assert_success(fixture.publish("task_2ebaf90139c5"))
        prepared = fixture.prepare("task_1254fa47b9b7")
        paths = PUBLISHER.publication_paths(fixture.repo)
        external = b"external-pointer-after-journal\n"
        original_write = PUBLISHER.write_atomic

        def inject(path: Path, data: bytes, label: str) -> None:
            original_write(path, data, label)
            if label == "local publication journal":
                paths["pointer"].write_bytes(external)

        with mock.patch.object(PUBLISHER, "write_atomic", side_effect=inject):
            with self.assertRaisesRegex(
                PUBLISHER.LocalLedgerError,
                "rollback refused",
            ):
                fixture.publish_prepared(prepared)
        self.assertEqual(paths["pointer"].read_bytes(), external)
        self.assertTrue((fixture.repo / PUBLISHER.JOURNAL_REL).exists())

    def test_15_external_pointer_edit_after_commit_is_preserved(self) -> None:
        fixture = self.fixture()
        self.assert_success(fixture.publish("task_2ebaf90139c5"))
        prepared = fixture.prepare("task_1254fa47b9b7")
        paths = PUBLISHER.publication_paths(fixture.repo)
        external = b"external-pointer-after-commit\n"

        def validate(repo: Path, pointer: Path | None = None) -> None:
            if pointer is not None:
                return
            paths["pointer"].write_bytes(external)
            raise PUBLISHER.LocalLedgerError("forced final validation failure")

        with mock.patch.object(
            PUBLISHER, "run_independent_validator", side_effect=validate
        ):
            with self.assertRaisesRegex(
                PUBLISHER.LocalLedgerError,
                "journal retained",
            ):
                fixture.publish_prepared(prepared)
        self.assertEqual(paths["pointer"].read_bytes(), external)
        self.assertTrue((fixture.repo / PUBLISHER.JOURNAL_REL).exists())

    def test_16_live_writer_lock_is_never_reclaimed(self) -> None:
        fixture = self.fixture()
        lock, token, _, guard = PUBLISHER.acquire_lock(fixture.repo)
        try:
            with self.assertRaisesRegex(
                PUBLISHER.LocalLedgerError,
                "already active",
            ):
                PUBLISHER.recover_local_publication(fixture.repo)
        finally:
            PUBLISHER.release_lock(lock, token, guard)

    def test_17_mismatched_stale_owner_cannot_be_bypassed_on_retry(self) -> None:
        fixture = self.fixture()
        self.assert_success(fixture.publish("task_2ebaf90139c5"))
        paths = PUBLISHER.publication_paths(fixture.repo)
        images = {name: PUBLISHER.preimage(path) for name, path in paths.items()}
        lock = {
            "documentKind": "local-ledger-lock",
            "schemaVersion": 1,
            "pid": 999999999,
            "platform": PUBLISHER.sys.platform,
            "host": PUBLISHER.socket.gethostname(),
            "token": "stale-owner-a",
        }
        journal = {
            "documentKind": "local-progress-ledger-publication-journal",
            "schemaVersion": 1,
            "transactionId": "different-owner-b",
            "commitPoint": "pointer-replacement",
            "preimages": images,
            "newHashes": {
                name: PUBLISHER.current_hash(path) for name, path in paths.items()
            },
        }
        PUBLISHER.write_atomic(
            fixture.repo / PUBLISHER.LOCK_REL,
            PUBLISHER.document(lock),
            "mismatched stale lock",
        )
        PUBLISHER.write_atomic(
            fixture.repo / PUBLISHER.JOURNAL_REL,
            PUBLISHER.document(journal),
            "mismatched journal",
        )
        for _ in range(2):
            with self.assertRaisesRegex(
                PUBLISHER.LocalLedgerError,
                "does not own",
            ):
                PUBLISHER.recover_local_publication(fixture.repo)
            self.assertTrue((fixture.repo / PUBLISHER.LOCK_REL).exists())
            self.assertTrue((fixture.repo / PUBLISHER.JOURNAL_REL).exists())

    def test_18_simultaneous_recoverers_never_overlap(self) -> None:
        fixture = self.fixture()
        self.assert_success(fixture.publish("task_2ebaf90139c5"))
        paths = PUBLISHER.publication_paths(fixture.repo)
        before = {name: path.read_bytes() for name, path in paths.items()}
        crashed = fixture.crash_writer(with_journal=True)
        self.assertEqual(crashed.returncode, 73, crashed.stderr)
        first = fixture.start_recovery()
        second = fixture.start_recovery()
        first_stdout, first_stderr = first.communicate(timeout=30)
        second_stdout, second_stderr = second.communicate(timeout=30)
        self.assertIn(first.returncode, {0, 2}, first_stderr)
        self.assertIn(second.returncode, {0, 2}, second_stderr)
        self.assertTrue(
            first.returncode == 0 or second.returncode == 0,
            first_stderr + second_stderr,
        )
        self.assertEqual(
            {name: path.read_bytes() for name, path in paths.items()}, before
        )
        self.assertFalse((fixture.repo / PUBLISHER.LOCK_REL).exists())
        self.assertFalse((fixture.repo / PUBLISHER.JOURNAL_REL).exists())
        self.assert_success(fixture.validator("--check"))

    def test_19_guard_identity_is_independent_of_temp_namespace(self) -> None:
        fixture = self.fixture()
        expected = fixture.repo / PUBLISHER.GUARD_REL
        original = tempfile.tempdir
        with tempfile.TemporaryDirectory(prefix="alternate-temp-namespace-") as alternate:
            try:
                tempfile.tempdir = alternate
                self.assertEqual(PUBLISHER.guard_path(fixture.repo), expected)
            finally:
                tempfile.tempdir = original
            guard = PUBLISHER.acquire_advisory_guard(fixture.repo)
            try:
                code = r'''
import importlib.util
import sys
from pathlib import Path

spec = importlib.util.spec_from_file_location("alternate_guard", Path(sys.argv[1]))
module = importlib.util.module_from_spec(spec)
assert spec and spec.loader
spec.loader.exec_module(module)
try:
    acquired = module.acquire_advisory_guard(Path(sys.argv[2]))
except module.LocalLedgerError:
    raise SystemExit(0)
module.release_advisory_guard(acquired)
raise SystemExit(3)
'''
                environment = os.environ.copy()
                environment["TEMP"] = alternate
                environment["TMP"] = alternate
                blocked = subprocess.run(
                    [
                        sys.executable,
                        "-B",
                        "-c",
                        code,
                        str(PUBLISHER_PATH),
                        str(fixture.repo),
                    ],
                    cwd=fixture.repo,
                    env=environment,
                    text=True,
                    encoding="utf-8",
                    capture_output=True,
                    check=False,
                )
                self.assertEqual(blocked.returncode, 0, blocked.stderr)
            finally:
                PUBLISHER.release_advisory_guard(guard)

    def test_20_pinned_v1_failure_chain_remains_valid(self) -> None:
        fixture = self.fixture()
        fixture.bootstrap_failed_g6()
        self.assert_success(fixture.validator("--check"))
        pointer = json.loads(
            (fixture.repo / PUBLISHER.LOCAL_POINTER_REL).read_text(
                encoding="utf-8"))
        self.assertEqual(
            pointer["bundleId"], fixture.FAILED_G6_V1_BUNDLE_ID
        )
        snapshot = json.loads(
            (fixture.repo / pointer["bundlePath"] / "snapshot.json").read_text(
                encoding="utf-8"))
        self.assertEqual(snapshot["schemaVersion"], 1)
        self.assertNotIn("recoveryTaskDefinitions", snapshot)

    def test_21_forged_recovery_authority_is_rejected(self) -> None:
        fixture = self.fixture()
        fixture.bootstrap_failed_g6()
        self.assert_failure(
            fixture.publish_recovery(
                outcome="succeeded", corruption="authority"),
            "authority is not bound",
        )

    def test_22_unknown_dependency_and_cycle_are_rejected(self) -> None:
        for corruption, expected in (
            ("unknown", "unknown dependency"),
            ("cycle", "self cycle"),
        ):
            with self.subTest(corruption=corruption):
                fixture = self.fixture()
                fixture.bootstrap_failed_g6()
                self.assert_failure(
                    fixture.publish_recovery(
                        outcome="succeeded", corruption=corruption),
                    expected,
                )

    def test_23_wrong_gate_supersession_is_rejected(self) -> None:
        fixture = self.fixture()
        fixture.bootstrap_failed_g6()
        self.assert_failure(
            fixture.publish_recovery(
                outcome="succeeded", corruption="supersedes"),
            "unknown event",
        )

    def test_24_failed_regate_does_not_unlock_and_projects_failure(self) -> None:
        fixture = self.fixture()
        fixture.bootstrap_failed_g6()
        self.assert_success(fixture.publish_recovery(outcome="failed"))
        self.assert_success(fixture.validator("--check"))
        state = json.loads(
            (fixture.repo / PUBLISHER.LOCAL_STATE_REL).read_text(
                encoding="utf-8"))
        tasks = {row["taskId"]: row for row in state["tasks"]}
        phases = {row["phase"]: row for row in state["phases"]}
        self.assertEqual(tasks["task_g6_r1"]["status"], "failed")
        self.assertEqual(tasks["task_1a40c0d302ae"]["status"], "blocked")
        self.assertEqual(phases["P6"]["engineeringGate"], "FAIL/BLOCKED")
        self.assertEqual(phases["P6"]["gateTaskId"], "task_g6_r1")

    def test_25_pass_regate_unlocks_replaced_dependency_and_projects_pass(self) -> None:
        fixture = self.fixture()
        fixture.bootstrap_failed_g6()
        old_pointer = json.loads(
            (fixture.repo / PUBLISHER.LOCAL_POINTER_REL).read_text(
                encoding="utf-8"))
        old_snapshot = json.loads(
            (fixture.repo / old_pointer["bundlePath"] / "snapshot.json").read_text(
                encoding="utf-8"))
        old_failure = old_snapshot["transitionHeadSha256"]
        self.assert_success(fixture.publish_recovery(outcome="succeeded"))
        self.assert_success(fixture.validator("--check"))
        state = json.loads(
            (fixture.repo / PUBLISHER.LOCAL_STATE_REL).read_text(
                encoding="utf-8"))
        tasks = {row["taskId"]: row for row in state["tasks"]}
        phases = {row["phase"]: row for row in state["phases"]}
        self.assertEqual(tasks["task_4af12cfad98a"]["status"], "failed")
        self.assertEqual(tasks["task_g6_r1"]["status"], "completed")
        self.assertEqual(tasks["task_1a40c0d302ae"]["status"], "ready")
        self.assertEqual(state["nextRunnableTaskIds"], ["task_1a40c0d302ae"])
        self.assertEqual(phases["P6"]["engineeringGate"], "PASS")
        self.assertEqual(phases["P6"]["gateTaskId"], "task_g6_r1")
        self.assertIn(old_failure, phases["P6"]["gateHistoryReceiptSha256s"])
        self.assertEqual(len(phases["P6"]["gateHistoryReceiptSha256s"]), 2)

    def test_26_recovery_authority_tamper_is_rejected(self) -> None:
        fixture = self.fixture()
        fixture.bootstrap_failed_g6()
        self.assert_success(fixture.publish_recovery(outcome="succeeded"))
        pointer = json.loads(
            (fixture.repo / PUBLISHER.LOCAL_POINTER_REL).read_text(
                encoding="utf-8"))
        snapshot = json.loads(
            (fixture.repo / pointer["bundlePath"] / "snapshot.json").read_text(
                encoding="utf-8"))
        authority = snapshot["recoveryTaskDefinitions"]["task_g6_r1"][
            "authorityArtifact"]
        (fixture.repo / authority["path"]).write_text(
            "tampered authority\n", encoding="utf-8")
        self.assert_failure(fixture.validator("--check"), "bytes/hash differ")

    def test_27_v2_schema_is_closed_world(self) -> None:
        fixture = self.fixture()
        fixture.bootstrap_failed_g6()
        clean_path = fixture.recovery_receipt(outcome="succeeded")
        clean = json.loads(clean_path.read_text(encoding="utf-8"))
        for name in ("authorityArtifact", "report", "verdictArtifact"):
            clean[name]["path"] = (
                PUBLISHER.EVIDENCE_REL / clean[name]["sha256"]
            ).as_posix()
        schema = json.loads(
            (fixture.repo / "docs/editor-parity/schemas/local-progress-ledger.schema.json").read_text(
                encoding="utf-8"))
        validator = VALIDATOR.JsonSchemaValidator(schema)
        validator.validate(clean)
        extra = copy.deepcopy(clean)
        extra["unexpected"] = True
        with self.assertRaises(VALIDATOR.JsonSchemaError):
            validator.validate(extra)
        partial = copy.deepcopy(clean)
        del partial["authorityArtifact"]
        with self.assertRaises(VALIDATOR.JsonSchemaError):
            validator.validate(partial)
        self.assert_failure(
            fixture.publish_recovery(outcome="succeeded", corruption="extra"),
            "keys differ",
        )

    def test_28_existing_task_can_transition_after_v2_recovery(self) -> None:
        fixture = self.fixture()
        fixture.bootstrap_failed_g6()
        self.assert_success(fixture.publish_recovery(outcome="succeeded"))
        self.assert_success(fixture.publish("task_1a40c0d302ae"))
        self.assert_success(fixture.validator("--check"))
        state = json.loads(
            (fixture.repo / PUBLISHER.LOCAL_STATE_REL).read_text(
                encoding="utf-8"))
        tasks = {row["taskId"]: row for row in state["tasks"]}
        self.assertEqual(tasks["task_1a40c0d302ae"]["status"], "completed")
        self.assertEqual(tasks["task_395d3d07b807"]["status"], "ready")
        self.assertEqual(state["nextRunnableTaskIds"], ["task_395d3d07b807"])

    def test_29_explicit_schema2_ordinary_g7_bridge_projects_pass(self) -> None:
        fixture = self.fixture()
        fixture.bootstrap_bundle(fixture.FAILED_G7_V2_BUNDLE_ID)
        # The historical prefix remains valid and intentionally has no P7
        # Gate event until a later recovery receipt explicitly names G7's hash.
        self.assert_success(fixture.validator("--check"))
        before_state = json.loads(
            (fixture.repo / PUBLISHER.LOCAL_STATE_REL).read_text(
                encoding="utf-8"))
        before_phase = next(row for row in before_state["phases"]
                            if row["phase"] == "P7")
        self.assertEqual(before_phase["engineeringGate"], "NOT_RUN")
        self.assertNotIn(
            "e297bd471f5f7a5441abe7bb9e645736b6085987d9e452f4168de71445aca7cd",
            before_phase["gateHistoryReceiptSha256s"],
        )

        receipt = fixture.g7_bridge_receipt()
        self.assert_success(fixture.local(
            "--receipt", str(receipt), "--evidence-root", str(fixture.evidence),
            "--publish"))
        self.assert_success(fixture.validator("--check"))
        pointer = json.loads(
            (fixture.repo / PUBLISHER.LOCAL_POINTER_REL).read_text(
                encoding="utf-8"))
        snapshot = json.loads(
            (fixture.repo / pointer["bundlePath"] / "snapshot.json").read_text(
                encoding="utf-8"))
        state = json.loads(
            (fixture.repo / PUBLISHER.LOCAL_STATE_REL).read_text(
                encoding="utf-8"))
        phases = {row["phase"]: row for row in state["phases"]}
        tasks = {row["taskId"]: row for row in state["tasks"]}
        self.assertEqual(phases["P7"]["engineeringGate"], "PASS")
        self.assertEqual(phases["P7"]["gateTaskId"], "task_g7_r1_bridge")
        self.assertEqual(phases["P7"]["gateHistoryReceiptSha256s"][0],
                         "e297bd471f5f7a5441abe7bb9e645736b6085987d9e452f4168de71445aca7cd")
        self.assertEqual(len(phases["P7"]["gateHistoryReceiptSha256s"]), 2)
        self.assertEqual(tasks["task_15f206f912f6"]["status"], "ready")
        events = snapshot["gateEvents"]
        bridged = [event for event in events
                   if event["eventId"] == "e297bd471f5f7a5441abe7bb9e645736b6085987d9e452f4168de71445aca7cd"]
        self.assertEqual(bridged, [{
            "eventId": "e297bd471f5f7a5441abe7bb9e645736b6085987d9e452f4168de71445aca7cd",
            "phase": "P7", "taskId": "task_3f216d250677",
            "gateVerdict": PUBLISHER.GATE_FAIL, "supersedesEventId": None,
        }])
        self.assertEqual(
            [event["supersedesEventId"] for event in events
             if event["phase"] == "P7" and event["taskId"] == "task_g7_r1_bridge"],
            ["e297bd471f5f7a5441abe7bb9e645736b6085987d9e452f4168de71445aca7cd"],
        )

    def test_30_schema2_bridge_rejects_unknown_or_mismatched_predecessor(self) -> None:
        for corruption in ("unknown", "mismatched"):
            with self.subTest(corruption=corruption):
                fixture = self.fixture()
                fixture.bootstrap_bundle(fixture.FAILED_G7_V2_BUNDLE_ID)
                receipt = fixture.g7_bridge_receipt(corruption=corruption)
                self.assert_failure(
                    fixture.local(
                        "--receipt", str(receipt), "--evidence-root",
                        str(fixture.evidence), "--publish"),
                    "unknown event",
                )
                # The failed publication must leave the pinned parent intact.
                self.assert_success(fixture.validator("--check"))

    def test_31_schema2_bridge_rejects_forged_candidate_identity(self) -> None:
        fixture = self.fixture()
        origin = PUBLISHER.legacy_origin(fixture.repo)
        legacy = VALIDATOR.load_legacy(fixture.repo)
        receipt_path = fixture.repo / PUBLISHER.EVIDENCE_REL / (
            "e297bd471f5f7a5441abe7bb9e645736b6085987d9e452f4168de71445aca7cd")
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        self.assertIsNone(PUBLISHER.ordinary_schema2_failed_gate_event(
            receipt, "0" * 64, origin["tasks"]))
        self.assertIsNone(VALIDATOR.ordinary_schema2_failed_gate_event(
            receipt, "0" * 64, legacy["tasks"]))
        for field, value in (
            ("taskId", "task_forged_gate"),
            ("displayName", "G7-forged"),
            ("toStatus", "completed"),
            ("outcome", "succeeded"),
        ):
            with self.subTest(field=field):
                forged = copy.deepcopy(receipt)
                forged[field] = value
                forged_sha = PUBLISHER.digest(PUBLISHER.document(forged))
                self.assertIsNone(
                    PUBLISHER.ordinary_schema2_failed_gate_event(
                        forged, forged_sha, origin["tasks"]))
                self.assertIsNone(
                    VALIDATOR.ordinary_schema2_failed_gate_event(
                        forged, forged_sha, legacy["tasks"]))

        recovery_fields = copy.deepcopy(receipt)
        recovery_fields["gateEvent"] = None
        recovery_sha = PUBLISHER.digest(PUBLISHER.document(recovery_fields))
        self.assertIsNone(
            PUBLISHER.ordinary_schema2_failed_gate_event(
                recovery_fields, recovery_sha, origin["tasks"]))
        self.assertIsNone(
            VALIDATOR.ordinary_schema2_failed_gate_event(
                recovery_fields, recovery_sha, legacy["tasks"]))

    def test_32_failed_g7_gate_rejects_dependency_replacements(self) -> None:
        fixture = self.fixture()
        fixture.bootstrap_bundle(fixture.FAILED_G7_V2_BUNDLE_ID)
        receipt = fixture.g7_bridge_receipt(
            outcome="failed", corruption="failed-replacement")
        self.assert_failure(
            fixture.local(
                "--receipt", str(receipt), "--evidence-root",
                str(fixture.evidence), "--publish"),
            "failed Gate cannot declare dependency replacements",
        )
        self.assert_success(fixture.validator("--check"))
        state = json.loads(
            (fixture.repo / PUBLISHER.LOCAL_STATE_REL).read_text(
                encoding="utf-8"))
        tasks = {row["taskId"]: row for row in state["tasks"]}
        self.assertEqual(tasks["task_15f206f912f6"]["status"], "blocked")

    def test_33_successful_recovery_integration_releases_failed_dependency(self) -> None:
        fixture = self.fixture()
        fixture.bootstrap_bundle(fixture.FAILED_P8_90_V2_BUNDLE_ID)
        receipt = fixture.recovery_integration_receipt()
        self.assert_success(fixture.local(
            "--receipt", str(receipt), "--evidence-root", str(fixture.evidence),
            "--publish"))
        self.assert_success(fixture.validator("--check"))
        state = json.loads(
            (fixture.repo / PUBLISHER.LOCAL_STATE_REL).read_text(
                encoding="utf-8"))
        tasks = {row["taskId"]: row for row in state["tasks"]}
        self.assertEqual(tasks["task_p8_90_r1"]["status"], "completed")
        self.assertEqual(tasks["task_99a11c23bb9a"]["status"], "failed")
        self.assertEqual(tasks["task_eb85190ed1ac"]["status"], "ready")
        self.assertEqual(state["nextRunnableTaskIds"],
                         ["task_eb85190ed1ac"])

    def test_34_recovery_integration_only_supersedes_failed_task(self) -> None:
        for supersedes, dependencies, expected in (
            ("task_15f206f912f6", None, "failed task"),
            ("task_99a11c23bb9a", ["task_99a11c23bb9a"],
             "cannot depend on its superseded task"),
        ):
            with self.subTest(supersedes=supersedes):
                fixture = self.fixture()
                fixture.bootstrap_bundle(fixture.FAILED_P8_90_V2_BUNDLE_ID)
                receipt = fixture.recovery_integration_receipt(
                    supersedes=supersedes, dependencies=dependencies)
                self.assert_failure(
                    fixture.local(
                        "--receipt", str(receipt), "--evidence-root",
                        str(fixture.evidence), "--publish"),
                    expected,
                )
                self.assert_success(fixture.validator("--check"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
