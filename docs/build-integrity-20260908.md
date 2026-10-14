# Build Integrity Findings, 2026-09-08

Status: three independent build-integrity defects were found while localizing a
Debug `/RTC1` stack-corruption abort. All three are resolved or fully explained.
No editor C++ semantics were changed; the only source edit is a CMake charset
repair. The memory-safety symptom and its writer localization are recorded in
`docs/editor-selection-picking-debug-rtc-stack-corruption.md`.

## 1. Ninja deps-log orphans (`#deps 0`)

`ninja -t deps` reports how many dependencies the deps log recorded for each
output. An object with `#deps 0` never rebuilds when a header it includes
changes, so a binary can be linked from translation units compiled against
different revisions of the same class definition.

Measured before repair:

| tree | objects with a deps-log entry | `#deps 0` orphans |
| --- | --- | --- |
| Debug (`mgif-p6-90-msvc-20260905T231500Z-Debug`) | 305 | 176 |
| Release (`mgif-p6-90-msvc-20260905T232000Z-Release`) | 245 | 94 |

The orphans included every `demo_core` RHI and render object
(`rhi/vulkan/Vulkan*.cpp.obj`, `render/GPUDrivenRenderer.cpp.obj`, …) and
`tests/editor/editor_selection_picking_integration_tests.cpp.obj`, while
`app/editor/EditorController.cpp.obj` correctly recorded `#deps 214`. That
asymmetry is what produced the mixed `EditorController` layout and the RTC
abort.

Repair: all orphan objects were deleted (lists preserved at
`G:\Temp\orphan-objs-Debug.txt` and `G:\Temp\orphan-objs-Release.txt`) and both
trees were rebuilt completely. After the rebuild the audited entry count is 349
objects per tree with `orphan_deps0=0 non_valid=0`.

New reusable audit, host-independent by design:

```bash
tools\dump_ninja_deps.cmd G:\Temp\<tree> G:\Temp\ninjadeps.txt
python3 tools/check_ninja_deps_orphans.py --deps-dump G:/Temp/ninjadeps.txt \
  --tree G:/Temp/<tree> --allow-list tools/ninja-deps-orphans.allowlist
```

The checker exits non-zero when any object records zero deps or a non-`VALID`
state, so it can gate evidence collection. One allow-list entry exists:
`tests/gpu_visibility_depth_model_tests.cpp` includes eight C++ standard headers
and no project header, and a clean rebuild of that single object reproduces
`#deps 0`, so its empty deps log cannot cause a layout mismatch. The allow-list
file states that criterion and requires it for every future entry.

## 2. `/utf-8` never reached targets defined before the option

`add_compile_options(/utf-8)` sits at `CMakeLists.txt:1344-1346`, and
`add_compile_options()` only reaches targets created **after** the call. Three
test targets are created before it and compile `loader/GltfLoader.cpp`
themselves:

| target | defined at | compiles the loader at |
| --- | --- | --- |
| `p2_projection_cpu_tests` | 518 | 522 |
| `scene_editor_scene_commands_tests` | 537 | 541 |
| `scene_editor_p3_regression_tests` | 568 | 572, 592 |

Without `/utf-8`, MSVC reads the file's UTF-8 Chinese comments through the OEM
code page (936). One multi-byte sequence swallows the following line break, so
a comment consumes the next declaration:

```text
loader/GltfLoader.cpp(1): warning C4819
loader/GltfLoader.cpp(512): error C2447: "{": missing function header
loader/GltfLoader.cpp(535): error C3861: "readTinyGltfInt": identifier not found
  (also 1132, 1236, 1244, 1469, 1483, 1725, 1746)
```

Line 512 is a comment; the definition it ate is `bool readTinyGltfInt(...)` at
`:513`. `demo_core` compiles the same translation unit cleanly because it is
created after the option. The breakage stayed invisible because those three
objects were `#deps 0` orphans (finding 1) and were therefore never recompiled
after the loader gained its comments — the working tree differs from `HEAD` by
831 inserted lines in that file.

### Constraint: frozen CMake line anchors

`tools/validate_editor_p0_artifacts.py:3770-3785` approves effectful CMake
commands **by line number**:

