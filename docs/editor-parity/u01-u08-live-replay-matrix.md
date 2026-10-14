# U01-U08 Bounded Live Replay Matrix Development Record

Date: 2026-09-08. This is a development record, not a sealed receipt or a
progress-ledger transition. `G8` remains failed and `P9`/`P10` remain blocked.
The U07 repair that this matrix depends on is recorded in
`u07-exit-boundary-live-replay.md`.

## Session Context

The morning live session ran on executable SHA-256
`0b17a15120c446918de04db026ac5c9e6acd7c6426ed7c29ec897419ded960ef` (source
identity `3aa80888e69822e276064820279f9311345acb5ffa9bba0961a7fdd7884754e4`):

| Case | Evidence root | Result |
| --- | --- | --- |
| `U04-material` | `G:\Temp\mgif-u04-live-debug-20260908` | failed at 11:02:49 with `Editor: transaction texture upload validation failed: texture=0 code=payload-range detail=plan and asset payload ranges are not the same bounded slice`; no step artifacts, no manifest |
| `U04-material` | `G:\Temp\mgif-u04-live-debug-20260908-r2` | `PASS_BOUNDED_LIVE_PROOF`, 9 steps, 11:31:22 |
| `U05-camera-light-vis` | `G:\Temp\mgif-u05-live-debug-20260908` | `PASS_BOUNDED_LIVE_PROOF`, 11 steps, 11:48:57 |
| `U06-merge-compact-save` | `G:\Temp\mgif-u06-live-debug-20260908` | `PASS_BOUNDED_LIVE_PROOF`, 6 steps, 11:53:24 |
| `U07-modified-failures` | `G:\Temp\mgif-u07-live-debug-20260908` | failed at 11:56:30; step 8 `exit` never dispatched, no manifest/PNG/sync |
| `U08-large-unicode-dpi` | - | not run |

Those roots are bound to a superseded executable and are kept as diagnostic
history only. After the U07 repair the whole matrix was rerun on one binary so
that every accepted case shares a single executable and source identity.

## Authoritative Matrix After The U01 Empty-Start Repair And The Blank-Capture Gate

This section supersedes every matrix below it. Three changes landed after the
line-ending normalization pass:

1. `U01-empty-new-open` now runs live. `app/MinimalLatestApp.h` gained an
   empty-start bootstrap: the U01 fixture stays bound for the scripted
   `open-valid` step but is not preloaded, the document-less ready gate no
   longer requires `session()` or `projection()`, and no scene, node,
   material, camera, light, or animation binding is fabricated before the
   scripted open.
2. The harness pixel audit was hardened twice after a capture-environment
   fault produced blank frames that the original audit accepted
   (`docs/editor-parity/capture-blank-environment-fault.md`). It now measures
   `edgeTransitions` and gates on `nonBlackRatio >= 0.05`,
   `colorfulRatio >= 0.02`, and `edgeRatio >= 0.03`, which are ratios of the
   sampled pixel count so the gate holds at every client extent.
   `luminanceMean` and `luminanceStdDev` are recorded but not gated.
3. The boundary capture retries up to 12 times without relaxing the audit and
   records `screenshot.captureAttempts`; callers rerun the whole case when a
   run lands inside a blank epoch.

Thirty-two roots are accepted: eight cases, two configurations, two client
extents. All are `PASS_BOUNDED_LIVE_PROOF`, all thirty-two `client.png`
SHA-256 values are distinct, and all thirty-two carry one scoped source
identity:

- Scoped source identity:
  `83687dfa404296649bbdf223b98947978779adb7e692a65486668e06acf69d52`
- Debug executable: 21488128 bytes, SHA-256 `443430691e80efb443e8f3773093df0c572f7fc8330fb5934e68b49556dbfa5b`
- Release executable: 6097408 bytes, SHA-256 `31d7b2f02a1d8f389b54f2d2cacd6aea41a2484e101c0beecb45715125d07221`

