# U07 Exit-Boundary Live Replay Development Record

Date: 2026-09-08. This is a development record, not a sealed receipt or a
progress-ledger transition. `G8` remains failed and `P9`/`P10` remain blocked.
The bounded live matrix that this fix unblocked is recorded separately in
`u01-u08-live-replay-matrix.md`.

## Observed Failure

`U07-modified-failures` was the only live case whose script ends with an `exit`
command, and the only case that failed in the 2026-09-08 live replay session.

Evidence root `G:\Temp\mgif-u07-live-debug-20260908` (run 11:54:37, harness
kill 11:56:30, executable built 11:14:49):

- `app/steps/` contained `0-capture-start.json` .. `7-new-after-error.json`;
  step 8 (`exit`) never produced a step artifact.
- `stdout.log` ended at `marker=action ... step=7 id=new-after-error accepted=1
  state=NoDocument`, followed only by `[CSM_AUTOMATION]` frame markers to
  `frame=39`. There was no `marker=failed`, no `marker=marker`, and no
  `marker=complete`.
- `manifest.json`, `client.png`, and `sync/` were absent, so the harness died
  with `Timed out waiting for live replay ready marker`.
- `stderr.log` held the three scripted failure strings plus one extra line:
  `Editor: stale editor command sequence`.

The silent stall (no failure receipt, no exception, frames still rendering) is
the signature of a `return` inside the live replay gate rather than a rejected
action: a rejection would have called `failEditorLiveReplay`, written
`state.json`/`result.json`, and logged `marker=failed`.

## Root Cause

Three independent defects each blocked the U07 tail. The script requires
`exit` at step 8 and the terminal screenshot boundary at step 9
(`app/editor/EditorTestDriver.cpp:2059-2062`, and `app/script.json` in every
U07 evidence root).

1. **Uninitialized-adapter gate after a document reset.** Step 7 dispatches
   `newScene`, and `EditorController` handles it as `unload(); return
   accepted();` (`app/editor/EditorController.cpp:1883`). `unload()` fires
   `onDocumentReset`, which resets the renderer adapter
   (`app/editor/EditorRendererAdapter.cpp:882`), leaving `initialized() ==
   false` with no pending publication. With no document there is no projection
   that can ever re-initialize the adapter, but
   `MinimalLatestApp::advanceEditorLiveReplay()` returned silently on
   `!m_editorRendererAdapter.initialized()`, so no later step could dispatch.
   The working tree already contained an unbuilt exemption for the `exit`
   action only (`app/MinimalLatestApp.h:2846`, edited 12:11:42, after the
   11:14:49 executable that produced the failure), which is why the original
   run stalled one step earlier than the rebuilt runs.
2. **`exit` closed the window before the terminal boundary.** The controller's
   exit path calls the installed `requestExit` callback
   (`app/editor/EditorController.cpp:1886`), which was bound directly to
   `glfwSetWindowShouldClose(m_window, true)`. In live replay a scripted `exit`
   is an acceptance assertion, not a teardown request: closing the window ends
   the run loop before step 9 renders, so the boundary marker, the real client
   PNG, and the `mgif-csm-capture-sync-v1` handshake could never be produced.
3. **Harness digest rule rejected a truthful `NoDocument` boundary.** At the
   U07 boundary the authority block legitimately reports `hasDocument=false`,
   `hasProjection=false`, `documentDigest=""`, and `projectionDigest=""`.
   `tools/editor_ui_live_replay.ps1` required every one of those digests to
   match `^[0-9a-f]{64}$` and threw `Live replay state contains a malformed
   digest`.

The extra `stale editor command sequence` line is a separate latent defect: the
scripted driver stamps `command.sequence = action.step`
(`app/editor/EditorTestDriver.cpp:1360`) while the UI stamps its own
independent counter (`app/editor/SceneEditorUi.cpp:578`, submitted at `:754`),
and `EditorController::dispatch` rejects any sequence `<= m_commandSequence`
(`app/editor/EditorController.cpp:1543-1547`). Both spaces share one counter, so
a UI-originated command during a scripted run can be dropped as stale. It did
not decide the U07 outcome and it did not recur after the fix (see below), but
it remains unfixed.

