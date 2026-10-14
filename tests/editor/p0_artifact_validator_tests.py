#!/usr/bin/env python3
"""Regression tests for P0 manifest and fixture fail-closed validation."""

from __future__ import annotations

import copy
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path, PurePosixPath
from types import SimpleNamespace
from unittest.mock import patch


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools"))

from validate_editor_p0_artifacts import (  # noqa: E402
    ValidationError,
    _validate_configured_volk_foundation,
    _validate_o09_selected_attempt_outcome,
    validate_corpus,
    validate_configured_editor_build,
    validate_d20_trace_index_data,
    validate_edge_obligations,
    validate_editor_dependency_boundary,
    validate_f39_repair,
    validate_input_clipboard_index_data,
    validate_manifest,
    validate_reference_trace_artifacts,
    validate_resolved_dear_imgui_identity,
    validate_ui_semantic_index_data,
    validate_o09_real_ui_index_data,
    validate_ui_runtime_index_data,
    verify_o09_real_ui_external,
    verify_ui_semantic_external,
    verify_ui_runtime_external,
)
import generate_editor_dependency_identity as dependency_identity  # noqa: E402
from generate_editor_dependency_identity import (  # noqa: E402
    IdentityError,
    _compiler_tokens,
    _expand_response_tokens,
    generate_identity,
)


TEST_SAFETY_POLICY = {
    "schemaVersion": 1,
    "resolution": "mgif_safety_exception",
    "sceneRouting": "clicked_scene_id",
    "snapshotScope": "all_scenes",
    "occurrenceRouting": "clicked_scene_occurrence",
    "sharedNodeHandling": "preserve_shared_node_occurrence_consistency",
    "preflightRejections": ["cycle", "multiparent", "stale_occurrence"],
    "rejectionTiming": "before_command_or_history_entry",
    "traversal": "bounded_iterative",
    "uiParitySurface": [
        "entry", "label", "layout", "shortcut", "modal", "focus", "enablement", "history_description",
    ],
    "boundOracleIds": ["O09-multi-scene-structural-matrix"],
    "acceptanceIds": [
        "T-EDIT-07", "T-UI-15", "U01-empty-new-open", "U02-hierarchy",
        "V05-delta-edit", "V06-topology-swap", "P10-T01", "P10-T05",
    ],
}
TEST_O09_READINESS = {
    "status": "eligible_with_debt",
    "coverageBinding": {
        "artifactPath": "docs/editor-parity/reference-traces/d20-hierarchy-index.json",
        "artifactSha256": "b49deaf8a08a378c3162de6166bf3646e1d91d3e52a85c1779a974bac747be91",
        "matrixCellCount": 298,
        "executedCellCount": 298,
        "notExecutedCellCount": 0,
        "caseIdSetSha256": "948f9a74ce6b47e5af7df988c55a584a907c8614fe3b038daf1bd9d9b4582046",
    },
    "nativeUiBinding": {
        "artifactPath": "docs/editor-parity/reference-traces/o09-real-ui-index.json",
        "artifactSha256": "b30b1d173a4f63733bfdad068e49a91d405763b8f7b6581839905ca34177280e",
        "actionPathCells": 298,
        "numericExitCells": 298,
        "fullVisibleHistoryCells": 290,
        "exactSerializedTopologyCells": 7,
        "exactSerializedTopologyCheckpoints": 20,
    },
    "coverageCeiling": {
        "realUiActionPathsCaptured": 298,
        "fullStateUiReplays": 0,
        "privateStateReadbacks": 0,
        "visibleOnlyCurrentSceneCells": 298,
        "visibleOnlySelectionCells": 294,
        "domainMatrixRelabeledAsUiReplays": False,
        "referenceFailuresPreserved": True,
    },
    "evidenceDebt": {
        "uiIds": ["T-UI-15", "U01-empty-new-open", "U02-hierarchy"],
        "vulkanIds": ["V05-delta-edit", "V06-topology-swap"],
        "p10Ids": ["P10-T01", "P10-T05"],
    },
}
TEST_O04_CONFLICT_DISPOSITION = {
    "status": "disposed_with_debt",
    "observedBehavior": "reference_preserves_semantic_document_and_advances_scene_graph_revision_2_3_4_5",
    "targetBehavior": "mgif_preserves_semantic_document_without_revision_advance",
    "disposition": "target_behavior_authoritative",
    "evidenceDebt": {
        "uiIds": ["T-UI-06", "U02-hierarchy"],
        "vulkanIds": ["V05-delta-edit"],
        "p10Ids": ["P10-T01", "P10-T05"],
    },
}
TEST_O06_CONFLICT_DISPOSITION = {
    "status": "disposed_with_debt",
    "observedBehavior": "both_native_orders_clear_selection_retain_transformed_scene_and_leave_undo_disabled",
    "targetBehavior": "mgif_matches_native_visible_result_without_predicted_transform_history_entry",
    "disposition": "observed_behavior_authoritative",
    "evidenceDebt": {
        "uiIds": ["T-UI-13", "U03-transform-gizmo"],
        "vulkanIds": ["V02-gizmo-components", "V07-dpi-resize"],
        "p10Ids": ["P10-T01", "P10-T05"],
    },
}
TEST_NATIVE_PARTIAL_DEBTS = {
    "O05-gizmo-escape-only": {
        "uiIds": ["T-UI-13", "U03-transform-gizmo"],
        "vulkanIds": ["V02-gizmo-components", "V07-dpi-resize"],
        "p10Ids": ["P10-T01", "P10-T05"],
    },
    "O07-gizmo-focus-loss": {
        "uiIds": ["T-UI-13", "U03-transform-gizmo"],
        "vulkanIds": ["V02-gizmo-components", "V07-dpi-resize"],
        "p10Ids": ["P10-T01", "P10-T05"],
    },
    "O08-viewport-click-classifier": {
        "uiIds": ["T-UI-03", "U03-transform-gizmo"],
        "vulkanIds": ["V05-delta-edit", "V07-dpi-resize"],
        "p10Ids": ["P10-T01", "P10-T05"],
    },
    "O10-material-clipboard": {
        "uiIds": ["T-UI-13", "U04-material"],
        "vulkanIds": ["V05-delta-edit"],
        "p10Ids": ["P10-T01", "P10-T05"],
    },
}
TEST_LICENSE_TEXT = """Synthetic MGIF Validator Fixture License

Copyright (c) 2026 Synthetic Fixture Authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this synthetic fixture text to use it only for validator regression tests.
The fixture may be copied, modified, and distributed with the test suite.
This text does not license the MGIF project or any production dependency.
The above copyright notice and this permission notice shall be included.
THE FIXTURE IS PROVIDED AS IS, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.
IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES, OR OTHER LIABILITY.
This final line makes truncation tests unambiguous and preserves a complete fixture.
"""


