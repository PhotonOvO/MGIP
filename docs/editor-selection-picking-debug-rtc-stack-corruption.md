# Debug RTC Stack Corruption In `editor_selection_picking_integration_tests`

Date: 2026-09-08. Status: **root-caused and resolved the same day**; no C++
source change was required. The writer was localized with a hardware write
watchpoint, and the cause is a build-integrity defect documented in
`docs/build-integrity-20260908.md`. This is a development record; no ledger
state was changed.

## Symptom

The Debug build of the test aborts without printing anything:

```text
G:\Temp\mgif-p6-90-msvc-20260905T231500Z-Debug\editor_selection_picking_integration_tests.exe
rc = 3, stdout = 0 bytes, stderr = 0 bytes
```

Exit code 3 is the MSVC CRT `abort()` code. One invocation blocked instead of
returning (the Debug CRT abort dialog) and had to be terminated with
`Stop-Process`. The freshly relinked Release build of the same test exits 0.

The test's own failure path cannot produce this: `expect()` writes
`FAILED: <message>` to `stderr` (`tests/editor/editor_selection_picking_integration_tests.cpp:58-61`)
and `main()` returns `EXIT_FAILURE` (1), never 3.

## Evidence

`cdb` with the build PDB, breaking on `_RTC_StackFailure`, identifies the
corrupted stack local by name:

```text
00007ff7`1dea9158  "controller"
00 editor_selection_picking_integration_tests!_RTC_StackFailure  [...\crt\vcstartup\src\rtc\error.cpp @ 243]
01 editor_selection_picking_integration_tests!_RTC_CheckStackVars+0x62 [...\crt\vcstartup\src\rtc\stack.cpp @ 69]
02 editor_selection_picking_integration_tests!`anonymous namespace'::testProductionControllerRuntimeAndAdapter+0x3201
   [G:\MGIF\tests\editor\editor_selection_picking_integration_tests.cpp @ 643]
03 editor_selection_picking_integration_tests!main+0x24 [G:\MGIF\...\editor_selection_picking_integration_tests.cpp @ 1254]
```

Full log: `G:\Temp\cdb-selpick3.log` (script `G:\Temp\rtc.cdb`), earlier
epilogue-only capture in `G:\Temp\cdb-selpick2.log`.

So a `/RTC1` stack guard around a local was overwritten, and the check fired at
the function epilogue (`:643`, the `return passed;` of
`testProductionControllerRuntimeAndAdapter`). The named local is

```cpp
ui::EditorController controller(std::move(callbacks));   // :414
```

declared next to `ui::EditorRendererAdapter adapter(rhi::BackendType::vulkan);`
(`:398`). Operations exercised on it in that function include `open()`,
`buildFrameModel()`, `session()->document()`, `applyViewportSelection()`,
`applyPickingSelection()`, `drainSelectionChanges()`, and
`setSelectionOccurrenceLookup()`.

Because RTC only validates guards at the epilogue of the function that owns the
local, the writer can be anywhere in that call tree; the report localizes the
victim, not the writer.

## Writer Localization

The victim name is not enough, so the writer was caught directly: break at
`EditorController::open` (the first production call on the local), step to frame
1, read the object address and extent, then arm byte write watchpoints on the
guard bytes immediately before and after it.

```text
bp editor_selection_picking_integration_tests!demo::editor::ui::EditorController::open
.frame 1
?? &controller                 -> 0x00000001`91f05d80
   +0x14a0 m_viewportHovered
   +0x14a1 m_viewportFocused
   +0x14a2 m_gizmoHovered
ba w1 @@c++((char*)&controller+sizeof(controller))   -> 0x00000001`91f07228
ba w1 @@c++((char*)&controller-1)
```

The first watchpoint to fire was the trailing guard (`sizeof` = `0x14A8`):

```text
Breakpoint 1 hit
00 editor_selection_picking_integration_tests!demo::editor::ui::EditorController::unload+0x1f9
   [G:\MGIF\app\editor\EditorController.cpp @ 1509]
01 editor_selection_picking_integration_tests!demo::editor::ui::EditorController::~EditorController+0x23
   [G:\MGIF\app\editor\EditorController.cpp @ 1012]
02 editor_selection_picking_integration_tests!`anonymous namespace'::testProductionControllerRuntimeAndAdapter+0x31c0
   [G:\MGIF\tests\editor\editor_selection_picking_integration_tests.cpp @ 642]
