"""Source-level contracts for the Android viewer and Dear ImGui identity policy."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
ANDROID_APP = (REPO_ROOT / "app/AndroidNativeApp.cpp").read_text(encoding="utf-8")
MINIMAL_APP = (REPO_ROOT / "app/MinimalLatestApp.h").read_text(encoding="utf-8")
GPU_RENDERER_SOURCE = (REPO_ROOT / "render/GPUDrivenRenderer.cpp").read_text(
    encoding="utf-8"
)
GPU_RENDERER_HEADER = (REPO_ROOT / "render/GPUDrivenRenderer.h").read_text(
    encoding="utf-8"
)
ANDROID_GRADLE = (REPO_ROOT / "android/app/build.gradle").read_text(encoding="utf-8")
ANDROID_GENERATOR = (REPO_ROOT / "tools/generate_android_gradle_project.ps1").read_text(
    encoding="utf-8"
)
CMAKE = (REPO_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")

IMGUI_ANDROID_HEADER = "#include <backends/imgui_impl_android.h>"
IMGUI_ANDROID_HANDLER = "ImGui_ImplAndroid_HandleInputEvent"
IMGUI_PINNED_COMMIT = "fd13a1e8923a0a7077b404fc36fd063b25a0c0b5"

DESKTOP_EDITOR_MARKERS = (
    "app/editor",
    "render/editor",
    "MinimalLatestApp",
    "EditorController",
    "EditorOverlay",
    "imgui_impl_glfw",
    "SceneBrowserPanel",
    "InspectorPanel",
    "NativeFileDialog",
    "tinyfiledialogs",
    "portable-file-dialogs",
    "pfd::",
    "FileDialog",
    "FileBrowser",
    "nfd_",
)

P5_DESKTOP_ONLY_TARGETS = (
    "renderer_scene_transaction",
    "editor_renderer_integration_tests",
)

P5_DOCUMENT_ADAPTER_TARGETS = (
    "scene_change_capture_runtime",
    "scene_runtime_mapping_contract",
)

P6_P7_DESKTOP_RENDER_MARKERS = (
    "editor_picking_runtime",
    "EditorPickingPass",
    "ScenePicking",
    "SceneRenderOccurrenceMapping",
    "editor_picking.slang",
    "editor_highlight.slang",
    "editor_gizmo_overlay_runtime",
    "editor_overlay_rhi_runtime",
    "EditorOverlayPass",
    "editor_overlay.slang",
    "editor_gizmo_integration_tests",
    "editor_overlay_rhi_tests",
    "ImGuiClipboardAdapter",
)

EXPECTED_ANDROID_CACHE_ARGUMENTS = {
    "Slang_ROOT",
    "FETCHCONTENT_SOURCE_DIR_GLM",
    "FETCHCONTENT_SOURCE_DIR_VOLK",
    "FETCHCONTENT_SOURCE_DIR_TINYGLTF",
    "FETCHCONTENT_SOURCE_DIR_MESHOPTIMIZER",
    "FETCHCONTENT_SOURCE_DIR_VULKANMEMORYALLOCATOR",
    "DEMO_VULKAN_HEADER_DIR",
}


def generated_app_gradle() -> str:
    template_match = re.search(
        r"\$appBuildGradle\s*=\s*@'\r?\n(?P<body>.*?)\r?\n'@",
        ANDROID_GENERATOR,
        re.DOTALL,
    )
    if template_match is None:
        raise AssertionError("Android Gradle generator template was not found")

    generated = template_match.group("body")
    for placeholder, value in {
        "@@NAMESPACE@@": "com.vkdemo",
        "@@COMPILE_SDK@@": "35",
        "@@NDK_VERSION@@": "23.1.7779620",
        "@@APPLICATION_ID@@": "com.vkdemo",
        "@@MIN_SDK@@": "26",
        "@@TARGET_SDK@@": "35",
        "@@ABI_FILTERS@@": '"arm64-v8a"',
    }.items():
        generated = generated.replace(placeholder, value)
    return generated


GENERATED_ANDROID_GRADLE = generated_app_gradle()


def android_active_cmake(source: str) -> str:
    """Return lines that can be active when CMake's ANDROID variable is true."""

    active = True
    frames: list[tuple[bool, bool, bool]] = []
    result: list[str] = []
    logical_lines: list[str] = []
    pending_control: list[str] = []
    parenthesis_depth = 0

    for line in source.splitlines():
        stripped = line.strip()
        if pending_control:
            pending_control.append(line)
            parenthesis_depth += line.count("(") - line.count(")")
            if parenthesis_depth == 0:
                logical_lines.append(" ".join(pending_control))
                pending_control.clear()
            continue

        if re.match(r"(?:if|elseif|else|endif)\s*\(", stripped, re.IGNORECASE):
            pending_control.append(line)
            parenthesis_depth = line.count("(") - line.count(")")
            if parenthesis_depth == 0:
                logical_lines.append(line)
                pending_control.clear()
            continue

        logical_lines.append(line)

    if pending_control:
        raise AssertionError("unterminated multiline CMake control command")

    for line in logical_lines:
        stripped = line.strip()
        if_match = re.match(r"if\s*\((.*)\)\s*$", stripped, re.IGNORECASE)
        if if_match:
            expression = re.sub(r"\s+", " ", if_match.group(1).strip()).upper()
            android_false_conjunction = re.fullmatch(
                r"(?:[A-Z_][A-Z0-9_]* AND )*NOT ANDROID"
                r"(?: AND [A-Z_][A-Z0-9_]*)*",
                expression,
            )
            if android_false_conjunction:
                if_possible, else_possible = False, True
            elif expression == "ANDROID":
                if_possible, else_possible = True, False
            else:
                # Unknown build predicates can select either branch, so keep both.
                if_possible, else_possible = True, True
            frames.append((active, if_possible, else_possible))
            active = active and if_possible
            continue

        if re.match(r"elseif\s*\(", stripped, re.IGNORECASE):
            if not frames:
                raise AssertionError("unmatched CMake elseif()")
            parent_active, _, _ = frames[-1]
            # Conservatively include every elseif branch under Android.
            active = parent_active
            continue

        if re.match(r"else\b\s*\(?.*\)?\s*$", stripped, re.IGNORECASE):
            if not frames:
                raise AssertionError("unmatched CMake else()")
            parent_active, _, else_possible = frames[-1]
            active = parent_active and else_possible
            continue

        if re.match(r"endif\s*\(?.*\)?\s*$", stripped, re.IGNORECASE):
            if not frames:
                raise AssertionError("unmatched CMake endif()")
            parent_active, _, _ = frames.pop()
            active = parent_active
            continue

        if active:
            result.append(line)

    if frames:
        raise AssertionError("unterminated CMake if() block")
    return "\n".join(result)


