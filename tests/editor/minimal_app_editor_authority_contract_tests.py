#!/usr/bin/env python3
"""Static regression for the desktop editor authority cutover."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HEADER = (ROOT / "app/MinimalLatestApp.h").read_text(encoding="utf-8")
SOURCE = (ROOT / "app/MinimalLatestApp.cpp").read_text(encoding="utf-8")
APP = HEADER + "\n" + SOURCE


def function_body(name: str) -> str:
    marker = f"MinimalLatestApp::{name}"
    start = HEADER.find(marker)
    if start < 0:
        raise AssertionError(f"missing function {name}")
    brace = HEADER.find("{", start)
    if brace < 0:
        raise AssertionError(f"missing body for {name}")
    depth = 0
    for index in range(brace, len(HEADER)):
        if HEADER[index] == "{":
            depth += 1
        elif HEADER[index] == "}":
            depth -= 1
            if depth == 0:
                return HEADER[brace : index + 1]
    raise AssertionError(f"unterminated body for {name}")


class MinimalAppEditorAuthorityContractTests(unittest.TestCase):
    def test_legacy_desktop_loader_and_mutation_symbols_are_absent(self) -> None:
        forbidden = (
            "AsyncLoadResult",
            "loadModelAsync",
            "updateAsyncLoading",
            "beginLegacySceneUpload",
            "beginExperimentalSceneUpload",
            "m_sceneModel",
            "m_currentModel",
            "m_sceneAssetView",
            "m_sceneUploadPlan",
            "m_asyncLoadingCoordinator",
            "editableSceneLights",
            "m_editorTransactionNodeIndex",
            "destroyGltfResources",
            "uploadGltfModelBatch",
        )
        for symbol in forbidden:
            with self.subTest(symbol=symbol):
                self.assertNotIn(symbol, APP)

    def test_runtime_snapshot_is_const_and_not_an_editor_authority(self) -> None:
        self.assertIn(
            "std::shared_ptr<const demo::SceneAsset> m_projectedSceneAsset;",
            HEADER,
        )
        self.assertIn("frameParams.gltfModel      = nullptr;", HEADER)
        # The published snapshot is const-qualified, so a write through it
        # cannot compile.  This guard forbids assignment syntax only; the
        # negative lookahead keeps read-only `->field == value` authority
        # comparisons legal.
        self.assertNotRegex(
            HEADER,
            r"m_projectedSceneAsset\s*->\s*[A-Za-z_][A-Za-z0-9_]*\s*=(?!=)",
        )
        self.assertNotIn("std::optional<demo::SceneAsset>", HEADER)

    def test_open_prepares_candidate_before_renderer_publication(self) -> None:
        body = function_body("openModelWithEditor")
        self.assertIn("m_editorController->open(path, &error)", body)
        for destructive in (
            "resetPublishedEditorScene",
            "m_editorRendererAdapter.reset",
            "waitForIdle",
            "destroy",
            "loadModelAsync",
        ):
            with self.subTest(destructive=destructive):
                self.assertNotIn(destructive, body)

    def test_busy_state_distinguishes_no_document_from_pending_publication(self) -> None:
        self.assertGreaterEqual(
            HEADER.count(
                "m_editorController->hasDocument() &&\n"
                "          !m_projectedSceneAsset"
            ),
            2,
        )
        self.assertNotIn(
            "return m_editorRendererAdapter.hasPending() ||\n"
            "             !m_projectedSceneAsset;",
            HEADER,
        )

    def test_camera_polling_occurs_after_imgui_frame_begin(self) -> None:
        new_frame = HEADER.find("m_renderer.beginUiFrame();")
        camera_input = HEADER.find("updateCameraInput();", new_frame)
        self.assertGreaterEqual(new_frame, 0)
        self.assertGreater(camera_input, new_frame)

    def test_document_reset_uses_one_adapter_owned_release_boundary(self) -> None:
        reset = function_body("resetPublishedEditorScene")
        self.assertEqual(reset.count("m_editorRendererAdapter.reset(m_renderer)"), 1)
        self.assertEqual(reset.count("m_projectedSceneAsset.reset()"), 1)
        self.assertNotRegex(reset, re.compile(r"waitForIdle|destroyGltfResources"))


if __name__ == "__main__":
    unittest.main()
