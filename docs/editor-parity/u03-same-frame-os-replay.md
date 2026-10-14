# U03 Same-Frame Terminal OS Replay Development Record

Date: 2026-09-06. This is a development record, not a sealed receipt or a
progress-ledger transition. Full U03 is not accepted; G8 remains failed and
P9/P10 remain blocked.

## Verified Scope

`U03-gizmo-escape-release` and `U03-gizmo-release-escape` each completed on
Windows/MSVC Debug and Release at 1280x720, 96 DPI: 5 steps, 15 ready/continue
exchanges, 8 planned SendInput phases, 10 before/after PNGs, and one clean process
per case and configuration. All four manifests report `PASS_TRUE_OS_REPLAY_PARTIAL`,
`parentCase=U03-transform-gizmo`, `coverageComplete=false`, and
`sameFrameNativeOrderValidated=true`.

Each run selects the actual `Selected` Scene Graph row, drags the classified
Y-translation component outside the observed viewport, then injects Escape-down
and left-button-up in one ordered, two-event `SendInput` call. The application
routes both real callbacks within the same `EditorInputFrameId` and adapter
`beginFrame`/`closeFrame` interval. This does not claim that both callbacks
arrived during one `glfwPollEvents` invocation.

Both configurations produced the following observations:

| Order | Journal: Raw Callback -> Routed Sequence | Input Frame | Escape Trigger Sequence |
| --- | --- | --- | --- |
| Escape-first | Escape 4 -> 59; release 5 -> 60 | 22 | 59 |
| Release-first | Release 4 -> 59; Escape 5 -> 60 | 22 | 60 |

The pre-batch receipt binds callback-clock boundary 3 and input-frame boundary
21. Both terminal callbacks are therefore newer than that boundary. Raw
callback ordinals and routed input sequence numbers are separate counters;
`lastTerminalSequence` identifies Escape, not necessarily the journal's last
event. The post-batch ready snapshot is rendered at frame 24 and retains the
actual terminal frame 22.

Both orders retain local translation `0,1.4937688105067666,0`, rotation
`0,0,0,1`, and scale `1,1,1`. Selection is cleared, attachment becomes null,
capture is released, and `gestureNode` retains the actual authoritative Node
identity. Document revision is 1 and selection revision is 2. Each run records
1 begin, 1 preview, 1 detach, 1 delivered release, 1 Escape, 0 commit, and
0 latch/focus-loss/suppressed-release actions. Undo/Redo counts remain zero
with empty descriptions; no rollback or transform history entry is introduced.

Subsequent real Escape-up and a redundant real left-button-up advance the raw
callback clock to 7. The terminal journal, its frame and trigger sequence,
retained TRS, selection, document authority, and history remain unchanged.

## Evidence

Final evidence roots:

- Debug, Escape-first: `G:\Temp\mgif-u03-o06-escape-first-debug-20260906-r1`
- Debug, release-first: `G:\Temp\mgif-u03-o06-release-first-debug-20260906-r1`
- Release, Escape-first: `G:\Temp\mgif-u03-o06-escape-first-release-20260906-r1`
- Release, release-first: `G:\Temp\mgif-u03-o06-release-first-release-20260906-r1`

Each contains the manifest, app result, ready/continue records, actual input
batch metadata, PNGs, and enumerated source identity.

Escape-first plan SHA-256:
`a62d1e31d7f211b53a436342b86b6ae0643865d380c0d5a077e5d8a1086bff9b`.

Release-first plan SHA-256:
`605849a5deb99a3b89dcd0e2993a80b5042d6fb5a16a142d77c0e23cbdda9764`.

Shared enumerated source SHA-256:
`5dd793351e56572b3909c975c3b40aad69f0c2051aa7b1d118e0136b491ba7e0`.
The scoped set has 84 files, including `EditorNativeGizmoInput.h`; all were
checked against the final workspace. It is not an exhaustive repository closure.
Referenced artifacts in all four new replay manifests were rehashed after the
runs completed.

Debug executable SHA-256:
`b123de785395e19d3529be48ff74b4493e5f770399f5bfa479b1c421473bf5a7`.

Release executable SHA-256:
`77e577ca1f05b27db0a2b6079d12706b67e546afd3cf6083c73a32ad0c07d813`.

MSVC/Ninja build roots:

- Debug: `G:\Temp\mgif-p6-90-msvc-20260905T231500Z-Debug`
- Release: `G:\Temp\mgif-p6-90-msvc-20260905T232000Z-Release`

## Implementation And Regression

- A bounded production callback queue observes real main-window Escape,
  primary-button, and focus callbacks. Observers are installed before ImGui,
  which chains them. No replay/controller callback is invented or reordered.
- The same-frame Host receipt gate waits for exactly two queued native terminal
  callbacks while continuing to pump the window. It does not open another app
  input frame while waiting. Missing/excess events, overflow, stale provenance,
  incorrect order, and changed terminal state fail validation.
- ImGui can present release-first keyboard input on a later UI frame. The
  native-Escape consumption marker lasts until that actual UI key frame, so
  the UI cannot dispatch a duplicate clear-selection after native detach.
- Ordered draining applies to already-active native gestures. Focus-interrupted
  batches keep the existing polling/suppression policy. This is not a wholesale
  replacement of press, pointer-move, cursor, or focus-loss input handling.
- Native ownership, foreground, DPI, actual-viewport pixel masking, and persisted
  capture checks remain enabled. Held-key cleanup remains available on failure.

Both configurations passed all 10 related CTests, including the 15-test Windows
Python/MSVC contract suite:

- `G:\Temp\mgif-u03-o06-validation-debug-20260906.xml`
- `G:\Temp\mgif-u03-o06-validation-release-20260906.xml`

The ordinary `Demo` and replay targets built successfully in both configurations.
Debug `Demo` initially retained an old `MinimalLatestApp.cpp.obj` referring to
the previous routing signature; only that generated object was removed and
successfully rebuilt. No source compatibility workaround or build-root cleanup
was used. RHI boundary checks and configured editor dependency preflight passed.

The original U02, Inspector, Translate, and Escape-only plan hashes remain
unchanged. These real OS regressions also passed:

- Escape-only: `G:\Temp\mgif-u03-o06-escape-regression-debug-20260906-r1`
- Translate: `G:\Temp\mgif-u03-o06-translate-regression-debug-20260906-r1`
- Inspector: `G:\Temp\mgif-u03-o06-inspector-regression-debug-20260906-r1`
- U02: `G:\Temp\mgif-u03-o06-u02-regression-debug-20260906-r1`
- Escape-only: `G:\Temp\mgif-u03-o06-escape-regression-release-20260906-r1`
- Translate: `G:\Temp\mgif-u03-o06-translate-regression-release-20260906-r1`
- Inspector: `G:\Temp\mgif-u03-o06-inspector-regression-release-20260906-r1`
- U02: `G:\Temp\mgif-u03-o06-u02-regression-release-20260906-r1`

All eight use the same enumerated source identity above. Escape-only remains
5 steps/15 phases, Translate 6/16, Inspector 8/26, and U02 29/91 with two fresh
processes per configuration.

## Remaining Work

These MGIF-side Debug/Release observations prove the two scoped native terminal orders;
they do not establish the reference binary's private callback order or direct
history-memory contents. Existing O06 `conflict`/`disposed_with_debt` and the
associated reference evidence debt remain unchanged.

O07 focus-loss partitions and other U03 coverage remain pending. No complete
visual/DPI/performance or RenderDoc acceptance is claimed, and existing clipping
is not signed off by this input proof. No ledger, reference manifest, or gate
status is promoted by this development record.