## Repair Sequence And Evidence

Each rebuild moved the failure exactly one boundary further, which pins each
cause to its own run. Build tree: `G:\Temp\mgif-p6-90-msvc-20260905T231500Z-Debug`,
target `editor_ui_live_replay`.

| Run | Evidence root | Change under test | Observed terminal boundary |
| --- | --- | --- | --- |
| 11:54 | `G:\Temp\mgif-u07-live-debug-20260908` | none (11:14 executable) | step 7; step 8 never dispatched; harness timeout |
| 12:48 | `G:\Temp\mgif-u07-live-debug-20260908-r2` | exit exemption built + deferred exit | step 8 `exit` accepted and written, `marker=exit-deferred frame=9`; step 9 never dispatched; harness timeout |
| 12:54 | `G:\Temp\mgif-u07-live-debug-20260908-r3` | + `NoDocument` renderer settlement | step 9 marker accepted, `marker=focus-audit applied=1 found=1 active=1 dock_visible=1`, `marker=capture-ready`; harness threw on empty digests |
| 12:56 | `G:\Temp\mgif-u07-live-debug-20260908-r4` | + harness `NoDocument` digest rule | `PASS_BOUNDED_LIVE_PROOF` |

The `-r2` and `-r3` roots are diagnostic. `-r4` is the accepted run: 10 step
artifacts (`0-capture-start.json` .. `9-boundary-marker.json`), `client.png`
(30776 bytes, 1280x720, `print-window`), `sync/`, `manifest.json`, terminal
state `NoDocument`, `result.status=passed`, `complete=true`, and a `stderr.log`
containing only the three scripted failure strings — no `stale editor command
sequence`, no validation diagnostics.

## Source Changes

`app/MinimalLatestApp.h`:

- `requestExit` now defers the physical close during live replay, records
  `m_editorLiveExitRequested`, and logs
  `[EDITOR_LIVE_REPLAY] marker=exit-deferred case=... frame=...`
  (`:260-277`, member at `:1847`).
- The marker stage closes the window on `autoExit || m_editorLiveExitRequested`
  (`:3015`), so a case that asserted `exit` still terminates the process after
  its boundary handshake.
- `advanceEditorLiveReplay()` treats a document-less controller with no pending
  publication as settled, so the terminal `exit` and boundary steps can run
  after a document reset (`rendererSettledWithoutDocument`, `:2855-2863`). The
  authoritative-rebuild branch is unchanged and still fails closed.

`tools/editor_ui_live_replay.ps1`:

- Empty authority digests are accepted only when the terminal state is
  `NoDocument` **and** `hasDocument`/`hasProjection` are both false
  (`$NoDocumentBoundary`, `:463-481`). `renderer.runtime_content_digest` still
  must be 64 lowercase hex in every case, and every document-bearing boundary
  still must publish full digests.

No controller, driver, UI, or renderer contract was changed. No sequence-space
repair was attempted in this pass.

## Limitations

- The proof is bounded: typed commands are dispatched directly at application
  frame boundaries, no OS click and no native dialog is claimed, and the
  screenshot marker remains boundary-only evidence rather than an image
  assertion.
- The shared command-sequence space between the scripted driver and the UI
  (`EditorTestDriver.cpp:1360` vs `SceneEditorUi.cpp:578`) is still a latent
  collision risk for any case that mixes UI-originated submits with scripted
  steps.
- U07 was verified in Debug at 1280x720 only. The Release configuration and the
  wider resolution/DPI combinations required by the parity task remain unrun.
- No reference manifest, progress ledger, `G8`/`P9`/`P10` state, or release
  policy was changed by this record.
