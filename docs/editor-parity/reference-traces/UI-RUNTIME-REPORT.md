# Native UI/Vulkan Reference Evidence Mirror

## Status and location

This is a lightweight repository mirror of the sealed external reference run at
`G:\ReferenceOracleRuns\vkgr-50373c7b-ui-runtime`. It records identities,
hashes, coverage limits, and screenshot metadata only. No screenshot, source
export, build product, executable, log corpus, or other large run artifact was
copied into MGIF.

The run provides **targeted partial P0-T02 evidence only**. It does not close
P0-T02 as a whole, any obligation listed below as open, or D20.

## Reference identity

| Item | Exact identity |
|---|---|
| Application | `vk_gltf_renderer` commit `50373c7b0ff58078727af2c2aac70e29d5e74752` |
| Application tree | `03e44de44217b109ab71dfbf6aee3989593d76b0` |
| Application source archive | `source-vk-gltf-renderer-50373c7b.tar.gz`; SHA-256 `d4a293e76a349d85c1f220fe8d0cd9ec01de92f50dff5b81a644a53a5d458c6e` |
| Companion | `nvpro_core2` commit `9b482eb8961433c51b77e04ac48964f4eb585481` |
| Companion tree | `d9881fe4bfed55f8d9238e4ae16ac44d00bcaaf8` |
| Companion source archive | `source-nvpro-core2-9b482eb8.tar.gz`; SHA-256 `ed698eb45b838042c3536226e764aee0fce11973e2569adfd473bb4ee40b08f2` |
| Executable | `runtime-exact-name/_bin/RelWithDebInfo/vk_gltf_renderer.exe`; 16,502,784 bytes |
| Executable SHA-256 | `a02572b63f4af50bbaa26cf1909e1938bf7d72a7ae28904766d7e83394fd326b` |
| Build/runtime | Visual Studio 17 2022 x64, `RelWithDebInfo`, Vulkan SDK 1.4.328.1, Slang 2026.5, runtime validation enabled |

The application does not pin its moving `nvpro_core2` dependency. The
application and companion commit/tree pairs above therefore both participate in
the executable provenance.

## Independent integrity check

The repository mirror was produced after a read-only comparison against the
external files, not from prose alone.

| Check | Verified result |
|---|---:|
| Exact-commit exports | **1,778 / 1,778** files verified |
| Direct Git-blob byte matches | 201 |
| Matches after recorded Git clean normalization | 1,577 |
| Export missing / mismatch / extra | 0 / 0 / 0 |
| `SHA256SUMS.txt` artifact entries | **7,104 / 7,104** hashes matched |
| Artifact missing / mismatch / malformed entries | 0 / 0 / 0 |
| Screenshot records | 23 total: 19 primary, 4 superseded |
| Screenshot mechanical nonblank results | 23 / 23 true |

The export result is authoritative in
`provenance/export-verification-v2.json`. Its SHA-256 is
`4b20dee41fa765d50814012c393a26b837b25bb654b199d1d37729361a7e0220`.
The complete artifact list is `SHA256SUMS.txt`, whose independently calculated
SHA-256 is
`b446338749310b3e0ee30de85422577f8ffd3531ff014841eeab69734b207191`.

Key sealed metadata bindings:

| External artifact | SHA-256 |
|---|---|
| `REPORT.md` | `5add6d86f88cb33c351509cf8d9f3bfc1c3d2730d177e3ba287e97114b52d4c5` |
| `SCREENSHOT-INDEX.json` | `bf38b15883a7144b912a3dbfd4f95064c5777a44c15458c1508c8935d7a92df6` |
| `SCREENSHOT-INDEX.md` | `ce46f8c67f75975320b0b954fc14e0fdbfc30efcfca2df7016f01b5f61b05c20` |
| `provenance/build-identity.json` | `aa88097b0c4991b19fac6bcf1064b8ba7316a2e4d0be723e273985e9d93e7b96` |
| `provenance/build-runtime-provenance.md` | `81c33d2208e4562281031abd0e68a6ce966d429b8c0168b213b43e2060c63d87` |
| `provenance/action-trace-index.json` | `5cd9e9a72b08d0c345156e17989861ed097a44b7ad5bfbbac330b2b42a79f7a4` |

## Strict evidence ceiling

