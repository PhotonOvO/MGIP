# Native UI Semantic Reference Evidence Mirror

## Status and location

This is a lightweight repository pointer to the sealed external native UI
semantic run at
`G:\ReferenceOracleRuns\vkgr-50373c7b-ui-semantic-20260822T100212Z`.
The sealed run, not this mirror, is the evidence source of record.

No screenshots, source exports, executables, binaries, runtime logs, fixture
payloads, or `nvpro_core2`/`nvcore`/`nvapp`/`nvgui`/`nvvk`/`nvvkgltf`
artifacts were copied into MGIF. This mirror contains pointer metadata,
independently recalculated hashes, classifications, and coverage ceilings
only.

The run contains 20 native-citable cases and 21 diagnostic cases. It provides
targeted native observations for O05, O06, O07, O08, and O10; it is not a
blanket closure claim. D20 was intentionally not evaluated and remains
unresolved.

## Exact reference identity

| Item | Independently verified identity |
|---|---|
| Application | `vk_gltf_renderer` commit `50373c7b0ff58078727af2c2aac70e29d5e74752` |
| Application tree | `03e44de44217b109ab71dfbf6aee3989593d76b0` |
| Companion provenance | `nvpro_core2` commit `9b482eb8961433c51b77e04ac48964f4eb585481`, tree `d9881fe4bfed55f8d9238e4ae16ac44d00bcaaf8` |
| Exact executable | `G:\ReferenceOracleRuns\vkgr-50373c7b-ui-runtime\runtime-exact-name\_bin\RelWithDebInfo\vk_gltf_renderer.exe` |
| Executable identity | 16,502,784 bytes; SHA-256 `a02572b63f4af50bbaa26cf1909e1938bf7d72a7ae28904766d7e83394fd326b` |
| Configuration | `RelWithDebInfo`; runtime Vulkan validation enabled |

The application checkout at `F:\GitHub\vk_gltf_renderer` independently
resolved to the commit and tree above. The companion checkout independently
resolved to its stated commit and tree. Every one of the 20 citable launch
sidecars names a case-local hard-linked copy of the exact executable, records
the same runtime hashes, fixture hashes, command line, working directory,
process identity, and initial visible/responding native window state, and does
not execute from the sealed binary directory.

## External artifact bindings

All paths below are relative to the sealed semantic run root unless an
absolute path is shown.

| External artifact | Bytes | Independently calculated SHA-256 |
|---|---:|---|
| `REPORT.md` | 16,657 | `4c25c26505e9d3beecf45064a1ba9feeed8eda7e9072a35333a20bcfa0a20554` |
| `INDEX.json` | 9,638 | `91ffb73304c0b23747f83511dee179296e922df1e4bcc843d5c220278a6aaf53` |
| `ARTIFACT-INDEX.json` | 267,715 | `ac6d2b6b129d62f4429459ee80675df65ee857ffa00e43fc0d70676fa28de202` |
| `SHA256SUMS.txt` | 104,622 | `44af679974916cdef5d3727427d05e20e7fd2a2480abf76a5b30733ef5ed8327` |
| `provenance/identity.json` | 715 | `80ce6f08712ac066e13a2d090343b46973ba444c1c3c1b5298fbfe51111dfbcd` |
| `provenance/evidence-classification.json` | 9,019 | `49a392d511beb79c07eb03d00ffe1d25a93b7d814a2cc554b20c8216b44c996b` |
| `provenance/final-safety-audit.json` | 3,658 | `e5705a96b106d724d330ef9866e70fddb2584f15dfd319e0ace1eed4d26bb06b` |
| `provenance/clipboard-cleanup.json` | 396 | `c9b6dd55a4752c080939a8ee535c69ed04c512eadcc7ad3fdfd11ed7fefe42a2` |
| `provenance/native-log-scan.json` | 10,633 | `305d8332468cb90df296f923bbf27da861d073313db4098e6280b83afe02d136` |
| `provenance/sealed-prior-run-restoration-audit.json` | 1,730 | `3b03c329e3923967f48f344b2bf83968ef1d2f91cfeca70227683a4dc695c638` |
| `provenance/source-evidence.json` | 3,006 | `651f9be9285d0f434c93795467dc6b3f81ad65d47a4fead4b4dafb6df463d193` |

## Independent integrity verification

The mirror was prepared from a read-only verification of the external run,
not by trusting its prose summaries alone.

| Check | Verified result |
|---|---:|
| External files on disk | 769 |
| `SHA256SUMS.txt` entries parsed | 768 / 768 |
| Manifest entries independently hashed and matched | 768 / 768 |
| Malformed / escaping-root / duplicate manifest entries | 0 / 0 / 0 |
| Missing / mismatched / unmanifested files | 0 / 0 / 0 |
| `ARTIFACT-INDEX.json` records | 767 / 767 |
| Artifact path/hash/byte mismatches | 0 |
| Native-citable classifications | 20 unique cases |
| Diagnostic classifications | 21 unique cases |
| Classified case directories | 41 / 41, with no unclassified directory |
| Citable launch-sidecar identity checks | 20 / 20 passed |
| Retained native logs independently scanned | 20 / 20 present; zero matching logs |
| Live reference renderer / focus-sink processes | 0 / 0 |

