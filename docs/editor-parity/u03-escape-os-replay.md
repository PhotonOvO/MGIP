# U03 Native Escape OS Replay Development Record

Date: 2026-09-06. This is a development record, not a sealed receipt or a
progress-ledger transition. Full U03 is not accepted; G8 remains failed and
P9/P10 remain blocked.

## Verified Scope

`U03-gizmo-escape` completed on Windows/MSVC Debug and Release at 1280x720, 96 DPI:
5 steps, 15 ready/continue exchanges, 8 planned SendInput phases, 10 before/after
PNGs, and one clean process per configuration. Both manifests report
`PASS_TRUE_OS_REPLAY_PARTIAL`, `parentCase=U03-transform-gizmo`, and
`coverageComplete=false`.

The replay selects `Selected` through its real Scene Graph row, starts a drag
on the classified, drawn Y-translation component, moves outside the observed
viewport, and presses Escape while the left mouse button remains down. Only
after the detached state is observed does it release Escape, then inject the
separate physical left-button release.

| Observation | Local Translation | Selection / Capture | Accepted Actions |
| --- | --- | --- | --- |
| Before drag | 0, 0, 0 | Node / Off | None |
| Before Escape | 0, 1.4937688105067666, 0 | Node / On | 1 begin, 1 preview |
| After Escape | 0, 1.4937688105067666, 0 | None / Off | 1 detach, 0 commit |
| After late release | 0, 1.4937688105067666, 0 | None / Off | Unchanged |

Rotation remains `0,0,0,1`, scale remains `1,1,1`, and X/Z translation and
hierarchy order are unchanged. Preview advances document revision from 0 to 1;
Escape advances selection revision from 1 to 2 without changing the document
again. Undo/Redo counts remain zero and both history descriptions remain empty.
The detached attachment is null, but `gestureNode` retains the actual Node
identity so the final TRS comes from that authoritative node, not default values.

Both configurations agree on the states above. The terminal journal contains
exactly `escape-pressed`, sequence 59, input frame 22 in each run. It remains
identical through the late-release and final boundaries.
`escapePressCount=1`; commit, latch, focus-loss, suppressed-release, and
active-gesture delivered-release counts are zero. The latter does not mean no
physical release occurred: `late-release/release` records one real left-up
SendInput, after the gesture has already detached.

## Evidence

Final evidence roots:

- Debug: `G:\Temp\mgif-u03-escape-debug-20260906-r1`
- Release: `G:\Temp\mgif-u03-escape-release-20260906-r1`

Each contains `manifest.json`, `app/main/result.json`, per-phase observations and
input receipts, PNGs, and the scoped source identity record. Both app results
report complete/pass and 15 validated receipts.

Plan SHA-256:
`3d1114057328c871ef8bacafa17e434d34eae124a9234ddc8e3e204bdc10d879`.

Shared enumerated source identity SHA-256:
`2abc24e322485434555b2c7600c862bbcb82af846c604a65a35f822c6263394f`.
All 83 enumerated files were checked against the final workspace after both
configurations ran. This is a scoped source set, not an exhaustive repository
closure.

Debug executable SHA-256:
`ca3cd81344571ca614702a34faac57304690a266e7435088330ab507e4afd815`.

Release executable SHA-256:
`e543d43db7d886eecbdf9ef1aac2787b7ea7db98f4f9c9b9d85af228d505c300`.

MSVC/Ninja build roots:

- Debug: `G:\Temp\mgif-p6-90-msvc-20260905T231500Z-Debug`
- Release: `G:\Temp\mgif-p6-90-msvc-20260905T232000Z-Release`

## Implementation And Regression

- Escape key-down/key-up are separate, single native input events. Held keys
  receive best-effort cleanup on failure; the accepted sequence contains no
  invented release or direct replay callback.
- Active Gizmo Escape ownership prevents the UI clear-selection shortcut from
  invalidating the attachment and erasing the terminal batch before `closeFrame`.
  Recoverable rejected previews retain that ownership and the publication
  baseline while the native gesture remains active.
- Runtime and harness checks bind actual gesture identity, fresh frame/authority
  observations, Vulkan's NDC Y convention, exact terminal provenance, retained
  TRS, and the empty history envelope. Missing callbacks cannot satisfy the
  expected action counts.
- The actual-viewport pixel mask, native ownership, foreground, DPI, and
  persisted capture checks remain enabled.

Both configurations passed 9 related CTests, including the 13-test Windows
Python/MSVC contract suite. The RHI boundary guard and configured editor
dependency preflight also passed. JUnit results:

- `G:\Temp\mgif-u03-escape-validation-debug-20260906.xml`
- `G:\Temp\mgif-u03-escape-validation-release-20260906.xml`

The same source identity also passed these real OS regressions:

- Translate: `G:\Temp\mgif-u03-escape-translate-regression-debug-20260906-r1`
- Inspector: `G:\Temp\mgif-u03-escape-inspector-regression-debug-20260906-r1`
- U02: `G:\Temp\mgif-u03-escape-u02-regression-debug-20260906-r1`
- Translate: `G:\Temp\mgif-u03-escape-translate-regression-release-20260906-r1`
- Inspector: `G:\Temp\mgif-u03-escape-inspector-regression-release-20260906-r1`
- U02: `G:\Temp\mgif-u03-escape-u02-regression-release-20260906-r1`

Their existing plan hashes are unchanged. Translate remains 6 steps/16 phases,
Inspector 8/26, and U02 29/91 with two fresh processes per configuration.

## Remaining Work

O06 task prose now follows the existing observed-authoritative disposition:
both same-frame Escape/release orders retain preview and detach without transform
history. This Escape-only replay does not prove either same-frame native order.
Both MGIF orders now have separate Debug and Release evidence in the
[same-frame OS replay record](u03-same-frame-os-replay.md). Reference
private-callback/direct-history-memory debt,
O07 focus-loss partitions, and other U03 coverage remain pending.
No reference artifact or progress-ledger status is changed by this record.

No complete visual/DPI/performance or RenderDoc acceptance is claimed. The
displaced fixture is partly clipped at the top in the final screenshot, and
existing clipped Browser labels are not signed off by this input proof.
