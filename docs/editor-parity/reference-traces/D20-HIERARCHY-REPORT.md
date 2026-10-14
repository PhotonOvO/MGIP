# D20 / Hierarchy Reference Evidence Report

## Status

This ledger integrates the sealed CPU/domain reference run for hierarchy oracles
<code>O01</code>, <code>O02</code>, <code>O03</code>, <code>O04</code>, and
<code>O09</code>. The reference identity is:

- repository: <code>vk_gltf_renderer</code>
- commit: <code>50373c7b0ff58078727af2c2aac70e29d5e74752</code>
- commit tree: <code>03e44de44217b109ab71dfbf6aee3989593d76b0</code>
- sealed evidence root:
  <code>G:\ReferenceOracleRuns\vkgr-50373c7b-d20-probe</code>
- audit index:
  <code>docs/editor-parity/reference-traces/d20-hierarchy-index.json</code>

The evidence confirms the D20 data-loss and crash paths at the reference commit.
It does **not** choose between <code>exact_defect_parity</code> and
<code>mgif_safety_exception</code>; that choice remains a user decision.

<code>O09</code> is **partial**. Every one of its 298 CPU/domain matrix cells
ran in a fresh process using the exact source-derived command envelope, but none
ran the reference editor UI, ImGui runtime, Vulkan renderer, GPU queue, or
screenshot workflow.

The scope flags are exact and apply to every raw trace:

| Flag | Value |
|---|---:|
| <code>domainExecution</code> | <code>true</code> |
| <code>exactUiCommandEnvelopes</code> | <code>true</code> |
| <code>freshProcessPerCase</code> | <code>true</code> |
| <code>imguiRuntime</code> | <code>false</code> |
| <code>vulkanRuntime</code> | <code>false</code> |
| <code>gpuRuntime</code> | <code>false</code> |

An exact UI command envelope means that the probe called the domain command
with the branch and arguments found at the fixed source lines. It does not mean
that an ImGui widget, popup, focus path, modal lifecycle, Vulkan rebuild, or GPU
settlement was driven.

## Independent Seal Verification

The integration did not rely only on the prose report. It independently parsed
the sealed index and all raw traces, then recalculated every entry in
<code>ARTIFACT_SHA256.txt</code>.

| Check | Verified result |
|---|---:|
| Hash-list header lines | 1 |
| Hash-list artifact entries | 626 |
| Existing files | 626 |
| Byte-count mismatches | 0 |
| SHA-256 mismatches | 0 |
| Raw JSONL trace files | 304 |
| Unique raw-trace paths | 304 |
| Parsed JSONL records | 1,942 |
| Parsed checkpoints | 1,336 |
| Passed processes with <code>case_result.passed=true</code> | 302 |
| Crash processes without a fabricated <code>case_result</code> | 2 |
| Timed-out processes | 0 |
| Distinct scope fingerprints | 1 |
| O09 cells / D20 case IDs | 298 / 298, exact set match |

For every case, the independently parsed checkpoint names match the compact
source index. The two crash traces contain valid <code>run_header</code>,
<code>before</code>, and <code>execute</code> records, then terminate before
<code>switch</code> or <code>case_result</code>.

### Sealed artifacts

