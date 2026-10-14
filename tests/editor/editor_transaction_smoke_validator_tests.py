#!/usr/bin/env python3
"""Adversarial tests for the independent P5 transaction smoke validator."""

from __future__ import annotations

import copy
import hashlib
import json
import re
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools"))

from editor_transaction_smoke import (  # noqa: E402
    AUTHORITATIVE_MARKERS,
    CASE_SPECS,
    MODEL_IDENTITY,
    SUPPLEMENTARY_MARKERS,
    ValidationError,
    validate_case,
    validate_saved_artifact,
    validate_summary,
    write_case_result,
    write_summary,
)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def glb_bytes(document: dict[str, Any], binary: bytes = b"\x00\x01\x02\x03") -> bytes:
    json_chunk = json.dumps(document, separators=(",", ":")).encode("utf-8")
    json_chunk += b" " * ((4 - len(json_chunk) % 4) % 4)
    binary += b"\x00" * ((4 - len(binary) % 4) % 4)
    chunks = struct.pack("<II", len(json_chunk), 0x4E4F534A) + json_chunk
    chunks += struct.pack("<II", len(binary), 0x004E4942) + binary
    return struct.pack("<4sII", b"glTF", 2, 12 + len(chunks)) + chunks


class EditorTransactionSmokeValidatorTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name)
        self.executable = self.root / "Demo.exe"
        self.executable.write_bytes(b"stable Demo executable identity")
        self.executable_hash = sha256_bytes(self.executable.read_bytes())
        self.source_root = self.root / "source-runtime"
        self.source_directory = self.source_root / "resources" / "GLTF_Sponza"
        self.source_directory.mkdir(parents=True)
        (self.source_directory / "mesh.bin").write_bytes(b"immutable source mesh")
        (self.source_directory / "sponza_sdf.bin").write_bytes(b"immutable source sdf")
        (self.source_directory / "sponza.gltf").write_text(
            json.dumps(
                {
                    "asset": {"version": "2.0"},
                    "buffers": [{"uri": "mesh.bin", "byteLength": 21}],
                },
                separators=(",", ":"),
            ),
            encoding="utf-8",
        )
        self.source_manifest = self.make_source_manifest()
        self.cases: list[dict[str, Any]] = [self.make_case(spec, index) for index, spec in enumerate(CASE_SPECS)]

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def make_source_manifest(self) -> list[dict[str, Any]]:
        records: list[dict[str, Any]] = []
        for path in sorted(self.source_directory.iterdir()):
            relative = path.relative_to(self.source_root).as_posix()
            records.append(
                {
                    "relative_path": relative,
                    "size_bytes": path.stat().st_size,
                    "sha256": sha256_bytes(path.read_bytes()),
                    "role": "model" if relative == MODEL_IDENTITY else "runtime-dependency",
                }
            )
        return records

    def make_artifact(self, case_dir: Path, index: int) -> Path:
        case_dir.mkdir(parents=True, exist_ok=True)
        path = case_dir / "editor-transaction.glb"
        path.write_bytes(glb_bytes({"asset": {"version": "2.0"}}))
        return path

    def make_logs(self, spec: Any, artifact_size: int, index: int) -> str:
        baseline_revision = 10 + index * 4
        undo_revision = baseline_revision + 2
        redo_revision = baseline_revision + 3
        undo_digest = "a" * 64
        redo_digest = "b" * 64
        pose = (
            "current_pos=(8.000000,1.500000,1.000000) "
            "current_yaw_pitch=(180.000000,0.000000) "
            "previous_pos=(8.000000,1.500000,1.000000) "
            "previous_yaw_pitch=(180.000000,0.000000)"
        )
        lines = [
            f"[12:00:00.000] [CSM_AUTOMATION] marker=config mode={spec.mode} fixed_dt=0.016666668 warmup=8 motion=24 hold=8 no_ui=1 no_post={int(spec.no_post)} no_ddgi={int(spec.no_ddgi)} taa={int(spec.taa)} auto_exit=1 capture_sync=0 capture_control=0 capture_sync_timeout_ms=30000 capture_sync_dir=",
            f"[12:00:01.000] [CSM_AUTOMATION] marker=scene-ready mode={spec.mode} model={MODEL_IDENTITY}",
            f"[12:00:01.001] [CSM_AUTOMATION] marker=start mode={spec.mode} frame=0",
            f"[12:00:01.002] [CSM_AUTOMATION] marker=automation-start mode={spec.mode} transaction=rename-undo-redo-save frame_budget=40",
            f"[12:00:01.003] [CSM_AUTOMATION] marker=undo-observed mode={spec.mode} revision={undo_revision} undo=0 redo=1 digest={undo_digest} renderer_revision={undo_revision}",
            f"[12:00:01.004] [CSM_AUTOMATION] marker=redo-observed mode={spec.mode} revision={redo_revision} undo=1 redo=0 digest={redo_digest} renderer_revision={redo_revision}",
            f"[12:00:01.005] [CSM_AUTOMATION] marker=save-complete mode={spec.mode} revision={redo_revision} clean_revision={redo_revision} bytes={artifact_size} reload_valid=1 semantic_equal=1",
            f"[12:00:01.031] [CSM_AUTOMATION] marker=last-moving mode={spec.mode} frame=31 {pose}",
            f"[12:00:01.032] [CSM_AUTOMATION] marker=first-still mode={spec.mode} frame=32 {pose}",
            f"[12:00:01.039] [CSM_AUTOMATION] marker=settled mode={spec.mode} frame=39 {pose}",
            f"[12:00:01.040] [CSM_AUTOMATION] marker=complete mode={spec.mode} frames=40",
            f"[12:00:01.041] [CSM_AUTOMATION] marker=frame-budget-complete mode={spec.mode} frames=40",
            f"[12:00:01.042] [CSM_AUTOMATION] marker=automation-complete mode={spec.mode} frames=40 auto_exit=1",
        ]
        if spec.require_sdf:
            lines.insert(1, "[12:00:00.500] Loaded DDGI mesh SDF: resources/GLTF_Sponza/sponza_sdf.bin")
        return "\n".join(lines) + "\n"

    def make_case(self, spec: Any, index: int) -> dict[str, Any]:
        case_dir = self.root / spec.name
        artifact = self.make_artifact(case_dir, index)
        artifact_bytes = artifact.read_bytes()
        logs = self.make_logs(spec, len(artifact_bytes), index)
        baseline_revision = 10 + index * 4
        baseline_epoch = 20 + index * 4
        return {
            "case": spec.name,
            "mode": spec.mode,
            "no_post": spec.no_post,
            "no_ddgi": spec.no_ddgi,
            "taa": spec.taa,
            "timed_out": False,
            "exit_code": 0,
            "stdout": logs,
            "stderr": "",
            "case_dir": str(case_dir),
            "artifact_path": str(artifact),
            "save_target": str(artifact),
            "artifact_sha256": sha256_bytes(artifact_bytes),
            "artifact_size_bytes": len(artifact_bytes),
            "demo_path": str(self.executable),
            "expected_demo_sha256": self.executable_hash,
            "demo_sha256": self.executable_hash,
            "demo_sha256_before": self.executable_hash,
            "demo_sha256_after": self.executable_hash,
            "transaction_progression": {
                "revisions": {
                    "baseline": baseline_revision,
                    "mutation": baseline_revision + 1,
                    "undo": baseline_revision + 2,
                    "redo": baseline_revision + 3,
                },
                "mapping_epochs": {
                    "baseline": baseline_epoch,
                    "mutation": baseline_epoch + 1,
                    "undo": baseline_epoch + 2,
                    "redo": baseline_epoch + 3,
                },
            },
            "source_manifest": {
                "source_root": str(self.source_root),
                "before": copy.deepcopy(self.source_manifest),
                "after": copy.deepcopy(self.source_manifest),
            },
            "ddgi_sdf_marker_count": int(spec.require_sdf),
            "strict_scene_ready_count": 1,
            "rendered_frames": 40,
            "auto_exit_successful": True,
            "literal_marker_order_valid": True,
            "documented_marker_contract_passed": True,
            "literal_marker_counts": {
                "sceneReady": 1,
                "automationStart": 1,
                "undoObserved": 1,
                "redoObserved": 1,
                "saveComplete": 1,
                "frameBudgetComplete": 1,
                "automationComplete": 1,
            },
            "passed": True,
        }

    def make_summary(self, *, cases: list[dict[str, Any]] | None = None) -> dict[str, Any]:
        selected = copy.deepcopy(self.cases if cases is None else cases)
        return {
            "schema": "mgif.p5.editor-transaction.summary.v1",
            "evidence_root": str(self.root),
            "identity": {
                "demo_path": str(self.executable),
                "demo_sha256": self.executable_hash,
                "demo_sha256_before": self.executable_hash,
                "demo_sha256_after": self.executable_hash,
            },
            "status": "pass",
            "passed": 5,
            "failed": 0,
            "total": 5,
            "cases": selected,
        }

    def assert_rejects(self, summary: dict[str, Any], message: str = "") -> None:
        with self.assertRaises(ValidationError, msg=message):
            validate_summary(summary, evidence_root=self.root)

    def case_with_log(self, index: int, transform: Any) -> dict[str, Any]:
        cases = copy.deepcopy(self.cases)
        transform(cases[index])
        return self.make_summary(cases=cases)

    def replace_line(self, case: dict[str, Any], old: str, new: str) -> None:
        self.assertIn(old, case["stdout"])
        case["stdout"] = case["stdout"].replace(old, new, 1)

    def replace_marker_field(self, case: dict[str, Any], marker: str, old: str, new: str) -> None:
        lines = case["stdout"].splitlines()
        position = next(index for index, line in enumerate(lines) if f"marker={marker} " in line)
        self.assertIn(old, lines[position])
        lines[position] = lines[position].replace(old, new, 1)
        case["stdout"] = "\n".join(lines) + "\n"

    def remove_source_manifest_path(self, case: dict[str, Any], relative_path: str) -> None:
        for snapshot in ("before", "after"):
            records = case["source_manifest"][snapshot]
            position = next(
                index
                for index, record in enumerate(records)
                if record["relative_path"] == relative_path
            )
            records.pop(position)

    def write_summary_files(self, summary: dict[str, Any]) -> Path:
        result_refs: list[dict[str, str]] = []
        for case in summary["cases"]:
            case_path = self.root / case["case"] / "result.json"
            case_path.write_text(json.dumps(case, indent=2), encoding="utf-8")
            result_refs.append({"result_path": str(case_path.relative_to(self.root)).replace("\\", "/")})
        on_disk = copy.deepcopy(summary)
        on_disk["cases"] = result_refs
        summary_path = self.root / "summary.json"
        summary_path.write_text(json.dumps(on_disk, indent=2), encoding="utf-8")
        return summary_path

    def test_accepts_complete_five_case_set_and_revalidates_written_summary(self) -> None:
        result = validate_summary(self.make_summary(), evidence_root=self.root, repo_root=REPO_ROOT)
        self.assertEqual(result["status"], "pass")
        self.assertEqual(result["passed"], 5)
        self.assertEqual(len(result["cases"]), 5)
        self.assertTrue(all(row["artifact"]["format"] == "glb" for row in result["cases"]))
        # All five GLBs intentionally have identical bytes; path uniqueness,
        # rather than content uniqueness, is the isolation gate.
        self.assertEqual(len({row["artifact"]["sha256"] for row in result["cases"]}), 1)

        summary_path = self.write_summary_files(self.make_summary())
        disk_result = validate_summary(summary_path, repo_root=REPO_ROOT)
        self.assertEqual(disk_result["passed"], 5)
        self.assertEqual(disk_result["identity"]["sha256"], self.executable_hash)

        # The validator's own closed case and summary values are independently
        # re-loadable; a future runner can write these without a second schema.
        derived_case_path = self.root / "derived-case.json"
        derived_case_path.write_text(json.dumps(result["cases"][0], indent=2), encoding="utf-8")
        derived_case = validate_case(derived_case_path, evidence_root=self.root)
        self.assertEqual(derived_case, result["cases"][0])
        derived_summary_path = self.root / "derived-summary.json"
        derived_summary_path.write_text(json.dumps(result, indent=2), encoding="utf-8")
        self.assertEqual(validate_summary(derived_summary_path), result)
        self.assertEqual(validate_summary(result["cases"], evidence_root=self.root), result)

        written_case = write_case_result(self.root / "writer-case.json", self.cases[0], evidence_root=self.root)
        self.assertEqual(written_case, result["cases"][0])
        written_summary = write_summary(
            self.root / "writer-summary.json",
            self.make_summary(),
            evidence_root=self.root,
            repo_root=REPO_ROOT,
        )
        self.assertEqual(written_summary, result)

    def test_rejects_each_missing_authoritative_marker(self) -> None:
        for marker in AUTHORITATIVE_MARKERS:
            with self.subTest(marker=marker):
                summary = self.make_summary()
                line = next(line for line in summary["cases"][0]["stdout"].splitlines() if f"marker={marker} " in line)
                summary["cases"][0]["stdout"] = summary["cases"][0]["stdout"].replace(line + "\n", "", 1)
                self.assert_rejects(summary, f"missing {marker}")

    def test_rejects_each_duplicate_authoritative_marker(self) -> None:
        first = self.cases[0]
        lines = first["stdout"].splitlines()
        for marker in AUTHORITATIVE_MARKERS:
            with self.subTest(marker=marker):
                summary = self.make_summary()
                line = next(line for line in lines if f"marker={marker} " in line)
                summary["cases"][0]["stdout"] += line + "\n"
                self.assert_rejects(summary, f"duplicate {marker}")

    def test_rejects_every_pairwise_authoritative_reorder(self) -> None:
        marker_lines = {
            marker: next(line for line in self.cases[0]["stdout"].splitlines() if f"marker={marker} " in line)
            for marker in AUTHORITATIVE_MARKERS
        }
        for left_index in range(len(AUTHORITATIVE_MARKERS)):
            for right_index in range(left_index + 1, len(AUTHORITATIVE_MARKERS)):
                with self.subTest(
                    kind="adjacent" if right_index == left_index + 1 else "non-adjacent",
                    left=AUTHORITATIVE_MARKERS[left_index],
                    right=AUTHORITATIVE_MARKERS[right_index],
                ):
                    summary = self.make_summary()
                    lines = summary["cases"][0]["stdout"].splitlines()
                    left = marker_lines[AUTHORITATIVE_MARKERS[left_index]]
                    right = marker_lines[AUTHORITATIVE_MARKERS[right_index]]
                    left_position = lines.index(left)
                    right_position = lines.index(right)
                    lines[left_position], lines[right_position] = lines[right_position], lines[left_position]
                    summary["cases"][0]["stdout"] = "\n".join(lines) + "\n"
                    self.assert_rejects(summary)

    def test_rejects_cross_ledger_reorders_and_nonproduction_payloads(self) -> None:
        swaps = (
            ("marker=start ", "marker=automation-start "),
            ("marker=save-complete ", "marker=last-moving "),
            ("marker=complete ", "marker=frame-budget-complete "),
        )
        for left_token, right_token in swaps:
            with self.subTest(left=left_token, right=right_token):
                summary = self.make_summary()
                lines = summary["cases"][0]["stdout"].splitlines()
                left = next(index for index, line in enumerate(lines) if left_token in line)
                right = next(index for index, line in enumerate(lines) if right_token in line)
                lines[left], lines[right] = lines[right], lines[left]
                summary["cases"][0]["stdout"] = "\n".join(lines) + "\n"
                self.assert_rejects(summary)

        malformed = (
            (" capture_sync=0", ""),
            (" capture_control=0", ""),
            (" capture_sync_timeout_ms=30000", ""),
            (" capture_sync_dir=", ""),
            (" current_pos=(8.000000,1.500000,1.000000)", ""),
            (" current_yaw_pitch=(180.000000,0.000000)", ""),
            (" previous_pos=(8.000000,1.500000,1.000000)", ""),
            (" previous_yaw_pitch=(180.000000,0.000000)", ""),
        )
        for old, new in malformed:
            with self.subTest(missing_field=old):
                self.assert_rejects(
                    self.case_with_log(0, lambda case, old=old, new=new: self.replace_line(case, old, new))
                )

        summary = self.make_summary()
        summary["cases"][0]["stdout"] = summary["cases"][0]["stdout"].replace(
            "[12:00:01.002] [CSM_AUTOMATION] marker=automation-start",
            "not-a-production-prefix [CSM_AUTOMATION] marker=automation-start",
            1,
        )
        self.assert_rejects(summary, "invalid timestamp prefix")

    def test_rejects_aliases_substrings_and_malformed_complete_lines(self) -> None:
        replacements = (
            ("marker=automation-start ", "marker=start "),
            ("marker=undo-observed ", "marker=undo-observed-extra "),
            ("marker=automation-complete ", "marker=automation-complete-extra "),
        )
        for old, new in replacements:
            with self.subTest(alias=new):
                summary = self.make_summary()
                summary["cases"][0]["stdout"] = summary["cases"][0]["stdout"].replace(old, new, 1)
                self.assert_rejects(summary)
        with self.subTest(alias="trailing text"):
            summary = self.make_summary()
            summary["cases"][0]["stdout"] = summary["cases"][0]["stdout"].replace(
                "frame_budget=40\n", "frame_budget=40 trailing\n", 1
            )
            self.assert_rejects(summary)

        with self.subTest(alias="legacy transaction prefix alongside valid evidence"):
            summary = self.make_summary()
            summary["cases"][0]["stdout"] += (
                "[12:00:02.000] [EDITOR_TRANSACTION] marker=automation-start "
                "mode=csm-translate-stop transaction=rename-undo-redo-save frame_budget=40\n"
            )
            self.assert_rejects(summary)

    def test_rejects_wrong_model_mode_frame_revision_history_and_save_fields(self) -> None:
        artifact_size = self.cases[0]["artifact_size_bytes"]
        mutations = (
            ("model", lambda case: self.replace_line(case, MODEL_IDENTITY, "resources/GLTF_Sponza/Sponza.gltf")),
            ("mode", lambda case: case.__setitem__("stdout", case["stdout"].replace("csm-translate-stop", "csm-rotate-stop"))),
            ("start frame budget", lambda case: self.replace_marker_field(case, "automation-start", "frame_budget=40", "frame_budget=41")),
            ("frame completion", lambda case: self.replace_marker_field(case, "frame-budget-complete", "frames=40", "frames=39")),
            ("final frame count", lambda case: self.replace_marker_field(case, "automation-complete", "frames=40", "frames=41")),
            ("auto exit", lambda case: self.replace_marker_field(case, "automation-complete", "auto_exit=1", "auto_exit=0")),
            ("undo renderer revision", lambda case: self.replace_marker_field(case, "undo-observed", "renderer_revision=12", "renderer_revision=11")),
            ("redo renderer revision", lambda case: self.replace_marker_field(case, "redo-observed", "renderer_revision=13", "renderer_revision=12")),
            ("redo revision gap", lambda case: self.replace_marker_field(case, "redo-observed", "revision=13", "revision=15")),
            ("undo history", lambda case: self.replace_marker_field(case, "undo-observed", "undo=0 redo=1", "undo=1 redo=0")),
            ("redo history", lambda case: self.replace_marker_field(case, "redo-observed", "undo=1 redo=0", "undo=0 redo=1")),
            ("clean revision", lambda case: self.replace_marker_field(case, "save-complete", "clean_revision=13", "clean_revision=12")),
            ("save revision", lambda case: self.replace_marker_field(case, "save-complete", "revision=13", "revision=12")),
            ("save bytes", lambda case: self.replace_marker_field(case, "save-complete", f"bytes={artifact_size}", f"bytes={artifact_size + 1}")),
            ("reload", lambda case: self.replace_marker_field(case, "save-complete", "reload_valid=1", "reload_valid=0")),
            ("semantic", lambda case: self.replace_marker_field(case, "save-complete", "semantic_equal=1", "semantic_equal=0")),
            ("digest restoration", lambda case: self.replace_marker_field(case, "redo-observed", "digest=" + "b" * 64, "digest=" + "a" * 64)),
        )
        for name, mutation in mutations:
            with self.subTest(field=name):
                summary = self.case_with_log(0, mutation)
                self.assert_rejects(summary, name)

    def test_rejects_missing_empty_malformed_and_non_gltf_artifacts(self) -> None:
        variants = (
            ("missing", lambda path: path.unlink()),
            ("empty", lambda path: path.write_bytes(b"")),
            ("invalid utf8", lambda path: path.write_bytes(b"\xff\xfe\xfd")),
            ("invalid json", lambda path: path.write_text("{", encoding="utf-8")),
            ("non-object", lambda path: path.write_text("[]", encoding="utf-8")),
            ("glTF 1", lambda path: path.write_text(json.dumps({"asset": {"version": "1.0"}}), encoding="utf-8")),
        )
        for name, mutate in variants:
            with self.subTest(artifact=name):
                case_dir = self.root / f"artifact-{name.replace(' ', '-') }"
                case_dir.mkdir()
                path = case_dir / "output.gltf"
                path.write_text(json.dumps({"asset": {"version": "2.0"}}), encoding="utf-8")
                if name == "missing":
                    path.unlink()
                else:
                    mutate(path)
                with self.assertRaises(ValidationError):
                    validate_saved_artifact(path, case_dir=case_dir)

    def test_rejects_unsafe_or_missing_external_dependencies(self) -> None:
        unsafe_uris = (
            ("traversal", "../outside.bin"),
            ("encoded-traversal", "%2e%2e/outside.bin"),
            ("absolute", "/outside.bin"),
            ("drive", "C:/outside.bin"),
            ("backslash", "..\\outside.bin"),
            ("file-scheme", "file:///outside.bin"),
            ("http-uri", "https://example.invalid/mesh.bin"),
            ("query", "mesh.bin?cache=1"),
            ("missing", "not-present.bin"),
        )
        for name, uri in unsafe_uris:
            with self.subTest(uri=uri):
                case_dir = self.root / f"dependency-{name}"
                case_dir.mkdir()
                artifact = case_dir / "output.gltf"
                artifact.write_text(
                    json.dumps({"asset": {"version": "2.0"}, "buffers": [{"uri": uri}]}),
                    encoding="utf-8",
                )
                with self.assertRaises(ValidationError):
                    validate_saved_artifact(artifact, case_dir=case_dir)

    def test_rejects_artifact_hash_size_and_requested_path_mismatches(self) -> None:
        mutations = (
            ("artifact hash", lambda case: case.__setitem__("artifact_sha256", "f" * 64)),
            ("artifact size", lambda case: case.__setitem__("artifact_size_bytes", case["artifact_size_bytes"] + 1)),
            ("missing requested path", lambda case: case.pop("save_target")),
            (
                "wrong requested path",
                lambda case: case.__setitem__("save_target", str(Path(case["case_dir"]) / "different.gltf")),
            ),
        )
        for name, mutation in mutations:
            with self.subTest(field=name):
                self.assert_rejects(self.case_with_log(0, mutation))

    def test_accepts_and_rejects_glb_header_variants(self) -> None:
        case_dir = self.root / "glb-check"
        case_dir.mkdir()
        good = case_dir / "good.glb"
        good.write_bytes(glb_bytes({"asset": {"version": "2.0"}}))
        self.assertEqual(validate_saved_artifact(good, case_dir=case_dir)["format"], "glb")
        variants = (
            ("magic", lambda data: b"BAD!" + data[4:]),
            ("version", lambda data: data[:4] + struct.pack("<I", 1) + data[8:]),
            ("length", lambda data: data[:8] + struct.pack("<I", len(data) + 4) + data[12:]),
        )
        original = good.read_bytes()
        for name, mutate in variants:
            with self.subTest(variant=name):
                bad = case_dir / f"{name}.glb"
                bad.write_bytes(mutate(original))
                with self.assertRaises(ValidationError):
                    validate_saved_artifact(bad, case_dir=case_dir)

    def test_p5_case_rejects_gltf_and_artifact_aliases(self) -> None:
        summary = self.make_summary()
        case = summary["cases"][0]
        case_dir = Path(case["case_dir"])
        gltf = case_dir / "editor-transaction.gltf"
        gltf.write_text(json.dumps({"asset": {"version": "2.0"}}), encoding="utf-8")
        gltf_bytes = gltf.read_bytes()
        case["artifact_path"] = str(gltf)
        case["save_target"] = str(gltf)
        case["artifact_sha256"] = sha256_bytes(gltf_bytes)
        case["artifact_size_bytes"] = len(gltf_bytes)
        self.replace_marker_field(
            case,
            "save-complete",
            f"bytes={self.cases[0]['artifact_size_bytes']}",
            f"bytes={len(gltf_bytes)}",
        )
        self.assert_rejects(summary, "P5 output must be GLB")

        alias_dir = self.root / "artifact-alias"
        alias_dir.mkdir()
        target = alias_dir / "target.glb"
        target.write_bytes(glb_bytes({"asset": {"version": "2.0"}}))
        alias = alias_dir / "alias.glb"
        try:
            alias.symlink_to(target)
        except (OSError, NotImplementedError):
            return
        with self.assertRaises(ValidationError):
            validate_saved_artifact(alias, case_dir=alias_dir, require_glb=True)

    def test_rejects_timeout_nonzero_hash_drift_and_failure_regex(self) -> None:
        mutations = (
            ("timeout", lambda case: case.__setitem__("timed_out", True)),
            ("exit", lambda case: case.__setitem__("exit_code", 7)),
            ("hash drift", lambda case: case.__setitem__("demo_sha256_after", "f" * 64)),
            ("missing before hash", lambda case: case.pop("demo_sha256_before")),
            ("missing after hash", lambda case: case.pop("demo_sha256_after")),
            ("VUID", lambda case: case.__setitem__("stderr", "VUID-vkQueueSubmit-123")),
            ("SYNC", lambda case: case.__setitem__("stderr", "SYNC-HAZARD-WRITE-AFTER-READ")),
            ("validation", lambda case: case.__setitem__("stderr", "Vulkan validation WARNING/ sync")),
            ("failure regex", lambda case: case.__setitem__("stderr", "marker=automation-failed stage=save")),
        )
        for name, mutation in mutations:
            with self.subTest(reason=name):
                self.assert_rejects(self.case_with_log(0, mutation))

    def test_rejects_combined_output_that_hides_explicit_stderr(self) -> None:
        summary = self.make_summary()
        case = summary["cases"][0]
        case["combined_output"] = case.pop("stdout")
        case["stderr"] = "VUID-vkQueueSubmit-hidden\nSYNC-HAZARD-hidden\nmarker=automation-failed phase=save"
        self.assert_rejects(summary, "combined output must not bypass stderr")

        summary = self.make_summary()
        case = summary["cases"][0]
        case["combined_output"] = case.pop("stdout")
        stderr_path = self.root / "hidden.stderr.log"
        stderr_path.write_text("marker=automation-failed phase=save\n", encoding="utf-8")
        case["stderr_path"] = str(stderr_path)
        self.assert_rejects(summary, "combined output must not bypass stderr_path")

        summary = self.make_summary()
        case = summary["cases"][0]
        case["combined_output"] = case.pop("stdout")
        case["stdout"] = "independent stdout stream\n"
        self.assert_rejects(summary, "combined output must not be combined with stdout")

        summary = self.make_summary()
        case = summary["cases"][0]
        case["combined_output"] = case.pop("stdout")
        stdout_path = self.root / "hidden.stdout.log"
        stdout_path.write_text("independent stdout stream\n", encoding="utf-8")
        case["stdout_path"] = str(stdout_path)
        self.assert_rejects(summary, "combined output must not be combined with stdout_path")

    def test_rejects_frame_40_in_marker_and_telemetry_evidence(self) -> None:
        for evidence in (
            "[12:00:02.000] [CSM_AUTOMATION] marker=capture-continue mode=csm-translate-stop frame=40 phase=pre-render\n",
            "CSM_AUTOMATION_FRAME mode=csm-translate-stop boundary=settled frame=40\n",
            '{"frame":40,"phase":"render"}\n',
        ):
            with self.subTest(evidence=evidence):
                summary = self.make_summary()
                summary["cases"][0]["stdout"] += evidence
                self.assert_rejects(summary)
        summary = self.make_summary()
        summary["cases"][0]["frame_telemetry"] = [{"frame": 40, "status": "rendered"}]
        self.assert_rejects(summary, "structured frame telemetry cannot contain frame 40")

    def test_rejects_source_manifest_mutations_and_accepts_closed_manifest(self) -> None:
        # A valid manifest is already required by every positive fixture.
        result = validate_case(self.cases[0], evidence_root=self.root)
        self.assertEqual(result["source_manifest"]["before"], result["source_manifest"]["after"])
        mutations = (
            (
                "missing manifest",
                lambda case: case.pop("source_manifest"),
            ),
            (
                "changed digest",
                lambda case: case["source_manifest"]["after"][0].__setitem__("sha256", "f" * 64),
            ),
            (
                "removed entry",
                lambda case: case["source_manifest"]["after"].pop(),
            ),
            (
                "path escape",
                lambda case: case["source_manifest"]["after"][0].__setitem__("relative_path", "../sponza.gltf"),
            ),
            (
                "unnormalized path",
                lambda case: case["source_manifest"]["after"][0].__setitem__("relative_path", "resources/GLTF_Sponza/./mesh.bin"),
            ),
            (
                "missing runtime dependency",
                lambda case: self.remove_source_manifest_path(
                    case, "resources/GLTF_Sponza/mesh.bin"
                ),
            ),
            (
                "case collision",
                lambda case: case["source_manifest"]["after"].append(
                    {**case["source_manifest"]["after"][0], "relative_path": MODEL_IDENTITY.upper()}
                ),
            ),
        )
        for name, mutation in mutations:
            with self.subTest(manifest=name):
                self.assert_rejects(self.case_with_log(0, mutation), name)

        added_path = self.source_directory / "added.bin"
        added_path.write_bytes(b"added only after launch")
        try:
            summary = self.make_summary()
            summary["cases"][0]["source_manifest"]["after"].append(
                {
                    "relative_path": added_path.relative_to(self.source_root).as_posix(),
                    "size_bytes": added_path.stat().st_size,
                    "sha256": sha256_bytes(added_path.read_bytes()),
                    "role": "runtime-dependency",
                }
            )
            self.assert_rejects(summary, "added source entry")
        finally:
            added_path.unlink()

        dependency = self.source_directory / "mesh.bin"
        original = dependency.read_bytes()
        dependency.write_bytes(b"changed source dependency")
        try:
            self.assert_rejects(self.make_summary(), "final source rehash")
        finally:
            dependency.write_bytes(original)

        unlisted = self.source_directory / "unlisted-final-file.bin"
        unlisted.write_bytes(b"not represented in either claimed snapshot")
        try:
            self.assert_rejects(
                self.make_summary(),
                "unlisted file in final strict source tree",
            )
        finally:
            unlisted.unlink()

    def test_accepts_source_manifest_root_outside_evidence_and_rejects_aliases(self) -> None:
        with tempfile.TemporaryDirectory() as external_directory:
            external_root = Path(external_directory) / "source-runtime"
            external_source = external_root / "resources" / "GLTF_Sponza"
            external_source.mkdir(parents=True)
            for source in self.source_directory.iterdir():
                (external_source / source.name).write_bytes(source.read_bytes())

            external_manifest: list[dict[str, Any]] = []
            for path in sorted(external_source.iterdir()):
                relative = path.relative_to(external_root).as_posix()
                external_manifest.append(
                    {
                        "relative_path": relative,
                        "size_bytes": path.stat().st_size,
                        "sha256": sha256_bytes(path.read_bytes()),
                        "role": "model" if relative == MODEL_IDENTITY else "runtime-dependency",
                    }
                )

            summary = self.make_summary()
            for case in summary["cases"]:
                case["source_manifest"] = {
                    "source_root": str(external_root),
                    "before": copy.deepcopy(external_manifest),
                    "after": copy.deepcopy(external_manifest),
                }
            result = validate_summary(summary, evidence_root=self.root)
            self.assertEqual(
                Path(result["cases"][0]["source_manifest"]["source_root"]),
                external_root.resolve(),
            )

            alias = external_source / "mesh-alias.bin"
            try:
                alias.symlink_to(external_source / "mesh.bin")
            except (OSError, NotImplementedError):
                return
            alias_record = {
                "relative_path": alias.relative_to(external_root).as_posix(),
                "size_bytes": alias.stat().st_size,
                "sha256": sha256_bytes(alias.read_bytes()),
                "role": "runtime-dependency",
            }
            alias_summary = self.make_summary()
            for case in alias_summary["cases"]:
                case["source_manifest"] = {
                    "source_root": str(external_root),
                    "before": external_manifest + [copy.deepcopy(alias_record)],
                    "after": external_manifest + [copy.deepcopy(alias_record)],
                }
            self.assert_rejects(alias_summary, "source manifest alias entry")

            alias_directory = external_root / "resources" / "alias-directory"
            try:
                alias_directory.symlink_to(external_source, target_is_directory=True)
            except (OSError, NotImplementedError):
                return
            nested_alias_record = {
                "relative_path": "resources/alias-directory/mesh.bin",
                "size_bytes": (external_source / "mesh.bin").stat().st_size,
                "sha256": sha256_bytes((external_source / "mesh.bin").read_bytes()),
                "role": "runtime-dependency",
            }
            nested_alias_summary = self.make_summary()
            for case in nested_alias_summary["cases"]:
                case["source_manifest"] = {
                    "source_root": str(external_root),
                    "before": external_manifest + [copy.deepcopy(nested_alias_record)],
                    "after": external_manifest + [copy.deepcopy(nested_alias_record)],
                }
            self.assert_rejects(nested_alias_summary, "source manifest alias directory")

    def test_rejects_skipped_structured_revision_or_epoch_steps(self) -> None:
        mutations = (
            (
                "skipped mutation revision",
                lambda case: case["transaction_progression"]["revisions"].__setitem__("mutation", 12),
            ),
            (
                "skipped undo revision",
                lambda case: case["transaction_progression"]["revisions"].__setitem__("undo", 14),
            ),
            (
                "skipped redo revision",
                lambda case: case["transaction_progression"]["revisions"].__setitem__("redo", 16),
            ),
            (
                "skipped mapping epoch",
                lambda case: case["transaction_progression"]["mapping_epochs"].__setitem__("redo", 25),
            ),
        )
        for name, mutation in mutations:
            with self.subTest(progression=name):
                self.assert_rejects(self.case_with_log(0, mutation), name)

    def test_rejects_wrong_ddgi_sdf_rule_independently(self) -> None:
        summary = self.make_summary()
        summary["cases"][4]["ddgi_sdf_marker_count"] = 0
        self.assert_rejects(summary, "required SDF marker removed from metadata")

        summary = self.make_summary()
        summary["cases"][0]["stdout"] += "Loaded DDGI mesh SDF: resources/GLTF_Sponza/sponza_sdf.bin\n"
        self.assert_rejects(summary, "no-DDGI case has SDF success")

    def test_rejects_each_missing_supplementary_marker_and_old_substitution(self) -> None:
        for marker in SUPPLEMENTARY_MARKERS:
            with self.subTest(marker=marker):
                summary = self.make_summary()
                line = next(line for line in summary["cases"][0]["stdout"].splitlines() if f"marker={marker} " in line)
                summary["cases"][0]["stdout"] = summary["cases"][0]["stdout"].replace(line + "\n", "", 1)
                self.assert_rejects(summary)
        with self.subTest(marker="old start substituted for new automation-start"):
            summary = self.make_summary()
            summary["cases"][0]["stdout"] = summary["cases"][0]["stdout"].replace(
                "marker=automation-start", "marker=start", 1
            )
            self.assert_rejects(summary)

    def test_rejects_false_aggregate_pass_and_duplicate_output_paths(self) -> None:
        summary = self.make_summary()
        summary["cases"][2]["stdout"] = summary["cases"][2]["stdout"].replace("marker=redo-observed", "marker=redo-observed-extra", 1)
        # The top-level claim stays pass; the independently recomputed case must veto it.
        self.assert_rejects(summary, "false aggregate pass")

        summary = self.make_summary()
        summary["cases"][2]["passed"] = False
        self.assert_rejects(summary, "case failure cannot be hidden by aggregate tallies")

        summary = self.make_summary()
        summary["cases"][1]["case_dir"] = summary["cases"][0]["case_dir"]
        summary["cases"][1]["artifact_path"] = summary["cases"][0]["artifact_path"]
        summary["cases"][1]["save_target"] = summary["cases"][0]["save_target"]
        self.assert_rejects(summary, "duplicate per-case output path")

    def test_rejects_artifact_outside_evidence_root(self) -> None:
        summary = self.make_summary()
        outside = self.root.parent / "outside-output.gltf"
        outside.write_text(json.dumps({"asset": {"version": "2.0"}}), encoding="utf-8")
        summary["cases"][0]["artifact_path"] = str(outside)
        summary["cases"][0]["save_target"] = str(outside)
        summary["cases"][0]["artifact_sha256"] = sha256_bytes(outside.read_bytes())
        summary["cases"][0]["artifact_size_bytes"] = outside.stat().st_size
        self.assert_rejects(summary)

    def test_rejects_summary_claim_that_is_not_revalidated_from_disk(self) -> None:
        summary_path = self.write_summary_files(self.make_summary())
        on_disk = json.loads(summary_path.read_text(encoding="utf-8"))
        on_disk["passed"] = 4
        summary_path.write_text(json.dumps(on_disk), encoding="utf-8")
        with self.assertRaises(ValidationError):
            validate_summary(summary_path)

    def test_production_literal_marker_sources_match_the_seven_marker_contract(self) -> None:
        application = (REPO_ROOT / "app" / "MinimalLatestApp.h").read_text(
            encoding="utf-8"
        )
        state_contract = (
            REPO_ROOT / "app" / "editor" / "EditorTransactionSmoke.h"
        ).read_text(encoding="utf-8")
        validator = (REPO_ROOT / "tools" / "editor_transaction_smoke.py").read_text(
            encoding="utf-8"
        )
        validator_patterns = validator.split("def _marker_patterns()", 1)[1].split(
            "def _supplementary_patterns", 1
        )[0]
        marker_name_switch = state_contract.split("markerName(", 1)[1].split(
            "phaseName(", 1
        )[0]
        production_literals = (
            "[CSM_AUTOMATION] marker=scene-ready mode=%s model=%s",
            "[CSM_AUTOMATION] marker=automation-start mode=%s transaction=rename-undo-redo-save frame_budget=40",
            "[CSM_AUTOMATION] marker=undo-observed mode=%s revision=%llu undo=0 redo=1 digest=%s renderer_revision=%llu",
            "[CSM_AUTOMATION] marker=redo-observed mode=%s revision=%llu undo=1 redo=0 digest=%s renderer_revision=%llu",
            "[CSM_AUTOMATION] marker=save-complete mode=%s revision=%llu clean_revision=%llu bytes=%llu reload_valid=1 semantic_equal=1",
            "[CSM_AUTOMATION] marker=frame-budget-complete mode=%s frames=40",
            "[CSM_AUTOMATION] marker=automation-complete mode=%s frames=40 auto_exit=1",
        )
        for literal in production_literals:
            with self.subTest(literal=literal):
                self.assertEqual(application.count(literal), 1)
        self.assertIn(
            "[CSM_AUTOMATION] marker=config mode=%s fixed_dt=%.9f warmup=%u motion=%u hold=%u no_ui=%d no_post=%d no_ddgi=%d taa=%d auto_exit=%d capture_sync=%d capture_control=%d capture_sync_timeout_ms=%u capture_sync_dir=%s",
            application,
        )
        self.assertIn(
            "[CSM_AUTOMATION] marker=%s mode=%s frame=%llu current_pos=(%.6f,%.6f,%.6f) current_yaw_pitch=(%.6f,%.6f) previous_pos=(%.6f,%.6f,%.6f) previous_yaw_pitch=(%.6f,%.6f)",
            application,
        )
        for marker in AUTHORITATIVE_MARKERS:
            with self.subTest(state_marker=marker):
                self.assertEqual(marker_name_switch.count(f'return "{marker}";'), 1)
        for marker in AUTHORITATIVE_MARKERS:
            with self.subTest(old_prefix=marker):
                self.assertNotIn(
                    f"[EDITOR_TRANSACTION] marker={marker}", application
                )
        self.assertNotIn("EDITOR_TRANSACTION", validator_patterns)
        self.assertEqual(validator_patterns.count(r"\[CSM_AUTOMATION\] marker="), 7)

    def test_bounded_production_directories_have_no_forbidden_helper_dependency(self) -> None:
        forbidden = re.compile(
            r"(?:nvcore|nvpro|nvvk|nvapp|nvgui|nvvkgltf|reference-helper)",
            re.IGNORECASE,
        )
        production_roots = (
            REPO_ROOT / "app",
            REPO_ROOT / "scene" / "editor",
            REPO_ROOT / "render" / "editor",
            REPO_ROOT / "rhi",
        )
        source_suffixes = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inl", ".m", ".mm"}
        matches: list[str] = []
        for root in production_roots:
            self.assertTrue(root.is_dir(), f"missing bounded production root: {root}")
            for path in root.rglob("*"):
                if not path.is_file() or path.suffix.lower() not in source_suffixes:
                    continue
                text = path.read_text(encoding="utf-8", errors="ignore")
                if forbidden.search(text):
                    matches.append(str(path.relative_to(REPO_ROOT)))
        self.assertEqual(matches, [])

    def test_rejects_contradictory_structured_fields(self) -> None:
        summary = self.make_summary()
        summary["cases"][0]["undo_count"] = 1
        self.assert_rejects(summary)
        summary = self.make_summary()
        summary["cases"][0]["reload_valid"] = 0
        self.assert_rejects(summary)


if __name__ == "__main__":
    unittest.main()
