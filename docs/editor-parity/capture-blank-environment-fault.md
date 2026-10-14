# Blank-Capture Environment Fault And Pixel-Audit Hardening

Date: 2026-09-08 (measurements 14:21-21:03 local)
Scope: `tools/editor_ui_live_replay.ps1` boundary-capture transport and audit;
U01-U08 bounded live replay evidence in both configurations.
Ledger effect: none. No reference manifest, progress ledger, gate, or release
policy was changed by this record.

## 1. Symptom

Live replay runs began failing with either

- `Client PNG is blank or low-information: nonBlack=57600 colorful=0 unique=13`,
  or
- `PASS_BOUNDED_LIVE_PROOF` on a `client.png` that was **not** the editor.

The second form is the dangerous one: the harness recorded an accepted boundary
screenshot that contained no application content.

## 2. Evidence that the frames were blank, not rendered content

Decoded block-mean luminance (6x8 grid) of a captured frame from the affected
window, against a known-good frame from 14:21:55:

| Frame | Block mean luminance | Sampled mean | Sampled sd | Edge transitions | Colorful |
| --- | --- | --- | --- | --- | --- |
| Known-good `U02` 14:21:55 | 26-53 | 34.9 | 26.2 | 4276 | 11409 |
| Known-good `U08` 14:21 | 26-53 | 34.5 | 25.7 | 4119 | 9888 |
| Known-good `U07` 14:21 | 26-53 | 37.0 | 23.6 | 3264 | 25773 |
| Affected Debug `U01` 20:44 | 230-255 | 247.5 | 13.9 | 483 | 591 |
| Affected Debug `U02` 20:44 | 230-255 | 246.8 | 14.1 | 515 | 591 |
| Affected Debug `U06` 20:4x | 230-255 | 247.8 | 14.6 | 734 | 558 |
| Affected Release `U08` 20:4x | 230-255 | 247.8 | 14.6 | 734 | 558 |
| Affected Debug `U01` 20:14 | 230-255 | 248.9 | 18.2 | 937 | 10320 |

The affected frames are a near-uniform bright surface with faint dither, not a
dark editor UI.

Decisive: two affected frames from **different cases and different build
configurations** are byte-identical.

- `G:\Temp\mgif-U01-empty-new-open-live-debug-20260908-R1\client.png`
- `G:\Temp\mgif-U03-transform-gizmo-live-release-20260908-R3\client.png`
- both 27705 bytes, SHA-256
  `830014e476355649c452c1cbe530d9d63f7f9cdacf7f86cd186e6750a72bae33`

Two independent processes cannot render pixel-identical different scenes, so
the capture was returning a static surface rather than window content.

## 3. Evidence that the application binary was not at fault

- The same Debug executable alternated between blank and genuine captures
  within minutes, with no rebuild in between.
- Whenever a genuine capture landed, content matched earlier known-good
  measurements: Debug `U02` returned to `uniqueColors=131`, the value recorded
  before the U01 empty-start rebuild.
- After the environment recovered, the same binaries produced 16/16
  `PASS_BOUNDED_LIVE_PROOF` runs with 5640-6814 edge transitions
  (`docs/editor-parity/u01-u08-live-replay-matrix.md`, authoritative section).

## 4. Capture transport was already the strongest documented path

`Capture-Client` tries, in order (`tools/editor_ui_live_replay.ps1`):

1. `PrintWindow(hwnd, hdc, 0x00000003)` = `PW_CLIENTONLY | PW_RENDERFULLCONTENT`
   (`:304-305`), transport `print-window`;
2. `GetDC(hwnd)` + `BitBlt(..., 0x00CC0020)` (`:318-324`), transport
   `window-dc-bitblt`;
3. `Graphics.CopyFromScreen` at the window origin (`:339-343`), transport
   `desktop-copy`.

Each transport's result is audited before acceptance. In the affected window all
three returned the same bright surface, and transport 2 was accepted because the
audit was too weak (section 6). No transport change was made: switching the
boundary evidence to application self-capture would change what the screenshot
proves and is an owner policy decision, not a repair.

## 5. Host environment evidence