Identity coherence was re-checked after all documentation edits: a fresh
preflight probe (`G:\Temp\mgif-identity-drift-check`, which the harness rejects
before launching because 2560x1440 is unreachable) recomputes the scoped
identity from 60 records as
`83687dfa404296649bbdf223b98947978779adb7e692a65486668e06acf69d52`, which is
exactly the identity carried by all thirty-two accepted roots. The harness has
not drifted since the matrices were collected.

The executables are unchanged by the audit work; only the harness source
changed, which is why the scoped source identity moved. Both were built with
the configured production-editor dependency preflight enabled and passing,
with `VK_LAYER_KHRONOS_validation` and its synchronization mode, and with
`messageFilter=null`.

### Matrix 1: 1280x720 client extent

Debug roots are `G:\Temp\mgif-<case>-live-debug-20260908-H<attempt>`, Release roots are
`G:\Temp\mgif-<case>-live-release-20260908-H<attempt>`, and every accepted attempt in this
matrix is `1` except where the Attempt column says otherwise.

| Case | Steps | Terminal state | Attempt (Debug / Release) | edgeRatio (Debug / Release) | colorfulRatio (Debug / Release) | Mean luminance (Debug / Release) |
|---|---:|---|---|---|---|---|
| `U01-empty-new-open` | 5 | `ReadyClean` | 2 / 1 | 0.1371 / 0.1291 | 0.3504 / 0.4332 | 52.1 / 49.5 |
| `U02-hierarchy` | 8 | `ReadyModified` | 1 / 1 | 0.1045 / 0.1297 | 0.6238 / 0.4007 | 43.8 / 50.7 |
| `U03-transform-gizmo` | 13 | `ReadyModified` | 1 / 1 | 0.1064 / 0.1370 | 0.6416 / 0.3885 | 43.7 / 50.7 |
| `U04-material` | 9 | `ReadyModified` | 1 / 1 | 0.1087 / 0.1206 | 0.6289 / 0.3767 | 44.1 / 49.7 |
| `U05-camera-light-vis` | 11 | `ReadyModified` | 1 / 1 | 0.1091 / 0.1303 | 0.6243 / 0.3621 | 44.1 / 49.7 |
| `U06-merge-compact-save` | 6 | `ReadyClean` | 1 / 1 | 0.1053 / 0.1240 | 0.6155 / 0.3504 | 44.2 / 49.7 |
| `U07-modified-failures` | 10 | `NoDocument` | 1 / 1 | 0.1156 / 0.1047 | 0.6049 / 0.6245 | 45.3 / 43.8 |
| `U08-large-unicode-dpi` | 6 | `ReadyModified` | 1 / 1 | 0.1080 / 0.1149 | 0.5850 / 0.6371 | 45.1 / 44.8 |

Edge ratios span 0.1045-0.1371, colourful ratios span 0.3504-0.6416, and the
lowest `nonBlackRatio` is 0.9950. Every value clears the gate with at least
1.5x margin; the weakest blank frame measured this session scored
`edgeRatio=0.0195`, `colorfulRatio=0.0042`, `nonBlackRatio=0.019`.

### Matrix 2: 1920x1080 client extent

Debug roots are `G:\Temp\mgif-<case>-live-debug-20260908-H1080-<attempt>`, Release roots are
`G:\Temp\mgif-<case>-live-release-20260908-H1080-<attempt>`, and every accepted attempt in this
matrix is `1` except where the Attempt column says otherwise.