| Artifact | Bytes | SHA-256 |
|---|---:|---|
| <code>REFERENCE_ORACLE_REPORT.md</code> | 15,915 | <code>cdbabbe4f058e5cbcc23fd8fa713f6f5e64ff4822a94470aa5ba4f618c15d345</code> |
| <code>TRACE_INDEX.json</code> | 8,527,803 | <code>b873a4c898532381882b658d30720d857cd74c0ae3e0bf292615be3dfe488fea</code> |
| <code>ARTIFACT_SHA256.txt</code> | 117,326 | <code>50fc5c4bee7aa8585b12180550379dd13fdff1d2388fcfe5ab9468c2a1c61e80</code> |
| <code>vkgr_reference_trace_probe.exe</code> | 4,772,864 | <code>52d8ac640f630e7fbd56c493e2817e7e5eed7ab840afff9baba54dfea6153918</code> |
| <code>vkgr_reference_trace_probe.pdb</code> | 40,882,176 | <code>6485918d31698ec3efaf08020d1ecd4197de155eee57a592127feaeab2d20f12</code> |
| <code>reference_trace_probe.cpp</code> | 78,448 | <code>0add3958ce75dd57686e755be503663e0124f15528361a9605820d7ad47d8949</code> |
| <code>multi_scene_oracle.gltf</code> | 1,317 | <code>db06feb281758dcc8b5b62ea5f54d042a7d9c099ec16551e68477d9590f15196</code> |
| <code>minimal_scene.gltf</code> | 236 | <code>67002e80869880f68495ec4d3593b7e87510482a1094ce13b7b4c54b25021b91</code> |
| <code>vkgr-p0-d20-trace_tests.exe</code> | 4,591,616 | <code>c7c2a7f9ef740b4664bb307bdbd00b11985de0b4bbefa99ecdd10c97d6d3d3fd</code> |

The machine-readable index records the absolute path, byte count, and SHA-256
for each artifact above, the six fixed reference source files, all 304 raw
traces, and every O09 cell.

## Registry Totals

| Family | Cells | Dimensions |
|---|---:|---|
| <code>D20-SW</code> | 2 | S0/S1 scene switch and switch-back |
| <code>D20-RN</code> | 4 | S0/S1 x current/non-current clicked row |
| <code>D20-RL</code> | 24 | S0/S1 x row category x point/directional/spot x immediate/switch |
| <code>D20-N-DUP</code> | 24 | orientation x target category x history variant |
| <code>D20-N-DEL</code> | 24 | orientation x target category x history variant |
| <code>D20-N-REN</code> | 24 | orientation x target category x history variant |
| <code>D20-N-AC-E</code> | 24 | add empty child matrix |
| <code>D20-N-AC-P</code> | 24 | add point-light child matrix |
| <code>D20-N-AC-D</code> | 24 | add directional-light child matrix |
| <code>D20-N-AC-S</code> | 24 | add spot-light child matrix |
| <code>D20-RD</code> | 48 | source category x visible root row x history x orientation |
| <code>D20-DD</code> | 52 | node relationship x history x orientation |
| **O09 total** | **298** | one independent process and raw trace per cell |

The complete registry contains 304 cases: 298 O09 cells plus six O01-O04
cases. There are 155 S0 cases and 149 S1 cases. Exit status is 302 x
<code>0x00000000</code> and 2 x <code>0xC00000FD</code>; no case timed out.

## D20 Critical Results

### B0 Delete, immediate Undo/Redo

Case: <code>D20_S0_delete_noncurrent_root_immediate</code>. Scene 0 is current
and B0 is selected from non-current Scene 1. Root names and raw indices are both
retained because global node erase remaps raw indices even where root identity
appears unchanged.

| Checkpoint | Scene 0 roots <code>name(raw)</code> | Scene 1 roots <code>name(raw)</code> | Selection | History <code>undo/redo</code> |
|---|---|---|---|---|
| <code>before</code> | <code>A0(0), A1(1), SharedRoot(6), Camera(8)</code> | <code>B0(3), B1(4), SharedRoot(6)</code> | <code>B0(3)</code> | <code>0/0</code> |
| <code>execute</code> | <code>A0(0), A1(1), SharedRoot(4), Camera(6)</code> | <code>B1(3), SharedRoot(4)</code> | none | <code>1/0, Delete 'B0'</code> |
| <code>undo</code> | <code>A0(0), A1(1), SharedRoot(6), Camera(8)</code> | <code>B0(3), B1(4)</code> | <code>B0(3)</code> | <code>0/1, Delete 'B0'</code> |
| <code>redo</code> | <code>A0(0), A1(1), SharedRoot(4), Camera(6)</code> | <code>B1(3)</code> | none | <code>1/0, Delete 'B0'</code> |

