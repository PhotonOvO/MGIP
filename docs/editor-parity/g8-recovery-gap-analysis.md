# G8 Recovery Gap Analysis — 2026-09-08

## Decision

No ledger transition was published today, and no `ENGINEERING PASS` is asserted
for `G8` or a `G8-R1` recovery gate. Two independent blockers stand between the
current tree and an unblocked ledger: a mechanical publication blocker
(Blocker A) and unclosed G8 recovery obligations (Blocker B). Both are recorded
below with evidence so an owner can decide the next authorized step.

Update later the same day: **Blocker A is closed.** The locked specification
bytes were recovered from off-repository fixture copies and restored, and all
four frozen validators now exit 0 on Windows
(`docs/editor-parity/locked-spec-input-restoration.md`). Blocker B is unchanged.

## Authoritative Ledger State (Unchanged)

- `python3 tools/validate_editor_local_progress.py`
  -> `Local editor progress ledger validated: head=1d54cec00c96d704229448564c3b762e869159eb333e8bb2f4e68051b294f00b ready=0`, exit 0
- POSIX `tools/local_editor_progress_ledger.py --repo-root . --check`
  -> `Local progress ledger is valid: bundle=404366e527af67ea2d0bb57ba751be2aa0d2c7812d233ae869c1833a5d5120b7 head=1d54cec0...`, exit 0
- Failed gate task: `G8` = `task_eb85190ed1ac`, dependency `task_99a11c23bb9a`,
  `completedAtUtc = 2026-09-05T08:50:00Z`
- Sealed G8 evidence: report `000e0d8cb0b29a87216956fa89a347a9493ef4450093b35cc0db906d8a61aa36`
  (decision `GATE P8 FAIL/BLOCKED`), verdict
  `1f962e3616fd592ef7421cd9ad16f06aafa455d85c8e245ef1ef31781b782eea`
- `nextRunnableTaskIds = []`; fixed policy unchanged (`F42 BLOCKED`,
  `R24 BLOCKED`, `metal Not Verified`, `releaseDistribution NOT PASS`)

No pointer, state, bundle, mirror, or policy file was modified during this
session.

## Blocker A — Windows Publication Path Was Fail-Closed (CLOSED 2026-09-08)

`tools/local_editor_progress_ledger.py:1745` calls `run_legacy_validator(repo)`
in every mode. That function (`:1590-1598`) returns immediately unless
`os.name == "nt"`, so the frozen legacy ledger is re-validated only on the
Windows host. On Windows it now fails:

```text
Editor progress-ledger validation failed: state policy or policy inputs are stale
```

The check is `tools/validate_editor_progress_ledger.py:1363-1364`, which
recomputes `derive_policy(repo)` (`:569-585`) and requires it to equal the
frozen `state["inputs"]`. `derive_policy` binds three documents:

| Frozen input | Frozen identity | Current identity | Status |
| --- | --- | --- | --- |
| `docs/gltf-scene-editor-parity-task.md` | 165772 bytes, `bfd4b5ffe8d73b9bb887047a8cad5ad96c7184d182eeb752aa47028b05bd679e` | 167073 bytes, `29c32851ef6e072f094627d71d748b2d6ebad3059a5eb714b8a5965c41303721`, mtime `2026-09-06T22:20` | **diverged** |
| `docs/editor-parity/manifest.json` | 56527 bytes, `a5182ddaeb88368b293ee55931e6dd7deec989eb62cda57d5c9c09c65b38b053` | identical | match |
| `docs/editor-parity/LICENSE-SOURCES.md` | 9644 bytes, `b302ff87a11192a284b23dad7baa493826473b98aaf34f288994ee39ffcd3971` | identical | match |

The same locked specification hash `bfd4b5ff...` is also bound by
`tools/validate_editor_p0_artifacts.py:23` (`EXPECTED_SPEC_SHA256`),
`docs/editor-parity/manifest.json:7`, `docs/editor-parity/edge-obligations.json:5`,
`docs/editor-parity/g6-r1-authority-recovery.json:27`, and every frozen legacy
bundle under `docs/editor-parity/progress-ledger/bundles/sha256/`.

