# Fixed-Commit Reference Source Evidence

This ledger binds the P0 source-level parity evidence to
`vk_gltf_renderer` commit
`50373c7b0ff58078727af2c2aac70e29d5e74752`. It is not executable behavior
evidence. All interaction-sensitive oracle traces remain pending until their
hash-bound probes or application runs are captured.

## Provenance

- Reference repository: `F:/GitHub/vk_gltf_renderer`
- Fixed commit: `50373c7b0ff58078727af2c2aac70e29d5e74752`
- Existing application:
  `F:/GitHub/vk_gltf_renderer/_bin/Debug/vk_gltf_renderer.exe`
- Application SHA-256:
  `e3c81b08b9138b623cfbf1c2338a6ef9b10994787ed1b8761bf65061a2009192`
- PDB SHA-256:
  `8d8037aeda20b941c319a2683c7f94a372c21883c0b8913feac39588babe4f5c`
- PDB identity: `{25533B10-07FB-4977-9ED4-0795DBCB9F4C}`, age `3`
- Full read-only investigation report:
  `G:/P0-reference-oracle-task_c89206ad3925.md`
- Full report SHA-256:
  `381175fd9038ad4e9480254a5ae4408699189af3b8f8409137c67b8c928b34f2`

The PDB records checksums for 91 tracked reference files. All 91 checksums
match the bytes at the fixed commit. The PDB CodeView identity also matches the
application. This strongly binds the compiled editor C/C++ source bytes, but it
does not bind every generated shader, DLL, build option, or driver state. No
behavioral trace is upgraded solely from this provenance.

## Oracle Predictions

| Oracle | Fixed-commit source | Provisional result |
|---|---|---|
| O01 | `src/ui_scene_browser.cpp:751-772`; `src/undo_redo.cpp:37-45`; `src/gltf_scene_editor.cpp:578-583` | Same-parent drop leaves the document unchanged but clears redo and pushes one Move command. |
| O02 | `src/ui_scene_browser.cpp:459-470`; `src/gltf_scene_editor.cpp:578-583` | Root-to-root drop leaves root order unchanged but records the same command envelope. |
| O03 | `src/gltf_scene_editor.cpp:585-606`; `src/undo_redo.cpp:320-332` | Execute, Undo, and Redo append at the destination tail; Undo does not restore the original sibling slot. |
| O04 | `src/undo_redo.cpp:252-265`; `src/gltf_scene_editor.cpp:457-491` | Unique-root Delete clears selection/redo and enters history even though the document mutation is rejected. |
| O05/O06 | `src/ui_renderer.cpp:52-89,820-883`; `src/renderer.cpp:868-958`; `src/gizmo_transform_vk.cpp:184-260,849-855` | Escape-only detaches without an end callback; Escape plus release in the same frame reaches `endDrag`. Exact frame behavior remains unexecuted. |
| O07 | `imgui_impl_glfw.cpp:563-575`; `imgui.cpp:1821-1837,10821-10829,11160-11201` | The matched focus-loss path clears mouse state without a proven release callback. Both release/no-release partitions require execution. |
| O08 | `src/ui_mouse_state.hpp:55-103`; `src/ui_renderer.cpp:52-163` | Single click is delayed by the double-click window; double hit changes camera center/speed and suppresses single selection. |
| O09 | `src/ui_scene_browser.cpp:408-495`; `src/gltf_scene_editor.cpp:223-242,267-611,737-778`; `src/gltf_scene.cpp:684-688,1232-1269` | Visible row actions route through current-scene state and current-only parent/snapshot maps, creating cross-scene data-loss risks described below. |
| O10 | `src/scene_selection.cpp:119-147`; `src/ui_inspector.cpp:975-1135` | Clipboard format is `gltf_material:<index>`; runtime states and malformed input still require execution. |

## Feature Evidence

Every row below is `source/provisional` unless explicitly described as negative
source evidence. Named tests were inspected but were not run in this source
audit.