Service Control Manager events for today, queried directly:

- 2000 SCM events, **all** NVIDIA-related.
- `Id=7023` (service terminated unexpectedly) x1000 and `Id=7031` (service
  restarted automatically) x1000, first 16:26:23, last 21:06:35 - roughly one
  crash/restart cycle every 17 seconds for 4h40m.
- Sample message: `NVIDIA LocalSystem Container 服务因下列错误而停止: 执行的通用
  请求失败` (ERROR_GEN_FAILURE).
- `Get-Service NvContainerLocalSystem` now reports `Stopped`, start type
  `Automatic`.

Correlation timeline:

| Local time | Observation |
| --- | --- |
| 14:21:55 | genuine captures (`*-norm` matrix) |
| 16:26:23 | NVIDIA container crash loop begins |
| 20:14-20:48 | blank/white captures accepted or rejected |
| 21:00:51 | genuine captures resume (edge transitions 6716) |
| 21:06:35 | last crash-loop event; service remains `Stopped` |

The blank epoch falls inside the crash loop and genuine capture resumed near its
end. This is correlation, not a proven mechanism: DWM composition was not
instrumented, and blank frames were also observed in windows where service state
was not sampled. The honest conclusion is that window-content capture on this
host is intermittently unavailable and that the harness must fail closed when it
is.

One unrelated transient was observed in the same period and must not be
conflated with blank capture: Release `U07-modified-failures` attempt 1 at
21:02:04 failed with `Timed out waiting for live replay ready marker`
(`tools/editor_ui_live_replay.ps1:451`); attempt 2 passed on the first capture
try.

## 6. Harness defect A: the audit could not reject a bright blank

The original gate was

```powershell
if ($nonBlack -lt 500 -or $colorful -lt 20 -or $colors.Count -lt 16) { throw ... }
```

`nonBlack` counts pixels with `max(R,G,B) > 20`, so a **white** frame scores
57600/57600 by construction. Dither noise on the bright surface then supplied
`colorful=558..591` and `unique=114..131`, clearing both remaining thresholds.
The gate only ever defended against black or low-colour blanks.

Repair: `Get-PixelAudit` now also counts `edgeTransitions` - horizontally
adjacent sampled pixels whose luminance differs by more than 24 - and rejects
below 2000. It records `luminanceMean` and `luminanceStdDev` without gating on
them, so a future light theme is not rejected by theme-specific numbers.

Measured separation, threshold 2000:

- genuine editor frames: 3264-6814 (1.63x margin at the weakest)
- blank white frames: 483-937 (2.14x margin at the strongest)

Gate verification used the harness's own function source, extracted from
`tools/editor_ui_live_replay.ps1` and invoked against saved frames:

| Frame | Result |
| --- | --- |
| Known-good Debug `U02` 14:21 | ACCEPT, edges 4276 |
| Known-good Debug `U08` 14:21 | ACCEPT, edges 4119 |
| Known-good Debug `U07` 14:21 | ACCEPT, edges 3264 |
| Blank Debug `U01` 20:44 | REJECT, edges 483, mean 247.6 |
| Blank Debug `U02` 20:44 | REJECT, edges 515, mean 246.9 |
| Blank Release `U08` 20:4x | REJECT, edges 734, mean 247.8 |

Live confirmation after the repair: `G:\Temp\mgif-u01-live-debug-20260908-gatecheck2`
passed with `edges=6716`, `colorful=36659`, `luminanceMean=44.3`,
`luminanceStdDev=33.4`, 5 steps, terminal `ReadyClean`.

## 7. Harness defect B: one blank capture aborted the whole run

Repair: the boundary capture now retries up to 12 times, 1500 ms apart, bounded
by the run deadline and by a process-exit check. The audit is never relaxed
between attempts, and the manifest records `screenshot.captureAttempts`.

Retry-budget tuning was measured, not guessed:

- A run either captured on attempt 1 or stayed blank for the entire budget.
- Budgets of 24 attempts (~40 s) and 90 attempts (~2.5 min) both failed inside
  a blank epoch, while every genuine capture succeeded on attempt 1.