class P0ArtifactValidatorTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name)
        (self.root / "docs/editor-parity/schemas").mkdir(parents=True)
        (self.root / "tests/fixtures").mkdir(parents=True)
        shutil.copy2(
            REPO_ROOT / "docs/gltf-scene-editor-parity-task.md",
            self.root / "docs/gltf-scene-editor-parity-task.md",
        )
        for name in (
            "edge-obligations.json",
            "F39-REPAIR.md",
            "f39-repair-index.json",
            "manifest.json",
            "LICENSE-SOURCES.md",
            "MGIF-BASELINE.md",
            "REFERENCE-SOURCE-EVIDENCE.md",
        ):
            shutil.copy2(REPO_ROOT / "docs/editor-parity" / name, self.root / "docs/editor-parity" / name)
        shutil.copy2(
            REPO_ROOT / "docs/editor-parity/schemas/parity-manifest.schema.json",
            self.root / "docs/editor-parity/schemas/parity-manifest.schema.json",
        )
        shutil.copytree(
            REPO_ROOT / "docs/editor-parity/reference-traces",
            self.root / "docs/editor-parity/reference-traces",
        )
        shutil.copytree(
            REPO_ROOT / "tests/fixtures/gltf_editor",
            self.root / "tests/fixtures/gltf_editor",
        )
        (self.root / "tests/editor").mkdir(parents=True)
        for name in (
            "gltf_document_load_tests.cpp",
            "gltf_document_roundtrip_tests.cpp",
            "gltf_document_io_tests.cpp",
            "gltf_validator_tests.cpp",
        ):
            shutil.copy2(REPO_ROOT / "tests/editor" / name, self.root / "tests/editor" / name)
        self.dependency_project_index = 0

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def load_manifest(self) -> dict[str, object]:
        return json.loads((self.root / "docs/editor-parity/manifest.json").read_text(encoding="utf-8"))

    def write_manifest(self, manifest: dict[str, object]) -> None:
        (self.root / "docs/editor-parity/manifest.json").write_text(
            json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )

    def make_p1_ready_manifest(self) -> dict[str, object]:
        manifest = self.load_manifest()
        self.assertEqual(manifest["status"], "p1_engineering_ready")
        self.assertEqual(manifest["decisionGates"]["D20"]["selectedPolicyContract"], TEST_SAFETY_POLICY)
        self.assertEqual(
            next(item for item in manifest["oracleTraces"] if item["id"] == "O09-multi-scene-structural-matrix")[
                "p1Readiness"
            ],
            TEST_O09_READINESS,
        )
        return manifest

    def make_release_ready_manifest(self) -> dict[str, object]:
        manifest = self.make_p1_ready_manifest()
        f42 = next(item for item in manifest["features"] if item["id"] == "F42")
        f42["mgifEvidence"].update(
            {
                "grade": "source",
                "status": "pass",
                "artifacts": ["docs/editor-parity/LICENSE-SOURCES.md", "LICENSE"],
                "notes": "Synthetic root-license policy resolution for the release validator fixture.",
            }
        )
        (self.root / "LICENSE").write_text(TEST_LICENSE_TEXT, encoding="utf-8")
        accepted_license_sha256 = hashlib.sha256((self.root / "LICENSE").read_bytes()).hexdigest()
        manifest["licensePolicy"] = {
            "status": "selected",
            "selectedSpdxId": "LicenseRef-MGIF-Validator-Fixture",
            "selectedFile": "LICENSE",
            "acceptedSha256": accepted_license_sha256,
            "approvedBy": "synthetic project owner",
            "approvedAt": "2026-08-22T12:00:00+08:00",
            "developmentPolicy": {
                "distributionStatus": "licensed",
                "engineeringImplementation": "original_clean_room_unless_provenance_ledger_says_otherwise",
                "upstreamApacheSourceCopied": False,
                "preserveThirdPartyNotices": True,
                "p1EngineeringReadinessIndependent": True,
            },
            "releaseGate": {
                "status": "ready_with_selected_root_license",
                "blockedFeatureIds": [],
                "blockedRequirementIds": [],
                "passClaimAllowed": True,
            },
            "validation": {
                "utf8Required": True,
                "nonWhitespaceRequired": True,
                "minimumBytes": 256,
                "exactAcceptedDigestRequired": True,
            },
        }
        license_path = self.root / "docs/editor-parity/LICENSE-SOURCES.md"
        license_text = license_path.read_text(encoding="utf-8")
        license_text = license_text.replace(
            "no `LICENSE`, `COPYING`, or `NOTICE` file",
            "declared root policy in `LICENSE`",
        )
        license_path.write_text(license_text + "\nRoot policy: `LICENSE`\n", encoding="utf-8")
        return manifest

    def load_edges(self) -> dict[str, object]:
        return json.loads((self.root / "docs/editor-parity/edge-obligations.json").read_text(encoding="utf-8"))

    def write_edges(self, edges: dict[str, object]) -> None:
        (self.root / "docs/editor-parity/edge-obligations.json").write_text(
            json.dumps(edges, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )

    def load_reference_trace(self, name: str) -> dict[str, object]:
        return json.loads(
            (self.root / "docs/editor-parity/reference-traces" / name).read_text(encoding="utf-8")
        )

    def make_dependency_project(
        self,
        cmake: str,
        files: dict[str, str],
    ) -> Path:
        self.dependency_project_index += 1
        root = self.root / f"dependency-project-{self.dependency_project_index}"
        (root / "docs/editor-parity").mkdir(parents=True)
        shutil.copy2(
            REPO_ROOT / "docs/editor-parity/manifest.json",
            root / "docs/editor-parity/manifest.json",
        )
        (root / "CMakeLists.txt").write_text(cmake.strip() + "\n", encoding="utf-8")
        for relative_path, contents in files.items():
            path = root.joinpath(*Path(relative_path).parts)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(contents, encoding="utf-8")
        return root

    def make_tinygltf_dependency_project(
        self,
        *,
        repository: str = "https://github.com/syoyo/tinygltf.git",
        tag: str = "v2.9.3",
        authority_edge: str = "target_link_libraries(scene_editor_document PRIVATE tinygltf)",
        extra_cmake: str = "",
        extra_files: dict[str, str] | None = None,
    ) -> Path:
        document_sources = """
          loader/TinyGltfImplementation.cpp
          scene/editor/include/mgif/scene_editor/EditorLimits.h
          scene/editor/include/mgif/scene_editor/EditorObjectId.h
          scene/editor/include/mgif/scene_editor/EditorHistoryPolicy.h
          scene/editor/include/mgif/scene_editor/GltfDocument.h
          scene/editor/include/mgif/scene_editor/StructuralDelta.h
          scene/editor/include/mgif/scene_editor/SceneCommands.h
          scene/editor/include/mgif/scene_editor/GltfDocumentIO.h
          scene/editor/include/mgif/scene_editor/GltfSemanticCompare.h
          scene/editor/include/mgif/scene_editor/GltfValidator.h
          scene/editor/GltfDocument.cpp
          scene/editor/StructuralDelta.cpp
          scene/editor/SceneCommands.cpp
          scene/editor/GltfDocumentIO.cpp
          scene/editor/GltfSemanticCompare.cpp
          scene/editor/GltfValidator.cpp
        """
        core_sources = """
          scene/editor/include/mgif/scene_editor/EditorClock.h
          scene/editor/include/mgif/scene_editor/EditorFaultInjection.h
          scene/editor/include/mgif/scene_editor/EditorSelection.h
          scene/editor/include/mgif/scene_editor/testing/EditorStateInspector.h
          scene/editor/include/mgif/scene_editor/commands/EditorCommand.h
          scene/editor/include/mgif/scene_editor/commands/EditorHistory.h
          scene/editor/include/mgif/scene_editor/commands/HistoryLifecycle.h
          scene/editor/include/mgif/scene_editor/commands/TransformCommands.h
          scene/editor/include/mgif/scene_editor/commands/HierarchyCommands.h
          scene/editor/include/mgif/scene_editor/commands/PropertyCommands.h
          scene/editor/include/mgif/scene_editor/commands/ContinuousEdit.h
          scene/editor/include/mgif/scene_editor/EditorSession.h
          scene/editor/include/mgif/scene_editor/SceneEditor.h
          scene/editor/SceneEditorCore.cpp
          scene/editor/EditorSelection.cpp
          scene/editor/commands/EditorCommand.cpp
          scene/editor/commands/EditorHistory.cpp
          scene/editor/commands/HistoryLifecycle.cpp
          scene/editor/commands/TransformCommands.cpp
          scene/editor/commands/HierarchyCommands.cpp
          scene/editor/commands/PropertyCommands.cpp
          scene/editor/commands/ContinuousEdit.cpp
          scene/editor/EditorSession.cpp
          scene/editor/SceneEditor.cpp
        """
        files = {
            "loader/TinyGltfImplementation.cpp": (
                "#define TINYGLTF_IMPLEMENTATION\n"
                "#define STB_IMAGE_IMPLEMENTATION\n"
                "#define STB_IMAGE_WRITE_IMPLEMENTATION\n"
                "#include <tiny_gltf.h>\n"
            ),
            "loader/GltfLoader.cpp": "#include <tiny_gltf.h>\nint loader_anchor = 0;\n",
            "scene/editor/include/mgif/scene_editor/EditorLimits.h": "#pragma once\n",
            "scene/editor/include/mgif/scene_editor/EditorObjectId.h": "#pragma once\n",
            "scene/editor/include/mgif/scene_editor/EditorHistoryPolicy.h": "#pragma once\n",
            "scene/editor/include/mgif/scene_editor/GltfDocument.h": "#pragma once\n",
            "scene/editor/include/mgif/scene_editor/StructuralDelta.h": "#pragma once\n",
            "scene/editor/include/mgif/scene_editor/SceneCommands.h": "#pragma once\n",
            "scene/editor/include/mgif/scene_editor/GltfDocumentIO.h": "#pragma once\n",
            "scene/editor/include/mgif/scene_editor/GltfSemanticCompare.h": "#pragma once\n",
            "scene/editor/include/mgif/scene_editor/GltfValidator.h": "#pragma once\n",
            "scene/editor/GltfDocument.cpp": "#include <tiny_gltf.h>\nint document_anchor = 0;\n",
            "scene/editor/StructuralDelta.cpp": "#include <tiny_gltf.h>\nint delta_anchor = 0;\n",
            "scene/editor/SceneCommands.cpp": "#include <tiny_gltf.h>\nint commands_anchor = 0;\n",
            "scene/editor/GltfDocumentIO.cpp": "#include <tiny_gltf.h>\nint io_anchor = 0;\n",
            "scene/editor/GltfSemanticCompare.cpp": "int semantic_compare_anchor = 0;\n",
            "scene/editor/GltfValidator.cpp": "#include <tiny_gltf.h>\nint validator_anchor = 0;\n",
            "scene/editor/include/mgif/scene_editor/EditorClock.h": "#pragma once\n",
            "scene/editor/include/mgif/scene_editor/EditorFaultInjection.h": "#pragma once\n",
            "scene/editor/include/mgif/scene_editor/EditorSelection.h": "#pragma once\n",
            "scene/editor/include/mgif/scene_editor/testing/EditorStateInspector.h": "#pragma once\n",
            "scene/editor/include/mgif/scene_editor/commands/EditorCommand.h": "#pragma once\n",
            "scene/editor/include/mgif/scene_editor/commands/EditorHistory.h": "#pragma once\n",
            "scene/editor/include/mgif/scene_editor/commands/HistoryLifecycle.h": "#pragma once\n",
            "scene/editor/include/mgif/scene_editor/commands/TransformCommands.h": "#pragma once\n",
            "scene/editor/include/mgif/scene_editor/commands/HierarchyCommands.h": "#pragma once\n",
            "scene/editor/include/mgif/scene_editor/commands/PropertyCommands.h": "#pragma once\n",
            "scene/editor/include/mgif/scene_editor/commands/ContinuousEdit.h": "#pragma once\n",
            "scene/editor/include/mgif/scene_editor/EditorSession.h": "#pragma once\n",
            "scene/editor/include/mgif/scene_editor/SceneEditor.h": "#pragma once\n",
            "scene/editor/SceneEditorCore.cpp": "int scene_editor_core_anchor = 0;\n",
            "scene/editor/EditorSelection.cpp": "int selection_anchor = 0;\n",
            "scene/editor/commands/EditorCommand.cpp": "int command_anchor = 0;\n",
            "scene/editor/commands/EditorHistory.cpp": "int history_anchor = 0;\n",
            "scene/editor/commands/HistoryLifecycle.cpp": "int lifecycle_anchor = 0;\n",
            "scene/editor/commands/TransformCommands.cpp": "int transform_anchor = 0;\n",
            "scene/editor/commands/HierarchyCommands.cpp": "int hierarchy_anchor = 0;\n",
            "scene/editor/commands/PropertyCommands.cpp": "int property_anchor = 0;\n",
            "scene/editor/commands/ContinuousEdit.cpp": "int continuous_anchor = 0;\n",
            "scene/editor/EditorSession.cpp": "int session_anchor = 0;\n",
            "scene/editor/SceneEditor.cpp": "int editor_anchor = 0;\n",
            "tests/editor/editor_history_core_tests.cpp": "int main() { return 0; }\n",
            "tests/editor/editor_history_structural_tests.cpp": "int main() { return 0; }\n",
            "tests/editor/editor_history_property_tests.cpp": "int main() { return 0; }\n",
            "tests/editor/editor_continuous_edit_tests.cpp": "int main() { return 0; }\n",
        }
        files.update(extra_files or {})
        return self.make_dependency_project(
            f"""
            include(FetchContent)
            FetchContent_Declare(tinygltf
              GIT_REPOSITORY {repository}
              GIT_TAG {tag})
            set(TINYGLTF_HEADER_ONLY ON CACHE BOOL "" FORCE)
            FetchContent_MakeAvailable(tinygltf)
            add_library(scene_editor_document STATIC {document_sources})
            {authority_edge}
            target_compile_definitions(scene_editor_document PRIVATE MGIF_EDITOR_TESTING=1)
            add_library(scene_editor_core STATIC {core_sources})
            target_link_libraries(scene_editor_core PRIVATE scene_editor_document)
            add_executable(editor_history_core_tests tests/editor/editor_history_core_tests.cpp)
            add_executable(editor_history_structural_tests tests/editor/editor_history_structural_tests.cpp)
            add_executable(editor_history_property_tests tests/editor/editor_history_property_tests.cpp)
            add_executable(editor_continuous_edit_tests tests/editor/editor_continuous_edit_tests.cpp)
            foreach(p4_target IN ITEMS
                editor_history_core_tests
                editor_history_structural_tests
                editor_history_property_tests
                editor_continuous_edit_tests)
              target_compile_definitions(${{p4_target}} PRIVATE MGIF_EDITOR_TESTING=1)
              target_link_libraries(${{p4_target}} PRIVATE scene_editor_core)
            endforeach()
            {extra_cmake}
            """,
            files,
        )

    def make_volk_dependency_project(
        self,
        *,
        volk_declaration: str | None = None,
        volk_population: str | None = None,
        authority_edge: str | None = None,
        before_edge: str = "",
        extra_cmake: str = "",
        extra_files: dict[str, str] | None = None,
    ) -> Path:
        if volk_declaration is None:
            volk_declaration = """
              FetchContent_Declare(volk
                GIT_REPOSITORY https://github.com/zeux/volk.git
                GIT_TAG master)
            """
        if volk_population is None:
            volk_population = "FetchContent_MakeAvailable(volk)"
        if authority_edge is None:
            authority_edge = """
              target_link_libraries(demo_core
                PUBLIC
                  scene_editor_core
                  volk
                  imgui_demo
                  glm
                  tinygltf
                  meshoptimizer
                  Vulkan::Vulkan)
            """
        files = {
            "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
            "app/editor/DesktopUi.cpp": "int desktop_ui_anchor = 0;\n",
            "app/editor/Leaf.cpp": "int editor_leaf_anchor = 0;\n",
            "render/editor/DemoCore.cpp": "int demo_core_anchor = 0;\n",
            "rhi/GlmFoundation.cpp": "int glm_foundation_anchor = 0;\n",
            "rhi/TinyGltfFoundation.cpp": "int tinygltf_foundation_anchor = 0;\n",
            "rhi/MeshOptimizerFoundation.cpp": "int meshoptimizer_foundation_anchor = 0;\n",
            "rhi/VulkanFoundation.cpp": "int vulkan_foundation_anchor = 0;\n",
        }
        files.update(extra_files or {})
        return self.make_dependency_project(
            f"""
            include(FetchContent)
            {volk_declaration}
            {volk_population}
            FetchContent_Declare(imgui
              GIT_REPOSITORY https://github.com/ocornut/imgui.git
              GIT_TAG fd13a1e8923a0a7077b404fc36fd063b25a0c0b5)
            FetchContent_MakeAvailable(imgui)
            add_library(imgui_demo STATIC
              ${{imgui_SOURCE_DIR}}/imgui.cpp
              ${{imgui_SOURCE_DIR}}/imgui.h
              ${{imgui_SOURCE_DIR}}/imgui_draw.cpp
              ${{imgui_SOURCE_DIR}}/imgui_tables.cpp
              ${{imgui_SOURCE_DIR}}/imgui_widgets.cpp)
            add_library(glm STATIC rhi/GlmFoundation.cpp)
            add_library(tinygltf STATIC rhi/TinyGltfFoundation.cpp)
            add_library(meshoptimizer STATIC rhi/MeshOptimizerFoundation.cpp)
            add_library(vulkan_foundation STATIC rhi/VulkanFoundation.cpp)
            add_library(Vulkan::Vulkan ALIAS vulkan_foundation)
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            add_library(desktop_scene_editor_ui STATIC app/editor/DesktopUi.cpp)
            add_library(editor_owned_leaf STATIC app/editor/Leaf.cpp)
            add_library(demo_core STATIC render/editor/DemoCore.cpp)
            {before_edge}
            {authority_edge}
            {extra_cmake}
            """,
            files,
        )

    def test_compiler_response_expansion_dialects_encodings_and_relative_nesting(self) -> None:
        response_root = self.root / "response files"
        response_root.mkdir()
        forced = response_root / "forced header.hpp"
        forced.write_text("#pragma once\n", encoding="utf-8")

        cases = (
            ("posix", "inner-posix.rsp", f"-include '{forced.as_posix()}'\n".encode("utf-8")),
            ("msvc", "inner-msvc.rsp", f'/FI"{forced.as_posix()}"\n'.encode("utf-8-sig")),
            ("msvc", "inner-utf16.rsp", f'/FI"{forced.as_posix()}"\n'.encode("utf-16")),
        )
        for style, name, payload in cases:
            with self.subTest(style=style, name=name):
                inner = response_root / name
                inner.write_bytes(payload)
                outer = response_root / f"outer-{name}"
                outer.write_text(f'@"{inner.name}"\n', encoding="utf-8")
                expanded, evidence = _expand_response_tokens(
                    [f'@{outer.name}'],
                    directory=response_root,
                    style=style,
                    label=name,
                )
                self.assertEqual(len(evidence), 2)
                self.assertTrue(
                    expanded[0].lower().startswith("-include" if style == "posix" else "/fi")
                )
                self.assertIn(forced.as_posix(), " ".join(expanded))

    def test_compiler_response_expansion_fails_closed_for_malformed_inputs_and_limits(self) -> None:
        response_root = self.root / "response-errors"
        response_root.mkdir()
        missing = response_root / "missing.rsp"
        with self.assertRaisesRegex(IdentityError, r"missing or unreadable compiler response file"):
            _expand_response_tokens([f"@{missing}"], directory=response_root, style="posix", label="missing")

        malformed = response_root / "malformed.rsp"
        malformed.write_bytes(b"\xff\xff")
        with self.assertRaisesRegex(IdentityError, r"unsupported or invalid encoding"):
            _expand_response_tokens([f"@{malformed}"], directory=response_root, style="posix", label="encoding")

        utf16 = response_root / "utf16.rsp"
        utf16.write_bytes("-DOK=1".encode("utf-16"))
        with self.assertRaisesRegex(IdentityError, r"UTF-16.*unsupported"):
            _expand_response_tokens([f"@{utf16}"], directory=response_root, style="posix", label="utf16")

        cycle_a = response_root / "a.rsp"
        cycle_b = response_root / "b.rsp"
        cycle_a.write_text("@b.rsp\n", encoding="utf-8")
        cycle_b.write_text("@a.rsp\n", encoding="utf-8")
        with self.assertRaisesRegex(IdentityError, r"response-file cycle"):
            _expand_response_tokens(["@a.rsp"], directory=response_root, style="posix", label="cycle")

        with patch.object(dependency_identity, "RESPONSE_MAX_DEPTH", 1):
            leaf = response_root / "leaf.rsp"
            leaf.write_text("-DOK=1\n", encoding="utf-8")
            cycle_b.write_text("@leaf.rsp\n", encoding="utf-8")
            with self.assertRaisesRegex(IdentityError, r"nesting exceeds"):
                _expand_response_tokens(["@a.rsp"], directory=response_root, style="posix", label="depth")

        count_a = response_root / "count-a.rsp"
        count_b = response_root / "count-b.rsp"
        count_a.write_text("-DONE=1", encoding="utf-8")
        count_b.write_text("-DTWO=1", encoding="utf-8")
        with patch.object(dependency_identity, "RESPONSE_MAX_FILES", 1):
            with self.assertRaisesRegex(IdentityError, r"count exceeds"):
                _expand_response_tokens(
                    ["@count-a.rsp", "@count-b.rsp"],
                    directory=response_root,
                    style="posix",
                    label="count",
                )

        large = response_root / "large.rsp"
        large.write_text("-DVALUE=123456789", encoding="utf-8")
        with patch.object(dependency_identity, "RESPONSE_MAX_FILE_BYTES", 4):
            with self.assertRaisesRegex(IdentityError, r"response file exceeds"):
                _expand_response_tokens(["@large.rsp"], directory=response_root, style="posix", label="bytes")
        with patch.object(dependency_identity, "RESPONSE_MAX_BYTES", 4):
            with self.assertRaisesRegex(IdentityError, r"response-file bytes exceed"):
                _expand_response_tokens(["@large.rsp"], directory=response_root, style="posix", label="aggregate")
        with patch.object(dependency_identity, "RESPONSE_MAX_TOKENS", 1):
            with self.assertRaisesRegex(IdentityError, r"tokens"):
                _expand_response_tokens(["-DONE", "-DTWO"], directory=response_root, style="posix", label="tokens")
        with patch.object(dependency_identity, "RESPONSE_MAX_TOKEN_BYTES", 4):
            with self.assertRaisesRegex(IdentityError, r"token larger"):
                _expand_response_tokens(["@large.rsp"], directory=response_root, style="posix", label="token-bytes")

    def test_compiler_response_tokenizers_preserve_compiler_quoting(self) -> None:
        self.assertEqual(
            _compiler_tokens(r'/I"C:\Program Files\SDK" /DNAME="two words"', "msvc", "msvc"),
            [r"/IC:\Program Files\SDK", "/DNAME=two words"],
        )
        self.assertEqual(
            _compiler_tokens("-I'/opt/My SDK' -DNAME='two words'", "posix", "posix"),
            ["-I/opt/My SDK", "-DNAME=two words"],
        )

    def make_resolved_identity_fixture(self, build_root: Path) -> Path:
        source_root = build_root / "_deps/imgui-src"
        source_root.mkdir(parents=True)
        target_sources = [
            "backends/imgui_impl_glfw.cpp",
            "backends/imgui_impl_glfw.h",
            "imgui.cpp",
            "imgui.h",
            "imgui_demo.cpp",
            "imgui_draw.cpp",
            "imgui_tables.cpp",
            "imgui_widgets.cpp",
        ]
        for relative in target_sources:
            path = source_root.joinpath(*PurePosixPath(relative).parts)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(f"// canonical fixture: {relative}\n", encoding="utf-8")
        (self.root / "tools").mkdir(exist_ok=True)
        shutil.copy2(
            REPO_ROOT / "tools/generate_editor_dependency_identity.py",
            self.root / "tools/generate_editor_dependency_identity.py",
        )
        (self.root / "CMakeLists.txt").write_text("# identity fixture\n", encoding="utf-8")
        hashes = {
            relative: hashlib.sha256(
                source_root.joinpath(*PurePosixPath(relative).parts).read_bytes()
            ).hexdigest()
            for relative in target_sources
        }
        compiled = [relative for relative in target_sources if not relative.endswith(".h")]
        generator_path = self.root / "tools/generate_editor_dependency_identity.py"
        identity = {
            "schemaVersion": 2,
            "kind": "mgif.editor-dependency-build-identity",
            "dependency": "dear_imgui",
            "repository": "https://github.com/ocornut/imgui.git",
            "target": "imgui_demo",
            "sourceMode": "fetchcontent_git_outside_repository",
            "projectSourceRoot": self.root.resolve().as_posix(),
            "projectCMakeSha256": hashlib.sha256((self.root / "CMakeLists.txt").read_bytes()).hexdigest(),
            "generator": {
                "path": "tools/generate_editor_dependency_identity.py",
                "sha256": hashlib.sha256(generator_path.read_bytes()).hexdigest(),
            },
            "resolvedSourceRoot": source_root.resolve().as_posix(),
            "buildRoot": build_root.resolve().as_posix(),
            "sourceOverride": {"cacheOrCli": False, "environment": False},
            "resolvedGitCommit": "fd13a1e8923a0a7077b404fc36fd063b25a0c0b5",
            "resolvedGitTree": "0c8986ccb662ea838c04f7bed4cc56d3379132ef",
            "platformBackend": "glfw",
            "targetSourceRelativePaths": target_sources,
            "compiledSourceRelativePaths": compiled,
            "compiledSourceSha256": {relative: hashes[relative] for relative in compiled},
            "targetSourceSha256": hashes,
            "configuredBuildEvidence": {
                "cmakeGenerator": "Ninja",
                "configuration": "Debug",
                "fileApiIndex": {"path": ".cmake/api/v1/reply/index.json", "sha256": "0" * 64},
                "fileApiCodemodel": {"path": ".cmake/api/v1/reply/codemodel.json", "sha256": "0" * 64},
                "fileApiCache": {"path": ".cmake/api/v1/reply/cache.json", "sha256": "0" * 64},
                "fileApiToolchains": {"path": ".cmake/api/v1/reply/toolchains.json", "sha256": "0" * 64},
                "fileApiImguiTarget": {"path": ".cmake/api/v1/reply/imgui.json", "sha256": "0" * 64},
                "compileCommands": {"path": "compile_commands.json", "sha256": "0" * 64},
                "buildSystem": {"path": "build.ninja", "sha256": "0" * 64},
                "compiledCommandSha256": {relative: "0" * 64 for relative in compiled},
            },
        }
        identity_path = build_root / "editor-dependency-build-identity.json"
        identity_path.write_text(json.dumps(identity, indent=2) + "\n", encoding="utf-8")
        return identity_path

    def make_f39_repair_fixture(self) -> tuple[dict[str, str], Path]:
        evidence_relative = "out/validation/synthetic-f39-repair"
        evidence_root = self.root / evidence_relative
        evidence_root.mkdir(parents=True, exist_ok=True)
        demo_sha256 = "789518c264c43d8426345576e43a7c138082eed9c67466c37c8562c5d8e26f1a"
        marker_names = ("config", "scene_ready", "start", "last_moving", "first_still", "settled", "complete")
        cases = []
        first_stdout = evidence_root / "case-1.stdout.log"
        for index in range(1, 6):
            case_id = f"case-{index}"
            stdout_path = evidence_root / f"{case_id}.stdout.log"
            stderr_path = evidence_root / f"{case_id}.stderr.log"
            stdout_path.write_text(
                "".join(f"[CSM_AUTOMATION] marker={name.replace('_', '-')}\n" for name in marker_names),
                encoding="utf-8",
            )
            stderr_path.write_bytes(b"")
            case = {
                "case": case_id,
                "timed_out": False,
                "exit_code": 0,
                "marker_counts": {name: 1 for name in marker_names},
                "invalid_markers": [],
                "failure_matches": [],
                "demo_sha256_before": demo_sha256,
                "demo_sha256_after": demo_sha256,
                "demo_sha256_stable": True,
                "stdout_log": str(stdout_path),
                "stderr_log": str(stderr_path),
                "passed": True,
            }
            cases.append(case)
            (evidence_root / f"{case_id}.result.json").write_text(
                json.dumps(case, indent=2) + "\n",
                encoding="utf-8",
            )
        summary = {
            "demo_sha256": demo_sha256,
            "case_count": 5,
            "passed": 5,
            "failed": 0,
            "cases": cases,
        }
        identity = {"demo_sha256": demo_sha256}
        summary_path = evidence_root / "summary.json"
        identity_path = evidence_root / "identity.json"
        summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        identity_path.write_text(json.dumps(identity, indent=2) + "\n", encoding="utf-8")
        index = {
            "schemaVersion": 1,
            "schema": "mgif.editor-parity.f39-renderer-repair-index",
            "historicalBaseline": {
                "commit": "6c36567625b305286755a734a66188a22df18e77",
                "demoSha256": "92226d901a95d0e6b2abbcb5c33acc788f081473ae7dca11d45e8ababb1f08c8",
                "passed": 0,
                "caseCount": 5,
                "failure": "SYNC-HAZARD-READ-AFTER-WRITE",
                "waived": False,
            },
            "currentRepair": {
                "evidenceRoot": evidence_relative,
                "demoSha256": demo_sha256,
                "summary": {
                    "path": f"{evidence_relative}/summary.json",
                    "bytes": summary_path.stat().st_size,
                    "sha256": hashlib.sha256(summary_path.read_bytes()).hexdigest(),
                },
                "identity": {
                    "path": f"{evidence_relative}/identity.json",
                    "bytes": identity_path.stat().st_size,
                    "sha256": hashlib.sha256(identity_path.read_bytes()).hexdigest(),
                },
                "caseCount": 5,
                "passed": 5,
                "failed": 0,
                "allExitZero": True,
                "allRequiredMarkersExactlyOnce": True,
                "allExecutableHashesStable": True,
                "allStderrEmpty": True,
                "allFailureMatchesEmpty": True,
            },
        }
        index_path = self.root / "docs/editor-parity/f39-repair-index.json"
        index_path.write_text(json.dumps(index, indent=2) + "\n", encoding="utf-8")
        expected_artifacts = {
            "docs/editor-parity/F39-REPAIR.md": hashlib.sha256(
                (self.root / "docs/editor-parity/F39-REPAIR.md").read_bytes()
            ).hexdigest(),
            "docs/editor-parity/f39-repair-index.json": hashlib.sha256(index_path.read_bytes()).hexdigest(),
        }
        return expected_artifacts, first_stdout

    def assert_data_mutation_rejected(
        self,
        validator,
        baseline: dict[str, object],
        path: tuple[str | int, ...],
        replacement: object,
        message_pattern: str,
    ) -> None:
        data = copy.deepcopy(baseline)
        target = data
        for component in path[:-1]:
            target = target[component]
        target[path[-1]] = replacement
        with self.assertRaisesRegex(ValidationError, message_pattern):
            validator(data)

    def make_ui_runtime_external_data(self) -> tuple[Path, dict[str, object]]:
        evidence_root = self.root / "synthetic-ui-runtime"
        files = {
            "application.tar.gz": b"application-source",
            "companion.tar.gz": b"companion-source",
            "runtime/bin/editor.exe": b"native-executable",
            "REPORT.md": b"sealed report\n",
            "screenshots/main.png": b"synthetic-png-bytes",
        }
        for relative_path, contents in files.items():
            path = evidence_root.joinpath(*PurePosixPath(relative_path).parts)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(contents)
        hashes = {
            relative_path: hashlib.sha256(contents).hexdigest()
            for relative_path, contents in files.items()
        }
        manifest_path = evidence_root / "SHA256SUMS.txt"
        manifest_path.write_text(
            "".join(f"{digest}  {relative_path}\n" for relative_path, digest in hashes.items()),
            encoding="utf-8",
        )
        data = {
            "sealedEvidenceRoot": str(evidence_root),
            "reference": {
                "application": {
                    "sourceArchive": {
                        "path": str(evidence_root / "application.tar.gz"),
                        "sha256": hashes["application.tar.gz"],
                    }
                },
                "companion": {
                    "sourceArchive": {
                        "path": str(evidence_root / "companion.tar.gz"),
                        "sha256": hashes["companion.tar.gz"],
                    }
                },
                "executable": {
                    "path": str(evidence_root / "runtime/bin/editor.exe"),
                    "relativePath": "runtime/bin/editor.exe",
                    "bytes": len(files["runtime/bin/editor.exe"]),
                    "sha256": hashes["runtime/bin/editor.exe"],
                },
            },
            "sourceEvidence": {
                "report": {
                    "path": str(evidence_root / "REPORT.md"),
                    "sha256": hashes["REPORT.md"],
                },
                "artifactHashManifest": {
                    "path": str(manifest_path),
                    "sha256": hashlib.sha256(manifest_path.read_bytes()).hexdigest(),
                },
            },
            "integrity": {"artifactHashes": {"expected": len(files)}},
            "screenshots": {
                "primary": [
                    {
                        "file": "screenshots/main.png",
                        "sha256": hashes["screenshots/main.png"],
                    }
                ],
                "superseded": [],
            },
        }
        return evidence_root, data

    def test_current_scaffold_manifest_is_valid(self) -> None:
        validate_manifest(self.root)

    def test_rejects_unapproved_dependency_boundary_exception(self) -> None:
        manifest = self.load_manifest()
        manifest["dependencyBoundary"]["approvedExceptions"] = [
            {
                "target": "desktop_editor_ui",
                "dependency": "nvvk",
            }
        ]
        self.write_manifest(manifest)
        with self.assertRaisesRegex(
            ValidationError,
            r"(?i)(?:dependencyBoundary|approvedExceptions|more than 0|unapproved exception)",
        ):
            validate_manifest(self.root)

    def test_current_editor_dependency_boundary_is_valid(self) -> None:
        validate_editor_dependency_boundary(REPO_ROOT)

    def test_rejects_each_forbidden_dependency_family_in_editor_source(self) -> None:
        cases = {
            "nvpro_core": ("scene/editor/NvproBoundary.cpp", "#include <nvpro_core/application.hpp>\n"),
            "nvcore": ("app/editor/NvcoreBoundary.cpp", "#include <nvcore/logger.hpp>\n"),
            "nvapp": ("render/editor/NvappBoundary.cpp", "#include <nvapp/application.hpp>\n"),
            "nvgui": ("scene/editor/NvguiBoundary.cpp", "#include <nvgui/toolbar.hpp>\n"),
            "nvvk": ("render/passes/EditorOverlayPass.cpp", "#include <nvvk/context.hpp>\n"),
            "nvvkgltf": ("shaders/editor_overlay.slang", "// nvvkgltf scene helper\n"),
            "reference_vulkan_helper": ("rhi/EditorBridge.h", "#include <nvh/camera_manipulator.hpp>\n"),
        }
        for family, (relative_path, source) in cases.items():
            with self.subTest(family=family):
                root = self.make_dependency_project(
                    "add_library(scene_editor_core STATIC scene/editor/Core.cpp)",
                    {
                        "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                        relative_path: source,
                    },
                )
                with self.assertRaisesRegex(
                    ValidationError,
                    r"(?i)forbidden editor dependency",
                ):
                    validate_editor_dependency_boundary(root)

    def test_rejects_forbidden_dependency_in_editor_named_public_api(self) -> None:
        root = self.make_dependency_project(
            "add_library(scene_editor_core STATIC scene/editor/Core.cpp)",
            {
                "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                "render/passes/EditorOverlayPass.h": "#include <nvapp/elem.hpp>\n",
            },
        )
        with self.assertRaisesRegex(ValidationError, r"(?i)EditorOverlayPass.*nvapp|nvapp.*EditorOverlayPass"):
            validate_editor_dependency_boundary(root)

    def test_rejects_forbidden_editor_target_link_dependency(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            target_link_libraries(scene_editor_core PRIVATE nvpro_core::nvvk)
            """,
            {"scene/editor/Core.cpp": "int editor_core_anchor = 0;\n"},
        )
        with self.assertRaisesRegex(ValidationError, r"(?i)CMake.*target_link_libraries|nvpro_core"):
            validate_editor_dependency_boundary(root)

    def test_rejects_variable_hidden_editor_include_dependency(self) -> None:
        root = self.make_dependency_project(
            """
            set(REFERENCE_UI_INCLUDE "F:/reference/nvgui/include")
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            target_include_directories(scene_editor_core PRIVATE ${REFERENCE_UI_INCLUDE})
            """,
            {"scene/editor/Core.cpp": "int editor_core_anchor = 0;\n"},
        )
        with self.assertRaisesRegex(ValidationError, r"(?i)nvgui.*target_include_directories|target_include_directories.*nvgui"):
            validate_editor_dependency_boundary(root)

    def test_rejects_unresolved_editor_dependency_variable(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            target_link_libraries(scene_editor_core PRIVATE ${UNREVIEWED_EDITOR_DEPENDENCIES})
            """,
            {"scene/editor/Core.cpp": "int editor_core_anchor = 0;\n"},
        )
        with self.assertRaisesRegex(ValidationError, r"(?i)cannot resolve editor dependency.*UNREVIEWED"):
            validate_editor_dependency_boundary(root)

    def test_rejects_forbidden_transitive_target_dependency(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            add_library(platform_bridge STATIC scene/PlatformBridge.cpp)
            target_link_libraries(scene_editor_core PRIVATE platform_bridge)
            target_link_libraries(platform_bridge PRIVATE nvvkgltf)
            """,
            {
                "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                "scene/PlatformBridge.cpp": "int platform_bridge_anchor = 0;\n",
            },
        )
        with self.assertRaisesRegex(ValidationError, r"(?i)nvvkgltf.*target_link_libraries|target_link_libraries.*nvvkgltf"):
            validate_editor_dependency_boundary(root)

    def test_rejects_forbidden_dependency_behind_interface_property(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            add_library(platform_bridge STATIC scene/PlatformBridge.cpp)
            add_library(hidden_native_bridge STATIC render/HiddenNativeBridge.cpp)
            target_link_libraries(scene_editor_core PRIVATE platform_bridge)
            set_property(TARGET platform_bridge PROPERTY INTERFACE_LINK_LIBRARIES hidden_native_bridge)
            target_link_libraries(hidden_native_bridge PRIVATE nvapp)
            """,
            {
                "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                "scene/PlatformBridge.cpp": "int platform_bridge_anchor = 0;\n",
                "render/HiddenNativeBridge.cpp": "int native_bridge_anchor = 0;\n",
            },
        )
        with self.assertRaisesRegex(ValidationError, r"(?i)nvapp.*target_link_libraries|target_link_libraries.*nvapp"):
            validate_editor_dependency_boundary(root)

    def test_rejects_forbidden_include_hidden_in_transitive_wrapper_source(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            add_library(platform_bridge STATIC scene/PlatformBridge.cpp)
            target_link_libraries(scene_editor_core PRIVATE platform_bridge)
            """,
            {
                "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                "scene/PlatformBridge.cpp": "#include <nvvk/resource_allocator.hpp>\n",
            },
        )
        with self.assertRaisesRegex(ValidationError, r"(?i)PlatformBridge.*nvvk|nvvk.*PlatformBridge"):
            validate_editor_dependency_boundary(root)

    def test_rejects_object_library_edges_from_constructor_and_target_sources(self) -> None:
        attachments = (
            "add_library(scene_editor_core STATIC $<TARGET_OBJECTS:native_objects>)",
            "add_library(scene_editor_core STATIC "
            "$<TARGET_OBJECTS:$<IF:1,native_objects,unused_objects>>)",
            "add_library(scene_editor_core STATIC scene/editor/Core.cpp)\n"
            "target_sources(scene_editor_core PRIVATE $<TARGET_OBJECTS:native_objects>)",
        )
        for attachment in attachments:
            with self.subTest(api=attachment.split("(", 1)[0]):
                root = self.make_dependency_project(
                    f"""
                    add_library(native_objects OBJECT render/NativeObject.cpp)
                    {attachment}
                    """,
                    {
                        "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                        "render/NativeObject.cpp": "#include <nvvk/context.hpp>\n",
                    },
                )
                with self.assertRaisesRegex(
                    ValidationError,
                    r"(?i)NativeObject.*nvvk|nvvk.*NativeObject",
                ):
                    validate_editor_dependency_boundary(root)

    def test_rejects_custom_target_and_target_build_command_edges(self) -> None:
        declarations = (
            "add_custom_target(platform_codegen COMMAND cmake -E echo nvvk)\n"
            "add_dependencies(scene_editor_core platform_codegen)",
            "add_custom_command(TARGET scene_editor_core POST_BUILD "
            "COMMAND cmake -E echo nvvk)",
        )
        for declaration in declarations:
            with self.subTest(api=declaration.split("(", 1)[0]):
                root = self.make_dependency_project(
                    f"""
                    add_library(scene_editor_core STATIC scene/editor/Core.cpp)
                    {declaration}
                    """,
                    {"scene/editor/Core.cpp": "int editor_core_anchor = 0;\n"},
                )
                with self.assertRaisesRegex(
                    ValidationError,
                    r"(?i)nvvk.*add_custom_(?:target|command)|add_custom_(?:target|command).*nvvk",
                ):
                    validate_editor_dependency_boundary(root)

    def test_configured_testing_option_overrides_default_without_hiding_dependencies(self) -> None:
        for dependency in ("scene_editor_core", "nvvk"):
            with self.subTest(dependency=dependency):
                root = self.make_dependency_project(
                    f"""
                    option(BUILD_TESTING "Build tests" OFF)
                    add_library(scene_editor_core STATIC scene/editor/Core.cpp)
                    if(BUILD_TESTING)
                      foreach(target IN ITEMS editor_replay_helper)
                        add_library(${{target}} STATIC app/editor/Replay.cpp)
                        target_link_libraries(${{target}} PRIVATE {dependency})
                      endforeach()
                    endif()
                    """,
                    {
                        "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                        "app/editor/Replay.cpp": "int replay_anchor = 0;\n",
                    },
                )
                default_model = validate_editor_dependency_boundary(root)
                self.assertNotIn("editor_replay_helper", default_model["editorAnchors"])
                if dependency == "nvvk":
                    with self.assertRaisesRegex(ValidationError, r"(?i)nvvk"):
                        validate_editor_dependency_boundary(root, cache_variables={"BUILD_TESTING": "ON"})
                else:
                    configured_model = validate_editor_dependency_boundary(
                        root, cache_variables={"BUILD_TESTING": "ON"})
                    self.assertIn("editor_replay_helper", configured_model["editorAnchors"])

    def test_allows_repository_custom_target_edge_without_forbidden_dependencies(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            add_custom_target(platform_codegen COMMAND cmake -E echo safe)
            add_dependencies(scene_editor_core platform_codegen)
            """,
            {"scene/editor/Core.cpp": "int editor_core_anchor = 0;\n"},
        )
        validate_editor_dependency_boundary(root)

    def test_allows_reference_evidence_and_unrelated_renderer_sentinel(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            add_library(demo_core STATIC render/GPUDrivenRenderer.cpp)
            target_link_libraries(demo_core PUBLIC scene_editor_core)
            target_compile_definitions(demo_core PUBLIC NVVK_SEMAPHORE_DEBUG=1)
            add_executable(Demo app/Main.cpp)
            target_link_libraries(Demo PRIVATE demo_core)
            """,
            {
                "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                "render/GPUDrivenRenderer.cpp": "#define NVVK_SEMAPHORE_DEBUG 1\n",
                "app/Main.cpp": "int main() { return 0; }\n",
                "docs/editor-parity/reference-traces/native-dependencies.md": (
                    "External evidence uses nvpro_core, nvcore, nvapp, nvgui, nvvk, and nvvkgltf.\n"
                ),
            },
        )
        validate_editor_dependency_boundary(root)

    def test_rejects_renderer_sentinel_inside_editor_scope(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            target_compile_definitions(scene_editor_core PRIVATE NVVK_SEMAPHORE_DEBUG=1)
            """,
            {"scene/editor/Core.cpp": "int editor_core_anchor = 0;\n"},
        )
        with self.assertRaisesRegex(ValidationError, r"(?i)NVVK_SEMAPHORE_DEBUG"):
            validate_editor_dependency_boundary(root)

    def test_rejects_direct_forbidden_include_control(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            target_include_directories(scene_editor_core PRIVATE vendor/nvvk/include)
            """,
            {"scene/editor/Core.cpp": "int editor_core_anchor = 0;\n"},
        )
        with self.assertRaisesRegex(
            ValidationError,
            r"(?i)nvvk.*target_include_directories|target_include_directories.*nvvk",
        ):
            validate_editor_dependency_boundary(root)

    def test_rejects_direct_target_compile_option_control(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            target_compile_options(scene_editor_core PRIVATE -include vendor/nvvk/context.hpp)
            """,
            {"scene/editor/Core.cpp": "int editor_core_anchor = 0;\n"},
        )
        with self.assertRaisesRegex(ValidationError, r"(?i)nvvk.*target_compile_options|target_compile_options.*nvvk"):
            validate_editor_dependency_boundary(root)

    def test_rejects_set_target_properties_link_control(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            set_target_properties(scene_editor_core PROPERTIES LINK_LIBRARIES nvvk)
            """,
            {"scene/editor/Core.cpp": "int editor_core_anchor = 0;\n"},
        )
        with self.assertRaisesRegex(
            ValidationError,
            r"(?i)set_target_properties.*nvvk|nvvk.*set_target_properties",
        ):
            validate_editor_dependency_boundary(root)

    def test_rejects_alias_reverse_production_consumer(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            add_library(MGIF::scene_editor ALIAS scene_editor_core)
            add_executable(shipping_app app/Main.cpp)
            target_link_libraries(shipping_app PRIVATE MGIF::scene_editor nvvk)
            """,
            {
                "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                "app/Main.cpp": "int main() { return 0; }\n",
            },
        )
        with self.assertRaisesRegex(
            ValidationError,
            r"(?i)target_link_libraries.*nvvk|nvvk.*target_link_libraries",
        ):
            validate_editor_dependency_boundary(root)

    def test_rejects_generator_join_materialized_include(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            target_include_directories(scene_editor_core PRIVATE
              "$<JOIN:${CMAKE_CURRENT_SOURCE_DIR}/vendor/nv;vk/include,>")
            """,
            {
                "scene/editor/Core.cpp": "#include <context.hpp>\nint editor_core_anchor = 0;\n",
                "vendor/nvvk/include/context.hpp": "#pragma once\n",
            },
        )
        with self.assertRaisesRegex(
            ValidationError,
            r"(?i)nvvk.*target_include_directories|target_include_directories.*nvvk",
        ):
            validate_editor_dependency_boundary(root)

    def test_rejects_generator_join_materialized_link_dependency(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            target_link_libraries(scene_editor_core PRIVATE "$<JOIN:nv;vk,>")
            """,
            {"scene/editor/Core.cpp": "int editor_core_anchor = 0;\n"},
        )
        with self.assertRaisesRegex(
            ValidationError,
            r"(?i)target_link_libraries.*nvvk|nvvk.*target_link_libraries",
        ):
            validate_editor_dependency_boundary(root)

    def test_case_whitespace_comments_and_bracket_arguments_cannot_hide_dependency(self) -> None:
        root = self.make_dependency_project(
            """
            #[=[ set(BAD mgif_scene_adapter) ]=]
            SeT   ( BAD [=[nvvk]=] ) # set(BAD mgif_scene_adapter)
            AdD_LiBrArY ( scene_editor_core STATIC scene/editor/Core.cpp )
            TaRgEt_LiNk_LiBrArIeS (
              scene_editor_core
              PRIVATE
              ${BAD}
            )
            """,
            {"scene/editor/Core.cpp": "int editor_core_anchor = 0;\n"},
        )
        with self.assertRaisesRegex(
            ValidationError,
            r"(?i)target_link_libraries.*nvvk|nvvk.*target_link_libraries",
        ):
            validate_editor_dependency_boundary(root)

    def test_rejects_function_wrapped_dependency(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            function(wire_native target dependency)
              target_link_libraries(${target} PRIVATE ${dependency})
            endfunction()
            wire_native(scene_editor_core nvvk)
            """,
            {"scene/editor/Core.cpp": "int editor_core_anchor = 0;\n"},
        )
        with self.assertRaisesRegex(
            ValidationError,
            r"(?i)target_link_libraries.*nvvk|nvvk.*target_link_libraries",
        ):
            validate_editor_dependency_boundary(root)

    def test_rejects_macro_wrapped_dependency(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            macro(wire_native target dependency)
              target_link_libraries(${target} PRIVATE ${dependency})
            endmacro()
            wire_native(scene_editor_core nvvk)
            """,
            {"scene/editor/Core.cpp": "int editor_core_anchor = 0;\n"},
        )
        with self.assertRaisesRegex(
            ValidationError,
            r"(?i)target_link_libraries.*nvvk|nvvk.*target_link_libraries",
        ):
            validate_editor_dependency_boundary(root)

    def test_rejects_reachable_add_subdirectory_outside_fixed_roots(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            add_subdirectory(vendor/native)
            target_link_libraries(scene_editor_core PRIVATE platform_bridge)
            """,
            {
                "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                "vendor/native/CMakeLists.txt": (
                    "add_library(platform_bridge INTERFACE)\n"
                    "target_link_libraries(platform_bridge INTERFACE nvvk)\n"
                ),
            },
        )
        with self.assertRaisesRegex(
            ValidationError,
            r"(?i)(?=.*vendor/native)(?=.*target_link_libraries)(?=.*nvvk)",
        ):
            validate_editor_dependency_boundary(root)

    def test_rejects_reachable_include_outside_fixed_roots(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            include(config/wire-native.cmake)
            """,
            {
                "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                "config/wire-native.cmake": "target_link_libraries(scene_editor_core PRIVATE nvvk)\n",
            },
        )
        with self.assertRaisesRegex(
            ValidationError,
            r"(?i)(?=.*wire-native)(?=.*target_link_libraries)(?=.*nvvk)",
        ):
            validate_editor_dependency_boundary(root)

    def test_rejects_global_compile_option_forced_include(self) -> None:
        root = self.make_dependency_project(
            """
            add_compile_options(-include "${CMAKE_CURRENT_SOURCE_DIR}/vendor/nvvk/context.hpp")
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            """,
            {
                "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                "vendor/nvvk/context.hpp": "#pragma once\n",
            },
        )
        with self.assertRaisesRegex(ValidationError, r"(?i)nvvk.*add_compile_options|add_compile_options.*nvvk"):
            validate_editor_dependency_boundary(root)

    def test_rejects_source_property_compile_option(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            set_source_files_properties(scene/editor/Core.cpp PROPERTIES COMPILE_OPTIONS
              "-include;${CMAKE_CURRENT_SOURCE_DIR}/vendor/nvvk/context.hpp")
            """,
            {
                "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                "vendor/nvvk/context.hpp": "#pragma once\n",
            },
        )
        with self.assertRaisesRegex(
            ValidationError,
            r"(?i)nvvk.*set_source_files_properties|set_source_files_properties.*nvvk",
        ):
            validate_editor_dependency_boundary(root)

    def test_rejects_forbidden_dependency_in_neutral_header_include_closure(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            target_include_directories(scene_editor_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
            """,
            {
                "scene/editor/Core.cpp": (
                    "#include <render/PlatformBridge.h>\n"
                    "int editor_core_anchor = bridge_value();\n"
                ),
                "render/PlatformBridge.h": (
                    "#pragma once\n#include <vendor/nvvk/context.hpp>\ninline int bridge_value() { return 9; }\n"
                ),
                "vendor/nvvk/context.hpp": "#pragma once\n",
            },
        )
        with self.assertRaisesRegex(ValidationError, r"(?i)PlatformBridge.*nvvk|nvvk.*PlatformBridge"):
            validate_editor_dependency_boundary(root)

    def test_rejects_installed_latest_named_production_consumer(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            add_executable(latest_app app/Main.cpp)
            target_link_libraries(latest_app PRIVATE scene_editor_core nvvk)
            install(TARGETS latest_app RUNTIME DESTINATION bin)
            """,
            {
                "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                "app/Main.cpp": "int main() { return 0; }\n",
            },
        )
        with self.assertRaisesRegex(
            ValidationError,
            r"(?i)target_link_libraries.*nvvk|nvvk.*target_link_libraries",
        ):
            validate_editor_dependency_boundary(root)

    def test_rejects_non_allowlisted_imported_qt_ui(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            add_library(Qt6::Widgets INTERFACE IMPORTED)
            target_link_libraries(scene_editor_core PUBLIC Qt6::Widgets)
            """,
            {"scene/editor/Core.cpp": "int editor_core_anchor = 0;\n"},
        )
        with self.assertRaisesRegex(
            ValidationError,
            r"(?i)Qt6::Widgets.*allowedUiFoundation|allowedUiFoundation.*Qt6::Widgets",
        ):
            validate_editor_dependency_boundary(root)

    def test_rejects_computed_cmake_prefixed_dependency_variable(self) -> None:
        root = self.make_dependency_project(
            """
            string(CONCAT CMAKE_EDITOR_DEP nv vk)
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            target_link_libraries(scene_editor_core PRIVATE ${CMAKE_EDITOR_DEP})
            """,
            {"scene/editor/Core.cpp": "int editor_core_anchor = 0;\n"},
        )
        with self.assertRaisesRegex(
            ValidationError,
            r"(?i)target_link_libraries.*nvvk|nvvk.*target_link_libraries",
        ):
            validate_editor_dependency_boundary(root)

    def test_rejects_forbidden_source_from_sources_property(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            set_property(TARGET scene_editor_core APPEND PROPERTY SOURCES render/HiddenBridge.cpp)
            """,
            {
                "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                "render/HiddenBridge.cpp": "#include <vendor/nvvk/context.hpp>\n",
            },
        )
        with self.assertRaisesRegex(ValidationError, r"(?i)HiddenBridge.*nvvk|nvvk.*HiddenBridge"):
            validate_editor_dependency_boundary(root)

    def test_rejects_forbidden_source_from_interface_sources_property(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            add_library(platform_bridge INTERFACE)
            set_property(TARGET platform_bridge PROPERTY INTERFACE_SOURCES render/HiddenBridge.cpp)
            target_link_libraries(scene_editor_core PRIVATE platform_bridge)
            """,
            {
                "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                "render/HiddenBridge.cpp": "#include <vendor/nvvk/context.hpp>\n",
            },
        )
        with self.assertRaisesRegex(ValidationError, r"(?i)HiddenBridge.*nvvk|nvvk.*HiddenBridge"):
            validate_editor_dependency_boundary(root)

    def test_rejects_forbidden_public_header_property(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            set_target_properties(scene_editor_core PROPERTIES PUBLIC_HEADER include/mgif/scene/Controller.h)
            """,
            {
                "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                "include/mgif/scene/Controller.h": "#pragma once\n#include <vendor/nvvk/context.hpp>\n",
            },
        )
        with self.assertRaisesRegex(ValidationError, r"(?i)Controller.*nvvk|nvvk.*Controller"):
            validate_editor_dependency_boundary(root)

    def test_allows_sequential_set_overwrite(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(mgif_scene_adapter INTERFACE)
            set(EDITOR_LINK nvvk)
            set(EDITOR_LINK mgif_scene_adapter)
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            target_link_libraries(scene_editor_core PRIVATE ${EDITOR_LINK})
            """,
            {"scene/editor/Core.cpp": "int editor_core_anchor = 0;\n"},
        )
        validate_editor_dependency_boundary(root)

    def test_allows_non_dependency_folder_property(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            add_library(platform_bridge STATIC render/Bridge.cpp)
            add_library(nvvk INTERFACE)
            set_target_properties(scene_editor_core PROPERTIES FOLDER platform_bridge)
            target_link_libraries(platform_bridge PRIVATE nvvk)
            """,
            {
                "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                "render/Bridge.cpp": "int bridge_anchor = 0;\n",
            },
        )
        validate_editor_dependency_boundary(root)

    def test_rejects_directory_compile_option_forced_include(self) -> None:
        root = self.make_dependency_project(
            """
            set_directory_properties(PROPERTIES COMPILE_OPTIONS
              "-include;${CMAKE_CURRENT_SOURCE_DIR}/vendor/nvvk/context.hpp")
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            """,
            {
                "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                "vendor/nvvk/context.hpp": "#pragma once\n",
            },
        )
        with self.assertRaisesRegex(
            ValidationError,
            r"(?i)nvvk.*set_directory_properties|set_directory_properties.*nvvk",
        ):
            validate_editor_dependency_boundary(root)

    def test_rejects_dynamic_cmake_evaluation_in_editor_scope(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            cmake_language(EVAL CODE "target_link_libraries(scene_editor_core PRIVATE nvvk)")
            """,
            {"scene/editor/Core.cpp": "int editor_core_anchor = 0;\n"},
        )
        with self.assertRaisesRegex(ValidationError, r"(?i)unsupported dynamic CMake evaluation"):
            validate_editor_dependency_boundary(root)

    def test_allows_dear_imgui_and_repository_owned_adapter_foundation(self) -> None:
        root = self.make_dependency_project(
            """
            include(FetchContent)
            FetchContent_Declare(imgui
              GIT_REPOSITORY https://github.com/ocornut/imgui.git
              GIT_TAG fd13a1e8923a0a7077b404fc36fd063b25a0c0b5)
            FetchContent_MakeAvailable(imgui)
            add_library(dear_imgui_upstream STATIC
              ${imgui_SOURCE_DIR}/imgui.cpp
              ${imgui_SOURCE_DIR}/imgui.h
              ${imgui_SOURCE_DIR}/imgui_draw.cpp
              ${imgui_SOURCE_DIR}/imgui_tables.cpp
              ${imgui_SOURCE_DIR}/imgui_widgets.cpp)
            add_library(DearImGui::DearImGui ALIAS dear_imgui_upstream)
            add_library(mgif_scene_adapter INTERFACE)
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            target_link_libraries(scene_editor_core PRIVATE mgif_scene_adapter DearImGui::DearImGui)
            """,
            {
                "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
            },
        )
        validate_editor_dependency_boundary(root)

    def test_imgui_glfw_backend_is_attributed_to_imgui_not_production_foundation(self) -> None:
        foundation = """
            include(FetchContent)
            FetchContent_Declare(glfw
              GIT_REPOSITORY https://github.com/glfw/glfw.git
              GIT_TAG 3.4)
            FetchContent_MakeAvailable(glfw)
            FetchContent_Declare(imgui
              GIT_REPOSITORY https://github.com/ocornut/imgui.git
              GIT_TAG fd13a1e8923a0a7077b404fc36fd063b25a0c0b5)
            FetchContent_MakeAvailable(imgui)
            add_library(imgui_demo STATIC
              ${imgui_SOURCE_DIR}/imgui.cpp
              ${imgui_SOURCE_DIR}/imgui.h
              ${imgui_SOURCE_DIR}/imgui_demo.cpp
              ${imgui_SOURCE_DIR}/imgui_draw.cpp
              ${imgui_SOURCE_DIR}/imgui_tables.cpp
              ${imgui_SOURCE_DIR}/imgui_widgets.cpp)
            target_sources(imgui_demo PRIVATE
              ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
              ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.h)
            target_link_libraries(imgui_demo PUBLIC glfw)
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            target_link_libraries(scene_editor_core PRIVATE imgui_demo)
        """
        files = {"scene/editor/Core.cpp": "int editor_core_anchor = 0;\n"}

        valid = self.make_dependency_project(foundation, files)
        model = validate_editor_dependency_boundary(valid)
        self.assertIn("imgui_demo", model["protectedTargets"])

        hostile_cases = {
            "glfw-direct-to-editor": (
                "target_link_libraries(scene_editor_core PRIVATE glfw)",
                r"(?i)glfw.*outside allowedUiFoundation|outside allowedUiFoundation.*glfw",
            ),
            "forbidden-direct-to-editor": (
                "target_link_libraries(scene_editor_core PRIVATE nvvk)",
                r"(?i)forbidden editor dependency.*nvvk|nvvk.*forbidden editor dependency",
            ),
            "forbidden-transitive-through-closure": (
                "add_library(forbidden_bridge INTERFACE)\n"
                "target_link_libraries(forbidden_bridge INTERFACE nvapp)\n"
                "target_link_libraries(scene_editor_core PRIVATE forbidden_bridge)",
                r"(?i)forbidden editor dependency.*nvapp|nvapp.*forbidden editor dependency",
            ),
        }
        for name, (hostile_edge, message_pattern) in hostile_cases.items():
            with self.subTest(case=name):
                hostile = self.make_dependency_project(f"{foundation}\n{hostile_edge}", files)
                with self.assertRaisesRegex(ValidationError, message_pattern):
                    validate_editor_dependency_boundary(hostile)

    def test_demo_core_glfw_requires_imgui_authority_and_exact_desktop_edge(self) -> None:
        foundation = """
            include(FetchContent)
            FetchContent_Declare(glfw
              GIT_REPOSITORY https://github.com/glfw/glfw.git
              GIT_TAG 3.4)
            FetchContent_MakeAvailable(glfw)
            FetchContent_Declare(imgui
              GIT_REPOSITORY https://github.com/ocornut/imgui.git
              GIT_TAG fd13a1e8923a0a7077b404fc36fd063b25a0c0b5)
            FetchContent_MakeAvailable(imgui)
            add_library(imgui_demo STATIC
              ${imgui_SOURCE_DIR}/imgui.cpp
              ${imgui_SOURCE_DIR}/imgui.h
              ${imgui_SOURCE_DIR}/imgui_demo.cpp
              ${imgui_SOURCE_DIR}/imgui_draw.cpp
              ${imgui_SOURCE_DIR}/imgui_tables.cpp
              ${imgui_SOURCE_DIR}/imgui_widgets.cpp)
            target_sources(imgui_demo PRIVATE
              ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
              ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.h)
            target_link_libraries(imgui_demo PUBLIC glfw)
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            add_library(demo_core STATIC render/editor/DemoCore.cpp)
            target_link_libraries(scene_editor_core PRIVATE demo_core)
            if(ANDROID)
            else()
              target_link_libraries(demo_core PUBLIC glfw)
            endif()
        """
        files = {
            "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
            "render/editor/DemoCore.cpp": "int demo_core_anchor = 0;\n",
        }
        validate_editor_dependency_boundary(
            self.make_dependency_project(foundation, files)
        )

        hostile_cases = (
            foundation + "\ntarget_link_libraries(demo_core PRIVATE glfw)",
            foundation.replace(
                "else()\n              target_link_libraries(demo_core PUBLIC glfw)",
                "endif()\n            target_link_libraries(demo_core PUBLIC glfw)\n            if(FALSE)",
            ),
            foundation.replace(
                "target_link_libraries(demo_core PUBLIC glfw)",
                "target_link_libraries(scene_editor_core PUBLIC glfw)",
            ),
        )
        for index, hostile_cmake in enumerate(hostile_cases):
            with self.subTest(case=index):
                with self.assertRaisesRegex(
                    ValidationError,
                    r"(?i)glfw.*outside allowedUiFoundation|outside allowedUiFoundation.*glfw",
                ):
                    validate_editor_dependency_boundary(
                        self.make_dependency_project(hostile_cmake, files)
                    )

    def test_demo_core_apple_frameworks_require_exact_guard_owner_and_provenance(self) -> None:
        canonical_platform_block = """
            if(APPLE)
              find_library(DEMO_METAL_FRAMEWORK Metal REQUIRED)
              find_library(DEMO_FOUNDATION_FRAMEWORK Foundation REQUIRED)
              target_link_libraries(demo_core PUBLIC
                "${DEMO_METAL_FRAMEWORK}"
                "${DEMO_FOUNDATION_FRAMEWORK}")
            endif()
        """
        foundation = """
            add_library(demo_core STATIC rhi/DemoCore.cpp)
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            target_link_libraries(scene_editor_core PRIVATE demo_core)
        """
        files = {
            "rhi/DemoCore.cpp": "int demo_core_anchor = 0;\n",
            "rhi/PlatformBridge.cpp": "int platform_bridge_anchor = 0;\n",
            "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
        }

        valid = self.make_dependency_project(foundation + canonical_platform_block, files)
        result = validate_editor_dependency_boundary(valid)
        self.assertIn("demo_core", result["protectedTargets"])

        hostile_projects = {
            "unguarded": foundation + """
                find_library(DEMO_METAL_FRAMEWORK Metal REQUIRED)
                find_library(DEMO_FOUNDATION_FRAMEWORK Foundation REQUIRED)
                target_link_libraries(demo_core PUBLIC
                  "${DEMO_METAL_FRAMEWORK}"
                  "${DEMO_FOUNDATION_FRAMEWORK}")
            """,
            "wrong-platform-guard": foundation + canonical_platform_block.replace("if(APPLE)", "if(WIN32)"),
            "forged-apple-condition": "set(APPLE ON)\n" + foundation + canonical_platform_block,
            "apple-else-arm": foundation + """
                if(APPLE)
                  message(STATUS "canonical branch intentionally empty")
                else()
                  find_library(DEMO_METAL_FRAMEWORK Metal REQUIRED)
                  find_library(DEMO_FOUNDATION_FRAMEWORK Foundation REQUIRED)
                  target_link_libraries(demo_core PUBLIC
                    "${DEMO_METAL_FRAMEWORK}"
                    "${DEMO_FOUNDATION_FRAMEWORK}")
                endif()
            """,
            "arbitrary-link-variable": foundation
            + canonical_platform_block.replace(
                "DEMO_FOUNDATION_FRAMEWORK Foundation REQUIRED",
                "DEMO_QUARTZ_FRAMEWORK QuartzCore REQUIRED",
            ).replace('"${DEMO_FOUNDATION_FRAMEWORK}"', '"${DEMO_QUARTZ_FRAMEWORK}"'),
            "other-production-closure-owner": foundation + """
                add_library(platform_bridge STATIC rhi/PlatformBridge.cpp)
                target_link_libraries(demo_core PRIVATE platform_bridge)
                if(APPLE)
                  find_library(DEMO_METAL_FRAMEWORK Metal REQUIRED)
                  find_library(DEMO_FOUNDATION_FRAMEWORK Foundation REQUIRED)
                  target_link_libraries(platform_bridge PUBLIC
                    "${DEMO_METAL_FRAMEWORK}"
                    "${DEMO_FOUNDATION_FRAMEWORK}")
                endif()
            """,
            "direct-editor-owner": foundation + """
                if(APPLE)
                  find_library(DEMO_METAL_FRAMEWORK Metal REQUIRED)
                  find_library(DEMO_FOUNDATION_FRAMEWORK Foundation REQUIRED)
                  target_link_libraries(scene_editor_core PUBLIC
                    "${DEMO_METAL_FRAMEWORK}"
                    "${DEMO_FOUNDATION_FRAMEWORK}")
                endif()
            """,
            "unguarded-direct-platform-edge": foundation + """
                target_link_libraries(scene_editor_core PRIVATE "-framework" Metal)
            """,
            "altered-find-library-provenance": foundation + canonical_platform_block.replace(
                "DEMO_METAL_FRAMEWORK Metal REQUIRED",
                "DEMO_METAL_FRAMEWORK MetalKit REQUIRED",
            ),
            "duplicate-framework-edge": foundation + canonical_platform_block + """
                if(APPLE)
                  target_link_libraries(demo_core PUBLIC "${DEMO_METAL_FRAMEWORK}")
                endif()
            """,
            "forged-demo-core-alias": """
                add_library(platform_bridge STATIC rhi/PlatformBridge.cpp)
                add_library(demo_core ALIAS platform_bridge)
                add_library(scene_editor_core STATIC scene/editor/Core.cpp)
                target_link_libraries(scene_editor_core PRIVATE demo_core)
            """ + canonical_platform_block,
        }
        for name, cmake in hostile_projects.items():
            with self.subTest(case=name):
                hostile = self.make_dependency_project(cmake, files)
                with self.assertRaisesRegex(
                    ValidationError,
                    r"(?i)(?:cannot resolve editor dependency|outside allowedUiFoundation|APPLE guard provenance)",
                ):
                    validate_editor_dependency_boundary(hostile)

    def test_demo_core_apple_frameworks_reject_all_link_channels_and_encodings(self) -> None:
        foundation = """
            add_library(demo_core STATIC rhi/DemoCore.cpp)
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            target_link_libraries(scene_editor_core PRIVATE demo_core)
        """
        canonical_platform_block = """
            if(APPLE)
              find_library(DEMO_METAL_FRAMEWORK Metal REQUIRED)
              find_library(DEMO_FOUNDATION_FRAMEWORK Foundation REQUIRED)
              target_link_libraries(demo_core PUBLIC
                "${DEMO_METAL_FRAMEWORK}"
                "${DEMO_FOUNDATION_FRAMEWORK}")
            endif()
        """
        files = {
            "rhi/DemoCore.cpp": "int demo_core_anchor = 0;\n",
            "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
        }
        cases = {
            "global-link-libraries-metal": 'link_libraries("-framework" Metal)',
            "global-add-link-options-metal": 'add_link_options("-framework" Metal)',
            "directory-link-libraries-metal": (
                'set_property(DIRECTORY PROPERTY LINK_LIBRARIES "-framework;Metal")'
            ),
            "target-link-single-token-metal": (
                'target_link_libraries(scene_editor_core PRIVATE "-framework Metal")'
            ),
            "target-link-options-property-metal": (
                'set_property(TARGET scene_editor_core PROPERTY LINK_OPTIONS "-framework Metal")'
            ),
            "target-link-flags-metal": (
                'set_property(TARGET scene_editor_core PROPERTY LINK_FLAGS "-framework Metal")'
            ),
            "global-link-libraries-foundation": 'link_libraries("-framework" Foundation)',
            "global-add-link-options-foundation": 'add_link_options("-framework" Foundation)',
            "directory-link-libraries-foundation": (
                'set_property(DIRECTORY PROPERTY LINK_LIBRARIES "-framework;Foundation")'
            ),
            "target-link-single-token-foundation": (
                'target_link_libraries(scene_editor_core PRIVATE "-framework Foundation")'
            ),
            "target-link-options-property-foundation": (
                'set_property(TARGET scene_editor_core PROPERTY LINK_OPTIONS "-framework Foundation")'
            ),
            "target-link-flags-foundation": (
                'set_property(TARGET scene_editor_core PROPERTY LINK_FLAGS "-framework Foundation")'
            ),
            "target-linker-prefix-metal": (
                'target_link_options(scene_editor_core PRIVATE "LINKER:-framework,Metal")'
            ),
            "target-shell-framework-foundation": (
                'target_link_options(scene_editor_core PRIVATE "SHELL:-framework Foundation")'
            ),
            "target-wl-framework-metal": (
                'target_link_libraries(scene_editor_core PRIVATE "-Wl,-framework,Metal")'
            ),
            "global-linker-prefix-foundation": (
                'add_link_options("LINKER:-framework,Foundation")'
            ),
            "target-single-token-weak-framework-metal": (
                'target_link_libraries(scene_editor_core PRIVATE "-weak_framework Metal")'
            ),
            "target-cxx-linker-launcher-metal": (
                'set_property(TARGET scene_editor_core PROPERTY CXX_LINKER_LAUNCHER '
                '"${CMAKE_COMMAND};-E;echo;-framework;Metal")'
            ),
            "target-rule-launch-link-foundation": (
                'set_property(TARGET scene_editor_core PROPERTY RULE_LAUNCH_LINK '
                '"${CMAKE_COMMAND} -E echo -framework Foundation")'
            ),
            "directory-rule-launch-link-metal": (
                'set_property(DIRECTORY PROPERTY RULE_LAUNCH_LINK '
                '"${CMAKE_COMMAND} -E echo -framework Metal")'
            ),
            "global-rule-launch-link-foundation": (
                'set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK '
                '"${CMAKE_COMMAND} -E echo -framework Foundation")'
            ),
            "cmake-cxx-link-executable-metal": (
                'set(CMAKE_CXX_LINK_EXECUTABLE '
                '"${CMAKE_COMMAND} -E echo -framework Metal")'
            ),
            "cmake-cxx-linker-launcher-foundation": (
                'set(CMAKE_CXX_LINKER_LAUNCHER '
                '"${CMAKE_COMMAND};-E;echo;-framework;Foundation")'
            ),
            "cmake-exe-linker-flags-metal": (
                'set(CMAKE_EXE_LINKER_FLAGS "-framework Metal")'
            ),
            "cmake-exe-linker-flags-string-append-foundation": (
                'string(APPEND CMAKE_EXE_LINKER_FLAGS " -framework Foundation")'
            ),
            "cmake-cxx-standard-libraries-metal": (
                'set(CMAKE_CXX_STANDARD_LIBRARIES "-framework Metal")'
            ),
        }
        global_prefixes = (
            "link_libraries(",
            "add_link_options(",
            "set_property(DIRECTORY",
            "set_property(GLOBAL",
            "set(CMAKE_",
            "string(",
        )
        for name, channel in cases.items():
            with self.subTest(case=name):
                cmake = (
                    f"{channel}\n{foundation}{canonical_platform_block}"
                    if channel.startswith(global_prefixes)
                    else f"{foundation}\n{channel}\n{canonical_platform_block}"
                )
                hostile = self.make_dependency_project(cmake, files)
                with self.assertRaisesRegex(
                    ValidationError,
                    r"(?i)(?:framework|outside allowedUiFoundation)",
                ):
                    validate_editor_dependency_boundary(hostile)

    def test_framework_link_channel_fixtures_propagate_in_real_cmake(self) -> None:
        cmake_executable = shutil.which("cmake")
        ninja_executable = shutil.which("ninja")
        if cmake_executable is None or ninja_executable is None:
            self.skipTest("CMake and Ninja are required for framework propagation controls")
        probes = (
            ("global-link-libraries-metal", 'link_libraries("-framework" Metal)', "", ("-framework", "Metal")),
            ("global-add-link-options-metal", 'add_link_options("-framework" Metal)', "", ("-framework", "Metal")),
            ("directory-link-libraries-metal", 'set_property(DIRECTORY PROPERTY LINK_LIBRARIES "-framework;Metal")', "", ("-framework", "Metal")),
            ("target-link-single-token-metal", "", 'target_link_libraries(scene_editor_core PRIVATE "-framework Metal")', ("-framework Metal",)),
            ("target-link-options-property-metal", "", 'set_property(TARGET scene_editor_core PROPERTY LINK_OPTIONS "-framework Metal")', ("-framework Metal",)),
            ("target-link-flags-metal", "", 'set_property(TARGET scene_editor_core PROPERTY LINK_FLAGS "-framework Metal")', ("-framework Metal",)),
            ("global-link-libraries-foundation", 'link_libraries("-framework" Foundation)', "", ("-framework", "Foundation")),
            ("global-add-link-options-foundation", 'add_link_options("-framework" Foundation)', "", ("-framework", "Foundation")),
            ("directory-link-libraries-foundation", 'set_property(DIRECTORY PROPERTY LINK_LIBRARIES "-framework;Foundation")', "", ("-framework", "Foundation")),
            ("target-link-single-token-foundation", "", 'target_link_libraries(scene_editor_core PRIVATE "-framework Foundation")', ("-framework Foundation",)),
            ("target-link-options-property-foundation", "", 'set_property(TARGET scene_editor_core PROPERTY LINK_OPTIONS "-framework Foundation")', ("-framework Foundation",)),
            ("target-link-flags-foundation", "", 'set_property(TARGET scene_editor_core PROPERTY LINK_FLAGS "-framework Foundation")', ("-framework Foundation",)),
            # The LINKER: prefix is the only channel whose spelling is toolchain
            # defined: GNU/Clang wrap it as -Wl,<items> while MSVC has no wrapper
            # and passes the items through space separated. Measured on this host
            # with cmake -G Ninja and ninja -t commands (G:\Temp\lp2.txt):
            #   target_link_options(... "LINKER:-framework,Metal")
            #   -> PROBE_OPTIONS='LINKER:-framework,Metal'
            #   -> link line "... /subsystem:console -framework Metal kernel32.lib ..."
            # Both spellings are the same propagation fact, so either one proves
            # the channel reached the real link command; a host that produces
            # neither still fails.
            ("target-linker-prefix-metal", "", 'target_link_options(scene_editor_core PRIVATE "LINKER:-framework,Metal")', (("-Wl,-framework,Metal", "-framework Metal"),)),
            ("target-shell-framework-foundation", "", 'target_link_options(scene_editor_core PRIVATE "SHELL:-framework Foundation")', ("-framework Foundation",)),
            ("target-wl-framework-metal", "", 'target_link_libraries(scene_editor_core PRIVATE "-Wl,-framework,Metal")', ("-Wl,-framework,Metal",)),
            ("global-linker-prefix-foundation", 'add_link_options("LINKER:-framework,Foundation")', "", (("-Wl,-framework,Foundation", "-framework Foundation"),)),
            ("target-single-token-weak-framework-metal", "", 'target_link_libraries(scene_editor_core PRIVATE "-weak_framework Metal")', ("-weak_framework Metal",)),
            (
                "target-cxx-linker-launcher-metal",
                "",
                'set_property(TARGET scene_editor_core PROPERTY CXX_LINKER_LAUNCHER '
                '"${CMAKE_COMMAND};-E;echo;-framework;Metal")',
                ("-framework", "Metal"),
            ),
            (
                "target-rule-launch-link-foundation",
                "",
                'set_property(TARGET scene_editor_core PROPERTY RULE_LAUNCH_LINK '
                '"${CMAKE_COMMAND} -E echo -framework Foundation")',
                ("-framework", "Foundation"),
            ),
            (
                "directory-rule-launch-link-metal",
                'set_property(DIRECTORY PROPERTY RULE_LAUNCH_LINK '
                '"${CMAKE_COMMAND} -E echo -framework Metal")',
                "",
                ("-framework", "Metal"),
            ),
            (
                "global-rule-launch-link-foundation",
                'set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK '
                '"${CMAKE_COMMAND} -E echo -framework Foundation")',
                "",
                ("-framework", "Foundation"),
            ),
            (
                "cmake-cxx-link-executable-metal",
                'set(CMAKE_CXX_LINK_EXECUTABLE '
                '"${CMAKE_COMMAND} -E echo -framework Metal")',
                "",
                ("-framework", "Metal"),
            ),
            (
                "cmake-cxx-linker-launcher-foundation",
                'set(CMAKE_CXX_LINKER_LAUNCHER '
                '"${CMAKE_COMMAND};-E;echo;-framework;Foundation")',
                "",
                ("-framework", "Foundation"),
            ),
        )
        template = """cmake_minimum_required(VERSION 3.25)
project(framework_channel_regression CXX)
{before}
add_executable(scene_editor_core scene_editor_core.cpp)
{after}
get_target_property(PROBE_LINKS scene_editor_core LINK_LIBRARIES)
get_target_property(PROBE_OPTIONS scene_editor_core LINK_OPTIONS)
get_target_property(PROBE_FLAGS scene_editor_core LINK_FLAGS)
get_directory_property(PROBE_DIRECTORY_LINKS LINK_LIBRARIES)
get_directory_property(PROBE_DIRECTORY_OPTIONS LINK_OPTIONS)
message(STATUS "PROBE_LINKS='${{PROBE_LINKS}}'")
message(STATUS "PROBE_OPTIONS='${{PROBE_OPTIONS}}'")
message(STATUS "PROBE_FLAGS='${{PROBE_FLAGS}}'")
message(STATUS "PROBE_DIRECTORY_LINKS='${{PROBE_DIRECTORY_LINKS}}'")
message(STATUS "PROBE_DIRECTORY_OPTIONS='${{PROBE_DIRECTORY_OPTIONS}}'")
"""
        for probe_index, (name, before, after, expected) in enumerate(probes):
            with self.subTest(case=name):
                source = self.root / "fp" / f"{probe_index:02d}" / "s"
                build = self.root / "fp" / f"{probe_index:02d}" / "b"
                source.mkdir(parents=True)
                (source / "scene_editor_core.cpp").write_text(
                    "int main() { return 0; }\n",
                    encoding="utf-8",
                )
                (source / "CMakeLists.txt").write_text(
                    template.format(before=before, after=after),
                    encoding="utf-8",
                )
                environment = os.environ.copy()
                environment["TEMP"] = os.environ["TEMP"]
                environment["TMP"] = os.environ["TMP"]
                completed = subprocess.run(
                    [cmake_executable, "-S", str(source), "-B", str(build), "-G", "Ninja"],
                    check=False,
                    capture_output=True,
                    text=True,
                    encoding="utf-8",
                    errors="replace",
                    env=environment,
                )
                output = completed.stdout + completed.stderr
                property_text = "\n".join(line for line in output.splitlines() if "PROBE_" in line)
                self.assertEqual(completed.returncode, 0, output)
                commands = subprocess.run(
                    [ninja_executable, "-C", str(build), "-t", "commands", "scene_editor_core"],
                    check=False,
                    capture_output=True,
                    text=True,
                    encoding="utf-8",
                    errors="replace",
                    env=environment,
                )
                command_output = commands.stdout + commands.stderr
                self.assertEqual(commands.returncode, 0, command_output)
                for expected_text in expected:
                    alternatives = expected_text if isinstance(expected_text, tuple) else (expected_text,)
                    self.assertTrue(
                        any(alternative in command_output for alternative in alternatives),
                        f"none of {alternatives} reached the real link command: {command_output}",
                    )

    def test_demo_core_apple_frameworks_reject_every_guard_writer(self) -> None:
        canonical_platform_block = """
            if(APPLE)
              find_library(DEMO_METAL_FRAMEWORK Metal REQUIRED)
              find_library(DEMO_FOUNDATION_FRAMEWORK Foundation REQUIRED)
              target_link_libraries(demo_core PUBLIC
                "${DEMO_METAL_FRAMEWORK}"
                "${DEMO_FOUNDATION_FRAMEWORK}")
            endif()
        """
        foundation = """
            add_library(demo_core STATIC rhi/DemoCore.cpp)
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            target_link_libraries(scene_editor_core PRIVATE demo_core)
        """
        files = {
            "rhi/DemoCore.cpp": "int demo_core_anchor = 0;\n",
            "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
        }
        hostile_tails: dict[str, tuple[str, dict[str, str]]] = {
            "cache-set-property": (
                'set(APPLE OFF CACHE BOOL "probe" FORCE)\n'
                "set_property(CACHE APPLE PROPERTY VALUE ON)\n"
                + canonical_platform_block,
                {},
            ),
            "find-library": ("find_library(APPLE Metal REQUIRED)\n" + canonical_platform_block, {}),
            "file-read": ("file(READ payload.txt APPLE)\n" + canonical_platform_block, {"payload.txt": "ON\n"}),
            "execute-process": (
                "execute_process(COMMAND cmake -E echo ON OUTPUT_VARIABLE APPLE)\n"
                + canonical_platform_block,
                {},
            ),
            "math": ('math(EXPR APPLE "1")\n' + canonical_platform_block, {}),
            "get-filename-component": (
                "get_filename_component(APPLE ON NAME)\n" + canonical_platform_block,
                {},
            ),
            "file-glob": (
                "file(GLOB APPLE payload.txt)\n" + canonical_platform_block,
                {"payload.txt": "probe\n"},
            ),
            "get-property": (
                "set_property(GLOBAL PROPERTY HOSTILE_GUARD ON)\n"
                "get_property(APPLE GLOBAL PROPERTY HOSTILE_GUARD)\n"
                + canonical_platform_block,
                {},
            ),
            "foreach-loop-variable": (
                "foreach(APPLE IN ITEMS ON)\n"
                + canonical_platform_block
                + "\nendforeach()",
                {},
            ),
            "function-return-propagate": (
                "function(forge_guard APPLE)\n"
                "  return(PROPAGATE APPLE)\n"
                "endfunction()\n"
                "forge_guard(ON)\n"
                + canonical_platform_block,
                {},
            ),
            "find-file": (
                'find_file(APPLE NAMES payload.txt PATHS "${CMAKE_CURRENT_SOURCE_DIR}" NO_DEFAULT_PATH)\n'
                + canonical_platform_block,
                {"payload.txt": "probe\n"},
            ),
            "find-path": (
                'find_path(APPLE NAMES payload.h PATHS "${CMAKE_CURRENT_SOURCE_DIR}" NO_DEFAULT_PATH)\n'
                + canonical_platform_block,
                {"payload.h": "probe\n"},
            ),
            "find-program": ("find_program(APPLE NAMES cmake)\n" + canonical_platform_block, {}),
            "file-strings": (
                "file(STRINGS payload.txt APPLE)\n" + canonical_platform_block,
                {"payload.txt": "ON\n"},
            ),
            "file-size": (
                "file(SIZE payload.txt APPLE)\n" + canonical_platform_block,
                {"payload.txt": "probe\n"},
            ),
            "file-relative-path": (
                'file(RELATIVE_PATH APPLE "${CMAKE_CURRENT_SOURCE_DIR}" '
                '"${CMAKE_CURRENT_SOURCE_DIR}/payload.txt")\n'
                + canonical_platform_block,
                {"payload.txt": "probe\n"},
            ),
            "get-cmake-property": (
                "get_cmake_property(APPLE VARIABLES)\n" + canonical_platform_block,
                {},
            ),
            "get-directory-property": (
                "get_directory_property(APPLE DEFINITION CMAKE_MINIMUM_REQUIRED_VERSION)\n"
                + canonical_platform_block,
                {},
            ),
            "get-target-property": (
                "get_target_property(APPLE demo_core TYPE)\n" + canonical_platform_block,
                {},
            ),
        }
        for name, (tail, extra_files) in hostile_tails.items():
            with self.subTest(case=name):
                hostile = self.make_dependency_project(foundation + tail, files | extra_files)
                with self.assertRaisesRegex(ValidationError, r"(?i)APPLE guard provenance"):
                    validate_editor_dependency_boundary(hostile)

    def test_demo_core_apple_frameworks_reject_computed_guard_and_provenance_bypasses(self) -> None:
        canonical_platform_block = """
            if(APPLE)
              find_library(DEMO_METAL_FRAMEWORK Metal REQUIRED)
              find_library(DEMO_FOUNDATION_FRAMEWORK Foundation REQUIRED)
              target_link_libraries(demo_core PUBLIC
                "${DEMO_METAL_FRAMEWORK}"
                "${DEMO_FOUNDATION_FRAMEWORK}")
            endif()
        """
        foundation = """
            add_library(demo_core STATIC rhi/DemoCore.cpp)
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            target_link_libraries(scene_editor_core PRIVATE demo_core)
        """
        files = {
            "rhi/DemoCore.cpp": "int demo_core_anchor = 0;\n",
            "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
        }
        hostile_tails = {
            "bracket-quoted-guard": "find_library([=[APPLE]=] Metal REQUIRED)\n" + canonical_platform_block,
            "computed-guard-destination": (
                "set(GUARD_DESTINATION AP)\n"
                "string(APPEND GUARD_DESTINATION PLE)\n"
                'math(EXPR ${GUARD_DESTINATION} "1")\n'
                + canonical_platform_block
            ),
            "computed-function-propagation": (
                "function(forge_guard DESTINATION)\n"
                '  math(EXPR ${DESTINATION} "1")\n'
                "  return(PROPAGATE ${DESTINATION})\n"
                "endfunction()\n"
                "set(GUARD_DESTINATION AP)\n"
                "string(APPEND GUARD_DESTINATION PLE)\n"
                "forge_guard(${GUARD_DESTINATION})\n"
                + canonical_platform_block
            ),
            "computed-macro-mutation": (
                "macro(forge_guard DESTINATION)\n"
                "  set(${DESTINATION} ON)\n"
                "endmacro()\n"
                "set(GUARD_DESTINATION AP)\n"
                "string(APPEND GUARD_DESTINATION PLE)\n"
                "forge_guard(${GUARD_DESTINATION})\n"
                + canonical_platform_block
            ),
            "unsupported-side-effect-command": "mutate_platform_guard(APPLE)\n" + canonical_platform_block,
        }
        for name, tail in hostile_tails.items():
            with self.subTest(case=name):
                hostile = self.make_dependency_project(foundation + tail, files)
                with self.assertRaisesRegex(ValidationError, r"(?i)APPLE guard provenance"):
                    validate_editor_dependency_boundary(hostile)

        included_guard_writer = self.make_dependency_project(
            foundation + "include(cmake/GuardWriter.cmake)\n" + canonical_platform_block,
            files
            | {
                "cmake/GuardWriter.cmake": (
                    "set(GUARD_DESTINATION AP)\n"
                    "string(APPEND GUARD_DESTINATION PLE)\n"
                    'math(EXPR ${GUARD_DESTINATION} "1")\n'
                ),
            },
        )
        with self.assertRaisesRegex(ValidationError, r"(?i)APPLE guard provenance"):
            validate_editor_dependency_boundary(included_guard_writer)

        computed_framework_producer = self.make_dependency_project(
            foundation
            + """
                set(FRAMEWORK_DESTINATION DEMO_METAL)
                string(APPEND FRAMEWORK_DESTINATION _FRAMEWORK)
                find_library(${FRAMEWORK_DESTINATION} Metal REQUIRED)
            """
            + canonical_platform_block,
            files,
        )
        with self.assertRaisesRegex(
            ValidationError,
            r"(?i)(?:outside allowedUiFoundation|framework.*provenance)",
        ):
            validate_editor_dependency_boundary(computed_framework_producer)

        nonmutating_platform_read = self.make_dependency_project(
            foundation
            + """
                if(UNIX AND NOT APPLE AND NOT ANDROID)
                  message(STATUS "non-Apple platform branch")
                endif()
            """
            + canonical_platform_block,
            files,
        )
        result = validate_editor_dependency_boundary(nonmutating_platform_read)
        self.assertIn("demo_core", result["protectedTargets"])

    def test_demo_core_apple_frameworks_do_not_mask_forbidden_closure_edges(self) -> None:
        foundation = """
            add_library(demo_core STATIC rhi/DemoCore.cpp)
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            target_link_libraries(scene_editor_core PRIVATE demo_core)
            if(APPLE)
              find_library(DEMO_METAL_FRAMEWORK Metal REQUIRED)
              find_library(DEMO_FOUNDATION_FRAMEWORK Foundation REQUIRED)
              target_link_libraries(demo_core PUBLIC
                "${DEMO_METAL_FRAMEWORK}"
                "${DEMO_FOUNDATION_FRAMEWORK}")
            endif()
        """
        files = {
            "rhi/DemoCore.cpp": "int demo_core_anchor = 0;\n",
            "rhi/PlatformBridge.cpp": "int platform_bridge_anchor = 0;\n",
            "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
        }
        for forbidden in ("nvpro_core", "nvcore", "nvapp", "nvgui", "nvvk", "nvvkgltf", "nvh"):
            with self.subTest(edge="direct", dependency=forbidden):
                hostile = self.make_dependency_project(
                    foundation + f"\ntarget_link_libraries(demo_core PRIVATE {forbidden})",
                    files,
                )
                with self.assertRaisesRegex(ValidationError, rf"(?i)forbidden editor dependency.*{forbidden}"):
                    validate_editor_dependency_boundary(hostile)

        transitive = self.make_dependency_project(
            foundation + """
                add_library(platform_bridge STATIC rhi/PlatformBridge.cpp)
                target_link_libraries(demo_core PRIVATE platform_bridge)
                target_link_libraries(platform_bridge PRIVATE nvapp)
            """,
            files,
        )
        with self.assertRaisesRegex(ValidationError, r"(?i)forbidden editor dependency.*nvapp"):
            validate_editor_dependency_boundary(transitive)

    def test_demo_core_volk_requires_exact_fetch_target_and_common_renderer_edge(self) -> None:
        canonical_edge = """
          target_link_libraries(demo_core
            PUBLIC
              scene_editor_core
              volk
              imgui_demo
              glm
              tinygltf
              meshoptimizer
              Vulkan::Vulkan)
        """
        # TinyGLTF's separate document-only authority would intentionally be
        # the next policy decision on the real full list. Rename that sentinel
        # only in this unit test so it isolates the Volk classifier itself.
        with patch("validate_editor_p0_artifacts.EXPECTED_TINYGLTF_TARGET", "isolated_tinygltf_authority"):
            valid = self.make_volk_dependency_project(authority_edge=canonical_edge)
            model = validate_editor_dependency_boundary(valid)
        self.assertIn("demo_core", model["protectedTargets"])
        authority = model["repositoryExternalFoundations"]["volk"]
        self.assertEqual(
            {key: authority[key] for key in ("target", "repository", "tag", "owner")},
            {
                "target": "volk",
                "repository": "https://github.com/zeux/volk.git",
                "tag": "master",
                "owner": "demo_core",
            },
        )
        self.assertLess(authority["declarationLine"], authority["populationLine"])
        self.assertLess(authority["populationLine"], authority["edgeLine"])

        canonical_declaration = """
          FetchContent_Declare(volk
            GIT_REPOSITORY https://github.com/zeux/volk.git
            GIT_TAG master)
        """
        hostile_cases = {
            "changed-repository": {
                "volk_declaration": canonical_declaration.replace(
                    "https://github.com/zeux/volk.git",
                    "https://github.com/example/volk.git",
                ),
            },
            "changed-tag": {
                "volk_declaration": canonical_declaration.replace("GIT_TAG master", "GIT_TAG 1.4.304"),
            },
            "repository-local-forgery": {
                "before_edge": "add_library(volk STATIC external/volk/volk.c)",
                "extra_files": {"external/volk/volk.c": "int forged_volk = 0;\n"},
            },
            "imported-forgery": {
                "before_edge": "add_library(volk STATIC IMPORTED GLOBAL)",
            },
            "aliased-forgery": {
                "before_edge": (
                    "add_library(actual_volk STATIC rhi/VulkanFoundation.cpp)\n"
                    "add_library(volk ALIAS actual_volk)"
                ),
            },
            "namespaced-aliased-forgery": {
                "before_edge": (
                    "add_library(actual_volk STATIC rhi/VulkanFoundation.cpp)\n"
                    "add_library(volk::volk ALIAS actual_volk)"
                ),
            },
            "reverse-aliased-forgery": {
                "before_edge": (
                    "add_library(actual_volk STATIC rhi/VulkanFoundation.cpp)\n"
                    "add_library(actual_volk_alias ALIAS volk)"
                ),
            },
            "non-public-edge": {
                "authority_edge": canonical_edge.replace("\n            PUBLIC", "\n            PRIVATE"),
            },
            "altered-common-renderer-list": {
                "authority_edge": canonical_edge.replace("\n              meshoptimizer", ""),
            },
            "variable-generated-edge": {
                "before_edge": "set(DEMO_VOLK_TARGET volk)",
                "authority_edge": canonical_edge.replace("\n              volk", "\n              ${DEMO_VOLK_TARGET}"),
            },
            "guarded-edge": {
                "authority_edge": f"if(WIN32)\n{canonical_edge}\nendif()",
            },
            "duplicate-declaration": {
                "extra_cmake": canonical_declaration,
            },
            "duplicate-population": {
                "extra_cmake": "FetchContent_MakeAvailable(volk)",
            },
            "duplicate-edge": {
                "extra_cmake": canonical_edge,
            },
            "function-generated-declaration": {
                "volk_declaration": (
                    "function(provide_volk)\n"
                    f"{canonical_declaration}\n"
                    "endfunction()\n"
                    "provide_volk()"
                ),
            },
            "macro-generated-edge": {
                "authority_edge": (
                    "macro(link_demo_volk)\n"
                    f"{canonical_edge}\n"
                    "endmacro()\n"
                    "link_demo_volk()"
                ),
            },
            "arbitrary-target-edge": {
                "extra_cmake": (
                    "add_library(renderer_bridge STATIC rhi/VulkanFoundation.cpp)\n"
                    "target_link_libraries(renderer_bridge PRIVATE volk)"
                ),
            },
            "namespaced-arbitrary-target-edge": {
                "extra_cmake": (
                    "add_library(renderer_bridge INTERFACE)\n"
                    "target_link_libraries(renderer_bridge INTERFACE volk::volk)"
                ),
            },
            "generator-expression-arbitrary-target-edge": {
                "extra_cmake": (
                    "add_library(renderer_bridge INTERFACE)\n"
                    'target_link_libraries(renderer_bridge INTERFACE "$<LINK_ONLY:volk>")'
                ),
            },
            "directory-link-edge": {
                "extra_cmake": "link_libraries(volk)",
            },
            "variable-directory-link-edge": {
                "extra_cmake": "set(EXTRA_VOLK_TARGET volk)\nlink_libraries(${EXTRA_VOLK_TARGET})",
            },
            "directory-link-property-edge": {
                "extra_cmake": "set_directory_properties(PROPERTIES LINK_LIBRARIES volk)",
            },
            "alternate-find-package-acquisition": {
                "extra_cmake": "find_package(volk CONFIG)",
            },
            "variable-alternate-find-package-acquisition": {
                "extra_cmake": "set(EXTRA_VOLK_PACKAGE volk)\nfind_package(${EXTRA_VOLK_PACKAGE} CONFIG)",
            },
            "alternate-include-acquisition": {
                "extra_cmake": "include(volk)",
            },
            "file-download-acquisition": {
                "extra_cmake": (
                    'file(DOWNLOAD "https://github.com/zeux/volk/archive/master.zip" '
                    '"${CMAKE_BINARY_DIR}/volk.zip")'
                ),
            },
            "file-archive-extraction": {
                "extra_cmake": (
                    'file(ARCHIVE_EXTRACT INPUT "${CMAKE_BINARY_DIR}/volk.zip" '
                    'DESTINATION "${CMAKE_BINARY_DIR}/alternate-volk")'
                ),
            },
            "subdirectory-acquisition": {
                "extra_cmake": "add_subdirectory(external/volk)",
                "extra_files": {
                    "external/volk/CMakeLists.txt": "add_library(alternate_volk STATIC volk.c)\n",
                    "external/volk/volk.c": "int alternate_volk;\n",
                },
            },
            "custom-command-acquisition": {
                "extra_cmake": (
                    "add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/volk-fetch.stamp\n"
                    "  COMMAND git clone https://github.com/zeux/volk.git external/volk)"
                ),
            },
            "custom-target-acquisition": {
                "extra_cmake": (
                    "add_custom_target(fetch_volk_repository\n"
                    "  COMMAND git clone https://github.com/zeux/volk.git external/volk)"
                ),
            },
            "execute-process-acquisition": {
                "extra_cmake": (
                    "execute_process(COMMAND git clone "
                    "https://github.com/zeux/volk.git external/volk)"
                ),
            },
            "repository-side-target-mutation": {
                "extra_cmake": "target_compile_definitions(volk PUBLIC FORGED_VOLK=1)",
            },
            "repository-side-compile-feature-mutation": {
                "extra_cmake": "target_compile_features(volk PUBLIC c_std_11)",
            },
            "repository-side-source-property-mutation": {
                "extra_cmake": (
                    "set_source_files_properties(${volk_SOURCE_DIR}/volk.c "
                    "PROPERTIES COMPILE_DEFINITIONS FORGED_VOLK=1)"
                ),
            },
            "repository-side-source-set-property-mutation": {
                "extra_cmake": (
                    "set_property(SOURCE ${volk_SOURCE_DIR}/volk.c "
                    "PROPERTY COMPILE_OPTIONS -DFORGED_VOLK=1)"
                ),
            },
            "direct-scene-editor-edge": {
                "extra_cmake": "target_link_libraries(scene_editor_core PRIVATE volk)",
            },
            "direct-desktop-editor-edge": {
                "extra_cmake": "target_link_libraries(desktop_scene_editor_ui PRIVATE volk)",
            },
            "direct-editor-leaf-edge": {
                "extra_cmake": "target_link_libraries(editor_owned_leaf PRIVATE volk)",
            },
            "source-override": {
                "before_edge": "set(FETCHCONTENT_SOURCE_DIR_VOLK external/volk)",
            },
            "header-only-forgery": {
                "before_edge": "set(VOLK_HEADERS_ONLY ON)",
            },
            "global-inherited-compile-options": {
                "volk_declaration": (
                    "add_compile_options(/Od /FIhostile_volk.h)\n"
                    + canonical_declaration
                ),
                "extra_files": {"hostile_volk.h": "#pragma once\n"},
            },
            "directory-inherited-compile-options": {
                "volk_declaration": (
                    "set_property(DIRECTORY APPEND PROPERTY COMPILE_OPTIONS "
                    "/Od /FIhostile_volk.h)\n"
                    + canonical_declaration
                ),
                "extra_files": {"hostile_volk.h": "#pragma once\n"},
            },
            "environment-execute-process-copy": {
                "extra_cmake": (
                    "execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different "
                    '"$ENV{MGIF_PAYLOAD_SOURCE}" "$ENV{MGIF_PAYLOAD_DESTINATION}")'
                ),
            },
            "environment-file-copy-file": {
                "extra_cmake": (
                    'file(COPY_FILE "$ENV{MGIF_PAYLOAD_SOURCE}" '
                    '"$ENV{MGIF_PAYLOAD_DESTINATION}")'
                ),
            },
            "environment-configure-file": {
                "extra_cmake": (
                    'configure_file("$ENV{MGIF_PAYLOAD_SOURCE}" '
                    '"$ENV{MGIF_PAYLOAD_DESTINATION}" COPYONLY)'
                ),
            },
        }
        for name, arguments in hostile_cases.items():
            with self.subTest(case=name):
                project_arguments = {"authority_edge": canonical_edge, **arguments}
                hostile = self.make_volk_dependency_project(**project_arguments)
                message = (
                    r"(?i)unapproved configure-time"
                    if name in {
                        "environment-execute-process-copy",
                        "environment-file-copy-file",
                        "environment-configure-file",
                    }
                    else r"(?i)(?:outside allowedUiFoundation.*volk|volk.*outside allowedUiFoundation)"
                )
                with patch(
                    "validate_editor_p0_artifacts.EXPECTED_TINYGLTF_TARGET",
                    "isolated_tinygltf_authority",
                ), self.assertRaisesRegex(
                    ValidationError,
                    message,
                ):
                    validate_editor_dependency_boundary(hostile)

    def test_demo_core_volk_does_not_mask_forbidden_dependency_families(self) -> None:
        forbidden_families = ("nvpro_core", "nvcore", "nvapp", "nvgui", "nvvk", "nvvkgltf", "nvh")
        for dependency in forbidden_families:
            with self.subTest(edge="direct", dependency=dependency):
                root = self.make_volk_dependency_project(
                    extra_cmake=f"target_link_libraries(demo_core PRIVATE {dependency})",
                )
                with patch(
                    "validate_editor_p0_artifacts.EXPECTED_TINYGLTF_TARGET",
                    "isolated_tinygltf_authority",
                ), self.assertRaisesRegex(ValidationError, rf"(?i)forbidden editor dependency.*{dependency}"):
                    validate_editor_dependency_boundary(root)

            with self.subTest(edge="transitive-through-volk", dependency=dependency):
                root = self.make_volk_dependency_project(
                    extra_cmake=f"target_link_libraries(volk PUBLIC {dependency})",
                )
                with patch(
                    "validate_editor_p0_artifacts.EXPECTED_TINYGLTF_TARGET",
                    "isolated_tinygltf_authority",
                ), self.assertRaisesRegex(ValidationError, rf"(?i)forbidden editor dependency.*{dependency}"):
                    validate_editor_dependency_boundary(root)

            with self.subTest(edge="transitive", dependency=dependency):
                root = self.make_volk_dependency_project(
                    extra_cmake=(
                        "add_library(forbidden_bridge INTERFACE)\n"
                        f"target_link_libraries(forbidden_bridge INTERFACE {dependency})\n"
                        "target_link_libraries(demo_core PRIVATE forbidden_bridge)"
                    ),
                )
                with patch(
                    "validate_editor_p0_artifacts.EXPECTED_TINYGLTF_TARGET",
                    "isolated_tinygltf_authority",
                ), self.assertRaisesRegex(ValidationError, rf"(?i)forbidden editor dependency.*{dependency}"):
                    validate_editor_dependency_boundary(root)

    def test_demo_core_renderer_foundations_require_one_exact_common_edge(self) -> None:
        foundations = ("glm", "tinygltf", "meshoptimizer", "Vulkan::Vulkan")
        for dependency in foundations:
            with self.subTest(channel="second-direct-edge", dependency=dependency):
                root = self.make_volk_dependency_project(
                    extra_cmake=f"target_link_libraries(demo_core PRIVATE {dependency})",
                )
                with patch(
                    "validate_editor_p0_artifacts.EXPECTED_TINYGLTF_TARGET",
                    "isolated_tinygltf_authority",
                ), self.assertRaisesRegex(
                    ValidationError,
                    rf"(?i)(?:outside allowedUiFoundation.*{re.escape(dependency)}|"
                    rf"{re.escape(dependency)}.*outside allowedUiFoundation)",
                ):
                    validate_editor_dependency_boundary(root)

            with self.subTest(channel="wrapper-edge", dependency=dependency):
                root = self.make_volk_dependency_project(
                    extra_cmake=(
                        "add_library(renderer_foundation_wrapper INTERFACE)\n"
                        f"target_link_libraries(renderer_foundation_wrapper INTERFACE {dependency})\n"
                        "target_link_libraries(demo_core PRIVATE renderer_foundation_wrapper)"
                    ),
                )
                with patch(
                    "validate_editor_p0_artifacts.EXPECTED_TINYGLTF_TARGET",
                    "isolated_tinygltf_authority",
                ), self.assertRaisesRegex(ValidationError, r"(?i)outside allowedUiFoundation"):
                    validate_editor_dependency_boundary(root)

    def test_renderer_scene_transaction_requires_exact_public_foundation_edge(self) -> None:
        canonical = """
            include(FetchContent)
            FetchContent_Declare(tinygltf
              GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
              GIT_TAG v2.9.3)
            set(TINYGLTF_HEADER_ONLY ON)
            FetchContent_MakeAvailable(tinygltf)
            add_library(scene_editor_document STATIC
              loader/TinyGltfImplementation.cpp
              scene/editor/include/mgif/scene_editor/EditorLimits.h
              scene/editor/include/mgif/scene_editor/EditorObjectId.h
              scene/editor/include/mgif/scene_editor/EditorHistoryPolicy.h
              scene/editor/include/mgif/scene_editor/GltfDocument.h
              scene/editor/include/mgif/scene_editor/StructuralDelta.h
              scene/editor/include/mgif/scene_editor/SceneCommands.h
              scene/editor/include/mgif/scene_editor/GltfDocumentIO.h
              scene/editor/include/mgif/scene_editor/GltfSemanticCompare.h
              scene/editor/include/mgif/scene_editor/GltfValidator.h
              scene/editor/GltfDocument.cpp
              scene/editor/GltfDocumentIO.cpp
              scene/editor/GltfSemanticCompare.cpp
              scene/editor/GltfValidator.cpp
              scene/editor/SceneCommands.cpp
              scene/editor/StructuralDelta.cpp)
            target_link_libraries(scene_editor_document PRIVATE tinygltf)
            add_library(scene_change_contract STATIC scene/editor/SceneChangeSet.cpp)
            if(NOT ANDROID)
              add_library(renderer_scene_transaction STATIC
                render/editor/SceneRuntimePublisher.cpp)
              target_link_libraries(renderer_scene_transaction PUBLIC
                scene_change_contract glm tinygltf)
            endif()
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            target_link_libraries(scene_editor_core PRIVATE
              renderer_scene_transaction)
        """
        files = {
            "loader/TinyGltfImplementation.cpp": (
                "#define TINYGLTF_IMPLEMENTATION\n"
                "#define STB_IMAGE_IMPLEMENTATION\n"
                "#define STB_IMAGE_WRITE_IMPLEMENTATION\n"
                "#include <tiny_gltf.h>\n"
            ),
            "loader/GltfLoader.cpp": "int loader_anchor = 0;\n",
            "scene/editor/include/mgif/scene_editor/EditorLimits.h": "#pragma once\n",
            "scene/editor/include/mgif/scene_editor/EditorObjectId.h": "#pragma once\n",
            "scene/editor/include/mgif/scene_editor/EditorHistoryPolicy.h": "#pragma once\n",
            "scene/editor/include/mgif/scene_editor/GltfDocument.h": "#pragma once\n",
            "scene/editor/include/mgif/scene_editor/StructuralDelta.h": "#pragma once\n",
            "scene/editor/include/mgif/scene_editor/SceneCommands.h": "#pragma once\n",
            "scene/editor/include/mgif/scene_editor/GltfDocumentIO.h": "#pragma once\n",
            "scene/editor/include/mgif/scene_editor/GltfSemanticCompare.h": "#pragma once\n",
            "scene/editor/include/mgif/scene_editor/GltfValidator.h": "#pragma once\n",
            "scene/editor/GltfDocument.cpp": "int document_anchor = 0;\n",
            "scene/editor/GltfDocumentIO.cpp": "int io_anchor = 0;\n",
            "scene/editor/GltfSemanticCompare.cpp": "int semantic_anchor = 0;\n",
            "scene/editor/GltfValidator.cpp": "int validator_anchor = 0;\n",
            "scene/editor/SceneCommands.cpp": "int commands_anchor = 0;\n",
            "scene/editor/StructuralDelta.cpp": "int delta_anchor = 0;\n",
            "scene/editor/SceneChangeSet.cpp": "int change_anchor = 0;\n",
            "scene/editor/Core.cpp": "int core_anchor = 0;\n",
            "render/editor/SceneRuntimePublisher.cpp": "int publisher_anchor = 0;\n",
        }
        validate_editor_dependency_boundary(
            self.make_dependency_project(canonical, files)
        )

        hostile_cases = (
            canonical + "\ntarget_link_libraries(renderer_scene_transaction PRIVATE glm)",
            canonical.replace(
                "scene_change_contract glm tinygltf",
                "scene_change_contract tinygltf glm",
            ),
            canonical.replace(
                "target_link_libraries(renderer_scene_transaction PUBLIC",
                "target_link_libraries(scene_editor_core PUBLIC",
            ),
        )
        for index, hostile_cmake in enumerate(hostile_cases):
            with self.subTest(case=index):
                with self.assertRaisesRegex(
                    ValidationError,
                    r"(?i)(?:glm|tinygltf).*outside allowedUiFoundation|outside allowedUiFoundation.*(?:glm|tinygltf)",
                ):
                    validate_editor_dependency_boundary(
                        self.make_dependency_project(hostile_cmake, files)
                    )

    def test_configured_volk_requires_real_target_checkout_and_literal_edge_backtrace(self) -> None:
        source_root = self.root / "configured-volk-source"
        build_root = self.root / "configured-volk-build"
        volk_source = build_root / "_deps/volk-src"
        volk_build = build_root / "_deps/volk-build"
        vulkan_include = build_root / "vulkan-sdk/include"
        source_root.mkdir()
        volk_source.mkdir(parents=True)
        volk_build.mkdir(parents=True)
        vulkan_include.mkdir(parents=True)
        edge_line = 87
        source_lines = ["# configured fixture"] * (edge_line - 1)
        source_lines.append(
            "target_link_libraries(demo_core PUBLIC scene_editor_core volk imgui_demo glm "
            "tinygltf meshoptimizer Vulkan::Vulkan)"
        )
        (source_root / "CMakeLists.txt").write_text(
            "\n".join(source_lines) + "\n",
            encoding="utf-8",
        )
        (volk_source / "CMakeLists.txt").write_text(
            "add_library(volk STATIC volk.h volk.c)\n"
            "target_include_directories(volk PUBLIC "
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}> $<INSTALL_INTERFACE:include>)\n"
            'target_include_directories(volk PUBLIC "${VOLK_INCLUDES}")\n',
            encoding="utf-8",
        )
        (volk_source / "volk.h").write_text("#pragma once\n", encoding="utf-8")
        (volk_source / "volk.c").write_text("int volk_anchor = 0;\n", encoding="utf-8")
        static_model = {
            "repositoryExternalFoundations": {
                "volk": {
                    "target": "volk",
                    "repository": "https://github.com/zeux/volk.git",
                    "tag": "master",
                    "owner": "demo_core",
                    "declarationLine": 12,
                    "populationLine": 16,
                    "edgeLine": edge_line,
                    "inheritedCompileAuthority": [],
                },
            },
        }
        volk_model = {
            "name": "volk",
            "id": "volk::@canonical",
            "type": "STATIC_LIBRARY",
            "paths": {
                "source": volk_source.as_posix(),
                "build": "_deps/volk-build",
            },
            "backtrace": 1,
            "backtraceGraph": {
                "commands": ["add_library", "target_include_directories"],
                "files": [volk_source.joinpath("CMakeLists.txt").as_posix(), "CMakeLists.txt"],
                "nodes": [
                    {"file": 0},
                    {"command": 0, "file": 0, "line": 1, "parent": 0},
                    {"command": 1, "file": 0, "line": 2, "parent": 0},
                    {"command": 1, "file": 0, "line": 3, "parent": 0},
                ],
            },
            "sources": [
                {"path": volk_source.joinpath("volk.h").as_posix(), "backtrace": 1},
                {
                    "path": volk_source.joinpath("volk.c").as_posix(),
                    "compileGroupIndex": 0,
                    "backtrace": 1,
                },
            ],
            "compileGroups": [
                {
                    "language": "C",
                    "sourceIndexes": [1],
                    "compileCommandFragments": [
                        {"fragment": "/DWIN32 /D_WINDOWS /Ob0 /Od /RTC1 -MDd -Zi"},
                    ],
                    "includes": [
                        {"path": volk_source.as_posix(), "backtrace": 2},
                        {"path": vulkan_include.as_posix(), "backtrace": 3},
                    ],
                },
            ],
        }
        demo_model = {
            "name": "demo_core",
            "id": "demo_core::@repository",
            "type": "STATIC_LIBRARY",
            "dependencies": [{"id": "volk::@canonical", "backtrace": 1}],
            "backtraceGraph": {
                "commands": ["target_link_libraries"],
                "files": ["CMakeLists.txt"],
                "nodes": [
                    {"file": 0},
                    {"command": 0, "file": 0, "line": edge_line, "parent": 0},
                ],
            },
        }
        state = {
            "targetModels": {"volk": volk_model, "demo_core": demo_model},
            "configuration": "Debug",
            "targetIds": {
                "volk": "volk::@canonical",
                "demo_core": "demo_core::@repository",
            },
            "targetReferenceIds": {
                "volk": "volk::@canonical",
                "demo_core": "demo_core::@repository",
            },
            "cacheEntries": {
                "CMAKE_C_FLAGS": "/DWIN32 /D_WINDOWS",
                "CMAKE_C_FLAGS_DEBUG": "/Ob0 /Od /RTC1",
                "VOLK_HEADERS_ONLY": "OFF",
                "VOLK_INSTALL": "OFF",
                "VOLK_NAMESPACE": "OFF",
                "VOLK_PULL_IN_VULKAN": "ON",
                "VOLK_STATIC_DEFINES": "",
                "VULKAN_HEADERS_INSTALL_DIR": "",
                "Vulkan_INCLUDE_DIR": vulkan_include.as_posix(),
                "volk_SOURCE_DIR": volk_source.as_posix(),
                "volk_BINARY_DIR": volk_build.as_posix(),
            },
        }
        reply_root = build_root / ".cmake/api/v1/reply"
        reply_root.mkdir(parents=True)
        target_paths = {}
        for target_name, target_model in state["targetModels"].items():
            target_path = reply_root / f"target-{target_name}.json"
            target_path.write_text(json.dumps(target_model, sort_keys=True), encoding="utf-8")
            target_paths[target_name] = target_path
        state["targetPaths"] = target_paths

        def git_result(arguments: list[str], **_: object) -> SimpleNamespace:
            if arguments[-3:] == ["remote", "get-url", "origin"]:
                output = "https://github.com/zeux/volk.git\n"
            elif arguments[-4:] == ["symbolic-ref", "--quiet", "--short", "HEAD"]:
                output = "master\n"
            elif arguments[-3:] == ["status", "--porcelain", "--untracked-files=no"]:
                output = ""
            else:
                return SimpleNamespace(returncode=1, stdout="", stderr="unexpected Git invocation")
            return SimpleNamespace(returncode=0, stdout=output, stderr="")

        with patch("validate_editor_p0_artifacts.subprocess.run", side_effect=git_result):
            _validate_configured_volk_foundation(source_root, build_root, static_model, state)

        hostile_states: dict[str, tuple[dict[str, object], str]] = {}
        missing_target_ids = copy.deepcopy(state)
        del missing_target_ids["targetIds"]
        hostile_states["missing-target-ids"] = (
            missing_target_ids,
            r"references and replies disagree on exact IDs/names",
        )
        missing_target_paths = copy.deepcopy(state)
        del missing_target_paths["targetPaths"]
        hostile_states["missing-target-paths"] = (
            missing_target_paths,
            r"references and reply paths disagree",
        )
        missing_reference_ids = copy.deepcopy(state)
        del missing_reference_ids["targetReferenceIds"]
        hostile_states["missing-target-reference-ids"] = (
            missing_reference_ids,
            r"reference identity snapshot is missing or changed",
        )
        coherent_replacement = copy.deepcopy(state)
        replacement_id = "volk::@coherent-replacement"
        coherent_replacement["targetModels"]["volk"]["id"] = replacement_id
        coherent_replacement["targetModels"]["demo_core"]["dependencies"][0]["id"] = replacement_id
        coherent_replacement["targetIds"]["volk"] = replacement_id
        coherent_replacement["targetReferenceIds"]["volk"] = replacement_id
        hostile_states["coherent-target-model-replacement"] = (
            coherent_replacement,
            r"configured target reply identity changed",
        )
        missing = copy.deepcopy(state)
        del missing["targetModels"]["volk"]
        hostile_states["missing-real-target"] = (missing, r"real volk target")
        imported_or_alias = copy.deepcopy(state)
        imported_or_alias["targetModels"]["volk"]["type"] = "INTERFACE_LIBRARY"
        hostile_states["non-static-lookalike"] = (imported_or_alias, r"non-imported, non-aliased static target")
        escaped = copy.deepcopy(state)
        escaped["targetModels"]["volk"]["paths"]["source"] = source_root.as_posix()
        hostile_states["escaped-source"] = (escaped, r"canonical build-local FetchContent")
        changed_sources = copy.deepcopy(state)
        changed_sources["targetModels"]["volk"]["sources"].pop()
        hostile_states["changed-source-set"] = (changed_sources, r"source/compiled identity")
        changed_origin = copy.deepcopy(state)
        changed_origin["targetModels"]["volk"]["backtraceGraph"]["files"][0] = (source_root / "CMakeLists.txt").as_posix()
        hostile_states["changed-definition-origin"] = (changed_origin, r"canonical FetchContent")
        duplicate_edge = copy.deepcopy(state)
        duplicate_edge["targetModels"]["demo_core"]["dependencies"].append(
            {"id": "volk::@duplicate", "backtrace": 1}
        )
        hostile_states["duplicate-configured-edge"] = (duplicate_edge, r"exactly one real volk dependency")
        changed_edge = copy.deepcopy(state)
        changed_edge["targetModels"]["demo_core"]["backtraceGraph"]["nodes"][1]["line"] += 1
        hostile_states["changed-edge-backtrace"] = (changed_edge, r"literal repository edge")
        forged_target_id = copy.deepcopy(state)
        forged_target_id["targetModels"]["volk"]["id"] = "volk::@forged"
        hostile_states["forged-target-id"] = (forged_target_id, r"exact IDs|exact target ID")
        duplicate_target_id = copy.deepcopy(state)
        duplicate_target_id["targetModels"]["impostor"] = {
            "name": "impostor",
            "id": "volk::@canonical",
            "type": "UTILITY",
        }
        hostile_states["duplicate-target-id"] = (duplicate_target_id, r"target ID is ambiguous")
        missing_compile_groups = copy.deepcopy(state)
        del missing_compile_groups["targetModels"]["volk"]["compileGroups"]
        hostile_states["missing-compile-groups"] = (missing_compile_groups, r"exactly one compile group")
        changed_include = copy.deepcopy(state)
        changed_include["targetModels"]["volk"]["compileGroups"][0]["includes"][0]["path"] = (
            source_root / "forged-include"
        ).as_posix()
        hostile_states["changed-include-root"] = (changed_include, r"include roots")
        extra_include = copy.deepcopy(state)
        extra_include["targetModels"]["volk"]["compileGroups"][0]["includes"].append(
            {"path": source_root.as_posix(), "backtrace": 2}
        )
        hostile_states["extra-include-root"] = (extra_include, r"include identity")
        changed_include_backtrace = copy.deepcopy(state)
        changed_include_backtrace["targetModels"]["volk"]["backtraceGraph"]["nodes"][2]["line"] = 3
        hostile_states["changed-include-backtrace-line"] = (
            changed_include_backtrace,
            r"include backtrace does not match canonical source arguments",
        )
        wrong_language = copy.deepcopy(state)
        wrong_language["targetModels"]["volk"]["compileGroups"][0]["language"] = "CXX"
        hostile_states["wrong-compile-language"] = (wrong_language, r"language/source identity")
        forged_definition = copy.deepcopy(state)
        forged_definition["targetModels"]["volk"]["compileGroups"][0]["defines"] = [
            {"define": "FORGED_VOLK=1"}
        ]
        hostile_states["forged-compile-definition"] = (forged_definition, r"compile definitions")
        forged_definition_fragment = copy.deepcopy(state)
        forged_definition_fragment["targetModels"]["volk"]["compileGroups"][0][
            "compileCommandFragments"
        ].append({"fragment": "-DFORGED_VOLK=1"})
        hostile_states["compile-command-fragment-definition"] = (
            forged_definition_fragment,
            r"compile fragments contain unauthorized options",
        )
        forged_forced_include_fragment = copy.deepcopy(state)
        forged_forced_include_fragment["targetModels"]["volk"]["compileGroups"][0][
            "compileCommandFragments"
        ].append({"fragment": "/FIhostile_volk.h"})
        hostile_states["compile-command-fragment-forced-include"] = (
            forged_forced_include_fragment,
            r"compile fragments contain unauthorized options",
        )
        missing_cache = copy.deepcopy(state)
        del missing_cache["cacheEntries"]
        hostile_states["missing-cache-provenance"] = (missing_cache, r"cache provenance")
        changed_static_defines = copy.deepcopy(state)
        changed_static_defines["cacheEntries"]["VOLK_STATIC_DEFINES"] = "FORGED_VOLK=1"
        hostile_states["changed-cache-definition"] = (changed_static_defines, r"cache options")
        forbidden_configured_edge = copy.deepcopy(state)
        forbidden_configured_edge["targetModels"]["volk"]["dependencies"] = [
            {"id": "nvvk::@forged"},
        ]
        hostile_states["forbidden-configured-edge"] = (
            forbidden_configured_edge,
            r"forbidden editor dependency.*nvvk",
        )
        forbidden_configured_transitive = copy.deepcopy(state)
        forbidden_configured_transitive["targetModels"]["volk"]["dependencies"] = [
            {"id": "external_bridge::@forged"},
        ]
        forbidden_configured_transitive["targetModels"]["external_bridge"] = {
            "name": "external_bridge",
            "id": "external_bridge::@forged",
            "dependencies": [{"id": "nvapp::@forged"}],
        }
        forbidden_configured_transitive["targetIds"]["external_bridge"] = "external_bridge::@forged"
        hostile_states["forbidden-configured-transitive-edge"] = (
            forbidden_configured_transitive,
            r"forbidden editor dependency.*nvapp",
        )
        for name, (hostile_state, message) in hostile_states.items():
            with self.subTest(case=name), patch(
                "validate_editor_p0_artifacts.subprocess.run",
                side_effect=git_result,
            ), self.assertRaisesRegex(ValidationError, message):
                _validate_configured_volk_foundation(source_root, build_root, static_model, hostile_state)

        hostile_git_results = {
            "changed-git-origin": (
                lambda arguments, **_: SimpleNamespace(
                    returncode=0,
                    stdout=(
                        "https://github.com/example/volk.git\n"
                        if arguments[-3:] == ["remote", "get-url", "origin"]
                        else "master\n"
                        if arguments[-4:] == ["symbolic-ref", "--quiet", "--short", "HEAD"]
                        else ""
                    ),
                    stderr="",
                ),
                r"Git origin",
            ),
            "changed-git-branch": (
                lambda arguments, **_: SimpleNamespace(
                    returncode=0,
                    stdout=(
                        "https://github.com/zeux/volk.git\n"
                        if arguments[-3:] == ["remote", "get-url", "origin"]
                        else "forged\n"
                        if arguments[-4:] == ["symbolic-ref", "--quiet", "--short", "HEAD"]
                        else ""
                    ),
                    stderr="",
                ),
                r"master branch",
            ),
            "dirty-git-checkout": (
                lambda arguments, **_: SimpleNamespace(
                    returncode=0,
                    stdout=(
                        "https://github.com/zeux/volk.git\n"
                        if arguments[-3:] == ["remote", "get-url", "origin"]
                        else "master\n"
                        if arguments[-4:] == ["symbolic-ref", "--quiet", "--short", "HEAD"]
                        else " M volk.c\n"
                    ),
                    stderr="",
                ),
                r"tracked source mutations",
            ),
        }
        for name, (side_effect, message) in hostile_git_results.items():
            with self.subTest(case=name), patch(
                "validate_editor_p0_artifacts.subprocess.run",
                side_effect=side_effect,
            ), self.assertRaisesRegex(ValidationError, message):
                _validate_configured_volk_foundation(source_root, build_root, static_model, state)

    def test_file_api_target_replies_require_containment_exact_identity_and_unique_ids(self) -> None:
        direct_root = self.root / "direct-file-api"
        reply_root = direct_root / "reply"
        reply_root.mkdir(parents=True)
        codemodel_path = reply_root / "codemodel-v2.json"
        target_path = reply_root / "target-volk.json"

        def write_codemodel(targets: list[dict[str, str]]) -> None:
            codemodel_path.write_text(
                json.dumps({"configurations": [{"name": "Debug", "targets": targets}]}) + "\n",
                encoding="utf-8",
            )

        target_path.write_text(
            json.dumps({"name": "volk", "id": "volk::@canonical"}) + "\n",
            encoding="utf-8",
        )
        canonical_reference = {
            "name": "volk",
            "id": "volk::@canonical",
            "jsonFile": target_path.name,
        }
        write_codemodel([canonical_reference])
        configuration, loaded_path, target, _ = dependency_identity._configured_target(
            codemodel_path,
            "volk",
            "Debug",
        )
        self.assertEqual(configuration, "Debug")
        self.assertEqual(loaded_path, target_path.resolve())
        self.assertEqual(target["id"], "volk::@canonical")

        escaped = direct_root / "escaped-target.json"
        escaped.write_text(
            json.dumps({"name": "volk", "id": "volk::@canonical"}) + "\n",
            encoding="utf-8",
        )
        hostile_references = {
            "traversal": [
                {**canonical_reference, "jsonFile": "../escaped-target.json"},
            ],
            "absolute": [
                {**canonical_reference, "jsonFile": escaped.as_posix()},
            ],
            "duplicate-id": [
                canonical_reference,
                {"name": "impostor", "id": "volk::@canonical", "jsonFile": target_path.name},
            ],
        }
        for name, references in hostile_references.items():
            with self.subTest(private_loader=name):
                write_codemodel(references)
                with self.assertRaises(IdentityError):
                    dependency_identity._configured_target(codemodel_path, "volk", "Debug")

        write_codemodel([canonical_reference])
        target_path.write_text(
            json.dumps({"name": "volk", "id": "volk::@forged"}) + "\n",
            encoding="utf-8",
        )
        with self.assertRaisesRegex(IdentityError, r"reply identity"):
            dependency_identity._configured_target(codemodel_path, "volk", "Debug")

        symlink_path = reply_root / "target-volk-symlink.json"
        try:
            symlink_path.symlink_to(escaped)
        except OSError:
            pass
        else:
            write_codemodel([{**canonical_reference, "jsonFile": symlink_path.name}])
            with self.assertRaisesRegex(IdentityError, r"escapes the reply directory"):
                dependency_identity._configured_target(codemodel_path, "volk", "Debug")

        repository_root = self.root / "configured-file-api-source"
        build_root = self.root / "configured-file-api-build"
        configured_reply = build_root / ".cmake/api/v1/reply"
        repository_root.mkdir()
        configured_reply.mkdir(parents=True)
        (repository_root / "CMakeLists.txt").write_text("# fixture\n", encoding="utf-8")
        configured_codemodel = configured_reply / "codemodel-v2.json"
        configured_cache = configured_reply / "cache-v2.json"
        configured_toolchains = configured_reply / "toolchains-v1.json"
        configured_cache.write_text("{}\n", encoding="utf-8")
        configured_toolchains.write_text("{}\n", encoding="utf-8")
        index_path = configured_reply / "index-1.json"
        index_path.write_text(
            json.dumps(
                {
                    "cmake": {"generator": {"name": "Ninja"}},
                    "objects": [
                        {"kind": "codemodel", "jsonFile": configured_codemodel.name},
                        {"kind": "cache", "jsonFile": configured_cache.name},
                        {"kind": "toolchains", "jsonFile": configured_toolchains.name},
                    ],
                }
            )
            + "\n",
            encoding="utf-8",
        )

        def write_configured_codemodel(reference: dict[str, str]) -> None:
            configured_codemodel.write_text(
                json.dumps(
                    {
                        "paths": {
                            "source": repository_root.resolve().as_posix(),
                            "build": build_root.resolve().as_posix(),
                        },
                        "configurations": [{"name": "Debug", "targets": [reference]}],
                    }
                )
                + "\n",
                encoding="utf-8",
            )

        escaped_reference = {
            "name": "volk",
            "id": "volk::@reference",
            "jsonFile": "../../forged-target.json",
        }
        write_configured_codemodel(escaped_reference)
        with self.assertRaisesRegex(IdentityError, r"traversal|reply directory"):
            dependency_identity.inspect_configured_editor_build(
                repository_root,
                build_root,
                requested_config="Debug",
                require_ninja_dependencies=False,
            )
        with self.assertRaisesRegex(ValidationError, r"traversal|reply directory"):
            validate_configured_editor_build(
                repository_root,
                build_root,
                "Debug",
                require_ninja_dependencies=False,
            )

        configured_target = configured_reply / "target-volk.json"
        configured_target.write_text(
            json.dumps({"name": "volk", "id": "volk::@forged"}) + "\n",
            encoding="utf-8",
        )
        write_configured_codemodel(
            {**escaped_reference, "jsonFile": configured_target.name}
        )
        with self.assertRaisesRegex(IdentityError, r"reply identity"):
            dependency_identity.inspect_configured_editor_build(
                repository_root,
                build_root,
                requested_config="Debug",
                require_ninja_dependencies=False,
            )

        imgui_source = build_root / "_deps/imgui-src"
        imgui_source.mkdir(parents=True)
        fake_generator = repository_root / "tools/generate_editor_dependency_identity.py"
        fake_generator.parent.mkdir()
        fake_generator.write_text("# fixture\n", encoding="utf-8")
        output_path = build_root / "identity.json"
        arguments = SimpleNamespace(
            source_dir=imgui_source,
            expected_source_dir=imgui_source,
            repo_root=repository_root,
            build_root=build_root,
            config="Debug",
            output=output_path,
            cache_override=False,
            environment_override=False,
        )
        write_configured_codemodel(escaped_reference)
        with patch.object(dependency_identity, "parse_args", return_value=arguments), patch.object(
            dependency_identity,
            "__file__",
            str(fake_generator),
        ):
            self.assertEqual(dependency_identity.main(), 1)
        self.assertFalse(output_path.exists())

    def test_allows_only_pinned_tinygltf_document_authority_closure(self) -> None:
        root = self.make_tinygltf_dependency_project()
        result = validate_editor_dependency_boundary(root)
        self.assertIn("scene_editor_document", result["protectedTargets"])
        testing_definition_mutations = {
            "document-public": (
                "target_compile_definitions(scene_editor_document PUBLIC MGIF_EDITOR_TESTING=1)"
            ),
            "core-private": (
                "target_compile_definitions(scene_editor_core PRIVATE MGIF_EDITOR_TESTING=1)"
            ),
            "non-p4-target": (
                "add_library(mgif_scene_adapter INTERFACE)\n"
                "target_compile_definitions(mgif_scene_adapter INTERFACE MGIF_EDITOR_TESTING=1)"
            ),
            "global": "add_compile_definitions(MGIF_EDITOR_TESTING=1)",
            "interface-property": (
                "set_property(TARGET scene_editor_document PROPERTY "
                "INTERFACE_COMPILE_DEFINITIONS MGIF_EDITOR_TESTING=1)"
            ),
        }
        for name, mutation in testing_definition_mutations.items():
            with self.subTest(testing_definition=name):
                hostile = self.make_tinygltf_dependency_project(extra_cmake=mutation)
                with self.assertRaisesRegex(ValidationError, r"MGIF_EDITOR_TESTING"):
                    validate_editor_dependency_boundary(hostile)

    def test_rejects_unregistered_document_authority_source(self) -> None:
        root = self.make_tinygltf_dependency_project(
            extra_cmake=(
                "target_sources(scene_editor_document PRIVATE "
                "scene/editor/UnregisteredDocumentSource.cpp)"
            ),
            extra_files={
                "scene/editor/UnregisteredDocumentSource.cpp":
                    "int unregistered_document_source = 0;\n",
            },
        )
        with self.assertRaisesRegex(ValidationError, r"(?i)source|protected|document|tinygltf"):
            validate_editor_dependency_boundary(root)

    def test_rejects_tinygltf_authority_provenance_lookalikes_and_indirection(self) -> None:
        cases = (
            (
                "wrong-repository",
                {"repository": "https://example.invalid/syoyo/tinygltf.git"},
                r"(?i)tinygltf.*outside allowedUiFoundation|outside allowedUiFoundation.*tinygltf",
            ),
            (
                "wrong-tag",
                {"tag": "v2.9.2"},
                r"(?i)tinygltf.*outside allowedUiFoundation|outside allowedUiFoundation.*tinygltf",
            ),
            (
                "computed-edge",
                {
                    "authority_edge": (
                        "set(DOCUMENT_AUTHORITY tinygltf)\n"
                        "target_link_libraries(scene_editor_document PRIVATE ${DOCUMENT_AUTHORITY})"
                    )
                },
                r"(?i)tinygltf.*outside allowedUiFoundation|outside allowedUiFoundation.*tinygltf",
            ),
            (
                "wrapper-edge",
                {
                    "authority_edge": (
                        "add_library(mgif_scene_adapter INTERFACE)\n"
                        "target_link_libraries(mgif_scene_adapter INTERFACE tinygltf)\n"
                        "target_link_libraries(scene_editor_document PRIVATE mgif_scene_adapter)"
                    )
                },
                r"(?i)tinygltf.*outside allowedUiFoundation|outside allowedUiFoundation.*tinygltf",
            ),
            (
                "forged-target",
                {"extra_cmake": "add_library(tinygltf INTERFACE)"},
                r"(?i)tinygltf.*outside allowedUiFoundation|outside allowedUiFoundation.*tinygltf",
            ),
            (
                "forbidden-transitive-edge",
                {
                    "authority_edge": (
                        "target_link_libraries(scene_editor_document PRIVATE tinygltf nvvk)"
                    )
                },
                r"(?i)tinygltf|nvvk",
            ),
        )
        for name, arguments, reason in cases:
            with self.subTest(name=name):
                root = self.make_tinygltf_dependency_project(**arguments)
                with self.assertRaisesRegex(ValidationError, reason):
                    validate_editor_dependency_boundary(root)

    def test_rejects_tinygltf_header_outside_exact_consumers_and_local_lookalikes(self) -> None:
        unauthorized = self.make_tinygltf_dependency_project(
            extra_files={"scene/editor/Other.cpp": "#include <tiny_gltf.h>\nint other_anchor = 0;\n"}
        )
        with self.assertRaisesRegex(ValidationError, r"(?i)tiny_gltf.*Other"):
            validate_editor_dependency_boundary(unauthorized)

        lookalike = self.make_tinygltf_dependency_project(
            extra_files={"tiny_gltf.h": "#pragma once\n#define LOOKALIKE_TINYGLTF 1\n"}
        )
        with self.assertRaisesRegex(ValidationError, r"(?i)TinyGLTF lookalike"):
            validate_editor_dependency_boundary(lookalike)

    def test_rejects_forged_exact_dear_imgui_identity(self) -> None:
        cases = (
            """
            add_library(DearImGui::DearImGui INTERFACE IMPORTED)
            target_link_libraries(scene_editor_core PRIVATE DearImGui::DearImGui)
            """,
            """
            add_library(qt_widgets UNKNOWN IMPORTED)
            add_library(DearImGui::DearImGui ALIAS qt_widgets)
            target_link_libraries(scene_editor_core PRIVATE DearImGui::DearImGui)
            """,
            """
            set(imgui_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lookalike/imgui")
            add_library(fake_imgui STATIC
              ${imgui_SOURCE_DIR}/imgui.cpp
              ${imgui_SOURCE_DIR}/imgui.h
              ${imgui_SOURCE_DIR}/imgui_draw.cpp
              ${imgui_SOURCE_DIR}/imgui_tables.cpp
              ${imgui_SOURCE_DIR}/imgui_widgets.cpp)
            add_library(DearImGui::DearImGui ALIAS fake_imgui)
            target_link_libraries(scene_editor_core PRIVATE DearImGui::DearImGui)
            """,
        )
        for cmake_tail in cases:
            with self.subTest(alias="ALIAS" in cmake_tail):
                root = self.make_dependency_project(
                    f"""
                    add_library(scene_editor_core STATIC scene/editor/Core.cpp)
                    {cmake_tail}
                    """,
                    {
                        "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                        "vendor/Qt6Widgets.lib": "synthetic import\n",
                        "lookalike/imgui/imgui.cpp": "int fake_imgui_anchor = 0;\n",
                        "lookalike/imgui/imgui.h": "#pragma once\n",
                        "lookalike/imgui/imgui_draw.cpp": "int fake_imgui_draw_anchor = 0;\n",
                        "lookalike/imgui/imgui_tables.cpp": "int fake_imgui_tables_anchor = 0;\n",
                        "lookalike/imgui/imgui_widgets.cpp": "int fake_imgui_widgets_anchor = 0;\n",
                    },
                )
                with self.assertRaisesRegex(ValidationError, r"(?i)Dear ImGui.*outside allowedUiFoundation"):
                    validate_editor_dependency_boundary(root)

    def test_rejects_forged_dear_imgui_fetchcontent_provenance(self) -> None:
        core_sources = """
          ${imgui_SOURCE_DIR}/imgui.cpp
          ${imgui_SOURCE_DIR}/imgui.h
          ${imgui_SOURCE_DIR}/imgui_draw.cpp
          ${imgui_SOURCE_DIR}/imgui_tables.cpp
          ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        """
        declarations = (
            """
            FetchContent_Declare(imgui
              GIT_REPOSITORY https://github.com/ocornut/imgui.git
              GIT_TAG docking)
            FetchContent_MakeAvailable(imgui)
            """,
            """
            FetchContent_Declare(imgui
              GIT_REPOSITORY https://github.com/ocornut/imgui.git
              GIT_TAG fd13a1e8923a0a7077b404fc36fd063b25a0c0b5
              SOURCE_DIR lookalike/imgui)
            FetchContent_MakeAvailable(imgui)
            """,
            """
            FetchContent_Declare(imgui
              GIT_REPOSITORY https://github.com/ocornut/imgui.git
              GIT_TAG fd13a1e8923a0a7077b404fc36fd063b25a0c0b5)
            set(imgui_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lookalike/imgui")
            FetchContent_MakeAvailable(imgui)
            """,
            """
            FetchContent_Declare(imgui
              GIT_REPOSITORY https://github.com/ocornut/imgui.git
              GIT_TAG fd13a1e8923a0a7077b404fc36fd063b25a0c0b5)
            """,
            """
            FetchContent_Declare(imgui
              URL file:///synthetic/spoof-imgui.tar.gz
              URL_HASH SHA256=0000000000000000000000000000000000000000000000000000000000000000)
            FetchContent_MakeAvailable(imgui)
            """,
            """
            FetchContent_Declare(imgui
              GIT_REPOSITORY https://github.com/ocornut/imgui.git
              GIT_TAG fd13a1e8923a0a7077b404fc36fd063b25a0c0b5
              GIT_REPOSITORY file:///synthetic/spoof-imgui
              GIT_TAG main)
            FetchContent_MakeAvailable(imgui)
            """,
            """
            FetchContent_Declare(imgui
              GIT_REPOSITORY https://github.com/ocornut/imgui.git
              GIT_TAG fd13a1e8923a0a7077b404fc36fd063b25a0c0b5)
            FetchContent_Declare(imgui
              GIT_REPOSITORY file:///synthetic/spoof-imgui
              GIT_TAG main)
            FetchContent_MakeAvailable(imgui)
            """,
            """
            set(FETCHCONTENT_SOURCE_DIR_IMGUI "${CMAKE_CURRENT_SOURCE_DIR}/lookalike/imgui")
            FetchContent_Declare(imgui
              GIT_REPOSITORY https://github.com/ocornut/imgui.git
              GIT_TAG fd13a1e8923a0a7077b404fc36fd063b25a0c0b5)
            FetchContent_MakeAvailable(imgui)
            """,
            """
            function(FetchContent_MakeAvailable)
            endfunction()
            FetchContent_Declare(imgui
              GIT_REPOSITORY https://github.com/ocornut/imgui.git
              GIT_TAG fd13a1e8923a0a7077b404fc36fd063b25a0c0b5)
            FetchContent_MakeAvailable(imgui)
            """,
        )
        for declaration in declarations:
            with self.subTest(declaration=declaration.strip().splitlines()[-1].strip()):
                root = self.make_dependency_project(
                    f"""
                    include(FetchContent)
                    {declaration}
                    add_library(dear_imgui_upstream STATIC {core_sources})
                    add_library(DearImGui::DearImGui ALIAS dear_imgui_upstream)
                    add_library(scene_editor_core STATIC scene/editor/Core.cpp)
                    target_link_libraries(scene_editor_core PRIVATE DearImGui::DearImGui)
                    """,
                    {
                        "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                        "lookalike/imgui/imgui.cpp": "int forged_body = 0;\n",
                    },
                )
                with self.assertRaisesRegex(ValidationError, r"(?i)Dear ImGui.*outside allowedUiFoundation"):
                    validate_editor_dependency_boundary(root)

    def test_allows_safe_variable_from_reachable_include(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(mgif_scene_adapter INTERFACE)
            include(config/editor-foundation.cmake)
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            target_link_libraries(scene_editor_core PRIVATE ${EDITOR_LINK})
            """,
            {
                "config/editor-foundation.cmake": "set(EDITOR_LINK mgif_scene_adapter)\n",
                "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
            },
        )
        validate_editor_dependency_boundary(root)

    def test_function_local_safe_shadow_does_not_hide_parent_forbidden_value(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(mgif_scene_adapter INTERFACE)
            set(EDITOR_LINK nvvk)
            function(configure_locally)
              set(EDITOR_LINK mgif_scene_adapter)
            endfunction()
            configure_locally()
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            target_link_libraries(scene_editor_core PRIVATE ${EDITOR_LINK})
            """,
            {"scene/editor/Core.cpp": "int editor_core_anchor = 0;\n"},
        )
        with self.assertRaisesRegex(
            ValidationError,
            r"(?i)target_link_libraries.*nvvk|nvvk.*target_link_libraries",
        ):
            validate_editor_dependency_boundary(root)

    def test_child_directory_safe_shadow_does_not_hide_parent_forbidden_value(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(mgif_scene_adapter INTERFACE)
            set(EDITOR_LINK nvvk)
            add_subdirectory(child)
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            target_link_libraries(scene_editor_core PRIVATE ${EDITOR_LINK})
            """,
            {
                "child/CMakeLists.txt": "set(EDITOR_LINK mgif_scene_adapter)\n",
                "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
            },
        )
        with self.assertRaisesRegex(
            ValidationError,
            r"(?i)target_link_libraries.*nvvk|nvvk.*target_link_libraries",
        ):
            validate_editor_dependency_boundary(root)

    def test_callable_redefinition_is_applied_in_command_time_order(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(mgif_scene_adapter INTERFACE)
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            function(wire target)
              target_link_libraries(${target} PRIVATE nvvk)
            endfunction()
            wire(scene_editor_core)
            function(wire target)
              target_link_libraries(${target} PRIVATE mgif_scene_adapter)
            endfunction()
            """,
            {"scene/editor/Core.cpp": "int editor_core_anchor = 0;\n"},
        )
        with self.assertRaisesRegex(
            ValidationError,
            r"(?i)target_link_libraries.*nvvk|nvvk.*target_link_libraries",
        ):
            validate_editor_dependency_boundary(root)

    def test_foreach_values_and_loop_variable_scope_cannot_hide_dependencies(self) -> None:
        cases = (
            """
            foreach(EDITOR_LINK IN ITEMS nvvk mgif_scene_adapter)
              target_link_libraries(scene_editor_core PRIVATE ${EDITOR_LINK})
            endforeach()
            """,
            """
            set(EDITOR_LINK nvvk)
            foreach(EDITOR_LINK IN ITEMS mgif_scene_adapter)
            endforeach()
            target_link_libraries(scene_editor_core PRIVATE ${EDITOR_LINK})
            """,
        )
        for loop in cases:
            with self.subTest(case="body" if "target_link_libraries" in loop.split("endforeach", 1)[0] else "restore"):
                root = self.make_dependency_project(
                    f"""
                    add_library(mgif_scene_adapter INTERFACE)
                    add_library(scene_editor_core STATIC scene/editor/Core.cpp)
                    {loop}
                    """,
                    {"scene/editor/Core.cpp": "int editor_core_anchor = 0;\n"},
                )
                with self.assertRaisesRegex(
                    ValidationError,
                    r"(?i)target_link_libraries.*nvvk|nvvk.*target_link_libraries",
                ):
                    validate_editor_dependency_boundary(root)

    def test_nested_include_add_subdirectory_uses_caller_source_directory(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            add_subdirectory(external)
            target_link_libraries(scene_editor_core PRIVATE platform_bridge)
            """,
            {
                "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                "external/CMakeLists.txt": "include(../config/wire.cmake)\n",
                "config/wire.cmake": "add_subdirectory(native)\n",
                "external/native/CMakeLists.txt": (
                    "add_library(platform_bridge INTERFACE)\n"
                    "target_link_libraries(platform_bridge INTERFACE nvvk)\n"
                ),
            },
        )
        with self.assertRaisesRegex(
            ValidationError,
            r"(?i)(?=.*external/native)(?=.*target_link_libraries)(?=.*nvvk)",
        ):
            validate_editor_dependency_boundary(root)

    def test_included_module_preserves_current_source_and_current_list_directories(self) -> None:
        forbidden_root = self.make_dependency_project(
            """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            add_subdirectory(external)
            """,
            {
                "scene/editor/Core.cpp": "#include <bridge.hpp>\nint editor_core_anchor = 0;\n",
                "external/CMakeLists.txt": "include(../config/wire.cmake)\n",
                "config/wire.cmake": (
                    "target_include_directories(scene_editor_core PRIVATE "
                    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/sdk)\n"
                ),
                "external/vendor/sdk/bridge.hpp": "#include <nvvk/context.hpp>\n",
            },
        )
        with self.assertRaisesRegex(ValidationError, r"(?i)bridge.*nvvk|nvvk.*bridge"):
            validate_editor_dependency_boundary(forbidden_root)

        safe_root = self.make_dependency_project(
            """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            add_subdirectory(external)
            """,
            {
                "scene/editor/Core.cpp": "#include <bridge.hpp>\nint editor_core_anchor = 0;\n",
                "external/CMakeLists.txt": "include(../config/wire.cmake)\n",
                "config/wire.cmake": (
                    "target_include_directories(scene_editor_core PRIVATE "
                    "${CMAKE_CURRENT_LIST_DIR}/vendor/sdk)\n"
                ),
                "config/vendor/sdk/bridge.hpp": "#pragma once\n",
                "external/vendor/sdk/bridge.hpp": "#include <nvvk/context.hpp>\n",
            },
        )
        validate_editor_dependency_boundary(safe_root)

    def test_module_path_and_function_wrapped_traversal_are_followed(self) -> None:
        cases = (
            (
                """
                list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/config")
                include(WireNative)
                """,
                {"config/WireNative.cmake": "add_subdirectory(native)\n"},
            ),
            (
                """
                function(load_native)
                  add_subdirectory(native)
                endfunction()
                load_native()
                """,
                {},
            ),
        )
        for traversal, extra_files in cases:
            with self.subTest(traversal=traversal.strip().splitlines()[0].strip()):
                root = self.make_dependency_project(
                    f"""
                    add_library(scene_editor_core STATIC scene/editor/Core.cpp)
                    {traversal}
                    target_link_libraries(scene_editor_core PRIVATE platform_bridge)
                    """,
                    {
                        "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                        "native/CMakeLists.txt": (
                            "add_library(platform_bridge INTERFACE)\n"
                            "target_link_libraries(platform_bridge INTERFACE nvvk)\n"
                        ),
                        **extra_files,
                    },
                )
                with self.assertRaisesRegex(
                    ValidationError,
                    r"(?i)(?=.*native/CMakeLists)(?=.*target_link_libraries)(?=.*nvvk)",
                ):
                    validate_editor_dependency_boundary(root)

    def test_external_cmake_traversal_cannot_join_the_mgif_build_graph(self) -> None:
        external_root = self.root / "external-cmake-inputs"
        (external_root / "module").mkdir(parents=True)
        (external_root / "module/WireExternal.cmake").write_text(
            "target_link_libraries(scene_editor_core PRIVATE nvvk)\n",
            encoding="utf-8",
        )
        (external_root / "subdir").mkdir()
        (external_root / "subdir/CMakeLists.txt").write_text(
            "target_link_libraries(scene_editor_core PRIVATE nvvk)\n",
            encoding="utf-8",
        )
        cases = (
            f'list(APPEND CMAKE_MODULE_PATH "{(external_root / "module").as_posix()}")\ninclude(WireExternal)',
            f'add_subdirectory("{(external_root / "subdir").as_posix()}")',
        )
        for traversal in cases:
            with self.subTest(command=traversal.splitlines()[-1].split("(", 1)[0]):
                root = self.make_dependency_project(
                    f"""
                    add_library(scene_editor_core STATIC scene/editor/Core.cpp)
                    {traversal}
                    """,
                    {"scene/editor/Core.cpp": "int editor_core_anchor = 0;\n"},
                )
                with self.assertRaisesRegex(ValidationError, r"(?i)external CMake traversal"):
                    validate_editor_dependency_boundary(root)

    def test_late_source_attachments_promote_neutral_targets_to_editor_production(self) -> None:
        attachment_commands = (
            "target_sources(platform_bridge PRIVATE scene/editor/Overlay.cpp)",
            "set_property(TARGET platform_bridge APPEND PROPERTY SOURCES scene/editor/Overlay.cpp)",
            "set_target_properties(platform_bridge PROPERTIES PUBLIC_HEADER scene/editor/Overlay.h)",
        )
        for attachment in attachment_commands:
            with self.subTest(attachment=attachment.split("(", 1)[0]):
                root = self.make_dependency_project(
                    f"""
                    add_library(platform_bridge STATIC scene/PlatformBridge.cpp)
                    {attachment}
                    target_link_libraries(platform_bridge PRIVATE nvvk)
                    """,
                    {
                        "scene/PlatformBridge.cpp": "int platform_bridge_anchor = 0;\n",
                        "scene/editor/Overlay.cpp": "int overlay_anchor = 0;\n",
                        "scene/editor/Overlay.h": "#pragma once\n",
                    },
                )
                with self.assertRaisesRegex(
                    ValidationError,
                    r"(?i)target_link_libraries.*nvvk|nvvk.*target_link_libraries",
                ):
                    validate_editor_dependency_boundary(root)

    def test_recursive_positive_allowlist_rejects_wrapped_ui_and_imgui_lookalikes(self) -> None:
        cases = (
            """
            add_library(platform_bridge INTERFACE)
            add_library(Qt6::Widgets INTERFACE IMPORTED)
            target_link_libraries(platform_bridge INTERFACE Qt6::Widgets)
            target_link_libraries(scene_editor_core PRIVATE platform_bridge)
            """,
            """
            add_library(NotRepositoryImGui::Widgets INTERFACE IMPORTED)
            target_link_libraries(scene_editor_core PRIVATE NotRepositoryImGui::Widgets)
            """,
            """
            add_library(custom_widgets INTERFACE)
            target_link_libraries(scene_editor_core PRIVATE custom_widgets)
            """,
        )
        for cmake_tail in cases:
            dependency = "Qt" if "Qt6" in cmake_tail else ("lookalike" if "NotRepository" in cmake_tail else "internal UI")
            with self.subTest(dependency=dependency):
                root = self.make_dependency_project(
                    f"""
                    add_library(scene_editor_core STATIC scene/editor/Core.cpp)
                    {cmake_tail}
                    """,
                    {"scene/editor/Core.cpp": "int editor_core_anchor = 0;\n"},
                )
                with self.assertRaisesRegex(ValidationError, r"(?i)outside allowedUiFoundation"):
                    validate_editor_dependency_boundary(root)

    def test_recursive_positive_allowlist_rejects_link_option_and_import_channels(self) -> None:
        cases = (
            "target_link_libraries(platform_bridge INTERFACE -lQt6Widgets)",
            "target_link_options(platform_bridge INTERFACE -lQt6Widgets)",
            "set_property(TARGET platform_bridge PROPERTY INTERFACE_LINK_OPTIONS -lQt6Widgets)",
            "set_target_properties(platform_bridge PROPERTIES LINK_OPTIONS Qt6Widgets.lib)",
            "target_link_options(platform_bridge INTERFACE "
            "-I${CMAKE_CURRENT_SOURCE_DIR}/vendor/Qt6/include)",
            """
            add_library(qt_payload UNKNOWN IMPORTED)
            set_target_properties(qt_payload PROPERTIES
              IMPORTED_CONFIGURATIONS RELEASE
              IMPORTED_LOCATION_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/vendor/Qt6Widgets.lib")
            target_link_libraries(platform_bridge INTERFACE qt_payload)
            """,
        )
        for cmake_tail in cases:
            with self.subTest(channel=cmake_tail.strip().split("(", 1)[0]):
                root = self.make_dependency_project(
                    f"""
                    add_library(scene_editor_core STATIC scene/editor/Core.cpp)
                    add_library(platform_bridge INTERFACE)
                    {cmake_tail}
                    target_link_libraries(scene_editor_core PRIVATE platform_bridge)
                    """,
                    {
                        "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                        "vendor/Qt6Widgets.lib": "synthetic import\n",
                    },
                )
                with self.assertRaisesRegex(ValidationError, r"(?i)outside allowedUiFoundation"):
                    validate_editor_dependency_boundary(root)

    def test_recursive_positive_allowlist_rejects_external_include_channel(self) -> None:
        cases = (
            "target_include_directories(platform_bridge INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/vendor/Qt6/include)",
            "target_compile_options(platform_bridge INTERFACE "
            "-I${CMAKE_CURRENT_SOURCE_DIR}/vendor/Qt6/include)",
            "set_property(TARGET platform_bridge PROPERTY INTERFACE_INCLUDE_DIRECTORIES "
            "${CMAKE_CURRENT_SOURCE_DIR}/vendor/Qt6/include)",
            "set_property(TARGET platform_bridge PROPERTY INTERFACE_COMPILE_OPTIONS "
            "-I${CMAKE_CURRENT_SOURCE_DIR}/vendor/Qt6/include)",
        )
        for cmake_tail in cases:
            with self.subTest(channel=cmake_tail.split("(", 1)[0]):
                root = self.make_dependency_project(
                    f"""
                    add_library(scene_editor_core STATIC scene/editor/Core.cpp)
                    add_library(platform_bridge INTERFACE)
                    {cmake_tail}
                    target_link_libraries(scene_editor_core PRIVATE platform_bridge)
                    """,
                    {
                        "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                        "vendor/Qt6/include/QtWidgets": "synthetic include root\n",
                    },
                )
                with self.assertRaisesRegex(ValidationError, r"(?i)outside allowedUiFoundation"):
                    validate_editor_dependency_boundary(root)

    def test_allows_benign_link_option_and_mgif_include_channels(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            add_library(platform_bridge STATIC render/Bridge.cpp)
            target_include_directories(platform_bridge INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/render/include)
            set_property(TARGET platform_bridge PROPERTY INTERFACE_LINK_OPTIONS -Wl,--as-needed)
            target_link_libraries(scene_editor_core PRIVATE platform_bridge)
            """,
            {
                "scene/editor/Core.cpp": (
                    "#include <system_error>\n"
                    "#include <cwchar>\n"
                    "#include <random>\n"
                    "#include <commdlg.h>\n"
                    "#include <GLFW/glfw3.h>\n"
                    "#include <imgui_internal.h>\n"
                    "#include <nvtx3/nvToolsExt.h>\n"
                    "int editor_core_anchor = 0;\n"
                ),
                "render/Bridge.cpp": "int platform_bridge_anchor = 0;\n",
                "render/include/Bridge.h": "#pragma once\n",
            },
        )
        validate_editor_dependency_boundary(root)

    def test_source_less_mgif_contract_keeps_its_transitive_closure_guarded(self) -> None:
        foundation = """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            add_library(scene_runtime_mapping_runtime STATIC scene/editor/Mapping.cpp)
            add_library(scene_runtime_mapping_contract INTERFACE)
            target_link_libraries(scene_runtime_mapping_contract INTERFACE
              scene_runtime_mapping_runtime)
            target_link_libraries(scene_editor_core PRIVATE
              scene_runtime_mapping_contract)
        """
        files = {
            "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
            "scene/editor/Mapping.cpp": "int mapping_anchor = 0;\n",
        }
        validate_editor_dependency_boundary(
            self.make_dependency_project(foundation, files)
        )

        hostile = self.make_dependency_project(
            foundation
            + """
              add_library(Qt6::Widgets INTERFACE IMPORTED)
              target_link_libraries(scene_runtime_mapping_contract INTERFACE
                Qt6::Widgets)
            """,
            files,
        )
        with self.assertRaisesRegex(
            ValidationError,
            r"(?i)Qt6::Widgets.*outside allowedUiFoundation|outside allowedUiFoundation.*Qt6::Widgets",
        ):
            validate_editor_dependency_boundary(hostile)

    def test_demo_core_android_platform_inputs_require_exact_guarded_edge(self) -> None:
        canonical = """
            add_library(scene_runtime_mapping_runtime STATIC scene/editor/Mapping.cpp)
            add_library(demo_core STATIC render/editor/DemoCore.cpp)
            if(ANDROID)
              add_library(native_app_glue STATIC rhi/NativeGlue.cpp)
              target_link_libraries(demo_core
                PRIVATE
                  scene_runtime_mapping_runtime
                PUBLIC android log native_app_glue)
            endif()
        """
        files = {
            "scene/editor/Mapping.cpp": "int mapping_anchor = 0;\n",
            "render/editor/DemoCore.cpp": "int demo_core_anchor = 0;\n",
            "rhi/NativeGlue.cpp": "int native_glue_anchor = 0;\n",
        }
        validate_editor_dependency_boundary(
            self.make_dependency_project(canonical, files)
        )

        hostile_cases = (
            canonical + "\ntarget_link_libraries(demo_core PRIVATE android)",
            canonical.replace("PUBLIC android log", "PUBLIC log android"),
            canonical.replace("if(ANDROID)", "if(WIN32)"),
        )
        for index, hostile_cmake in enumerate(hostile_cases):
            with self.subTest(case=index):
                with self.assertRaisesRegex(
                    ValidationError,
                    r"(?i)(?:android|log).*outside allowedUiFoundation|outside allowedUiFoundation.*(?:android|log)",
                ):
                    validate_editor_dependency_boundary(
                        self.make_dependency_project(hostile_cmake, files)
                    )

    def test_demo_core_unix_platform_inputs_require_exact_guarded_edge(self) -> None:
        canonical = """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            add_library(demo_core STATIC render/editor/DemoCore.cpp)
            target_link_libraries(scene_editor_core PRIVATE demo_core)
            if(UNIX AND NOT APPLE AND NOT ANDROID)
              find_package(X11 REQUIRED)
              find_package(Threads REQUIRED)
              target_link_libraries(demo_core PUBLIC
                ${X11_LIBRARIES} Threads::Threads)
            endif()
        """
        files = {
            "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
            "render/editor/DemoCore.cpp": "int demo_core_anchor = 0;\n",
        }
        validate_editor_dependency_boundary(
            self.make_dependency_project(canonical, files)
        )

        hostile_cases = (
            canonical + "\ntarget_link_libraries(demo_core PRIVATE Threads::Threads)",
            canonical.replace(
                "${X11_LIBRARIES} Threads::Threads",
                "Threads::Threads ${X11_LIBRARIES}",
            ),
            canonical.replace(
                "if(UNIX AND NOT APPLE AND NOT ANDROID)",
                "if(UNIX)",
            ),
            canonical.replace("find_package(X11 REQUIRED)\n", ""),
        )
        for index, hostile_cmake in enumerate(hostile_cases):
            with self.subTest(case=index):
                with self.assertRaisesRegex(
                    ValidationError,
                    r"(?i)(?:X11|Threads).*outside allowedUiFoundation|outside allowedUiFoundation.*(?:X11|Threads)",
                ):
                    validate_editor_dependency_boundary(
                        self.make_dependency_project(hostile_cmake, files)
                    )

    def test_demo_core_windows_sdk_inputs_require_exact_nested_edges(self) -> None:
        canonical = """
            add_library(demo_core STATIC render/editor/DemoCore.cpp)
            if(WIN32)
              target_link_libraries(demo_core PUBLIC winmm.lib)
              if(DEMO_ENABLE_D3D12)
                target_link_libraries(demo_core PUBLIC
                  d3d12.lib dxgi.lib dxguid.lib)
              endif()
            endif()
        """
        files = {
            "render/editor/DemoCore.cpp": "int demo_core_anchor = 0;\n",
        }
        validate_editor_dependency_boundary(
            self.make_dependency_project(canonical, files)
        )

        hostile_cases = (
            canonical + "\ntarget_link_libraries(demo_core PRIVATE winmm.lib)",
            canonical.replace("if(WIN32)", "if(UNIX)", 1),
            canonical.replace(
                "d3d12.lib dxgi.lib dxguid.lib",
                "dxgi.lib d3d12.lib dxguid.lib",
            ),
            canonical.replace("if(DEMO_ENABLE_D3D12)", "if(WIN32)"),
        )
        for index, hostile_cmake in enumerate(hostile_cases):
            with self.subTest(case=index):
                with self.assertRaisesRegex(
                    ValidationError,
                    r"(?i)(?:winmm|d3d12|dxgi|dxguid).*outside allowedUiFoundation|outside allowedUiFoundation.*(?:winmm|d3d12|dxgi|dxguid)",
                ):
                    validate_editor_dependency_boundary(
                        self.make_dependency_project(hostile_cmake, files)
                    )

    def test_texture_converter_nvtt_requires_exact_tool_authority(self) -> None:
        canonical = """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            if(NOT ANDROID)
              add_executable(texture_converter tools/texture_converter.cpp)
              set(NVTT_ROOT "C:/Program Files/NVIDIA Corporation/NVIDIA Texture Tools"
                CACHE PATH "Path to the NVIDIA Texture Tools install root")
              set(NVTT_INCLUDE_DIR "${NVTT_ROOT}/include")
              set(NVTT_LIBRARY "${NVTT_ROOT}/lib/x64-v142/nvtt30205.lib")
              set(NVTT_DLL "${NVTT_ROOT}/nvtt30205.dll")
              target_link_libraries(texture_converter PRIVATE "${NVTT_LIBRARY}")
              add_dependencies(scene_editor_core texture_converter)
            endif()
        """
        files = {
            "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
            "tools/texture_converter.cpp": "int main() { return 0; }\n",
        }
        validate_editor_dependency_boundary(
            self.make_dependency_project(canonical, files)
        )

        hostile_cases = (
            canonical + "\ntarget_link_libraries(texture_converter PRIVATE ${NVTT_LIBRARY})",
            canonical.replace("if(NOT ANDROID)", "if(WIN32)"),
            canonical.replace("x64-v142/nvtt30205.lib", "x64-v142/other.lib"),
            canonical.replace(
                "target_link_libraries(texture_converter PRIVATE \"${NVTT_LIBRARY}\")",
                "target_link_libraries(scene_editor_core PRIVATE \"${NVTT_LIBRARY}\")",
            ),
        )
        for index, hostile_cmake in enumerate(hostile_cases):
            with self.subTest(case=index):
                with self.assertRaisesRegex(
                    ValidationError,
                    r"(?i)(?:nvtt|Texture Tools).*outside allowedUiFoundation|outside allowedUiFoundation.*(?:nvtt|Texture Tools)",
                ):
                    validate_editor_dependency_boundary(
                        self.make_dependency_project(hostile_cmake, files)
                    )

    def test_dear_imgui_exemption_does_not_hide_transitive_ui_dependency(self) -> None:
        root = self.make_dependency_project(
            """
            include(FetchContent)
            FetchContent_Declare(imgui
              GIT_REPOSITORY https://github.com/ocornut/imgui.git
              GIT_TAG fd13a1e8923a0a7077b404fc36fd063b25a0c0b5)
            FetchContent_MakeAvailable(imgui)
            add_library(dear_imgui_upstream STATIC
              ${imgui_SOURCE_DIR}/imgui.cpp
              ${imgui_SOURCE_DIR}/imgui.h
              ${imgui_SOURCE_DIR}/imgui_draw.cpp
              ${imgui_SOURCE_DIR}/imgui_tables.cpp
              ${imgui_SOURCE_DIR}/imgui_widgets.cpp)
            add_library(DearImGui::DearImGui ALIAS dear_imgui_upstream)
            add_library(Qt6::Widgets INTERFACE IMPORTED)
            target_link_libraries(dear_imgui_upstream PRIVATE Qt6::Widgets)
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            target_link_libraries(scene_editor_core PRIVATE DearImGui::DearImGui)
            """,
            {
                "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
            },
        )
        with self.assertRaisesRegex(ValidationError, r"(?i)Qt6::Widgets.*outside allowedUiFoundation"):
            validate_editor_dependency_boundary(root)

    def test_imported_or_aliased_targets_cannot_claim_production_editor_identity(self) -> None:
        cases = (
            "add_library(future_editor_backend INTERFACE IMPORTED)",
            "add_library(external_backend INTERFACE IMPORTED)\n"
            "add_library(AppEditor::Backend ALIAS external_backend)",
        )
        for index, cmake in enumerate(cases):
            with self.subTest(index=index):
                root = self.make_dependency_project(cmake, {})
                with self.assertRaisesRegex(ValidationError, r"(?i)imported.*production editor identity"):
                    validate_editor_dependency_boundary(root)

    def test_all_source_property_pairs_are_dependency_scanned(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            set_source_files_properties(scene/editor/Core.cpp PROPERTIES
              LANGUAGE CXX
              COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/config/forced.hpp")
            """,
            {
                "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                "config/forced.hpp": "#include <nvvk/context.hpp>\n",
            },
        )
        with self.assertRaisesRegex(ValidationError, r"(?i)forced.*nvvk|nvvk.*forced"):
            validate_editor_dependency_boundary(root)

    def test_global_c_and_cxx_flag_forced_includes_are_dependency_scanned(self) -> None:
        cases = (
            ("set(CMAKE_CXX_FLAGS \"-include ${CMAKE_CURRENT_SOURCE_DIR}/config/forced.hpp\")", "Core.cpp"),
            ("set(CMAKE_C_FLAGS_DEBUG \"/FI${CMAKE_CURRENT_SOURCE_DIR}/config/forced.hpp\")", "Core.c"),
            ("string(APPEND CMAKE_CXX_FLAGS \" -imacros ${CMAKE_CURRENT_SOURCE_DIR}/config/forced.hpp\")", "Core.cpp"),
            (
                "string(CONCAT CMAKE_CXX_FLAGS \"-include \" "
                "\"${CMAKE_CURRENT_SOURCE_DIR}/config/forced.hpp\")",
                "Core.cpp",
            ),
            (
                "string(JOIN \" \" CMAKE_C_FLAGS -include "
                "\"${CMAKE_CURRENT_SOURCE_DIR}/config/forced.hpp\")",
                "Core.c",
            ),
            (
                "string(REPLACE @FORCED@ \"-include ${CMAKE_CURRENT_SOURCE_DIR}/config/forced.hpp\" "
                "CMAKE_CXX_FLAGS @FORCED@)",
                "Core.cpp",
            ),
            (
                "set(EDITOR_FLAGS -include ${CMAKE_CURRENT_SOURCE_DIR}/config/forced.hpp)\n"
                "list(JOIN EDITOR_FLAGS \" \" CMAKE_CXX_FLAGS)",
                "Core.cpp",
            ),
            (
                "cmake_parse_arguments(CMAKE_CXX \"\" \"\" FLAGS FLAGS -include "
                "${CMAKE_CURRENT_SOURCE_DIR}/config/forced.hpp)",
                "Core.cpp",
            ),
        )
        for declaration, source_name in cases:
            with self.subTest(variable=declaration.split("(", 1)[1].split()[0]):
                root = self.make_dependency_project(
                    f"""
                    {declaration}
                    add_library(scene_editor_core STATIC scene/editor/{source_name})
                    """,
                    {
                        f"scene/editor/{source_name}": "int editor_core_anchor = 0;\n",
                        "config/forced.hpp": "#include <nvvk/context.hpp>\n",
                    },
                )
                with self.assertRaisesRegex(ValidationError, r"(?i)forced.*nvvk|nvvk.*forced"):
                    validate_editor_dependency_boundary(root)

    def test_empty_cmake_arguments_keep_join_and_replace_positions(self) -> None:
        cases = (
            'string(JOIN "" BAD nv vk)',
            'string(REPLACE @DROP@ "" BAD nv@DROP@vk)',
            'set(PARTS nv vk)\nlist(JOIN PARTS "" BAD)',
        )
        for declaration in cases:
            with self.subTest(declaration=declaration):
                root = self.make_dependency_project(
                    f"""
                    {declaration}
                    add_library(scene_editor_core STATIC scene/editor/Core.cpp)
                    target_link_libraries(scene_editor_core PRIVATE ${{BAD}})
                    """,
                    {"scene/editor/Core.cpp": "int editor_core_anchor = 0;\n"},
                )
                with self.assertRaisesRegex(ValidationError, r"(?i)forbidden editor dependency.*nvvk"):
                    validate_editor_dependency_boundary(root)

    def test_forced_include_spellings_are_dependency_scanned(self) -> None:
        declarations = (
            'target_compile_options(scene_editor_core PRIVATE "SHELL:-include config/forced.hpp")',
            'target_compile_options(scene_editor_core PRIVATE /FI config/forced.hpp)',
            'target_compile_options(scene_editor_core PRIVATE -Xclang -include -Xclang config/forced.hpp)',
            'set_target_properties(scene_editor_core PROPERTIES COMPILE_OPTIONS "SHELL:-include config/forced.hpp")',
            'add_compile_options("SHELL:-include config/forced.hpp")',
        )
        for declaration in declarations:
            with self.subTest(declaration=declaration):
                root = self.make_dependency_project(
                    f"""
                    add_library(scene_editor_core STATIC scene/editor/Core.cpp)
                    {declaration}
                    """,
                    {
                        "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                        "config/forced.hpp": "#include <nvvk/context.hpp>\n",
                    },
                )
                with self.assertRaisesRegex(ValidationError, r"(?i)forced.*nvvk|nvvk.*forced"):
                    validate_editor_dependency_boundary(root)

    def test_string_ascii_and_list_pop_mutations_are_interpreted(self) -> None:
        cases = (
            "string(ASCII 110 118 118 107 BAD)",
            "set(BAD neutral nvvk)\nlist(POP_FRONT BAD)",
            "set(BAD nvvk neutral)\nlist(POP_BACK BAD REMOVED)",
        )
        for declaration in cases:
            with self.subTest(declaration=declaration):
                root = self.make_dependency_project(
                    f"""
                    {declaration}
                    add_library(scene_editor_core STATIC scene/editor/Core.cpp)
                    target_link_libraries(scene_editor_core PRIVATE ${{BAD}})
                    """,
                    {"scene/editor/Core.cpp": "int editor_core_anchor = 0;\n"},
                )
                with self.assertRaisesRegex(ValidationError, r"(?i)forbidden editor dependency.*nvvk"):
                    validate_editor_dependency_boundary(root)

    def test_unsupported_variable_mutations_fail_closed_when_dependency_bearing(self) -> None:
        cases = (
            "set(BAD neutral)\nlist(TRANSFORM BAD APPEND nvvk)",
            "set(BAD neutral)\nstring(REGEX REPLACE neutral nvvk BAD neutral)",
        )
        for declaration in cases:
            with self.subTest(declaration=declaration):
                root = self.make_dependency_project(
                    f"""
                    {declaration}
                    add_library(scene_editor_core STATIC scene/editor/Core.cpp)
                    target_link_libraries(scene_editor_core PRIVATE ${{BAD}})
                    """,
                    {"scene/editor/Core.cpp": "int editor_core_anchor = 0;\n"},
                )
                with self.assertRaisesRegex(ValidationError, r"(?i)cannot resolve editor dependency declaration"):
                    validate_editor_dependency_boundary(root)

    def test_known_cmake_producers_cannot_overwrite_a_modeled_dependency_value(self) -> None:
        cases = (
            'file(READ payload.txt BAD)',
            'file(STRINGS payload.txt BAD)',
            'execute_process(COMMAND cmake -E echo nvvk OUTPUT_VARIABLE BAD)',
            'find_library(BAD neutral)',
            'math(EXPR BAD "1 + 1")',
        )
        for declaration in cases:
            with self.subTest(declaration=declaration.split("(", 1)[0]):
                root = self.make_dependency_project(
                    f"""
                    set(BAD neutral)
                    {declaration}
                    add_library(scene_editor_core STATIC scene/editor/Core.cpp)
                    target_link_libraries(scene_editor_core PRIVATE ${{BAD}})
                    """,
                    {
                        "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                        "payload.txt": "nvvk\n",
                    },
                )
                with self.assertRaisesRegex(ValidationError, r"(?i)cannot resolve editor dependency declaration"):
                    validate_editor_dependency_boundary(root)

    def test_generated_cmake_includes_fail_closed(self) -> None:
        cases = (
            'file(WRITE generated.cmake "target_link_libraries(scene_editor_core PRIVATE nvvk)")',
            'file(APPEND generated.cmake "target_link_libraries(scene_editor_core PRIVATE nvvk)")',
            'file(GENERATE OUTPUT generated.cmake CONTENT "target_link_libraries(scene_editor_core PRIVATE nvvk)")',
            'file(CONFIGURE OUTPUT generated.cmake CONTENT "target_link_libraries(scene_editor_core PRIVATE nvvk)")',
            'configure_file(template.cmake.in generated.cmake @ONLY)',
        )
        for declaration in cases:
            with self.subTest(declaration=declaration.split("(", 1)[0]):
                root = self.make_dependency_project(
                    f"""
                    add_library(scene_editor_core STATIC scene/editor/Core.cpp)
                    {declaration}
                    include(generated.cmake)
                    """,
                    {
                        "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                        "template.cmake.in": "target_link_libraries(scene_editor_core PRIVATE nvvk)\n",
                    },
                )
                with self.assertRaisesRegex(ValidationError, r"(?i)unsupported generated CMake traversal"):
                    validate_editor_dependency_boundary(root)

    def test_unsupported_computed_global_flag_mutation_fails_closed(self) -> None:
        root = self.make_dependency_project(
            """
            string(REGEX REPLACE @FORCED@ "-include ${CMAKE_CURRENT_SOURCE_DIR}/config/forced.hpp"
              CMAKE_CXX_FLAGS @FORCED@)
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            """,
            {
                "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                "config/forced.hpp": "#include <nvvk/context.hpp>\n",
            },
        )
        with self.assertRaisesRegex(ValidationError, r"(?i)unsupported computed mutation.*CMAKE_CXX_FLAGS"):
            validate_editor_dependency_boundary(root)

    def test_function_local_compile_flags_do_not_leak_to_editor_targets(self) -> None:
        declarations = (
            "set(CMAKE_CXX_FLAGS \"-include ${CMAKE_CURRENT_SOURCE_DIR}/config/forced.hpp\")",
            "string(CONCAT CMAKE_CXX_FLAGS \"-include \" "
            "\"${CMAKE_CURRENT_SOURCE_DIR}/config/forced.hpp\")",
        )
        for declaration in declarations:
            with self.subTest(command=declaration.split("(", 1)[0]):
                root = self.make_dependency_project(
                    f"""
                    function(configure_tool)
                      {declaration}
                    endfunction()
                    configure_tool()
                    add_library(scene_editor_core STATIC scene/editor/Core.cpp)
                    """,
                    {
                        "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                        "config/forced.hpp": "#include <nvvk/context.hpp>\n",
                    },
                )
                validate_editor_dependency_boundary(root)

    def test_literal_false_branches_are_ignored_but_unknown_branches_fail_closed(self) -> None:
        false_conditions = (
            "FALSE",
            "(FALSE)",
            "((FALSE))",
            "OFF",
            "0",
            "IGNORE",
            "MISSING-NOTFOUND",
            "NOT TRUE",
            "FALSE OR 0",
            "FALSE AND (UNKNOWN_PLATFORM_SWITCH OR TRUE)",
            "((FALSE) AND (TRUE))",
            "NOT (TRUE OR UNKNOWN_PLATFORM_SWITCH)",
            "TRUE OR FALSE AND FALSE",
            "DISABLED",
        )
        for condition in false_conditions:
            with self.subTest(condition=condition):
                prefix = "set(DISABLED OFF)" if condition == "DISABLED" else ""
                root = self.make_dependency_project(
                    f"""
                    {prefix}
                    add_library(scene_editor_core STATIC scene/editor/Core.cpp)
                    if({condition})
                      target_link_libraries(scene_editor_core PRIVATE nvvk)
                    endif()
                    """,
                    {"scene/editor/Core.cpp": "int editor_core_anchor = 0;\n"},
                )
                validate_editor_dependency_boundary(root)

        live_conditions = (
            "UNKNOWN_PLATFORM_SWITCH",
            "UNKNOWN_PLATFORM_SWITCH AND (TRUE OR FALSE)",
            "FALSE OR UNKNOWN_PLATFORM_SWITCH AND TRUE",
            "NOT FALSE AND UNKNOWN_PLATFORM_SWITCH",
            "FALSE AND FALSE OR TRUE",
        )
        for condition in live_conditions:
            with self.subTest(live_condition=condition):
                unknown_root = self.make_dependency_project(
                    f"""
                    add_library(scene_editor_core STATIC scene/editor/Core.cpp)
                    if({condition})
                      target_link_libraries(scene_editor_core PRIVATE nvvk)
                    endif()
                    """,
                    {"scene/editor/Core.cpp": "int editor_core_anchor = 0;\n"},
                )
                with self.assertRaisesRegex(
                    ValidationError,
                    r"(?i)target_link_libraries.*nvvk|nvvk.*target_link_libraries",
                ):
                    validate_editor_dependency_boundary(unknown_root)

    def test_bare_strequal_operands_and_nested_boolean_precedence_match_cmake(self) -> None:
        live_conditions = (
            "SW STREQUAL ON",
            "(SW STREQUAL ON) AND (NOT (FALSE OR FALSE))",
            "FALSE OR ((SW STREQUAL ON) AND TRUE)",
            '(NOT (SW STREQUAL "OFF")) AND ("x" STREQUAL "x" OR FALSE)',
        )
        for condition in live_conditions:
            with self.subTest(live=condition):
                root = self.make_dependency_project(
                    f"""
                    set(SW ON)
                    add_library(scene_editor_core STATIC scene/editor/Core.cpp)
                    if({condition})
                      target_link_libraries(scene_editor_core PRIVATE nvvk)
                    endif()
                    """,
                    {"scene/editor/Core.cpp": "int editor_core_anchor = 0;\n"},
                )
                with self.assertRaisesRegex(
                    ValidationError,
                    r"(?i)target_link_libraries.*nvvk|nvvk.*target_link_libraries",
                ):
                    validate_editor_dependency_boundary(root)

        false_conditions = (
            '"SW" STREQUAL "ON"',
            "NOT (SW STREQUAL ON)",
            "SW STREQUAL ON OR FALSE AND FALSE",
        )
        for condition in false_conditions:
            with self.subTest(false=condition):
                root = self.make_dependency_project(
                    f"""
                    set(SW ON)
                    add_library(scene_editor_core STATIC scene/editor/Core.cpp)
                    if({condition})
                      target_link_libraries(scene_editor_core PRIVATE nvvk)
                    endif()
                    """,
                    {"scene/editor/Core.cpp": "int editor_core_anchor = 0;\n"},
                )
                validate_editor_dependency_boundary(root)

    def test_reference_probe_in_mgif_build_graph_is_not_an_exception(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            add_executable(editor_reference_probe tests/Probe.cpp)
            target_link_libraries(editor_reference_probe PRIVATE nvapp)
            """,
            {
                "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                "tests/Probe.cpp": "int main() { return 0; }\n",
            },
        )
        with self.assertRaisesRegex(
            ValidationError,
            r"(?i)target_link_libraries.*nvapp|nvapp.*target_link_libraries",
        ):
            validate_editor_dependency_boundary(root)

    def test_allows_output_name_metadata_without_dependency_semantics(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            add_library(platform_bridge STATIC render/Bridge.cpp)
            add_library(nvvk INTERFACE)
            set_target_properties(scene_editor_core PROPERTIES OUTPUT_NAME platform_bridge)
            target_link_libraries(platform_bridge PRIVATE nvvk)
            """,
            {
                "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                "render/Bridge.cpp": "int bridge_anchor = 0;\n",
            },
        )
        validate_editor_dependency_boundary(root)

    def test_independent_thirty_case_cmake_bypass_matrix(self) -> None:
        core = "add_library(scene_editor_core STATIC scene/editor/Core.cpp)"
        pinned_imgui = """
          include(FetchContent)
          FetchContent_Declare(imgui
            GIT_REPOSITORY https://github.com/ocornut/imgui.git
            GIT_TAG fd13a1e8923a0a7077b404fc36fd063b25a0c0b5)
          FetchContent_MakeAvailable(imgui)
        """
        imgui_sources = """
          ${imgui_SOURCE_DIR}/imgui.cpp
          ${imgui_SOURCE_DIR}/imgui.h
          ${imgui_SOURCE_DIR}/imgui_draw.cpp
          ${imgui_SOURCE_DIR}/imgui_tables.cpp
          ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        """
        cases = (
            ("direct-link", True, f"{core}\ntarget_link_libraries(scene_editor_core PRIVATE nvvk)"),
            ("link-option", True, f"{core}\ntarget_link_options(scene_editor_core PRIVATE -lQt6Widgets)"),
            ("interface-link-options", True, f"{core}\nadd_library(platform_bridge INTERFACE)\nset_property(TARGET platform_bridge PROPERTY INTERFACE_LINK_OPTIONS -lQt6Widgets)\ntarget_link_libraries(scene_editor_core PRIVATE platform_bridge)"),
            ("imported-location", True, f"{core}\nadd_library(platform_bridge UNKNOWN IMPORTED)\nset_target_properties(platform_bridge PROPERTIES IMPORTED_LOCATION ${{CMAKE_CURRENT_SOURCE_DIR}}/vendor/Qt6Widgets.lib)\ntarget_link_libraries(scene_editor_core PRIVATE platform_bridge)"),
            ("forged-dear-imgui", True, f"{pinned_imgui}\nset(imgui_SOURCE_DIR ${{CMAKE_CURRENT_SOURCE_DIR}}/lookalike/imgui)\nadd_library(fake_imgui STATIC {imgui_sources})\nadd_library(DearImGui::DearImGui ALIAS fake_imgui)\n{core}\ntarget_link_libraries(scene_editor_core PRIVATE DearImGui::DearImGui)"),
            ("string-concat", True, f"string(CONCAT BAD nv vk)\n{core}\ntarget_link_libraries(scene_editor_core PRIVATE ${{BAD}})"),
            ("string-join-empty", True, f"string(JOIN \"\" BAD nv vk)\n{core}\ntarget_link_libraries(scene_editor_core PRIVATE ${{BAD}})"),
            ("string-replace-empty", True, f"string(REPLACE @DROP@ \"\" BAD nv@DROP@vk)\n{core}\ntarget_link_libraries(scene_editor_core PRIVATE ${{BAD}})"),
            ("list-join-empty", True, f"set(PARTS nv vk)\nlist(JOIN PARTS \"\" BAD)\n{core}\ntarget_link_libraries(scene_editor_core PRIVATE ${{BAD}})"),
            ("parse-arguments-include", True, f"cmake_parse_arguments(CMAKE_CXX \"\" \"\" FLAGS FLAGS -include ${{CMAKE_CURRENT_SOURCE_DIR}}/config/forced.hpp)\n{core}"),
            ("shell-include", True, f"{core}\ntarget_compile_options(scene_editor_core PRIVATE \"SHELL:-include config/forced.hpp\")"),
            ("msvc-fi", True, f"{core}\ntarget_compile_options(scene_editor_core PRIVATE /FI config/forced.hpp)"),
            ("clang-include", True, f"{core}\ntarget_compile_options(scene_editor_core PRIVATE -include config/forced.hpp)"),
            ("xclang-include", True, f"{core}\ntarget_compile_options(scene_editor_core PRIVATE -Xclang -include -Xclang config/forced.hpp)"),
            ("compile-property", True, f"{core}\nset_target_properties(scene_editor_core PROPERTIES COMPILE_OPTIONS \"-include;config/forced.hpp\")"),
            ("global-compile-options", True, f"add_compile_options(\"SHELL:-include config/forced.hpp\")\n{core}"),
            ("string-ascii", True, f"string(ASCII 110 118 118 107 BAD)\n{core}\ntarget_link_libraries(scene_editor_core PRIVATE ${{BAD}})"),
            ("list-pop-back", True, f"set(BAD nvvk safe)\nlist(POP_BACK BAD)\n{core}\ntarget_link_libraries(scene_editor_core PRIVATE ${{BAD}})"),
            ("list-pop-front", True, f"set(BAD safe nvvk)\nlist(POP_FRONT BAD)\n{core}\ntarget_link_libraries(scene_editor_core PRIVATE ${{BAD}})"),
            ("file-write-include", True, f"{core}\nfile(WRITE generated.cmake \"target_link_libraries(scene_editor_core PRIVATE nvvk)\")\ninclude(generated.cmake)"),
            ("file-append-include", True, f"{core}\nfile(APPEND generated.cmake \"target_link_libraries(scene_editor_core PRIVATE nvvk)\")\ninclude(generated.cmake)"),
            ("file-generate-include", True, f"{core}\nfile(GENERATE OUTPUT generated.cmake CONTENT \"target_link_libraries(scene_editor_core PRIVATE nvvk)\")\ninclude(generated.cmake)"),
            ("file-configure-include", True, f"{core}\nfile(CONFIGURE OUTPUT generated.cmake CONTENT \"target_link_libraries(scene_editor_core PRIVATE nvvk)\")\ninclude(generated.cmake)"),
            ("configure-file-include", True, f"{core}\nconfigure_file(template.cmake.in generated.cmake @ONLY)\ninclude(generated.cmake)"),
            ("nested-bare-strequal", True, f"set(SW ON)\n{core}\nif((SW STREQUAL ON) AND (NOT (SW STREQUAL OFF)))\ntarget_link_libraries(scene_editor_core PRIVATE nvvk)\nendif()"),
            ("public-transitive-reexport", True, f"{core}\nadd_library(platform_bridge INTERFACE)\ntarget_link_libraries(platform_bridge INTERFACE nvvk)\ntarget_link_libraries(scene_editor_core PUBLIC platform_bridge)"),
            ("mgif-adapter-control", False, f"{core}\nadd_library(mgif_scene_adapter INTERFACE)\ntarget_link_libraries(scene_editor_core PRIVATE mgif_scene_adapter)"),
            ("upstream-imgui-control", False, f"{pinned_imgui}\nadd_library(dear_imgui_upstream STATIC {imgui_sources})\nadd_library(DearImGui::DearImGui ALIAS dear_imgui_upstream)\n{core}\ntarget_link_libraries(scene_editor_core PRIVATE DearImGui::DearImGui)"),
            ("renderer-sentinel-control", False, f"{core}\nadd_library(renderer STATIC render/Renderer.cpp)\ntarget_compile_definitions(renderer PRIVATE NVVK_SEMAPHORE_DEBUG)"),
            ("left-to-right-false-control", False, f"{core}\nif(TRUE OR FALSE AND FALSE)\ntarget_link_libraries(scene_editor_core PRIVATE nvvk)\nendif()"),
        )
        expected_reasons = {
            "direct-link": r"target_link_libraries.*nvvk|nvvk.*target_link_libraries",
            "link-option": r"target_link_options.*Qt6Widgets|Qt6Widgets.*target_link_options",
            "interface-link-options": r"set_property.*Qt6Widgets|Qt6Widgets.*set_property",
            "imported-location": r"set_target_properties.*Qt6Widgets|Qt6Widgets.*set_target_properties",
            "forged-dear-imgui": r"Dear ImGui upstream identity.*allowedUiFoundation",
            "string-concat": r"target_link_libraries.*nvvk|nvvk.*target_link_libraries",
            "string-join-empty": r"target_link_libraries.*nvvk|nvvk.*target_link_libraries",
            "string-replace-empty": r"target_link_libraries.*nvvk|nvvk.*target_link_libraries",
            "list-join-empty": r"target_link_libraries.*nvvk|nvvk.*target_link_libraries",
            "parse-arguments-include": r"forced\.hpp.*nvvk|nvvk.*forced\.hpp",
            "shell-include": r"forced\.hpp.*nvvk|nvvk.*forced\.hpp",
            "msvc-fi": r"forced\.hpp.*nvvk|nvvk.*forced\.hpp",
            "clang-include": r"forced\.hpp.*nvvk|nvvk.*forced\.hpp",
            "xclang-include": r"forced\.hpp.*nvvk|nvvk.*forced\.hpp",
            "compile-property": r"forced\.hpp.*nvvk|nvvk.*forced\.hpp",
            "global-compile-options": r"forced\.hpp.*nvvk|nvvk.*forced\.hpp",
            "string-ascii": r"target_link_libraries.*nvvk|nvvk.*target_link_libraries",
            "list-pop-back": r"target_link_libraries.*nvvk|nvvk.*target_link_libraries",
            "list-pop-front": r"target_link_libraries.*nvvk|nvvk.*target_link_libraries",
            "file-write-include": r"unsupported generated CMake traversal",
            "file-append-include": r"unsupported generated CMake traversal",
            "file-generate-include": r"unsupported generated CMake traversal",
            "file-configure-include": r"unsupported generated CMake traversal",
            "configure-file-include": r"unsupported generated CMake traversal",
            "nested-bare-strequal": r"target_link_libraries.*nvvk|nvvk.*target_link_libraries",
            "public-transitive-reexport": r"target_link_libraries.*nvvk|nvvk.*target_link_libraries",
        }
        counts = {"reject": 0, "accept": 0}
        for name, should_reject, cmake in cases:
            with self.subTest(case=name):
                root = self.make_dependency_project(
                    cmake,
                    {
                        "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                        "render/Renderer.cpp": "int renderer_anchor = 0;\n",
                        "config/forced.hpp": "#include <nvvk/context.hpp>\n",
                        "template.cmake.in": "target_link_libraries(scene_editor_core PRIVATE nvvk)\n",
                        "vendor/Qt6Widgets.lib": "synthetic import\n",
                    },
                )
                try:
                    validate_editor_dependency_boundary(root)
                except ValidationError as exc:
                    if not should_reject:
                        raise
                    self.assertRegex(str(exc), expected_reasons[name])
                    counts["reject"] += 1
                else:
                    if should_reject:
                        self.fail(f"matrix bypass accepted: {name}")
                    counts["accept"] += 1
        self.assertEqual(len(cases), 30)
        self.assertEqual(counts, {"reject": 26, "accept": 4})

    def test_rejects_exact_control_flow_overwrite_exploits_at_dependency_use(self) -> None:
        cases = {
            "while-false": """
                set(BAD nvvk)
                while(FALSE)
                  set(BAD mgif_scene_adapter)
                endwhile()
            """,
            "function-return": """
                set(BAD nvvk)
                function(wire)
                  return()
                  set(BAD mgif_scene_adapter PARENT_SCOPE)
                endfunction()
                wire()
            """,
            "foreach-break": """
                set(BAD nvvk)
                foreach(X IN ITEMS once)
                  break()
                  set(BAD mgif_scene_adapter)
                endforeach()
            """,
            "foreach-continue": """
                set(BAD nvvk)
                foreach(X IN ITEMS once)
                  continue()
                  set(BAD mgif_scene_adapter)
                endforeach()
            """,
        }
        for name, setup in cases.items():
            with self.subTest(case=name):
                root = self.make_dependency_project(
                    f"""
                    {setup}
                    add_library(mgif_scene_adapter INTERFACE)
                    add_library(scene_editor_core STATIC scene/editor/Core.cpp)
                    target_link_libraries(scene_editor_core PRIVATE ${{BAD}})
                    """,
                    {"scene/editor/Core.cpp": "int editor_core_anchor = 0;\n"},
                )
                with self.assertRaisesRegex(
                    ValidationError,
                    r"(?i)target_link_libraries.*nvvk|nvvk.*target_link_libraries",
                ):
                    validate_editor_dependency_boundary(root)

    def test_return_propagate_carries_dependency_state_to_the_calling_scope(self) -> None:
        root = self.make_dependency_project(
            """
            set(BAD mgif_scene_adapter)
            function(wire)
              set(BAD nvvk)
              return(PROPAGATE BAD)
            endfunction()
            wire()
            add_library(mgif_scene_adapter INTERFACE)
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            target_link_libraries(scene_editor_core PRIVATE ${BAD})
            """,
            {"scene/editor/Core.cpp": "int editor_core_anchor = 0;\n"},
        )
        with self.assertRaisesRegex(
            ValidationError,
            r"(?i)target_link_libraries.*nvvk|nvvk.*target_link_libraries",
        ):
            validate_editor_dependency_boundary(root)

    def test_rejects_unsupported_and_macro_return_control_flow(self) -> None:
        cases = {
            "unsupported-argument": "return(UNSUPPORTED)",
            "macro-return": "macro(stop_here)\nreturn()\nendmacro()\nstop_here()",
        }
        for name, control_flow in cases.items():
            with self.subTest(case=name):
                root = self.make_dependency_project(
                    f"""
                    add_library(scene_editor_core STATIC scene/editor/Core.cpp)
                    {control_flow}
                    """,
                    {"scene/editor/Core.cpp": "int editor_core_anchor = 0;\n"},
                )
                with self.assertRaisesRegex(ValidationError, r"(?i)unsupported CMake control flow"):
                    validate_editor_dependency_boundary(root)

    def test_rejects_generated_add_subdirectory_traversal_for_every_producer(self) -> None:
        producers = (
            'file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/CMakeLists.txt" "target_link_libraries(scene_editor_core PRIVATE nvvk)")',
            'file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/generated/CMakeLists.txt" "target_link_libraries(scene_editor_core PRIVATE nvvk)")',
            'file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/CMakeLists.txt" CONTENT "target_link_libraries(scene_editor_core PRIVATE nvvk)")',
            'file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/CMakeLists.txt" CONTENT "target_link_libraries(scene_editor_core PRIVATE nvvk)")',
            'configure_file(template.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/generated/CMakeLists.txt" @ONLY)',
        )
        for producer in producers:
            with self.subTest(producer=producer.split("(", 1)[0]):
                root = self.make_dependency_project(
                    f"""
                    add_library(scene_editor_core STATIC scene/editor/Core.cpp)
                    {producer}
                    add_subdirectory("${{CMAKE_CURRENT_BINARY_DIR}}/generated")
                    """,
                    {
                        "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                        "template.cmake.in": "target_link_libraries(scene_editor_core PRIVATE nvvk)\n",
                    },
                )
                with self.assertRaisesRegex(ValidationError, r"(?i)unsupported generated CMake traversal"):
                    validate_editor_dependency_boundary(root)

    def test_rejects_final_effective_sources_replace_append_and_target_property_forms(self) -> None:
        declarations = (
            "set_property(TARGET scene_editor_core PROPERTY SOURCES render/HiddenBridge.cpp)",
            "set_property(TARGET scene_editor_core APPEND PROPERTY SOURCES render/HiddenBridge.cpp)",
            "set_target_properties(scene_editor_core PROPERTIES SOURCES render/HiddenBridge.cpp)",
        )
        for declaration in declarations:
            with self.subTest(declaration=declaration.split("(", 1)[0]):
                root = self.make_dependency_project(
                    f"""
                    add_library(scene_editor_core STATIC scene/editor/Core.cpp)
                    {declaration}
                    """,
                    {
                        "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                        "render/HiddenBridge.cpp": "#include <nvvk/context.hpp>\n",
                    },
                )
                with self.assertRaisesRegex(ValidationError, r"(?i)HiddenBridge.*nvvk|nvvk.*HiddenBridge"):
                    validate_editor_dependency_boundary(root)

    def test_final_sources_replacement_drops_superseded_source_from_closure(self) -> None:
        replacements = (
            "set_property(TARGET scene_editor_core PROPERTY SOURCES scene/editor/Core.cpp)",
            "set_target_properties(scene_editor_core PROPERTIES SOURCES scene/editor/Core.cpp)",
        )
        for replacement in replacements:
            with self.subTest(api=replacement.split("(", 1)[0]):
                root = self.make_dependency_project(
                    f"""
                    add_library(scene_editor_core STATIC render/Superseded.cpp)
                    {replacement}
                    """,
                    {
                        "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                        "render/Superseded.cpp": "#include <nvvk/context.hpp>\n",
                    },
                )
                validate_editor_dependency_boundary(root)

    def test_rejects_append_string_mutation_of_final_sources(self) -> None:
        root = self.make_dependency_project(
            """
            add_library(scene_editor_core STATIC scene/editor/Core.cpp)
            set_property(TARGET scene_editor_core APPEND_STRING PROPERTY SOURCES render/HiddenBridge.cpp)
            """,
            {
                "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                "render/HiddenBridge.cpp": "#include <nvvk/context.hpp>\n",
            },
        )
        with self.assertRaisesRegex(
            ValidationError,
            r"(?i)unsupported APPEND_STRING mutation.*SOURCES.*set_property",
        ):
            validate_editor_dependency_boundary(root)

    def test_rejects_unknown_property_producers_that_overwrite_dependency_variables(self) -> None:
        producers = (
            "get_cmake_property(BAD VARIABLES)",
            "get_directory_property(BAD DEFINITION SOME_UNKNOWN_PROPERTY)",
            "get_property(BAD GLOBAL PROPERTY SOME_UNKNOWN_PROPERTY)",
            "get_source_file_property(BAD scene/editor/Core.cpp SOME_UNKNOWN_PROPERTY)",
            "get_target_property(BAD scene_editor_core SOME_UNKNOWN_PROPERTY)",
            "get_test_property(synthetic_test SOME_UNKNOWN_PROPERTY BAD)",
        )
        for producer in producers:
            with self.subTest(producer=producer.split("(", 1)[0]):
                root = self.make_dependency_project(
                    f"""
                    set(BAD nvvk)
                    add_library(scene_editor_core STATIC scene/editor/Core.cpp)
                    {producer}
                    target_link_libraries(scene_editor_core PRIVATE ${{BAD}})
                    """,
                    {"scene/editor/Core.cpp": "int editor_core_anchor = 0;\n"},
                )
                with self.assertRaisesRegex(
                    ValidationError,
                    r"(?i)cannot resolve editor dependency declaration.*MGIF_UNSUPPORTED_CMAKE_MUTATION_BAD",
                ):
                    validate_editor_dependency_boundary(root)

    def test_rejects_unknown_property_producers_and_return_propagate_for_global_flags(self) -> None:
        declarations = (
            "get_cmake_property(CMAKE_CXX_FLAGS VARIABLES)",
            "get_directory_property(CMAKE_CXX_FLAGS DEFINITION SOME_UNKNOWN_PROPERTY)",
            "get_property(CMAKE_CXX_FLAGS GLOBAL PROPERTY SOME_UNKNOWN_PROPERTY)",
            "get_source_file_property(CMAKE_CXX_FLAGS scene/editor/Core.cpp SOME_UNKNOWN_PROPERTY)",
            "get_target_property(CMAKE_CXX_FLAGS scene_editor_core SOME_UNKNOWN_PROPERTY)",
            "get_test_property(synthetic_test SOME_UNKNOWN_PROPERTY CMAKE_CXX_FLAGS)",
            "function(wire)\nset(CMAKE_CXX_FLAGS /FIconfig/forced.hpp)\nreturn(PROPAGATE CMAKE_CXX_FLAGS)\nendfunction()\nwire()",
        )
        for declaration in declarations:
            with self.subTest(declaration=declaration.split("(", 1)[0]):
                root = self.make_dependency_project(
                    f"""
                    add_library(scene_editor_core STATIC scene/editor/Core.cpp)
                    {declaration}
                    """,
                    {
                        "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                        "config/forced.hpp": "#include <nvvk/context.hpp>\n",
                    },
                )
                with self.assertRaisesRegex(
                    ValidationError,
                    r"(?i)unsupported computed mutation of global compiler flags.*CMAKE_CXX_FLAGS",
                ):
                    validate_editor_dependency_boundary(root)

    def test_rejects_include_search_options_that_reveal_transitive_forbidden_headers(self) -> None:
        options = (
            '"SHELL:-cxx-isystem ${CMAKE_CURRENT_SOURCE_DIR}/config/vendor"',
            '"SHELL:-idirafter ${CMAKE_CURRENT_SOURCE_DIR}/config/vendor"',
            '"SHELL:-imsvc ${CMAKE_CURRENT_SOURCE_DIR}/config/vendor"',
            '"SHELL:-internal-isystem ${CMAKE_CURRENT_SOURCE_DIR}/config/vendor"',
            '"SHELL:-iquote ${CMAKE_CURRENT_SOURCE_DIR}/config/vendor"',
            '"SHELL:-external:I ${CMAKE_CURRENT_SOURCE_DIR}/config/vendor"',
            '"SHELL:/external:I ${CMAKE_CURRENT_SOURCE_DIR}/config/vendor"',
            '"SHELL:-I ${CMAKE_CURRENT_SOURCE_DIR}/config/vendor"',
            '"SHELL:/I ${CMAKE_CURRENT_SOURCE_DIR}/config/vendor"',
            '"SHELL:/imsvc ${CMAKE_CURRENT_SOURCE_DIR}/config/vendor"',
            '"SHELL:-isystem ${CMAKE_CURRENT_SOURCE_DIR}/config/vendor"',
        )
        for option in options:
            with self.subTest(option=option):
                root = self.make_dependency_project(
                    f"""
                    add_library(scene_editor_core STATIC scene/editor/Core.cpp)
                    target_compile_options(scene_editor_core PRIVATE {option})
                    """,
                    {
                        "scene/editor/Core.cpp": "#include <Bridge.hpp>\nint editor_core_anchor = 0;\n",
                        "config/vendor/Bridge.hpp": "#include <nvvk/context.hpp>\n",
                        "config/vendor/nvvk/context.hpp": "#pragma once\n",
                    },
                )
                with self.assertRaisesRegex(ValidationError, r"(?i)Bridge.*nvvk|nvvk.*Bridge"):
                    validate_editor_dependency_boundary(root)

    def test_rejects_unmodeled_include_search_root_modifiers(self) -> None:
        options = (
            '"SHELL:--sysroot=${CMAKE_CURRENT_SOURCE_DIR}/sysroot"',
            '"SHELL:-iframework ${CMAKE_CURRENT_SOURCE_DIR}/frameworks"',
            '"SHELL:-iprefix ${CMAKE_CURRENT_SOURCE_DIR}/prefix"',
            '"SHELL:-isysroot ${CMAKE_CURRENT_SOURCE_DIR}/sysroot"',
            '"SHELL:-resource-dir ${CMAKE_CURRENT_SOURCE_DIR}/resource"',
            '"SHELL:/winsysroot ${CMAKE_CURRENT_SOURCE_DIR}/sysroot"',
        )
        for option in options:
            with self.subTest(option=option):
                root = self.make_dependency_project(
                    f"""
                    add_library(scene_editor_core STATIC scene/editor/Core.cpp)
                    target_compile_options(scene_editor_core PRIVATE {option})
                    """,
                    {"scene/editor/Core.cpp": "int editor_core_anchor = 0;\n"},
                )
                with self.assertRaisesRegex(
                    ValidationError,
                    r"(?i)unsupported production editor include-search option",
                ):
                    validate_editor_dependency_boundary(root)

    def test_rejects_cache_and_environment_global_forced_include_mutations(self) -> None:
        declarations = (
            'set_property(CACHE CMAKE_CXX_FLAGS PROPERTY VALUE "/FI${CMAKE_CURRENT_SOURCE_DIR}/config/forced.hpp")',
            'set(ENV{CXXFLAGS} "-include ${CMAKE_CURRENT_SOURCE_DIR}/config/forced.hpp")',
            'function(wire)\nset_property(CACHE CMAKE_CXX_FLAGS PROPERTY VALUE "/FI${CMAKE_CURRENT_SOURCE_DIR}/config/forced.hpp")\nendfunction()\nwire()',
            'function(wire)\nset(ENV{CL} "/FI${CMAKE_CURRENT_SOURCE_DIR}/config/forced.hpp")\nendfunction()\nwire()',
        )
        for declaration in declarations:
            with self.subTest(declaration=declaration.split("(", 1)[0]):
                root = self.make_dependency_project(
                    f"""
                    {declaration}
                    add_library(scene_editor_core STATIC scene/editor/Core.cpp)
                    """,
                    {
                        "scene/editor/Core.cpp": "int editor_core_anchor = 0;\n",
                        "config/forced.hpp": "#include <nvvk/context.hpp>\n",
                    },
                )
                with self.assertRaisesRegex(ValidationError, r"(?i)forced.*nvvk|nvvk.*forced"):
                    validate_editor_dependency_boundary(root)

    def test_rejects_d20_trace_downgraded_below_native_partial(self) -> None:
        manifest = self.load_manifest()
        next(
            trace for trace in manifest["oracleTraces"] if trace["id"] == "O09-multi-scene-structural-matrix"
        )["status"] = "pending_executable"
        self.write_manifest(manifest)
        with self.assertRaisesRegex(ValidationError, r"O09.*status.*evidence ceiling"):
            validate_manifest(self.root)

    def test_rejects_false_p0_pass_with_pending_evidence(self) -> None:
        manifest = self.load_manifest()
        manifest["status"] = "p0_passed"
        self.write_manifest(manifest)
        with self.assertRaisesRegex(ValidationError, r"legacy manifest.status=p0_passed"):
            validate_manifest(self.root)

    def test_gate_mode_rejects_in_progress_scaffold(self) -> None:
        manifest = self.load_manifest()
        manifest["status"] = "p0_in_progress"
        d20 = manifest["decisionGates"]["D20"]
        d20.update(
            {
                "status": "pending_user_decision",
                "selectedResolution": None,
                "blocks": [f"P{index}" for index in range(1, 11)],
                "approvedBy": None,
                "approvedAt": None,
            }
        )
        for field in ("approvalEvidenceText", "approvalTimestampProvenance", "selectedPolicyContract"):
            d20.pop(field, None)
        next(item for item in manifest["features"] if item["id"] == "F02")["referenceEvidence"][
            "status"
        ] = "blocked"
        self.write_manifest(manifest)
        with self.assertRaisesRegex(ValidationError, "resolved D20"):
            with patch("validate_editor_p0_artifacts.validate_reference_trace_artifacts"):
                validate_manifest(self.root, mode="gate")

    def test_p1_readiness_gate_accepts_hash_bound_partial_and_disposed_conflict(self) -> None:
        manifest = self.make_p1_ready_manifest()
        self.write_manifest(manifest)
        with patch("validate_editor_p0_artifacts.validate_reference_trace_artifacts"), patch(
            "validate_editor_p0_artifacts.verify_mgif_baseline_external"
        ), patch(
            "validate_editor_p0_artifacts.validate_resolved_dear_imgui_identity"
        ), patch(
            "validate_editor_p0_artifacts.validate_f39_repair"
        ):
            validate_manifest(self.root, mode="gate")

    def test_p1_readiness_gate_requires_explicit_build_local_identity(self) -> None:
        manifest = self.make_p1_ready_manifest()
        self.write_manifest(manifest)
        with patch("validate_editor_p0_artifacts.validate_reference_trace_artifacts"), patch(
            "validate_editor_p0_artifacts.verify_mgif_baseline_external"
        ), patch(
            "validate_editor_p0_artifacts.validate_f39_repair"
        ), self.assertRaisesRegex(ValidationError, r"(?i)--resolved-build-identity"):
            validate_manifest(self.root, mode="gate")

    def test_resolved_build_identity_recomputes_checkout_and_source_hashes(self) -> None:
        with tempfile.TemporaryDirectory() as build_directory:
            identity_path = self.make_resolved_identity_fixture(Path(build_directory))

            def git_result(_source_root: Path, *arguments: str) -> str:
                if arguments == ("rev-parse", "HEAD^{commit}"):
                    return "fd13a1e8923a0a7077b404fc36fd063b25a0c0b5"
                if arguments == ("rev-parse", "HEAD^{tree}"):
                    return "0c8986ccb662ea838c04f7bed4cc56d3379132ef"
                if arguments == ("remote", "get-url", "origin"):
                    return "https://github.com/ocornut/imgui.git"
                if arguments == ("status", "--porcelain=v1", "--untracked-files=all"):
                    return ""
                self.fail(f"unexpected identity Git query: {arguments}")

            with patch("validate_editor_p0_artifacts._identity_git", side_effect=git_result), patch(
                "validate_editor_p0_artifacts.validate_configured_editor_build_identity"
            ):
                validate_resolved_dear_imgui_identity(self.root, identity_path)

                identity = json.loads(identity_path.read_text(encoding="utf-8"))
                mutations = {
                    "override": lambda value: value["sourceOverride"].update({"cacheOrCli": True}),
                    "tree": lambda value: value.update({"resolvedGitTree": "0" * 40}),
                    "source-list": lambda value: value["targetSourceRelativePaths"].pop(),
                    "compiled-list": lambda value: value["compiledSourceRelativePaths"].pop(),
                    "source-hash": lambda value: value["targetSourceSha256"].update({"imgui.cpp": "0" * 64}),
                    "project-binding": lambda value: value.update({"projectCMakeSha256": "0" * 64}),
                }
                for name, mutate in mutations.items():
                    with self.subTest(mutation=name):
                        changed = copy.deepcopy(identity)
                        mutate(changed)
                        identity_path.write_text(json.dumps(changed, indent=2) + "\n", encoding="utf-8")
                        with self.assertRaises(ValidationError):
                            validate_resolved_dear_imgui_identity(self.root, identity_path)
                identity_path.write_text(json.dumps(identity, indent=2) + "\n", encoding="utf-8")
                source = Path(identity["resolvedSourceRoot"]) / "imgui.cpp"
                source.write_text("// spoofed working tree\n", encoding="utf-8")
                with self.assertRaisesRegex(ValidationError, r"(?i)source hash changed"):
                    validate_resolved_dear_imgui_identity(self.root, identity_path)

    def test_identity_generator_rejects_override_and_noncanonical_final_sources(self) -> None:
        with tempfile.TemporaryDirectory() as build_directory:
            build_root = Path(build_directory)
            identity_path = self.make_resolved_identity_fixture(build_root)
            identity = json.loads(identity_path.read_text(encoding="utf-8"))
            target_sources = [
                Path(identity["resolvedSourceRoot"]).joinpath(*PurePosixPath(relative).parts)
                for relative in identity["targetSourceRelativePaths"]
            ]
            arguments = SimpleNamespace(
                repo_root=self.root,
                build_root=build_root,
                source_dir=Path(identity["resolvedSourceRoot"]),
                expected_source_dir=Path(identity["resolvedSourceRoot"]),
                config="Debug",
                output=identity_path,
                cache_override=False,
                environment_override=False,
            )
            compiled_sources = [source for source in target_sources if source.suffix != ".h"]
            evidence_files = {
                "indexPath": build_root / ".cmake/api/v1/reply/index.json",
                "codemodel": build_root / ".cmake/api/v1/reply/codemodel.json",
                "cache": build_root / ".cmake/api/v1/reply/cache.json",
                "toolchains": build_root / ".cmake/api/v1/reply/toolchains.json",
                "target": build_root / ".cmake/api/v1/reply/imgui.json",
                "compile": build_root / "compile_commands.json",
                "build": build_root / "build.ninja",
            }
            for path in evidence_files.values():
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("{}\n", encoding="utf-8")
            compile_entries = [
                {
                    "directory": str(build_root),
                    "file": str(source),
                    "output": str(build_root / f"CMakeFiles/imgui_demo.dir/{source.name}.obj"),
                    "command": f"cl.exe /c {source}",
                }
                for source in compiled_sources
            ]
            state = {
                "indexPath": evidence_files["indexPath"],
                "objects": {
                    "codemodel": evidence_files["codemodel"],
                    "cache": evidence_files["cache"],
                    "toolchains": evidence_files["toolchains"],
                },
                "configuration": "Debug",
                "targetPaths": {"imgui_demo": evidence_files["target"]},
                "imguiSources": target_sources,
                "imguiCompiledSources": compiled_sources,
                "compileCommandsPath": evidence_files["compile"],
                "compileEntries": {"imgui_demo": compile_entries},
                "buildSystemPath": evidence_files["build"],
                "cmakeGenerator": "Ninja",
            }

            def git_result(_source_root: Path, *git_arguments: str) -> str:
                values = {
                    ("rev-parse", "HEAD^{commit}"): "fd13a1e8923a0a7077b404fc36fd063b25a0c0b5",
                    ("rev-parse", "HEAD^{tree}"): "0c8986ccb662ea838c04f7bed4cc56d3379132ef",
                    ("remote", "get-url", "origin"): "https://github.com/ocornut/imgui.git",
                    ("status", "--porcelain=v1", "--untracked-files=all"): "",
                }
                return values[git_arguments]

            with patch("generate_editor_dependency_identity.run_git", side_effect=git_result), patch(
                "generate_editor_dependency_identity.__file__",
                str(self.root / "tools/generate_editor_dependency_identity.py"),
            ), patch(
                "generate_editor_dependency_identity.inspect_configured_editor_build",
                return_value=state,
            ):
                generated = generate_identity(arguments)
                self.assertEqual(generated["resolvedGitTree"], "0c8986ccb662ea838c04f7bed4cc56d3379132ef")
                self.assertEqual(generated["compiledSourceRelativePaths"], identity["compiledSourceRelativePaths"])

                for override_name in ("cache_override", "environment_override"):
                    with self.subTest(override=override_name):
                        setattr(arguments, override_name, True)
                        with self.assertRaisesRegex(IdentityError, r"(?i)override.*forbidden"):
                            generate_identity(arguments)
                        setattr(arguments, override_name, False)

                evil = Path(identity["resolvedSourceRoot"]) / "evil.cpp"
                evil.write_text("int spoofed_imgui = 0;\n", encoding="utf-8")
                state["imguiSources"] = [evil]
                state["imguiCompiledSources"] = [evil]
                with self.assertRaisesRegex(IdentityError, r"(?i)final .*imgui_demo SOURCES"):
                    generate_identity(arguments)

    def test_release_license_rejects_empty_whitespace_truncated_and_wrong_content(self) -> None:
        baseline = self.make_release_ready_manifest()
        self.write_manifest(baseline)
        invalid_contents = {
            "empty": "",
            "whitespace": " \r\n\t",
            "truncated": TEST_LICENSE_TEXT[:120],
            "wrong": TEST_LICENSE_TEXT.replace("Synthetic Fixture Authors", "Spoofed Authors"),
        }
        for name, contents in invalid_contents.items():
            with self.subTest(case=name):
                (self.root / "LICENSE").write_text(contents, encoding="utf-8")
                with patch("validate_editor_p0_artifacts.validate_reference_trace_artifacts"), patch(
                    "validate_editor_p0_artifacts.verify_mgif_baseline_external"
                ), patch(
                    "validate_editor_p0_artifacts.validate_resolved_dear_imgui_identity"
                ), patch(
                    "validate_editor_p0_artifacts.validate_f39_repair"
                ), self.assertRaisesRegex(ValidationError, r"(?i)license.*(?:empty|truncated|digest|invalid)"):
                    validate_manifest(self.root, mode="release")

    def test_release_mode_rejects_current_pending_owner_license_without_a_pass_claim(self) -> None:
        self.write_manifest(self.make_p1_ready_manifest())
        with patch("validate_editor_p0_artifacts.validate_reference_trace_artifacts"), patch(
            "validate_editor_p0_artifacts.validate_resolved_dear_imgui_identity"
        ), patch(
            "validate_editor_p0_artifacts.validate_f39_repair"
        ), self.assertRaisesRegex(
            ValidationError,
            r"(?i)release readiness blocked: owner root-license selection is pending",
        ):
            validate_manifest(self.root, mode="release")

    def test_engineering_readiness_rejects_every_rights_reserved_policy_upgrade(self) -> None:
        baseline = self.make_p1_ready_manifest()
        mutations = {
            "selected without owner license": (
                lambda value: value["licensePolicy"].update({"status": "selected"}),
                r"(?i)engineering readiness.*rights-reserved development policy",
            ),
            "licensed distribution": (
                lambda value: value["licensePolicy"]["developmentPolicy"].update(
                    {"distributionStatus": "licensed"}
                ),
                r"(?i)engineering readiness.*rights-reserved development policy",
            ),
            "upstream source copied": (
                lambda value: value["licensePolicy"]["developmentPolicy"].update(
                    {"upstreamApacheSourceCopied": True}
                ),
                r"(?i)licensePolicy\.developmentPolicy\.upstreamApacheSourceCopied.*constant False",
            ),
            "release pass claim": (
                lambda value: value["licensePolicy"]["releaseGate"].update(
                    {"passClaimAllowed": True}
                ),
                r"(?i)engineering readiness.*rights-reserved development policy",
            ),
        }
        for name, (mutate, message_pattern) in mutations.items():
            with self.subTest(mutation=name):
                manifest = copy.deepcopy(baseline)
                mutate(manifest)
                self.write_manifest(manifest)
                with self.assertRaisesRegex(ValidationError, message_pattern):
                    validate_manifest(self.root)

    def test_resolved_d20_requires_exact_owner_attestation_and_timestamp_provenance(self) -> None:
        baseline = self.make_p1_ready_manifest()
        mutations = (
            ("approvedBy", "different owner", r"(?i)approval identity changed"),
            ("approvedAt", "2026-08-22T16:55:25Z", r"(?i)attestation timestamp changed"),
            ("approvalEvidenceText", "approved", r"(?i)approval evidence text changed"),
            (
                "timestamp provenance",
                "changed provenance note",
                r"(?i)distinguish unavailable user-message time.*durable coordinator attestation",
            ),
        )
        for field, replacement, message_pattern in mutations:
            with self.subTest(field=field):
                manifest = copy.deepcopy(baseline)
                d20 = manifest["decisionGates"]["D20"]
                if field == "timestamp provenance":
                    d20["approvalTimestampProvenance"]["note"] = replacement
                else:
                    d20[field] = replacement
                self.write_manifest(manifest)
                with self.assertRaisesRegex(ValidationError, message_pattern):
                    validate_manifest(self.root)

    def test_p1_gate_requires_the_exact_o06_conflict_disposition(self) -> None:
        baseline = self.make_p1_ready_manifest()
        o06 = next(
            item for item in baseline["oracleTraces"]
            if item["id"] == "O06-gizmo-escape-release-same-frame"
        )
        self.assertEqual(o06["conflictDisposition"], TEST_O06_CONFLICT_DISPOSITION)
        mutations = {
            "missing": lambda value: value.pop("conflictDisposition"),
            "wrong target": lambda value: value["conflictDisposition"].update(
                {"targetBehavior": "invented"}
            ),
            "wrong authority": lambda value: value["conflictDisposition"].update(
                {"disposition": "target_behavior_authoritative"}
            ),
        }
        for name, mutate in mutations.items():
            with self.subTest(mutation=name):
                manifest = copy.deepcopy(baseline)
                trace = next(
                    item for item in manifest["oracleTraces"]
                    if item["id"] == "O06-gizmo-escape-release-same-frame"
                )
                mutate(trace)
                self.write_manifest(manifest)
                with patch("validate_editor_p0_artifacts.validate_reference_trace_artifacts"), patch(
                    "validate_editor_p0_artifacts.validate_resolved_dear_imgui_identity"
                ), self.assertRaisesRegex(ValidationError, r"(?i)O06.*conflict.*disposition"):
                    validate_manifest(self.root, mode="gate")

    def test_p1_readiness_gate_rejects_every_contract_degradation(self) -> None:
        baseline = self.make_p1_ready_manifest()
        o09_id = "O09-multi-scene-structural-matrix"
        o04_id = "O04-unique-root-delete"

        def trace(manifest: dict[str, object], trace_id: str) -> dict[str, object]:
            return next(item for item in manifest["oracleTraces"] if item["id"] == trace_id)

        mutations = {
            "blank approval identity": lambda value: value["decisionGates"]["D20"].update({"approvedBy": " "}),
            "timezone-free approval": lambda value: value["decisionGates"]["D20"].update(
                {"approvedAt": "2026-08-22T12:00:00"}
            ),
            "missing selected-policy contract": lambda value: value["decisionGates"]["D20"].pop(
                "selectedPolicyContract"
            ),
            "policy resolution mismatch": lambda value: value["decisionGates"]["D20"][
                "selectedPolicyContract"
            ].update({"resolution": "exact_defect_parity"}),
            "incomplete policy routing": lambda value: value["decisionGates"]["D20"][
                "selectedPolicyContract"
            ].update({"sceneRouting": "reference_current_scene_state"}),
            "incomplete policy snapshot": lambda value: value["decisionGates"]["D20"][
                "selectedPolicyContract"
            ].update({"snapshotScope": "reference_current_scene_only"}),
            "incomplete policy shared nodes": lambda value: value["decisionGates"]["D20"][
                "selectedPolicyContract"
            ].update({"sharedNodeHandling": "reference_observed"}),
            "incomplete policy invalid graph": lambda value: value["decisionGates"]["D20"][
                "selectedPolicyContract"
            ].update({"preflightRejections": ["cycle", "multiparent"]}),
            "missing policy acceptance": lambda value: value["decisionGates"]["D20"][
                "selectedPolicyContract"
            ].update({"acceptanceIds": []}),
            "pending O09": lambda value: trace(value, o09_id).update({"status": "pending_executable"}),
            "promoted O09": lambda value: trace(value, o09_id).update({"status": "captured"}),
            "missing O09 readiness": lambda value: trace(value, o09_id).pop("p1Readiness"),
            "O09 incomplete cells": lambda value: trace(value, o09_id)["p1Readiness"]["coverageBinding"].update(
                {"executedCellCount": 297, "notExecutedCellCount": 1}
            ),
            "O09 inflated domain": lambda value: trace(value, o09_id)["p1Readiness"]["coverageBinding"].update(
                {"matrixCellCount": 299}
            ),
            "O09 unbound artifact hash": lambda value: trace(value, o09_id)["p1Readiness"]["coverageBinding"].update(
                {"artifactSha256": "0" * 64}
            ),
            "O09 unbound case IDs": lambda value: trace(value, o09_id)["p1Readiness"]["coverageBinding"].update(
                {"caseIdSetSha256": "0" * 64}
            ),
            "O09 UI ceiling inflation": lambda value: trace(value, o09_id)["p1Readiness"]["coverageCeiling"].update(
                {"fullStateUiReplays": 1}
            ),
            "O09 domain relabel": lambda value: trace(value, o09_id)["p1Readiness"]["coverageCeiling"].update(
                {"domainMatrixRelabeledAsUiReplays": True}
            ),
            "O09 private-state inflation": lambda value: trace(value, o09_id)["p1Readiness"]["coverageCeiling"].update(
                {"privateStateReadbacks": 1}
            ),
            "O09 failure erasure": lambda value: trace(value, o09_id)["p1Readiness"]["coverageCeiling"].update(
                {"referenceFailuresPreserved": False}
            ),
            "O09 native UI count inflation": lambda value: trace(value, o09_id)["p1Readiness"]["nativeUiBinding"].update(
                {"actionPathCells": 299}
            ),
            "O09 missing UI debt": lambda value: trace(value, o09_id)["p1Readiness"]["evidenceDebt"].update(
                {"uiIds": []}
            ),
            "O09 missing Vulkan debt": lambda value: trace(value, o09_id)["p1Readiness"]["evidenceDebt"].update(
                {"vulkanIds": []}
            ),
            "O09 missing P10 debt": lambda value: trace(value, o09_id)["p1Readiness"]["evidenceDebt"].update(
                {"p10Ids": []}
            ),
            "missing O04 disposition": lambda value: trace(value, o04_id).pop("conflictDisposition"),
            "O04 observed mismatch": lambda value: trace(value, o04_id)["conflictDisposition"].update(
                {"observedBehavior": "vague"}
            ),
            "O04 target mismatch": lambda value: trace(value, o04_id)["conflictDisposition"].update(
                {"targetBehavior": "vague"}
            ),
            "O04 undisposed conflict": lambda value: trace(value, o04_id)["conflictDisposition"].update(
                {"status": "pending"}
            ),
            "O04 wrong disposition": lambda value: trace(value, o04_id)["conflictDisposition"].update(
                {"disposition": "observed_behavior_authoritative"}
            ),
            "missing provisional debt": lambda value: trace(value, "O05-gizmo-escape-only").pop(
                "readinessDebt"
            ),
            "blocked reference evidence": lambda value: next(
                item for item in value["features"] if item["id"] == "F02"
            )["referenceEvidence"].update({"status": "blocked"}),
            "false F42 engineering pass": lambda value: next(
                item for item in value["features"] if item["id"] == "F42"
            )["mgifEvidence"].update({"status": "pass"}),
            "legacy pass status": lambda value: value.update({"status": "p0_passed"}),
            "missing build identity": lambda value: value.pop("upstreamBuildIdentityContract"),
            "forged build identity commit": lambda value: value["upstreamBuildIdentityContract"]["dearImGui"].update(
                {"pinnedCommit": "0" * 40}
            ),
            "waiver surface": lambda value: trace(value, o09_id)["p1Readiness"].update({"waiver": True}),
        }
        for name, mutate in mutations.items():
            with self.subTest(mutation=name):
                manifest = copy.deepcopy(baseline)
                mutate(manifest)
                self.write_manifest(manifest)
                with patch("validate_editor_p0_artifacts.validate_reference_trace_artifacts"), patch(
                    "validate_editor_p0_artifacts.verify_mgif_baseline_external"
                ), patch(
                    "validate_editor_p0_artifacts.validate_resolved_dear_imgui_identity"
                ), patch(
                    "validate_editor_p0_artifacts.validate_f39_repair"
                ), self.assertRaises(ValidationError):
                    validate_manifest(self.root, mode="gate")

    def test_rejects_required_oracles_disguised_as_not_applicable(self) -> None:
        baseline = self.load_manifest()
        for oracle in baseline["oracleTraces"]:
            oracle_id = oracle["id"]
            short_id = oracle_id.split("-", 1)[0]
            with self.subTest(oracle=short_id):
                manifest = copy.deepcopy(baseline)
                trace = next(item for item in manifest["oracleTraces"] if item["id"] == oracle_id)
                trace["status"] = "not_applicable"
                trace["artifacts"] = []
                self.write_manifest(manifest)
                with self.assertRaisesRegex(
                    ValidationError,
                    rf"(?i)(?:{short_id}.*not_applicable|not_applicable.*{short_id})",
                ):
                    validate_manifest(self.root)

    def test_rejects_oracle_artifact_pointing_to_arbitrary_existing_file(self) -> None:
        manifest = self.load_manifest()
        trace = manifest["oracleTraces"][0]
        trace["status"] = "captured"
        trace["artifacts"] = ["docs/editor-parity/LICENSE-SOURCES.md"]
        self.write_manifest(manifest)
        with self.assertRaisesRegex(ValidationError, r"(?i)(?:O01.*artifact|artifact.*O01)"):
            validate_manifest(self.root)

    def test_native_evidence_mirrors_do_not_upgrade_oracle_traces_beyond_their_ceiling(self) -> None:
        baseline = self.load_manifest()
        self.assertEqual(baseline["decisionGates"]["D20"]["status"], "resolved")
        self.assertEqual(baseline["decisionGates"]["D20"]["selectedResolution"], "mgif_safety_exception")
        for oracle in baseline["oracleTraces"]:
            short_id = oracle["id"].split("-", 1)[0]
            if short_id not in {"O04", "O05", "O06", "O07", "O08", "O09", "O10"}:
                continue
            with self.subTest(oracle=short_id, mutation="status"):
                manifest = copy.deepcopy(baseline)
                trace = next(item for item in manifest["oracleTraces"] if item["id"] == oracle["id"])
                trace["status"] = "captured"
                self.write_manifest(manifest)
                with self.assertRaisesRegex(ValidationError, rf"(?i){short_id}.*status.*evidence ceiling"):
                    validate_manifest(self.root)
            with self.subTest(oracle=short_id, mutation="artifacts"):
                manifest = copy.deepcopy(baseline)
                trace = next(item for item in manifest["oracleTraces"] if item["id"] == oracle["id"])
                trace["artifacts"].extend(
                    [
                        "docs/editor-parity/reference-traces/UI-RUNTIME-REPORT.md",
                        "docs/editor-parity/reference-traces/ui-runtime-index.json",
                    ]
                )
                self.write_manifest(manifest)
                with self.assertRaisesRegex(ValidationError, rf"(?i){short_id}.*canonical reference artifacts"):
                    validate_manifest(self.root)

    def test_rejects_pending_user_decision_with_selected_resolution(self) -> None:
        manifest = self.load_manifest()
        manifest["decisionGates"]["D20"].update(
            {
                "status": "pending_user_decision",
                "selectedResolution": "exact_defect_parity",
                "blocks": [f"P{index}" for index in range(1, 11)],
            }
        )
        self.write_manifest(manifest)
        with self.assertRaisesRegex(
            ValidationError,
            r"(?i)(?:pending_user_decision|unresolved D20).*select",
        ):
            validate_manifest(self.root)

    def test_rejects_f39_baseline_failure_rewritten_as_pass(self) -> None:
        manifest = self.load_manifest()
        feature = next(item for item in manifest["features"] if item["id"] == "F39")
        feature["mgifEvidence"]["status"] = "pass"
        self.write_manifest(manifest)
        with self.assertRaisesRegex(ValidationError, r"(?i)F39.*(?:provisional|full feature|baseline|waiv)"):
            validate_manifest(self.root)

    def test_rejects_f39_baseline_failure_rewritten_as_waived(self) -> None:
        manifest = self.load_manifest()
        feature = next(item for item in manifest["features"] if item["id"] == "F39")
        feature["mgifEvidence"]["status"] = "waived"
        self.write_manifest(manifest)
        with self.assertRaisesRegex(
            ValidationError,
            r"(?i)(?:mgifEvidence\.status.*enum|F39.*waiv)",
        ):
            validate_manifest(self.root)

    def test_rejects_f42_blocked_license_policy_disguised_as_pass(self) -> None:
        manifest = self.load_manifest()
        feature = next(item for item in manifest["features"] if item["id"] == "F42")
        feature["mgifEvidence"]["status"] = "pass"
        self.write_manifest(manifest)
        with self.assertRaisesRegex(ValidationError, r"(?i)F42.*(?:blocked|license)"):
            validate_manifest(self.root)

    def test_current_reference_trace_artifacts_are_valid(self) -> None:
        validate_reference_trace_artifacts(self.root)

    def test_current_ui_semantic_and_o09_real_ui_mirrors_are_valid(self) -> None:
        validate_ui_semantic_index_data(self.load_reference_trace("ui-semantic-index.json"))
        validate_o09_real_ui_index_data(self.load_reference_trace("o09-real-ui-index.json"))

    def test_rejects_o09_real_ui_coverage_or_decision_upgrades(self) -> None:
        baseline = self.load_reference_trace("o09-real-ui-index.json")
        mutations = (
            (("coverage", "registryCells"), 299, r"(?i)exact coverage ceiling changed"),
            (("coverageCeiling", "status"), "captured", r"(?i)must remain native_partial"),
            (("coverageCeiling", "allCellsAreFullStateUiReplays"), True, r"(?i)cannot claim 298 full UI state replays"),
            (("coverageCeiling", "domainMatrixRelabeledAsUiReplays"), True, r"(?i)relabeled the domain matrix"),
            (("decisionBoundary", "policySelectedByEvidenceRun"), True, r"(?i)evidence run cannot select D20"),
        )
        for path, replacement, message_pattern in mutations:
            with self.subTest(field=".".join(path)):
                self.assert_data_mutation_rejected(
                    validate_o09_real_ui_index_data,
                    baseline,
                    path,
                    replacement,
                    message_pattern,
                )

    def test_rejects_external_o09_cell_bindings_that_differ_from_the_domain_registry(self) -> None:
        evidence_root = self.root / "synthetic-o09-external"
        evidence_root.mkdir()
        mirror = self.load_reference_trace("o09-real-ui-index.json")
        d20 = self.load_reference_trace("d20-hierarchy-index.json")
        domain = d20["o09Coverage"]["cells"][0]
        envelope = domain["sourceEnvelope"]
        native_cell = {
            "ordinal": 0,
            "caseId": domain["caseId"],
            "requirement": domain["requirement"],
            "dimensions": copy.deepcopy(domain["dimensions"]),
            "registryBinding": {
                "expectedOutcome": domain["outcome"],
                "expectedExitHex": domain["exit"]["processExitCodeHex"],
                "rawTraceSha256": domain["rawTraceSha256"],
                "evidenceKind": envelope["evidenceKind"],
                "source": envelope["source"],
                "uiBranch": envelope["uiBranch"],
            },
        }
        coverage = {
            "registryCellsRequested": 298,
            "realUiActionCellsCaptured": 298,
            "exitIdentityCapturedCells": 298,
            "unexecutedLogicalCaseCount": 0,
            "cleanExitCells": 290,
            "expectedStackOverflowCells": 2,
            "accessViolationExitCells": 6,
            "fullHistoryUndoRedoVisibleCells": 290,
            "exactApplicationSerializedTopologyCells": 7,
            "exactApplicationSerializedCheckpointCount": 20,
            "privateStateReadbackCells": 0,
            "visibleOnlyCurrentSceneCells": 298,
            "visibleOnlySelectionCells": 294,
        }
        external = {
            "reference": {
                "commit": "50373c7b0ff58078727af2c2aac70e29d5e74752",
                "tree": "03e44de44217b109ab71dfbf6aee3989593d76b0",
                "executableSha256": "a02572b63f4af50bbaa26cf1909e1938bf7d72a7ae28904766d7e83394fd326b",
            },
            "coverage": coverage,
            "cells": [native_cell, *({} for _ in range(297))],
        }
        final_seal = {
            "schema": "vkgr-o09-final-seal/v1",
            "integrityPassed": True,
            "coverage": coverage,
            "verdict": {"o09CanTruthfullyBecomeCaptured": False, "recommendedStatus": "partial"},
        }
        integrity = {
            "schema": "vkgr-o09-integrity-reverification/v1",
            "passed": True,
            "allProducerProcessesExited": True,
            "producerProcessCount": 0,
            "artifactIndex": {"payloadEntries": 8657, "verificationFailureCount": 0},
            "matrix": {"requested": 298, "unexecuted": 0},
            "mgif": {"productionDependencyImplication": "none"},
        }
        pointer = copy.deepcopy(mirror)
        pointer["sealedEvidenceRoot"] = str(evidence_root)
        pointer["externalArtifacts"] = {
            role: {"path": f"{role}.json"}
            for role in (
                "report", "index", "artifactIndex", "integrityReverification", "finalSeal",
                "detachedSha256Manifest",
            )
        }
        loaded = {
            "index.json": external,
            "finalSeal.json": final_seal,
            "integrityReverification.json": integrity,
        }

        mutations = (
            (("requirement",), "invented", r"(?i)requirement differs from registry"),
            (("dimensions", "action"), "invented", r"(?i)dimensions differ from registry"),
            (("registryBinding", "expectedOutcome"), "invented", r"(?i)outcome differs from registry"),
            (("registryBinding", "expectedExitHex"), "0xDEADBEEF", r"(?i)expected exit differs from registry"),
            (("registryBinding", "rawTraceSha256"), "0" * 64, r"(?i)raw trace hash differs from registry"),
            (("registryBinding", "evidenceKind"), "captured_ui_runtime", r"(?i)source envelope evidenceKind differs"),
            (("registryBinding", "source"), "src/spoof.cpp:1", r"(?i)source envelope source differs"),
            (("registryBinding", "uiBranch"), "invented", r"(?i)source envelope uiBranch differs"),
        )
        for path, replacement, message_pattern in mutations:
            with self.subTest(field=".".join(path)):
                changed = copy.deepcopy(external)
                target = changed["cells"][0]
                for component in path[:-1]:
                    target = target[component]
                target[path[-1]] = replacement
                loaded["index.json"] = changed
                with patch("validate_editor_p0_artifacts.verify_external_artifact"), patch(
                    "validate_editor_p0_artifacts._verify_detached_sha_manifest_inventory"
                ), patch(
                    "validate_editor_p0_artifacts.load_json",
                    side_effect=lambda path: copy.deepcopy(loaded[Path(path).name]),
                ), self.assertRaisesRegex(ValidationError, message_pattern):
                    verify_o09_real_ui_external(pointer, d20)

    def test_o09_selected_attempt_classification_preserves_clean_and_failure_exits(self) -> None:
        attempts = {
            "clean": {
                "observedExitHex": "0x00000000",
                "executionComplete": True,
                "expectedExitMatch": True,
                "grade": "complete-clean",
            },
            "stack overflow": {
                "observedExitHex": "0xC00000FD",
                "executionComplete": True,
                "expectedExitMatch": True,
                "grade": "complete-expected-stack-overflow",
            },
            "access violation": {
                "observedExitHex": "0xC0000005",
                "executionComplete": False,
                "expectedExitMatch": False,
                "grade": "action-captured-unexpected-exit",
            },
        }
        for name, attempt in attempts.items():
            with self.subTest(classification=name):
                self.assertEqual(
                    _validate_o09_selected_attempt_outcome(attempt, f"synthetic-{name}"),
                    attempt["observedExitHex"].upper(),
                )

        mutations = (
            ("clean", "executionComplete", False, r"(?i)clean selected attempt"),
            ("stack overflow", "grade", "complete-clean", r"(?i)stack-overflow.*classification"),
            ("access violation", "executionComplete", True, r"(?i)access-violation.*classification"),
        )
        for name, field, replacement, message_pattern in mutations:
            with self.subTest(classification=name, field=field):
                attempt = copy.deepcopy(attempts[name])
                attempt[field] = replacement
                with self.assertRaisesRegex(ValidationError, message_pattern):
                    _validate_o09_selected_attempt_outcome(attempt, f"synthetic-{name}")

    def test_f39_repair_rejects_semantic_summary_and_log_tampering(self) -> None:
        expected_artifacts, first_stdout = self.make_f39_repair_fixture()
        with patch.dict(
            "validate_editor_p0_artifacts.EXPECTED_F39_REPAIR_ARTIFACTS",
            expected_artifacts,
            clear=True,
        ):
            validate_f39_repair(self.root, verify_external=True)

        index_path = self.root / "docs/editor-parity/f39-repair-index.json"
        index = json.loads(index_path.read_text(encoding="utf-8"))
        summary_path = self.root.joinpath(*PurePosixPath(index["currentRepair"]["summary"]["path"]).parts)
        summary = json.loads(summary_path.read_text(encoding="utf-8"))
        summary["passed"] = 4
        summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        index["currentRepair"]["summary"].update(
            {
                "bytes": summary_path.stat().st_size,
                "sha256": hashlib.sha256(summary_path.read_bytes()).hexdigest(),
            }
        )
        index_path.write_text(json.dumps(index, indent=2) + "\n", encoding="utf-8")
        expected_artifacts["docs/editor-parity/f39-repair-index.json"] = hashlib.sha256(
            index_path.read_bytes()
        ).hexdigest()
        with patch.dict(
            "validate_editor_p0_artifacts.EXPECTED_F39_REPAIR_ARTIFACTS",
            expected_artifacts,
            clear=True,
        ), self.assertRaisesRegex(ValidationError, r"(?i)summary is not 5/5"):
            validate_f39_repair(self.root, verify_external=True)

        expected_artifacts, first_stdout = self.make_f39_repair_fixture()
        first_stdout.write_text("[CSM_AUTOMATION] marker=config\n", encoding="utf-8")
        with patch.dict(
            "validate_editor_p0_artifacts.EXPECTED_F39_REPAIR_ARTIFACTS",
            expected_artifacts,
            clear=True,
        ), self.assertRaisesRegex(ValidationError, r"(?i)marker [a-z_]+ count differs from log"):
            validate_f39_repair(self.root, verify_external=True)

    def test_ui_runtime_external_verification_recomputes_files_and_inventory(self) -> None:
        evidence_root, data = self.make_ui_runtime_external_data()
        verify_ui_runtime_external(data, evidence_root=evidence_root, expected_entries=5)

        screenshot = evidence_root / "screenshots/main.png"
        screenshot.write_bytes(b"tampered-screenshot")
        with self.assertRaisesRegex(ValidationError, r"(?i)screenshot SHA-256 mismatch"):
            verify_ui_runtime_external(data, evidence_root=evidence_root, expected_entries=5)

    def test_ui_runtime_external_verification_rejects_manifest_escape_and_extra_file(self) -> None:
        evidence_root, data = self.make_ui_runtime_external_data()
        manifest_path = evidence_root / "SHA256SUMS.txt"
        manifest_record = data["sourceEvidence"]["artifactHashManifest"]
        original_manifest = manifest_path.read_text(encoding="utf-8")
        manifest_path.write_text(original_manifest + f"{'0' * 64}  ../escape.bin\n", encoding="utf-8")
        manifest_record["sha256"] = hashlib.sha256(manifest_path.read_bytes()).hexdigest()
        with self.assertRaisesRegex(ValidationError, r"(?i)hash manifest line.*escapes"):
            verify_ui_runtime_external(data, evidence_root=evidence_root, expected_entries=6)

        manifest_path.write_text(original_manifest, encoding="utf-8")
        manifest_record["sha256"] = hashlib.sha256(manifest_path.read_bytes()).hexdigest()
        (evidence_root / "unindexed.bin").write_bytes(b"extra")
        with self.assertRaisesRegex(ValidationError, r"(?i)file inventory differs"):
            verify_ui_runtime_external(data, evidence_root=evidence_root, expected_entries=5)

    def test_rejects_repo_reference_trace_byte_tampering(self) -> None:
        names = (
            "D20-HIERARCHY-REPORT.md",
            "d20-hierarchy-index.json",
            "INPUT-CLIPBOARD-REPORT.md",
            "input-clipboard-index.json",
            "UI-RUNTIME-REPORT.md",
            "ui-runtime-index.json",
            "UI-SEMANTIC-REPORT.md",
            "ui-semantic-index.json",
            "O09-REAL-UI-REPORT.md",
            "o09-real-ui-index.json",
        )
        for name in names:
            with self.subTest(artifact=name):
                artifact = self.root / "docs/editor-parity/reference-traces" / name
                original = artifact.read_bytes()
                artifact.write_bytes(original + b" ")
                try:
                    with self.assertRaisesRegex(
                        ValidationError,
                        rf"(?i)(?:{re.escape(name)}.*(?:hash|SHA-?256|digest)|"
                        rf"(?:hash|SHA-?256|digest).*{re.escape(name)})",
                    ):
                        validate_reference_trace_artifacts(self.root)
                finally:
                    artifact.write_bytes(original)

    def test_current_ui_runtime_index_data_is_valid(self) -> None:
        validate_ui_runtime_index_data(self.load_reference_trace("ui-runtime-index.json"))

    def test_rejects_ui_runtime_locked_identity_mutations(self) -> None:
        baseline = self.load_reference_trace("ui-runtime-index.json")
        mutations = (
            (("sealedEvidenceRoot",), r"G:\ReferenceOracleRuns\other", r"(?i)sealed evidence root"),
            (("reference", "application", "commit"), "0" * 40, r"(?i)application identity"),
            (("reference", "application", "tree"), "0" * 40, r"(?i)application identity"),
            (("reference", "application", "sourceArchive", "sha256"), "0" * 64, r"(?i)application identity"),
            (("reference", "companion", "commit"), "0" * 40, r"(?i)companion identity"),
            (("reference", "companion", "tree"), "0" * 40, r"(?i)companion identity"),
            (("reference", "companion", "sourceArchive", "sha256"), "0" * 64, r"(?i)companion identity"),
            (("reference", "executable", "relativePath"), "other.exe", r"(?i)executable identity"),
            (("reference", "executable", "bytes"), 16502783, r"(?i)executable identity"),
            (("reference", "executable", "bytes"), 16502784.0, r"(?i)executable identity"),
            (("reference", "executable", "sha256"), "0" * 64, r"(?i)executable identity"),
            (("sourceEvidence", "report", "sha256"), "0" * 64, r"(?i)source evidence identity.*report"),
            (("integrity", "exports", "passed"), 1, r"(?i)export integrity summary"),
        )
        for path, replacement, message_pattern in mutations:
            with self.subTest(field=".".join(path)):
                self.assert_data_mutation_rejected(
                    validate_ui_runtime_index_data,
                    baseline,
                    path,
                    replacement,
                    message_pattern,
                )

    def test_rejects_ui_runtime_coverage_ceiling_upgrades(self) -> None:
        baseline = self.load_reference_trace("ui-runtime-index.json")
        expected_statuses = {
            "P0-T02": "targeted-partial-only",
            "O04": "not-closed",
            "O05": "uncovered",
            "O06": "partial",
            "O07": "uncovered",
            "O08": "not-closed",
            "O09": "partial",
            "O10": "not-closed",
        }
        self.assertEqual(
            {scope: baseline["coverageCeiling"][scope]["status"] for scope in expected_statuses},
            expected_statuses,
        )
        for scope, expected_status in expected_statuses.items():
            with self.subTest(scope=scope):
                self.assert_data_mutation_rejected(
                    validate_ui_runtime_index_data,
                    baseline,
                    ("coverageCeiling", scope, "status"),
                    "closed",
                    rf"(?i){re.escape(scope)}.*remain.*{re.escape(expected_status)}",
                )

    def test_rejects_ui_runtime_d20_evidence_or_policy_upgrades(self) -> None:
        baseline = self.load_reference_trace("ui-runtime-index.json")
        mutations = (
            (("coverageCeiling", "d20", "totalCells"), 299, r"(?i)D20 total.*298"),
            (("coverageCeiling", "d20", "totalCells"), 298.0, r"(?i)D20 total.*298"),
            (("coverageCeiling", "d20", "realUiCellsReplayed"), 1, r"(?i)D20 real UI replay.*zero"),
            (("coverageCeiling", "d20", "realUiCellsReplayed"), False, r"(?i)D20 real UI replay.*zero"),
            (
                ("coverageCeiling", "d20", "sourceOrCpuEvidenceUpgradedToUiGpu"),
                True,
                r"(?i)D20 source/CPU evidence.*cannot be upgraded",
            ),
            (("coverageCeiling", "d20", "policySelected"), True, r"(?i)D20 policy.*pending"),
            (
                ("coverageCeiling", "d20", "selectedPolicy"),
                "exact_defect_parity",
                r"(?i)D20 cannot select a policy",
            ),
        )
        for path, replacement, message_pattern in mutations:
            with self.subTest(field=".".join(path)):
                self.assert_data_mutation_rejected(
                    validate_ui_runtime_index_data,
                    baseline,
                    path,
                    replacement,
                    message_pattern,
                )

    def test_rejects_ui_runtime_screenshot_inventory_mutations(self) -> None:
        baseline = self.load_reference_trace("ui-runtime-index.json")
        mutations = (
            (("screenshots", "primaryCount"), 20, r"(?i)19 primary screenshots"),
            (("screenshots", "primaryCount"), 19.0, r"(?i)19 primary screenshots"),
            (("screenshots", "supersededCount"), 3, r"(?i)4 superseded screenshots"),
            (("screenshots", "primary", 0, "sha256"), "0" * 64, r"(?i)primary screenshot hash changed"),
            (
                ("screenshots", "superseded", 0, "grade"),
                "primary-native-ui-vulkan",
                r"(?i)superseded screenshot became citable",
            ),
        )
        for path, replacement, message_pattern in mutations:
            with self.subTest(field=".".join(str(component) for component in path)):
                self.assert_data_mutation_rejected(
                    validate_ui_runtime_index_data,
                    baseline,
                    path,
                    replacement,
                    message_pattern,
                )

    def test_current_d20_trace_index_data_is_valid(self) -> None:
        validate_d20_trace_index_data(self.load_reference_trace("d20-hierarchy-index.json"))

    def test_rejects_d20_locked_summary_mutations(self) -> None:
        baseline = self.load_reference_trace("d20-hierarchy-index.json")
        mutations = (
            (("reference", "commit"), "0" * 40, r"(?i)reference.*commit"),
            (("totals", "cases"), 305, r"(?i)D20.*execution totals changed"),
            (("totals", "passed"), 301, r"(?i)D20.*execution totals changed"),
            (
                ("totals", "processCrashes"),
                1,
                r"(?i)D20.*execution totals changed",
            ),
            (
                ("o09Coverage", "matrixCellCount"),
                297,
                r"(?i)D20.*O09 coverage totals changed",
            ),
        )
        for path, replacement, message_pattern in mutations:
            with self.subTest(field=".".join(path)):
                self.assert_data_mutation_rejected(
                    validate_d20_trace_index_data,
                    baseline,
                    path,
                    replacement,
                    message_pattern,
                )

    def test_rejects_every_o09_cell_registry_and_source_envelope_mismatch(self) -> None:
        baseline = self.load_reference_trace("d20-hierarchy-index.json")
        case_id = baseline["o09Coverage"]["cells"][0]["caseId"]
        registry_index = next(
            index for index, row in enumerate(baseline["caseRegistry"]) if row["caseId"] == case_id
        )
        source_arguments = copy.deepcopy(
            baseline["o09Coverage"]["cells"][0]["sourceEnvelope"]["arguments"]
        )
        source_arguments.pop(next(iter(source_arguments)))
        mutations = (
            (("o09Coverage", "cells", 0, "outcome"), "process_crash_stack_overflow_0xC00000FD", r"(?i)outcome.*registry"),
            (("o09Coverage", "cells", 0, "status"), "captured", r"(?i)status changed"),
            (("o09Coverage", "cells", 0, "requirement"), "D20-DD", r"(?i)requirement classification"),
            (("o09Coverage", "cells", 0, "rawTraceSha256"), "0" * 64, r"(?i)raw trace hash.*registry"),
            (("o09Coverage", "cells", 0, "exit", "processExitCode"), 9, r"(?i)exit.*registry"),
            (("o09Coverage", "cells", 0, "exit", "timedOut"), True, r"(?i)exit.*registry"),
            (("o09Coverage", "cells", 0, "sourceEnvelope", "source"), "src/spoof.cpp:1", r"(?i)source envelope classification"),
            (("o09Coverage", "cells", 0, "sourceEnvelope", "evidenceKind"), "captured_ui_runtime", r"(?i)source envelope classification"),
            (("o09Coverage", "cells", 0, "sourceEnvelope", "uiBranch"), "invented branch", r"(?i)UI branch changed"),
            (("o09Coverage", "cells", 0, "sourceEnvelope", "arguments"), source_arguments, r"(?i)source argument envelope"),
            (("o09Coverage", "cells", 0, "dimensions", "action"), "invented_action", r"(?i)action classification"),
            (("o09Coverage", "cells", 0, "uiRuntimeDriven"), True, r"(?i)must not claim UI runtime"),
            (("caseRegistry", registry_index, "rawTrace", "sha256"), "0" * 64, r"(?i)raw trace hash.*registry"),
            (("caseRegistry", registry_index, "executionStatus"), "invented", r"(?i)zero-exit execution status"),
        )
        for path, replacement, message_pattern in mutations:
            with self.subTest(field=".".join(str(value) for value in path)):
                self.assert_data_mutation_rejected(
                    validate_d20_trace_index_data,
                    baseline,
                    path,
                    replacement,
                    message_pattern,
                )

        coordinated = copy.deepcopy(baseline)
        coordinated["caseRegistry"][registry_index]["executionStatus"] = "invented"
        coordinated["o09Coverage"]["cells"][0]["outcome"] = "invented"
        with self.assertRaisesRegex(ValidationError, r"(?i)zero-exit execution status"):
            validate_d20_trace_index_data(coordinated)

    def test_rejects_d20_scope_flag_mutations(self) -> None:
        baseline = self.load_reference_trace("d20-hierarchy-index.json")
        for flag, expected in baseline["scope"].items():
            with self.subTest(flag=flag):
                self.assert_data_mutation_rejected(
                    validate_d20_trace_index_data,
                    baseline,
                    ("scope", flag),
                    not expected,
                    r"(?i)(?:D20.*scope|scope.*(?:flag|locked|exact))",
                )

    def test_rejects_d20_exact_crash_case_id_mutations(self) -> None:
        baseline = self.load_reference_trace("d20-hierarchy-index.json")
        crash_cases = baseline["criticalEvidence"]["crashCases"]
        self.assertEqual(len(crash_cases), 2)
        for index, crash_case in enumerate(crash_cases):
            case_id = crash_case["caseId"]
            with self.subTest(case_id=case_id):
                self.assert_data_mutation_rejected(
                    validate_d20_trace_index_data,
                    baseline,
                    ("criticalEvidence", "crashCases", index, "caseId"),
                    f"{case_id}_tampered",
                    r"(?i)(?:D20.*crash.*(?:case|ID)|crash.*case.*IDs?)",
                )

    def test_current_input_clipboard_index_data_is_valid(self) -> None:
        validate_input_clipboard_index_data(self.load_reference_trace("input-clipboard-index.json"))

    def test_rejects_input_clipboard_locked_summary_mutations(self) -> None:
        baseline = self.load_reference_trace("input-clipboard-index.json")
        mutations = (
            (("reference", "commit"), "0" * 40, r"(?i)reference.*commit"),
            (
                ("recordSummary", "primary", "records"),
                143,
                r"(?i)(?:records.*144|144.*records)",
            ),
            (
                ("recordSummary", "repeat", "records"),
                143,
                r"(?i)(?:records.*144|144.*records)",
            ),
            (
                ("recordSummary", "primary", "assertions"),
                39,
                r"(?i)(?:assertions.*40|40.*assertions)",
            ),
            (
                ("recordSummary", "repeat", "assertions"),
                39,
                r"(?i)(?:assertions.*40|40.*assertions)",
            ),
            (
                ("recordSummary", "primary", "assertionFailures"),
                1,
                r"(?i)(?:assertionFailures|assertion failures)",
            ),
            (
                ("recordSummary", "repeat", "assertionFailures"),
                1,
                r"(?i)(?:assertionFailures|assertion failures)",
            ),
            (
                ("repeatability", "sealedNormalizedPrimarySha256"),
                "0" * 64,
                r"(?i)(?:sealedNormalizedPrimarySha256|normalized.*(?:SHA-?256|hash))",
            ),
            (
                ("repeatability", "sealedNormalizedRepeatSha256"),
                "0" * 64,
                r"(?i)(?:sealedNormalizedRepeatSha256|normalized.*(?:SHA-?256|hash))",
            ),
            (
                ("evidenceScope", "fullInteractiveApplicationObservation"),
                True,
                r"(?i)(?:fullInteractiveApplicationObservation|full.*application.*observation)",
            ),
        )
        for path, replacement, message_pattern in mutations:
            with self.subTest(field=".".join(path)):
                self.assert_data_mutation_rejected(
                    validate_input_clipboard_index_data,
                    baseline,
                    path,
                    replacement,
                    message_pattern,
                )

    def test_current_corpus_semantics_are_valid(self) -> None:
        validate_corpus(self.root)

    def test_rejects_degenerate_multi_scene_even_with_updated_hash(self) -> None:
        fixture_path = self.root / "tests/fixtures/gltf_editor/multi_scene_oracle.gltf"
        document = json.loads(fixture_path.read_text(encoding="utf-8"))
        document["scenes"][1]["nodes"] = [3, 6]
        encoded = (json.dumps(document, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
        fixture_path.write_bytes(encoded)
        corpus_path = self.root / "tests/fixtures/gltf_editor/corpus.json"
        corpus = json.loads(corpus_path.read_text(encoding="utf-8"))
        row = next(item for item in corpus["fixtures"] if item["path"] == "multi_scene_oracle.gltf")
        row["bytes"] = len(encoded)
        row["sha256"] = hashlib.sha256(encoded).hexdigest()
        corpus_path.write_text(json.dumps(corpus, indent=2) + "\n", encoding="utf-8")
        with self.assertRaisesRegex(ValidationError, "scene 1 roots"):
            validate_corpus(self.root)

    def test_rejects_fixture_path_traversal(self) -> None:
        corpus_path = self.root / "tests/fixtures/gltf_editor/corpus.json"
        corpus = json.loads(corpus_path.read_text(encoding="utf-8"))
        corpus["fixtures"][0]["path"] = "../invalid_child_index.gltf"
        corpus_path.write_text(json.dumps(corpus, indent=2) + "\n", encoding="utf-8")
        with self.assertRaisesRegex(ValidationError, "must not escape"):
            validate_corpus(self.root)

    def test_rejects_unbound_and_tampered_binary_fixtures(self) -> None:
        fixture_root = self.root / "tests/fixtures/gltf_editor"
        unbound = fixture_root / "unbound.gltf"
        unbound.write_text('{"asset":{"version":"2.0"}}\n', encoding="utf-8")
        with self.assertRaisesRegex(ValidationError, "must bind every committed fixture"):
            validate_corpus(self.root)
        unbound.unlink()

        (fixture_root / "external_buffer.bin").write_bytes(b"MGIFDATA!")
        with self.assertRaisesRegex(ValidationError, "fixture (?:length|hash) mismatch"):
            validate_corpus(self.root)

    def test_current_edge_registry_is_valid(self) -> None:
        validate_edge_obligations(self.root)

    def test_rejects_noncontiguous_edge_category_order(self) -> None:
        edges = self.load_edges()
        edges["obligations"][0]["category"], edges["obligations"][16]["category"] = (
            edges["obligations"][16]["category"],
            edges["obligations"][0]["category"],
        )
        self.write_edges(edges)
        with self.assertRaisesRegex(ValidationError, "contiguous section 13.1 category order"):
            validate_edge_obligations(self.root)

    def test_rejects_requirement_category_incidence_drift(self) -> None:
        edges = self.load_edges()
        edges["obligations"][0]["requirements"] = ["R02"]
        edges["obligations"][0]["features"] = ["F09"]
        self.write_edges(edges)
        with self.assertRaisesRegex(ValidationError, "section 13.2 requirement/category incidences"):
            validate_edge_obligations(self.root)

    def test_rejects_missing_reference_oracle_allocation(self) -> None:
        edges = self.load_edges()
        for row in edges["obligations"]:
            row["targetEvidence"] = [item for item in row["targetEvidence"] if item != "O04-unique-root-delete"]
        self.write_edges(edges)
        with self.assertRaisesRegex(ValidationError, "allocate every required O01-O10"):
            validate_edge_obligations(self.root)

    def test_rejects_undefined_edge_evidence_id(self) -> None:
        edges = self.load_edges()
        edges["obligations"][0]["targetEvidence"].append("T-NOT-DEFINED-99")
        self.write_edges(edges)
        with self.assertRaisesRegex(ValidationError, "undefined targetEvidence"):
            validate_edge_obligations(self.root)

    def test_rejects_false_edge_execution_claim(self) -> None:
        edges = self.load_edges()
        edges["obligations"][0]["status"] = "executed_passed"
        self.write_edges(edges)
        with self.assertRaisesRegex(ValidationError, "must remain assigned"):
            validate_edge_obligations(self.root)

    def test_rejects_false_dependency_boundary_execution_claim(self) -> None:
        edges = self.load_edges()
        edges["dependencyBoundaryBindings"]["executed"] = 1
        self.write_edges(edges)
        with self.assertRaisesRegex(ValidationError, r"(?i)dependencyBoundaryBindings.*without claiming execution"):
            validate_edge_obligations(self.root)


if __name__ == "__main__":
    unittest.main()
