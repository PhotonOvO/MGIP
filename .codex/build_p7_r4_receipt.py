#!/usr/bin/env python3
"""Build the sealed local receipt for the P7 R4 visual integration leaf."""

from __future__ import annotations

import copy
import importlib.util
import os
from pathlib import Path


if os.name == "nt":
    REPO = Path("G:/MGIF")
    ROOT = Path("G:/Temp/mgif-p7-visual-r4-20260904T223000Z")
else:
    REPO = Path("/mnt/g/mgif")
    ROOT = Path("/mnt/g/Temp/mgif-p7-visual-r4-20260904T223000Z")
PUBLISHER_PATH = REPO / "tools" / "local_editor_progress_ledger.py"
REPORT_PATH = ROOT / "P7-VISUAL-MATRIX-R4-REPORT.md"


def load_publisher():
    spec = importlib.util.spec_from_file_location("p7_r4_publisher", PUBLISHER_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load local ledger publisher")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def win_path(path: Path) -> str:
    value = str(path).replace("/", "\\")
    if value.startswith("\\mnt\\g\\"):
        return "G:\\" + value[len("\\mnt\\g\\") :]
    return value


def record(path: Path, publisher) -> dict[str, object]:
    data = path.read_bytes()
    return {"path": win_path(path), "bytes": len(data), "sha256": publisher.digest(data)}


def write(path: Path, value: object, publisher) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(publisher.document(value))


def main() -> None:
    publisher = load_publisher()
    origin = publisher.legacy_origin(REPO)
    parent = publisher.load_local(REPO, origin)
    if parent is None:
        raise RuntimeError("P7 R4 requires the selected local parent")
    expected = {
        "bundleId": "6563cc92f2053e5cf290c90006858f9e946e63037ed7395b08eae3f7603723e2",
        "snapshotSha256": "1ed5d632a5a44a9b7c4aee02f6034e108ba7c78b536558b26659fedd882286b4",
        "transitionHeadSha256": "c437e6ca655f21867de9de8840f0c4805f13c84e712172efb04d67119d908779",
    }
    actual = {
        "bundleId": parent["bundleId"],
        "snapshotSha256": parent["pointer"]["snapshotSha256"],
        "transitionHeadSha256": parent["snapshot"]["transitionHeadSha256"],
    }
    if actual != expected:
        raise RuntimeError(f"selected local parent drifted: {actual}")

    definition = {
        "taskId": "task_p7_rhi_visual_r4_20260904",
        "displayName": "P7-RHI-VISUAL-R4",
        "dependencies": ["task_p7_rhi_visual_r3_20260904"],
        "createdAtUtc": "2026-09-04T13:52:00.000Z",
        "phase": "P7",
        "role": "integration",
        "supersedesTaskId": "task_p7_rhi_visual_r3_20260904",
    }
    task = publisher.recovery_task_view(definition)
    task["status"] = "ready"
    before_task_sha = publisher.task_hash(task)
    definition_sha = publisher.digest(publisher.payload(definition))

    authority = {
        "documentKind": "local-recovery-task-authority",
        "schemaVersion": 1,
        "ledgerId": publisher.LEDGER_ID,
        "parentBundleId": parent["bundleId"],
        "parentSnapshotSha256": parent["pointer"]["snapshotSha256"],
        "taskDefinitionSha256": definition_sha,
        "authorizedAtUtc": definition["createdAtUtc"],
        "fixedPolicy": copy.deepcopy(publisher.POLICY),
    }
    draft_dir = ROOT / "ledger-draft"
    authority_path = draft_dir / "P7-RHI-VISUAL-R4-authority.json"
    write(authority_path, authority, publisher)
    authority_record = record(authority_path, publisher)

    report_record = record(REPORT_PATH, publisher)
    replacements: list[dict[str, object]] = []
    gate_event = None
    verdict = {
        "documentKind": "local-task-verdict",
        "schemaVersion": 2,
        "ledgerId": publisher.LEDGER_ID,
        "parentBundleId": parent["bundleId"],
        "parentSnapshotSha256": parent["pointer"]["snapshotSha256"],
        "taskId": definition["taskId"],
        "displayName": definition["displayName"],
        "beforeTaskSha256": before_task_sha,
        "decision": "completed",
        "outcome": "succeeded",
        "reportSha256": report_record["sha256"],
        "fixedPolicy": copy.deepcopy(publisher.POLICY),
        "reviewAuthority": "local-automatic-review",
        "reviewExecutionOutcome": "succeeded",
        "taskDefinitionSha256": definition_sha,
        "authoritySha256": authority_record["sha256"],
        "gateEvent": gate_event,
        "dependencyReplacementsSha256": publisher.digest(publisher.payload(replacements)),
    }
    verdict_path = draft_dir / "P7-RHI-VISUAL-R4-verdict.json"
    write(verdict_path, verdict, publisher)
    verdict_record = record(verdict_path, publisher)

    receipt = {
        "documentKind": "local-task-transition-receipt",
        "schemaVersion": 2,
        "ledgerId": publisher.LEDGER_ID,
        "parentBundleId": parent["bundleId"],
        "parentSnapshotSha256": parent["pointer"]["snapshotSha256"],
        "previousTransitionSha256": parent["snapshot"]["transitionHeadSha256"],
        "taskId": definition["taskId"],
        "displayName": definition["displayName"],
        "beforeTaskSha256": before_task_sha,
        "fromStatus": "ready",
        "toStatus": "completed",
        "outcome": "succeeded",
        "completedAtUtc": "2026-09-04T13:53:00.000Z",
        "reviewAuthority": "local-automatic-review",
        "reviewExecutionOutcome": "succeeded",
        "report": report_record,
        "verdictArtifact": verdict_record,
        "fixedPolicy": copy.deepcopy(publisher.POLICY),
        "taskDefinition": definition,
        "authorityArtifact": authority_record,
        "gateEvent": gate_event,
        "dependencyReplacements": replacements,
    }
    receipt_path = draft_dir / "P7-RHI-VISUAL-R4-receipt-draft.json"
    write(receipt_path, receipt, publisher)

    for path in (REPORT_PATH, authority_path, verdict_path, receipt_path):
        item = record(path, publisher)
        print(f"{path.name} bytes={item['bytes']} sha256={item['sha256']}")
    print(f"beforeTaskSha256={before_task_sha}")
    print(f"taskDefinitionSha256={definition_sha}")


if __name__ == "__main__":
    main()