- Blank epochs therefore last minutes, and in-run retrying cannot cross them.
  The budget was set back to 12 attempts and the caller reruns the whole case.

Caller policy used for the authoritative matrices (`G:\Temp\run_matrix_robust.ps1`,
a temporary driver outside the repository): per case, up to 4 whole-run attempts
with unique evidence roots, 40 s pause after a failure, first
`PASS_BOUNDED_LIVE_PROOF` accepted. Observed worst case in the authoritative
Debug and Release matrices: Release `U07` needed attempt 2.

## 8. Invalidated evidence

The following roots were collected while the audit was weak and their
`client.png` files are blank white frames. They are **not** citable as boundary
evidence, even where the run reported `PASS_BOUNDED_LIVE_PROOF`:

- tags `*-empty3`, `*-u01fix`, `*-u01recheck`, `*-fix2`
- tags `*-retry`, `*-final`, `*-final2`
- tags `*-R1`, `*-R2`, `*-R3` (Debug and Release), collected 20:41-20:50

Their app-side artifacts (steps, state, renderer digests) remain usable as
diagnostic history for controller behaviour, but no screenshot claim from these
roots may be recorded in the ledger or cited as visual or boundary proof.

The authoritative set is the `*-G1`/`*-G2` matrices in
`docs/editor-parity/u01-u08-live-replay-matrix.md`, all sixteen roots carrying
non-blank captures under the hardened audit, with no duplicated `client.png`
SHA-256 across roots.

## 9. What remains open

- The host fault itself is not repaired. `NvContainerLocalSystem` is `Automatic`
  but currently `Stopped` after 1000 crash cycles; stopping or repairing it
  needs elevation this session does not have, so it is an owner action.
- Window-content capture on this host must be treated as intermittently
  unavailable. Every future matrix run needs the whole-case rerun policy above,
  and any root whose audit shows `edgeTransitions < 2000` must be discarded.
- The transport remains external window capture by policy. Moving boundary
  evidence to application self-capture would change the meaning of the
  screenshot claim and requires an explicit owner decision.

## Second Gate Hole: Absolute Thresholds Do Not Scale With Extent

The 2,000-edge threshold was tuned on 1280x720 frames and it was wrong in the
other direction too. Extending the matrix to 1920x1080 exposed a near-black
frame that the audit accepted:

```text
G:\Temp\mgif-u01-1080p-probe2/manifest.json
screenshot.transport = print-window
screenshot.audit = sampled 129600, nonBlack 2461, colorful 539,
                   uniqueColors 40, edgeTransitions 2523,
                   luminanceMean 2.7, luminanceStdDev 15.9
```

Decoding the frame into a 4x6 luminance grid gives values of 0-21 against 30-59
for a genuine 1080p frame, so this is an essentially black client, not editor
UI. It cleared every absolute threshold because a 1920x1080 capture samples
129,600 pixels instead of 57,600: 2,461 non-black and 2,523 dither edges are
enough to satisfy `nonBlack >= 500`, `colorful >= 20`, `unique >= 16`, and
`edges >= 2000`, while representing only 1.9% of the frame. The run was written
as `PASS_BOUNDED_LIVE_PROOF`.

So the absolute gate had a hole at each end: uniform white passed at 720p, and
near-black passed at 1080p.

## Ratio Gate And Its Verification

`Get-PixelAudit` now gates on ratios of the sampled pixel count, which is the
only formulation that holds across extents:

| Gate | Threshold |
|---|---:|
| `nonBlackRatio` | `>= 0.05` |
| `colorfulRatio` | `>= 0.02` |
| `edgeRatio` | `>= 0.03` |
| `uniqueColors` | `>= 16` |

Measured separation, taken from every blank and genuine frame decoded this
session:

| Population | `nonBlackRatio` | `colorfulRatio` | `edgeRatio` | Mean luminance |
|---|---|---|---|---|
| Genuine editor frames, 720p and 1080p | 0.9726-1.0 | 0.0393-0.6559 | 0.0567-0.1183 | 34.5-52.1 |
| Uniform white blanks, 720p | 0.9994-1.0 | 0.0097-0.0127 | 0.0084-0.0127 | 246.9-248.9 |
| Near-black blank, 1080p | 0.019 | 0.0042 | 0.0195 | 2.7 |

