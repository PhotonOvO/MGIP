# U03 Focus-Loss OS Replay Development Record

Date: 2026-09-08. This is a development record, not a sealed receipt or a
progress-ledger transition. Full U03 is not accepted; G8 remains failed and
P9/P10 remain blocked.

## Implemented Scope

O07 is split into three independent, bounded native-input cases:

- `U03-gizmo-focus-no-release`: real focus loss while the physical left button
  remains down, followed by focus restore and a late physical release.
- `U03-gizmo-focus-same-frame`: a physical left-up immediately followed by real
  focus loss in one ordered native batch.
- `U03-gizmo-focus-delivered-release`: a normal delivered release commits first,
  then a later focus loss must not change the committed history.

Each case has 4 steps and 14 phases. The plan publishes a ready boundary before
each input. A focus-loss frame records only the `focus-lost` terminal batch;
after focus is restored, a late physical up is recorded as a suppression-only
batch. ImGui's invalid mouse sentinel is deliberately not converted into a
pointer or Gizmo ray on an unfocused frame.

The native queue is bounded and FIFO. It records callback sequence numbers for
Escape, primary-button release, and focus loss. Focus loss latches the active
Gizmo and retains the preview/publication baseline without adding transform
history. A later release cannot commit a latched transaction. A same-frame
focus boundary takes precedence over a delivered-up edge, and the latch clears
the rendered active component while retaining logical ownership. Callback clock
and terminal provenance are validated in both the C++ runtime and PowerShell
harness.

## Initial Contract And Regression Verification

Windows/MSVC Ninja builds completed for both configurations:

- Debug: `G:\Temp\mgif-p6-90-msvc-20260905T231500Z-Debug`
- Release: `G:\Temp\mgif-p6-90-msvc-20260905T232000Z-Release`

Pre-audit replay executable SHA-256:

- Debug: `d7c1bafe9115c1244e305e779bb207ace6fb979a5241d66c96979734de6771db`
- Release: `2c035ddaf6450dc5de7d70260516d50e6de28e7a1c83b85aa84cbb16a6e1224f`

The focused Windows/MSVC regression set passed in both configurations: the
O07 CTest selection was 8/8 (OS replay plan/runtime/CLI, Gizmo CPU/integration,
input routing, Controller transactions, and selection/picking integration),
and the Windows Python contract suite was 18/18. The additional continuous-edit
and animation/controller tests in the configured trees also passed.

The source-bound plan identities are:

- no-release: `a6f2b64c963618c6faeb9582f3d98ecdd422688e568c09825ce16a5c7daa03d1`
- same-frame: `6e824061fa61e8b67e00f7f9fd5b83dcd213ea3248812fc5c6a08b0fdd8cb014`
- delivered-release: `50e52526ac560d9f017a4c96ffe38e92861aa781ba421b86697872a5bebeb8f4`

The pre-audit scoped source identity emitted by the replay harness was
`3f98946887a4844702f34b1688f8aa688f288f794ab84883adabea26e1023371` (84
records). This is a scoped identity, not an exhaustive repository closure.

## Real OS Attempts And Blocker

All six required Debug/Release runs reached the first native `ready` boundary,
with the launched HWND/PID and 1280x720, 96-DPI client contract validated. Each
run then failed the mandatory first PNG audit with the identical result:

`sampled=57600 nonBlack=57600 colorful=0 colors=2`

The independent evidence roots are:

- `G:\Temp\mgif-u03-o07-no-release-debug-20260907-r4`
- `G:\Temp\mgif-u03-o07-same-frame-debug-20260907-r1`
- `G:\Temp\mgif-u03-o07-delivered-debug-20260907-r1`
- `G:\Temp\mgif-u03-o07-no-release-release-20260907-r1`
- `G:\Temp\mgif-u03-o07-same-frame-release-20260907-r1`
- `G:\Temp\mgif-u03-o07-delivered-release-20260907-r1`

These roots contain failed diagnostic PNGs only and are not acceptance
evidence. No planned input phase was signed and no PASS manifest was produced.
The strict blank/low-information oracle remains enabled.

The capture failure was reproduced outside the formal harness on a live replay
window after reactivating it on the primary monitor. `PrintWindow` flags 0/1/2/3,
window/client DC `BitBlt`, desktop DC, `CopyFromScreen`, and Windows Graphics
Capture all returned the same white 1280x720 client surface, including when the
window was foreground, visible, non-iconic, non-hung, and had
`GetWindowDisplayAffinity=0` and DWM cloaked state 0. The Vulkan diagnostic
path separately observed successful image acquisition, clear/blit, ImGui draw
calls, and queue present. This identifies the current Windows/NVIDIA
independent-flip/DWM capture environment as the blocker; it does not establish
an O07 state-machine failure.

## Initial Vulkan-Layer Rerun (Pre-audit)

To separate the DWM transport failure from the editor proof, the harness now has
an explicit `-UseVulkanScreenshotLayer` mode. It validates the SDK layer
manifest/DLL (`VK_LAYER_LUNARG_screenshot`, manifest SHA-256
`955ff58ad4dcec524ea0c650622fd2878f0066f97fbc78416b672bd48ba60a69`, DLL
SHA-256 `be0f76f10f4d1484dac440e095ad652da1ba0ee66a09cf239204cbdcad371402`),
sets the child `ProcessStartInfo` environment to validation plus screenshot
layer with implicit layers disabled, and records that environment per process.
The layer output is pinned to the unique `P6` frame
`index = ready.frame.frameSerial - 1`; the source payload is empirically
BGR/top-left and is converted to RGB/opaque PNG through an atomic temporary
file. The existing global and observed-viewport blank oracle runs unchanged on
the converted PNG. Ready bytes, source bytes, converted bytes, process PID and
run nonce are all hash-bound.

