# Input and clipboard reference evidence

## Result and identity

This repository artifact integrates the sealed external evidence at
`G:\ReferenceOracleRuns\vkgr-50373c7b-input-probe` for O05, O06, O07, O08,
and O10. The isolated reference worktree was directly checked at HEAD
`50373c7b0ff58078727af2c2aac70e29d5e74752`, tree
`03e44de44217b109ab71dfbf6aee3989593d76b0`. Its seven frozen application
source files match the worktree, whose `src/` diff against HEAD is empty.
The worktree is intentionally dirty only in `CMakeLists.txt` and the three
files under `tools/p0_input_trace_probe/`. The linked `nvpro_core2` checkout
has an unrelated dirty early return in `nvapp/elem_inspector.cpp`; it is
hash-bound but is not on a directly exercised probe path.

Independent integration checks found all 68 `SHA256SUMS.txt` entries present
and matching. The primary trace contains 144 parseable JSONL records, one
header, one summary, and 40 passing assertions with 0 failures; its summary
exit code is 0 and stderr is empty. Assertions by oracle are O05=4, O06=8,
O07=11, O08=12, and O10=5. The repeat trace has the same counts and no failed
assertions.

The machine-readable index is
[`input-clipboard-index.json`](input-clipboard-index.json). It contains the 30
validated case IDs, exact build/run commands, artifact sizes and hashes,
record/test summaries, evidence grades, clipboard restoration, repeatability,
and runtime gaps.

## Raw artifact binding

| Raw artifact | Bytes | SHA-256 |
|---|---:|---|
| `REPORT.md` | 16,302 | `8305D258858BBD72768A743CBBB305244E60EA8FB0415AA525EE483229DAE856` |
| `SHA256SUMS.txt` | 6,716 | `0F691CBEA80776AFF10A95CB2CBCD7BE6A4EC171837F25A7940FAECE560905A1` |
| `identity.json` | 3,319 | `4483652C5174C627298AA6699AECA59363D7D9C918CB3F90CB4382BB8C85732F` |
| `trace.jsonl` | 72,534 | `28A1F1AED76D5A07D121ED87E6370FD3F9EC570507592927D8B7C6E885F1C184` |
| `trace-repeat.jsonl` | 72,534 | `E280C87392E482E4CEB644052A16026784C9B87D2816249DF66D9A3D63A37B07` |
| `bin/vkgr_p0_input_trace_probe.exe` | 7,391,232 | `E96024230565E6B1823879D39FA74A052E241EE2DA47A03B707C855727744AA4` |
| `probe-source/p0_input_trace_probe.cpp` | 43,068 | `0E13DB85182A04F885DE36A7589BB50AB59624808A2B559AB682C5F45B828842` |

All paths above are relative to the external evidence root. The checksum index
does not list itself; its size and hash were computed separately. The frozen
probe source and EXE hashes also match their files in the isolated worktree.

## Evidence scope

The ceiling is a source-level executable probe with explicitly labeled
source-order models, not a full interactive application observation.

| Grade | What ran | What remains modeled |
|---|---|---|
| `EXECUTED_SOURCE_LEVEL_PROBE` | The named fixed function or Win32 clipboard transport. This covers O10 material copy and clipboard backup/restore. | No surrounding application UI consequence is implied. |
| `EXECUTED_FIXED_CORE_PLUS_SOURCE_ORDER_MODEL` | Fixed Dear ImGui input processing and relevant `UiMouseState`, `SceneSelection`, or `TransformHelperVk` core. | Application phases, renderer attachment/history, supplied picks, Browser focus, and camera consequences follow the fixed-source order model. |
| `EXECUTED_FIXED_PARSER_PLUS_SOURCE_UI_MODEL` | The fixed material parser against the OS clipboard. | Active-document range, assignment, tooltip, and history consequences follow the fixed source/UI model. |

The primary trace contains 3, 82, and 17 grade-bearing records respectively.
Assertion records do not carry a grade.

## O05: Escape-only drag

The fixed gizmo core runs directly. After an initial real preview scale update
from `[1,1,1]` to `[1.1,1.1,1.1]`, Escape clears the modeled selection while
left remains down. The still-attached gizmo performs one final real update to
`[1.2,1.2,1.2]`; modeled render attachment sync then calls real
`clearAttachment`. The preview remains, attached/dragging become false, and a
later physical release produces neither `onTransformEnd` nor modeled history.
The modeled camera remains center `[4,5,6]`, speed `2.5`.

## O06: callback order is observable

The fixed Dear ImGui default is `ConfigInputTrickleEventQueue=true`. Therefore
"both callbacks were queued before `NewFrame()`" does not define one outcome;
their order does.

| Queue order | Frame N | Following frame | Final consequence |
|---|---|---|---|
| Escape down, then left-button up | Both are visible in N. Menu Escape clears selection before viewport `MouseReleased`; real `endDrag` fires once. | No deferred input is needed. | One modeled `Transform 'ProbeNode'` history entry, scale `[1.15,1.15,1.15]`, then modeled render sync detaches. |
| Left-button up, then Escape down | N sees the release but trickles Escape. The drag commits once while selection and attachment remain. | N+1 exposes Escape; menu clears selection and modeled render sync detaches. | The same preview and one history entry, but selection clearing is one application frame later. |

Thus, Escape clearing before release commit is confirmed only for the
Escape-first callback order. It is false for the release-first order even
though both callbacks entered the queue before the same `NewFrame()`.

## O07: three focus-loss partitions

