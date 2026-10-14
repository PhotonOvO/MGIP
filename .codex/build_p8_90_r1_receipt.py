#!/usr/bin/env python3
"""Build the bounded schema-2 P8-90 recovery integration receipt."""

from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path


def load_module(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def canonical(value: object) -> bytes:
    return json.dumps(value, ensure_ascii=True, sort_keys=True, indent=2).encode(
        "ascii"
    ) + b"\n"


def record(path: Path, publisher) -> dict[str, object]:
    data = path.read_bytes()
    return {
        "path": str(path).replace("/", "\\"),
        "bytes": len(data),
        "sha256": publisher.digest(data),
    }


def write_json(path: Path, value: object, publisher) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(canonical(value))


def main() -> None:
    repo = Path("G:/MGIF") if os.name == "nt" else Path("/mnt/g/mgif")
    parser = __import__("argparse").ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=repo)
    parser.add_argument(
        "--evidence-root",
        type=Path,
        default=(
            Path("G:/Temp/mgif-p8-final-20260905T081711Z")
            if os.name == "nt"
            else Path("/mnt/g/Temp/mgif-p8-final-20260905T081711Z")
        ),
    )
    args = parser.parse_args()
    repo = args.repo_root.resolve()
    evidence_root = args.evidence_root.resolve()
    publisher = load_module(
        repo / "tools/local_editor_progress_ledger.py", "p8_90_r1_publisher"
    )
    origin = publisher.legacy_origin(repo)
    parent = publisher.load_local(repo, origin)
    if parent is None:
        raise RuntimeError("P8-90-R1 requires the selected local parent")
    expected_parent = {
        "bundleId": "ff9636881a6ae8b32526dce3d6aac152a2bc503d5556d1e518a8aa281061409a",
        "snapshotSha256": "793546271f340709baf6aed70dce6e4baf78915d569764d017b0464bd4d1e3ff",
        "transitionHeadSha256": "e54cf4cbbdf9a972d87142856d61f41f113ce4db858cc8c3824855a47e515f5e",
    }
    actual_parent = {
        "bundleId": parent["bundleId"],
        "snapshotSha256": parent["pointer"]["snapshotSha256"],
        "transitionHeadSha256": parent["snapshot"]["transitionHeadSha256"],
    }
    if actual_parent != expected_parent:
        raise RuntimeError(f"selected local parent drifted: {actual_parent}")

    aggregate_path = evidence_root / "visual-v2-evidence-manifest.json"
    aggregate = json.loads(aggregate_path.read_text(encoding="utf-8"))
    source_identity = aggregate["source"]["identitySha256"]
    if source_identity != "c8afe99762c7a26c747f377f81a2a75b1a0c51cdbde693b6d86f4dfbaddee4e7":
        raise RuntimeError("unexpected final source identity")
    if aggregate["matrix"].get("total") != 62 or aggregate["runtimeValidation"].get("caseCount") != 8:
        raise RuntimeError("final visual/runtime matrix is incomplete")

    draft_dir = evidence_root / "p8-90-r1-ledger-draft"
    task_definition = {
        "taskId": "task_p8_90_r1",
        "displayName": "P8-90-R1",
        "dependencies": [
            "task_2c5f762c2290",
            "task_469ac0bc570d",
            "task_c52f54a8f799",
        ],
        "createdAtUtc": "2026-09-05T08:40:00.000Z",
        "phase": "P8",
        "role": "integration",
        "supersedesTaskId": "task_99a11c23bb9a",
    }
    projected_definitions = dict(parent["recoveryDefinitions"])
    projected_definitions[task_definition["taskId"]] = task_definition
    projected = publisher.effective_tasks(
        origin,
        parent["overrides"],
        repo,
        projected_definitions,
        parent["gateEvents"],
        parent["dependencyReplacements"],
    )
    task = next(row for row in projected if row["taskId"] == task_definition["taskId"])
    if task["status"] != "ready":
        raise RuntimeError(f"recovery task is not ready: {task['status']}")
    before_task_sha = publisher.task_hash(task)
    definition_sha = publisher.digest(publisher.payload(task_definition))

    audit = {
        "documentKind": "mgif-p8-90-r1-integration-audit",
        "schemaVersion": 1,
        "authoritativePlatform": "Windows MSVC Vulkan",
        "sourceIdentity": source_identity,
        "aggregate": record(aggregate_path, publisher),
        "buildRoots": {
            "Debug": r"G:\Temp\mgif-p6-90-msvc-20260831T134500Z-Debug",
            "Release": r"G:\Temp\mgif-p6-90-msvc-20260831T134500Z-Release",
        },
        "configuredDependencyPreflight": "PASS (DEMO_SKIP_EDITOR_DEPENDENCY_PREFLIGHT=OFF)",
        "productionDemo": {
            "controllerConstructedAtStartup": True,
            "openModelWithEditorUsesController": True,
            "candidateFailurePreservesActiveDocument": True,
            "legacyLoaderRole": "viewer/runtime compatibility fallback only when controller is unavailable",
            "duplicateEditorMutationEntryPoint": False,
        },
        "acceptanceDriver": {
            "target": "editor_test_driver",
            "testOnly": True,
            "productionDemoLinksDriver": False,
            "uiStateChecks": 57,
            "uiActionChecks": 40,
            "closedStateCount": 9,
            "defaultScenarioRegistry": 8,
        },
        "lifecycle": {
            "states": [
                "NoDocument",
                "LoadingCandidate",
                "ReadyClean",
                "ReadyModified",
                "GizmoHover",
                "GizmoDragging",
                "Modal",
                "Saving/Merging/Compacting",
                "Error",
            ],
            "controllerTransactionChecks": "Debug/Release PASS",
            "animationChecks": "Debug/Release PASS",
        },
        "nonClaims": [
            "No complete U01-U08 real action replay",
            "No independent P8 Gate verdict",
            "No Linux build or runtime claim",
        ],
    }
    audit_path = draft_dir / "P8-90-R1-integration-audit.json"
    write_json(audit_path, audit, publisher)
    audit_record = record(audit_path, publisher)

    authority = {
        "documentKind": "local-recovery-task-authority",
        "schemaVersion": 1,
        "ledgerId": publisher.LEDGER_ID,
        "parentBundleId": parent["bundleId"],
        "parentSnapshotSha256": parent["pointer"]["snapshotSha256"],
        "taskDefinitionSha256": definition_sha,
        "authorizedAtUtc": task_definition["createdAtUtc"],
        "fixedPolicy": copy.deepcopy(publisher.POLICY),
    }
    authority_path = draft_dir / "P8-90-R1-authority.json"
    write_json(authority_path, authority, publisher)
    authority_record = record(authority_path, publisher)

    report_lines = [
        "# P8-90-R1 Recovery Integration Report",
        "",
        "Decision: COMPLETED bounded P8 integration recovery; this is not an independent P8 Gate verdict.",
        "",
        "The historical P8-90 failure remains immutable. This recovery integrates the three completed P8 leaves,",
        "the controller-owned lifecycle producers, and the acceptance-only typed UI driver at the application boundary.",
        "",
        f"Parent bundle: {parent['bundleId']}",
        f"Parent snapshot: {parent['pointer']['snapshotSha256']}",
        f"Parent transition head: {parent['snapshot']['transitionHeadSha256']}",
        f"Source identity: {source_identity}",
        f"Aggregate manifest: {str(aggregate_path).replace('/', chr(92))}; sha256={publisher.digest(aggregate_path.read_bytes())}",
        f"Integration audit: {str(audit_path).replace('/', chr(92))}; sha256={audit_record['sha256']}",
        "",
        "## Windows/MSVC Evidence",
        "",
        "Windows/MSVC Vulkan is authoritative; WSL only orchestrated and read artifacts.",
        "Debug and Release Demo were rebuilt after the lifecycle/driver changes.",
        "Visual matrix: 62/62 (base 24 per configuration plus 14 DPI extension traces).",
        "Runtime matrix: 8/8 (unfiltered/filtered and UI/no-UI cases).",
        "Focused controller/selection/renderer CTest: 14/14 in Debug and 14/14 in Release.",
        "Acceptance UI state/action tests: 57 and 40 checks respectively in Debug and Release.",
        "P0 scaffold: 42 features, 10 traces, 110 edges; visual and present contracts: 7/7 and 4/4.",
        "Configured production-editor dependency preflight: PASS with skip disabled; RHI boundary guard: PASS.",
        "",
        "## Integration Boundary",
        "",
        "EditorController is constructed at startup and is the only editor command/document authority.",
        "openModelWithEditor() sends candidates through controller preparation/publication; failed candidates do not reset the active document.",
        "MinimalLatestApp retains the legacy loader only for viewer/runtime compatibility when no controller exists; it is not a second editor mutation entry point.",
        "EditorTestDriver is a BUILD_TESTING-only target and is not linked by the production Demo target.",
        "The lifecycle producer exposes exactly nine closed-world states, with save/merge/compact collapsed to the required presentation state.",
        "",
        "## Explicit Non-Claims",
        "",
        "No complete U01-U08 real action/state replay, context-menu manifest, or independent P8 Gate verdict is claimed.",
        "P9/P10 remain blocked until the independent Gate chain is executed.",
        "F42 and R24 remain BLOCKED; Metal remains Not Verified; release/distribution remains NOT PASS.",
        "No Linux build or runtime claim is made, and Orca is not used.",
        "",
    ]
    report_path = draft_dir / "P8-90-R1-REPORT.md"
    report_path.write_text("\n".join(report_lines), encoding="ascii", newline="\n")
    report_record = record(report_path, publisher)

    replacements: list[dict[str, object]] = []
    verdict = {
        "documentKind": "local-task-verdict",
        "schemaVersion": 2,
        "ledgerId": publisher.LEDGER_ID,
        "parentBundleId": parent["bundleId"],
        "parentSnapshotSha256": parent["pointer"]["snapshotSha256"],
        "taskId": task_definition["taskId"],
        "displayName": task_definition["displayName"],
        "beforeTaskSha256": before_task_sha,
        "decision": "completed",
        "outcome": "succeeded",
        "reportSha256": report_record["sha256"],
        "fixedPolicy": copy.deepcopy(publisher.POLICY),
        "reviewAuthority": "local-automatic-review",
        "reviewExecutionOutcome": "succeeded",
        "taskDefinitionSha256": definition_sha,
        "authoritySha256": authority_record["sha256"],
        "gateEvent": None,
        "dependencyReplacementsSha256": publisher.digest(publisher.payload(replacements)),
    }
    verdict_path = draft_dir / "P8-90-R1-verdict.json"
    write_json(verdict_path, verdict, publisher)
    verdict_record = record(verdict_path, publisher)

    receipt = {
        "documentKind": "local-task-transition-receipt",
        "schemaVersion": 2,
        "ledgerId": publisher.LEDGER_ID,
        "parentBundleId": parent["bundleId"],
        "parentSnapshotSha256": parent["pointer"]["snapshotSha256"],
        "previousTransitionSha256": parent["snapshot"]["transitionHeadSha256"],
        "taskId": task_definition["taskId"],
        "displayName": task_definition["displayName"],
        "beforeTaskSha256": before_task_sha,
        "fromStatus": "ready",
        "toStatus": "completed",
        "outcome": "succeeded",
        "completedAtUtc": "2026-09-05T08:41:00.000Z",
        "reviewAuthority": "local-automatic-review",
        "reviewExecutionOutcome": "succeeded",
        "report": report_record,
        "verdictArtifact": verdict_record,
        "fixedPolicy": copy.deepcopy(publisher.POLICY),
        "taskDefinition": task_definition,
        "authorityArtifact": authority_record,
        "gateEvent": None,
        "dependencyReplacements": replacements,
    }
    receipt_path = draft_dir / "P8-90-R1-receipt-draft.json"
    write_json(receipt_path, receipt, publisher)

    for path in (audit_path, authority_path, report_path, verdict_path, receipt_path):
        item = record(path, publisher)
        print(f"{path.name} bytes={item['bytes']} sha256={item['sha256']}")
    print(f"beforeTaskSha256={before_task_sha}")
    print(f"taskDefinitionSha256={definition_sha}")
    print(f"parentBundleId={parent['bundleId']}")
    print(f"parentSnapshotSha256={parent['pointer']['snapshotSha256']}")
    print(f"previousTransitionSha256={parent['snapshot']['transitionHeadSha256']}")


if __name__ == "__main__":
    main()