| Case | Steps | Terminal state | Attempt (Debug / Release) | edgeRatio (Debug / Release) | colorfulRatio (Debug / Release) | Mean luminance (Debug / Release) |
|---|---:|---|---|---|---|---|
| `U01-empty-new-open` | 5 | `ReadyClean` | 1 / 1 | 0.1031 / 0.0833 | 0.4420 / 0.4031 | 47.0 / 46.5 |
| `U02-hierarchy` | 8 | `ReadyModified` | 1 / 1 | 0.1074 / 0.0858 | 0.4250 / 0.4053 | 47.0 / 49.6 |
| `U03-transform-gizmo` | 13 | `ReadyModified` | 1 / 1 | 0.0977 / 0.0815 | 0.4200 / 0.3883 | 47.0 / 51.5 |
| `U04-material` | 9 | `ReadyModified` | 1 / 1 | 0.1021 / 0.1076 | 0.4126 / 0.4837 | 46.8 / 46.6 |
| `U05-camera-light-vis` | 11 | `ReadyModified` | 1 / 1 | 0.0936 / 0.1116 | 0.4038 / 0.4694 | 47.1 / 46.7 |
| `U06-merge-compact-save` | 6 | `ReadyClean` | 1 / 1 | 0.0934 / 0.1038 | 0.4027 / 0.4420 | 46.2 / 47.1 |
| `U07-modified-failures` | 10 | `NoDocument` | 1 / 1 | 0.0893 / 0.1078 | 0.3927 / 0.4250 | 46.9 / 47.0 |
| `U08-large-unicode-dpi` | 6 | `ReadyModified` | 1 / 1 | 0.0946 / 0.0983 | 0.4060 / 0.4200 | 46.0 / 47.0 |

Edge ratios span 0.0815-0.1116, colourful ratios span 0.3883-0.4837, and the
lowest `nonBlackRatio` is 0.9881. Every value clears the gate with at least
1.5x margin; the weakest blank frame measured this session scored
`edgeRatio=0.0195`, `colorfulRatio=0.0042`, `nonBlackRatio=0.019`.

### Runtime Validation Across All Thirty-Two Roots

Each root records its validation configuration in `manifest.validation` as
`{"layers": ["VK_LAYER_KHRONOS_validation"], "synchronization": true,
"messageFilter": null}`. Scanning all sixty-four captured logs (32 roots x
`stderr.log` + `stdout.log`) for `VUID-`, `SYNC-HAZARD`, `Validation Error`,
`device lost`, `VK_ERROR`, and `stale editor command sequence` returns zero
occurrences, in both configurations and at both extents. This is bounded to the
live acceptance target; it is not a clean-runtime claim for the whole runtime
surface.

The 1920x1080 matrix is not a magnified copy of the 1280x720 matrix: sample
counts rise from 57,600 to 129,600, unique colours rise (for example
`U03-transform-gizmo` Release goes from 92 to 186), and every frame hash
differs, so the second extent exercises real surface recreation and
attachment resizing rather than reusing one buffer.

### Resolution axis beyond 1920x1080

2560x1440 is unreachable on this host and the harness now says so before
launching. The host virtual screen is 2560x1440 with a work area of
2560x1392, and the default `WM_GETMINMAXINFO` max track size is the virtual
screen, so a decorated window can never have a 2560x1440 client: measured
Debug and Release both produced a 2560x1421 client because the non-client
frame is 19 px tall, and moving the window did not change it. `G:\Temp\run-1440p-*`
records the runs; `G:\Temp\mgif-1440p-preflight-probe` records the fast
preflight rejection. Extent validation stays strict instead of tolerating the
clamped size. Closing this axis needs either a display larger than 2560x1440
or a borderless client policy, which is an owner decision.

DPI remains unreachable for the reason recorded in
`docs/editor-parity/g8-recovery-gap-analysis.md`: the interactive session is
per-monitor DPI aware and `SetProcessDpiAwarenessContext` fails from a child
process, so `monitorDpi` stays 144 and `uiScaleFactor` stays 1.5.

## Current Identities After The Line-Ending Normalization

The 2026-09-08 repository hygiene pass (`docs/repository-hygiene-20260908.md`)
stripped CRLF from 72 tracked files, including `app/MinimalLatestApp.h` and
`CMakeLists.txt`. File content is unchanged, but the scoped source identity is
byte-derived, so both configurations were rebuilt and the entire matrix was
rerun. Results are identical to the pre-normalization run, and the identities
below supersede the ones in the following two sections.