| Partition | Direct input/core result | Modeled application consequence |
|---|---|---|
| No delivered release | `AddFocusEvent(false)` makes fixed Dear ImGui report `AppFocusLost=true`, `MouseDown=false`, `MouseReleased=false`. The real gizmo stays attached and dragging. A later physical-up callback after focus restoration is not classified as a release because ImGui already cleared mouse state. | Selection stays primitive and history stays 0 until a later modeled selection clear makes render sync call real `clearAttachment`, discarding the latched drag without an end callback. |
| Mouse-up then focus-loss in the same input queue | Even with mouse-up queued first, focus clearing prevents application-visible `MouseReleased`. The real drag remains latched. | Selection and attachment remain; history stays 0. A physical callback in the queue is not the same as a release delivered to `TransformHelperVk::processInput`. |
| Release delivered in the preceding application frame | The release frame exposes `MouseReleased=true`; real `endDrag` runs once. | One modeled history entry is committed. Focus loss in the next frame adds no callback and does not clear selection or detach the committed gizmo. |

All three variants leave the modeled camera unchanged.

## O08: delayed click classifier boundary

Direct execution covers fixed Dear ImGui mouse timing,
`UiMouseState::update`, and `SceneSelection` methods. Pick values are supplied;
the application selection, Browser focus, and camera consequences are
source-order models.

- A valid single hit remains pending through time 0.27 and dispatches once at
  frame 4, time 0.33, after the 0.30 second threshold. It models selection of
  primitive render node 0 and one Browser focus request.
- A single hit on the sole already-selected render node clears selection.
  Single blank and out-of-range instance 99 also clear selection.
- A second press at frame 3, time 0.12, classifies a double hit with no prior
  single dispatch. Existing selection at render node 1 remains, while the
  modeled camera becomes center `[1,2,-2]`, speed `5` from the supplied hit.
- A double blank leaves selection and camera center `[10,20,30]`, speed `4`,
  unchanged. Every sequence dispatches exactly one modeled pick.

No real Vulkan ray picker, real scene, camera manipulator, GLFW window
hover/focus, or Browser widget was driven.

## O10: material clipboard and `atoi`

Direct execution covers `SceneSelection::copyMaterialToClipboard`,
`getMaterialFromClipboard`, `hasClipboardMaterial`, Dear ImGui's default Win32
clipboard functions, and OLE backup/restore. Current-document range,
assignment, tooltip, and history consequences are source/UI models. Copy writes
exact ASCII `gltf_material:2`.

For a modeled document with two materials, the fixed MSVC executable reports:

| Clipboard text | `atoi` result | Paste enabled | In range | Modeled mutation |
|---|---:|---|---|---|
| `material:1` | -1 | no | no | none |
| `gltf_material:` | 0 | yes | yes | material 0 |
| `gltf_material:-1` | -1 | no | no | none |
| `gltf_material: 1` | 1 | yes | yes | material 1 |
| `gltf_material:+1` | 1 | yes | yes | material 1 |
| `gltf_material:abc` | 0 | yes | yes | material 0 |
| `gltf_material:1tail` | 1 | yes | yes | material 1 |
| `gltf_material:1.5` | 1 | yes | yes | material 1 |
| `gltf_material:2147483648` | 2147483647 | yes | no | none |
| `gltf_material:999999999999999999999999` | 2147483647 | yes | no | none |
| `gltf_material:0` | 0 | yes | yes | material 0 |
| `gltf_material:1` | 1 | yes | yes | material 1 |
| `gltf_material:7` | 7 | yes | no | none |

Overflow is implementation-specific and is bound only to this MSVC executable.
Cross-document `gltf_material:1` resolves to material 1 in documents with two
or three materials; in a one-material document the parser still enables Paste,
but the modeled active-document range check makes the click a no-op. The text
contains a document-local integer, not a stable identity from the source
document. All modeled assignment paths have history delta 0.

### D16 safety exception

The executable reference's permissive `std::atoi` behavior does not override
D16. MGIF keeps the parity transport `gltf_material:<document index>`, keeps
stable IDs in process only, revalidates the index against the active document
on every Paste, leaves Paste out of history, and rejects malformed suffixes
fail-closed. Empty, whitespace/plus-prefixed, non-digit, trailing-junk,
decimal, and overflow suffixes above are reference observations, not MGIF
acceptance requirements.

## Clipboard restoration and repeatability

The primary run backed up the original OS `IDataObject`, wrote/read the Win32
clipboard through bounded retries, and restored the original object with
HRESULT 0 in one restore attempt. The repeat run also restored it. Sequence
numbers and retry counts are transport metadata.

The two raw trace hashes differ in six records. Independent comparison found
that every difference is confined to `sequence_before`,
`sequence_after_cases`, `backup_attempts`, `transport_attempts`, or
`restore_attempts`. Removing only those fields leaves 0 differing records. The
sealed byte-normalized primary and repeat traces both hash to
`A008988DD8D1A8FE4CFB978A14853887C919BE539A97FA14435B1A1EAE83E4D5`.

## Existing tests and ceiling

The recorded CTest run exited 0 with 123 registered, 43 passed, 80 skipped, and
0 failed. The 80 tests were skipped because `GLTF_SAMPLE_ASSETS_PATH` was
empty. No existing test independently covers `UiMouseState`,
`SceneSelection` clipboard, or gizmo input, so this result does not strengthen
the O05-O08/O10 runtime grade.

This evidence did not launch a full GLFW window or full ImGui application UI,
render Vulkan, drive the GPU, perform a real scene ray query, operate the real
camera manipulator, click Inspector/Browser widgets, mutate a real material
binding, execute the real renderer undo stack, or capture a UI screenshot. It
contains no visual, DPI, hover, modal, viewport-outside, or GPU-validation
observation and does not close full P0 readiness or the D20 multi-scene gate.