| ID | Fixed-commit evidence | Finding |
|---|---|---|
| F01 | `src/gltf_scene.cpp:240-294,445-551`; `tests/test_roundtrip.cpp` | Model-primary load/save and round-trip tests exist. |
| F02 | `src/ui_renderer.cpp:374-390`; `src/gltf_scene.cpp:684-688`; `src/ui_scene_browser.cpp:408-495` | Multiple Scenes selection exists; structural result is blocked by D20. |
| F03 | `src/ui_scene_browser.cpp:129-155,362-385`; `src/ui_xmp.cpp` | Asset metadata, extensions, and XMP UI exist. |
| F04 | `src/ui_animation_control.hpp:36-130`; `tests/test_animation_update.cpp` | Animation controls and tests exist. |
| F05 | `src/ui_scene_browser.cpp:166-176`; `src/gltf_scene.cpp:1359-1393,1888-1929` | Variant listing and global application exist. |
| F06 | `src/ui_scene_browser.cpp:391-499,682-820,1602-1660` | Hierarchy, icons, expansion, and focus exist. |
| F07 | `src/ui_scene_browser.cpp:505-515,1009-1400` | Eight resource-list groups and selection rules exist. |
| F08 | `src/ui_scene_browser.cpp:199-206,221-360` | Debug counts and mapping diagnostics exist. |
| F09 | `src/scene_selection.cpp:35-114`; browser selection sites | One six-type selection context exists. |
| F10 | `src/ui_mouse_state.hpp:25-103`; `src/ui_renderer.cpp:52-175` | Delayed click, double-click, and focus paths exist; runtime trace is pending. |
| F11 | `src/ui_scene_browser.cpp:418-454`; `src/gltf_scene_editor.cpp:389-454` | Root node/light creation exists; row target is D20-sensitive. |
| F12 | `src/ui_scene_browser.cpp:1431-1461`; `src/undo_redo.cpp:273-305,388-424` | Child node/light creation exists. |
| F13 | `src/gltf_scene_editor.cpp:267-386`; index-remap tests | Subtree, skin, and animation remapping exist. |
| F14 | `src/ui_renderer.cpp:500-542`; `src/undo_redo.cpp:238-270`; `src/gltf_scene_editor.cpp:457-529` | Recursive delete and unique-root command envelope exist. |
| F15 | `src/ui_scene_browser.cpp:1468-1575`; `src/undo_redo.cpp:177-198` | Node history rename and direct mesh/material rename exist. |
| F16 | `src/ui_scene_browser.cpp:459-470,743-774`; `src/gltf_scene_editor.cpp:535-611` | Append-only reparent, cycle checking, and root drops exist. |
| F17 | `src/ui_inspector.cpp:698-785`; `src/undo_redo.hpp:110-137` | Numeric TRS and quaternion-backed edit commands exist. |
| F18 | `src/ui_scene_browser.cpp:521-675` | Scene transform baseline/reset/freeze exists and is non-history. |
| F19 | `src/gizmo_transform_vk.cpp:221-260,644-665,715-855`; `src/renderer.cpp:363-390,904-958` | Gizmo components and drag callbacks exist; frame trace is pending. |
| F20 | `src/ui_renderer.cpp:460-479,742-818`; `src/renderer.cpp:868-878` | Grid, snap, and toolbar state exist. |
| F21 | `src/ui_inspector.cpp:791-969` | Metallic-roughness factors and texture editing exist. |
| F22 | `src/ui_inspector.cpp:810-845,1167-1192` | Spec-gloss and 13 extension dispatch functions exist. |
| F23 | `src/ui_inspector.cpp:1188-1235`; dirty-flag tests | Extension add/remove and material dirty propagation exist. |
| F24 | `src/ui_inspector.cpp:975-1135`; `src/scene_selection.cpp:119-147`; `src/gltf_scene_editor.cpp:881-928` | Assign and OS clipboard workflows exist; executable states are pending. |
| F25 | `src/ui_inspector.cpp:975-1160`; `src/gltf_scene_editor.cpp:930-1125` | Duplicate Material, Split, and Merge-equivalent paths exist. |
| F26 | `src/ui_inspector.cpp:500-534` | Perspective and orthographic camera fields exist. |
| F27 | `src/ui_inspector.cpp:539-550`; `src/ui_renderer.cpp:1135-1240` | Apply/Set current-view workflows exist. |
| F28 | `src/gltf_scene_editor.cpp:421-454`; `src/ui_inspector.cpp:557-692` | Three punctual light types and editing exist. |
| F29 | `src/ui_inspector.cpp:755-781`; `src/gltf_scene_editor.cpp:189-217` | Authored and recursive effective visibility exist. |
| F30 | `src/undo_redo.cpp:37-113`; `src/undo_redo.hpp:75-104`; `src/ui_renderer.cpp:585-606` | Linear history, descriptions, 100 cap, and 500 ms merge exist. |
| F31 | `src/gltf_scene.cpp:834-878`; dirty-flag tests | Incremental transform/material/light dirty APIs exist. |
| F32 | `src/gltf_scene.cpp:1192-1328`; `src/renderer.cpp:1201-1205` | Topology rebuild exists; reference full rebuild waits for queue idle. |
| F33 | `src/ui_renderer.cpp:545-583,820-956`; `src/renderer.cpp:621-692` | New/Open/Recent/drop replacement exist. |
| F34 | `src/ui_renderer.cpp:545-563,958-968`; `src/renderer.cpp:585-681` | Menu and Shift-drop merge exist. |
| F35 | `src/ui_renderer.cpp:575,975-981`; `src/gltf_scene.cpp:240-294,445-551` | Save As and glTF/GLB writer paths exist. |
| F36 | `src/ui_renderer.cpp:691-724,1011-1021`; `src/gltf_compact_*.cpp` | Orphan compaction and remapping exist. |
| F37 | `src/ui_scene_browser.cpp:129-150`; `src/renderer.cpp:585-681`; `src/ui_renderer.cpp:500-724` | Busy/disabled/error/Delete modal states exist; no unsaved or Compact confirmation was found. |
| F38 | `src/ui_renderer.cpp:545-1036`; `src/ui_scene_browser.cpp:1406-1532` | Menu, shortcut, context, and toolbar definitions exist. |
| F39 | `src/main.cpp:84-98,199-216`; `src/ui_renderer.cpp:983-1001` | Validation and screenshot mechanisms exist; editor visual evidence is pending. |
| F40 | Reference CMake and Vulkan-only startup | Negative source evidence: no D3D12, Metal, or Android editor implementation exists; this is an MGIF contract. |
| F41 | Scene Browser raw loops; `tests/benchmark_main.cpp` | Negative source evidence: no list clipper or editor UI performance proof was found. |
| F42 | `LICENSE`; SPDX source headers; `README.md:157-159` | Apache-2.0 and NVIDIA notices are present. |

## D20 Data-Safety Risk

The source renders every scene row but root Add/Light/drop actions call helpers
with `m_currentScene`; row identity is not passed. Parent lookup and structural
snapshots are also derived from the current scene only, while node-vector erase
and index remapping affect all scenes.

The highest-risk source-predicted cases are:

- deleting a non-current root globally remaps all scenes, while Undo restores
  roots only for the scene that is current at restore time;
- switching scenes before Undo can restore a root snapshot from one scene into
  another scene;
- a non-current source can remain in its original collection while also being
  appended to a target, creating duplicate or multi-parent reachability;
- current-only cycle checking can miss a cycle in a non-current tree;
- root Add/Light on a clicked non-current row can author into the current scene.

The canonical executable case is a pristine `multi_scene_oracle.gltf` with
Scene 0 current, followed by Delete of non-current `B0`, then Undo and Redo.
The probe must record every scene root/child index and stable fixture ID at each
checkpoint. Until that and the symmetric Scene 1 orientation are executed,
D20 remains `pending_executable_trace` and P1 remains blocked.