```

`EditorController.cpp:1509` is `m_viewportFocused = false;` inside `unload()`,
reached from `~EditorController()` (`:1012`) at the end of the test function.
The library write targeted offset `+0x14A8`; the test's own object placed
`m_viewportFocused` at `+0x14A1` and ended at `+0x14A8`. One binary therefore
contained **two incompatible layouts of the same class**, and the destructor
wrote one byte past the smaller stack object.

Logs: `G:\Temp\cdb-writer5.log` (script `G:\Temp\rtc-writer5.cdb`), victim-name
enumeration in `G:\Temp\cdb-rtc-all.log`. A second site was observed the same
way in `testNonCurrentScenePrimitiveSelectionFocusAndStats`
(`tests/editor/editor_selection_picking_integration_tests.cpp:857`), whose
victim is also a local `ui::EditorController`.

## Root Cause

Not a memory error in the editor. `ninja -t deps` shows the two translation
units disagreed because one of them was never rebuilt:

```text
CMakeFiles/editor_selection_picking_integration_tests.dir/tests/editor/editor_selection_picking_integration_tests.cpp.obj
  #deps 0, deps mtime 8103265518216331 (VALID)     mtime 2026-09-05 23:49
CMakeFiles/desktop_scene_editor_ui.dir/app/editor/EditorController.cpp.obj
  #deps 214, deps mtime 8105514309341768 (VALID)   mtime 2026-09-08 14:17
  ../../MGIF/app/editor/EditorController.h
```

The test object recorded **zero** dependencies, so ninja could not know that it
includes `app/editor/EditorController.h`. That header changed on
`2026-09-06 23:18` (Windows and WSL agree on the mtime, so this is not clock
skew) and grew the class; the library objects were recompiled from the new
header while the test object kept the old layout. The executable was relinked at
`2026-09-08 14:27` from the stale object plus fresh libraries.

This is an ODR/layout mismatch produced by the build system, so:

- the line-ending normalization is not the cause (CRLF/LF-only edits cannot
  change C++ semantics); it merely recompiled the library side and exposed a
  hazard that had existed since 2026-09-06;
- the earlier passing run today used a binary whose libraries also predated the
  header change, which is why it looked fine.

## Scope

`ninja -t deps` dumps for both trees (`G:\Temp\ninjadeps-Debug.txt`,
`G:\Temp\ninjadeps-Release.txt`) show the hazard was systemic, including all of
`demo_core`'s RHI and render objects:

| tree | objects with a deps-log entry | `#deps 0` orphans |
| --- | --- | --- |
| Debug | 305 | 176 |
| Release | 245 | 94 |

## Fix

1. Deleted every orphan object (lists preserved:
   `G:\Temp\orphan-objs-Debug.txt`, `G:\Temp\orphan-objs-Release.txt`).
2. Rebuilt both trees completely after the charset repair in
   `docs/build-integrity-20260908.md`: Debug 261 steps `DEBUG_RC=0`, Release 269
   steps `RELEASE_RC=0`, zero compile or link errors.
3. Added a reusable audit so the condition cannot return unnoticed:
   `tools/dump_ninja_deps.cmd` plus `tools/check_ninja_deps_orphans.py`, with
   `tools/ninja-deps-orphans.allowlist` for the one translation unit that
   legitimately records no deps.

No `app/`, `scene/`, `render/`, `rhi/` or `tests/` source was changed for this
defect.

## Verification

- Rebuilt test object now records `#deps 331` (was `0`).
- `editor_selection_picking_integration_tests.exe` exits `rc=0` with zero
  `Run-Time Check Failure` records in Debug and in Release.
- Deps audit passes for both trees:
  `objects=349 orphan_deps0=0 non_valid=0 allow_listed=1`.
- Fourteen Debug executables and eleven Release executables re-run `rc=0`,
  including `p2_projection_cpu_tests`, `scene_editor_scene_commands_tests` and
  `scene_editor_p3_regression_tests`.

The earlier risk note no longer applies: Release was not silently overrunning
the same guard, it was linking the same mixed-layout binary. With both
configurations rebuilt coherently, the Release `rc=0` is meaningful again.

## Regression Check

```bash
# any host: dump then audit, both must report zero orphans
tools\\dump_ninja_deps.cmd G:\\Temp\\<tree> G:\\Temp\\ninjadeps.txt
python3 tools/check_ninja_deps_orphans.py --deps-dump G:/Temp/ninjadeps.txt \
  --tree G:/Temp/<tree> --allow-list tools/ninja-deps-orphans.allowlist
```

The original failure no longer reproduces; the pre-fix evidence remains in
`G:\Temp\cdb-rtc-all.log`, `G:\Temp\cdb-writer5.log` and
`G:\Temp\ninjadeps-Debug.txt`.