- Scoped source identity:
  `3f1c0752f4a63ef9cb7478ad67ec44a61053584f703a1bec6d3dcfad62fcce19`; the
  emitted `source-identity.json` is 15979 bytes with SHA-256
  `f9305945b54bbd4120bf2b7a38d1a2557df042f9de424db31e6f854c32313976` in every
  accepted root of both configurations.
- Debug executable: `G:\Temp\mgif-p6-90-msvc-20260905T231500Z-Debug\editor_ui_live_replay.exe`,
  21487104 bytes, SHA-256
  `eed637f429e1cf867a05329315edfc28b0ca5d3e0484d1bc13a998891d2170f4`,
  `CMakeCache.txt` SHA-256 `877f60b2d929540bcd0aa41cf4177089c936f52224be3d116bbc37db2511311a`
  (unchanged).
- Release executable: `G:\Temp\mgif-p6-90-msvc-20260905T232000Z-Release\editor_ui_live_replay.exe`,
  6096896 bytes, SHA-256
  `9dc373a88ebc5ca84646927555f39aa1a3d9f47c236a0d074c8ab2788ea8ff45`,
  `CMakeCache.txt` SHA-256 `79a3b2ef6085bd4c45df1e7d3a740c90082af1c664b6cfd8c85a2db86dd4d7d9`
  (unchanged).
- Rebuild scope after normalization: Debug 173 ninja steps, Release 169, both
  exit 0 (`G:\Temp\build-after-eol-normalization-debug.log`,
  `G:\Temp\build-after-eol-normalization-release.log`).

Evidence roots are `G:\Temp\mgif-u0N-live-debug-20260908-norm` and
`G:\Temp\mgif-u0N-live-release-20260908-norm`.

| Case | Steps | Status (Debug and Release) | Terminal state (identical in both) |
| --- | --- | --- | --- |
| `U02-hierarchy` | 8 | `PASS_BOUNDED_LIVE_PROOF` | `ReadyModified`, revision 5, 2 scenes, `canUndo=true` |
| `U03-transform-gizmo` | 13 | `PASS_BOUNDED_LIVE_PROOF` | `ReadyModified`, revision 3, `canUndo=true` |
| `U04-material` | 9 | `PASS_BOUNDED_LIVE_PROOF` | `ReadyModified`, revision 4, `canUndo=true` |
| `U05-camera-light-vis` | 11 | `PASS_BOUNDED_LIVE_PROOF` | `ReadyModified`, revision 6, `canUndo=true` |
| `U06-merge-compact-save` | 6 | `PASS_BOUNDED_LIVE_PROOF` | `ReadyClean`, revision 0, `canUndo=false` |
| `U07-modified-failures` | 10 | `PASS_BOUNDED_LIVE_PROOF` | `NoDocument`, `hasDocument=false` |
| `U08-large-unicode-dpi` | 6 | `PASS_BOUNDED_LIVE_PROOF` | `ReadyModified`, revision 1, `canUndo=true` |
| `U01-empty-new-open` | 1 | failed at frame 0 in both | see U01 Gap |

The normalized Release roots again contain no `VUID-`, `SYNC-HAZARD`,
`Validation Error`, or `device lost` record. Debug `stderr.log` is empty for
every case except U07 (197 bytes: the three scripted failure strings) and U01
(426 bytes: its failure markers).

## Debug Bound Identity (pre-normalization)

- Executable: `G:\Temp\mgif-p6-90-msvc-20260905T231500Z-Debug\editor_ui_live_replay.exe`,
  21487104 bytes, SHA-256
  `123d1361aa66cda88fe1029e798d00e183539b067affecb59cc3d3c804a73cf2`