def target_command_bodies(source: str, command: str, target: str) -> list[str]:
    pattern = re.compile(
        rf"\b{re.escape(command)}\s*\(\s*{re.escape(target)}\b(?P<body>.*?)\)",
        re.IGNORECASE | re.DOTALL,
    )
    return [match.group("body") for match in pattern.finditer(source)]


def unguarded_android_tokens(source: str, tokens: tuple[str, ...]) -> list[str]:
    """Return tokens that occur outside a !__ANDROID__ preprocessor region."""

    token_pattern = re.compile(
        r"\b(?:" + "|".join(re.escape(token) for token in tokens) + r")\b"
    )
    android_guard_depth = 0
    guard_stack: list[bool] = []
    violations: list[str] = []

    for line in source.splitlines():
        directive = line.strip()
        if re.match(r"#\s*if(?:n?def)?\b", directive, re.IGNORECASE):
            is_android_guard = bool(
                re.search(
                    r"!\s*defined\s*\(\s*__ANDROID__\s*\)",
                    directive,
                    re.IGNORECASE,
                )
            )
            guard_stack.append(is_android_guard)
            if is_android_guard:
                android_guard_depth += 1
        elif re.match(r"#\s*endif\b", directive, re.IGNORECASE):
            if guard_stack:
                if guard_stack.pop():
                    android_guard_depth -= 1

        if android_guard_depth == 0:
            violations.extend(match.group(0) for match in token_pattern.finditer(line))

    return violations