| Scope | Status after this run |
|---|---|
| P0-T02 | **Targeted partial only**; no exhaustive hover/active/disabled/busy or viewport/DPI matrix |
| O04 | **Not closed**; generic Delete modal surface only, not the unique-root lifecycle/results |
| O05 | **Uncovered**; no active-drag Escape or release-ordering trace |
| O06 | **Partial**; Duplicate, Undo, and history labels only |
| O07 | **Uncovered**; no focus-loss partition or synthetic-release trace |
| O08 | **Not closed**; live viewport only, without picking/camera/Primitive/browser-focus results |
| O09 | **Partial**; multiple-scene controls and generic actions only |
| O10 | **Not closed**; visible Scene List/Inspector surfaces only, without clipboard or assignment/history traces |
| D20 replay | **0 / 298** D20 cells replayed in the real UI; prior CPU/domain traces are not upgraded to ImGui, Vulkan, or GPU evidence |
| D20 policy | **None selected**; neither `exact_defect_parity` nor `mgif_safety_exception` is chosen by this run |

## Production dependency boundary

`nvpro_core2`, `nvcore`, `nvapp`, `nvgui`, `nvvk`, and `nvvkgltf` are
**external-reference provenance only** and are forbidden MGIF production
dependencies. Their presence in the sealed run identifies how the external
reference executable was reproduced; it does not authorize copying, linking,
adapting, or introducing those interfaces into MGIF.

MGIF uses Dear ImGui directly with MGIF-owned app, RHI, and scene abstractions.

## Primary screenshots (19)

All files below are external paths relative to the sealed root. All are genuine
1616x1039 native-window captures and passed the mechanical nonblank check.

| File | SHA-256 | State | Grade | Runtime |
|---|---|---|---|---|
| `screenshots/01-clean-first-layout.png` | `9905c5c48864cd198d80d8415b0c776d8d54d1ae13784cd1ee327359dbe97fcf` | Clean default editor layout with live ShaderBall rendering, canonical docks, FPS, and frame counter | `primary-native-ui-vulkan` | `runtime-03` (PID 73272) |
| `screenshots/02-file-menu.png` | `a6567dbd032a8b02dad55b100a8e0e9cf31999216f83d7a93b4fade3e1466942` | File menu open | `primary-native-ui-vulkan` | `runtime-03` (PID 73272) |
| `screenshots/03b-edit-menu.png` | `8979691e2501ecfdb9ae1394ea5b3f2dedaaa4539bbc200672a49ec87054f4f1` | Edit menu open | `primary-native-ui-vulkan` | `runtime-03` (PID 73272) |
| `screenshots/04b-view-menu.png` | `cb3c22b214978182cb84b5ff1eff0ecc3a8e89478db153dcaa09847a16d8a431` | View menu open | `primary-native-ui-vulkan` | `runtime-03` (PID 73272) |
| `screenshots/05b-windows-menu.png` | `6872f0cb2e3345014b2a3bebd13b3018ece93dcba3333e95db8aeaf43a5a348b` | Windows menu open | `primary-native-ui-vulkan` | `runtime-03` (PID 73272) |
| `screenshots/06b-tools-menu.png` | `4d5dbab4755c81b9634726042aea73cbae6a3ad9a48ba4794a1847062c7fa9f0` | Tools menu open | `primary-native-ui-vulkan` | `runtime-03` (PID 73272) |
| `screenshots/07-multi-scene-clean-layout.png` | `fa75c031b42c28778d4dc6457313751b87efc45c3e2975343d49d34d57299dac` | Clean layout with the GPU-valid two-scene fixture loaded | `primary-native-ui-vulkan-gpu-correlated` | `runtime-05` (PID 13472) |
| `screenshots/08-settings-multiple-scenes-expanded.png` | `74c96267542fbbbe71ef036b4184395690f918e7c59cc9dc8b392afa119f2c1f` | Settings Multiple Scenes header expanded with ordered Scene-0 and Scene-1 choices | `primary-native-ui-vulkan-gpu-correlated` | `runtime-05` (PID 13472) |
| `screenshots/09-settings-scene-1-active.png` | `f5926113859b9084201c1dede032d92ec244fb216e620e617f51f6783e91ec4f` | Scene-1 activated after a real radio-button click; reset frame counter shows the runtime rebuild | `primary-native-ui-vulkan-gpu-correlated` | `runtime-05` (PID 13472) |
| `screenshots/10-scene-list-tab.png` | `ae05b9071793d263e273522d388dc0893a2f16b0d0ddef2e35bfa924f10bcc2e` | Scene List tab showing real asset groups | `primary-native-ui-vulkan-gpu-correlated` | `runtime-05` (PID 13472) |
| `screenshots/11-hierarchy-node-context.png` | `4f3176d22aa5277abe68fdf9b1489cc943033991be03adcc1c97b5ca07b68a74` | Hierarchy node context menu open | `primary-native-ui-vulkan-gpu-correlated` | `runtime-05` (PID 13472) |
| `screenshots/12-delete-confirm-modal.png` | `f85d0625b8d2485c8b89a44bfc157de4994d1b6c7f1989b36765ec60646a31ee` | Delete confirmation modal opened from the hierarchy context menu | `primary-native-ui-vulkan-gpu-correlated` | `runtime-05` (PID 13472) |
| `screenshots/13-rename-modal.png` | `b28153c87aa65ba37d5ed25b4c64bb06064c035fba02a08e00e80c8c60ae7cbc` | Rename modal opened from the hierarchy context menu | `primary-native-ui-vulkan-gpu-correlated` | `runtime-05` (PID 13472) |
| `screenshots/14-duplicate-executed.png` | `df4714370d2f70105b80328641e0cb375c5d612a4498bece1f46bb0502231cd7` | Duplicate executed with ShaderBall_copy visible and selected in Inspector | `primary-native-ui-vulkan-gpu-correlated` | `runtime-05` (PID 13472) |
| `screenshots/15-edit-menu-undo-duplicate.png` | `a20ce0b86f058d7670a74b117d41d44cde3aae4c07ced1d064521af0b8906e6a` | Edit menu shows Undo Duplicate 'ShaderBall' after the real duplicate action | `primary-native-ui-vulkan-gpu-correlated` | `runtime-05` (PID 13472) |
| `screenshots/16-undo-duplicate-executed.png` | `b3254d2daaeb20332bb36186061acdd8e2d5334ebd5ca79de6ad615d51351641` | Ctrl+Z executed; ShaderBall_copy removed from the hierarchy | `primary-native-ui-vulkan-gpu-correlated` | `runtime-05` (PID 13472) |
| `screenshots/17-edit-menu-redo-duplicate.png` | `593a37ddc239c113e85b7145b037ce14728cab48cce88394fd2d4762109265cd` | Edit menu shows Redo Duplicate 'ShaderBall'; capture is valid but the immediate menu-open input sidecar was not retained | `primary-native-ui-vulkan-gpu-correlated` | `runtime-05` (PID 13472) |
| `screenshots/18-scene-row-context.png` | `7d4cd0496a2e6fef22fdcae150c70e4c1b0d7b4e0e75f02b0cf6131d1052158a` | Scene-row context menu with Add Node and three light actions | `primary-native-ui-vulkan-gpu-correlated` | `runtime-05` (PID 13472) |
| `screenshots/19-add-child-submenu.png` | `457b52d3ac23f1e3cd2c41c367efff73ee53622ce2daca8e4a7259974c383214` | Add Child submenu with Empty Node, Point Light, Directional Light, and Spot Light | `primary-native-ui-vulkan-gpu-correlated` | `runtime-05` (PID 13472) |

