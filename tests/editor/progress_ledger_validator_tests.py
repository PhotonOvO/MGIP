#!/usr/bin/env python3
"""Adversarial tests for the v2 editor progress-ledger protocol."""

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
import textwrap
import unittest
from contextlib import contextmanager
from datetime import datetime, timedelta, timezone
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
UPDATER_PATH = ROOT / "tools/update_editor_progress_ledger.py"
VALIDATOR_PATH = ROOT / "tools/validate_editor_progress_ledger.py"
SCHEMA_PATH = ROOT / "docs/editor-parity/schemas/progress-ledger.schema.json"
STORE = Path("docs/editor-parity/progress-ledger")
POINTER = Path("docs/editor-parity/progress-ledger-current.json")
STATE = Path("docs/editor-parity/progress-ledger-state.json")
LEDGER = Path("docs/gltf-scene-editor-parity-progress.md")


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


UPDATER = load_module("progress_ledger_updater_under_test", UPDATER_PATH)
VALIDATOR = load_module("progress_ledger_validator_under_test", VALIDATOR_PATH)


def payload(value: object) -> bytes:
    return json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":")).encode("ascii")


def document(value: object) -> bytes:
    return json.dumps(value, ensure_ascii=True, sort_keys=True, indent=2).encode("ascii") + b"\n"


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))


def read_state(path: Path) -> dict:
    return UPDATER.decode_state(read_json(path))


def read_snapshot(path: Path) -> dict:
    return UPDATER.decode_snapshot(read_json(path))


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(document(value))


def add_seconds(value: str, count: int) -> str:
    parsed = datetime.fromisoformat(value.replace("Z", "+00:00")) + timedelta(seconds=count)
    rendered = parsed.astimezone(timezone.utc).isoformat(timespec="milliseconds")
    return rendered[:-6] + "Z"


def recompute_snapshot(snapshot: dict) -> None:
    projection = {key: snapshot[key] for key in (
        "runId", "runtimeId", "runGeneration", "runTaskIndex", "tasks", "decisionGates"
    )}
    snapshot["revisionSha256"] = digest(payload(projection))


def recovery_settlement_payload() -> dict:
    return {
        "provenance": UPDATER.RECOVERY_SETTLEMENT_PROVENANCE,
        "objectiveCompleted": True,
        "acceptanceVerdict": "FAIL",
        "workerOutcome": "failed",
        "originalMessageId": UPDATER.RECOVERY_SETTLEMENT_MESSAGE_ID,
        "originalDispatchId": UPDATER.RECOVERY_SETTLEMENT_DISPATCH_ID,
        "reportPath": r"C:\Temp\mgif-recovery-task_8c29dd510a99\report.md",
        "reportSha256": "A" * 64,
        "commandLedgerSha256": "B" * 64,
        "postSourceManifestSha256": "C" * 64,
        "machineReceiptSha256": "D" * 64,
        "integrityCheckSha256": "E" * 64,
        "blockingFindings": copy.deepcopy(UPDATER.RECOVERY_SETTLEMENT_FINDINGS),
        "fixedPolicy": copy.deepcopy(UPDATER.RECOVERY_SETTLEMENT_POLICY),
        "note": UPDATER.RECOVERY_SETTLEMENT_NOTE,
    }


def add_recovery_settlement(snapshot: dict) -> dict:
    task_id = UPDATER.RECOVERY_SETTLEMENT_TASK_ID
    index = next(row for row in snapshot["runTaskIndex"] if row["taskId"] == task_id)
    task = next(row for row in snapshot["tasks"] if row["taskId"] == task_id)
    dispatched = add_seconds(snapshot["capturedAtUtc"], 1)
    failed = add_seconds(dispatched, 1)
    settled = add_seconds(failed, 1)
    snapshot["capturedAtUtc"] = add_seconds(settled, 1)
    raw = recovery_settlement_payload()
    worker = UPDATER.normalize_result(payload(raw).decode("ascii"), task_id, settled)
    assert worker is not None
    index["status"] = "completed"
    task.update({
        "status": "completed", "completedAtUtc": settled,
        "dispatch": {
            "dispatchId": UPDATER.RECOVERY_SETTLEMENT_DISPATCH_ID, "status": "failed",
            "dispatchedAtUtc": dispatched, "completedAtUtc": failed,
        },
        "workerResult": worker,
        "completionProvenance": UPDATER.RECOVERY_SETTLEMENT_PROVENANCE,
    })
    recompute_snapshot(snapshot)
    return task