| Checkpoint | Semantic document SHA-256 | Runtime SHA-256 | Revision |
|---|---|---|---:|
| <code>before</code> | <code>6e0b4fed9e7e4a7cfe1a0b1c6e08cb79e80af83f95754138d0572de91bb42d30</code> | <code>61b6c74873572237141745bd872d207dd454f2e494ee09260d5091c57d6f6dd0</code> | 1 |
| <code>execute</code> | <code>5aeddee5d2339739928581594976532cf6267f61645926a437e52c0ea5670946</code> | <code>a0dd247d3babb96df08ac84ab2d76f52a9ef356ff897e08ead2279da024fa8fb</code> | 2 |
| <code>undo</code> | <code>d9606c0d61608847f08359c2a53238ee436f95ad3986e59c0056d97e13f62725</code> | <code>03bd4601b1a2b99c027932717dfe9374db91a8c6f2dede5d79ec7941babe3f06</code> | 3 |
| <code>redo</code> | <code>dd1b7cd91c5c5817bffaa8c4952fad4c6222ca2091861aa3405c842c958ca77d</code> | <code>2e33a9dbd2edd5f4c5f285197b2ddcdf26f631e39aa9c3e4cba0bf74f4f0d97d</code> | 4 |

Undo reconstructs B0 but does not restore Scene 1's <code>SharedRoot</code>;
Redo then removes it from the authored Scene 1 root list. Undo is not
semantically equal to <code>before</code>.

### B0 Delete, switch before Undo

Case: <code>D20_S0_delete_noncurrent_root_switch_before_undo</code>.

| Checkpoint | Current | Scene 0 roots <code>name(raw)</code> | Scene 1 roots <code>name(raw)</code> | Selection | History <code>undo/redo</code> |
|---|---:|---|---|---|---|
| <code>before</code> | 0 | <code>A0(0), A1(1), SharedRoot(6), Camera(8)</code> | <code>B0(3), B1(4), SharedRoot(6)</code> | <code>B0(3)</code> | <code>0/0</code> |
| <code>execute</code> | 0 | <code>A0(0), A1(1), SharedRoot(4), Camera(6)</code> | <code>B1(3), SharedRoot(4)</code> | none | <code>1/0, Delete 'B0'</code> |
| <code>switch</code> | 1 | <code>A0(0), A1(1), SharedRoot(4), Camera(6)</code> | <code>B1(3), SharedRoot(4), Camera(7)</code> | none | <code>1/0, Delete 'B0'</code> |
| <code>undo</code> | 1 | <code>A0(0), A1(1), B1(4), SharedRoot(6)</code> | <code>A0(0), A1(1), SharedRoot(6), Camera(8)</code> | node index 3 | <code>0/1, Delete 'B0'</code> |
| <code>redo</code> | 1 | <code>A0(0), A1(1), B1(3), SharedRoot(4)</code> | <code>A0(0), A1(1), SharedRoot(4), Camera(6)</code> | none | <code>1/0, Delete 'B0'</code> |

| Checkpoint | Semantic document SHA-256 | Runtime SHA-256 | Revision |
|---|---|---|---:|
| <code>before</code> | <code>6e0b4fed9e7e4a7cfe1a0b1c6e08cb79e80af83f95754138d0572de91bb42d30</code> | <code>61b6c74873572237141745bd872d207dd454f2e494ee09260d5091c57d6f6dd0</code> | 1 |
| <code>execute</code> | <code>5aeddee5d2339739928581594976532cf6267f61645926a437e52c0ea5670946</code> | <code>a0dd247d3babb96df08ac84ab2d76f52a9ef356ff897e08ead2279da024fa8fb</code> | 2 |
| <code>switch</code> | <code>00bc8a2b0169fd09e5c3121cf5be53c09c054ef8948c20cc57982c5baa6f587c</code> | <code>b3212403df4be04aadce2f0fb0252d9f4982aa8cc8c19119a58f199941612cad</code> | 3 |
| <code>undo</code> | <code>cc5d2378c0188edbb9bb3d766a4fa2dfd0f3713165403ebb5368756ac770b29a</code> | <code>8d042c4551f7addad07c76189f05c6ac2083f7982619e56868f23841215ce01f</code> | 4 |
| <code>redo</code> | <code>1c4cf86a47325b9623cc534f83a63aff3996f901cfc2340f4429dfeafcba079a</code> | <code>2c06a49cd5de14aef5dea173b692341f41625317825b117a9a680a53ca3f14aa</code> | 5 |