The thresholds keep at least 1.5x margin on both sides of every measurement.
Mean and standard deviation are recorded in the manifest but deliberately not
gated, so a future light theme is not rejected by theme-specific numbers.

Verification, using the audit function extracted from the harness itself and run
against ten stored frames (`G:\Temp\verify_audit_gate2.ps1`): six genuine frames
at both extents accepted, three white blanks rejected, one near-black blank
rejected, `mismatches=0`.

Recomputing the ratios for roots accepted before this gate confirms none of them
was contaminated: the fifteen `-G1`/`-G2` 720p roots span `edgeRatio`
0.0979-0.1183 and `colorfulRatio` 0.4939-0.6559, and the fifteen `-1080p-1`
roots span `edgeRatio` 0.0602-0.0706 and `colorfulRatio` 0.0393-0.0554. All
clear the new gate.

## Additional Invalidated Root

- `G:\Temp\mgif-u01-1080p-probe2` reported `PASS_BOUNDED_LIVE_PROOF` on a
  near-black 1920x1080 frame (`luminanceMean=2.7`, `nonBlackRatio=0.019`). It is
  invalid boundary evidence and is rejected by the ratio gate.

The `-G1`, `-G2`, and `-1080p-1` roots remain genuine, but the authoritative
matrices are the `-H` and `-H1080-` roots in
`docs/editor-parity/u01-u08-live-replay-matrix.md`, because those thirty-two
roots were all collected with the final harness and therefore share one scoped
source identity, `83687dfa404296649bbdf223b98947978779adb7e692a65486668e06acf69d52`.

## Resolution Axis: 2560x1440 Is Unreachable On This Host

Extending the matrix further failed for a reason unrelated to the application:

```text
requested 2560x1440, produced 2560x1421, in Debug and in Release
host virtual screen 2560x1440, work area 2560x1392
window outer 2560x1440 at left=0 top=-19, client 2560x1421 at 0,0
window outer 2560x1440 at left=-16 top=0, client 2560x1421 at -16,19
```

The default `WM_GETMINMAXINFO` max track size is the virtual screen, so a
decorated window's outer rectangle can never exceed 2560x1440, and the 19 px
non-client frame consumes the difference. Moving the window does not change it.
A 2560x1440 *client* therefore requires either a borderless client policy or a
display larger than the host screen; both are owner decisions, and the harness
extent validation stays strict rather than tolerating the clamped size.

`tools/editor_ui_live_replay.ps1` now rejects such a request before launching
the application, using `SM_CXVIRTUALSCREEN` / `SM_CYVIRTUALSCREEN`:

```text
Requested client 2560x1440 cannot fit inside a decorated window on this host:
the virtual screen is 2560x1440 and the non-client frame consumes part of it
(measured reachable maximum 2560x1421).
```

`G:\Temp\run-1440p-debug.log`, `G:\Temp\run-1440p-release.log`, and
`G:\Temp\run-1440p-anchored.log` record the measured runs;
`G:\Temp\mgif-1440p-preflight-probe` records the fast preflight rejection,
which writes only `source-identity.json` and no application artefacts.

## Final Authoritative Matrices

Thirty-two accepted roots: eight cases, Debug and Release, at 1280x720 and
1920x1080, all `PASS_BOUNDED_LIVE_PROOF` under the ratio gate, all sharing one
scoped source identity, with thirty-two distinct `client.png` hashes and no
reused frame. Full tables, audit ratios, and root naming are in
`docs/editor-parity/u01-u08-live-replay-matrix.md`.

DPI variation remains unreachable in this session, for the reason recorded in
`docs/editor-parity/g8-recovery-gap-analysis.md`: the interactive session is
per-monitor DPI aware, `SetProcessDpiAwarenessContext` fails from a child
process, and `monitorDpi` therefore stays 144 with `uiScaleFactor` 1.5. U08 is
still exercised at that scale, so the resolution axis is now two extents deep
while the DPI axis stays open.