- Scoped source identity:
  `34e32dc9b4b459cc63bf0de3969f41e763054da6bac4cf8de961d28de77ef6d1`; the
  emitted `source-identity.json` artifact is 15979 bytes with SHA-256
  `0b99ea3abcb85d422034c9266689043e38a74e4439bb5dd93fc9aa2b1d050612` in every
  accepted root. This is a scoped identity, not an exhaustive repository
  closure.
- Configured build: `CMakeCache.txt` SHA-256
  `877f60b2d929540bcd0aa41cf4177089c936f52224be3d116bbc37db2511311a`,
  `CMAKE_BUILD_TYPE=Debug`, `BUILD_TESTING=ON`,
  `DEMO_SKIP_EDITOR_DEPENDENCY_PREFLIGHT=OFF`. The configured production-editor
  dependency preflight passed on every build.
- Harness: `tools/editor_ui_live_replay.ps1`, client contract 1280x720,
  `VK_LAYER_KHRONOS_validation` with synchronization enabled and no message
  filter.

## Debug Matrix Result (pre-normalization)

Seven of eight cases reached `PASS_BOUNDED_LIVE_PROOF` on this identity. Digest
and PNG values below are 16-hex prefixes; the full values are in each root's
`manifest.json` under `authority` and `screenshot`.

| Case | Evidence root | Steps | Terminal state | Boundary frame | Document digest | Client PNG |
| --- | --- | --- | --- | --- | --- | --- |
| `U02-hierarchy` | `G:\Temp\mgif-U02-live-debug-20260908-fix` | 8 | `ReadyModified` | 14 | `e07381b212e7520d` | `7e22cea1c9b5e8ec` |
| `U03-transform-gizmo` | `G:\Temp\mgif-U03-live-debug-20260908-fix` | 13 | `ReadyModified` | 17 | `193e6d8b8cc8ada2` | `703ab6be12950a9b` |
| `U04-material` | `G:\Temp\mgif-u04-live-debug-20260908-fix` | 9 | `ReadyModified` | 14 | `6b86e346f424b8b7` | `be0cd71cba097caf` |
| `U05-camera-light-vis` | `G:\Temp\mgif-u05-live-debug-20260908-fix` | 11 | `ReadyModified` | 18 | `4e55a433c60b9fda` | `98e16f95e9edf3de` |
| `U06-merge-compact-save` | `G:\Temp\mgif-u06-live-debug-20260908-fix` | 6 | `ReadyClean` (`saved.glb`) | 9 | `f30191e625ad3193` | `129ae5c85076c59d` |
| `U07-modified-failures` | `G:\Temp\mgif-u07-live-debug-20260908-r4` | 10 | `NoDocument` | 12 | empty (no document) | `04cf18f9c557f31d` |
| `U08-large-unicode-dpi` | `G:\Temp\mgif-u08-live-debug-20260908` | 6 | `ReadyModified` | 8 | `17e26e81436618a4` | `1f270b9c52e0abe1` |

Every accepted root holds `manifest.json`, `client.png`, `source-identity.json`,
`stdout.log`, `stderr.log`, `sync/`, and one step artifact per script step. In
all seven, `projectionDigest` equals `renderer.runtime_content_digest`, the
handshake is `mgif-csm-capture-sync-v1` at `phase=post-render`,
`dispatchMode=direct-typed-command`, `osClick=false`, `nativeDialog=false`,
`screenshotClaim=real-client-png-at-boundary`, and the focus audit reports
`applied=1 found=1 active=1 hidden=0 dock_visible=1` with
`uiFocus.method=acceptance-imgui-window-focus`. U07's terminal
`runtime_content_digest` is 64 zeros, which is the truthful empty-scene value
after `newScene`.

## Release Matrix Result (pre-normalization)

The same source identity was rebuilt in the Release tree and the matrix was
rerun, so both configurations are bound to one scoped source identity.

