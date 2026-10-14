#!/usr/bin/env python3

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "editor_ui_os_replay.ps1"
PLAN_HEADER = REPO / "app" / "editor" / "EditorUiOsReplayPlan.h"
PLAN_SOURCE = REPO / "app" / "editor" / "EditorUiOsReplayPlan.cpp"
CLI_SOURCE = REPO / "app" / "editor" / "EditorUiOsReplayCli.cpp"
CLI_TEST = REPO / "tests" / "editor" / "editor_ui_os_replay_cli_tests.cpp"
PROBE_HEADER = REPO / "app" / "editor" / "EditorUiProbe.h"
APP_HEADER = REPO / "app" / "MinimalLatestApp.h"

CONTEXT_STEP_CONTRACTS = (
    ("scene-context-open", 1, "main", 2, "primary-scene", "scene-add-node", "menu:add-node"),
    (
        "node-context-add-child",
        3,
        "main",
        9,
        "primary-node",
        "node-add-child-menu",
        "menu:add-child",
    ),
    ("node-context-rename", 6, "main", 19, "primary-node", "node-rename", "menu:rename"),
    (
        "node-context-duplicate",
        10,
        "main",
        32,
        "primary-node",
        "node-duplicate",
        "menu:duplicate",
    ),
    (
        "node-context-delete",
        17,
        "main",
        58,
        "regular-delete-node",
        "node-delete",
        "menu:delete",
    ),
    (
        "unique-root-context-delete",
        23,
        "unique-root",
        75,
        "unique-root-node",
        "node-delete",
        "menu:delete",
    ),
)


def windows_path(path: Path) -> str:
    path = path.resolve()
    if os.name == "nt":
        return str(path)
    converted = subprocess.run(
        ["wslpath", "-w", str(path)],
        check=True,
        capture_output=True,
        text=True,
    )
    return converted.stdout.strip()


def powershell() -> str:
    executable = shutil.which("powershell.exe") or shutil.which("powershell")
    if executable is None:
        raise unittest.SkipTest("Windows PowerShell is unavailable")
    return executable