The frozen 165772-byte specification was **not recoverable inside the repository
or on `G:`**, which is what made this look permanent:

- no content-addressed copy exists in the repository (a full walk found no file
  of that size and hash);
- `docs/gltf-scene-editor-parity-task.md` is untracked in git (`?? `), so there
  is no committed revision to restore;
- the only other copy on `G:`,
  `/mnt/g/ReferenceOracleRuns/vkgr-50373c7b-input-probe/input-specs/gltf-scene-editor-parity-task.md`,
  is an older 155134-byte revision (`0fe1e3c221a33c2ff9e9e59117b0788feb88c3697a0f53a559236faf0b9a36aa`,
  2026-08-22).

That search was later widened to `%LOCALAPPDATA%\Temp` on `C:` and found the
pinned bytes in three ledger-v2 fixture repositories
(`mgif-ledger-v2-tests-t41g0r9w`, `-0eciy7oe`, `-ubh2id9y`, each at
`repo/docs/gltf-scene-editor-parity-task.md`, 165772 bytes,
`bfd4b5ffe8d73b9bb887047a8cad5ad96c7184d182eeb752aa47028b05bd679e`, mtime
`2026-08-23 14:28:50`). Option 1 below was therefore executed.

Observed consequences today:

- Windows CTest (`G:\Temp\mgif-live-matrix-ctest-debug-20260908.xml`, log
  `G:\Temp\ctest-debug-live-matrix.log`) fails
  `scene_editor.progress_ledger_validator`,
  `scene_editor.progress_ledger_updater_check`,
  `scene_editor.local_progress_ledger_publisher_check`,
  `scene_editor.local_progress_ledger_protocol`,
  `scene_editor.progress_ledger_protocol`, `scene_editor.p0_artifacts`, and
  `scene_editor.p0_artifact_validator`. The protocol-test failures all carry
  `frozen legacy-ledger validation failed: ... state policy or policy inputs are stale`.
- `tools/validate_editor_p0_artifacts.py --repo-root .` exits 1 with
  `P0 artifact validation failed: live task document differs from the manifest specification hash`.
- On POSIX the same publisher `--check` passes and
  `tests/editor/local_progress_ledger_tests.py` is 34/34 `OK` (91 s), because the
  legacy gate is skipped there by design.

Every one of those consequences cleared after the restore: the P0 validator
reports `P0 scaffold validation passed: 42 features, 10 traces, 110 edges,
schemas, dependency boundary, and corpus semantics`, the legacy validator
reports `FULL DURABLE EVIDENCE`, the legacy updater passes, and the Windows
publisher `--check` reports `Local progress ledger is valid`.

This is a contract contradiction, not a code bug introduced today:
`docs/editor-parity/LOCAL-PROGRESS-LEDGER.md` states that the live task document
"must stay current, and editing it does not change these selected artifacts",
while the Windows publication gate requires that same document to still hash to
its frozen value. Any edit to the live specification after the freeze therefore
disables Windows-hosted publication permanently.

Options as assessed at the time. Option 1 was executed once the bytes were
recovered, so options 2–4 were never needed and **no frozen artifact was
rewritten**:

1. Restore the frozen specification bytes. **Executed 2026-09-08.** The locked
   path was restored byte-for-byte and re-verified in place; the superseded
   revision is archived at
   `G:\Temp\mgif-spec-revision-archive-20260908\gltf-scene-editor-parity-task.29c32851-revised.md`
   rather than discarded. The O06 substance the drift carried is not lost:
   `docs/editor-parity/manifest.json` still matches its frozen hash and still
   binds `O06-gizmo-escape-release-same-frame.conflictDisposition`
   (`observed_behavior_authoritative`) plus the sealed
   `reference-traces/UI-SEMANTIC-REPORT.md` and `ui-semantic-index.json`
   artifacts, and the revised phrase occurs in no other repository file.
2. Re-lock the specification hash across `EXPECTED_SPEC_SHA256`,
   `manifest.json`, `edge-obligations.json`, `g6-r1-authority-recovery.json`,
   and the frozen legacy state. This rewrites frozen artifacts and is forbidden
   without explicit owner authorization.
3. Amend `run_legacy_validator` / `derive_policy` so the live specification is
   exempt from the frozen comparison. This weakens a publication guard and
   requires owner authorization.