- Executable: `G:\Temp\mgif-p6-90-msvc-20260905T232000Z-Release\editor_ui_live_replay.exe`,
  6096896 bytes, SHA-256
  `345dfe2feb1e8393e32b3f674ba372fb47f20ddf67758ef426d14923ee3579c6`
- Configured build: `CMakeCache.txt` SHA-256
  `79a3b2ef6085bd4c45df1e7d3a740c90082af1c664b6cfd8c85a2db86dd4d7d9`,
  `CMAKE_BUILD_TYPE=Release`, `BUILD_TESTING=ON`,
  `DEMO_SKIP_EDITOR_DEPENDENCY_PREFLIGHT=OFF`
- Source identity: unchanged,
  `34e32dc9b4b459cc63bf0de3969f41e763054da6bac4cf8de961d28de77ef6d1`

| Case | Evidence root | Steps | Status |
| --- | --- | --- | --- |
| `U02-hierarchy` | `G:\Temp\mgif-u02-live-release-20260908` | 8 | `PASS_BOUNDED_LIVE_PROOF` |
| `U03-transform-gizmo` | `G:\Temp\mgif-u03-live-release-20260908` | 13 | `PASS_BOUNDED_LIVE_PROOF` |
| `U04-material` | `G:\Temp\mgif-u04-live-release-20260908` | 9 | `PASS_BOUNDED_LIVE_PROOF` |
| `U05-camera-light-vis` | `G:\Temp\mgif-u05-live-release-20260908` | 11 | `PASS_BOUNDED_LIVE_PROOF` |
| `U06-merge-compact-save` | `G:\Temp\mgif-u06-live-release-20260908` | 6 | `PASS_BOUNDED_LIVE_PROOF` |
| `U07-modified-failures` | `G:\Temp\mgif-u07-live-release-20260908` | 10 | `PASS_BOUNDED_LIVE_PROOF` |
| `U08-large-unicode-dpi` | `G:\Temp\mgif-u08-live-release-20260908` | 6 | `PASS_BOUNDED_LIVE_PROOF` |
| `U01-empty-new-open` | `G:\Temp\mgif-u01-live-release-20260908` | 1 | failed at frame 0 (see U01 Gap) |

All seven Release PNGs are 1280x720 `print-window` captures with audits in the
same range as Debug (for example U07: `sampled=57600 nonBlack=3615
colorful=1090 uniqueColors=55`). Across every Release root, `stdout.log` and
`stderr.log` contain no `VUID-`, `SYNC-HAZARD`, `Validation Error`, or
`device lost` record even though `VK_LAYER_KHRONOS_validation` ran with
synchronization enabled and `messageFilter=null`. The only Release stderr
content is U07's three scripted failure strings and U01's failure markers. This
is bounded to the live replay acceptance target at one resolution; it is not a
clean-Release claim for the whole runtime surface.

## U01 Gap (closed)

Closed. The empty-start bootstrap described in the authoritative section above
lets `U01-empty-new-open` run live in both configurations: 5 steps,
`status=PASS_BOUNDED_LIVE_PROOF`, terminal `ReadyClean`, with genuine boundary
captures (Debug 6458 edges, Release 6219 edges). The headless
`editor_ui_replay_tests` receipt still reports
`acceptanceVerdict=INCOMPLETE_VISUAL_COVERAGE`, which is a separate claim about
visual coverage, not about U01 execution.

Historical record of the original failure follows.

`U01-empty-new-open` failed at frame 0 in
`G:\Temp\mgif-U01-live-debug-20260908-fix`:

- `stdout.log`: `marker=script-ready case=U01-empty-new-open steps=5`, then
  `marker=scene-ready`, then shutdown.
- `stderr.log`: `marker=failed case=U01-empty-new-open frame=0 reason=settled
  post-action state did not match expectation`, `Frame failed during phase
  EditorLiveReplay: ...`, `marker=process-failed`.