class Fixture:
    COPY_FILES = (
        "tools/json_schema_subset.py",
        "tools/update_editor_progress_ledger.py",
        "tools/validate_editor_progress_ledger.py",
        "docs/editor-parity/schemas/progress-ledger.schema.json",
        "docs/gltf-scene-editor-parity-task.md",
        "docs/editor-parity/manifest.json",
        "docs/editor-parity/LICENSE-SOURCES.md",
    )

    def __init__(self, published: bool = True) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="mgif-ledger-v2-tests-")
        self.base = Path(self.temp.name)
        self.repo = self.base / "repo"
        self.repo.mkdir()
        pointer = read_json(ROOT / POINTER)
        self.source_bundle = ROOT / pointer["bundlePath"]
        self.seed_state = (self.source_bundle / "state.json").read_bytes()
        self.seed_markdown = (self.source_bundle / "progress.md").read_bytes()
        self.seed_snapshot = (self.source_bundle / "snapshot.json").read_bytes()
        for relative in self.COPY_FILES:
            target = self.repo / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, target)
        shutil.copytree(ROOT / STORE, self.repo / STORE)
        if published:
            for relative in (POINTER, STATE, LEDGER):
                target = self.repo / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(ROOT / relative, target)
        else:
            bundles = self.repo / STORE / "bundles"
            if bundles.exists():
                shutil.rmtree(bundles)
        self.evidence = self.base / "external-evidence"
        self.evidence.mkdir()

    def close(self) -> None:
        self.temp.cleanup()

    def selected_bundle(self) -> Path:
        pointer = read_json(self.repo / POINTER)
        return self.repo / pointer["bundlePath"]

    def snapshot(self) -> dict:
        return UPDATER.decode_snapshot(json.loads(self.seed_snapshot))

    def run(self, script: Path, *arguments: str, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
        environment = os.environ.copy()
        if env:
            environment.update(env)
        return subprocess.run(
            [sys.executable, "-B", str(script), *map(str, arguments)],
            cwd=self.repo, env=environment, text=True, encoding="utf-8",
            capture_output=True, check=False,
        )

    def validator(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return self.run(self.repo / "tools/validate_editor_progress_ledger.py", "--repo-root", str(self.repo), *arguments)

    def updater(self, *arguments: str, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
        return self.run(self.repo / "tools/update_editor_progress_ledger.py", "--repo-root", str(self.repo), *arguments, env=env)

    def run_without_site(self, *arguments: str | Path) -> subprocess.CompletedProcess[str]:
        environment = os.environ.copy()
        environment.pop("PYTHONPATH", None)
        environment["PYTHONNOUSERSITE"] = "1"
        return subprocess.run(
            [sys.executable, "-S", "-B", *map(str, arguments)],
            cwd=self.repo, env=environment, text=True, encoding="utf-8",
            capture_output=True, check=False,
        )

    def make_receipt(
        self,
        snapshot: dict,
        phase: str = "P5",
        task_id: str | None = None,
        supersedes: str | None | object = ...,
        verdict: str | None = None,
        suffix: str = "a",
    ) -> tuple[Path, dict]:
        task_id = task_id or UPDATER.GATE_TASKS[phase]
        verdict = verdict or f"GATE {phase} ENGINEERING PASS"
        if supersedes is ...:
            previous = UPDATER.decode_state(json.loads(self.seed_state))
            phase_events = [event for event in previous["gateHistory"] if event["phase"] == phase]
            supersedes = phase_events[-1]["eventId"] if phase_events else None
        task = next(row for row in snapshot["tasks"] if row["taskId"] == task_id)
        index = next(row for row in snapshot["runTaskIndex"] if row["taskId"] == task_id)
        completed = add_seconds(snapshot["capturedAtUtc"], 10)
        dispatched = add_seconds(snapshot["capturedAtUtc"], 5)
        snapshot["capturedAtUtc"] = add_seconds(completed, 1)
        report = self.evidence / f"report-{suffix}.md"
        report.write_text(f"sealed Gate report {suffix}\n", encoding="utf-8")
        report_record = {"path": str(report.resolve()), "bytes": report.stat().st_size, "sha256": digest(report.read_bytes())}
        receipt: dict = {
            "documentKind": "gate-receipt", "schemaVersion": 2, "runId": UPDATER.RUN_ID,
            "phase": phase, "gateTaskId": task_id, "dispatchId": f"ctx_test_{suffix}",
            "workerMessageId": f"msg_test_{suffix}", "completedAtUtc": completed,
            "reviewExecutionOutcome": "succeeded", "gateVerdict": verdict, "gateScope": "engineering",
            "supersedesReceiptSha256": supersedes, "report": report_record,
            "verdictArtifact": {}, "inputFingerprintSha256": "0" * 64,
            "fixedPolicy": copy.deepcopy(UPDATER.FIXED_POLICY),
        }
        verdict_value = {
            "documentKind": "gate-verdict", "schemaVersion": 2, "runId": UPDATER.RUN_ID,
            "phase": phase, "gateTaskId": task_id, "dispatchId": receipt["dispatchId"],
            "gateVerdict": verdict, "gateScope": "engineering",
            "supersedesReceiptSha256": supersedes, "fixedPolicy": copy.deepcopy(UPDATER.FIXED_POLICY),
        }
        verdict_path = self.evidence / f"verdict-{suffix}.json"
        write_json(verdict_path, verdict_value)
        receipt["verdictArtifact"] = {
            "path": str(verdict_path.resolve()), "bytes": verdict_path.stat().st_size,
            "sha256": digest(verdict_path.read_bytes()),
        }
        attestation = {
            "phase": phase, "gateVerdict": verdict, "reportSha256": report_record["sha256"],
            "verdictSha256": receipt["verdictArtifact"]["sha256"],
        }
        body = "Gate execution complete.\nPROGRESS_LEDGER_ATTESTATION=" + payload(attestation).decode("ascii")
        raw_result = {
            "body": body, "completedAt": completed, "completedBy": f"term_test_{suffix}",
            "filesModified": [], "messageId": receipt["workerMessageId"], "outcome": "succeeded",
            "provenance": "worker_report", "reportPath": str(report.resolve()),
            "reportedBy": f"term_test_{suffix}", "subject": verdict,
        }
        raw_payload = payload(raw_result).decode("ascii")
        task.update({
            "status": "completed", "completedAtUtc": completed,
            "dispatch": {
                "dispatchId": receipt["dispatchId"], "status": "completed",
                "dispatchedAtUtc": dispatched, "completedAtUtc": completed,
            },
            "workerResult": {
                "messageId": receipt["workerMessageId"], "outcome": "succeeded",
                "completedAtUtc": completed, "reportPath": str(report.resolve()),
                "reportedBy": raw_result["reportedBy"], "subject": verdict, "body": body,
                "resultPayload": raw_payload, "resultSha256": digest(raw_payload.encode("ascii")),
            },
        })
        index["status"] = "completed"
        recompute_snapshot(snapshot)
        snapshot_path = self.base / f"snapshot-{suffix}.json"
        draft_path = self.evidence / f"receipt-{suffix}.json"
        write_json(snapshot_path, snapshot)
        write_json(draft_path, receipt)
        fingerprint = self.updater(
            "--snapshot", str(snapshot_path), "--evidence-root", str(self.evidence),
            "--print-receipt-fingerprint", str(draft_path),
        )
        if fingerprint.returncode != 0:
            raise AssertionError(fingerprint.stderr or fingerprint.stdout)
        receipt["inputFingerprintSha256"] = fingerprint.stdout.strip().splitlines()[-1]
        write_json(draft_path, receipt)
        return draft_path, receipt

    def candidate(self, snapshot: dict, receipt: Path | None = None, name: str = "candidate") -> subprocess.CompletedProcess[str]:
        output = self.base / name
        output.mkdir()
        snapshot_path = self.base / f"{name}-snapshot.json"
        write_json(snapshot_path, snapshot)
        args = ["--snapshot", str(snapshot_path), "--candidate-dir", str(output)]
        if receipt:
            args.extend(["--gate-receipt", str(receipt), "--evidence-root", str(self.evidence)])
        result = self.updater(*args)
        result.candidate_dir = output  # type: ignore[attr-defined]
        return result

    def publish_candidate(self, candidate: Path) -> str:
        return UPDATER.publish(
            self.repo, candidate,
            (candidate / STATE.name).read_bytes(),
            (candidate / LEDGER.name).read_bytes(),
            (candidate / "orca-snapshot.json").read_bytes(),
            {},
        )

    def empty_stage(self) -> Path:
        stage = self.base / "publish-stage"
        (stage / "evidence/sha256").mkdir(parents=True, exist_ok=True)
        return stage

    def publish_seed(self, stage: Path | None = None) -> str:
        return UPDATER.publish(
            self.repo, stage or self.empty_stage(),
            self.seed_state, self.seed_markdown, self.seed_snapshot, {},
        )


@contextmanager
def fixture(published: bool = True):
    value = Fixture(published)
    try:
        yield value
    finally:
        value.close()


class ProgressLedgerV2Tests(unittest.TestCase):
    maxDiff = None

    def assert_success(self, result: subprocess.CompletedProcess[str]) -> None:
        self.assertEqual(result.returncode, 0, result.stderr or result.stdout)

    def assert_failure(self, result: subprocess.CompletedProcess[str], text: str | None = None) -> None:
        self.assertNotEqual(result.returncode, 0, result.stdout)
        if text:
            self.assertIn(text, result.stderr + result.stdout)

    def test_01_published_migration_and_fixed_policy(self) -> None:
        state_wire = read_json(ROOT / STATE)
        self.assertEqual(state_wire["schemaVersion"], 4)
        state = UPDATER.decode_state(state_wire)
        self.assertEqual(state["schemaVersion"], 2)
        self.assertEqual(len(state["gateHistory"]), 10)
        self.assertEqual([event["gateVerdict"] for event in state["gateHistory"] if event["phase"] in {"P1", "P3", "P4"}],
                         ["GATE P1 FAIL/BLOCKED", "GATE P1 ENGINEERING PASS", "GATE P3 FAIL/BLOCKED",
                          "GATE P3 ENGINEERING PASS", "GATE P4 FAIL/BLOCKED", "GATE P4 ENGINEERING PASS"])
        self.assertEqual(state["policy"], {
            "basis": {
                "finalAcceptanceComplete": False, "manifestPassClaimAllowed": False,
                "manifestReleaseGateStatus": "blocked_pending_owner_root_license_selection",
                "metalArtifactAccepted": False, "rootLicenseSelected": False,
            },
            "f42": "BLOCKED", "metal": "Not Verified", "r24": "BLOCKED",
            "releaseDistribution": "NOT PASS",
        })
        p5_events = [event for event in state["gateHistory"] if event["phase"] == "P5"]
        self.assertEqual(p5_events[0]["eventId"], "c54ac4bfec9dd72471a291c39682a698e66e4ba80aaea0519eedb0e31a218595")
        self.assertEqual(p5_events[0]["gateVerdict"], "GATE P5 FAIL/BLOCKED")
        self.assertEqual(p5_events[-1]["gateVerdict"], "GATE P5 ENGINEERING PASS")
        self.assertEqual(p5_events[-1]["supersedesEventId"], p5_events[0]["eventId"])
        phases = {row["phase"]: row for row in state["phases"]}
        self.assertEqual((phases["P5"]["engineeringGate"], phases["P5"]["executionStatus"]), ("PASS", "GATED"))
        self.assertEqual([(row["gateId"], row["outcome"]) for row in state["integrationPrerequisites"]],
                         [("gate_ca804cbec712", "accepted"), ("gate_b66203c11b10", "accepted")])

    def test_01b_p5_r3_repair_is_projected_as_active(self) -> None:
        display_name = "P5-R3 production repair after Gate P5 FAIL/BLOCKED"
        self.assertEqual(UPDATER.label(display_name), "P5-R3")
        self.assertEqual(VALIDATOR.phase_label(display_name), "P5-R3")
        task = {"taskId": "task_test_p5_r3", "label": "P5-R3", "status": "dispatched"}
        updater_phase = {row["phase"]: row for row in UPDATER.derive_phases([task], [], [])}["P5"]
        validator_phase = {
            row["phase"]: row
            for row in VALIDATOR.independently_derive_phases([task], [], [])
        }["P5"]
        self.assertEqual(updater_phase["executionStatus"], "REPAIR_ACTIVE")
        self.assertEqual(validator_phase, updater_phase)

    def test_02_full_shallow_and_deterministic_reconstruction(self) -> None:
        with fixture() as f:
            self.assert_success(f.validator("--check"))
            shallow = f.validator("--check", "--shallow-integrity")
            self.assert_success(shallow)
            self.assertIn("SHALLOW INTEGRITY ONLY", shallow.stdout)
            first = f.candidate(f.snapshot(), name="first")
            second = f.candidate(f.snapshot(), name="second")
            self.assert_success(first)
            self.assert_success(second)
            for relative in (STATE.name, LEDGER.name, "orca-snapshot.json"):
                self.assertEqual((first.candidate_dir / relative).read_bytes(), (second.candidate_dir / relative).read_bytes())  # type: ignore[attr-defined]
            first_evidence = {p.name: p.read_bytes() for p in (first.candidate_dir / "evidence/sha256").iterdir()}  # type: ignore[attr-defined]
            second_evidence = {p.name: p.read_bytes() for p in (second.candidate_dir / "evidence/sha256").iterdir()}  # type: ignore[attr-defined]
            self.assertEqual(first_evidence, second_evidence)

    def test_03_durable_evidence_deletion_full_fails_shallow_is_explicit(self) -> None:
        with fixture() as f:
            state = read_state(f.repo / STATE)
            evidence = f.repo / state["gateHistory"][0]["report"]["path"]
            evidence.unlink()
            self.assert_failure(f.validator("--check"), "durable")
            self.assert_success(f.validator("--check", "--shallow-integrity"))

    def test_04_exact_gate_authority_and_protocol_vectors(self) -> None:
        authority = read_json(ROOT / STORE / "authority.json")
        vectors = read_json(ROOT / STORE / "protocol-vectors.json")
        self.assertEqual({row["phase"]: row["taskId"] for row in authority["materializedGateTasks"]}, UPDATER.GATE_TASKS)
        VALIDATOR.evaluate_vectors(authority, vectors)
        source = VALIDATOR_PATH.read_text(encoding="utf-8")
        self.assertNotRegex(source, r"\bimport\s+update_editor_progress_ledger\b|\bfrom\s+update_editor_progress_ledger\b")

    def test_05_every_phase_rejects_ordinary_or_wrong_gate_task(self) -> None:
        ordinary = "task_116c32740787"
        for phase in ("P5", "P6", "P7", "P8", "P9", "P10"):
            with self.subTest(phase=phase), fixture() as f:
                snap = f.snapshot()
                receipt, _ = f.make_receipt(snap, phase=phase, task_id=ordinary, suffix=phase.lower())
                self.assert_failure(f.candidate(snap, receipt), "ordinary or wrong-phase")
        for phase, wrong_phase in (("P5", "P6"), ("P6", "P5"), ("P10", "P9")):
            with self.subTest(phase=phase, wrong=wrong_phase), fixture() as f:
                snap = f.snapshot()
                receipt, _ = f.make_receipt(
                    snap, phase=phase, task_id=UPDATER.GATE_TASKS[wrong_phase],
                    suffix=f"{phase.lower()}-{wrong_phase.lower()}",
                )
                self.assert_failure(f.candidate(snap, receipt), "ordinary or wrong-phase")

    def test_06_bootstrap_authority_and_result_identity_mutations_fail(self) -> None:
        with fixture() as f:
            authority = f.repo / STORE / "authority.json"
            changed = read_json(authority)
            changed["bootstrapEvents"][0]["resultSha256"] = "0" * 64
            write_json(authority, changed)
            self.assert_failure(f.validator("--check"), "authority")
        with fixture() as f:
            state = read_state(f.repo / STATE)
            event = state["gateHistory"][0]
            result_path = f.repo / event["orcaResult"]["artifact"]["path"]
            result_path.write_bytes(result_path.read_bytes() + b" ")
            self.assert_failure(f.validator("--check"), "durable")

    def test_07_worker_message_subject_body_report_verdict_binding(self) -> None:
        cases = ("message", "subject", "body", "report", "verdict", "result")
        for case in cases:
            with self.subTest(case=case), fixture() as f:
                snap = f.snapshot()
                receipt_path, receipt = f.make_receipt(snap, suffix=case)
                task = next(row for row in snap["tasks"] if row["taskId"] == receipt["gateTaskId"])
                if case == "message":
                    receipt["workerMessageId"] = "msg_mismatch"
                    write_json(receipt_path, receipt)
                elif case == "subject":
                    task["workerResult"]["subject"] = "different subject"
                elif case == "body":
                    task["workerResult"]["body"] = "PROGRESS_LEDGER_ATTESTATION={}"
                elif case == "report":
                    receipt["report"]["sha256"] = "1" * 64
                    write_json(receipt_path, receipt)
                elif case == "verdict":
                    receipt["verdictArtifact"]["sha256"] = "2" * 64
                    write_json(receipt_path, receipt)
                else:
                    task["workerResult"]["resultSha256"] = "3" * 64
                recompute_snapshot(snap)
                self.assert_failure(f.candidate(snap, receipt_path))

    def test_08_decision_gate_carry_forward_and_closed_outcomes(self) -> None:
        with fixture() as f:
            previous = read_state(f.repo / STATE)
            snapshot = f.snapshot()
            snapshot["decisionGates"] = []
            with self.assertRaises(UPDATER.LedgerError):
                pending = copy.deepcopy(previous)
                pending["decisionGateHistory"][-1]["status"] = "pending"
                pending["decisionGateHistory"][-1]["outcome"] = "pending"
                pending["decisionGateHistory"][-1]["resolution"] = None
                pending["decisionGateHistory"][-1]["resolvedAtUtc"] = None
                UPDATER.decision_history(snapshot, pending)
            history, current = UPDATER.decision_history(snapshot, previous)
            self.assertEqual(history, previous["decisionGateHistory"])
            self.assertEqual({row["gateId"] for row in current}, {"gate_ca804cbec712", "gate_b66203c11b10"})
        policies = {row["gateId"]: row for row in read_json(ROOT / STORE / "authority.json")["decisionGatePolicies"]}
        self.assertEqual(UPDATER.classify_gate({"id": "gate_b66203c11b10", "task_id": "task_8c29dd510a99", "status": "resolved", "resolution": "accept_verified_r2f"}, policies), "accepted")
        self.assertEqual(UPDATER.classify_gate({"id": "gate_b66203c11b10", "task_id": "task_8c29dd510a99", "status": "resolved", "resolution": "reject_or_repair"}, policies), "rejected")
        with self.assertRaises(UPDATER.LedgerError):
            UPDATER.classify_gate({"id": "gate_b66203c11b10", "task_id": "task_8c29dd510a99", "status": "resolved", "resolution": "unknown"}, policies)

    def test_09_negative_or_omitted_prerequisite_never_unblocks_p5(self) -> None:
        event = {
            "phase": "P5", "gateVerdict": "GATE P5 ENGINEERING PASS",
            "gateTaskId": UPDATER.GATE_TASKS["P5"], "eventId": "a" * 64,
        }
        task = {"taskId": UPDATER.GATE_TASKS["P5"], "label": "G5", "status": "completed"}
        for outcome in ("pending", "rejected", "superseded"):
            gate = {"gateId": f"gate_{outcome}", "outcome": outcome}
            with self.subTest(outcome=outcome):
                with self.assertRaises(UPDATER.LedgerError):
                    UPDATER.derive_phases([task], [event], [gate])
                with self.assertRaises(VALIDATOR.ValidationError):
                    VALIDATOR.independently_derive_phases([task], [event], [gate])
        self.assertEqual(UPDATER.derive_phases([task], [event], [{"gateId": "gate_ok", "outcome": "accepted"}])[5]["engineeringGate"], "PASS")

    def test_10_legitimate_regate_appends_after_publication(self) -> None:
        with fixture() as f:
            first_snapshot = f.snapshot()
            first_receipt, _ = f.make_receipt(first_snapshot, suffix="regate-1", verdict="GATE P5 FAIL/BLOCKED")
            first_candidate = f.candidate(first_snapshot, first_receipt, "regate-first")
            self.assert_success(first_candidate)
            f.publish_candidate(first_candidate.candidate_dir)  # type: ignore[attr-defined]
            before = UPDATER.decode_state(read_json(f.repo / STATE))
            first_event = before["gateHistory"][-1]
            second_snapshot = copy.deepcopy(first_snapshot)
            second_receipt, _ = f.make_receipt(
                second_snapshot, supersedes=first_event["eventId"], suffix="regate-2",
                verdict="GATE P5 ENGINEERING PASS",
            )
            second_candidate = f.candidate(second_snapshot, second_receipt, "regate-second")
            self.assert_success(second_candidate)
            after = UPDATER.decode_state(read_json(second_candidate.candidate_dir / STATE.name))  # type: ignore[attr-defined]
            self.assertEqual(after["gateHistory"][:len(before["gateHistory"])], before["gateHistory"])
            self.assertEqual(after["gateHistory"][-1]["supersedesEventId"], first_event["eventId"])
            self.assertEqual(after["phases"][5]["engineeringGate"], "PASS")

    def test_11_snapshot_index_lifecycle_timestamp_dependency_matrix(self) -> None:
        base = json.loads((ROOT / POINTER).read_text())
        snapshot = read_snapshot(ROOT / base["bundlePath"] / "snapshot.json")
        pending = next(row for row in snapshot["tasks"] if row["status"] == "pending")
        invalid = []
        case = copy.deepcopy(snapshot)
        next(row for row in case["runTaskIndex"] if row["taskId"] == pending["taskId"])["displayName"] += " drift"
        invalid.append(case)
        case = copy.deepcopy(snapshot)
        task = next(row for row in case["tasks"] if row["taskId"] == pending["taskId"])
        task["status"] = "completed"
        next(row for row in case["runTaskIndex"] if row["taskId"] == pending["taskId"])["status"] = "completed"
        invalid.append(case)
        case = copy.deepcopy(snapshot)
        task = next(row for row in case["tasks"] if row["taskId"] == pending["taskId"])
        task["status"] = "dispatched"
        next(row for row in case["runTaskIndex"] if row["taskId"] == pending["taskId"])["status"] = "dispatched"
        invalid.append(case)
        case = copy.deepcopy(snapshot)
        completed = next(row for row in case["tasks"] if row["status"] == "completed" and row["dispatch"])
        completed["workerResult"]["outcome"] = "failed"
        invalid.append(case)
        case = copy.deepcopy(snapshot)
        completed = next(row for row in case["tasks"] if row["status"] == "completed" and row["dispatch"])
        completed["dispatch"]["completedAtUtc"] = add_seconds(completed["dispatch"]["dispatchedAtUtc"], -1)
        invalid.append(case)
        case = copy.deepcopy(snapshot)
        task = next(row for row in case["tasks"] if len(row["dependencies"]) > 1)
        task["dependencies"] = list(reversed(task["dependencies"]))
        invalid.append(case)
        case = copy.deepcopy(snapshot)
        task = next(row for row in case["tasks"] if row["dependencies"])
        task["dependencies"][0] = "task_missing"
        invalid.append(case)
        case = copy.deepcopy(snapshot)
        case["capturedAtUtc"] = "2000-01-01T00:00:00.000Z"
        invalid.append(case)
        for number, candidate in enumerate(invalid):
            with self.subTest(case=number):
                recompute_snapshot(candidate)
                with self.assertRaises(VALIDATOR.ValidationError):
                    VALIDATOR.validate_snapshot(candidate, read_json(ROOT / STORE / "authority.json"))

    def test_12_alternate_run_runtime_generation_revision_rejected(self) -> None:
        pointer = read_json(ROOT / POINTER)
        snapshot = read_snapshot(ROOT / pointer["bundlePath"] / "snapshot.json")
        authority = read_json(ROOT / STORE / "authority.json")
        for field, value, recompute in (
            ("runId", "run_forged", True),
            ("runtimeId", "runtime_forged", True),
            ("runGeneration", 2, True),
            ("revisionSha256", "f" * 64, False),
        ):
            with self.subTest(field=field):
                candidate = copy.deepcopy(snapshot)
                candidate[field] = value
                if recompute:
                    recompute_snapshot(candidate)
                with self.assertRaises(VALIDATOR.ValidationError):
                    VALIDATOR.validate_snapshot(candidate, authority)

    def test_13_fake_orca_two_pass_stability_and_drift(self) -> None:
        with fixture() as f:
            snapshot_path = f.base / "fake-orca-source-snapshot.json"
            write_json(snapshot_path, f.snapshot())
            fake = f.base / "fake_orca.py"
            counter = f.base / "counter.txt"
            fake.write_text(textwrap.dedent(r'''
                import json, os, pathlib, sys
                snapshot = json.loads(pathlib.Path(os.environ["FAKE_SNAPSHOT"]).read_text(encoding="utf-8"))
                runtime = snapshot["runtimeId"]
                args = sys.argv[1:]
                def emit(result):
                    print(json.dumps({"id":"fake","ok":True,"result":result,"_meta":{"runtimeId":runtime}}, ensure_ascii=False))
                tracked = {row["taskId"]: row for row in snapshot["tasks"]}
                if args[0] == "status":
                    emit({"runtime":{"runtimeId":runtime,"state":"ready"}})
                elif args[:2] == ["orchestration","run-show"]:
                    emit({"run":{"id":snapshot["runId"],"consumer_generation":snapshot["runGeneration"]}})
                elif args[:2] == ["orchestration","task-list"]:
                    count_path = pathlib.Path(os.environ["FAKE_COUNTER"])
                    count = int(count_path.read_text() or "0") + 1 if count_path.exists() else 1
                    count_path.write_text(str(count))
                    rows = []
                    mutate = os.environ.get("FAKE_DRIFT") == "1" and count % 2 == 1
                    changed = False
                    for index in snapshot["runTaskIndex"]:
                        task = tracked.get(index["taskId"])
                        status = index["status"]
                        if mutate and not changed and task is None and status == "pending":
                            status, changed = "blocked", True
                        raw_result = None
                        deps, completed = [], None
                        if task:
                            deps, completed = task["dependencies"], task["completedAtUtc"]
                            worker = task["workerResult"]
                            raw_result = worker["resultPayload"] if worker else None
                        rows.append({"id":index["taskId"],"display_name":index["displayName"],"status":status,
                                     "deps":json.dumps(deps),"result":raw_result,
                                     "created_at":index["createdAtUtc"],"completed_at":completed})
                    emit({"runId":snapshot["runId"],"tasks":rows})
                elif args[:2] == ["orchestration","gate-list"]:
                    gates = [{"id":g["gateId"],"task_id":g["taskId"],"status":g["status"],"question":g["question"],
                              "options":json.dumps(g["options"], ensure_ascii=False),"created_at":g["createdAtUtc"],
                              "resolved_at":g["resolvedAtUtc"],"resolution":g["resolution"]} for g in snapshot["decisionGates"]]
                    emit({"runId":snapshot["runId"],"gates":gates})
                elif args[:2] == ["orchestration","dispatch-show"]:
                    task = tracked[args[args.index("--task")+1]]
                    d = task["dispatch"]
                    value = None if d is None else {"id":d["dispatchId"],"status":d["status"],
                        "dispatched_at":d["dispatchedAtUtc"],"completed_at":d["completedAtUtc"]}
                    emit({"dispatch":value})
                else:
                    raise SystemExit(2)
            '''), encoding="utf-8")
            authority = read_json(ROOT / STORE / "authority.json")
            environment = {"FAKE_SNAPSHOT": str(snapshot_path), "FAKE_COUNTER": str(counter)}
            with mock.patch.dict(os.environ, environment, clear=False):
                stable = UPDATER.capture([sys.executable, str(fake)], set(), authority)
            self.assertEqual(stable["runtimeId"], authority["runtimeId"])
            counter.unlink(missing_ok=True)
            environment["FAKE_DRIFT"] = "1"
            with mock.patch.dict(os.environ, environment, clear=False):
                with self.assertRaisesRegex(UPDATER.LedgerError, "drifted"):
                    UPDATER.capture([sys.executable, str(fake)], set(), authority)

            settlement_snapshot = f.snapshot()
            settlement_task = add_recovery_settlement(settlement_snapshot)
            settlement_path = f.base / "settlement-snapshot.json"
            write_json(settlement_path, settlement_snapshot)
            counter.unlink(missing_ok=True)
            environment = {
                "FAKE_SNAPSHOT": str(settlement_path), "FAKE_COUNTER": str(counter), "FAKE_DRIFT": "0",
            }
            with mock.patch.dict(os.environ, environment, clear=False):
                captured = UPDATER.capture(
                    [sys.executable, str(fake)], {UPDATER.RECOVERY_SETTLEMENT_TASK_ID}, authority,
                )
            captured_task = next(row for row in captured["tasks"] if row["taskId"] == UPDATER.RECOVERY_SETTLEMENT_TASK_ID)
            self.assertEqual(captured_task, settlement_task)
            self.assertEqual(captured_task["status"], "completed")
            self.assertEqual(captured_task["dispatch"]["status"], "failed")
            self.assertEqual(captured_task["workerResult"]["outcome"], "failed")
            self.assertEqual(captured_task["workerResult"]["recoverySettlement"]["acceptanceVerdict"], "FAIL")
            self.assertEqual(captured_task["workerResult"]["recoverySettlement"]["reportSha256"], "a" * 64)
            VALIDATOR.validate_snapshot(copy.deepcopy(captured), authority)

            raw = recovery_settlement_payload()
            for name, mutate in (
                ("extra", lambda item: item.update({"unexpected": True})),
                ("objective", lambda item: item.update({"objectiveCompleted": False})),
                ("verdict", lambda item: item.update({"acceptanceVerdict": "PASS"})),
                ("outcome", lambda item: item.update({"workerOutcome": "succeeded"})),
                ("message", lambda item: item.update({"originalMessageId": "msg_forged"})),
                ("dispatch", lambda item: item.update({"originalDispatchId": "ctx_forged"})),
                ("report", lambda item: item.update({"reportPath": r"C:\outside\report.md"})),
                ("hash", lambda item: item.update({"integrityCheckSha256": "g" * 64})),
                ("findings", lambda item: item["blockingFindings"].reverse()),
                ("policy", lambda item: item["fixedPolicy"].update({"releaseDistribution": "PASS"})),
                ("note", lambda item: item.update({"note": item["note"] + " altered"})),
            ):
                with self.subTest(recovery_settlement=name):
                    forged = copy.deepcopy(raw)
                    mutate(forged)
                    encoded = payload(forged)
                    with self.assertRaises(UPDATER.LedgerError):
                        UPDATER.normalize_result(encoded.decode("ascii"), UPDATER.RECOVERY_SETTLEMENT_TASK_ID, settlement_task["completedAtUtc"])
                    with self.assertRaises(VALIDATOR.ValidationError):
                        VALIDATOR.normalize_result_payload(encoded, UPDATER.RECOVERY_SETTLEMENT_TASK_ID, settlement_task["completedAtUtc"])
            encoded = payload(raw)
            with self.assertRaises(UPDATER.LedgerError):
                UPDATER.normalize_result(encoded.decode("ascii"), "task_forged", settlement_task["completedAtUtc"])
            with self.assertRaises(VALIDATOR.ValidationError):
                VALIDATOR.normalize_result_payload(encoded, "task_forged", settlement_task["completedAtUtc"])

            for name, mutate in (
                ("marker", lambda item: item.pop("completionProvenance")),
                ("dispatch", lambda item: item["dispatch"].update({"dispatchId": "ctx_forged"})),
                ("task-status", lambda item: item.update({"status": "failed"})),
                ("dispatch-status", lambda item: item["dispatch"].update({"status": "completed"})),
                ("worker-outcome", lambda item: item["workerResult"].update({"outcome": "succeeded"})),
            ):
                with self.subTest(recovery_lifecycle=name):
                    forged_snapshot = copy.deepcopy(captured)
                    forged_task = next(row for row in forged_snapshot["tasks"] if row["taskId"] == UPDATER.RECOVERY_SETTLEMENT_TASK_ID)
                    mutate(forged_task)
                    recompute_snapshot(forged_snapshot)
                    with self.assertRaises(UPDATER.LedgerError):
                        UPDATER.validate_snapshot(copy.deepcopy(forged_snapshot), authority)
                    with self.assertRaises(VALIDATOR.ValidationError):
                        VALIDATOR.validate_snapshot(copy.deepcopy(forged_snapshot), authority)

    def test_14_reparse_junction_symlink_hardlink_and_lexical_paths(self) -> None:
        self.assertEqual(os.name, "nt", "the acceptance race matrix requires Windows reparse semantics")

        readers = (
            ("updater", UPDATER, lambda path, root: UPDATER.safe_pinned(path, [root], "race").data,
             UPDATER.LedgerError),
            ("validator", VALIDATOR, lambda path, root: VALIDATOR.opened_file(path, "race", roots=(root,)).data,
             VALIDATOR.ValidationError),
        )

        def junction(link: Path, target: Path) -> None:
            made = subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(target)],
                                  text=True, capture_output=True, check=False)
            self.assertEqual(made.returncode, 0, made.stderr or made.stdout)

        def file_link(link: Path, target: Path) -> bool:
            made = subprocess.run(["cmd", "/c", "mklink", str(link), str(target)],
                                  text=True, capture_output=True, check=False)
            return made.returncode == 0

        def remove_link(path: Path) -> None:
            if not os.path.lexists(path):
                return
            if path.is_dir() and not path.is_symlink():
                path.rmdir()
            else:
                path.unlink()

        # Static directory and file reparse points, including an outside-root
        # target, must be rejected by both independent readers.
        with fixture() as f:
            inside_dir, outside_dir = f.repo / "inside-target", f.base / "outside-target"
            inside_dir.mkdir(); outside_dir.mkdir()
            (inside_dir / "receipt.json").write_bytes(b"inside")
            (outside_dir / "receipt.json").write_bytes(b"outside")
            for kind, make_link, target in (
                ("directory-inside", junction, inside_dir),
                ("directory-outside", junction, outside_dir),
            ):
                link = f.repo / kind
                make_link(link, target)
                try:
                    for reader_name, _, reader, error in readers:
                        with self.subTest(kind=kind, reader=reader_name), self.assertRaisesRegex(error, "reparse"):
                            reader(link / "receipt.json", f.repo)
                finally:
                    remove_link(link)
            for kind, target in (("file-inside", inside_dir / "receipt.json"),
                ("file-outside", outside_dir / "receipt.json")):
                link = f.repo / f"{kind}.json"
                made = file_link(link, target)
                try:
                    for reader_name, _, reader, error in readers:
                        with self.subTest(kind=kind, reader=reader_name):
                            if made:
                                with self.assertRaisesRegex(error, "reparse"):
                                    reader(link, f.repo)
                            else:
                                # Standard-user Windows may prohibit creating
                                # file symlinks. Exercise the same final-handle
                                # metadata branch without skipping the case.
                                link.write_bytes(target.read_bytes())
                                module = UPDATER if reader_name == "updater" else VALIDATOR
                                info_name = "_win_info" if module is UPDATER else "_windows_info"
                                original_info = getattr(module, info_name)

                                def marked_reparse(api, handle, label, *, original=original_info):
                                    info = original(api, handle, label)
                                    if str(module.lexical(link)) in label:
                                        info.attributes |= 0x00000400
                                    return info

                                with mock.patch.object(module, info_name, side_effect=marked_reparse):
                                    with self.assertRaisesRegex(error, "reparse"):
                                        reader(link, f.repo)
                                link.unlink()
                finally:
                    remove_link(link)

            # Hard links are rejected even when the link path is inside the
            # root and its target name is outside it.
            for kind, target in (("inside", inside_dir / "receipt.json"),
                                 ("outside", outside_dir / "receipt.json")):
                hard = f.repo / f"hard-{kind}.json"
                os.link(target, hard)
                for reader_name, _, reader, error in readers:
                    with self.subTest(kind=f"hard-{kind}", reader=reader_name), self.assertRaisesRegex(error, "hard link"):
                        reader(hard, f.repo)

        # Before-component-open swaps reproduce the old check/open exploit.
        # OPEN_REPARSE_POINT binds the opened component, so neither an inside
        # nor outside junction can provide bytes.
        for reader_name, module, reader, error in readers:
            for location in ("inside", "outside"):
                with self.subTest(race="directory-before-open", target=location, reader=reader_name), fixture() as f:
                    checked, saved = f.repo / "checked", f.repo / "checked-saved"
                    inside_target, outside_target = f.repo / "alternate", f.base / "outside"
                    checked.mkdir(); inside_target.mkdir(); outside_target.mkdir()
                    (checked / "receipt.json").write_bytes(b"inside")
                    (inside_target / "receipt.json").write_bytes(b"alternate")
                    (outside_target / "receipt.json").write_bytes(b"outside")
                    selected = inside_target if location == "inside" else outside_target
                    fired = False

                    def swap(stage: str, path: Path) -> None:
                        nonlocal fired
                        if not fired and stage == "before-component-open" and module.lexical(path) == module.lexical(checked):
                            fired = True
                            os.replace(checked, saved)
                            junction(checked, selected)

                    try:
                        with mock.patch.object(module, "_SECURE_READ_TEST_HOOK", swap):
                            with self.assertRaisesRegex(error, "reparse"):
                                reader(checked / "receipt.json", f.repo)
                    finally:
                        remove_link(checked)
                        if saved.exists():
                            os.replace(saved, checked)
                    self.assertTrue(fired)

            for location in ("inside", "outside"):
                with self.subTest(race="file-before-open", target=location, reader=reader_name), fixture() as f:
                    parent = f.repo / "checked"; parent.mkdir()
                    victim, saved = parent / "receipt.json", parent / "receipt-saved.json"
                    inside_target, outside_target = f.repo / "alternate.json", f.base / "outside.json"
                    victim.write_bytes(b"inside"); inside_target.write_bytes(b"alternate"); outside_target.write_bytes(b"outside")
                    selected = inside_target if location == "inside" else outside_target
                    fired = False
                    synthetic_reparse = False
                    info_name = "_win_info" if module is UPDATER else "_windows_info"
                    original_info = getattr(module, info_name)

                    def swap_file(stage: str, path: Path) -> None:
                        nonlocal fired, synthetic_reparse
                        if not fired and stage == "before-component-open" and module.lexical(path) == module.lexical(victim):
                            fired = True
                            os.replace(victim, saved)
                            if not file_link(victim, selected):
                                synthetic_reparse = True
                                victim.write_bytes(selected.read_bytes())

                    def race_reparse_info(api, handle, label):
                        info = original_info(api, handle, label)
                        if synthetic_reparse and str(module.lexical(victim)) in label:
                            info.attributes |= 0x00000400
                        return info

                    try:
                        with mock.patch.object(module, "_SECURE_READ_TEST_HOOK", swap_file), \
                             mock.patch.object(module, info_name, side_effect=race_reparse_info):
                            with self.assertRaisesRegex(error, "reparse"):
                                reader(victim, f.repo)
                    finally:
                        remove_link(victim)
                        if saved.exists():
                            os.replace(saved, victim)
                    self.assertTrue(fired)

            # Every descendant open is relative to the retained ancestor
            # handle. Replacing the validated lexical ancestor with a junction
            # must be rejected even when the alternate bytes remain in-root.
            for location in ("inside", "outside"):
                with self.subTest(race="ancestor-aba", target=location, reader=reader_name), fixture() as f:
                    parent, saved_parent = f.repo / "checked", f.repo / "checked-saved"
                    target = f.repo / "alternate" if location == "inside" else f.base / "outside"
                    parent.mkdir(); target.mkdir()
                    victim = parent / "receipt.json"; victim.write_bytes(b"original")
                    (target / "receipt.json").write_bytes(("alternate-" + location).encode("ascii"))
                    fired = False

                    def ancestor_aba(stage: str, path: Path) -> None:
                        nonlocal fired
                        if not fired and stage == "after-component-validation" and module.lexical(path) == module.lexical(parent):
                            fired = True
                            os.replace(parent, saved_parent)
                            junction(parent, target)

                    try:
                        with mock.patch.object(module, "_SECURE_READ_TEST_HOOK", ancestor_aba), \
                             self.assertRaisesRegex(error, "retained chain"):
                            reader(victim, f.repo)
                    finally:
                        remove_link(parent)
                        if saved_parent.exists():
                            os.replace(saved_parent, parent)
                    self.assertTrue(fired)

            # Once the final file handle is open, delete, replacement, and
            # in-place byte mutation either fail at the OS sharing boundary or
            # make the exact-opened-object validation fail.
            for attack in ("remove", "replace", "bytes"):
                with self.subTest(race="after-open", attack=attack, reader=reader_name), fixture() as f:
                    parent, saved_parent = f.repo / "checked", f.repo / "checked-saved"
                    parent.mkdir(); victim = parent / "receipt.json"; victim.write_bytes(b"inside")
                    replacement = parent / "replacement.json"; replacement.write_bytes(b"outside")
                    fired = False

                    def after_open(stage: str, path: Path) -> None:
                        nonlocal fired
                        component = module.lexical(path)
                        if fired:
                            return
                        if stage == "after-open" and component == module.lexical(victim):
                            fired = True
                            try:
                                if attack == "remove":
                                    victim.unlink()
                                elif attack == "replace":
                                    os.replace(replacement, victim)
                                else:
                                    victim.write_bytes(b"outsid")
                            except OSError:
                                pass

                    try:
                        with mock.patch.object(module, "_SECURE_READ_TEST_HOOK", after_open):
                            try:
                                data = reader(victim, f.repo)
                            except error:
                                data = b""
                        self.assertNotEqual(data, b"outside")
                        self.assertNotEqual(data, b"outsid")
                    finally:
                        if os.path.lexists(parent) and parent.is_dir() and not (parent / "receipt.json").exists() and saved_parent.exists():
                            remove_link(parent)
                        if saved_parent.exists() and not parent.exists():
                            os.replace(saved_parent, parent)
                    self.assertTrue(fired)

            # Removal before the final open fails closed. A regular-file byte
            # replacement before open is bound to the replacement handle but
            # rejected by the caller's expected size/hash identity.
            with self.subTest(race="removal-before-open", reader=reader_name), fixture() as f:
                victim = f.repo / "victim.json"; victim.write_bytes(b"inside")
                fired = False

                def remove_before(stage: str, path: Path) -> None:
                    nonlocal fired
                    if not fired and stage == "before-component-open" and module.lexical(path) == module.lexical(victim):
                        fired = True; victim.unlink()

                with mock.patch.object(module, "_SECURE_READ_TEST_HOOK", remove_before):
                    with self.assertRaises(error):
                        reader(victim, f.repo)
                self.assertTrue(fired)

            with self.subTest(race="replacement-before-open", reader=reader_name), fixture() as f:
                victim = f.repo / "victim.json"; victim.write_bytes(b"inside")
                replacement = f.repo / "replacement.json"; replacement.write_bytes(b"outside")
                expected = {"path": "victim.json", "bytes": 6, "sha256": digest(b"inside")}
                fired = False

                def replace_before(stage: str, path: Path) -> None:
                    nonlocal fired
                    if not fired and stage == "before-component-open" and module.lexical(path) == module.lexical(victim):
                        fired = True; os.replace(replacement, victim)

                with mock.patch.object(module, "_SECURE_READ_TEST_HOOK", replace_before):
                    if module is UPDATER:
                        absolute = {**expected, "path": str(victim)}
                        with self.assertRaisesRegex(error, "does not match"):
                            UPDATER.validate_artifact(absolute, [f.repo], "race artifact")
                    else:
                        with self.assertRaisesRegex(error, "identity differs"):
                            VALIDATOR.artifact_path(expected, f.repo, f.repo, "race artifact")
                self.assertTrue(fired)

    def test_15_schema_valid_documents_and_negative_matrix(self) -> None:
        schema = read_json(SCHEMA_PATH)
        validator = VALIDATOR.JsonSchemaValidator(schema)
        try:
            import jsonschema as optional_jsonschema
        except ImportError:
            reference_validator = None
        else:
            optional_jsonschema.Draft202012Validator.check_schema(schema)
            reference_validator = optional_jsonschema.Draft202012Validator(
                schema, format_checker=optional_jsonschema.FormatChecker())
        pointer = read_json(ROOT / POINTER)
        selected_state_wire = read_json(ROOT / pointer["bundlePath"] / "state.json")
        selected_snapshot_wire = read_json(ROOT / pointer["bundlePath"] / "snapshot.json")
        legacy_state = UPDATER.decode_state(selected_state_wire)
        legacy_snapshot = UPDATER.decode_snapshot(selected_snapshot_wire)
        receipt = read_json(ROOT / legacy_state["gateHistory"][0]["receipt"]["path"])
        verdict = read_json(ROOT / legacy_state["gateHistory"][0]["verdictArtifact"]["path"])

        # Selected v2 publications remain independently readable by both CLIs.
        # New v4 documents eliminate redundant association maps: task/result,
        # decision-transition, and Gate evidence relationships are structural.
        with fixture() as f:
            settlement_snapshot = f.snapshot()
            add_recovery_settlement(settlement_snapshot)
            candidate = f.candidate(settlement_snapshot, name="schema-v4")
            self.assert_success(candidate)
            state = read_json(candidate.candidate_dir / STATE.name)  # type: ignore[attr-defined]
            snapshot = read_json(candidate.candidate_dir / "orca-snapshot.json")  # type: ignore[attr-defined]
        self.assertEqual((state["schemaVersion"], snapshot["schemaVersion"]), (4, 4))
        for valid in (state, snapshot, receipt, verdict):
            validator.validate(valid)
            if reference_validator is not None:
                reference_validator.validate(valid)
        for legacy in (legacy_state, legacy_snapshot):
            with self.assertRaises(VALIDATOR.JsonSchemaError):
                validator.validate(legacy)
            if reference_validator is not None:
                self.assertTrue(list(reference_validator.iter_errors(legacy)))

        invalid: list[tuple[str, dict]] = []

        def reject(name: str, value: dict) -> None:
            invalid.append((name, value))

        # All prior legacy duplicate-object attacks remain rejected because a
        # v2 state/snapshot is not a schema-governed publication wire form.
        item = copy.deepcopy(legacy_state)
        duplicate = copy.deepcopy(item["phases"][0]); duplicate["summary"] += " changed"
        item["phases"].append(duplicate); reject("legacy-duplicate-phase", item)
        item = copy.deepcopy(legacy_state)
        duplicate = copy.deepcopy(item["tasks"][0]); duplicate["displayName"] += " changed"
        item["tasks"].append(duplicate); reject("legacy-duplicate-task", item)
        item = copy.deepcopy(legacy_snapshot)
        duplicate = copy.deepcopy(item["decisionGates"][0]); duplicate["question"] += " changed"
        item["decisionGates"].append(duplicate); reject("legacy-duplicate-gate", item)
        for identity, mutate in (
            ("event", lambda row, source: row.update({"completedAtUtc": source[1]["completedAtUtc"]})),
            ("message", lambda row, source: row.update({"workerMessageId": source[0]["workerMessageId"], "eventId": "1" * 64})),
            ("dispatch", lambda row, source: row.update({"dispatchId": source[0]["dispatchId"], "eventId": "2" * 64})),
            ("receipt", lambda row, source: row.update({"receipt": copy.deepcopy(source[0]["receipt"]), "eventId": "3" * 64})),
            ("report", lambda row, source: row.update({"report": copy.deepcopy(source[0]["report"]), "eventId": "4" * 64})),
            ("verdict", lambda row, source: row.update({"verdictArtifact": copy.deepcopy(source[0]["verdictArtifact"]), "eventId": "5" * 64})),
            ("result", lambda row, source: row.update({"orcaResult": copy.deepcopy(source[0]["orcaResult"]), "eventId": "6" * 64})),
            ("supersession", lambda row, source: row.update({"supersedesEventId": source[1]["supersedesEventId"], "eventId": "7" * 64})),
        ):
            item = copy.deepcopy(legacy_state)
            source = item["gateHistory"]
            duplicate = copy.deepcopy(source[1])
            if identity == "event":
                duplicate["eventId"] = source[0]["eventId"]
            else:
                mutate(duplicate, source)
            source.append(duplicate)
            reject(f"legacy-duplicate-history-{identity}", item)

        # Removed v3 order/association surfaces are closed. Snapshot index/task
        # data now share one keyed object, while Gate evidence has one row axis.
        for name, source, key in (
            ("snapshot-run-task-order", snapshot, "runTaskOrder"),
            ("snapshot-run-task-index", snapshot, "runTaskIndex"),
            ("snapshot-task-order", snapshot, "taskOrder"),
            ("snapshot-tasks", snapshot, "tasks"),
            ("snapshot-gate-order", snapshot, "decisionGateOrder"),
            ("snapshot-gate-tasks", snapshot, "decisionGateTasks"),
            ("snapshot-worker-results", snapshot, "workerResults"),
            ("snapshot-worker-messages", snapshot, "workerMessages"),
            ("state-task-order", state, "taskOrder"),
            ("state-worker-messages", state, "workerMessages"),
            ("state-prerequisite-projection", state, "integrationPrerequisites"),
            ("state-decision-history-map", state, "decisionGateHistory"),
        ):
            item = copy.deepcopy(source); item[key] = []
            reject(f"closed-removed-{name}", item)
        item = copy.deepcopy(state); item["phases"]["P00"] = copy.deepcopy(item["phases"]["P0"])
        reject("duplicate-phase-alias", item)
        for name, source, collection in (
            ("snapshot-run-task", snapshot, "runTasks"),
            ("snapshot-gate", snapshot, "decisionGates"),
            ("state-task", state, "tasks"),
            ("state-phase", state, "phases"),
        ):
            item = copy.deepcopy(source); item[collection]["bad identity"] = copy.deepcopy(next(iter(item[collection].values())))
            reject(f"illegal-{name}-identity", item)

        # Every phase/verdict/scope pairing is closed, including the forbidden
        # final-release claim. Test receipts and verdicts independently.
        for phase in UPDATER.PHASES:
            item = copy.deepcopy(receipt)
            item["phase"] = phase
            item["gateVerdict"] = "GATE P10 ENGINEERING PASS" if phase != "P10" else "GATE P9 ENGINEERING PASS"
            reject(f"receipt-phase-verdict-{phase}", item)
            item = copy.deepcopy(verdict)
            item["phase"] = phase
            item["gateVerdict"] = "GATE P10 ENGINEERING PASS" if phase != "P10" else "GATE P9 ENGINEERING PASS"
            reject(f"verdict-phase-verdict-{phase}", item)
        item = copy.deepcopy(receipt); item["gateVerdict"] = "UNAUTHENTICATED PASS"
        reject("receipt-illegal-verdict", item)
        item = copy.deepcopy(receipt); item["gateScope"] = "final-release"
        reject("receipt-illegal-scope", item)
        item = copy.deepcopy(receipt); item.update({"phase": "P10", "gateVerdict": "GATE P10 FINAL RELEASE PASS", "gateScope": "final-release"})
        reject("receipt-final-release", item)
        item = copy.deepcopy(verdict); item.update({"phase": "P10", "gateVerdict": "GATE P10 FINAL RELEASE PASS", "gateScope": "final-release"})
        reject("verdict-final-release", item)

        # Decision Gate and snapshot task/result lifecycle combinations remain
        # fully closed after result/message embedding.
        gate_id = next(iter(snapshot["decisionGates"]))
        for name, changes in (
            ("pending-with-resolution", {"status": "pending", "outcome": "pending", "resolvedAtUtc": snapshot["capturedAtUtc"], "resolution": "x"}),
            ("resolved-with-pending-outcome", {"status": "resolved", "outcome": "pending"}),
            ("resolved-with-null-resolution", {"status": "resolved", "outcome": "accepted", "resolvedAtUtc": snapshot["capturedAtUtc"], "resolution": None}),
        ):
            item = copy.deepcopy(snapshot); item["decisionGates"][gate_id].update(changes)
            reject(f"decision-gate-{name}", item)
        item = copy.deepcopy(snapshot); item["decisionGates"][gate_id]["options"].append(item["decisionGates"][gate_id]["options"][0])
        reject("decision-gate-duplicate-option", item)

        pending_task = next(task_id for task_id, row in snapshot["runTasks"].items()
                            if "dependencies" in row and row["status"] in {"pending", "ready", "blocked"})
        item = copy.deepcopy(snapshot); item["runTasks"][pending_task]["completedAtUtc"] = snapshot["capturedAtUtc"]
        reject("snapshot-nonterminal-completed-time", item)
        item = copy.deepcopy(snapshot); item["runTasks"][pending_task].update({"status": "dispatched", "dispatch": None})
        reject("snapshot-dispatched-without-dispatch", item)
        completed_task = next(task_id for task_id, row in snapshot["runTasks"].items()
                              if "dependencies" in row and row["status"] == "completed" and row["dispatch"] is not None)
        item = copy.deepcopy(snapshot); item["runTasks"][completed_task]["dispatch"]["status"] = "failed"
        reject("snapshot-completed-failed-dispatch", item)
        result_task = next(task_id for task_id, row in snapshot["runTasks"].items()
                           if "dependencies" in row and row["workerResult"] is not None)
        item = copy.deepcopy(snapshot); item["runTasks"][result_task]["workerResult"]["outcome"] = "pending"
        reject("snapshot-illegal-worker-outcome", item)
        item = copy.deepcopy(snapshot); item["runTasks"][result_task]["workerResult"] = None
        reject("snapshot-completed-without-worker-result", item)

        settlement_task = UPDATER.RECOVERY_SETTLEMENT_TASK_ID
        item = copy.deepcopy(snapshot); item["runTasks"][settlement_task]["workerResult"]["recoverySettlement"]["acceptanceVerdict"] = "PASS"
        reject("settlement-promoted-acceptance", item)
        item = copy.deepcopy(snapshot); item["runTasks"][settlement_task]["workerResult"]["recoverySettlement"]["unexpected"] = True
        reject("settlement-open-object", item)
        item = copy.deepcopy(snapshot); item["runTasks"][settlement_task]["workerResult"]["recoverySettlement"]["reportSha256"] = "A" * 64
        reject("settlement-noncanonical-hash", item)
        item = copy.deepcopy(snapshot); item["runTasks"][settlement_task]["workerResult"]["recoverySettlement"]["reportPath"] = r"C:\outside\report.md"
        reject("settlement-outside-report", item)
        item = copy.deepcopy(snapshot); item["runTasks"][settlement_task]["workerResult"]["recoverySettlement"]["blockingFindings"].append(item["runTasks"][settlement_task]["workerResult"]["recoverySettlement"]["blockingFindings"][0])
        reject("settlement-duplicate-finding", item)
        item = copy.deepcopy(snapshot); del item["runTasks"][settlement_task]["workerResult"]["recoverySettlement"]
        reject("settlement-missing-typed-record", item)
        item = copy.deepcopy(snapshot); item["runTasks"][settlement_task]["workerResult"]["reportedBy"] = "ordinary-worker"
        reject("settlement-forged-reporter", item)
        item = copy.deepcopy(snapshot); item["runTasks"][settlement_task]["workerResult"]["messageId"] = "msg_forged"
        reject("settlement-forged-message-association", item)
        item = copy.deepcopy(snapshot); del item["runTasks"][settlement_task]["completionProvenance"]
        reject("settlement-missing-snapshot-marker", item)
        item = copy.deepcopy(snapshot); item["runTasks"][settlement_task]["status"] = "failed"
        reject("settlement-failed-task-lifecycle", item)
        other_snapshot_task = next(task_id for task_id, row in snapshot["runTasks"].items()
                                   if task_id != settlement_task and "dependencies" in row)
        item = copy.deepcopy(snapshot); item["runTasks"][other_snapshot_task] = copy.deepcopy(item["runTasks"][settlement_task])
        reject("settlement-marker-under-forged-task-identity", item)

        pending_state_task = next(task_id for task_id, row in state["tasks"].items() if row["status"] in {"pending", "ready", "blocked"})
        item = copy.deepcopy(state); item["tasks"][pending_state_task]["workerOutcome"] = "succeeded"
        reject("state-nonterminal-worker-result", item)
        completed_state_task = next(task_id for task_id, row in state["tasks"].items() if row["status"] == "completed")
        item = copy.deepcopy(state); item["tasks"][completed_state_task]["workerOutcome"] = "not_reported"
        reject("state-terminal-missing-result", item)
        item = copy.deepcopy(state); item["tasks"][completed_state_task].update({"status": "failed", "dispatchId": None, "workerOutcome": "failed"})
        reject("state-failed-without-dispatch", item)
        item = copy.deepcopy(state); del item["tasks"][settlement_task]["completionProvenance"]
        reject("settlement-missing-state-marker", item)
        item = copy.deepcopy(state); item["tasks"][settlement_task]["workerOutcome"] = "succeeded"
        reject("settlement-state-outcome-promoted", item)
        item = copy.deepcopy(state); item["tasks"][settlement_task]["workerMessageId"] = "msg_forged"
        reject("settlement-state-message-forged", item)
        other_state_task = next(task_id for task_id in state["tasks"] if task_id != settlement_task)
        item = copy.deepcopy(state); item["tasks"][other_state_task] = copy.deepcopy(item["tasks"][settlement_task])
        reject("settlement-state-marker-under-forged-task-identity", item)

        # New phase wire records contain only non-authoritative display/task
        # metadata. Status, Gate task, receipt history, and verdict are derived.
        for phase in UPDATER.PHASES:
            for field, value in (
                ("engineeringGate", "PASS"), ("executionStatus", "GATED"),
                ("gateTaskId", "task_forged"), ("gateHistoryReceiptSha256s", []),
                ("gateHistory", []),
            ):
                item = copy.deepcopy(state); item["phases"][phase][field] = value
                reject(f"phase-{phase}-closed-{field}", item)

        # The global Gate table has one row axis and twelve columns. All columns
        # are equal-length, every identity column is unique, and P0-P4 each have
        # a statically required row even when every column is cleared together.
        columns = UPDATER.GATE_TABLE_COLUMNS
        identity_columns = (
            "dispatchIds", "messageIds", "receiptSha256s", "reportSha256s",
            "verdictSha256s", "orcaArtifactSha256s", "resultSha256s",
        )
        item = copy.deepcopy(state)
        for column in columns:
            item["gateHistory"][column] = []
        reject("gate-history-cleared-all-columns", item)
        for phase in ("P0", "P1", "P2", "P3", "P4"):
            item = copy.deepcopy(state)
            keep = [index for index, row in enumerate(item["gateHistory"]["rows"])
                    if row["phase"] != phase]
            for column in columns:
                item["gateHistory"][column] = [item["gateHistory"][column][index] for index in keep]
            reject(f"gate-history-required-{phase}-cleared", item)
        for column in columns:
            item = copy.deepcopy(state); item["gateHistory"][column].pop()
            reject(f"gate-history-unequal-{column}", item)
            item = copy.deepcopy(state); del item["gateHistory"][column]
            reject(f"gate-history-missing-{column}", item)
        for column in identity_columns:
            item = copy.deepcopy(state); item["gateHistory"][column][1] = item["gateHistory"][column][0]
            reject(f"gate-history-duplicate-{column}", item)
        item = copy.deepcopy(state); item["gateHistory"]["rows"][1] = copy.deepcopy(item["gateHistory"]["rows"][0])
        reject("gate-history-duplicate-row", item)
        item = copy.deepcopy(state); item["gateHistory"]["messageIds"][0] = [item["gateHistory"]["messageIds"][0], "msg_second"]
        reject("gate-history-two-messages-one-event", item)
        item = copy.deepcopy(state); item["gateHistory"]["reportSha256s"][0] = [item["gateHistory"]["reportSha256s"][0], "0" * 64]
        reject("gate-history-two-reports-one-event", item)
        p1_index = next(index for index, row in enumerate(state["gateHistory"]["rows"]) if row["phase"] == "P1")
        item = copy.deepcopy(state); item["gateHistory"]["rows"][p1_index]["phase"] = "P7"
        reject("gate-history-phase-authority", item)
        item = copy.deepcopy(state); item["gateHistory"]["rows"][p1_index]["gateVerdict"] = "GATE P7 ENGINEERING PASS"
        reject("gate-history-phase-verdict", item)
        item = copy.deepcopy(state); item["gateHistory"]["rows"][p1_index]["gateScope"] = "final-release"
        reject("gate-history-scope", item)
        item = copy.deepcopy(state); item["gateHistory"]["rows"][p1_index]["authorityClass"] = "materialized-gate"
        reject("gate-history-authority-class", item)
        for field in ("eventId", "supersedesEventId", "workerMessageId", "dispatchId", "receipt", "report", "verdictArtifact", "orcaResult"):
            item = copy.deepcopy(state); item["gateHistory"]["rows"][p1_index][field] = None
            reject(f"gate-history-closed-row-{field}", item)
        for field in ("order", "events", "messages", "dispatches", "receipts", "reports", "verdicts", "resultPayloads", "supersessions", "supersessionTargets"):
            item = copy.deepcopy(state); item["gateHistory"][field] = []
            reject(f"gate-history-closed-{field}", item)

        oversized = copy.deepcopy(state)
        while len(oversized["gateHistory"]["rows"]) <= UPDATER.MAX_LEDGER_ROWS:
            index = len(oversized["gateHistory"]["rows"])
            row = copy.deepcopy(oversized["gateHistory"]["rows"][0])
            row["gateTaskId"] = f"task_bound_{index}"
            row["completedAtUtc"] = f"2099-01-01T00:00:{index % 60:02d}.{index:03d}Z"
            oversized["gateHistory"]["rows"].append(row)
            for column in identity_columns:
                if column == "dispatchIds":
                    value = f"ctx_bound_{index}"
                elif column == "messageIds":
                    value = f"msg_bound_{index}"
                else:
                    value = digest(f"{column}:{index}".encode("ascii"))
                oversized["gateHistory"][column].append(value)
            for column in ("receiptMetadata", "reportMetadata", "verdictMetadata", "orcaResultMetadata"):
                oversized["gateHistory"][column].append(copy.deepcopy(oversized["gateHistory"][column][0]))
        reject("gate-history-over-100-row-cap", oversized)

        # Decision history is one bounded ordered row array. Transition IDs and
        # predecessor/successor/latest projections are deliberately absent.
        item = copy.deepcopy(state); item["decisionHistory"][0].update({"status": "pending", "outcome": "accepted"})
        reject("decision-transition-lifecycle", item)
        item = copy.deepcopy(state); item["decisionHistory"] = []
        reject("decision-empty-history", item)
        for gate_id in ("gate_ca804cbec712", "gate_b66203c11b10"):
            item = copy.deepcopy(state); item["decisionHistory"] = [row for row in item["decisionHistory"] if row["gateId"] != gate_id]
            reject(f"decision-required-{gate_id}-cleared", item)
        item = copy.deepcopy(state); item["decisionHistory"].append(copy.deepcopy(item["decisionHistory"][0]))
        reject("decision-duplicate-transition", item)
        for field in ("transitionId", "previousTransitionId", "predecessor", "successor", "latestTransitionId"):
            item = copy.deepcopy(state); item["decisionHistory"][0][field] = "0" * 64
            reject(f"decision-closed-{field}", item)
        for field in ("predecessors", "successors", "order", "transitions"):
            item = copy.deepcopy(state); item["decisionHistory"] = {"rows": copy.deepcopy(item["decisionHistory"]), field: {}}
            reject(f"decision-closed-{field}", item)
        oversized = copy.deepcopy(state)
        while len(oversized["decisionHistory"]) <= UPDATER.MAX_LEDGER_ROWS:
            index = len(oversized["decisionHistory"])
            row = copy.deepcopy(oversized["decisionHistory"][0])
            row.update({"gateId": f"gate_bound_{index}", "taskId": f"task_bound_{index}",
                        "question": f"bounded decision {index}",
                        "observedAtUtc": f"2099-01-01T00:00:{index % 60:02d}.{index:03d}Z"})
            oversized["decisionHistory"].append(row)
        reject("decision-history-over-100-row-cap", oversized)

        for key in ("f42", "r24", "metal", "releaseDistribution"):
            item = copy.deepcopy(state); item["policy"][key] = "PASS"
            reject(f"fixed-policy-{key}-promotion", item)

        for key in ("snapshot", "policy", "materialization", "publication"):
            item = copy.deepcopy(state); item[key]["unexpected"] = True
            reject(f"closed-{key}", item)
        self.assertGreaterEqual(len(invalid), 100)
        for name, value in invalid:
            with self.subTest(case=name):
                with self.assertRaises(VALIDATOR.JsonSchemaError):
                    validator.validate(value)
                if reference_validator is not None:
                    self.assertTrue(list(reference_validator.iter_errors(value)))

    def test_16_pointer_bundle_mirror_and_cross_hash_tampering(self) -> None:
        for case in ("pointer", "descriptor", "state", "mirror"):
            with self.subTest(case=case), fixture() as f:
                pointer_path = f.repo / POINTER
                pointer = read_json(pointer_path)
                bundle = f.repo / pointer["bundlePath"]
                if case == "pointer":
                    pointer["bundlePath"] = "../outside"
                    write_json(pointer_path, pointer)
                elif case == "descriptor":
                    descriptor = read_json(bundle / "bundle.json")
                    descriptor["stateSha256"] = "0" * 64
                    write_json(bundle / "bundle.json", descriptor)
                elif case == "state":
                    (bundle / "state.json").write_bytes((bundle / "state.json").read_bytes() + b" ")
                else:
                    (f.repo / LEDGER).write_bytes((f.repo / LEDGER).read_bytes() + b" ")
                self.assert_failure(f.validator("--check"))

    def test_17_protocol_vector_digest_and_semantic_mutation_fail(self) -> None:
        with fixture() as f:
            vectors = f.repo / STORE / "protocol-vectors.json"
            value = read_json(vectors)
            value["rejected"][0]["expected"] = "accepted"
            write_json(vectors, value)
            self.assert_failure(f.validator("--check"), "authority/vector")
        authority = read_json(ROOT / STORE / "authority.json")
        vectors = read_json(ROOT / STORE / "protocol-vectors.json")
        changed = copy.deepcopy(vectors)
        changed["rejected"][0]["expected"] = "accepted"
        with self.assertRaises(VALIDATOR.ValidationError):
            VALIDATOR.evaluate_vectors(authority, changed)

    def test_18_fault_matrix_preserves_authoritative_preimage(self) -> None:
        precommit_stages = (
            "after-lock",
            "bundle-state-write", "bundle-state-file-fsync", "bundle-state-replace", "bundle-state-parent-fsync", "bundle-first-write",
            "bundle-markdown-write", "bundle-markdown-file-fsync", "bundle-markdown-replace", "bundle-markdown-parent-fsync", "bundle-second-write",
            "bundle-snapshot-write", "bundle-snapshot-file-fsync", "bundle-snapshot-replace", "bundle-snapshot-parent-fsync",
            "bundle-descriptor-write", "bundle-descriptor-file-fsync", "bundle-descriptor-replace", "bundle-descriptor-parent-fsync",
            "bundle-directory-fsync", "bundle-directory-post-fsync", "bundle-directory-replace",
            "bundle-directory-post-replace", "bundle-parent-fsync",
            "journal-write", "journal-file-fsync", "journal-replace", "journal-parent-fsync", "after-journal",
            "state-write", "state-file-fsync", "state-replace", "state-parent-fsync", "post-first-write",
            "ledger-write", "ledger-file-fsync", "ledger-replace", "ledger-parent-fsync", "post-second-write",
            "final-validation", "validation-before", "validation-after",
            "pointer-write", "pointer-file-fsync", "pointer-replace",
        )
        for stage in precommit_stages:
            with self.subTest(stage=stage), fixture(False) as f:
                with mock.patch.dict(os.environ, {"MGIF_LEDGER_FAULT_STAGE": stage}, clear=False):
                    with self.assertRaises(UPDATER.LedgerError):
                        f.publish_seed()
                self.assertFalse((f.repo / POINTER).exists())
                self.assertFalse((f.repo / STATE).exists())
                self.assertFalse((f.repo / LEDGER).exists())
                self.assertFalse((f.repo / UPDATER.JOURNAL_REL).exists())
                self.assertFalse((f.repo / UPDATER.LOCK_REL).exists())

        postcommit_stages = (
            "pointer-replaced-before-bookkeeping", "pointer-post-replace", "pointer-parent-fsync",
            "post-commit-diagnostic", "post-pointer-write", "journal-cleanup", "journal-post-unlink",
            "journal-cleanup-parent-fsync", "lock-cleanup",
        )
        for stage in postcommit_stages:
            with self.subTest(stage=stage), fixture(False) as f:
                with mock.patch.dict(os.environ, {"MGIF_LEDGER_FAULT_STAGE": stage}, clear=False):
                    bundle_id = f.publish_seed()
                pointer = read_json(f.repo / POINTER)
                self.assertEqual(pointer["bundleId"], bundle_id)
                self.assert_success(f.validator("--check"))
                UPDATER.recover(f.repo)
                UPDATER.recover(f.repo)
                self.assertEqual(read_json(f.repo / POINTER)["bundleId"], bundle_id)
                self.assertFalse((f.repo / UPDATER.JOURNAL_REL).exists())
                self.assertFalse((f.repo / UPDATER.LOCK_REL).exists())

        # Actual validator failures straddle the same boundary: candidate
        # validation is pre-commit, while the diagnostic --check is post-commit.
        with fixture(False) as f:
            original_validator = UPDATER.validator

            def fail_candidate(repo: Path, arguments: list[str]) -> None:
                if "--candidate-full" in arguments:
                    raise UPDATER.LedgerError("candidate validator failed")
                original_validator(repo, arguments)

            with mock.patch.object(UPDATER, "validator", side_effect=fail_candidate):
                with self.assertRaisesRegex(UPDATER.LedgerError, "candidate validator failed"):
                    f.publish_seed()
            self.assertFalse((f.repo / POINTER).exists())

        with fixture(False) as f:
            original_validator = UPDATER.validator

            def fail_diagnostic(repo: Path, arguments: list[str]) -> None:
                if arguments[-1:] == ["--check"]:
                    raise UPDATER.LedgerError("post-commit validator failed")
                original_validator(repo, arguments)

            with mock.patch.object(UPDATER, "validator", side_effect=fail_diagnostic):
                bundle_id = f.publish_seed()
            self.assertEqual(read_json(f.repo / POINTER)["bundleId"], bundle_id)
            self.assertTrue((f.repo / UPDATER.JOURNAL_REL).exists())
            UPDATER.recover(f.repo)
            self.assert_success(f.validator("--check"))

        # If the OS replacement takes effect and only then reports an error,
        # the pointer hash proves that the sole commit point was crossed.
        with fixture(False) as f:
            pointer_path = f.repo / POINTER
            original_replace = os.replace
            injected = False

            def replace_then_raise(source, target) -> None:
                nonlocal injected
                original_replace(source, target)
                if not injected and Path(target) == pointer_path:
                    injected = True
                    raise OSError("replace completed before error")

            with mock.patch.object(UPDATER.os, "replace", side_effect=replace_then_raise):
                bundle_id = f.publish_seed()
            self.assertTrue(injected)
            self.assertEqual(read_json(pointer_path)["bundleId"], bundle_id)
            UPDATER.recover(f.repo)

        # Journal and lock cleanup errors after commit are best-effort and do
        # not turn a valid publication into a reported failure.
        with fixture(False) as f:
            journal = f.repo / UPDATER.JOURNAL_REL
            original_unlink = Path.unlink

            def deny_journal(path: Path, *args, **kwargs):
                if path == journal:
                    raise PermissionError("journal cleanup denied")
                return original_unlink(path, *args, **kwargs)

            with mock.patch.object(Path, "unlink", deny_journal):
                bundle_id = f.publish_seed()
            self.assertEqual(read_json(f.repo / POINTER)["bundleId"], bundle_id)
            self.assertTrue(journal.exists())
            UPDATER.recover(f.repo)

        with fixture(False) as f:
            lock = f.repo / UPDATER.LOCK_REL
            original_unlink = Path.unlink

            def deny_lock(path: Path, *args, **kwargs):
                if path == lock:
                    raise PermissionError("lock cleanup denied")
                return original_unlink(path, *args, **kwargs)

            with mock.patch.object(Path, "unlink", deny_lock):
                bundle_id = f.publish_seed()
            self.assertEqual(read_json(f.repo / POINTER)["bundleId"], bundle_id)
            self.assertTrue(lock.exists())
            original_unlink(lock)

        # Reproduce the reported real-OSError boundary: journal unlink has
        # succeeded, its parent fsync raises, and publish must still report the
        # already committed bundle as success.
        with fixture(False) as f:
            journal = f.repo / UPDATER.JOURNAL_REL
            original_fsync = UPDATER.fsync_dir
            injected = False

            def fail_after_journal_unlink(path: Path) -> None:
                nonlocal injected
                if not injected and path == journal.parent and not journal.exists() and (f.repo / POINTER).exists():
                    injected = True
                    raise OSError("post-journal-unlink fsync")
                original_fsync(path)

            with mock.patch.object(UPDATER, "fsync_dir", side_effect=fail_after_journal_unlink):
                bundle_id = f.publish_seed()
            self.assertTrue(injected)
            self.assertEqual(read_json(f.repo / POINTER)["bundleId"], bundle_id)
            self.assert_success(f.validator("--check"))

        # Process exit reporting agrees with the same commit boundary.
        driver = textwrap.dedent("""
            import importlib.util, pathlib, sys
            spec=importlib.util.spec_from_file_location("u",sys.argv[1]); m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
            repo,stage,seed=map(pathlib.Path,sys.argv[2:5])
            print(m.publish(repo,stage,(seed/"state.json").read_bytes(),(seed/"progress.md").read_bytes(),(seed/"snapshot.json").read_bytes(),{}))
        """)
        for stage, expected_code, pointer_expected in (("pointer-replace", 1, False), ("journal-post-unlink", 0, True)):
            with self.subTest(cli_stage=stage), fixture(False) as f:
                stage_dir = f.empty_stage(); seed = f.base / "seed"; seed.mkdir()
                (seed / "state.json").write_bytes(f.seed_state); (seed / "progress.md").write_bytes(f.seed_markdown); (seed / "snapshot.json").write_bytes(f.seed_snapshot)
                environment = os.environ.copy(); environment["MGIF_LEDGER_FAULT_STAGE"] = stage
                run = subprocess.run([sys.executable, "-B", "-c", driver, str(UPDATER_PATH), str(f.repo), str(stage_dir), str(seed)],
                                     env=environment, text=True, capture_output=True, check=False)
                self.assertEqual(run.returncode, expected_code, run.stderr or run.stdout)
                self.assertEqual((f.repo / POINTER).exists(), pointer_expected)

    def test_19_rollback_faults_leave_recoverable_journal(self) -> None:
        cases = (
            ("post-first-write,rollback-state", "rollback-state"),
            ("post-second-write,rollback-ledger", "rollback-ledger"),
            ("pointer-replace,rollback-state", "pointer-boundary-rollback-state"),
        )
        for stages, expected in cases:
            with self.subTest(stage=expected), fixture(False) as f:
                with mock.patch.dict(os.environ, {"MGIF_LEDGER_FAULT_STAGE": stages}, clear=False):
                    with self.assertRaisesRegex(UPDATER.LedgerError, "needs recovery"):
                        f.publish_seed()
                self.assertTrue((f.repo / UPDATER.JOURNAL_REL).exists())
                with mock.patch.dict(os.environ, {"MGIF_LEDGER_FAULT_STAGE": ""}, clear=False):
                    UPDATER.recover(f.repo)
                self.assertFalse((f.repo / POINTER).exists())
                self.assertFalse((f.repo / STATE).exists())
                self.assertFalse((f.repo / LEDGER).exists())

        # A valid committed pointer remains authoritative while stale-journal
        # cleanup is retried. Unlink failure blocks a new transaction; a
        # directory-fsync failure after successful unlink does not.
        with fixture(False) as f:
            with mock.patch.dict(os.environ, {"MGIF_LEDGER_FAULT_STAGE": "journal-cleanup"}, clear=False):
                committed_id = f.publish_seed()
            journal = f.repo / UPDATER.JOURNAL_REL
            pointer_before = (f.repo / POINTER).read_bytes()
            self.assertTrue(journal.exists())
            original_unlink = Path.unlink

            def deny_journal_unlink(path: Path, *args, **kwargs):
                if path == journal:
                    raise PermissionError("journal unlink denied")
                return original_unlink(path, *args, **kwargs)

            with mock.patch.object(Path, "unlink", deny_journal_unlink):
                with self.assertRaises(PermissionError):
                    f.publish_seed()
            self.assertEqual((f.repo / POINTER).read_bytes(), pointer_before)
            self.assertEqual(read_json(f.repo / POINTER)["bundleId"], committed_id)
            self.assertTrue(journal.exists())
            UPDATER.recover(f.repo); UPDATER.recover(f.repo)
            self.assertFalse(journal.exists())

        with fixture(False) as f:
            with mock.patch.dict(os.environ, {"MGIF_LEDGER_FAULT_STAGE": "journal-cleanup"}, clear=False):
                committed_id = f.publish_seed()
            journal = f.repo / UPDATER.JOURNAL_REL
            original_fsync = UPDATER.fsync_dir
            injected = False

            def fail_removed_journal_fsync(path: Path) -> None:
                nonlocal injected
                if not injected and path == journal.parent and not journal.exists():
                    injected = True
                    raise OSError("journal directory fsync failed")
                original_fsync(path)

            with mock.patch.object(UPDATER, "fsync_dir", side_effect=fail_removed_journal_fsync):
                UPDATER.recover(f.repo)
            self.assertTrue(injected)
            self.assertFalse(journal.exists())
            self.assertEqual(read_json(f.repo / POINTER)["bundleId"], committed_id)
            UPDATER.recover(f.repo)

        # A surviving committed journal never blesses corrupt mirrors.
        with fixture(False) as f:
            with mock.patch.dict(os.environ, {"MGIF_LEDGER_FAULT_STAGE": "journal-cleanup"}, clear=False):
                committed_id = f.publish_seed()
            journal = f.repo / UPDATER.JOURNAL_REL
            (f.repo / STATE).unlink()
            with self.assertRaisesRegex(UPDATER.LedgerError, "mirrors differ"):
                UPDATER.recover(f.repo)
            self.assertTrue(journal.exists())
            (f.repo / STATE).write_bytes(f.seed_state)
            UPDATER.recover(f.repo)
            self.assertEqual(read_json(f.repo / POINTER)["bundleId"], committed_id)

    def test_20_lock_active_stale_ownership_and_cleanup(self) -> None:
        with fixture(False) as f:
            lock = f.repo / UPDATER.LOCK_REL
            write_json(lock, {"createdAtUtc": UPDATER.now(), "pid": os.getpid(), "token": "active"})
            with self.assertRaisesRegex(UPDATER.LedgerError, "active"):
                UPDATER.acquire_lock(f.repo)
        with fixture(False) as f:
            lock = f.repo / UPDATER.LOCK_REL
            write_json(lock, {"createdAtUtc": UPDATER.now(), "pid": 2147483647, "token": "dead"})
            path, token = UPDATER.acquire_lock(f.repo)
            UPDATER.release_lock(path, token)
            self.assertFalse(lock.exists())
        with fixture(False) as f:
            path, token = UPDATER.acquire_lock(f.repo)
            write_json(path, {"createdAtUtc": UPDATER.now(), "pid": os.getpid(), "token": "other"})
            with self.assertRaisesRegex(UPDATER.LedgerError, "ownership"):
                UPDATER.release_lock(path, token)
        with fixture(False) as f:
            with mock.patch.dict(os.environ, {"MGIF_LEDGER_FAULT_STAGE": "lock-cleanup"}, clear=False):
                f.publish_seed()
            self.assertTrue((f.repo / POINTER).exists())
            self.assertFalse((f.repo / UPDATER.LOCK_REL).exists())

    def test_21_source_and_output_preimage_drift_are_not_overwritten(self) -> None:
        with fixture(False) as f:
            source = f.base / "source.txt"
            source.write_text("old", encoding="ascii")
            old_hash = digest(source.read_bytes())
            original = UPDATER.install_evidence
            def drift(stage, repo):
                source.write_text("external", encoding="ascii")
                original(stage, repo)
            with mock.patch.object(UPDATER, "install_evidence", side_effect=drift):
                with self.assertRaisesRegex(UPDATER.LedgerError, "source drift"):
                    UPDATER.publish(f.repo, f.empty_stage(), f.seed_state, f.seed_markdown, f.seed_snapshot, {source: old_hash})
            self.assertFalse((f.repo / POINTER).exists())
        for target_name, label in (("state", "state mirror"), ("ledger", "Markdown mirror"), ("pointer", "authority pointer")):
            with self.subTest(target=target_name), fixture(False) as f:
                paths = {"state": f.repo / STATE, "ledger": f.repo / LEDGER, "pointer": f.repo / POINTER}
                original_verify = UPDATER.verify_preimage
                def drift_output(path, value, label_text):
                    if label_text == label:
                        path.parent.mkdir(parents=True, exist_ok=True)
                        path.write_bytes(b"external")
                    return original_verify(path, value, label_text)
                with mock.patch.object(UPDATER, "verify_preimage", side_effect=drift_output):
                    with self.assertRaises(UPDATER.LedgerError):
                        f.publish_seed()
                self.assertEqual(paths[target_name].read_bytes(), b"external")
                self.assertTrue((f.repo / UPDATER.JOURNAL_REL).exists())

    def test_22_crash_boundaries_recover_on_next_publication(self) -> None:
        precommit_stages = (
            "after-lock", "bundle-state-write", "bundle-state-file-fsync", "bundle-state-replace",
            "bundle-directory-post-replace", "journal-write", "journal-file-fsync", "journal-replace",
            "journal-parent-fsync", "after-journal", "state-write", "state-file-fsync", "state-replace",
            "state-parent-fsync", "ledger-write", "ledger-file-fsync", "ledger-replace", "ledger-parent-fsync",
            "validation-before", "validation-after", "pointer-write", "pointer-file-fsync", "pointer-replace",
        )
        postcommit_stages = (
            "pointer-replaced-before-bookkeeping", "pointer-post-replace", "pointer-parent-fsync",
            "post-commit-diagnostic", "post-pointer-write", "journal-cleanup", "journal-post-unlink",
            "journal-cleanup-parent-fsync", "lock-cleanup",
        )
        driver = textwrap.dedent("""
            import importlib.util, pathlib, sys
            spec=importlib.util.spec_from_file_location("u",sys.argv[1]); m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
            repo,stage,seed=map(pathlib.Path,sys.argv[2:5])
            m.publish(repo,stage,(seed/"state.json").read_bytes(),(seed/"progress.md").read_bytes(),(seed/"snapshot.json").read_bytes(),{})
        """)
        for stage in (*precommit_stages, *postcommit_stages):
            with self.subTest(stage=stage), fixture(False) as f:
                stage_dir = f.empty_stage()
                seed = f.base / "seed"
                seed.mkdir()
                (seed / "state.json").write_bytes(f.seed_state)
                (seed / "progress.md").write_bytes(f.seed_markdown)
                (seed / "snapshot.json").write_bytes(f.seed_snapshot)
                environment = os.environ.copy()
                environment["MGIF_LEDGER_CRASH_STAGE"] = stage
                crashed = subprocess.run(
                    [sys.executable, "-B", "-c", driver, str(UPDATER_PATH), str(f.repo), str(stage_dir), str(seed)],
                    env=environment, text=True, capture_output=True, check=False,
                )
                self.assertEqual(crashed.returncode, 97, crashed.stderr)
                self.assertEqual((f.repo / POINTER).exists(), stage in postcommit_stages)
                if stage in postcommit_stages:
                    self.assert_success(f.validator("--check"))
                with mock.patch.dict(os.environ, {"MGIF_LEDGER_CRASH_STAGE": ""}, clear=False):
                    f.publish_seed(stage_dir)
                self.assertTrue((f.repo / POINTER).is_file())
                self.assertFalse((f.repo / UPDATER.JOURNAL_REL).exists())
                self.assertFalse((f.repo / UPDATER.LOCK_REL).exists())
                bundle_parent = f.repo / UPDATER.BUNDLES_REL
                self.assertFalse(any(p.name.startswith(".") and p.name.endswith(".tmp") for p in bundle_parent.iterdir()))
                for parent, target in ((f.repo / POINTER.parent, POINTER.name),
                                       (f.repo / STATE.parent, STATE.name),
                                       (f.repo / LEDGER.parent, LEDGER.name),
                                       ((f.repo / UPDATER.JOURNAL_REL).parent, UPDATER.JOURNAL_REL.name)):
                    self.assertFalse(any(p.name.startswith(f".{target}.") and p.name.endswith(".tmp") for p in parent.iterdir()))

    def test_23_final_release_and_fixed_policy_cannot_be_promoted(self) -> None:
        with fixture() as f:
            snapshot = f.snapshot()
            receipt, _ = f.make_receipt(snapshot, phase="P10", suffix="release", verdict="GATE P10 FINAL RELEASE PASS")
            self.assert_failure(f.candidate(snapshot, receipt), "release PASS")
        state = read_state(ROOT / STATE)
        self.assertEqual(state["policy"]["releaseDistribution"], "NOT PASS")
        self.assertNotEqual(state["policy"]["releaseDistribution"], "PASS")
        self.assertEqual(state["policy"]["f42"], "BLOCKED")
        self.assertEqual(state["policy"]["r24"], "BLOCKED")
        self.assertEqual(state["policy"]["metal"], "Not Verified")

    def test_24_dependency_free_scripts_importlib_and_unsupported_keywords(self) -> None:
        with fixture() as f:
            probe = f.run_without_site(
                "-c",
                "import importlib.util; raise SystemExit(importlib.util.find_spec('jsonschema') is not None)",
            )
            self.assertEqual(probe.returncode, 0, probe.stderr or probe.stdout)

            direct_validator = f.run_without_site(
                f.repo / "tools/validate_editor_progress_ledger.py",
                "--repo-root", str(f.repo), "--check",
            )
            direct_updater = f.run_without_site(
                f.repo / "tools/update_editor_progress_ledger.py",
                "--repo-root", str(f.repo), "--check",
            )
            self.assert_success(direct_validator)
            self.assert_success(direct_updater)

            import_driver = textwrap.dedent("""
                import importlib.util, pathlib, sys
                for index, raw in enumerate(sys.argv[1:]):
                    path = pathlib.Path(raw)
                    spec = importlib.util.spec_from_file_location(f"ledger_import_{index}", path)
                    module = importlib.util.module_from_spec(spec)
                    spec.loader.exec_module(module)
                print("importlib dependency-free load PASS")
            """)
            environment = os.environ.copy()
            environment.pop("PYTHONPATH", None)
            environment["PYTHONNOUSERSITE"] = "1"
            imported = subprocess.run(
                [sys.executable, "-S", "-B", "-c", import_driver,
                 str(f.repo / "tools/validate_editor_progress_ledger.py"),
                 str(f.repo / "tools/update_editor_progress_ledger.py")],
                cwd=f.repo, env=environment, text=True, encoding="utf-8",
                capture_output=True, check=False,
            )
            self.assert_success(imported)

            unsupported = read_json(f.repo / "docs/editor-parity/schemas/progress-ledger.schema.json")
            unsupported["unevaluatedProperties"] = False
            write_json(f.repo / "docs/editor-parity/schemas/progress-ledger.schema.json", unsupported)
            failure_driver = textwrap.dedent("""
                import importlib.util, pathlib, sys
                path = pathlib.Path(sys.argv[1])
                spec = importlib.util.spec_from_file_location("ledger_unsupported", path)
                module = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(module)
                error_type = module.ValidationError if hasattr(module, "ValidationError") else module.LedgerError
                try:
                    module.raw_schema_validate({}, "unsupported-keyword probe")
                except error_type as exc:
                    print(exc)
                    raise SystemExit(0)
                raise SystemExit("unsupported keyword was accepted")
            """)
            for script in (
                f.repo / "tools/validate_editor_progress_ledger.py",
                f.repo / "tools/update_editor_progress_ledger.py",
            ):
                with self.subTest(unsupported_script=script.name):
                    failed_closed = subprocess.run(
                        [sys.executable, "-S", "-B", "-c", failure_driver, str(script)],
                        cwd=f.repo, env=environment, text=True, encoding="utf-8",
                        capture_output=True, check=False,
                    )
                    self.assertEqual(failed_closed.returncode, 0, failed_closed.stderr or failed_closed.stdout)
                    self.assertIn("unsupported schema keywords", failed_closed.stdout)
                    self.assertIn("unevaluatedProperties", failed_closed.stdout)


if __name__ == "__main__":
    unittest.main()
