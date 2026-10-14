# U03 Inspector OS Replay Development Record

Date: 2026-09-06. This is an implementation record, not a sealed acceptance
receipt or a progress-ledger transition. G8 remains failed; P9/P10 remain blocked.

## Verified Scope

`U03-inspector-transform` is an independent, partial child of
`U03-transform-gizmo`. Windows/MSVC Debug and Release each completed 8 steps,
26 ready/continue exchanges, 16 SendInput phases, and 16 before/after PNGs at
1280x720, 96 DPI. Both manifests explicitly use
`PASS_TRUE_OS_REPLAY_PARTIAL` and `coverageComplete=false`.

The replay selects the fixture's `Selected` node using its actual Scene Graph
row, then drags real Inspector controls. Each edit checks exactly three
PropertyEdit dispatches, in order: begin, preview, commit. Full Vec3 values are
checked, including unchanged components:

| Field | Before | Preview And Commit |
| --- | --- | --- |
| Translation | 0, 0, 0 | 0.8, 0, 0 |
| Rotation (degrees) | 0, 0, 0 | 8, 0, 0 |
| Scale | 1, 1, 1 | 1.8, 1, 1 |

Each edit advances the document revision once and pushes one history entry.
Undo and redo move the last scale entry with exact dispatch/history effects.
The runtime verifies unchanged selection and hierarchy ordering, renderer
settling, and synchronized document/projection/runtime digests.

Plan SHA-256:
`51e7c906fb2110153d10355291418358e62c53d28a589f5046bf931732896eb9`.

Source identity SHA-256 (same in both configurations):
`50d723133d42fc0f498f270938bd10d9b4dba0bc56d3d8c25e8f7680471681e9`.
This binds the harness's enumerated source set, not an exhaustive repository
dependency closure.

Evidence roots (each contains `manifest.json` and `app/main/result.json`):

- Debug: `G:\Temp\mgif-u03-inspector-debug-20260906-r1`
- Release: `G:\Temp\mgif-u03-inspector-release-20260906-r1`

## Integrity Changes

- Inspector probes now contain real per-component ImGui IDs, node identity,
  bounds, hover, focus, and nested-disabled state. The component IDs and width
  splitting match the pinned DragScalarN implementation.
- Unsupported viewport/gizmo/toolbar semantics fail closed. They no longer
  synthesize client-wide rectangles or copy expected hover/focus/drop flags
  into observed evidence. Full U03 capability reports `integration=false`.
- Each new-plan phase explicitly requires its dispatch prefix before input.
  Missing dispatches wait; excess or incorrect dispatches fail.
- Pointer displacement is encoded in the hashed plan as `80,0`, not hidden in
  an Inspector-specific harness branch. Foreground and native ownership checks
  remain mandatory.
- PropertyEdit records the actual field, phase, and complete numeric payload.
  Final result JSON retains the same fields as per-phase evidence.

## Verification

Windows/MSVC builds and the plan/runtime/CLI/contract CTests passed in Debug and
Release. The Windows Python contract suite contains 9 tests, including byte-exact
C++/PowerShell trace comparison for U02 and the partial Inspector case. ImGui
probe tests additionally exercise real mouse drags on Translation X, Rotation Y,
and Scale Z, matching IDs, rounded bounds, and disabled controls.

U02 regression replays also passed in both configurations: 29 steps, 91 phases,
57 injected phases, two fresh processes, and 58 PNGs per configuration:

- `G:\Temp\mgif-u02-regression-debug-20260906-r1`
- `G:\Temp\mgif-u02-regression-release-20260906-r1`

The U02 plan SHA remains
`15b7e1037139631e477b6e2ffb9bc0421ccb6932c5616ba2a3ef46c664769526`.

## Remaining Work

The later [native Gizmo development record](u03-gizmo-os-replay.md) covers a real
Y-translation outside-release/Undo/Redo subset and subsequent product fixes.
The artifacts above remain the original Inspector-only evidence snapshot.

Full U03 is not verified. Earlier full-U03 runs using synthetic passive
observations must not be reused as viewport/gizmo acceptance evidence.
The remaining work requires real viewport picking classification, gizmo component
geometry and input-owner/preview/terminal observations, toolbar setting changes,
outside release, and Escape/focus-loss branches. Same-frame Escape/release needs
actual shared-frame event evidence, not two separately acknowledged phases.

Reconcile the task prose's O06 commit prediction with the existing native
manifest's detach/no-history oracle before accepting those terminal branches.
Do not modify immutable historical evidence. Broader visual/DPI acceptance and
the unfiltered runtime matrix remain separate requirements; the profiler overlay
and clipped UI labels visible in current captures are not signed off here.