The initial Debug/Release matrix completed with 14/14 ready/continue receipts and
8/8 persisted PNGs for every case. All selected layer frames were unique,
`postFinalCount=0`, and every PNG had 1280x720 dimensions and passed the global
and viewport pixel thresholds.

| Configuration / case | Evidence root | Selected layer indices | Terminal result |
| --- | --- | --- | --- |
| Debug / no-release | `G:\Temp\mgif-o07-final-debug-no-release-20260907-r2` | `2,4,6,11,13,27,29,31` | latch 1, focus-loss 1, suppressed 2, commit 0 |
| Debug / same-frame | `G:\Temp\mgif-o07-final-debug-same-frame-20260907` | `2,4,6,11,13,26,28,30` | suppressed-release then focus-lost, latch 1, commit 0 |
| Debug / delivered-release | `G:\Temp\mgif-o07-final-debug-delivered-release-20260907` | `2,4,6,11,13,26,28,30` | delivered-release/commit 1, focus-loss 0 |
| Release / no-release | `G:\Temp\mgif-o07-final-release-no-release-20260907` | `2,4,6,11,13,27,29,31` | latch 1, focus-loss 1, suppressed 2, commit 0 |
| Release / same-frame | `G:\Temp\mgif-o07-final-release-same-frame-20260907` | `2,4,6,11,13,26,28,30` | suppressed-release then focus-lost, latch 1, commit 0 |
| Release / delivered-release | `G:\Temp\mgif-o07-final-release-delivered-release-20260907` | `2,4,6,11,13,26,28,30` | delivered-release/commit 1, focus-loss 0 |

These are bounded layer-backed O07 observations, not a claim that the ordinary
GDI/desktop capture transport is fixed. The six earlier all-white roots remain
diagnostic only. No reference manifest, progress ledger, G8 state, P9/P10
state, or release policy was changed.

## Current Source Rerun After FIFO Audit

The first layer-backed matrix above was produced before the native callback
FIFO audit. The current source keeps a trailing primary-button callback after a
focus marker, defers that callback through the restore frame, and replays it as
a suppression-only event on the next independent focused frame. Stale native
batches are drained fail-closed, document-lifetime reset discards pending
callbacks without rewinding the callback clock, and ordinary focus regain only
suppresses polling when a focus fence is present. The runtime and PowerShell
contracts explicitly allow the one deferred callback ordinal that was already
observed at the focus-loss ready boundary; all other callback-backed terminals
still require a strictly advancing clock.

Current Windows/MSVC replay executable SHA-256 values used by the final fresh
matrix:

- Debug: `c52b02e2929cb9638569001eeafe0a4eaa20aafc0d604c8fae65cfc6b0b71b36`
- Release: `a149551c7f02b0fb62d01089cc1d249c3c08f87f6d44ec6dee0898b5fac666f8`

The current scoped source identity is
`3cfd66d92f1ad7874cf45b04cc49e290397aedec7db769a01580831688670602`.
The three plan identities are unchanged from the matrix above. Both
configurations passed the focused MSVC CTest set (8/8), and the Windows
Python contract suite passed 18/18.

The fresh layer-backed matrix completed with 14/14 ready/continue receipts and
8/8 persisted PNGs for every case. Every selected frame still satisfies
`index = ready.frame.frameSerial - 1`, has `postFinalCount=0`, and passes the
global and observed-viewport pixel audits. A post-run readback verified every
selected PPM and converted PNG hash in all six manifests; no hash mismatch was
found.

| Configuration / case | Evidence root | Selected layer indices | Terminal result |
| --- | --- | --- | --- |
| Debug / no-release | `G:\Temp\mgif-o07-r4-debug-no-release-20260908` | `2,4,6,11,13,27,29,31` | latch 1, focus-loss 1, suppressed 2, commit 0 |
| Debug / same-frame | `G:\Temp\mgif-o07-r4-debug-same-frame-20260908` | `2,4,6,11,13,26,28,30` | suppressed-release then focus-lost, latch 1, commit 0 |
| Debug / delivered-release | `G:\Temp\mgif-o07-r4-debug-delivered-release-20260908-r2` | `2,4,6,11,13,26,28,30` | delivered-release/commit 1, focus-loss 0 |
| Release / no-release | `G:\Temp\mgif-o07-r4-release-no-release-20260908` | `2,4,6,11,13,27,29,31` | latch 1, focus-loss 1, suppressed 2, commit 0 |
| Release / same-frame | `G:\Temp\mgif-o07-r4-release-same-frame-20260908` | `2,4,6,11,13,26,28,30` | suppressed-release then focus-lost, latch 1, commit 0 |
| Release / delivered-release | `G:\Temp\mgif-o07-r4-release-delivered-release-20260908` | `2,4,6,11,13,26,28,30` | delivered-release/commit 1, focus-loss 0 |

These current roots remain bounded `PASS_TRUE_OS_REPLAY_PARTIAL` evidence. They
do not promote full U03, do not repair the ordinary DWM/GDI transport, and do
not alter the frozen ledger, G8, P9/P10, or release policy.

## Remaining Work

The bounded O07 matrix is now complete for the explicitly recorded Vulkan-layer
transport. Full U03 remains partial: the reference manifest still treats the
native focus-loss debt as a later acceptance boundary, and the normal DWM/GDI
transport remains unavailable in this Windows/NVIDIA session. Any future
promotion must preserve the exact per-frame layer fence and repeat the matrix
on the target capture transport.

No reference manifest, progress ledger, G8/P9/P10 state, or release policy was
changed by this record. O05/O06 evidence and hashes remain unchanged.