Undo while Scene 1 is current copies the saved Scene 0 root vector into Scene
1. It also leaves the remapped B1 in Scene 0. The S1-oriented symmetric case
independently cross-writes the opposite direction.

### Plain switch mutates the document

Case: <code>D20_S0_scene_switch_roundtrip</code>. There is no history command
and selection remains none.

| Checkpoint | Current | Scene 1 roots | History | Semantic document SHA-256 | Runtime SHA-256 | Revision |
|---|---:|---|---|---|---|---:|
| <code>before</code> | 0 | <code>B0(3), B1(4), SharedRoot(6)</code> | <code>0/0</code> | <code>6e0b4fed9e7e4a7cfe1a0b1c6e08cb79e80af83f95754138d0572de91bb42d30</code> | <code>61b6c74873572237141745bd872d207dd454f2e494ee09260d5091c57d6f6dd0</code> | 1 |
| <code>execute</code> | 1 | <code>B0(3), B1(4), SharedRoot(6), Camera(9)</code> | <code>0/0</code> | <code>1e020cf58a8c1b410c0e5d0c118a8192a16a95f030433a55f53bcd88673a1cf6</code> | <code>a1002732c6ca74f7630fa54f1ada336bffb17a1ee1c42f754800def65feb4534</code> | 2 |
| <code>switch_back</code> | 0 | <code>B0(3), B1(4), SharedRoot(6), Camera(9)</code> | <code>0/0</code> | <code>da9cd56d48316d4fb233b035a370f002b0db11614d7e05d63b4891c2ecd58c8b</code> | <code>7341913dbf237404f4c85d35791aa4bd97e69eab2d3f620e3d3bf226d3bfdcbf</code> | 3 |

<code>Scene::setCurrentScene(1)</code> injects Camera into Scene 1. Switching
back does not remove it, and <code>defaultScene</code> stays 0.

### Dangling, multiparent, and cycle results

- Multiparent:
  <code>D20_S0_node_drop_noncurrent_child_to_noncurrent_other_root_immediate</code>
  executes with the current-scene parent map reporting
  <code>oldParent=-1</code>. B0 and B1 both end with child raw index 5
  (BChild), and Scene 1 diagnostics report <code>pathCount=2</code>. Execute
  semantic SHA-256 is
  <code>73ce5d32f0fc501671b0f6f0a689219bffdb94ced61c829a84e3faaa50532c6c</code>.
  Undo then appends BChild as a Scene 0 root, with semantic SHA-256
  <code>0b7c9ad501a9031c6d2e90dc11c451ec5fb21736bd98c3ed98affe5384c54a44</code>.

- Cycle:
  <code>D20_S0_node_drop_noncurrent_root_to_noncurrent_child_immediate</code>
  reports <code>wouldCreateCycleResult=false</code>, then leaves
  <code>B0.children=[5]</code> and <code>BChild.children=[3]</code>.
  Diagnostics report cycle <code>[3,5,3]</code> and duplicate reachability for
  B0 with <code>pathCount=2</code>. Execute semantic SHA-256 is
  <code>69e30783b4d3bd051fc01fb9afa0e134675534ae1b3b1f217e98972b6a8830b9</code>.

- Dangling root:
  <code>D20_S0_row_add_directional_light_noncurrent_switch_before_undo</code>
  first appends Directional Light raw index 9 to current Scene 0. After
  switching and Undo, Scene 0 roots are <code>[0,1,6,8,9]</code>, but node 9
  has been removed; diagnostics report <code>{child:9,parent:-1}</code>. Scene
  1 is overwritten with <code>[A0,A1,SharedRoot,Camera]</code>. Redo places raw
  root 9 in both scenes.

### Crash cases