def run_contract_mode(case: str = "U02-hierarchy") -> dict:
    completed = subprocess.run(
        [
            powershell(),
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            windows_path(SCRIPT),
            "-Case",
            case,
            "-ContractOnly",
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    return json.loads(completed.stdout)


def run_contract_trace(case: str = "U02-hierarchy") -> bytes:
    return subprocess.run(
        [
            powershell(),
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            windows_path(SCRIPT),
            "-ContractTraceOnly",
            "-Case", case,
        ],
        check=True,
        capture_output=True,
    ).stdout


def find_vcvars() -> str:
    command = r"""
$candidates = @()
$installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path -LiteralPath $installer) {
  $installation = & $installer -latest -products * -property installationPath
  if ($installation) {
    $candidates += Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
  }
}
$candidates += @(
  'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat',
  'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat',
  'C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat',
  'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat'
)
$found = @($candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1)
if ($found.Count -ne 1) { exit 3 }
[Console]::Out.Write([string]$found[0])
"""
    completed = subprocess.run(
        [powershell(), "-NoProfile", "-Command", command],
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0 or not completed.stdout.strip():
        raise unittest.SkipTest("MSVC vcvars64.bat is unavailable")
    return completed.stdout.strip()


class EditorUiOsReplayContractTests(unittest.TestCase):
    def assert_context_steps_exact(self, plan_steps: list) -> None:
        steps = {step["id"]: step for step in plan_steps}
        expected_ids = {item[0] for item in CONTEXT_STEP_CONTRACTS}
        edge_kinds = [
            phase["input"]["kind"]
            for step in plan_steps
            for phase in step["phases"]
        ]
        edge_step_ids = {
            step["id"]
            for step in plan_steps
            if any(
                phase["input"]["kind"]
                in {"right-click", "right-down", "right-up"}
                for phase in step["phases"]
            )
        }
        self.assertEqual(edge_step_ids, expected_ids)
        self.assertEqual(edge_kinds.count("right-click"), 0)
        self.assertEqual(edge_kinds.count("right-down"), 6)
        self.assertEqual(edge_kinds.count("right-up"), 6)

        def phase(
            ordinal: int,
            phase_id: str,
            probe_phase: str,
            semantic: str,
            role: str,
            graph_role: str,
            input_kind: str,
            require_hovered: bool,
            capture: str,
        ) -> dict:
            return {
                "ordinal": ordinal,
                "id": phase_id,
                "phase": probe_phase,
                "target": {
                    "semantic": semantic,
                    "role": role,
                    "graphRowRole": graph_role,
                },
                "input": {"kind": input_kind, "value": None},
                "requireHovered": require_hovered,
                "requireFocused": False,
                "requireDropAccepted": False,
                "capture": capture,
            }

        for (
            step_id,
            step_index,
            subcase,
            first_ordinal,
            graph_role,
            popup_semantic,
            popup_role,
        ) in CONTEXT_STEP_CONTRACTS:
            self.assertIn(step_id, steps)
            step = steps[step_id]
            self.assertEqual(step["index"], step_index)
            self.assertEqual(step["subcase"], subcase)
            self.assertEqual(step["effect"], "presentation-only")
            self.assertEqual(step["expectedRevisionDelta"], 0)
            self.assertEqual(step["history"], "unchanged")
            self.assertEqual(step["expectedDispatches"], [])
            self.assertFalse(step["expectHierarchyOrderDigestChange"])
            self.assertFalse(step["relaunchProcessBefore"])
            self.assertEqual(
                step["phases"],
                [
                    phase(
                        first_ordinal,
                        "target",
                        "target",
                        "graph-row",
                        graph_role,
                        graph_role,
                        "move-pointer",
                        False,
                        "before",
                    ),
                    phase(
                        first_ordinal + 1,
                        "hover",
                        "hover",
                        "graph-row",
                        graph_role,
                        graph_role,
                        "right-down",
                        True,
                        "none",
                    ),
                    phase(
                        first_ordinal + 2,
                        "release",
                        "hover",
                        "graph-row",
                        graph_role,
                        graph_role,
                        "right-up",
                        True,
                        "none",
                    ),
                    phase(
                        first_ordinal + 3,
                        "settled",
                        "settled",
                        popup_semantic,
                        popup_role,
                        graph_role,
                        "none",
                        False,
                        "after",
                    ),
                ],
                step_id,
            )

    def test_powershell_ast_is_clean(self) -> None:
        script = windows_path(SCRIPT).replace("'", "''")
        command = (
            "$tokens=$null; $errors=$null; "
            f"[void][Management.Automation.Language.Parser]::ParseFile('{script}',"
            "[ref]$tokens,[ref]$errors); "
            "if($errors.Count -ne 0){$errors | ForEach-Object {Write-Error $_}; exit 2}"
        )
        completed = subprocess.run(
            [powershell(), "-NoProfile", "-Command", command],
            capture_output=True,
            text=True,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)

    def test_process_authority_reset_ast_cases(self) -> None:
        script = windows_path(SCRIPT).replace("'", "''")
        command = (
            "$tokens=$null; $parseErrors=$null; "
            f"$ast=[Management.Automation.Language.Parser]::ParseFile('{script}',"
            "[ref]$tokens,[ref]$parseErrors); "
            "if($parseErrors.Count -ne 0){throw 'source parse failed'}; "
            "$functions=@($ast.FindAll({param($node) "
            "$node -is [Management.Automation.Language.FunctionDefinitionAst] "
            "-and $node.Name -ceq 'Test-ProcessAuthorityReset'},$true)); "
            "if($functions.Count -ne 1){throw 'authority reset function count'}; "
            "Invoke-Expression ([string]$functions[0].Extent.Text); "
            "function New-ResetState([string]$nonce,[uint32]$processId,"
            "[int64]$undo=0,[int64]$redo=0){"
            "[pscustomobject]@{runNonce=$nonce;pid=$processId;authority=[int64]1;"
            "undo=$undo;redo=$redo}}; "
            "$previous=New-ResetState 'nonce-a' 41; "
            "$fresh=New-ResetState 'nonce-b' 42; "
            "Test-ProcessAuthorityReset $previous $fresh; "
            "function Assert-ResetRejected([string]$name,[object]$current){"
            "$rejected=$false; try{Test-ProcessAuthorityReset $previous $current}"
            "catch{if($_.Exception.Message -notlike '*fresh process authority*')"
            "{throw};$rejected=$true}; if(-not $rejected){"
            "throw ('negative case accepted: '+$name)}}; "
            "Assert-ResetRejected 'same-nonce' "
            "(New-ResetState 'nonce-a' 42); "
            "Assert-ResetRejected 'zero-pid' "
            "(New-ResetState 'nonce-b' 0); "
            "Assert-ResetRejected 'same-pid' "
            "(New-ResetState 'nonce-b' 41); "
            "Assert-ResetRejected 'undo-history' "
            "(New-ResetState 'nonce-b' 42 1 0); "
            "Assert-ResetRejected 'redo-history' "
            "(New-ResetState 'nonce-b' 42 0 1); "
            "[Console]::Out.Write('AUTHORITY_RESET_CASES_OK')"
        )
        completed = subprocess.run(
            [powershell(), "-NoProfile", "-Command", command],
            capture_output=True,
        )
        diagnostics = (completed.stdout + completed.stderr).decode(
            "utf-8", errors="replace"
        )
        self.assertEqual(completed.returncode, 0, diagnostics)
        self.assertEqual(completed.stdout, b"AUTHORITY_RESET_CASES_OK", diagnostics)

    def test_sendinput_only_and_fail_closed_ordering(self) -> None:
        source = SCRIPT.read_text(encoding="utf-8")
        self.assertRegex(source, r"public static extern uint SendInput\(")
        self.assertNotRegex(source.lower(), r"mouse_event|postmessage|sendmessage")
        for token in (
            "GetForegroundWindow",
            "GetWindowThreadProcessId",
            "GetDpiForWindow",
            "matchCount",
            "readySha256",
            "hierarchyOrderDigest",
            "selectedGraphRowKey",
            "observedDispatches",
            "EditorUiProbeFrameBinding",
            "Get-EvidenceRelativePath",
            "$cursor -is [IO.FileInfo]",
            "Graph same-object alias constraint failed",
            "main-process-boundary",
            "unique-root-select",
            "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT",
            'Remove("VK_LAYER_MESSAGE_ID_FILTER")',
            "desktop-dc-client-rect",
            "PNG client rectangle is outside the virtual desktop",
            "[uint32]0x40CC0020",
            "SetThreadDpiAwarenessContext",
            "WindowFromPoint",
            "right-down",
            "right-up",
            "RightButtonHeld",
            "Another top-level window occludes the capture client",
            "Capture client contract changed during pixel acquisition",
            "Persisted PNG pixels differ from the audited bitmap",
            "OS replay window lost foreground while waiting for ready",
            "best-effort-finally",
        ):
            self.assertIn(token, source)
        capability_gate = source.index("BLOCKED_APP_PROBE_UNAVAILABLE")
        evidence_creation = source.index(
            "New-Item -ItemType Directory -Path $EvidenceRoot"
        )
        pass_manifest = source.index('status = "PASS_TRUE_OS_REPLAY"')
        final_result = source.index("$FinalResultRecords")
        self.assertLess(capability_gate, evidence_creation)
        self.assertLess(final_result, pass_manifest)
        self.assertEqual(source.count('status = "PASS_TRUE_OS_REPLAY"'), 1)
        self.assertNotIn("CapabilityPath", source)
        self.assertIn(
            '$capabilityStart.Arguments = "--editor-os-capability '
            '--editor-os-capability-case=$Case"',
            source,
        )
        selection = source[
            source.index('if ([string]::IsNullOrWhiteSpace($ExecutablePath))'):
            source.index('$Executable = [IO.Path]::GetFullPath($ExecutablePath)')
        ]
        self.assertNotIn("$semantic", selection)
        self.assertNotIn("$ready", selection)

    def test_inspector_case_is_explicit_partial_os_coverage(self) -> None:
        contract = run_contract_mode("U03-inspector-transform")
        self.assertEqual(contract["status"], "CONTRACT_OK")
        self.assertEqual(contract["case"], "U03-inspector-transform")
        self.assertEqual(contract["parentCase"], "U03-transform-gizmo")
        self.assertEqual(contract["stepCount"], 8)
        self.assertEqual(contract["phaseCount"], 26)
        self.assertFalse(contract["coverageComplete"])
        self.assertFalse(contract["passEvidenceProduced"])
        self.assertTrue(contract["sendInputOnly"])
        source = SCRIPT.read_text(encoding="utf-8")
        self.assertIn('$Manifest.status = "PASS_TRUE_OS_REPLAY_PARTIAL"', source)
        self.assertIn("$Manifest.coverageComplete = $false", source)
        self.assertIn("expectedDispatchCount", source)
        self.assertIn('New-Input "move-pointer" "80,0"', source)
        self.assertNotIn("$x += 80", source)
        self.assertNotIn("$passiveSemantic", source)
        self.assertIn(
            "Unsupported passive target cannot establish true OS replay evidence",
            source,
        )
        passive_branch = re.search(
            r"elseif \(\$semantic -in @\(([^\n]*)\)\) \{\n"
            r'\s+throw "Unsupported passive target',
            source,
        )
        self.assertIsNotNone(passive_branch)
        self.assertNotIn("inspector-", passive_branch.group(1))

    def test_contract_mode_has_complete_u02_table(self) -> None:
        contract = run_contract_mode()
        self.assertEqual(contract["status"], "CONTRACT_OK")
        self.assertEqual(contract["stepCount"], 29)
        self.assertEqual(contract["phaseCount"], 91)
        self.assertRegex(contract["planSha256"], r"^[0-9a-f]{64}$")
        self.assertTrue(contract["sendInputOnly"])
        self.assertTrue(contract["appProbeRequired"])
        self.assertFalse(contract["passEvidenceProduced"])

        cpp = PLAN_SOURCE.read_text(encoding="utf-8")
        for action in (
            "scene-context-open",
            "scene-add-node",
            "node-context-add-child",
            "node-add-child-empty",
            "rename-replace-text",
            "rename-submit",
            "node-duplicate-menu",
            "cross-parent-drag",
            "undo-cross-parent",
            "redo-cross-parent",
            "same-parent-drag-noop",
            "root-root-drag-noop",
            "delete-confirm",
            "main-process-boundary",
            "unique-root-subcase",
            "unique-root-select",
            "unique-root-delete-confirm",
            "unique-root-undo",
            "unique-root-redo",
        ):
            self.assertIn(f'"{action}"', cpp)

    def test_gizmo_translate_case_has_independent_partial_contract(self) -> None:
        contract = run_contract_mode("U03-gizmo-translate")
        self.assertEqual(contract["status"], "CONTRACT_OK")
        self.assertEqual(contract["case"], "U03-gizmo-translate")
        self.assertEqual(contract["parentCase"], "U03-transform-gizmo")
        self.assertEqual(contract["stepCount"], 6)
        self.assertEqual(contract["phaseCount"], 16)
        self.assertFalse(contract["coverageComplete"])
        self.assertFalse(contract["passEvidenceProduced"])
        self.assertTrue(contract["sendInputOnly"])
        source = SCRIPT.read_text(encoding="utf-8")
        for token in (
            "editor-gizmo-adapter-v1",
            "Native gizmo target lost its actual adapter identity",
            "Native gizmo target is not its classified observed rectangle",
            "Native outside-release target intersects the observed viewport",
            "Native gizmo pointer does not match current physical cursor",
            "Native gizmo commit lacks its observed terminal event",
            "Native TranslateY drag changed an unrelated TRS component",
            "Native gizmo history did not restore the captured transform",
            "$phaseRecord.gizmo = $state.gizmo",
        ):
            self.assertIn(token, source)

    def test_escape_case_has_real_held_key_and_late_release_contract(self) -> None:
        contract = run_contract_mode("U03-gizmo-escape")
        self.assertEqual(contract["status"], "CONTRACT_OK")
        self.assertEqual(contract["case"], "U03-gizmo-escape")
        self.assertEqual(contract["parentCase"], "U03-transform-gizmo")
        self.assertEqual(contract["stepCount"], 5)
        self.assertEqual(contract["phaseCount"], 15)
        self.assertFalse(contract["coverageComplete"])
        self.assertFalse(contract["passEvidenceProduced"])
        trace = run_contract_trace("U03-gizmo-escape")
        self.assertEqual(hashlib.sha256(trace).hexdigest(), contract["planSha256"])
        phases = [line.split("\t") for line in trace.decode().splitlines() if line[:1].isdigit()]
        self.assertEqual(len(phases), 15)
        self.assertTrue(all(len(phase) == 25 for phase in phases))
        drag = [phase for phase in phases if phase[1] == "drag-escape-only"]
        self.assertEqual([phase[15] for phase in drag], [
            "move-pointer", "left-down", "move-pointer", "key-down", "none"
        ])
        self.assertEqual(drag[3][16], "Escape")
        self.assertEqual(drag[-1][23:], ["0:0:1:1:0:1", "1:0:1"])
        self.assertEqual(drag[-1][5], "unchanged")
        late = [phase for phase in phases if phase[1] == "late-release"]
        self.assertEqual([phase[15] for phase in late], ["key-up", "left-up", "none"])
        self.assertEqual(late[0][16], "Escape")
        self.assertTrue(all(phase[23:] == ["0:0:1:1:0:1", "1:0:1"] for phase in late))
        source = SCRIPT.read_text(encoding="utf-8")
        for token in (
            "Native Escape did not detach and clear actual selection",
            "Native Escape retained TRS is not bound to the actual gesture node",
            "Native Escape detach lacks its exact single Escape terminal event",
            "Native Escape unexpectedly committed, changed history, or received another terminal",
            "Test-GizmoTrsEqual $gizmo $GizmoEscapePreview",
        ):
            self.assertIn(token, source)

    def test_focus_cases_have_bounded_partition_contracts(self) -> None:
        expected_inputs = {
            "U03-gizmo-focus-no-release": [
                "move-pointer", "left-down", "move-pointer", "focus-loss",
                "focus-restore", "left-up", "none",
            ],
            "U03-gizmo-focus-same-frame": [
                "move-pointer", "left-down", "move-pointer",
                "left-up-focus-loss", "focus-restore", "none", "none",
            ],
            "U03-gizmo-focus-delivered-release": [
                "move-pointer", "left-down", "move-pointer", "left-up",
                "focus-loss", "focus-restore", "none",
            ],
        }
        for case, input_kinds in expected_inputs.items():
            with self.subTest(case=case):
                contract = run_contract_mode(case)
                self.assertEqual(contract["status"], "CONTRACT_OK")
                self.assertEqual(contract["case"], case)
                self.assertEqual(contract["parentCase"], "U03-transform-gizmo")
                self.assertEqual((contract["stepCount"], contract["phaseCount"]), (4, 14))
                self.assertFalse(contract["coverageComplete"])
                self.assertFalse(contract["passEvidenceProduced"])
                self.assertTrue(contract["sendInputOnly"])

                trace = run_contract_trace(case)
                self.assertEqual(hashlib.sha256(trace).hexdigest(), contract["planSha256"])
                phases = [
                    line.split("\t")
                    for line in trace.decode().splitlines()
                    if line[:1].isdigit()
                ]
                self.assertEqual(len(phases), 14)
                self.assertTrue(all(len(phase) == 25 for phase in phases))
                drag_id = {
                    "U03-gizmo-focus-no-release": "drag-focus-no-release",
                    "U03-gizmo-focus-same-frame": "drag-focus-same-frame",
                    "U03-gizmo-focus-delivered-release": "drag-focus-delivered-release",
                }[case]
                drag = [phase for phase in phases if phase[1] == drag_id]
                self.assertEqual([phase[15] for phase in drag], input_kinds)
                self.assertEqual(drag[0][23:], ["1:0:0:0:0:", "0:0:0:0"])
                self.assertEqual(drag[1][23:], ["1:0:0:0:0:", "0:0:0:0"])
                self.assertEqual(drag[2][23], "2:1:1:0:0:0")
                self.assertEqual(drag[3][23], "2:1:1:1:0:1")
                if case == "U03-gizmo-focus-delivered-release":
                    self.assertEqual(drag[-1][23:], ["1:0:1:1:1:1", "1:0:0:0"])
                    self.assertEqual(drag[4][23], "1:0:1:1:1:1")
                    self.assertEqual(drag[5][23], "1:0:1:1:1:")
                    self.assertEqual(drag[3][16], "")
                    self.assertEqual(drag[4][16], "sink")
                elif case == "U03-gizmo-focus-same-frame":
                    self.assertEqual(drag[3][16], "release-then-focus-loss")
                    self.assertEqual(drag[4][23], "3:0:1:1:0:")
                    self.assertEqual(drag[5][23], "3:0:1:1:0:1")
                    self.assertEqual(drag[-1][23:], ["3:0:1:1:0:1", "0:0:1:1"])
                else:
                    self.assertEqual(drag[3][16], "sink")
                    self.assertEqual(drag[4][16], "editor")
                    # Ready is published before each phase's input.  Win32
                    # focus loss has no mouse-up callback, so the first
                    # post-loss ready has only focus-lost; the first
                    # suppressed polling release appears after restore.
                    self.assertEqual(drag[4][23], "3:0:1:1:0:")
                    self.assertEqual(drag[5][23], "3:0:1:1:0:1")
                    self.assertEqual(drag[4][24], "0:0:1:0")
                    self.assertEqual(drag[5][24], "0:0:1:1")
                    self.assertEqual(drag[-1][23:], ["3:0:1:1:0:1", "0:0:1:2"])
                source = SCRIPT.read_text(encoding="utf-8")
                for token in (
                    "U03-gizmo-focus-no-release",
                    "U03-gizmo-focus-same-frame",
                    "U03-gizmo-focus-delivered-release",
                    "focus-loss",
                    "focus-restore",
                    "left-up-focus-loss",
                    "focus-loss-with-release",
                    "deliveredReleaseCount",
                    "suppressedReleaseCount",
                ):
                    self.assertIn(token, source)

    def test_same_frame_cases_have_exact_batch_contracts_without_old_hash_drift(self) -> None:
        for case, expected_hash in (
            ("U02-hierarchy", "15b7e1037139631e477b6e2ffb9bc0421ccb6932c5616ba2a3ef46c664769526"),
            ("U03-inspector-transform", "51e7c906fb2110153d10355291418358e62c53d28a589f5046bf931732896eb9"),
            ("U03-gizmo-translate", "d2900c9b1d188e2481d03a786f4c11ba1bcfe7dc61ab3f2f5555283b28bed60b"),
            ("U03-gizmo-escape", "3d1114057328c871ef8bacafa17e434d34eae124a9234ddc8e3e204bdc10d879"),
        ):
            with self.subTest(unchanged_case=case):
                self.assertEqual(run_contract_mode(case)["planSha256"], expected_hash)
        hashes = []
        for case, order in (
            ("U03-gizmo-escape-release", "escape-first"),
            ("U03-gizmo-release-escape", "release-first"),
        ):
            with self.subTest(case=case):
                contract = run_contract_mode(case)
                self.assertEqual(contract["status"], "CONTRACT_OK")
                self.assertEqual(contract["case"], case)
                self.assertEqual(contract["parentCase"], "U03-transform-gizmo")
                self.assertFalse(contract["coverageComplete"])
                self.assertFalse(contract["passEvidenceProduced"])
                self.assertEqual((contract["stepCount"], contract["phaseCount"]), (5, 15))
                trace = run_contract_trace(case)
                hashes.append(hashlib.sha256(trace).hexdigest())
                self.assertEqual(hashes[-1], contract["planSha256"])
                phases = [line.split("\t") for line in trace.decode().splitlines() if line[:1].isdigit()]
                self.assertEqual(len(phases), 15)
                self.assertTrue(all(len(phase) == 25 for phase in phases))
                drag = [phase for phase in phases if phase[1] == "drag-same-frame"]
                self.assertEqual([phase[15] for phase in drag], [
                    "move-pointer", "left-down", "move-pointer", "escape-release-batch", "none"
                ])
                self.assertEqual((drag[3][10], drag[3][16]), ("terminal-batch", order))
                self.assertEqual(drag[-1][23:], ["0:0:1:1:0:1", "1:0:1"])
                self.assertTrue(all(phase[5] == "unchanged" for phase in drag))
                late = [phase for phase in phases if phase[1] == "late-release"]
                self.assertEqual([phase[15] for phase in late], ["key-up", "left-up", "none"])
                self.assertEqual(late[0][16], "Escape")
        self.assertNotEqual(hashes[0], hashes[1])

    def test_native_pixel_audit_accepts_sparse_model_not_blank_viewport(self) -> None:
        script = windows_path(SCRIPT).replace("'", "''")
        command = (
            "Add-Type -AssemblyName System.Drawing; "
            "$tokens=$null; $parseErrors=$null; "
            f"$ast=[Management.Automation.Language.Parser]::ParseFile('{script}',"
            "[ref]$tokens,[ref]$parseErrors); "
            "if($parseErrors.Count -ne 0){throw 'source parse failed'}; "
            "$functions=@($ast.FindAll({param($node) "
            "$node -is [Management.Automation.Language.FunctionDefinitionAst] "
            "-and $node.Name -ceq 'Get-PixelAudit'},$true)); "
            "if($functions.Count -ne 1){throw 'pixel audit function count'}; "
            "Invoke-Expression ([string]$functions[0].Extent.Text); "
            "$Case='U03-gizmo-translate'; "
            "$mask=[pscustomobject]@{source='editor-gizmo-adapter-v1';"
            "renderedFrame=3;coordinateSpace='client-physical-px';"
            "left=100;top=20;right=500;bottom=360}; "
            "$bitmap=[Drawing.Bitmap]::new(600,360); "
            "$graphics=[Drawing.Graphics]::FromImage($bitmap); "
            "try { $graphics.Clear([Drawing.Color]::FromArgb(32,32,32)); "
            "for($y=0;$y -lt 360;$y+=4){"
            "$color=50+($y % 180);"
            "$pen=[Drawing.Pen]::new([Drawing.Color]::FromArgb($color,$color,$color));"
            "try{$graphics.DrawLine($pen,0,$y,599,$y)}finally{$pen.Dispose()}}; "
            "$graphics.FillRectangle([Drawing.Brushes]::Blue,8,4,84,12); "
            "$graphics.FillRectangle([Drawing.Brushes]::Black,100,20,400,340); "
            "$graphics.FillRectangle([Drawing.Brushes]::Red,260,80,80,40); "
            "$graphics.FillRectangle([Drawing.Brushes]::Lime,300,120,80,40); "
            "$audit=Get-PixelAudit $bitmap $mask; "
            "if($audit.zones.Count -ne 9 -or $audit.zones[7].uiSamples -ne 0 "
            "-or $audit.zones[7].nonBlack -ne 0 "
            "-or $audit.zones[7].rule -ne 'observed-viewport-only' "
            "-or $audit.viewport.filledColorfulBlocks -lt 16)"
            "{throw 'sparse viewport mask evidence is incomplete'}; "
            "$Case='U03-gizmo-escape'; $escapeAudit=Get-PixelAudit $bitmap $mask; "
            "if($escapeAudit.viewport.filledColorfulBlocks -ne $audit.viewport.filledColorfulBlocks)"
            "{throw 'Escape viewport mask differs from native Translate'}; "
            "$Case='U02-hierarchy'; $rejected=$false; "
            "try{Get-PixelAudit $bitmap | Out-Null}catch{"
            "if($_.Exception.Message -notlike '*uninformative zone*'){throw};"
            "$rejected=$true}; if(-not $rejected){throw 'U02 zone rule changed'}; "
            "$Case='U03-inspector-transform'; $rejected=$false; "
            "try{Get-PixelAudit $bitmap | Out-Null}catch{"
            "if($_.Exception.Message -notlike '*uninformative zone*'){throw};"
            "$rejected=$true}; if(-not $rejected){throw 'Inspector zone rule changed'}; "
            "$Case='U03-gizmo-translate'; "
            "$graphics.FillRectangle([Drawing.Brushes]::Black,100,20,400,340); "
            "$rejected=$false; try{Get-PixelAudit $bitmap $mask | Out-Null}catch{"
            "if($_.Exception.Message -notlike '*Observed viewport PNG is blank*'){throw};"
            "$rejected=$true}; if(-not $rejected){throw 'blank viewport accepted'}; "
            "$graphics.DrawLine([Drawing.Pens]::Red,100,140,499,140); "
            "$graphics.DrawLine([Drawing.Pens]::Lime,300,20,300,359); "
            "$rejected=$false; try{Get-PixelAudit $bitmap $mask | Out-Null}catch{"
            "if($_.Exception.Message -notlike '*Observed viewport PNG is blank*'){throw};"
            "$rejected=$true}; if(-not $rejected){throw 'thin axes accepted as model'}; "
            "$rejected=$false; try{Get-PixelAudit $bitmap | Out-Null}catch{"
            "if($_.Exception.Message -notlike '*observed physical mask*'){throw};"
            "$rejected=$true}; if(-not $rejected){throw 'missing viewport mask accepted'}; "
            "[Console]::Out.Write('NATIVE_PIXEL_AUDIT_CASES_OK') "
            "} finally {$graphics.Dispose();$bitmap.Dispose()}"
        )
        completed = subprocess.run(
            [powershell(), "-NoProfile", "-Command", command],
            capture_output=True,
        )
        diagnostics = (completed.stdout + completed.stderr).decode(
            "utf-8", errors="replace"
        )
        self.assertEqual(completed.returncode, 0, diagnostics)
        self.assertEqual(completed.stdout, b"NATIVE_PIXEL_AUDIT_CASES_OK", diagnostics)

    def test_held_key_edges_are_independent_and_recoverable(self) -> None:
        script = windows_path(SCRIPT).replace("'", "''")
        command = (
            "$tokens=$null; $parseErrors=$null; "
            f"$ast=[Management.Automation.Language.Parser]::ParseFile('{script}',"
            "[ref]$tokens,[ref]$parseErrors); "
            "if($parseErrors.Count -ne 0){throw 'source parse failed'}; "
            "foreach($name in @('Get-VirtualKey','Send-HeldKeyEdge')){"
            "$functions=@($ast.FindAll({param($node) "
            "$node -is [Management.Automation.Language.FunctionDefinitionAst] "
            "-and $node.Name -ceq $name},$true)); "
            "if($functions.Count -ne 1){throw 'held-key function count'}; "
            "Invoke-Expression ([string]$functions[0].Extent.Text)}; "
            "$HeldKeys=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal); "
            "$calls=[Collections.Generic.List[object]]::new(); $failSend=$false; "
            "function New-KeyInput([uint16]$key,[uint16]$scan,[uint32]$flags){"
            "[pscustomobject]@{key=$key;scan=$scan;flags=$flags}}; "
            "function Send-NativeInput([object[]]$values){"
            "$calls.Add(@($values)); if($failSend){throw 'injected test failure'};"
            "return @($values).Count}; "
            "$count=Send-HeldKeyEdge 'key-down' 'Escape'; "
            "if($count -ne 1 -or $calls[0].Count -ne 1 -or "
            "$calls[0][0].key -ne 27 -or $calls[0][0].flags -ne 0 -or "
            "-not $HeldKeys.Contains('Escape')){throw 'keydown was not one held edge'}; "
            "$rejected=$false; try{Send-HeldKeyEdge 'key-down' 'Escape' | Out-Null}"
            "catch{$rejected=$true}; if(-not $rejected -or $calls.Count -ne 1)"
            "{throw 'duplicate keydown was injected'}; "
            "$count=Send-HeldKeyEdge 'key-up' 'Escape'; "
            "if($count -ne 1 -or $calls[1].Count -ne 1 -or "
            "$calls[1][0].key -ne 27 -or $calls[1][0].flags -ne 2 -or "
            "$HeldKeys.Count -ne 0){throw 'keyup was not one release edge'}; "
            "$rejected=$false; try{Send-HeldKeyEdge 'key-up' 'Escape' | Out-Null}"
            "catch{$rejected=$true}; if(-not $rejected -or $calls.Count -ne 2)"
            "{throw 'unmatched keyup was injected'}; "
            "$failSend=$true; try{Send-HeldKeyEdge 'key-down' 'Escape' | Out-Null}"
            "catch{if($_.Exception.Message -ne 'injected test failure'){throw}}; "
            "if(-not $HeldKeys.Contains('Escape')){throw 'failed keydown lost cleanup state'}; "
            "$failSend=$false; $count=Send-HeldKeyEdge 'key-up' 'Escape'; "
            "if($count -ne 1 -or $HeldKeys.Count -ne 0){throw 'failed-key cleanup did not release'}; "
            "[Console]::Out.Write('HELD_KEY_EDGE_CASES_OK')"
        )
        completed = subprocess.run(
            [powershell(), "-NoProfile", "-Command", command], capture_output=True
        )
        diagnostics = (completed.stdout + completed.stderr).decode("utf-8", errors="replace")
        self.assertEqual(completed.returncode, 0, diagnostics)
        self.assertEqual(completed.stdout, b"HELD_KEY_EDGE_CASES_OK", diagnostics)
        source = SCRIPT.read_text(encoding="utf-8")
        self.assertIn('foreach ($heldKey in @($HeldKeys))', source)
        self.assertIn('Send-HeldKeyEdge "key-up" ([string]$heldKey)', source)

    def test_same_frame_batch_uses_one_ordered_send_and_preserves_cleanup(self) -> None:
        script = windows_path(SCRIPT).replace("'", "''")
        command = (
            "$tokens=$null; $parseErrors=$null; "
            f"$ast=[Management.Automation.Language.Parser]::ParseFile('{script}',"
            "[ref]$tokens,[ref]$parseErrors); "
            "if($parseErrors.Count -ne 0){throw 'source parse failed'}; "
            "foreach($name in @('Get-VirtualKey','Send-HeldKeyEdge','Send-EscapeReleaseBatch')){"
            "$functions=@($ast.FindAll({param($node) "
            "$node -is [Management.Automation.Language.FunctionDefinitionAst] "
            "-and $node.Name -ceq $name},$true)); "
            "if($functions.Count -ne 1){throw 'batch function count'}; "
            "Invoke-Expression ([string]$functions[0].Extent.Text)}; "
            "$HeldKeys=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal); "
            "$calls=[Collections.Generic.List[object]]::new(); "
            "$failSend=$false; $partialSend=$false; "
            "function New-KeyInput([uint16]$key,[uint16]$scan,[uint32]$flags){"
            "[pscustomobject]@{kind='key';key=$key;flags=$flags}}; "
            "function New-MouseInput([int]$x,[int]$y,[uint32]$flags){"
            "[pscustomobject]@{kind='mouse';x=$x;y=$y;flags=$flags}}; "
            "function Send-NativeInput([object[]]$values){"
            "$calls.Add(@($values)); if($failSend){throw 'injected batch failure'};"
            "if($partialSend){return 1}; return @($values).Count}; "
            "foreach($order in @('escape-first','release-first')){"
            "$Case=if($order -eq 'escape-first'){'U03-gizmo-escape-release'}else{'U03-gizmo-release-escape'};"
            "$LeftButtonHeld=$true; $HeldKeys.Clear(); $calls.Clear(); "
            "$sent=Send-EscapeReleaseBatch $order; "
            "if($sent -ne 2 -or $calls.Count -ne 1 -or $calls[0].Count -ne 2 "
            "-or $LeftButtonHeld -or -not $HeldKeys.Contains('Escape'))"
            "{throw 'batch was not one complete held-key native send'}; "
            "$keyIndex=if($order -eq 'escape-first'){0}else{1}; $mouseIndex=1-$keyIndex; "
            "if($calls[0][$keyIndex].kind -ne 'key' -or $calls[0][$keyIndex].key -ne 27 "
            "-or $calls[0][$keyIndex].flags -ne 0 -or $calls[0][$mouseIndex].kind -ne 'mouse' "
            "-or $calls[0][$mouseIndex].flags -ne 4){throw 'native batch order changed'}; "
            "$sent=Send-HeldKeyEdge 'key-up' 'Escape'; "
            "if($sent -ne 1 -or $HeldKeys.Count -ne 0 -or $calls[1].Count -ne 1 "
            "-or $calls[1][0].flags -ne 2){throw 'batch cleanup did not release Escape'}; "
            "foreach($fault in @('send-failure','partial-send')){"
            "$LeftButtonHeld=$true; $HeldKeys.Clear(); $calls.Clear(); "
            "$failSend=$fault -eq 'send-failure'; $partialSend=$fault -eq 'partial-send'; "
            "$rejected=$false; try{Send-EscapeReleaseBatch $order | Out-Null}catch{$rejected=$true}; "
            "if(-not $rejected -or -not $HeldKeys.Contains('Escape') -or -not $LeftButtonHeld)"
            "{throw 'failed native batch lost held-input cleanup state'}; "
            "$failSend=$false; $partialSend=$false; "
            "$sent=Send-HeldKeyEdge 'key-up' 'Escape'; "
            "if($sent -ne 1 -or $HeldKeys.Count -ne 0){throw 'failed-batch Escape cleanup failed'}}; "
            "foreach($fault in @('wrong-order','mouse-not-held','key-already-held')){"
            "$LeftButtonHeld=$fault -ne 'mouse-not-held'; $HeldKeys.Clear(); $calls.Clear(); "
            "if($fault -eq 'key-already-held'){[void]$HeldKeys.Add('Escape')}; "
            "$argument=if($fault -eq 'wrong-order'){'invalid'}else{$order}; "
            "$rejected=$false; try{Send-EscapeReleaseBatch $argument | Out-Null}catch{$rejected=$true}; "
            "if(-not $rejected -or $calls.Count -ne 0){throw 'invalid batch injected native input'}}}; "
            "$Case='U03-gizmo-escape'; $LeftButtonHeld=$true; $HeldKeys.Clear(); $calls.Clear(); "
            "$rejected=$false; try{Send-EscapeReleaseBatch 'escape-first' | Out-Null}catch{$rejected=$true}; "
            "if(-not $rejected -or $calls.Count -ne 0){throw 'batch escaped scoped same-frame cases'}; "
            "[Console]::Out.Write('SAME_FRAME_BATCH_CASES_OK')"
        )
        completed = subprocess.run(
            [powershell(), "-NoProfile", "-Command", command], capture_output=True
        )
        diagnostics = (completed.stdout + completed.stderr).decode("utf-8", errors="replace")
        self.assertEqual(completed.returncode, 0, diagnostics)
        self.assertEqual(completed.stdout, b"SAME_FRAME_BATCH_CASES_OK", diagnostics)

    def test_focus_loss_delivery_fence_and_unfocused_pointer_guard(self) -> None:
        app_source = APP_HEADER.read_text(encoding="utf-8")
        script_source = SCRIPT.read_text(encoding="utf-8")
        self.assertIn(
            "if(pointerUsable && (windowFocused || m_editorVisualEnabled))",
            app_source,
        )
        self.assertIn("function Wait-EditorInputDeliveryFence", script_source)
        release_index = script_source.rindex('"left-up-focus-loss"')
        release_block = script_source[release_index : release_index + 1000]
        self.assertIn("Wait-EditorInputDeliveryFence", release_block)
        self.assertIn("deliveryFence = $LastInputDeliveryFence", release_block)
        self.assertIn('"native-queue-before-continue"', script_source)

    def test_vulkan_screenshot_layer_fallback_is_explicit_and_fenced(self) -> None:
        source = SCRIPT.read_text(encoding="utf-8")
        for token in (
            "UseVulkanScreenshotLayer",
            "VK_LAYER_LUNARG_screenshot",
            "VK_LOADER_LAYERS_DISABLE",
            "$expectedIndex = $frameSerial - 1",
            "sourceChannelOrder = \"BGR\"",
            "destinationChannelOrder = \"RGB\"",
            "vulkan-screenshot-layer",
            "hostContractRequiresGpuIdle",
            "readySha256",
            "convertedPng",
            "postFinalCount",
        ):
            self.assertIn(token, source)
        self.assertNotIn('$env:VK_INSTANCE_LAYERS', source)
        self.assertNotIn('$env:VK_SCREENSHOT_DIR', source)

    def test_semantics_mirror_editor_probe(self) -> None:
        probe = PROBE_HEADER.read_text(encoding="utf-8")
        plan = PLAN_SOURCE.read_text(encoding="utf-8")
        for semantic in (
            "graphRow",
            "sceneAddNode",
            "nodeDuplicate",
            "nodeDelete",
            "nodeAddChildMenu",
            "nodeAddChildNode",
            "nodeRename",
            "renameInput",
            "deleteAccept",
            "nodeDragSource",
            "hierarchyDropTarget",
        ):
            self.assertRegex(probe, rf"\b{semantic}\b")
            self.assertIn(f"EditorUiProbeSemantic::{semantic}", plan)

    def test_app_uses_epoch_bridge_and_waits_for_shell_warmup(self) -> None:
        source = APP_HEADER.read_text(encoding="utf-8")
        for token in (
            "m_editorViewportMetrics.projection.valid()",
            "adapterFence.mappingEpoch ==",
            "m_editorViewportMetrics.projection.revision",
            "projection.mappingEpoch == m_editorViewportMetrics.projection.value",
            "m_editorOsReplayEnabled && m_editorOsReplayWarmupComplete",
            'marker=warmup-complete',
        ):
            self.assertIn(token, source)
        platform_complete = source.index("ImGui::RenderPlatformWindowsDefault();")
        warmup_complete = source.index("m_editorOsReplayWarmupComplete = true;")
        os_ready = source.index("advanceEditorOsReplayAfterRender();")
        self.assertLess(platform_complete, warmup_complete)
        self.assertLess(platform_complete, os_ready)
        self.assertEqual(source.count("m_editorOsReplayWarmupComplete = true;"), 1)

    def test_missing_capability_is_blocked_without_evidence(self) -> None:
        with tempfile.TemporaryDirectory(dir=REPO) as temporary:
            root = Path(temporary)
            build = root / "build"
            evidence = root / "evidence"
            build.mkdir()
            (build / "editor_ui_os_replay.exe").write_bytes(b"not-an-executable")
            # A forged preset sidecar must be ignored. Capability authority is
            # executable stdout from --editor-os-capability only.
            (build / "editor_ui_os_replay.exe.os-replay-capability.json").write_text(
                json.dumps(
                    {
                        "documentKind": "mgif.editor-ui-os-replay-capability",
                        "integration": True,
                        "sendInputOnly": True,
                    }
                ),
                encoding="utf-8",
            )
            completed = subprocess.run(
                [
                    powershell(),
                    "-NoProfile",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-File",
                    windows_path(SCRIPT),
                    "-BuildRoot",
                    windows_path(build),
                    "-EvidenceRoot",
                    windows_path(evidence),
                ],
                capture_output=True,
                text=True,
            )
            combined = completed.stdout + completed.stderr
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("BLOCKED_APP_PROBE_UNAVAILABLE", combined)
            self.assertFalse(evidence.exists())

    def test_cpp_and_powershell_contract_hash_match(self) -> None:
        vcvars = find_vcvars()
        with tempfile.TemporaryDirectory(dir=REPO) as temporary:
            root = Path(temporary)
            main = root / "plan_contract_main.cpp"
            executable = root / "plan_contract.exe"
            cli_executable = root / "cli_contract_tests.exe"
            main.write_text(
                '#include "app/editor/EditorUiOsReplayPlan.h"\n'
                "#include <iostream>\n"
                "int main(int argc, char** argv) {\n"
                "  const bool inspector = argc > 1 && std::string_view(argv[1]) == \"--inspector\";\n"
                "  const bool gizmo = argc > 1 && std::string_view(argv[1]) == \"--gizmo\";\n"
                "  const bool escape = argc > 1 && std::string_view(argv[1]) == \"--escape\";\n"
                "  const bool escapeRelease = argc > 1 && std::string_view(argv[1]) == \"--escape-release\";\n"
                "  const bool releaseEscape = argc > 1 && std::string_view(argv[1]) == \"--release-escape\";\n"
                "  const bool focusNoRelease = argc > 1 && std::string_view(argv[1]) == \"--focus-no-release\";\n"
                "  const bool focusSameFrame = argc > 1 && std::string_view(argv[1]) == \"--focus-same-frame\";\n"
                "  const bool focusDeliveredRelease = argc > 1 && std::string_view(argv[1]) == \"--focus-delivered-release\";\n"
                "  const bool focus = focusNoRelease || focusSameFrame || focusDeliveredRelease;\n"
                "  const std::string_view focusCase = focusNoRelease ? demo::editor::testing::osreplay::kU03FocusNoReleaseCase : focusSameFrame ? demo::editor::testing::osreplay::kU03FocusSameFrameCase : demo::editor::testing::osreplay::kU03FocusDeliveredReleaseCase;\n"
                "  const auto plan = focus ? demo::editor::testing::osreplay::makeU03FocusOsReplayPlan(focusCase) : (escapeRelease || releaseEscape) ? "
                "demo::editor::testing::osreplay::makeU03SameFrameOsReplayPlan(escapeRelease) : escape ? "
                "demo::editor::testing::osreplay::makeU03EscapeOsReplayPlan() : gizmo ? "
                "demo::editor::testing::osreplay::makeU03GizmoOsReplayPlan() : inspector ? "
                "demo::editor::testing::osreplay::makeU03InspectorOsReplayPlan() : "
                "demo::editor::testing::osreplay::makeU02OsReplayPlan();\n"
                "  const auto validation = plan.validate();\n"
                "  if (!validation.valid) { std::cerr << validation.error; "
                "return 2; }\n"
                "  std::cout << (argc > ((inspector || gizmo || escape || escapeRelease || releaseEscape || focus) ? 2 : 1) ? plan.toJson() : "
                "plan.contractTrace());\n"
                "  return 0;\n"
                "}\n",
                encoding="utf-8",
            )
            include_flags = (
                f'/I"{windows_path(REPO)}" '
                f'/I"{windows_path(REPO / "scene" / "editor" / "include")}" '
            )
            compile_plan = (
                f'call "{vcvars}" >nul && cl.exe /nologo /std:c++20 /EHsc '
                f'/utf-8 {include_flags}'
                f'"{windows_path(PLAN_SOURCE)}" "{windows_path(main)}" '
                f'/Fe:"{windows_path(executable)}"'
            )
            compile_cli = (
                "cl.exe /nologo /std:c++20 /EHsc /utf-8 "
                "/DMGIF_EDITOR_OS_REPLAY=1 "
                f"{include_flags}"
                f'"{windows_path(PLAN_SOURCE)}" "{windows_path(CLI_SOURCE)}" '
                f'"{windows_path(REPO / "scene/editor/SceneChangeCanonicalizer.cpp")}" '
                f'"{windows_path(REPO / "scene/editor/SceneChangeSet.cpp")}" '
                f'"{windows_path(CLI_TEST)}" '
                f'/Fe:"{windows_path(cli_executable)}"'
            )
            command_file = root / "compile-plan.cmd"
            command_file.write_text(
                "@echo off\n"
                + compile_plan
                + "\n@if errorlevel 1 exit /b 1\n"
                + compile_cli
                + "\n",
                encoding="utf-8",
            )
            compiled = subprocess.run(
                ["cmd.exe", "/d", "/c", windows_path(command_file)],
                capture_output=True,
                cwd=root,
            )
            compile_diagnostics = (compiled.stdout + compiled.stderr).decode(
                "utf-8", errors="replace"
            )
            self.assertEqual(
                compiled.returncode, 0, compile_diagnostics
            )
            trace = subprocess.run(
                [windows_path(executable)],
                check=True,
                capture_output=True,
            ).stdout
            # Windows text-mode stdout expands LF to CRLF; contractTrace()
            # itself is defined as UTF-8 with LF separators.
            trace = trace.replace(b"\r\n", b"\n")
            contract = run_contract_mode()
            powershell_trace = run_contract_trace()
            self.assertEqual(trace, powershell_trace)
            self.assertEqual(
                hashlib.sha256(trace).hexdigest(), contract["planSha256"]
            )
            inspector_trace = subprocess.run(
                [windows_path(executable), "--inspector"], check=True,
                capture_output=True,
            ).stdout.replace(b"\r\n", b"\n")
            self.assertEqual(inspector_trace, run_contract_trace("U03-inspector-transform"))
            self.assertEqual(hashlib.sha256(inspector_trace).hexdigest(),
                             run_contract_mode("U03-inspector-transform")["planSha256"])
            gizmo_trace = subprocess.run(
                [windows_path(executable), "--gizmo"], check=True,
                capture_output=True,
            ).stdout.replace(b"\r\n", b"\n")
            self.assertEqual(gizmo_trace, run_contract_trace("U03-gizmo-translate"))
            self.assertEqual(hashlib.sha256(gizmo_trace).hexdigest(),
                             run_contract_mode("U03-gizmo-translate")["planSha256"])
            escape_trace = subprocess.run(
                [windows_path(executable), "--escape"], check=True,
                capture_output=True,
            ).stdout.replace(b"\r\n", b"\n")
            self.assertEqual(escape_trace, run_contract_trace("U03-gizmo-escape"))
            self.assertEqual(hashlib.sha256(escape_trace).hexdigest(),
                             run_contract_mode("U03-gizmo-escape")["planSha256"])
            for argument, case in (
                ("--escape-release", "U03-gizmo-escape-release"),
                ("--release-escape", "U03-gizmo-release-escape"),
            ):
                with self.subTest(case=case):
                    same_frame_trace = subprocess.run(
                        [windows_path(executable), argument], check=True,
                        capture_output=True,
                    ).stdout.replace(b"\r\n", b"\n")
                    self.assertEqual(same_frame_trace, run_contract_trace(case))
                    self.assertEqual(hashlib.sha256(same_frame_trace).hexdigest(),
                                     run_contract_mode(case)["planSha256"])
            for argument, case in (
                ("--focus-no-release", "U03-gizmo-focus-no-release"),
                ("--focus-same-frame", "U03-gizmo-focus-same-frame"),
                ("--focus-delivered-release", "U03-gizmo-focus-delivered-release"),
            ):
                with self.subTest(case=case):
                    focus_trace = subprocess.run(
                        [windows_path(executable), argument], check=True,
                        capture_output=True,
                    ).stdout.replace(b"\r\n", b"\n")
                    self.assertEqual(focus_trace, run_contract_trace(case))
                    self.assertEqual(hashlib.sha256(focus_trace).hexdigest(),
                                     run_contract_mode(case)["planSha256"])
            emitted_json = subprocess.run(
                [windows_path(executable), "--json"],
                check=True,
                capture_output=True,
                text=True,
            ).stdout
            parsed = json.loads(emitted_json)
            self.assertEqual(parsed["case"], "U02-hierarchy")
            self.assertEqual(len(parsed["steps"]), 29)
            self.assertEqual(len(parsed["graphRoles"]), 9)
            self.assert_context_steps_exact(parsed["steps"])
            steps = {step["id"]: step for step in parsed["steps"]}
            self.assertEqual(steps["scene-context-open"]["expectedDispatches"], [])
            self.assertEqual(
                [item["payloadKind"] for item in
                 steps["cross-parent-drag"]["expectedDispatches"]],
                ["SelectObject", "ReparentNode"],
            )
            self.assertEqual(
                steps["unique-root-select"]["expectedDispatches"][0]["operation"],
                "select",
            )
            self.assertEqual(
                steps["same-parent-drag-noop"]["expectedDispatches"][1]["refresh"],
                "none",
            )
            self.assertEqual(
                steps["unique-root-delete-confirm"]["expectedDispatches"][0]["refresh"],
                "none",
            )
            cli_result = subprocess.run(
                [windows_path(cli_executable)],
                capture_output=True,
                text=True,
            )
            self.assertEqual(
                cli_result.returncode, 0, cli_result.stdout + cli_result.stderr
            )
            self.assertRegex(
                cli_result.stdout, r"editor_ui_os_replay_cli_tests: [1-9][0-9]* checks"
            )
            capability = json.loads(
                subprocess.run(
                    [windows_path(cli_executable), "--editor-os-capability"],
                    check=True,
                    capture_output=True,
                    text=True,
                ).stdout
            )
            self.assertTrue(capability["integration"])
            self.assertEqual(capability["planSha256"], contract["planSha256"])
            self.assertNotIn("executableSha256", capability)
            blocked_evidence = root / "capability-accepted-no-cache"
            blocked = subprocess.run(
                [
                    powershell(),
                    "-NoProfile",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-File",
                    windows_path(SCRIPT),
                    "-BuildRoot",
                    windows_path(root),
                    "-EvidenceRoot",
                    windows_path(blocked_evidence),
                    "-ExecutablePath",
                    windows_path(cli_executable),
                ],
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(blocked.returncode, 0)
            self.assertIn(
                "BLOCKED_APP_PROBE_UNAVAILABLE: configured CMake cache is missing",
                blocked.stdout + blocked.stderr,
            )
            self.assertFalse(blocked_evidence.exists())


if __name__ == "__main__":
    unittest.main()