```text
add_custom_target : 338, 906, 973, 979, 987, 1005, 2548, 2571
add_custom_command: 1475, 1506, 1559, 2505
```

The obvious fix — hoisting `if(MSVC) add_compile_options(/utf-8) endif()` next
to `project()` — shifts every anchor and the configured dependency preflight
fails closed. That was observed, not assumed:

```text
P0 artifact validation failed: unapproved add_custom_target effect in
CMakeLists.txt CMake add_custom_target at line 348
```

(`348` is `338` plus the ten inserted lines.) The hoist was reverted and all
twelve anchors were verified back at their original lines.

### Applied repair

The charset repair is appended **after** the last anchor (line 2571), before the
literal top-level EOF scheduling block, so no approved effect moves. It walks
`BUILDSYSTEM_TARGETS` and adds `/utf-8` privately to compiled targets that do
not already carry it, skipping alias, imported and non-compiled target types.
`CMakeLists.txt` grew from 2656 to 2678 lines, all of them after 2571, and the
twelve anchors were re-verified in place afterwards.

Verification: Debug rebuild 261 steps `DEBUG_RC=0`, Release rebuild 269 steps
`RELEASE_RC=0`, zero `error C`/`error LNK` records, and the three previously
uncompilable targets now build and exit `rc=0`.

## 3. Preflight `configured volk compile fragment has unauthorized CMake provenance`

Both trees failed the configured dependency preflight even with a pristine
`CMakeLists.txt`:

```text
P0 artifact validation failed: configured volk compile fragment has
unauthorized CMake provenance
```

The check (`tools/validate_editor_p0_artifacts.py:7705-7740`, authority built at
`:5012-5084`) matches each configured compile fragment's backtrace — command,
file and **line number captured at configure time** — against the
`add_compile_options` records found in the current source. volk is a C target
that inherits `/utf-8` from the directory-wide option, and the trees were
configured before the current `CMakeLists.txt` revision, so the recorded line no
longer matched any authorized origin.

Evidence that this was pre-existing rather than caused by the charset repair:
restoring the pristine 2656-line file reproduced the identical failure
(`G:\Temp\preflight-pristine.log`), while the patched file produced the same
single failure (`G:\Temp\preflight-after-charset-fix.log`).

Resolution: reconfigure. The CMakeLists.txt edit made ninja re-run CMake, the
backtraces were recaptured from the current source, and the preflight passed in
both trees during the 261-step and 269-step rebuilds. No policy input was
edited and no frozen hash was re-bound.

## Standing rules adopted

1. Before any binary is cited as evidence, run the deps audit on its tree and
   require `orphan_deps0=0`; a mixed-layout binary can pass tests by luck and
   corrupt memory elsewhere.
2. Edits to `CMakeLists.txt` must not shift the frozen anchor lines. If a change
   genuinely requires it, the frozen authority has to be re-bound by explicit
   owner decision, following the precedent in
   `docs/editor-parity/p6-90-write-set-rebind.json`.
3. A build tree whose configuration predates the current `CMakeLists.txt` cannot
   pass the configured preflight; reconfigure before collecting evidence.

## Artifacts

| path | content |
| --- | --- |
| `G:\Temp\ninjadeps-Debug.txt`, `G:\Temp\ninjadeps-Release.txt` | pre-repair `ninja -t deps` dumps |
| `G:\Temp\ninjadeps-Debug-post2.txt`, `G:\Temp\ninjadeps-Release-post.txt` | post-repair dumps (audit rc=0) |
| `G:\Temp\orphan-objs-Debug.txt`, `G:\Temp\orphan-objs-Release.txt` | deleted orphan object lists |
| `G:\Temp\build-debug-reconfigure.log`, `G:\Temp\build-release-reconfigure.log` | coherent full rebuilds |
| `G:\Temp\build-coherent-both.log`, `G:\Temp\build-utf8-coherent.log` | the failing builds that exposed finding 2 |
| `G:\Temp\preflight-pristine.log`, `G:\Temp\preflight-after-charset-fix.log` | finding 3 evidence |
| `G:\Temp\cdb-writer5.log`, `G:\Temp\cdb-rtc-all.log` | watchpoint localization of the RTC writer |
