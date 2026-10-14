#!/usr/bin/env python3
"""Static contract checks for the Windows-only P7 visual capture harness."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "editor_visual_capture.ps1"
APP_SOURCE = ROOT / "app" / "MinimalLatestApp.cpp"
APP_HEADER = ROOT / "app" / "MinimalLatestApp.h"
CASES = (
    "V01-grid",
    "V02-gizmo-components",
    "V03-parent-space",
    "V04-depth-xray",
    "V07-dpi-resize",
)


class EditorVisualCaptureContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = SCRIPT.read_text(encoding="utf-8")
        cls.app_source = APP_SOURCE.read_text(encoding="utf-8")
        cls.app_header = APP_HEADER.read_text(encoding="utf-8")

    def test_windows_harness_is_explicit_and_closed_world(self) -> None:
        self.assertIn("Set-StrictMode -Version Latest", self.source)
        self.assertIn('authoritativePlatform = "Windows MSVC"', self.source)
        self.assertIn("GetClientRect", self.source)
        self.assertIn("ClientToScreen", self.source)
        self.assertIn("CopyFromScreen", self.source)
        self.assertIn("BitBlt", self.source)
        self.assertIn("PrintWindow", self.source)
        self.assertIn("GetDC", self.source)
        self.assertIn("ReleaseDC", self.source)
        self.assertIn("SetCursorPos", self.source)
        self.assertIn("ClipCursor", self.source)
        self.assertIn("ReleaseCursor", self.source)
        self.assertIn("GetAsyncKeyState", self.source)
        self.assertIn("PostMessage", self.source)
        self.assertIn("mouse_event", self.source)
        self.assertIn("$process.Kill()", self.source)
        self.assertIn("VK_LAYER_KHRONOS_validation", self.source)
        self.assertNotIn("renderdoccmd", self.source.lower())
        self.assertNotIn("orca", self.source.lower())
        self.assertNotIn("mgif build entrypoints", self.source.lower())

    def test_case_matrix_and_required_artifacts_are_bound(self) -> None:
        for case in CASES:
            self.assertIn(f'"{case}"', self.source)
        for artifact in (".png", "state.json", "pixel.json", "stdout.log", "stderr.log", "manifest.json"):
            self.assertIn(artifact, self.source)
        for stage in ("normal", "hover", "active", "post-release"):
            self.assertIn(f'"{stage}.png"', self.source)
            self.assertIn(f'"{stage}-state.json"', self.source)
        self.assertIn('"mgif.editor-visual-state.v2"', self.source)
        self.assertIn('"p7-editor-visual-pixel-audit"', self.source)
        self.assertIn('"p7-editor-visual-evidence"', self.source)
        for field in (
            "document_digest",
            "projection_digest",
            "runtime_content_digest",
            "action_log",
            "visual-boundary",
            "command_count",
            "graph_rows",
            "inspector",
        ):
            self.assertIn(field, self.app_header)

    def test_normal_capture_is_post_render_and_input_is_armed_afterward(self) -> None:
        expected = '"arm-last-moving", "last-moving", "first-still", "normal-rendered", "arm-settled", "settled", "control-still", "control-rendered", "control-presented"'
        self.assertIn(expected, self.source)
        self.assertIn('payload.phase -ne "post-render"', self.source)
        self.assertIn('payload.frame -ne $expectedNormalRenderedFenceFrame', self.source)
        self.assertIn('payload.rendered_frame -ne $expectedFirstStillFrame', self.source)
        self.assertIn('return "normal-rendered";', self.app_header)
        self.assertIn('rendered_frame', self.app_header)
        self.assertIn('visualNormalRendered', self.app_header)
        self.assertIn('m_renderer.waitForIdle();', self.app_header)
        self.assertIn('fence.gpu_idle_waited', self.source)
        self.assertIn('controlCapturePending', self.source)
        self.assertIn('expectedControlPresentedFenceFrame', self.source)
        self.assertIn('payload.rendered_frame -ne $expectedControlStillFrame', self.source)
        self.assertIn('handshakePhase', self.app_header)
        self.assertIn('if ($null -ne $inputPoint -and $inputArmed', self.source)
        self.assertIn('Copy-Item -LiteralPath $statePath -Destination $normalStatePath -Force', self.source)

    def test_closed_variant_and_component_matrix_is_explicit(self) -> None:
        for projection in ("perspective", "orthographic"):
            for unit in ("0.001", "1", "1000"):
                self.assertIn(f'"{projection}-unit-{unit}"', self.source)
        self.assertIn('"perspective-horizon"', self.source)
        self.assertIn('@(0..12)', self.source)
        self.assertIn('variant = "component"', self.source)
        self.assertIn('"rotated-negative-nonuniform"', self.source)
        self.assertIn('@("xray", "occlusion-dither")', self.source)
        self.assertIn('"framebuffer-dpi-resize"', self.source)
        for source in (
            '"render/passes/EditorOverlayPass.cpp"',
            '"rhi/vulkan/VulkanSwapchain.cpp"',
            '"app/editor/MaterialClipboardCodec.cpp"',
            '"shaders/editor_overlay.slang"',
            '"tests/fixtures/gltf_editor/r09_shared_mesh.gltf"',
            '"tests/fixtures/gltf_editor/minimal_scene.gltf"',
        ):
            self.assertIn(source, self.source)
        self.assertNotIn('editorVisualCase != "all"', self.app_source)
        self.assertIn("componentAbsent", self.app_source)

    def test_dpi_and_case_specific_assertions_are_fail_closed(self) -> None:
        self.assertNotRegex(self.source, r"\$Width\s*\*\s*\$DpiScale")
        self.assertNotRegex(self.source, r"\$Height\s*\*\s*\$DpiScale")
        self.assertIn("observed_window_framebuffer_scale", self.source)
        self.assertIn('"synthetic-offscreen"', self.source)
        self.assertIn("physical_extent", self.source)
        self.assertIn("color_attachment_extent", self.source)
        self.assertIn("surface_transform_token", self.source)
        self.assertIn("V03 outside-viewport drag did not commit", self.source)
        self.assertIn("V04 xray and occlusion-dither", self.source)
        self.assertIn("V02 target probe/snap contract differs", self.source)
        self.assertIn("Assert-TargetPixels", self.source)
        self.assertIn("Assert-ComponentIndexTable", self.source)
        self.assertIn("outside drag point did not cross", self.source)
        self.assertIn("Pin-VisualWindow", self.source)
        self.assertIn("GetForegroundWindow", self.source)
        self.assertIn("if(m_editorVisualEnabled)", self.app_header)
        self.assertIn("glfwPollEvents();", self.app_header)
        for field in ("showPassGpuProfile", "showGPUCullingOverlay", "showViewportAxis"):
            self.assertIn(f"m_debugOptions.{field} = false;", self.app_header)
        self.assertIn("rhi_overlay", self.source)
        self.assertIn("native_escape_calls", self.source)
        self.assertIn("!m_editorVisualEnabled", self.app_header)
        self.assertIn("DockBuilderFinish(mainDockspaceID)", self.app_header)
        self.assertIn("StringComparer]::Ordinal", self.source)

    def test_fail_closed_pixel_and_runtime_diagnostics(self) -> None:
        self.assertRegex(self.source, r"nonBlack\s*-lt\s*500")
        self.assertRegex(self.source, r"colorful\s*-lt\s*20")
        self.assertIn("VUID-", self.source)
        self.assertIn("SYNC-HAZARD", self.source)
        self.assertIn("No control-still capture was observed", self.source)

    def test_no_linux_build_or_unbounded_process_control(self) -> None:
        self.assertNotRegex(self.source, r"(?i)cmake\\s+--build|ninja|make\\s")
        self.assertIn("MainWindowHandle", self.source)
        self.assertIn("HWND_TOPMOST", self.source)
        self.assertIn("fresh", self.source.lower())


if __name__ == "__main__":
    unittest.main()