| Case | Orientation | Exit | Last checkpoint | Cycle |
|---|---|---|---|---|
| <code>D20_S0_node_drop_noncurrent_root_to_noncurrent_child_switch_before_undo</code> | S0 | signed <code>-1073741571</code>, <code>0xC00000FD</code> | <code>execute</code> | <code>B0 -> BChild -> B0</code>, raw <code>[3,5,3]</code> |
| <code>D20_S1_node_drop_noncurrent_root_to_noncurrent_child_switch_before_undo</code> | S1 | signed <code>-1073741571</code>, <code>0xC00000FD</code> | <code>execute</code> | <code>A0 -> AChild -> A0</code>, raw <code>[0,2,0]</code> |

Both cases have <code>timedOut=false</code>. Both emit the cyclic document and
valid diagnostics, then overflow the stack during the requested scene switch.
Neither emits a <code>switch</code> checkpoint or <code>case_result</code>; the
compact registry preserves <code>caseResultPassed=null</code>.

### Clicked row identity is ignored

- <code>D20_S0_row_add_node_noncurrent</code> clicks Scene 1 while Scene 0 is
  current. The exact envelope calls <code>SceneEditor::addNode("", -1)</code>
  without scene ID; <code>Node_9</code> is appended to Scene 0, selection
  becomes node 9, and history remains <code>0/0</code>. The S1 case
  symmetrically appends <code>Node_10</code> to current Scene 1 after clicking
  Scene 0.
- Scene-row Add Point/Directional/Spot passes <code>parentIndex=-1</code>; the
  clicked <code>sceneID</code> is absent. Current and non-current row variants
  therefore all author into the current scene.
- Root-row drop likewise ignores clicked <code>sceneID</code> and calls the
  reparent command with <code>newParent=-1</code> using the current-scene
  parent map.

## O01-O04 Results

### O01 same-parent Move no-op

<code>O01_same_parent_move_noop</code> keeps AChild under A0, selection at
AChild raw index 2, semantic SHA-256
<code>6e0b4fed9e7e4a7cfe1a0b1c6e08cb79e80af83f95754138d0572de91bb42d30</code>,
and runtime SHA-256
<code>61b6c74873572237141745bd872d207dd454f2e494ee09260d5091c57d6f6dd0</code>
at every checkpoint. The history transition is:

<code>0/1 Rename 'A0'</code> -> <code>1/0 Move 'AChild'</code> ->
<code>0/1 Move 'AChild'</code> -> <code>1/0 Move 'AChild'</code>.

Thus the document and runtime are no-ops, but execute clears seeded redo and
pushes the Move command.

### O02 current/non-current root-row Move no-op

<code>O02_root_to_current_root_row_noop</code> and
<code>O02_root_to_noncurrent_root_row_noop</code> have identical document,
runtime, selection, and history outcomes. A1 remains selected at raw index 1;
roots and both hashes remain identical to <code>before</code>. The transition
is:

<code>0/1 Rename 'A0'</code> -> <code>1/0 Move 'A1'</code> ->
<code>0/1 Move 'A1'</code> -> <code>1/0 Move 'A1'</code>.

The non-current clicked row is not passed to
<code>ReparentNodeCommand</code>.

### O03 Undo appends instead of restoring sibling position

Case: <code>O03_reparent_undo_append_order</code>.

| Checkpoint | P children | Q children |
|---|---|---|
| <code>before</code> | <code>[p0, X, p1]</code> | <code>[q0]</code> |
| <code>execute</code> | <code>[p0, p1]</code> | <code>[q0, X]</code> |
| <code>undo</code> | <code>[p0, p1, X]</code> | <code>[q0]</code> |
| <code>redo</code> | <code>[p0, p1]</code> | <code>[q0, X]</code> |

Undo appends X to P's tail rather than restoring its original sibling slot.

### O04 true unique-root result and prediction conflict

The camera-less <code>minimal_scene.gltf</code> is not unique-root after
<code>Scene::load</code>; the loader injects a Camera root, yielding
<code>[Root, Camera]</code>. <code>O04_minimal_fixture_as_loaded_delete</code>
therefore performs a real Root delete.

<code>O04_true_unique_root_delete</code> reparents the injected Camera below
Root with the executed setup call
<code>SceneEditor::setNodeParent(defaultCameraNode, Root)</code>, then captures
a true single-root state:

| Checkpoint | Roots | Selection | History <code>undo/redo</code> | Semantic document SHA-256 | Revision | Runtime SHA-256 |
|---|---|---|---|---|---:|---|
| <code>before</code> | <code>[Root(0)]</code> | <code>Root(0)</code> | <code>0/1 Rename 'Root'</code> | <code>b95038a5c264ebd4afff9ba194f10b8ef53530904ba0b9e477fa2b074b2fee48</code> | 2 | <code>eab1a73311c9f45968bb8e150057100c2f5de06123d0894bb3826db354facdf6</code> |
| <code>execute</code> | <code>[Root(0)]</code> | none | <code>1/0 Delete 'Root'</code> | same | 3 | <code>c42c9f0fe6ffba9f02a4956d0f9cab56ac9e640231ea35d5adf5cebebc4b610b</code> |
| <code>undo</code> | <code>[Root(0)]</code> | <code>Root(0)</code> | <code>0/1 Delete 'Root'</code> | same | 4 | <code>6cfbdd4e37739c4a249d739c6b5d5c2f28cec02bcd6c563f78b9921b9bfea3b7</code> |
| <code>redo</code> | <code>[Root(0)]</code> | none | <code>1/0 Delete 'Root'</code> | same | 5 | <code>1480b4d68af3672585414ca658f6b0ba3b378112bb6f711b41a001e5c99a482d</code> |

This conflicts with the task specification's predicted runtime-invariance
clause. The semantic document and root vector are unchanged, but
<code>sceneGraphRevision</code> changes **2 -> 3 -> 4 -> 5**, and runtime
SHA-256 changes at every checkpoint.

The exact confirmed-delete command envelope was executed, but the reference
Delete modal was **not executed**: <code>imguiRuntime=false</code>, the envelope
records <code>uiRuntimeDriven=false</code>, and modal state is unavailable.
This is domain-command evidence, not modal-lifecycle evidence.

## Build, Run, and Test Identity

The full exact command strings are stored in the JSON index. The run used:

- Windows 11 Enterprise <code>10.0.26100</code>, x64
- CMake <code>4.1.2</code>, Visual Studio 17 2022 Community, platform x64
- MSVC <code>19.44.35217</code>,
  <code>_MSC_FULL_VER=194435217</code>, toolset v143
- Windows SDK <code>10.0.26100.0</code>
- Vulkan SDK <code>1.4.328.1</code>
- Git <code>2.51.0.windows.2</code>
- isolated <code>nvpro_core2</code> commit
  <code>9b482eb8961433c51b77e04ac48964f4eb585481</code>

Each of the 304 case IDs was launched as a separate process with a 20,000 ms
wait boundary and separate stdout/stderr logs. No timeout occurred.

CTest exited 0 after discovering 123 tests: 43 passed, 80 skipped, and 0
failed. The focused IndexRemapping/NodeHierarchy/ChildrenOrder binary also
exited 0 after discovering 21 tests, but all 21 called
<code>GTEST_SKIP</code> because <code>GLTF_SAMPLE_ASSETS_PATH</code> was empty.
Those skipped tests are build/run evidence, not substitute behavioral coverage;
the 304 executable probe traces provide the hierarchy behavior evidence.

## Remaining Runtime Gaps

Exactly three evidence layers remain unexecuted:

1. ImGui input routing, popup lifecycle, focus, and visible row state.
2. Vulkan/GPU scene rebuild, texture update, frame reset, and queue settlement.
3. P0-T02 screenshots and clean/hover/active/disabled/modal visual states.

These gaps are why <code>O09</code> remains <code>partial</code> even though
<code>executedCellCount=matrixCellCount=298</code>. The two crash cells count
as executed CPU/domain cells, not passed cells.

## Decision Boundary

The reference results are sufficient to present the D20 decision with concrete
data-loss, dangling-reference, multiparent, cycle, and crash evidence. They are
not authorization to select a policy. Until the user explicitly chooses
<code>exact_defect_parity</code> or <code>mgif_safety_exception</code>, the
JSON index retains:

    {
      "status": "pending_user_decision",
      "selectedPolicy": null
    }