4. Publish from a POSIX host, where the publisher skips the legacy gate by
   design. Mechanically available today, but it deliberately bypasses a
   Windows-only frozen check, so it must be an explicit owner decision rather
   than a silent workaround.

## Blocker B — G8 Recovery Obligations

Status of the sealed G8 blocking findings against today's evidence
(`docs/editor-parity/u01-u08-live-replay-matrix.md`,
`docs/editor-parity/u07-exit-boundary-live-replay.md`):

1. **"U01-U08 are placeholder capture/marker/capture scripts"** — mostly
   closed. U02–U08 are real controller-bound traces and now pass in both Debug
   and Release on one scoped source identity
   (`34e32dc9b4b459cc63bf0de3969f41e763054da6bac4cf8de961d28de77ef6d1`);
   the headless controller replay passes 8/8 scenarios
   (`Editor UI controller replay passed 8 scenario checks`). **Closed
   2026-09-08**: `app/MinimalLatestApp.h` gained an empty-start bootstrap, so the
   U01 fixture stays bound for the scripted `open-valid` step but is not
   preloaded, and the document-less ready gate no longer requires `session()` or
   `projection()`. All eight cases now reach `PASS_BOUNDED_LIVE_PROOF` in both
   Debug and Release at two client extents, 1280x720 and 1920x1080, giving
   thirty-two accepted roots on one scoped source identity
   (`83687dfa404296649bbdf223b98947978779adb7e692a65486668e06acf69d52`), each
   with a non-blank boundary capture under the ratio-based pixel audit and each
   with a distinct `client.png` hash; see the authoritative section of
   `docs/editor-parity/u01-u08-live-replay-matrix.md`.
2. **"aggregate is a P7 artifact, not a P8 UI Gate matrix; no context-menu
   manifest"** — partially closed. Today's matrix adds `NoDocument`,
   `ReadyClean`, `ReadyModified`, and failure-recovery boundaries, and the
   resolution axis now covers two client extents (1280x720 and 1920x1080) in
   both configurations. Still open: the exact command/context/toolbar/lifecycle
   matrix, a context-menu manifest, and the DPI axis. Two axes are host-limited
   rather than harness-limited: 2560x1440 cannot be a client extent on a
   2560x1440 display because the default `WM_GETMINMAXINFO` max track size is the
   virtual screen and the 19 px non-client frame consumes the difference
   (measured 2560x1421 in both configurations), and DPI cannot be varied because
   the interactive session is per-monitor DPI aware and
   `SetProcessDpiAwarenessContext` fails from a child process, so `monitorDpi`
   stays 144 and `uiScaleFactor` stays 1.5. `tools/editor_ui_live_replay.ps1`
   now rejects an unreachable extent before launching instead of failing on a
   clamped capture.
3. **"Published P8-10/P8-11/P8-12 reports retain non-claims"** — open. Those are
   immutable ledger receipts; today's work is recorded as development documents,
   not as new ledger receipts.
4. **"MinimalLatestApp still contains the legacy GltfModel/SceneAsset load and
   destructive replacement path, editableSceneLights, index-based transaction
   state"** — likely closed, pending gate review. `GltfModel` and
   `editableSceneLights` have zero occurrences in `app/MinimalLatestApp.h`;
   scene data flows from the controller publication `m_projectedSceneAsset`
   (27 references, declared `std::shared_ptr<const demo::SceneAsset>`); the
   `Demo` target is `app/MinimalLatestApp.cpp` plus
   `app/editor/EditorTransactionPath.cpp` linking `demo_core`
   (`CMakeLists.txt:2306-2310`). The "destructive replacement path" and
   "index-based transaction state" clauses were not separately audited today.
5. **"identity omits acceptance driver/tests/CMake/validator; focused logs omit
   ui_state/ui_action"** — partial. The scoped identity (60 records) includes
   `app/editor/EditorTestDriver.cpp/.h`, `app/editor/EditorUiLiveReplayMain.cpp`,
   `tools/editor_ui_live_replay.ps1`, `CMakeLists.txt`, and
   `app/MinimalLatestApp.*`, but omits `tests/editor/editor_ui_*_tests.cpp` and
   `tools/generate_editor_dependency_identity.py`. `editor_ui_state_tests`
   (57 checks) and `editor_ui_action_tests` (130 checks) now run clean in both
   Debug and Release.