`SHA256SUMS.txt` binds every produced file except itself. Its 768 entries are
the 767 records represented by `ARTIFACT-INDEX.json` plus
`ARTIFACT-INDEX.json` itself; the artifact index excludes itself and the
checksum manifest to avoid recursive hashes. The checksum manifest's own hash
was calculated separately and is listed above.

The independent native-log scan searched for `VUID`, `validation error`,
`ERROR`, and `WARNING`. Zero text matches is a retained-log observation, not
proof that every possible validation path executed.

## Sealed runtime hashes

The sealed source runtime directory contains exactly the four indexed files
below and no unexpected file. The semantic run's `runtime-master` contains
hard-linked copies of the first three with the same hashes.

| Sealed runtime file | Bytes | Independently calculated SHA-256 |
|---|---:|---|
| `slang-compiler.dll` | 26,345,472 | `531feaa16939f06bbf32b2a0a6bc01158e9ce1c7210d80e78ac983d5b97e2d00` |
| `slang-glslang.dll` | 11,006,464 | `4e9681bd1a8a2f9e21ffaa987973df14efb58d63d8c995eb4696e0c07a8ed91b` |
| `vk_gltf_renderer.exe` | 16,502,784 | `a02572b63f4af50bbaa26cf1909e1938bf7d72a7ae28904766d7e83394fd326b` |
| `vk_gltf_renderer.pdb` | 84,807,680 | `6185aea9eb3b2fb7c27adf7db96443a69a478340f9a15a98f4c5d60aef693273` |

## Strict native coverage ceiling

Only the achieved states below are native-observed. Source evidence explains
code paths but does not upgrade a listed native gap.

| Oracle | Exact native coverage achieved | Exact remaining native gap |
|---|---|---|
| O05 | Active visible gizmo drag; Escape only while physical left remained down; visible selection/gizmo detach; late release did not restore selection | Private gizmo dragging flag; definitive history-stack mutation |
| O06 | Escape-then-release and release-then-Escape with no intervening renderer frame; same visible cleared-selection/transformed-scene result; Undo visibly disabled in both | Private callback trace; direct history-stack memory |
| O07 | Real focus loss with no release; late physical release; synthetic renderer release while physical left stayed down; physical release before real focus loss | Actual recipient of late physical release; private gizmo dragging flag; definitive history-stack state |
| O08 | Delayed single dispatch; same-primitive second single clears; blank clears; valid hit focuses Scene Graph; valid double visibly reframes camera; native hit world position and distance | Invalid render-node hit; stale hit; hidden-object hit; numeric camera center; numeric manipulator speed |
| O10 | Canonical OS clipboard Copy index 0; canonical Paste index 1; named valid tooltip; wrong-prefix no-material tooltip; nonnumeric suffix coerced to material 0; active-document range rejection; cross-document native source copy and target validation; Inspector assignment modal; Paste and Assign leave Undo disabled | Native decimal-overflow result; exact restoration of pre-task OS clipboard contents |

The 20 citable cases partition as 1 O05, 2 O06, 4 O07, 5 O08, 7 O10,
and 1 runtime/Vulkan/GPU corroboration case. The exact case IDs and all 21
diagnostic IDs are retained in `ui-semantic-index.json`; the full action,
screenshot, log, and diagnostic metadata remains external.

## Clipboard cleanup limitation

The original OS clipboard data object was preserved and restored around the
citable O10 suites. A later manual decimal-overflow diagnostic crossed that
in-memory restoration boundary. The OS clipboard then remained locked through
20 cleanup attempts, so neither removal of the final test payload nor exact
restoration of the pre-task clipboard can be verified or claimed. This is an
explicit evidence gap, not a successful-cleanup claim.

## D20 decision boundary

D20 is `not-evaluated-unresolved` for this run. It has no achieved state here,
and this mirror neither resolves D20 nor selects or authorizes
`exact_defect_parity` or `mgif_safety_exception`. Evidence from this run must
not be used to upgrade source/CPU/domain evidence into native UI, Vulkan, or
GPU closure for D20.

## Production dependency boundary

Every occurrence of `nvpro_core2`, `nvcore`, `nvapp`, `nvgui`, `nvvk`, or
`nvvkgltf` in this mirror is external reference provenance only. All six are
forbidden MGIF production dependencies. Their reference provenance does not
authorize copying, linking, adapting, or introducing their source, binaries,
artifacts, or interfaces into MGIF; MGIF must continue to use Dear ImGui
directly with MGIF-owned application, RHI, and scene abstractions.

Machine-readable pointer metadata is in `ui-semantic-index.json`. Audit any
claim against the external sealed root; this compact mirror is deliberately
not a substitute for the external evidence corpus.
