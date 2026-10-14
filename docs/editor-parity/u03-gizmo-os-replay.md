# U03 Native Gizmo OS Replay Development Record

Date: 2026-09-06. This is a development record, not a sealed receipt or a
progress-ledger transition. Full U03 is not accepted; G8 remains failed and
P9/P10 remain blocked.

## Verified Scope

`U03-gizmo-translate` completed on Windows/MSVC Debug and Release at 1280x720,
96 DPI: 6 steps, 16 ready/continue exchanges, 8 planned SendInput phases,
12 before/after PNGs, and one clean process per configuration. The manifests
report `PASS_TRUE_OS_REPLAY_PARTIAL` and `coverageComplete=false`.

The script selects `Selected` through its real Scene Graph row, hovers the
drawn Y-translation component, presses, moves into the Browser band outside
the observed viewport, releases, then performs Undo and Redo.

| State | Local Translation | Accepted Actions | Capture |
| --- | --- | --- | --- |
| Before | 0, 0, 0 | 0 begin / 0 preview / 0 commit | Off |
| Dragging Outside | 0, 1.4937688105067666, 0 | 1 / 1 / 0 | On |
| Released | 0, 1.4937688105067666, 0 | 1 / 1 / 1 | Off |
| Undo | 0, 0, 0 | 1 / 1 / 1 | Off |
| Redo | 0, 1.4937688105067666, 0 | 1 / 1 / 1 | Off |

Both configurations agree. Selection, rotation, scale, X/Z translation, and
hierarchy order remain unchanged. The drag advances the document revision once
and adds one history entry; Undo/Redo each publish the expected revision and
history transition. The delivered terminal event retains its input-frame and
sequence identity. No detach or latch is reported.

Final evidence roots:

- Debug: `G:\Temp\mgif-u03-gizmo-debug-20260906-r6`
- Release: `G:\Temp\mgif-u03-gizmo-release-20260906-r4`

Each root contains `manifest.json`, `app/main/result.json`, per-phase observations,
input receipts, screenshots, and source identity records.

Plan SHA-256:
`d2900c9b1d188e2481d03a786f4c11ba1bcfe7dc61ab3f2f5555283b28bed60b`.

Shared enumerated source identity SHA-256:
`5997f84028ae6d196ab70fc1817aef727c204d44b342abecf1817c19e87497da`.
This is the harness's scoped source set, not an exhaustive repository closure.

## Implementation And Fixes

- Component targets are projected from visible draw-packet triangle geometry,
  quantized to real cursor coordinates, and confirmed by the production hit
  classifier. The probe does not mutate interaction state.
- Native snapshots bind node, document/selection revisions, mapping epoch,
  render/input frames, viewport, pointer, component, capture, transform, accepted
  action counters, and terminal provenance. Missing or inconsistent observations
  cannot establish a passing phase. Unchanged duplicate previews do not count.
- A real replay exposed native finish consuming `m_commandSequence`, which made
  the next UI Undo stale. `m_gizmoFinishSequence` now separately numbers native
  gesture terminals. The existing stale UI command rejection is unchanged.
- A real replay also exposed opposite Y motion with Vulkan's flipped projection.
  `GizmoCameraSnapshot::ndcYDown` now makes the inverse ray and projected target
  agree with the backend's viewport convention. Default/D3D behavior is retained;
  CPU tests independently project the resulting motion to screen coordinates.
- Native captures audit model pixels inside the actual observed viewport. They
  retain global checks, UI-region checks, all nine region records, ownership,
  occlusion, DPI, and persisted-pixel consistency checks. Sparse empty viewport
  regions are allowed; a blank viewport or isolated axis lines are rejected.
- Startup binds the HWND reported in the first ready file and verifies its PID.
  When ordinary activation is refused, an owned, unobscured caption can be
  activated with real SendInput; only the test window may be temporarily raised.
  These extra startup inputs are separately recorded in `startup-activation.json`
  and the manifest, not counted as planned replay phases. Foreground loss during
  replay still fails immediately. Closing the test window removes its temporary
  topmost state.

## Regression Verification

Both Windows configurations passed the OS replay plan/runtime/CLI/contract
CTests, Gizmo CPU tests, desktop Gizmo integration tests, controller transaction
tests, and rebuilt ImGui probe tests. The Python contract suite now has 11 tests,
including byte-exact plans and sparse/blank viewport pixel audit cases.

The original U02 and Inspector plan SHA values did not change. Their real OS
replays also passed after the changes:

- U02 Debug: `G:\Temp\mgif-u03-gizmo-u02-regression-debug-20260906`
- U02 Release: `G:\Temp\mgif-u03-gizmo-u02-regression-release-20260906`
- Inspector Debug: `G:\Temp\mgif-u03-gizmo-inspector-regression-debug-20260906-r5`
- Inspector Release: `G:\Temp\mgif-u03-gizmo-inspector-regression-release-20260906-r2`

## Remaining Work

Viewport picking classification, other Gizmo components and parent-space cases,
Grid/Gizmo/Snap controls and O07 focus-loss sequences remain outside this proof.
Escape-only with a held mouse button and
the detached late release are tracked separately in the
[Escape OS replay record](u03-escape-os-replay.md). Both MGIF same-frame orders
now have separate Debug and Release evidence in the
[same-frame OS replay record](u03-same-frame-os-replay.md). O06 task prose follows
the observed-authoritative detach/no-history disposition; the reference's
private-callback/direct-history-memory debt remains unchanged.

No RenderDoc or complete visual/DPI/performance acceptance is claimed. The
script deliberately releases outside the viewport; the selected object can move
partly out of view. Clipped Browser labels and other existing visual issues are
not signed off by this scoped input proof. Earlier synthetic passive observations
and failed development runs are not acceptance evidence.
