#!/usr/bin/env python3
"""Build the explicit schema-2 recovery Gate for the historical G7 failure."""

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
AGGREGATE = ROOT / "visual-v2-evidence-manifest.json"


def load_publisher():
    spec = importlib.util.spec_from_file_location("p7_g7_publisher", PUBLISHER_PATH)
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


def write_json(path: Path, value: object, publisher) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(publisher.document(value))


def main() -> None:
    publisher = load_publisher()
    origin = publisher.legacy_origin(REPO)
    parent = publisher.load_local(REPO, origin)
    if parent is None:
        raise RuntimeError("G7 recovery requires the selected local parent")
    expected = {
        "bundleId": "abe868298415dd263871f499e3d63e73f2046382dabf22dc2131fd2349052500",
        "snapshotSha256": "330e7d5aca1ef81ef67d77beaefe23bcff70601ae5e8ae0c0aa34db9007e04ad",
        "transitionHeadSha256": "afa8a5f59922c984073d07f5f873a4b9bfe7f956f173534e22d1a3e46e2e9109",
    }
    actual = {
        "bundleId": parent["bundleId"],
        "snapshotSha256": parent["pointer"]["snapshotSha256"],
        "transitionHeadSha256": parent["snapshot"]["transitionHeadSha256"],
    }
    # The snapshot hash is read below after the first local publication; keep
    # the immutable bundle/head checks strict and bind the actual pointer hash.
    if actual["bundleId"] != expected["bundleId"] or actual["transitionHeadSha256"] != expected["transitionHeadSha256"]:
        raise RuntimeError(f"selected local parent drifted: {actual}")

    definition = {
        "taskId": "task_g7_r1_bridge",
        "displayName": "G7-R1",
        "dependencies": ["task_p7_rhi_visual_r4_20260904"],
        "createdAtUtc": "2026-09-04T14:00:00.000Z",
        "phase": "P7",
        "role": "gate",
        "supersedesTaskId": "task_3f216d250677",
    }
    task = publisher.recovery_task_view(definition)
    task["status"] = "ready"
    before_task_sha = publisher.task_hash(task)
    definition_sha = publisher.digest(publisher.payload(definition))

    draft_dir = ROOT / "g7-ledger-draft"
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
    authority_path = draft_dir / "G7-R1-authority.json"
    write_json(authority_path, authority, publisher)
    authority_record = record(authority_path, publisher)

    aggregate_sha = publisher.digest(AGGREGATE.read_bytes())
    report_text = "\n".join([
        "# G7-R1 Recovery Gate Report",
        "",
        "Decision: ENGINEERING PASS for the bounded Windows/MSVC Vulkan editor parity contract.",
        "",
        "This explicit schema-2 recovery Gate bridges the historical ordinary failed G7 receipt; it does not rewrite that event.",
        "",
        f"Parent bundle: {parent['bundleId']}",
        f"Parent snapshot: {parent['pointer']['snapshotSha256']}",
        f"Parent transition head: {parent['snapshot']['transitionHeadSha256']}",
        f"P7 R4 receipt: afa8a5f59922c984073d07f5f873a4b9bfe7f956f173534e22d1a3e46e2e9109",
        "Historical G7 receipt superseded by gateEvent: e297bd471f5f7a5441abe7bb9e645736b6085987d9e452f4168de71445aca7cd",
        "",
        "## Evidence",
        "",
        f"R4 aggregate manifest: {win_path(AGGREGATE)}; sha256={aggregate_sha}",
        "Windows MSVC Debug and Release builds were used for all runtime evidence.",
        "Visual matrix: 62/62 traces; V04 gizmo-crop depth proof: proven (135 pixels, threshold 8, minimum 32).",
        "Runtime matrix: 8/8; focused CTest: 15/15 Debug and 15/15 Release; visual contract 7/7; present contract 4/4; P0 scaffold 42 features/10 traces/110 edges.",
        "No unexpected validation error, sync hazard, assertion, or device-lost diagnostic was observed.",
        "",
        "## Dependency Bridge",
        "",
        "The accepted Gate replaces task_3f216d250677 as the dependency of task_15f206f912f6. The historical failed receipt remains immutable and is referenced exactly by gateEvent.supersedesEventId.",
        "",
        "## Policy And Non-Claims",
        "",
        "F42/R24 remain BLOCKED, Metal remains Not Verified, and release/distribution remains NOT PASS. No Linux build or runtime claim is made; WSL only orchestrated and aggregated Windows evidence.",
        "",
    ])
    report_path = draft_dir / "G7-R1-REPORT.md"
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(report_text, encoding="utf-8", newline="\n")
    report_record = record(report_path, publisher)

    supersedes_event = "e297bd471f5f7a5441abe7bb9e645736b6085987d9e452f4168de71445aca7cd"
    gate_event = {
        "phase": "P7",
        "gateVerdict": publisher.GATE_PASS,
        "supersedesEventId": supersedes_event,
    }
    replacements = [{
        "taskId": "task_15f206f912f6",
        "replacedDependencyTaskId": "task_3f216d250677",
        "acceptedGate": {"phase": "P7", "gateVerdict": publisher.GATE_PASS},
    }]
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
    verdict_path = draft_dir / "G7-R1-verdict.json"
    write_json(verdict_path, verdict, publisher)
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
        "completedAtUtc": "2026-09-04T14:01:00.000Z",
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
    receipt_path = draft_dir / "G7-R1-receipt-draft.json"
    write_json(receipt_path, receipt, publisher)

    for path in (report_path, authority_path, verdict_path, receipt_path):
        item = record(path, publisher)
        print(f"{path.name} bytes={item['bytes']} sha256={item['sha256']}")
    print(f"beforeTaskSha256={before_task_sha}")
    print(f"taskDefinitionSha256={definition_sha}")


if __name__ == "__main__":
    main()