6. **"Release unfiltered runtime evidence contained VUID-vkCmdDraw-None-09600"**
   — partial, with stronger evidence than before. All sixty-four captured logs
   (32 roots x `stderr.log` + `stdout.log`) were scanned for `VUID-`,
   `SYNC-HAZARD`, `Validation Error`, `device lost`, `VK_ERROR`, and
   `stale editor command sequence`: zero occurrences, in **both** Debug and
   Release and at **both** client extents. The validation configuration is
   recorded per root in `manifest.validation` as
   `{"layers": ["VK_LAYER_KHRONOS_validation"], "synchronization": true,
   "messageFilter": null}` rather than in the logs, so the layer being active is
   a manifest fact, not a log grep. The authoritative roots are
   `*-live-{debug,release}-20260908-H1` at 1280x720 and
   `*-live-{debug,release}-20260908-H1080-1` at 1920x1080, plus
   `mgif-U01-empty-new-open-live-debug-20260908-H2` whose first whole-run
   attempt landed in a blank capture epoch. This remains bounded to the live
   acceptance target and is not a clean-runtime claim for the whole runtime
   surface.

### Repaired today: `scene_editor.minimal_app_authority_contract` false positive

This P8-UI contract test failed on both hosts before this session's change. Its
guard regex `m_projectedSceneAsset\s*->\s*[A-Za-z_][A-Za-z0-9_]*\s*=` also
matched the four read-only authority comparisons at
`app/MinimalLatestApp.h:3423-3429` (`->documentRevision ==`,
`->documentSemanticHash ==`). Because the member is
`std::shared_ptr<const demo::SceneAsset>`, a write through it cannot compile, so
those matches are false positives. The regex now carries a negative lookahead
(`=(?!=)`), which still rejects assignment syntax. The test passes in WSL
(6 tests `OK`) and in Windows CTest (`100% tests passed`).

## What a PASS-Capable `G8-R1` Needs

1. ~~An owner decision on Blocker A (options 1–4 above).~~ **Closed** by option
   1, without rewriting any frozen artifact and without weakening
   `run_legacy_validator`.
2. ~~A U01 live empty-start bootstrap, or an owner-approved scope change that
   records U01 as headless-only.~~ **Closed** by implementing the bootstrap; U01
   passes live in both configurations with genuine boundary captures.
3. The resolution/DPI matrix plus a context-menu manifest, including DPI control
   in the live harness.
4. Affirmative P8 leaf receipts published through the ledger rather than
   development documents.
5. Gate-reviewer confirmation of the legacy-path isolation claim (finding 4).
6. Release unfiltered runtime evidence beyond the live acceptance target, or an
   explicitly bounded scope accepted by the owner.
7. Identity scope extended to the acceptance tests and the identity generator.

## Non-Claims

- No `ENGINEERING PASS` is asserted for `G8` or `G8-R1`; a failed re-gate cannot
  unblock descendants, so publishing one today would add history without
  unblocking `P9`/`P10`.
- Today's live and Release matrices are development records, not sealed ledger
  receipts.
- Screenshots remain bounded boundary evidence; no image assertion is made.
- The Release validation-diagnostic result is bounded to the live replay
  acceptance target at one resolution.
- Roots tagged `*-empty3`, `*-u01fix`, `*-u01recheck`, `*-fix2`, `*-retry`,
  `*-final`, `*-final2`, and `*-R1`/`*-R2`/`*-R3` are invalidated as boundary
  evidence: their `client.png` files are blank white frames that the
  pre-hardening audit accepted, and two of them are byte-identical across
  different cases and configurations.
  `docs/editor-parity/capture-blank-environment-fault.md` records the
  measurements, the audit repair (`edgeTransitions >= 2000`), the bounded capture
  retry, and the host-side NVIDIA container crash loop correlated with the blank
  epoch.
- Items 3, 4, 5, 6, and 7 above remain open; the DPI axis is still not
  harness-reachable.
