# Locked Specification Input Restored, 2026-09-08

Status: **Blocker A of `docs/editor-parity/g8-recovery-gap-analysis.md` is
closed.** The frozen policy input `docs/gltf-scene-editor-parity-task.md` was
found to have drifted from its pinned bytes, the pinned bytes were recovered
byte-for-byte from an off-repository fixture copy, and every frozen validator
now passes on Windows. One second defect that the unblocked validator exposed
(unbound corpus fixture) was fixed in the same pass. No ledger state was
changed by this work.

## Symptom

Four failures shared one root cause:

| surface | message before repair |
| --- | --- |
| `tools/validate_editor_progress_ledger.py --check` | `state policy or policy inputs are stale` |
| `tools/update_editor_progress_ledger.py --check` | same stale legacy validation |
| `tools/validate_editor_p0_artifacts.py --repo-root .` | `live task document differs from the manifest specification hash` |
| CTest label `scene-editor-p0-scaffold` | 7 of 59 tests failed |

Because `tools/local_editor_progress_ledger.py:1745` runs the frozen legacy
validator before **any** publisher mode on Windows (`:1590-1598` skips it on
POSIX by design), the drift also fail-closed the Windows publication path. That
is exactly Blocker A.

## Diagnosis

`derive_policy` (`tools/validate_editor_progress_ledger.py:569-586`) pins three
policy inputs **by bytes** — specification, manifest, license provenance — and
`:1364` / `:1438` insist the frozen state's recorded `inputs` equal the derived
ones. The policy *text* assertions in the same function still passed, so the
failure was byte drift of the locked specification only:

```text
locked : 165772 bytes  sha256 bfd4b5ffe8d73b9bb887047a8cad5ad96c7184d182eeb752aa47028b05bd679e
actual : 167073 bytes  sha256 29c32851ef6e072f094627d71d748b2d6ebad3059a5eb714b8a5965c41303721  mtime 2026-09-06 22:20:45
```

The frozen tooling offers no rebind: `update_editor_progress_ledger.py --help`
exposes only `--snapshot`, `--gate-receipt`, `--evidence-root`, `--track-task`,
`--orca-command`, `--capture-live`, `--publish`, `--candidate-dir`, `--check`,
`--live-check`, `--shallow-integrity`, `--print-receipt-fingerprint`.

Who pins which hash decides who drifted:

- `bfd4b5ff…` (locked) is pinned by eight artifacts:
  `tools/validate_editor_p0_artifacts.py:23`,
  `docs/editor-parity/manifest.json`, `docs/editor-parity/edge-obligations.json`,
  `docs/editor-parity/g6-r1-authority-recovery.json`,
  `docs/editor-parity/progress-ledger-state.json`, six frozen bundles under
  `docs/editor-parity/progress-ledger/bundles/sha256/`, and
  `docs/gltf-scene-editor-parity-progress.md`.
- `29c32851…` (the 2026-09-06 revision) was pinned by **nothing** — the only
  occurrence in the repository was the observation recorded in
  `g8-recovery-gap-analysis.md`.

So the revision was unbound drift against a locked input, not a sanctioned
policy change.

## What the drift contained, and why reverting it loses no authority

The revision rewrote one semantic decision — the O06 same-frame Escape/release
disposition — and propagated it to nine places in the prose (spec lines 362,
471-473, 658-659, 870-871, 912-913, 1017-1018, 1046-1047, 1071-1072,
1167-1168). Frozen text predicted that a same-frame release runs `endDrag` and
commits exactly one transform history entry; the revision replaced that with
"either same-frame order clears selection, retains preview TRS, detaches, and
produces no history", citing
`manifest.json → O06-gizmo-escape-release-same-frame.conflictDisposition`
(`observed_behavior_authoritative`) and the sealed report
`O06: Same-Frame Escape/Release Ordering`.

That authority already lives in hash-verified frozen artifacts and survives the
revert intact:

- `docs/editor-parity/manifest.json` still matches its frozen hash and still
  carries `"id": "O06-gizmo-escape-release-same-frame"`, `"status": "conflict"`,
  the `conflictDisposition` block, and the sealed artifacts
  `reference-traces/UI-SEMANTIC-REPORT.md`,
  `reference-traces/ui-semantic-index.json`,
  `reference-traces/INPUT-CLIPBOARD-REPORT.md`,
  `reference-traces/input-clipboard-index.json`.
- The distinctive revised phrase `两个顺序` occurs in no other repository file,
  so no document, test or tool depended on the revised prose.
- The locked clause itself required the runtime trace to win
  (`若 runtime 与源码预测冲突，以 runtime trace 修订本条后才可实现`); the
  substance was applied through the manifest and the sealed trace, which is the
  channel the frozen design can verify. Mutating the locked copy is the one
  channel it cannot.

## Recovery

The locked bytes were not in git (the file is untracked), not in any evidence
CAS directory (no blob named `bfd4b5ff…`), and not anywhere on `G:` (a full
`-size 165772c` scan returned nothing). A bounded size scan of
`%LOCALAPPDATA%\Temp` found three ledger-v2 fixture repositories holding the
exact pinned bytes, all stamped `2026-08-23 14:28:50`:

```text
C:\Users\happyelements\AppData\Local\Temp\mgif-ledger-v2-tests-t41g0r9w\repo\docs\gltf-scene-editor-parity-task.md
C:\Users\happyelements\AppData\Local\Temp\mgif-ledger-v2-tests-0eciy7oe\repo\docs\gltf-scene-editor-parity-task.md
C:\Users\happyelements\AppData\Local\Temp\mgif-ledger-v2-tests-ubh2id9y\repo\docs\gltf-scene-editor-parity-task.md
  165772 bytes  sha256 bfd4b5ffe8d73b9bb887047a8cad5ad96c7184d182eeb752aa47028b05bd679e   (all three)
```

A byte-for-byte copy from the first fixture restored the locked path, and the
sha256 was re-verified in place after the copy. Two older revisions were also
located and are **not** the locked one, which bounds the search:
`155134 / 0fe1e3c2…` (`G:\ReferenceOracleRuns\vkgr-50373c7b-input-probe`) and
`163849 / ef66c3a6…` (`mgif-validator-readiness-proof-d83ea0e06eff44d3836312c4bb3e56dc`,
2026-08-22).

The method follows the existing precedent in
`docs/editor-parity/o06-authority-recovery.json`
(`recoveryMethod: "byte-for-byte copy while preserving recycle-bin source
evidence"`), including preservation of the superseded bytes:

```text
G:\Temp\mgif-spec-revision-archive-20260908\gltf-scene-editor-parity-task.29c32851-revised.md
  167073 bytes  sha256 29c32851ef6e072f094627d71d748b2d6ebad3059a5eb714b8a5965c41303721
G:\Temp\mgif-spec-revision-archive-20260908\README.txt   (identities, recovery source, sibling copies)
```

## Second defect exposed by the unblocked validator

With the specification hash satisfied, the P0 validator reached a deeper check
and failed on the fixture corpus:

```text
P0 artifact validation failed: corpus must bind every committed fixture and no absent fixture
  (tools/validate_editor_p0_artifacts.py:8531)
```

Cause: `tests/fixtures/gltf_editor/live_camera_light.gltf` (1587 bytes, created
2026-09-08 11:13 for the U05 live case, referenced by
`app/MinimalLatestApp.h:2313` and `tools/editor_ui_live_replay.ps1:159`) was
never bound in `corpus.json`, while the validator requires
`set(corpus paths) == every file in the corpus directory`.

Fix: bound the fixture in sorted position with its real identity —
`sha256 c6716928dda75614f2a2b376a3eb72a4f438e7274049577b36eb42aca2487c26`,
`bytes 1587`, `expected "valid"`, `covers ["KHR_lights_punctual", "camera",
"single-node scene", "U05 camera light visibility live replay"]`. This is safe
and consistent with the corpus contract:

- `corpus.json` is not hash-pinned by any artifact in the repository;
- its `policy` (`hand-authored for MGIF`, `thirdPartyBytes: false`) matches the
  fixture's own `asset.generator = "MGIF live camera light fixture"`;
- `validate_fixture_semantics` (`:8412`) is generic over `valid` / `invalid` /
  `binary`, and the mandatory set at `:8534-8547` is a subset check, so no
  frozen tool needed editing;
- the directory already binds task-specific extras
  (`r09_shared_mesh.gltf`, `scene_root_matrix.gltf`,
  `material_extensions.gltf`), so binding an obligation-specific fixture is the
  established pattern.

## Verification

Windows interpreter `C:\Users\happyelements\scoop\apps\python\current\python.exe`
(the WSL interpreter cannot run the P0 validator; it dies with
`OSError: [Errno 36] File name too long`):

```text
[1] P0 artifacts        P0 scaffold validation passed: 42 features, 10 traces,
                        110 edges, schemas, dependency boundary, and corpus
                        semantics                                    rc=0
[2] legacy validator    Editor progress ledger validated (FULL DURABLE EVIDENCE)
                        state=31ca0ba7db488f46eb3f8483960e9410ea03db9dfaead4cf
                        6cc9d8e953d17bca                             rc=0
[3] legacy updater      Progress ledger validation passed (full durable
                        evidence mode)                               rc=0
[4] local publisher     Local progress ledger is valid:
                        bundle=404366e527af67ea2d0bb57ba751be2aa0d2c7812d233ae8
                        69c1833a5d5120b7 head=1d54cec00c96d704229448564c3b762e8
                        69159eb333e8bb2f4e68051b294f00b               rc=0
```

CTest label `scene-editor-` in the coherent Debug tree: see
`G:\Temp\ctest-debug-restored.log` /
`G:\Temp\mgif-ctest-debug-restored-20260908.xml`; the seven
`scene-editor-p0-scaffold` failures listed above are the ones this repair
targets. The prior coherent-binary run was 88% (7 failed of 59) and the
pre-repair run was 53% (28 failed of 59).

## Consequences and adopted rule

- Windows publication of local ledger transitions is possible again, so
  `G8-R1` and the `P9` / `P10` evidence chain are no longer blocked by policy
  staleness. Blocker B (the G8 recovery obligations themselves: U01 empty-start
  bootstrap, DPI/resolution matrix, legacy viewer isolation, full visual
  coverage) is unchanged and still governs whether a truthful `ENGINEERING PASS`
  can be claimed.
- Rule: `docs/gltf-scene-editor-parity-task.md`,
  `docs/editor-parity/manifest.json` and
  `docs/editor-parity/LICENSE-SOURCES.md` are locked **byte** inputs. Substance
  changes belong in manifest-bound artifacts and local ledger documents. If a
  locked input must change, it requires an explicit owner-authorized rebind
  recorded like the existing authority-recovery documents, because no frozen
  tool can absorb the drift silently.