## Superseded screenshots (4)

These retained toggle attempts are trace artifacts, not citable evidence for
their intended menu states.

| File | SHA-256 | State | Grade | Runtime |
|---|---|---|---|---|
| `screenshots/03-edit-menu.png` | `fc3270244948c645847ec5253cdbefe010b5a7048c728f3546b4561a6663042e` | Superseded Edit-menu toggle attempt; intended menu state was not reliably captured | `superseded-not-citable` | `runtime-03` (PID 73272) |
| `screenshots/04-view-menu.png` | `edde9a4a0bebfaebebb832c8c7a888e7fe970110e6cba89e66063072cfdc341d` | Superseded View-menu toggle attempt; intended menu state was not reliably captured | `superseded-not-citable` | `runtime-03` (PID 73272) |
| `screenshots/05-windows-menu.png` | `0954bd901fdc160c703fde617f408cbec2ae1696377e544bc8d3bad2ca457375` | Superseded Windows-menu toggle attempt; intended menu state was not reliably captured | `superseded-not-citable` | `runtime-03` (PID 73272) |
| `screenshots/06-tools-menu.png` | `9d88fb104516a6eadb9355ba95ff82b6ac6ffd6190d67015f0a31761e8412f2b` | Superseded Tools-menu toggle attempt; intended menu state was not reliably captured | `superseded-not-citable` | `runtime-03` (PID 73272) |

Machine-readable metadata is in `ui-runtime-index.json`. Audit the full run at
the external sealed root; this mirror is deliberately not a substitute for the
sealed artifacts.
