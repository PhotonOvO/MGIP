#!/usr/bin/env python3
"""Build the ordinary schema-2 receipt for the independent failed G8 audit."""

from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path


def load_publisher(repo: Path):
    path = repo / "tools/local_editor_progress_ledger.py"
    spec = importlib.util.spec_from_file_location("g8_fail_publisher", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def record(path: Path, publisher) -> dict[str, object]:
    data = path.read_bytes()
    return {
        "path": str(path).replace("/", "\\"),
        "bytes": len(data),
        "sha256": publisher.digest(data),
    }


def write_json(path: Path, value: object, publisher) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(publisher.document(value))


def main() -> None:
    repo = Path("G:/MGIF") if os.name == "nt" else Path("/mnt/g/mgif")
    root = (
        Path("G:/Temp/mgif-p8-final-20260905T081711Z")
        if os.name == "nt"
        else Path("/mnt/g/Temp/mgif-p8-final-20260905T081711Z")
    )
    parser = __import__("argparse").ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=repo)
    parser.add_argument("--evidence-root", type=Path, default=root)
    args = parser.parse_args()
    repo = args.repo_root.resolve()
    root = args.evidence_root.resolve()
    publisher = load_publisher(repo)
    origin = publisher.legacy_origin(repo)
    parent = publisher.load_local(repo, origin)
    if parent is None:
        raise RuntimeError("G8 audit requires the selected local parent")
    expected_parent = {
        "bundleId": "66d52bd171123743eea3a04e16823a1101841d7ccb31d52471476ad86a23f37c",
        "snapshotSha256": "1a350670d0500526a216ad9f549dbf4fafabdde70d705996c7dce2d69c1a5d48",
        "transitionHeadSha256": "5f5ec880a009f2546768b57146e95635cf1c01b53cb33d983b4acef181b41cee",
    }
    actual_parent = {
        "bundleId": parent["bundleId"],
        "snapshotSha256": parent["pointer"]["snapshotSha256"],
        "transitionHeadSha256": parent["snapshot"]["transitionHeadSha256"],
    }
    if actual_parent != expected_parent:
        raise RuntimeError(f"selected local parent drifted: {actual_parent}")

    task_id = "task_eb85190ed1ac"
    task = next(row for row in parent["tasks"] if row["taskId"] == task_id)
    if task["status"] != "ready":
        raise RuntimeError(f"G8 is not ready for review: {task['status']}")
    before_task_sha = publisher.task_hash(task)
    aggregate_path = root / "visual-v2-evidence-manifest.json"
    recovery_report = root / "p8-90-r1-ledger-draft/P8-90-R1-REPORT.md"
    draft_dir = root / "g8-ledger-draft"
    report_lines = [
        "# G8 Independent Gate P8 Report",
        "",
        "Decision: GATE P8 FAIL/BLOCKED.",
        "",
        "The independent review was read-only. It did not modify source or ledger data, run a Linux build, or invoke Orca.",
        "",
        f"Parent bundle: {parent['bundleId']}",
        f"Parent snapshot: {parent['pointer']['snapshotSha256']}",
        f"Parent transition head: {parent['snapshot']['transitionHeadSha256']}",
        f"P8 aggregate: {str(aggregate_path).replace('/', chr(92))}; sha256={publisher.file_digest(aggregate_path)}",
        f"P8-90-R1 report: {str(recovery_report).replace('/', chr(92))}; sha256={publisher.file_digest(recovery_report)}",
        "",
        "## Blocking Findings",
        "",
        "1. U01-U08 are placeholder capture/marker/capture scripts and contain no real hierarchy, Gizmo, material, camera, merge, compact, or save actions.",
        "2. The aggregate is a P7 V01/V02/V03/V04/V07 artifact, not a P8 UI Gate matrix. It captures only ReadyClean/GizmoHover/GizmoDragging and has no context-menu manifest.",
        "3. Published P8-10/P8-11/P8-12 reports explicitly retain non-claims for their locked shell, Browser, Inspector, dialog, and complete action ownership.",
        "4. MinimalLatestApp still contains the desktop-linked legacy GltfModel/SceneAsset load and destructive replacement path, including editableSceneLights and index-based transaction state.",
        "5. The 81-file identity omits the acceptance driver/tests/CMake/validator; focused logs omit ui_state/ui_action and configured N11/boundary raw artifacts.",
        "6. Release unfiltered runtime evidence contains the documented VUID-vkCmdDraw-None-09600 records; filtered reruns cannot be represented as a clean unfiltered Gate.",
        "",
        "## Positive Evidence Retained",
        "",
        "Windows/MSVC Debug and Release builds succeed, configured dependency preflight passes with the skip disabled, and the production Demo does not link the test driver.",
        "Controller lifecycle producers expose exactly nine states, candidate failures preserve the active document, and focused state/action tests pass.",
        "These are necessary recovery inputs but do not satisfy the full Gate P8 contract.",
        "",
        "## Required Recovery",
        "",
        "Provide real controller-bound U01-U08 request/result traces and a P8 source/executable identity; close the exact command/context/toolbar/lifecycle/DPI matrix; publish affirmative P8 leaf evidence; isolate the legacy viewer path from the desktop editor target; and retain raw Windows preflight/boundary/build/test logs.",
        "",
        "F42 and R24 remain BLOCKED; Metal remains Not Verified; release/distribution remains NOT PASS. P9 and P10 are not authorized by this failed Gate.",
        "",
    ]
    report_path = draft_dir / "G8-REPORT.md"
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text("\n".join(report_lines), encoding="ascii", newline="\n")
    report_record = record(report_path, publisher)

    verdict = {
        "documentKind": "local-task-verdict",
        "schemaVersion": 2,
        "ledgerId": publisher.LEDGER_ID,
        "parentBundleId": parent["bundleId"],
        "parentSnapshotSha256": parent["pointer"]["snapshotSha256"],
        "taskId": task_id,
        "displayName": "G8",
        "beforeTaskSha256": before_task_sha,
        "decision": "failed",
        "outcome": "failed",
        "reportSha256": report_record["sha256"],
        "fixedPolicy": dict(publisher.POLICY),
        "reviewAuthority": "local-automatic-review",
        "reviewExecutionOutcome": "succeeded",
    }
    verdict_path = draft_dir / "G8-verdict.json"
    write_json(verdict_path, verdict, publisher)
    verdict_record = record(verdict_path, publisher)
    receipt = {
        "documentKind": "local-task-transition-receipt",
        "schemaVersion": 2,
        "ledgerId": publisher.LEDGER_ID,
        "parentBundleId": parent["bundleId"],
        "parentSnapshotSha256": parent["pointer"]["snapshotSha256"],
        "previousTransitionSha256": parent["snapshot"]["transitionHeadSha256"],
        "taskId": task_id,
        "displayName": "G8",
        "beforeTaskSha256": before_task_sha,
        "fromStatus": "ready",
        "toStatus": "failed",
        "outcome": "failed",
        "completedAtUtc": "2026-09-05T08:50:00.000Z",
        "reviewAuthority": "local-automatic-review",
        "reviewExecutionOutcome": "succeeded",
        "report": report_record,
        "verdictArtifact": verdict_record,
        "fixedPolicy": dict(publisher.POLICY),
    }
    receipt_path = draft_dir / "G8-receipt-draft.json"
    write_json(receipt_path, receipt, publisher)
    for path in (report_path, verdict_path, receipt_path):
        item = record(path, publisher)
        print(f"{path.name} bytes={item['bytes']} sha256={item['sha256']}")
    print(f"beforeTaskSha256={before_task_sha}")


if __name__ == "__main__":
    main()
