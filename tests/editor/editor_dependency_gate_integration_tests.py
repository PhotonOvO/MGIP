#!/usr/bin/env python3
"""Real CMake/Ninja regressions for the final configured editor dependency gate."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
VALIDATOR = REPO_ROOT / "tools/validate_editor_p0_artifacts.py"
sys.path.insert(0, str(REPO_ROOT / "tools"))

from generate_editor_dependency_identity import (  # noqa: E402
    IdentityError,
    _resolve_file_api_source_path,
)


DOCUMENT_SOURCES = (
    "loader/TinyGltfImplementation.cpp",
    "scene/editor/include/mgif/scene_editor/EditorLimits.h",
    "scene/editor/include/mgif/scene_editor/EditorObjectId.h",
    "scene/editor/include/mgif/scene_editor/EditorHistoryPolicy.h",
    "scene/editor/include/mgif/scene_editor/GltfDocument.h",
    "scene/editor/include/mgif/scene_editor/StructuralDelta.h",
    "scene/editor/include/mgif/scene_editor/SceneCommands.h",
    "scene/editor/include/mgif/scene_editor/GltfDocumentIO.h",
    "scene/editor/include/mgif/scene_editor/GltfSemanticCompare.h",
    "scene/editor/include/mgif/scene_editor/GltfValidator.h",
    "scene/editor/GltfDocument.cpp",
    "scene/editor/StructuralDelta.cpp",
    "scene/editor/SceneCommands.cpp",
    "scene/editor/GltfDocumentIO.cpp",
    "scene/editor/GltfSemanticCompare.cpp",
    "scene/editor/GltfValidator.cpp",
)
DOCUMENT_COMPILED_SOURCES = tuple(value for value in DOCUMENT_SOURCES if value.endswith(".cpp"))
CORE_SOURCES = (
    "scene/editor/include/mgif/scene_editor/EditorClock.h",
    "scene/editor/include/mgif/scene_editor/EditorFaultInjection.h",
    "scene/editor/include/mgif/scene_editor/EditorSelection.h",
    "scene/editor/include/mgif/scene_editor/testing/EditorStateInspector.h",
    "scene/editor/include/mgif/scene_editor/commands/EditorCommand.h",
    "scene/editor/include/mgif/scene_editor/commands/EditorHistory.h",
    "scene/editor/include/mgif/scene_editor/commands/HistoryLifecycle.h",
    "scene/editor/include/mgif/scene_editor/commands/TransformCommands.h",
    "scene/editor/include/mgif/scene_editor/commands/HierarchyCommands.h",
    "scene/editor/include/mgif/scene_editor/commands/PropertyCommands.h",
    "scene/editor/include/mgif/scene_editor/commands/ContinuousEdit.h",
    "scene/editor/include/mgif/scene_editor/EditorSession.h",
    "scene/editor/include/mgif/scene_editor/SceneEditor.h",
    "scene/editor/SceneEditorCore.cpp",
    "scene/editor/EditorSelection.cpp",
    "scene/editor/commands/EditorCommand.cpp",
    "scene/editor/commands/EditorHistory.cpp",
    "scene/editor/commands/HistoryLifecycle.cpp",
    "scene/editor/commands/TransformCommands.cpp",
    "scene/editor/commands/HierarchyCommands.cpp",
    "scene/editor/commands/PropertyCommands.cpp",
    "scene/editor/commands/ContinuousEdit.cpp",
    "scene/editor/EditorSession.cpp",
    "scene/editor/SceneEditor.cpp",
)
CORE_COMPILED_SOURCES = tuple(value for value in CORE_SOURCES if value.endswith(".cpp"))
P4_TEST_TARGETS = (
    "editor_history_core_tests",
    "editor_history_structural_tests",
    "editor_history_property_tests",
    "editor_continuous_edit_tests",
)


class FileApiSourcePathResolutionTests(unittest.TestCase):
    def test_relative_sources_use_codemodel_roots_and_absolute_sources_stay_absolute(self) -> None:
        with tempfile.TemporaryDirectory(prefix="mgif-file-api-source-unit-") as temporary_directory:
            root = Path(temporary_directory)
            source_root = root / "source"
            build_root = root / "build"
            absolute = root / "absolute.cpp"

            self.assertEqual(
                _resolve_file_api_source_path(
                    source_root,
                    build_root,
                    {"path": "render/editor/Ordinary.cpp"},
                    "ordinary source",
                ),
                (source_root / "render/editor/Ordinary.cpp").resolve(),
            )
            self.assertEqual(
                _resolve_file_api_source_path(
                    source_root,
                    build_root,
                    {"path": "generated/app/editor/Configured.cpp", "isGenerated": True},
                    "generated source",
                ),
                (build_root / "generated/app/editor/Configured.cpp").resolve(),
            )
            self.assertEqual(
                _resolve_file_api_source_path(
                    source_root,
                    build_root,
                    {"path": str(absolute), "isGenerated": True},
                    "absolute source",
                ),
                absolute.resolve(),
            )

    def test_relative_source_traversal_is_rejected_for_source_and_build_roots(self) -> None:
        with tempfile.TemporaryDirectory(prefix="mgif-file-api-traversal-unit-") as temporary_directory:
            root = Path(temporary_directory)
            for record in (
                {"path": "../outside.cpp"},
                {"path": "../outside.cpp", "isGenerated": True},
            ):
                with self.subTest(record=record), self.assertRaisesRegex(
                    IdentityError,
                    r"escapes the codemodel top-level (?:source|build) root",
                ):
                    _resolve_file_api_source_path(
                        root / "source",
                        root / "build",
                        record,
                        "hostile source",
                    )


class EditorDependencyGateIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        missing = [tool for tool in ("cmake", "ninja") if shutil.which(tool) is None]
        cls.toolchain_arguments: list[str] = []
        if os.name == "nt":
            program_files = Path(os.environ.get("ProgramFiles", r"C:\Program Files"))
            program_files_x86 = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"))
            compilers = sorted(
                program_files.glob(
                    "Microsoft Visual Studio/*/*/VC/Tools/MSVC/*/bin/Hostx64/x64/cl.exe"
                )
            )
            resource_compilers = sorted(
                program_files_x86.glob("Windows Kits/10/bin/*/x64/rc.exe")
            )
            if not compilers or not resource_compilers:
                missing.append("registered Visual Studio MSVC and Windows SDK")
            else:
                compiler = compilers[-1].as_posix()
                resource_compiler = resource_compilers[-1]
                manifest_tool = resource_compiler.with_name("mt.exe")
                cls.toolchain_arguments = [
                    f"-DCMAKE_CXX_COMPILER={compiler}",
                    f"-DCMAKE_RC_COMPILER={resource_compiler.as_posix()}",
                    f"-DCMAKE_MT={manifest_tool.as_posix()}",
                ]
        elif not any(shutil.which(tool) for tool in ("c++", "clang++", "g++")):
            missing.append("C++ compiler")
        if missing:
            raise unittest.SkipTest("configured dependency integration prerequisites missing: " + ", ".join(missing))

    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory(prefix="mgif-editor-gate-")
        self.case_root = Path(self.temporary_directory.name)
        self.source_root = self.case_root / "source"
        self.source_root.mkdir()

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    @staticmethod
    def final_fence_cmake() -> str:
        fence = textwrap.dedent(
            """
            function(mgif_fixture_collect_compile_targets directory output)
              get_property(local_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
              set(result)
              foreach(target IN LISTS local_targets)
                get_target_property(imported ${target} IMPORTED)
                get_target_property(type ${target} TYPE)
                if(NOT imported AND type MATCHES
                   "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$")
                  list(APPEND result ${target})
                endif()
              endforeach()
              get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
              foreach(child IN LISTS children)
                mgif_fixture_collect_compile_targets("${child}" child_targets)
                list(APPEND result ${child_targets})
              endforeach()
              set(${output} "${result}" PARENT_SCOPE)
            endfunction()

            function(mgif_fixture_finalize_fence)
              set(expected_document_sources
                __DOCUMENT_SOURCES__)
              get_target_property(document_sources scene_editor_document SOURCES)
              if(NOT "${document_sources}" STREQUAL "${expected_document_sources}")
                message(FATAL_ERROR "fixture scene_editor_document source identity changed")
              endif()
              get_target_property(document_definitions scene_editor_document COMPILE_DEFINITIONS)
              get_target_property(document_interface_definitions
                scene_editor_document INTERFACE_COMPILE_DEFINITIONS)
              if(document_interface_definitions)
                message(FATAL_ERROR "fixture testing definition reached document interface")
              endif()
              if(BUILD_TESTING)
                if(NOT "${document_definitions}" STREQUAL "MGIF_EDITOR_TESTING=1")
                  message(FATAL_ERROR "fixture document testing definition changed")
                endif()
              elseif(document_definitions)
                message(FATAL_ERROR "fixture document testing definition survived BUILD_TESTING=OFF")
              endif()

              set(expected_core_sources
                __CORE_SOURCES__)
              get_target_property(core_sources scene_editor_core SOURCES)
              if(NOT "${core_sources}" STREQUAL "${expected_core_sources}")
                message(FATAL_ERROR "fixture scene_editor_core source identity changed")
              endif()
              get_target_property(core_definitions scene_editor_core COMPILE_DEFINITIONS)
              get_target_property(core_interface_definitions
                scene_editor_core INTERFACE_COMPILE_DEFINITIONS)
              if(core_interface_definitions OR
                 "${core_definitions}" MATCHES "MGIF_EDITOR_TESTING")
                message(FATAL_ERROR "fixture testing definition reached scene_editor_core")
              endif()

              mgif_fixture_collect_compile_targets("${CMAKE_SOURCE_DIR}" fenced_targets)
              list(REMOVE_DUPLICATES fenced_targets)
              list(SORT fenced_targets COMPARE STRING CASE INSENSITIVE)
              foreach(target IN LISTS fenced_targets)
                add_dependencies(${target} scene_editor_dependency_preflight)
              endforeach()
              set(DEMO_EDITOR_FENCED_TARGETS "${fenced_targets}" CACHE INTERNAL "" FORCE)
            endfunction()

            function(mgif_fixture_tail_drain)
              get_property(count GLOBAL PROPERTY MGIF_FIXTURE_DRAIN_COUNT)
              if(NOT count)
                set(count 0)
              endif()
              math(EXPR count "${count} + 1")
              if(count GREATER 64)
                message(FATAL_ERROR "fixture deferred-call tail did not stabilize")
              endif()
              set_property(GLOBAL PROPERTY MGIF_FIXTURE_DRAIN_COUNT ${count})
              cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" GET_CALL_IDS pending)
              if(pending)
                cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL mgif_fixture_tail_drain)
                return()
              endif()
              mgif_fixture_finalize_fence()
            endfunction()
            cmake_language(DEFER CALL mgif_fixture_tail_drain)
            """
        ).strip()
        document_sources = "\n                ".join(DOCUMENT_SOURCES)
        core_sources = "\n                ".join(CORE_SOURCES)
        return fence.replace("__DOCUMENT_SOURCES__", document_sources).replace(
            "__CORE_SOURCES__", core_sources
        )

    def write_production_fixture_files(self) -> None:
        files: dict[str, str] = {}
        for index, relative in enumerate((*DOCUMENT_SOURCES, *CORE_SOURCES)):
            if relative == "scene/editor/include/mgif/scene_editor/testing/EditorStateInspector.h":
                contents = textwrap.dedent(
                    """
                    #pragma once
                    #include <cstdint>
                    #if defined(MGIF_EDITOR_TESTING)
                    namespace demo::editor {
                    class EditorStateTestAccess final {
                    public:
                      static constexpr std::uint64_t checkedRevisionSuccessor(
                          std::uint64_t current) noexcept { return current; }
                    };
                    }
                    #endif
                    """
                ).lstrip()
            elif relative.endswith(".h"):
                contents = "#pragma once\n"
            else:
                contents = f"int mgif_fixture_production_anchor_{index} = {index};\n"
            files[relative] = contents
        maximum_call = textwrap.dedent(
            """
            #include <mgif/scene_editor/testing/EditorStateInspector.h>
            #include <cstdint>
            #include <limits>
            int main() {
              return demo::editor::EditorStateTestAccess::checkedRevisionSuccessor(
                         std::numeric_limits<std::uint64_t>::max()) ==
                     std::numeric_limits<std::uint64_t>::max()
                   ? 0 : 1;
            }
            """
        ).lstrip()
        for target in P4_TEST_TARGETS:
            files[f"tests/{target}.cpp"] = maximum_call
        files["tests/non_testing_inspector_include.cpp"] = (
            "#include <mgif/scene_editor/testing/EditorStateInspector.h>\n"
            "int main() { return 0; }\n"
        )
        files["tests/non_testing_forbidden_access.cpp"] = maximum_call
        files["glfw.cpp"] = "int glfw_anchor = 0;\n"
        for relative, contents in files.items():
            path = self.source_root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(contents, encoding="utf-8")

    def write_fixture(self, build_root: Path, *, extra_cmake: str = "") -> Path:
        self.write_production_fixture_files()

        imgui_root = build_root / "_deps/imgui-src"
        imgui_sources = (
            "imgui.cpp",
            "imgui.h",
            "imgui_demo.cpp",
            "imgui_draw.cpp",
            "imgui_tables.cpp",
            "imgui_widgets.cpp",
            "backends/imgui_impl_glfw.cpp",
            "backends/imgui_impl_glfw.h",
        )
        for index, relative in enumerate(imgui_sources):
            path = imgui_root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            if path.suffix == ".h":
                path.write_text("#pragma once\n", encoding="utf-8")
            else:
                path.write_text(f"int imgui_fixture_anchor_{index} = {index};\n", encoding="utf-8")

        validator = VALIDATOR.as_posix()
        document_sources = "\n              ".join(DOCUMENT_SOURCES)
        core_sources = "\n              ".join(CORE_SOURCES)
        p4_targets = "\n              ".join(P4_TEST_TARGETS)
        p4_declarations = "\n".join(
            f"add_executable({target} tests/{target}.cpp)" for target in P4_TEST_TARGETS
        )
        cmake = textwrap.dedent(
            f"""
            cmake_minimum_required(VERSION 3.27)
            cmake_file_api(
              QUERY
              API_VERSION 1
              CODEMODEL 2
              CACHE 2
              TOOLCHAINS 1)
            project(MgifEditorConfiguredGateFixture LANGUAGES CXX)
            include(CTest)
            set(CMAKE_CXX_STANDARD 20)
            set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "" FORCE)
            add_compile_definitions(MGIF_EDITOR_PRODUCTION_COMPILE=1)

            add_library(tinygltf INTERFACE)
            add_library(scene_editor_document STATIC
              {document_sources})
            target_include_directories(scene_editor_document PUBLIC
              "${{CMAKE_CURRENT_SOURCE_DIR}}/scene/editor/include")
            target_link_libraries(scene_editor_document PRIVATE tinygltf)
            if(BUILD_TESTING)
              target_compile_definitions(scene_editor_document PRIVATE MGIF_EDITOR_TESTING=1)
            endif()

            add_library(scene_editor_core STATIC
              {core_sources})
            target_include_directories(scene_editor_core PUBLIC
              "${{CMAKE_CURRENT_SOURCE_DIR}}/scene/editor/include")
            target_link_libraries(scene_editor_core PRIVATE scene_editor_document)

            if(BUILD_TESTING)
              {p4_declarations}
              foreach(p4_target IN ITEMS
                  {p4_targets})
                target_compile_definitions(${{p4_target}} PRIVATE MGIF_EDITOR_TESTING=1)
                target_link_libraries(${{p4_target}} PRIVATE scene_editor_core)
              endforeach()
            else()
              add_executable(non_testing_inspector_include
                tests/non_testing_inspector_include.cpp)
              target_link_libraries(non_testing_inspector_include PRIVATE scene_editor_core)
              add_executable(non_testing_forbidden_access EXCLUDE_FROM_ALL
                tests/non_testing_forbidden_access.cpp)
              target_link_libraries(non_testing_forbidden_access PRIVATE scene_editor_core)
            endif()

            add_library(glfw STATIC glfw.cpp)
            set(imgui_SOURCE_DIR "${{CMAKE_BINARY_DIR}}/_deps/imgui-src")
            add_library(imgui_demo STATIC
              "${{imgui_SOURCE_DIR}}/imgui.cpp"
              "${{imgui_SOURCE_DIR}}/imgui.h"
              "${{imgui_SOURCE_DIR}}/imgui_demo.cpp"
              "${{imgui_SOURCE_DIR}}/imgui_draw.cpp"
              "${{imgui_SOURCE_DIR}}/imgui_tables.cpp"
              "${{imgui_SOURCE_DIR}}/imgui_widgets.cpp"
              "${{imgui_SOURCE_DIR}}/backends/imgui_impl_glfw.cpp"
              "${{imgui_SOURCE_DIR}}/backends/imgui_impl_glfw.h")
            target_include_directories(imgui_demo PUBLIC
              "${{imgui_SOURCE_DIR}}" "${{imgui_SOURCE_DIR}}/backends")
            target_link_libraries(imgui_demo PUBLIC glfw)

            find_package(Python3 REQUIRED COMPONENTS Interpreter)
            add_custom_target(scene_editor_dependency_preflight
              COMMAND "${{Python3_EXECUTABLE}}" "{validator}"
                --repo-root "${{CMAKE_CURRENT_SOURCE_DIR}}"
                --mode configured-dependency-preflight
                --configured-build-root "${{CMAKE_BINARY_DIR}}"
                --config "$<CONFIG>"
              VERBATIM USES_TERMINAL)
            add_custom_target(engineering_gate
              COMMAND "${{Python3_EXECUTABLE}}" "{validator}"
                --repo-root "${{CMAKE_CURRENT_SOURCE_DIR}}"
                --mode configured-dependency
                --configured-build-root "${{CMAKE_BINARY_DIR}}"
                --config "$<CONFIG>"
              DEPENDS scene_editor_core imgui_demo
              VERBATIM USES_TERMINAL)
            {extra_cmake}
            {self.final_fence_cmake()}
            """
        ).strip()
        (self.source_root / "CMakeLists.txt").write_text(cmake + "\n", encoding="utf-8")
        return imgui_root

    def write_core_only_fixture(self) -> None:
        self.write_production_fixture_files()
        validator = VALIDATOR.as_posix()
        document_sources = "\n              ".join(DOCUMENT_SOURCES)
        core_sources = "\n              ".join(CORE_SOURCES)
        cmake = textwrap.dedent(
            f"""
            cmake_minimum_required(VERSION 3.27)
            cmake_file_api(
              QUERY
              API_VERSION 1
              CODEMODEL 2
              CACHE 2
              TOOLCHAINS 1)
            project(MgifEditorCoreConfiguredGateFixture LANGUAGES CXX)
            set(CMAKE_CXX_STANDARD 20)
            set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "" FORCE)
            set(DEMO_EDITOR_CORE_ONLY ON CACHE BOOL "" FORCE)
            add_compile_definitions(MGIF_EDITOR_PRODUCTION_COMPILE=1)

            add_library(tinygltf INTERFACE)
            add_library(scene_editor_document STATIC
              {document_sources})
            target_include_directories(scene_editor_document PUBLIC
              "${{CMAKE_CURRENT_SOURCE_DIR}}/scene/editor/include")
            target_link_libraries(scene_editor_document PRIVATE tinygltf)
            add_library(scene_editor_core STATIC
              {core_sources})
            target_include_directories(scene_editor_core PUBLIC
              "${{CMAKE_CURRENT_SOURCE_DIR}}/scene/editor/include")
            target_link_libraries(scene_editor_core PRIVATE scene_editor_document)
            find_package(Python3 REQUIRED COMPONENTS Interpreter)
            add_custom_target(scene_editor_dependency_preflight
              COMMAND "${{Python3_EXECUTABLE}}" "{validator}"
                --repo-root "${{CMAKE_CURRENT_SOURCE_DIR}}"
                --mode configured-dependency-preflight
                --configured-build-root "${{CMAKE_BINARY_DIR}}"
                --config "$<CONFIG>"
              VERBATIM USES_TERMINAL)
            {self.final_fence_cmake()}
            """
        ).strip()
        (self.source_root / "CMakeLists.txt").write_text(cmake + "\n", encoding="utf-8")

    def run_command(
        self,
        arguments: list[str],
        *,
        environment: dict[str, str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            arguments,
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=environment,
        )

    def configure(
        self,
        build_root: Path,
        *,
        extra_arguments: list[str] | None = None,
        environment: dict[str, str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        return self.run_command(
            [
                "cmake",
                "-S",
                str(self.source_root),
                "-B",
                str(build_root),
                "-G",
                "Ninja",
                "-DCMAKE_BUILD_TYPE=Debug",
                *self.toolchain_arguments,
                *(extra_arguments or []),
            ],
            environment=environment,
        )

    @staticmethod
    def configured_targets(build_root: Path, configuration_name: str) -> dict[str, dict[str, object]]:
        reply_root = build_root / ".cmake/api/v1/reply"
        index_path = sorted(reply_root.glob("index-*.json"))[-1]
        index = json.loads(index_path.read_text(encoding="utf-8"))
        codemodel_reference = next(
            record for record in index["objects"] if record.get("kind") == "codemodel"
        )
        codemodel = json.loads(
            (reply_root / codemodel_reference["jsonFile"]).read_text(encoding="utf-8")
        )
        configuration = next(
            record
            for record in codemodel["configurations"]
            if record.get("name") == configuration_name
        )
        return {
            reference["name"]: json.loads(
                (reply_root / reference["jsonFile"]).read_text(encoding="utf-8")
            )
            for reference in configuration["targets"]
        }

    @staticmethod
    def target_definitions(target: dict[str, object]) -> set[str]:
        return {
            str(record["define"])
            for group in target.get("compileGroups", [])
            for record in group.get("defines", [])
        }

    def build_gate(self, build_root: Path) -> subprocess.CompletedProcess[str]:
        return self.run_command(
            ["cmake", "--build", str(build_root), "--target", "engineering_gate", "--verbose"]
        )

    def build_target(self, build_root: Path, target: str) -> subprocess.CompletedProcess[str]:
        return self.run_command(["cmake", "--build", str(build_root), "--target", target, "--verbose"])

    def assert_gate_rejected(self, build: subprocess.CompletedProcess[str], case: str) -> None:
        output = build.stdout + build.stderr
        self.assertNotEqual(build.returncode, 0, f"hostile configured gate unexpectedly passed: {case}\n{output}")
        self.assertRegex(output, r"(?i)(configured|dependency|imgui_demo|injection channel|forbidden)")

    def test_clean_build_and_reconfigure_pass(self) -> None:
        build_root = self.case_root / "clean-build"
        self.write_fixture(build_root)
        first_configure = self.configure(build_root)
        self.assertEqual(first_configure.returncode, 0, first_configure.stdout + first_configure.stderr)
        first_build = self.build_gate(build_root)
        self.assertEqual(first_build.returncode, 0, first_build.stdout + first_build.stderr)
        self.assertIn("Final configured production-editor dependency gate passed", first_build.stdout)

        second_configure = self.configure(build_root)
        self.assertEqual(second_configure.returncode, 0, second_configure.stdout + second_configure.stderr)
        second_build = self.build_gate(build_root)
        self.assertEqual(second_build.returncode, 0, second_build.stdout + second_build.stderr)

        targets = self.configured_targets(build_root, "Debug")
        compile_database = json.loads((build_root / "compile_commands.json").read_text(encoding="utf-8"))

        def entry_source(entry: dict[str, object]) -> Path:
            source = Path(str(entry["file"]))
            if not source.is_absolute():
                source = Path(str(entry["directory"])) / source
            return source.resolve()

        for target_name, expected_all, expected_compiled in (
            ("scene_editor_document", DOCUMENT_SOURCES, DOCUMENT_COMPILED_SOURCES),
            ("scene_editor_core", CORE_SOURCES, CORE_COMPILED_SOURCES),
        ):
            with self.subTest(target=target_name):
                target = targets[target_name]
                file_api_all = [
                    _resolve_file_api_source_path(
                        self.source_root,
                        build_root,
                        record,
                        f"configured target {target_name} source",
                    )
                    for record in target["sources"]
                ]
                file_api_compiled = [
                    _resolve_file_api_source_path(
                        self.source_root,
                        build_root,
                        record,
                        f"configured target {target_name} compiled source",
                    )
                    for record in target["sources"]
                    if "compileGroupIndex" in record
                ]
                expected_all_paths = [(self.source_root / value).resolve() for value in expected_all]
                expected_compiled_paths = {
                    (self.source_root / value).resolve() for value in expected_compiled
                }
                self.assertEqual(file_api_all, expected_all_paths)
                self.assertEqual(set(file_api_compiled), expected_compiled_paths)
                marker = f"/cmakefiles/{target_name}.dir/"
                database_sources = {
                    entry_source(entry)
                    for entry in compile_database
                    if marker
                    in ("/" + str(entry.get("output", "")).replace("\\", "/").lower().lstrip("/"))
                }
                self.assertEqual(database_sources, expected_compiled_paths)
                ninja = self.run_command(
                    ["ninja", "-C", str(build_root), "-t", "compdb-targets", target_name]
                )
                self.assertEqual(ninja.returncode, 0, ninja.stdout + ninja.stderr)
                ninja_sources = {
                    entry_source(entry)
                    for entry in json.loads(ninja.stdout)
                    if marker
                    in ("/" + str(entry.get("output", "")).replace("\\", "/").lower().lstrip("/"))
                }
                self.assertEqual(ninja_sources, expected_compiled_paths)

        self.assertIn("MGIF_EDITOR_TESTING=1", self.target_definitions(targets["scene_editor_document"]))
        self.assertNotIn("MGIF_EDITOR_TESTING=1", self.target_definitions(targets["scene_editor_core"]))
        for target_name in P4_TEST_TARGETS:
            self.assertIn("MGIF_EDITOR_TESTING=1", self.target_definitions(targets[target_name]))

        release_root = self.case_root / "release-testing-build"
        self.write_fixture(release_root)
        release_configure = self.configure(
            release_root,
            extra_arguments=["-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=ON"],
        )
        self.assertEqual(
            release_configure.returncode,
            0,
            release_configure.stdout + release_configure.stderr,
        )
        release_build = self.build_target(release_root, "editor_history_core_tests")
        self.assertEqual(release_build.returncode, 0, release_build.stdout + release_build.stderr)
        release_run = self.run_command([str(release_root / "editor_history_core_tests.exe")])
        self.assertEqual(release_run.returncode, 0, release_run.stdout + release_run.stderr)
        release_targets = self.configured_targets(release_root, "Release")
        self.assertIn(
            "MGIF_EDITOR_TESTING=1",
            self.target_definitions(release_targets["editor_history_core_tests"]),
        )

        no_test_root = self.case_root / "no-testing-build"
        self.write_fixture(no_test_root)
        no_test_configure = self.configure(
            no_test_root,
            extra_arguments=["-DBUILD_TESTING=OFF"],
        )
        self.assertEqual(
            no_test_configure.returncode,
            0,
            no_test_configure.stdout + no_test_configure.stderr,
        )
        no_test_document = self.build_target(no_test_root, "scene_editor_document")
        self.assertEqual(
            no_test_document.returncode,
            0,
            no_test_document.stdout + no_test_document.stderr,
        )
        no_test_include = self.build_target(no_test_root, "non_testing_inspector_include")
        self.assertEqual(no_test_include.returncode, 0, no_test_include.stdout + no_test_include.stderr)
        no_test_forbidden = self.build_target(no_test_root, "non_testing_forbidden_access")
        self.assertNotEqual(no_test_forbidden.returncode, 0)
        no_test_failure = no_test_forbidden.stdout + no_test_forbidden.stderr
        self.assertIn("non_testing_forbidden_access.cpp", no_test_failure)
        self.assertNotIn("-DMGIF_EDITOR_TESTING=1", no_test_failure)
        no_test_targets = self.configured_targets(no_test_root, "Debug")
        self.assertTrue(set(P4_TEST_TARGETS).isdisjoint(no_test_targets))
        for target in no_test_targets.values():
            self.assertNotIn("MGIF_EDITOR_TESTING=1", self.target_definitions(target))

    def test_add_subdirectory_sources_use_codemodel_roots_with_exact_compile_ancestry(self) -> None:
        build_root = self.case_root / "add-subdirectory-source-semantics-build"
        target_name = "subdirectory_neutral_runtime"
        source_contents = {
            "scene/editor/TopLevelRelative.cpp": "int top_level_relative_editor_source = 0;\n",
            "sub/editor/TargetRelative.cpp": "int target_relative_editor_source = 0;\n",
            "render/editor/AbsoluteSpelling.cpp": "int absolute_spelling_editor_source = 0;\n",
        }
        for relative, contents in source_contents.items():
            source = self.source_root / relative
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text(contents, encoding="utf-8")
        (self.source_root / "sub/CMakeLists.txt").write_text(
            textwrap.dedent(
                f"""
                add_library({target_name} STATIC
                  editor/TargetRelative.cpp
                  "${{CMAKE_SOURCE_DIR}}/render/editor/AbsoluteSpelling.cpp")
                """
            ).strip()
            + "\n",
            encoding="utf-8",
        )
        self.write_fixture(
            build_root,
            extra_cmake=textwrap.dedent(
                f"""
                add_subdirectory(sub)
                target_sources({target_name} PRIVATE
                  scene/editor/TopLevelRelative.cpp)
                """
            ).strip(),
        )

        configured = self.configure(build_root)
        self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)

        reply_root = build_root / ".cmake/api/v1/reply"
        index_path = sorted(reply_root.glob("index-*.json"))[-1]
        index = json.loads(index_path.read_text(encoding="utf-8"))
        codemodel_reference = next(record for record in index["objects"] if record.get("kind") == "codemodel")
        codemodel = json.loads((reply_root / codemodel_reference["jsonFile"]).read_text(encoding="utf-8"))
        configuration = next(record for record in codemodel["configurations"] if record.get("name") == "Debug")
        target_reference = next(record for record in configuration["targets"] if record.get("name") == target_name)
        target = json.loads((reply_root / target_reference["jsonFile"]).read_text(encoding="utf-8"))
        self.assertEqual(target["paths"]["source"].replace("\\", "/"), "sub")

        compiled_records = [record for record in target["sources"] if "compileGroupIndex" in record]
        expected_record_paths = {
            "scene/editor/TopLevelRelative.cpp",
            "sub/editor/TargetRelative.cpp",
            "render/editor/AbsoluteSpelling.cpp",
        }
        self.assertEqual({record["path"].replace("\\", "/") for record in compiled_records}, expected_record_paths)
        expected_sources = {(self.source_root / relative).resolve() for relative in source_contents}
        file_api_sources = {
            _resolve_file_api_source_path(
                self.source_root,
                build_root,
                record,
                f"configured target {target_name} source",
            )
            for record in compiled_records
        }
        self.assertEqual(len(compiled_records), len(expected_sources))
        self.assertEqual(file_api_sources, expected_sources)

        def entry_source(entry: dict[str, object]) -> Path:
            source = Path(str(entry["file"]))
            if not source.is_absolute():
                source = Path(str(entry["directory"])) / source
            return source.resolve()

        def entry_key(entry: dict[str, object]) -> tuple[Path, str]:
            output = "/" + str(entry["output"]).replace("\\", "/").lower().lstrip("/")
            return entry_source(entry), output[output.index(marker) :]

        marker = f"/cmakefiles/{target_name.lower()}.dir/"
        compile_database = json.loads((build_root / "compile_commands.json").read_text(encoding="utf-8"))
        compile_entries = [
            entry
            for entry in compile_database
            if marker in ("/" + str(entry.get("output", "")).replace("\\", "/").lower().lstrip("/"))
        ]
        self.assertEqual(len(compile_entries), len(expected_sources))
        self.assertEqual({entry_source(entry) for entry in compile_entries}, expected_sources)
        self.assertEqual(len({entry_key(entry) for entry in compile_entries}), len(expected_sources))

        ninja_compdb = self.run_command(
            ["ninja", "-C", str(build_root), "-t", "compdb-targets", target_name]
        )
        self.assertEqual(ninja_compdb.returncode, 0, ninja_compdb.stdout + ninja_compdb.stderr)
        ninja_entries = [
            entry
            for entry in json.loads(ninja_compdb.stdout)
            if marker in ("/" + str(entry.get("output", "")).replace("\\", "/").lower().lstrip("/"))
        ]
        self.assertEqual(len(ninja_entries), len(expected_sources))
        self.assertEqual({entry_source(entry) for entry in ninja_entries}, expected_sources)
        self.assertEqual({entry_key(entry) for entry in ninja_entries}, {entry_key(entry) for entry in compile_entries})

        outputs = [str(entry["output"]) for entry in ninja_entries]
        ancestry_result = self.run_command(
            ["ninja", "-C", str(build_root), "-t", "multi-inputs", *outputs]
        )
        self.assertEqual(ancestry_result.returncode, 0, ancestry_result.stdout + ancestry_result.stderr)
        ancestry = {output.replace("\\", "/").lower(): [] for output in outputs}
        for line in ancestry_result.stdout.splitlines():
            output, separator, dependency = line.partition("\t")
            self.assertTrue(separator, f"unexpected Ninja multi-inputs record: {line!r}")
            key = output.replace("\\", "/").lower()
            self.assertIn(key, ancestry)
            ancestry[key].append(dependency)
        for output, dependencies in ancestry.items():
            self.assertTrue(
                any(
                    dependency.replace("\\", "/").lower().rstrip("/").endswith(
                        "cmakefiles/scene_editor_dependency_preflight"
                    )
                    for dependency in dependencies
                ),
                f"compile edge lacks real preflight ancestry: {output}",
            )

        built = self.build_target(build_root, target_name)
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        self.assertIn("Final configured production-editor dependency preflight passed", built.stdout)
        gated = self.build_gate(build_root)
        self.assertEqual(gated.returncode, 0, gated.stdout + gated.stderr)
        self.assertIn("Final configured production-editor dependency gate passed", gated.stdout)

    def test_ninja_owned_response_content_is_expanded_and_accepted(self) -> None:
        build_root = self.case_root / "ninja-response-build"
        self.write_fixture(build_root)
        configured = self.configure(
            build_root,
            extra_arguments=["-DCMAKE_NINJA_FORCE_RESPONSE_FILE=ON"],
        )
        self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
        raw = self.run_command(
            ["ninja", "-C", str(build_root), "-t", "compdb-targets", "scene_editor_core"]
        )
        expanded = self.run_command(
            ["ninja", "-C", str(build_root), "-t", "compdb-targets", "-x", "scene_editor_core"]
        )
        self.assertEqual(raw.returncode, 0, raw.stdout + raw.stderr)
        self.assertEqual(expanded.returncode, 0, expanded.stdout + expanded.stderr)
        self.assertIn("@CMakeFiles", raw.stdout)
        self.assertNotIn("@CMakeFiles", expanded.stdout)
        built = self.build_gate(build_root)
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        self.assertIn("Final configured production-editor dependency gate passed", built.stdout)

    @unittest.skipUnless(os.name == "nt", "real nvpro_core2 response replay is Windows-specific")
    def test_external_and_nested_response_files_fail_before_real_hostile_header_compiles(self) -> None:
        hostile_header = Path(r"F:\GitHub\nvpro_core2\nvvk\check_error.hpp")
        if not hostile_header.is_file():
            self.skipTest(f"real hostile audit header is unavailable: {hostile_header}")
        for case in ("cache-response", "nested-target-response"):
            with self.subTest(case=case):
                build_root = self.case_root / f"{case}-build"
                build_root.mkdir()
                forced_header = self.case_root / f"{case}-forced.hpp"
                forced_header.write_text(
                    "#pragma once\n"
                    "#if defined(MGIF_EDITOR_PRODUCTION_COMPILE)\n"
                    '#include "F:/GitHub/nvpro_core2/nvvk/check_error.hpp"\n'
                    "#ifndef NVVK_CHECK\n"
                    "#error MGIF_REAL_HOSTILE_HEADER_NOT_PARSED\n"
                    "#endif\n"
                    "#error MGIF_RESPONSE_FILE_COMPILED_BEFORE_GATE\n"
                    "#endif\n",
                    encoding="utf-8",
                )
                nested = build_root / "nested options.rsp"
                nested.write_text(f'/FI"{forced_header.as_posix()}"\n', encoding="utf-8-sig")
                outer = build_root / "outer-options.rsp"
                outer.write_text(f'@"{nested.as_posix()}"\n', encoding="utf-8")
                if case == "cache-response":
                    self.write_fixture(build_root)
                    extra_arguments = [f"-DCMAKE_CXX_FLAGS=@{outer.as_posix()}"]
                else:
                    self.write_fixture(
                        build_root,
                        extra_cmake=f'target_compile_options(scene_editor_core PRIVATE "@{outer.as_posix()}")',
                    )
                    extra_arguments = []
                configured = self.configure(build_root, extra_arguments=extra_arguments)
                self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
                built = self.build_target(build_root, "scene_editor_core")
                self.assert_gate_rejected(built, case)
                output = built.stdout + built.stderr
                self.assertRegex(output, r"(?i)(nvpro_core2|nvvk|response file)")
                self.assertNotIn("MGIF_RESPONSE_FILE_COMPILED_BEFORE_GATE", output)
                self.assertNotIn("MGIF_REAL_HOSTILE_HEADER_NOT_PARSED", output)

    @unittest.skipUnless(os.name == "nt", "MSVC masquerading wrapper replay is Windows-specific")
    def test_masquerading_same_basename_compiler_is_rejected_before_it_runs_production_compile(self) -> None:
        real_compiler = Path(self.toolchain_arguments[0].split("=", 1)[1])
        fake_root = self.case_root / "VC/Tools/MSVC/audit/bin"
        fake_root.mkdir(parents=True)
        forced_header = self.case_root / "wrapper-forced.hpp"
        forced_header.write_text(
            "#pragma once\n"
            "#if defined(MGIF_EDITOR_PRODUCTION_COMPILE)\n"
            "#error MGIF_MASQUERADING_COMPILER_REACHED_PRODUCTION_COMPILE\n"
            "#endif\n",
            encoding="utf-8",
        )
        response = self.case_root / "wrapper-options.rsp"
        response.write_text(f'/FI"{forced_header.as_posix()}"\n', encoding="utf-8")
        wrapper_source = fake_root / "cl-wrapper.cpp"
        wrapper_source.write_text(
            textwrap.dedent(
                f'''
                #include <process.h>
                #include <vector>
                int wmain(int argc, wchar_t** argv) {{
                  static const wchar_t real[] = LR"({real_compiler})";
                  static const wchar_t response[] = LR"(@{response})";
                  std::vector<const wchar_t*> forwarded;
                  forwarded.push_back(L"cl.exe");
                  for(int index = 1; index < argc; ++index) forwarded.push_back(argv[index]);
                  forwarded.push_back(response);
                  forwarded.push_back(nullptr);
                  return static_cast<int>(_wspawnv(_P_WAIT, real, forwarded.data()));
                }}
                '''
            ).strip()
            + "\n",
            encoding="utf-8",
        )
        fake_compiler = fake_root / "cl.exe"
        compiled = self.run_command(
            [
                str(real_compiler),
                "/nologo",
                "/EHsc",
                str(wrapper_source),
                f"/Fe{fake_compiler}",
                f"/Fo{fake_root / 'cl-wrapper.obj'}",
            ]
        )
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)

        build_root = self.case_root / "wrapper-build"
        self.write_core_only_fixture()
        configured = self.configure(
            build_root,
            extra_arguments=[
                f"-DCMAKE_CXX_COMPILER={fake_compiler.as_posix()}",
                f"-DCMAKE_LINKER={real_compiler.with_name('link.exe').as_posix()}",
            ],
        )
        self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
        built = self.build_target(build_root, "scene_editor_core")
        self.assert_gate_rejected(built, "masquerading-compiler")
        output = built.stdout + built.stderr
        self.assertRegex(output, r"(?i)(registered by vswhere|authenticode|trusted compiler)")
        self.assertNotIn("MGIF_MASQUERADING_COMPILER_REACHED_PRODUCTION_COMPILE", output)

    @unittest.skipUnless(os.name == "nt", "MSVC launcher wrapper replay is Windows-specific")
    def test_compiler_launcher_is_rejected_before_it_runs_production_compile(self) -> None:
        real_compiler = Path(self.toolchain_arguments[0].split("=", 1)[1])
        launcher_root = self.case_root / "launcher"
        launcher_root.mkdir()
        forced_header = self.case_root / "launcher-forced.hpp"
        forced_header.write_text(
            "#pragma once\n"
            "#if defined(MGIF_EDITOR_PRODUCTION_COMPILE)\n"
            "#error MGIF_COMPILER_LAUNCHER_REACHED_PRODUCTION_COMPILE\n"
            "#endif\n",
            encoding="utf-8",
        )
        response = self.case_root / "launcher-options.rsp"
        response.write_text(f'/FI"{forced_header.as_posix()}"\n', encoding="utf-8")
        launcher_source = launcher_root / "launcher.cpp"
        launcher_source.write_text(
            textwrap.dedent(
                f'''
                #include <process.h>
                #include <vector>
                int wmain(int argc, wchar_t** argv) {{
                  static const wchar_t response[] = LR"(@{response})";
                  std::vector<const wchar_t*> forwarded;
                  for(int index = 1; index < argc; ++index) forwarded.push_back(argv[index]);
                  forwarded.push_back(response);
                  forwarded.push_back(nullptr);
                  return static_cast<int>(_wspawnv(_P_WAIT, argv[1], forwarded.data()));
                }}
                '''
            ).strip()
            + "\n",
            encoding="utf-8",
        )
        launcher = launcher_root / "launcher.exe"
        compiled = self.run_command(
            [
                str(real_compiler),
                "/nologo",
                "/EHsc",
                str(launcher_source),
                f"/Fe{launcher}",
                f"/Fo{launcher_root / 'launcher.obj'}",
            ]
        )
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)

        build_root = self.case_root / "launcher-build"
        self.write_core_only_fixture()
        configured = self.configure(
            build_root,
            extra_arguments=[f"-DCMAKE_CXX_COMPILER_LAUNCHER={launcher.as_posix()}"],
        )
        self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
        built = self.build_target(build_root, "scene_editor_core")
        self.assert_gate_rejected(built, "compiler-launcher")
        output = built.stdout + built.stderr
        self.assertRegex(output, r"(?i)(CMAKE_CXX_COMPILER_LAUNCHER|compiler/launcher)")
        self.assertNotIn("MGIF_COMPILER_LAUNCHER_REACHED_PRODUCTION_COMPILE", output)

    def test_future_editor_targets_objects_aliases_generated_and_deferred_sources_are_fenced(self) -> None:
        build_root = self.case_root / "future-targets-build"
        sources = {
            "app/editor/FutureAppEditor.cpp": "int future_app_editor = 0;\n",
            "render/editor/FutureRenderEditor.cpp": "int future_render_editor = 0;\n",
            "scene/editor/FutureObject.cpp": "int future_object = 0;\n",
            "app/editor/LateAttached.cpp": "int late_attached = 0;\n",
            "app/editor/DeferredEditor.cpp": "int deferred_editor = 0;\n",
            "neutral.cpp": "int neutral_editor_alias_target = 0;\n",
            "adapter.cpp": "int app_scene_adapter = 0;\n",
            "consumer.cpp": "int editor_consumer = 0;\n",
        }
        for relative, contents in sources.items():
            path = self.source_root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(contents, encoding="utf-8")
        extra_cmake = textwrap.dedent(
            """
            add_library(future_app_editor STATIC app/editor/FutureAppEditor.cpp)
            add_library(future_render_editor STATIC render/editor/FutureRenderEditor.cpp)
            add_library(future_editor_objects OBJECT scene/editor/FutureObject.cpp)
            add_library(neutral_editor_alias_target STATIC neutral.cpp $<TARGET_OBJECTS:future_editor_objects>)
            add_library(MGIF::FutureEditorAlias ALIAS neutral_editor_alias_target)
            add_library(late_neutral_target STATIC neutral.cpp)
            target_sources(late_neutral_target PRIVATE app/editor/LateAttached.cpp)
            file(WRITE "${CMAKE_BINARY_DIR}/ConfiguredEditor.cpp" "int configured_editor = 0;\\n")
            add_library(configured_editor_generated STATIC "${CMAKE_BINARY_DIR}/ConfiguredEditor.cpp")
            add_library(app_scene_adapter STATIC adapter.cpp)
            target_link_libraries(app_scene_adapter PUBLIC future_app_editor)
            add_library(editor_production_consumer STATIC consumer.cpp)
            target_link_libraries(editor_production_consumer PUBLIC app_scene_adapter)
            function(add_deferred_editor_target)
              add_library(deferred_editor_target STATIC app/editor/DeferredEditor.cpp)
            endfunction()
            cmake_language(DEFER CALL add_deferred_editor_target)
            """
        )
        self.write_fixture(build_root, extra_cmake=extra_cmake)
        configured = self.configure(build_root)
        self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
        expected = {
            "configured_editor_generated",
            "deferred_editor_target",
            "editor_production_consumer",
            "future_app_editor",
            "future_editor_objects",
            "future_render_editor",
            "late_neutral_target",
            "neutral_editor_alias_target",
        }
        cache = (build_root / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace")
        census_line = next(line for line in cache.splitlines() if line.startswith("DEMO_EDITOR_FENCED_TARGETS:INTERNAL="))
        census = set(census_line.split("=", 1)[1].split(";"))
        self.assertTrue(expected <= census)
        for target in sorted(expected):
            with self.subTest(target=target):
                built = self.build_target(build_root, target)
                self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                self.assertIn("Final configured production-editor dependency preflight passed", built.stdout)

    def test_build_time_generated_editor_source_is_rejected_before_generator_or_compiler(self) -> None:
        build_root = self.case_root / "build-generated-negative"
        sentinel = build_root / "BUILD_GENERATOR_RAN.txt"
        extra_cmake = textwrap.dedent(
            f"""
            add_custom_command(
              OUTPUT "${{CMAKE_BINARY_DIR}}/app/editor/BuildGenerated.cpp"
              COMMAND "${{CMAKE_COMMAND}}" -E make_directory "${{CMAKE_BINARY_DIR}}/app/editor"
              COMMAND "${{CMAKE_COMMAND}}" -E touch "{sentinel.as_posix()}"
              COMMAND "${{CMAKE_COMMAND}}" -E touch "${{CMAKE_BINARY_DIR}}/app/editor/BuildGenerated.cpp"
              VERBATIM)
            add_library(build_generated_editor STATIC
              "${{CMAKE_BINARY_DIR}}/app/editor/BuildGenerated.cpp")
            """
        )
        self.write_fixture(build_root, extra_cmake=extra_cmake)
        configured = self.configure(build_root)
        self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
        built = self.build_target(build_root, "build_generated_editor")
        self.assert_gate_rejected(built, "build-time-generated-editor-source")
        self.assertRegex(built.stdout + built.stderr, r"(?i)(generated|source is missing|before preflight)")
        self.assertFalse(sentinel.exists(), "build-time source generator ran before the dependency gate")

    def test_reported_hostile_configurations_fail_the_engineering_gate(self) -> None:
        source_identity_cases = {
            "omitted-document-source": (
                "set_property(TARGET scene_editor_document PROPERTY SOURCES\n  "
                + "\n  ".join(DOCUMENT_SOURCES[:-1])
                + ")"
            ),
            "omitted-core-source": (
                "set_property(TARGET scene_editor_core PROPERTY SOURCES\n  "
                + "\n  ".join(CORE_SOURCES[:-1])
                + ")"
            ),
            "reordered-document-sources": (
                "set_property(TARGET scene_editor_document PROPERTY SOURCES\n  "
                + "\n  ".join((DOCUMENT_SOURCES[1], DOCUMENT_SOURCES[0], *DOCUMENT_SOURCES[2:]))
                + ")"
            ),
            "reordered-core-sources": (
                "set_property(TARGET scene_editor_core PROPERTY SOURCES\n  "
                + "\n  ".join((CORE_SOURCES[1], CORE_SOURCES[0], *CORE_SOURCES[2:]))
                + ")"
            ),
            "appended-document-source": (
                "target_sources(scene_editor_document PRIVATE scene/editor/HiddenDocument.cpp)"
            ),
            "generator-expression-source": (
                'target_sources(scene_editor_core PRIVATE '
                '"$<$<BOOL:0>:scene/editor/HiddenDocument.cpp>")'
            ),
        }
        cases = (
            "cxxflags",
            "cache-flags",
            "deferred-wrapper",
            "generated-traversal",
            "late-imgui-source",
            *source_identity_cases,
            "compiled-outside-declared-target",
        )
        for case in cases:
            with self.subTest(case=case):
                build_root = self.case_root / f"{case}-build"
                if case in source_identity_cases:
                    hidden = self.source_root / "scene/editor/HiddenDocument.cpp"
                    hidden.parent.mkdir(parents=True, exist_ok=True)
                    hidden.write_text("int hidden_document_anchor = 0;\n", encoding="utf-8")
                    self.write_fixture(build_root, extra_cmake=source_identity_cases[case])
                    configured = self.configure(build_root)
                    self.assertNotEqual(
                        configured.returncode,
                        0,
                        f"source identity mutation unexpectedly configured: {case}\n"
                        + configured.stdout
                        + configured.stderr,
                    )
                    self.assertRegex(
                        configured.stdout + configured.stderr,
                        r"(?i)(source identity|SOURCES differ)",
                    )
                    continue
                if case == "compiled-outside-declared-target":
                    self.write_fixture(
                        build_root,
                        extra_cmake=(
                            "add_library(hidden_document_compiler STATIC "
                            "scene/editor/GltfDocument.cpp)"
                        ),
                    )
                    configured = self.configure(build_root)
                    self.assertEqual(
                        configured.returncode,
                        0,
                        configured.stdout + configured.stderr,
                    )
                    built = self.build_gate(build_root)
                    self.assert_gate_rejected(built, case)
                    self.assertRegex(
                        built.stdout + built.stderr,
                        r"(?i)compiled outside its declared target",
                    )
                    continue

                self.write_fixture(build_root)
                forced_header = self.case_root / f"{case}-forced.hpp"
                forced_header.write_text(
                    "#pragma once\n"
                    "#if defined(MGIF_EDITOR_PRODUCTION_COMPILE)\n"
                    "#error MGIF_HOSTILE_HEADER_COMPILED_BEFORE_DEPENDENCY_GATE\n"
                    "#endif\n"
                    "// nvvk configured dependency\n",
                    encoding="utf-8",
                )
                forced_option = ("/FI" if os.name == "nt" else "-include") + forced_header.as_posix()
                extra_arguments: list[str] = []
                environment = os.environ.copy()

                if case == "cxxflags":
                    environment["CXXFLAGS"] = forced_option
                elif case == "cache-flags":
                    extra_arguments.append(f"-DCMAKE_CXX_FLAGS={forced_option}")
                else:
                    injection = self.case_root / f"{case}.cmake"
                    if case == "deferred-wrapper":
                        contents = f"""
                        function(mgif_inject_forbidden_graph)
                          add_library(nvpro_core_imported INTERFACE IMPORTED GLOBAL)
                          set_target_properties(nvpro_core_imported PROPERTIES
                            INTERFACE_COMPILE_OPTIONS "{forced_option}")
                          add_library(NVPRO::core ALIAS nvpro_core_imported)
                          add_library(mgif_owned_adapter INTERFACE)
                          target_link_libraries(mgif_owned_adapter INTERFACE "$<$<BOOL:1>:NVPRO::core>")
                          target_link_libraries(scene_editor_core PRIVATE mgif_owned_adapter)
                        endfunction()
                        cmake_language(DEFER CALL mgif_inject_forbidden_graph)
                        """
                    elif case == "generated-traversal":
                        contents = f"""
                        file(WRITE "${{CMAKE_BINARY_DIR}}/generated-edge.cmake"
                          "set(CMAKE_CXX_FLAGS \"${{CMAKE_CXX_FLAGS}} {forced_option}\")\n")
                        include("${{CMAKE_BINARY_DIR}}/generated-edge.cmake")
                        file(MAKE_DIRECTORY "${{CMAKE_BINARY_DIR}}/generated-subdir")
                        file(WRITE "${{CMAKE_BINARY_DIR}}/generated-subdir/CMakeLists.txt"
                          "add_library(nvapp_generated_wrapper INTERFACE)\n"
                          "target_compile_definitions(nvapp_generated_wrapper INTERFACE NVAPP_GENERATED_EDGE=1)\n")
                        add_subdirectory("${{CMAKE_BINARY_DIR}}/generated-subdir"
                          "${{CMAKE_BINARY_DIR}}/generated-subdir-build")
                        function(mgif_attach_generated_wrapper)
                          target_link_libraries(scene_editor_core PRIVATE
                            "$<$<BOOL:1>:nvapp_generated_wrapper>")
                        endfunction()
                        cmake_language(DEFER CALL mgif_attach_generated_wrapper)
                        """
                    else:
                        late_source = self.case_root / "late-imgui-source.cpp"
                        late_source.write_text("int late_imgui_source = 0;\n", encoding="utf-8")
                        contents = f"""
                        function(mgif_inject_late_imgui_source)
                          target_sources(imgui_demo PRIVATE "{late_source.as_posix()}")
                        endfunction()
                        cmake_language(DEFER CALL mgif_inject_late_imgui_source)
                        """
                    injection.write_text(textwrap.dedent(contents).strip() + "\n", encoding="utf-8")
                    extra_arguments.append(f"-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES={injection.as_posix()}")

                configured = self.configure(
                    build_root,
                    extra_arguments=extra_arguments,
                    environment=environment,
                )
                self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
                built = self.build_gate(build_root)
                self.assert_gate_rejected(built, case)
                output = built.stdout + built.stderr
                self.assertIn("final configured editor dependency gate failed", output.lower())
                self.assertNotIn("MGIF_HOSTILE_HEADER_COMPILED_BEFORE_DEPENDENCY_GATE", output)

    def test_core_only_project_include_fails_before_compilation(self) -> None:
        build_root = self.case_root / "core-project-include-build"
        self.write_core_only_fixture()
        forced_header = self.case_root / "core-project-include-nvvk.hpp"
        forced_header.write_text(
            "#pragma once\n"
            "#if defined(MGIF_EDITOR_PRODUCTION_COMPILE)\n"
            "#error MGIF_HOSTILE_HEADER_COMPILED_BEFORE_DEPENDENCY_GATE\n"
            "#endif\n",
            encoding="utf-8",
        )
        forced_option = ("/FI" if os.name == "nt" else "-include") + forced_header.as_posix()
        project_include = self.case_root / "core-project-include.cmake"
        project_include.write_text(
            f'set(CMAKE_CXX_FLAGS "${{CMAKE_CXX_FLAGS}} {forced_option}")\n',
            encoding="utf-8",
        )
        configured = self.configure(
            build_root,
            extra_arguments=[f"-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES={project_include.as_posix()}"],
        )
        self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
        built = self.build_target(build_root, "scene_editor_core")
        self.assert_gate_rejected(built, "core-project-include")
        output = built.stdout + built.stderr
        self.assertIn("final configured editor dependency gate failed", output.lower())
        self.assertRegex(output, r"(?i)(forbidden editor dependency|injection channel)")
        self.assertNotIn("MGIF_HOSTILE_HEADER_COMPILED_BEFORE_DEPENDENCY_GATE", output)


if __name__ == "__main__":
    unittest.main()