class AndroidViewerContractTests(unittest.TestCase):
    def test_android_backend_header_precedes_input_handler_use(self) -> None:
        self.assertEqual(ANDROID_APP.count(IMGUI_ANDROID_HEADER), 1)
        self.assertLess(
            ANDROID_APP.index(IMGUI_ANDROID_HEADER),
            ANDROID_APP.index(IMGUI_ANDROID_HANDLER),
        )

    def test_checked_in_and_generated_gradle_keep_non_imgui_caches_only(self) -> None:
        for label, gradle in (
            ("checked-in Gradle", ANDROID_GRADLE),
            ("generator output", GENERATED_ANDROID_GRADLE),
        ):
            with self.subTest(label=label):
                self.assertNotRegex(
                    gradle,
                    re.compile(r"\bFETCHCONTENT_SOURCE_DIR_IMGUI\b", re.IGNORECASE),
                )
                forwarded_arguments = set(
                    re.findall(r'addCmakePathIfExists\("([^"]+)"', gradle)
                )
                self.assertEqual(forwarded_arguments, EXPECTED_ANDROID_CACHE_ARGUMENTS)

        self.assertEqual(
            ANDROID_GRADLE.splitlines(),
            GENERATED_ANDROID_GRADLE.splitlines(),
        )

    def test_root_cmake_keeps_pinned_zero_override_imgui_identity(self) -> None:
        declaration = re.search(
            r"FetchContent_Declare\(\s*imgui\b(?P<body>.*?)\)",
            CMAKE,
            re.IGNORECASE | re.DOTALL,
        )
        self.assertIsNotNone(declaration)
        declaration_body = declaration.group("body")
        self.assertIn("https://github.com/ocornut/imgui.git", declaration_body)
        self.assertRegex(
            declaration_body,
            re.compile(rf"\bGIT_TAG\s+{IMGUI_PINNED_COMMIT}\b", re.IGNORECASE),
        )
        self.assertIn(
            "FETCHCONTENT_SOURCE_DIR_IMGUI overrides are forbidden",
            CMAKE,
        )

    def test_android_build_has_no_desktop_editor_ui_orchestration(self) -> None:
        active_cmake = android_active_cmake(CMAKE)
        active_production_source = ANDROID_APP + "\n" + active_cmake
        for marker in DESKTOP_EDITOR_MARKERS:
            self.assertNotIn(marker.casefold(), active_production_source.casefold(), marker)

        compact_cmake = re.sub(r"\s+", " ", active_cmake)
        self.assertIn(
            "add_library(DemoAndroid SHARED app/AndroidNativeApp.cpp)",
            compact_cmake,
        )

    def test_android_closure_excludes_p5_desktop_transactions(self) -> None:
        active_cmake = android_active_cmake(CMAKE)
        compact_active = re.sub(r"\s+", " ", active_cmake)

        self.assertNotIn("MGIF_ENABLE_SCENE_TRANSACTIONS", active_cmake)
        for target in P5_DESKTOP_ONLY_TARGETS:
            self.assertNotRegex(
                compact_active,
                re.compile(
                    rf"\b(?:add_library|add_executable)\s*\(\s*{re.escape(target)}\b",
                    re.IGNORECASE,
                ),
                target,
            )

        demo_core_links = "\n".join(
            target_command_bodies(active_cmake, "target_link_libraries", "demo_core")
        )
        for target in (*P5_DESKTOP_ONLY_TARGETS, *P5_DOCUMENT_ADAPTER_TARGETS):
            self.assertNotRegex(
                demo_core_links,
                re.compile(rf"\b{re.escape(target)}\b", re.IGNORECASE),
                target,
            )

        android_links = target_command_bodies(
            active_cmake, "target_link_libraries", "DemoAndroid"
        )
        self.assertEqual(len(android_links), 1)
        self.assertEqual(re.findall(r"\b[A-Za-z_][A-Za-z0-9_]*\b", android_links[0]),
                         ["PRIVATE", "demo_core"])

    def test_android_closure_excludes_p6_p7_desktop_rendering(self) -> None:
        active_cmake = android_active_cmake(CMAKE)
        for marker in P6_P7_DESKTOP_RENDER_MARKERS:
            self.assertNotIn(marker.casefold(), active_cmake.casefold(), marker)

        shader_filter = re.search(
            r'if\(ANDROID\)\s+list\(FILTER SHADER_SLANG_FILES EXCLUDE REGEX '
            r'"editor_\(picking\|highlight\|overlay\)\\\\\.slang\$"\)\s+'
            r'list\(FILTER SHADER_GLSL_FILES EXCLUDE REGEX '
            r'"editor_\(picking\|highlight\|overlay\)\\\\\.glsl\$"\)\s+endif\(\)',
            CMAKE,
            re.IGNORECASE | re.DOTALL,
        )
        self.assertIsNotNone(shader_filter)

        # The source list is a glob, so verify each forbidden editor shader
        # basename is covered by the Android filters rather than relying on a
        # fixed set of files in the current checkout.
        for extension in ("slang", "glsl"):
            self.assertRegex(
                CMAKE,
                re.compile(
                    rf'list\(FILTER SHADER_[A-Z]+_FILES EXCLUDE REGEX '
                    rf'"editor_\(picking\|highlight\|overlay\)\\\\\.{extension}\$"\)',
                    re.IGNORECASE,
                ),
            )

    def test_android_renderer_shader_embeds_are_desktop_guarded(self) -> None:
        # Android must be able to compile GPUDrivenRenderer without generated
        # editor shader headers or overlay/picking symbols.
        self.assertRegex(
            GPU_RENDERER_SOURCE,
            re.compile(
                r'#if\s*!defined\(__ANDROID__\).*?'
                r'#include\s+"_autogen/editor_highlight\.slang\.h".*?'
                r'#include\s+"_autogen/editor_overlay\.slang\.h".*?'
                r'#include\s+"_autogen/editor_picking\.slang\.h".*?'
                r'#endif',
                re.IGNORECASE | re.DOTALL,
            ),
        )
        self.assertEqual(
            unguarded_android_tokens(
                GPU_RENDERER_SOURCE,
                (
                    "editor_overlay_slang",
                    "editor_picking_slang",
                    "editor_highlight_slang",
                ),
            ),
            [],
        )

    def test_p7_snapshot_does_not_use_legacy_imgui_overlay(self) -> None:
        self.assertNotIn("m_editorViewportRuntime->drawImGuiOverlay", MINIMAL_APP)
        self.assertIn("m_editorGizmoAdapter->snapshot", MINIMAL_APP)
        self.assertIn("editorOverlayFrame", MINIMAL_APP)

    def test_android_does_not_include_overlay_pass_header(self) -> None:
        guarded_include = re.compile(
            r"#if\s*!defined\(__ANDROID__\)\s*"
            r"(?:(?!#endif).)*"
            r"#include\s+\"passes/EditorOverlayPass\.h\""
            r"(?:(?!#endif).)*#endif",
            re.IGNORECASE | re.DOTALL,
        )
        self.assertRegex(GPU_RENDERER_HEADER, guarded_include)


if __name__ == "__main__":
    unittest.main()