- `app/steps/0-capture-start.json`: `status=invalid`, `accepted=false`,
  `beforeState=ReadyClean`, `afterState=ReadyClean`, while the script expects
  `NoDocument`.

This is a live-bootstrap scope gap, not a controller defect. The live harness
always resolves a case fixture and preloads it
(`app/MinimalLatestApp.h:644`, fixture mapping at `:2300-2325`), and the
automation scene-ready gate requires a published scene whose source matches that
fixture before `initializeEditorLiveReplay()` runs (`:6026`). U01's contract
starts with no document and opens the candidate document mid-script
(`app/editor/EditorTestDriver.cpp:1788-1806`), so step 0 can never observe
`NoDocument` in live mode. The failure is configuration-independent: the
Release run in `G:\Temp\mgif-u01-live-release-20260908` reports the same
`frame=0 reason=settled post-action state did not match expectation`. Supporting U01 live needs an empty-start bootstrap
(no preloaded fixture, a document-less ready gate, and a mid-script open that
publishes through the renderer adapter); that is a separate change and was not
attempted here.

The headless controller replay does cover U01. `editor_ui_replay_tests.exe`
(SHA-256 `6eb83163e78a6d7a4c21a9c4a0e0cbe0ed9df25d070dad250ce4c4b9452ea7cf`,
built from the same tree) exited 0 with
`Editor UI controller replay passed 8 scenario checks; visual verdict remains
INCOMPLETE` and the emitted receipt
`{"kind":"mgif.editor-ui-controller-replay","mockHost":false,
"screenshotEvidence":false,"scenarioCount":8,"executionPassed":true,
"acceptanceVerdict":"INCOMPLETE_VISUAL_COVERAGE"}`.

## Limitations

- Bounded live proof only: typed commands are dispatched directly at
  application frame boundaries. No OS click, no native dialog, and no image
  assertion is claimed; the screenshot marker remains boundary-only evidence.
- Debug and Release at 1280x720 and one DPI setting only. The parity task's
  `1920x1080`/`2560x1440` and 100/125/150/200% DPI combinations remain unrun;
  `tools/editor_ui_live_replay.ps1` exposes `-Width`/`-Height` but no DPI
  control, so the DPI axis is not harness-reachable yet.
- U01 now has live coverage in both configurations; the empty-start bootstrap
  exists and the fixture is bound but not preloaded for that case.
- The scripted driver and the UI still share one command-sequence space
  (`app/editor/EditorTestDriver.cpp:1360` vs `app/editor/SceneEditorUi.cpp:578`,
  guard at `app/editor/EditorController.cpp:1543`), which produced a
  `stale editor command sequence` rejection in the pre-fix U07 run.
- No reference manifest, progress ledger, `G8`/`P9`/`P10` state, or release
  policy was changed by this record.
- The pre-normalization roots (`*-fix`, `*-r4`, `*-release-20260908`) remain
  valid diagnostic history but are bound to source identity
  `34e32dc9b4b459cc63bf0de3969f41e763054da6bac4cf8de961d28de77ef6d1`, which no
  longer matches the working tree after the CRLF cleanup. The `*-norm` roots
  match the normalized tree but predate the U01 repair, so they carry no U01
  coverage.
- Roots collected between roughly 20:15 and 23:00 on 2026-09-08 with the tags
  `*-empty3`, `*-u01fix`, `*-u01recheck`, `*-fix2`, `*-retry`, `*-final`,
  `*-final2`, and `*-R1`/`*-R2`/`*-R3` are **not** citable boundary evidence:
  their `client.png` files are uniform white frames that the pre-strengthening
  audit accepted, and some are byte-identical across different cases and
  configurations. `docs/editor-parity/capture-blank-environment-fault.md`
  records the measurements and the invalidation.
- The capture host is intermittently unable to present window content, so a
  matrix run may need whole-case reruns. The strengthened audit guarantees such
  an epoch fails the run instead of recording a blank boundary.
