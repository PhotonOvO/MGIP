# Material Field Switch Stalls

## Measurement

Baseline: `F:\Captures\2026-09-29_14-40-42_Demo.slp`, running
`G:\MGIF\build\Debug\Demo.exe`. Duration: 34.297 s; sampling interval:
0.1221 ms; Main thread: 0 (OS 11776). Symbols resolved for 59/357 modules,
including 10,989 functions in Demo.exe.

The inferred frame timeline has a 17.51 ms median and three large windows:

| Start | Wall Time |
| --- | --- |
| 2.101 s | 1,753 ms |
| 4.851 s | 1,494 ms |
| 7.492 s | 1,549 ms |

Frame boundaries are inferred from sampling, not explicit frame markers.
No validation samples were silently excluded. Validation accounts for 25.33%
of Main-thread self samples over the complete trace. CRT export leaf labels
are unresolved; the diagnosis relies on resolved application ancestors.

## Diagnosis

In the 2,100-3,854 ms Main-thread window, bounded `slp.py callees` queries show:

- `finishFieldEdit -> commitMaterialDraft`: 1,384 ms.
- Material authority commit: 508 ms, of which document digest costs 494 ms.
- Projection rebuild: 574 ms, including 497 ms in `reprojectMaterialParameters`.
- That incremental path still copies a large projection (140 ms) and rebuilds
  canonical snapshot storage (344 ms).
- Renderer enqueue copies the projection again (119 ms); flush costs 308 ms.

There are **zero `GltfLoader::processImages` samples** over the new trace.
The earlier PNG-decode fix is active. The remaining stall is synchronous
canonicalization and copying of roughly 214 MB of projection payloads.

## Changes

Scene payloads and snapshot byte owners are now immutable shared values.
Projection snapshots retain metadata fragments and existing bulk payload
owners instead of allocating and copying a contiguous payload each edit.
Streaming SHA-256 hashes the same concatenated bytes. Full-snapshot consumers
can still request a contiguous representation.

The authority digest keeps unchanged payload URIs and checked SHA-256/FNV
prefix state. Pinned TinyGLTF serializers update only the edited material's
cached metadata entry. Structural changes invalidate the caches, and failures
restore material metadata alongside authority values and digest fences.

The normal/occlusion-only no-op check was also corrected. Previously it only
compared the other material parameters and could discard scalar-only edits.

## Verification

- 27 related Debug CTest cases passed, including renderer integration,
  document save/roundtrip, history, projection, publication, and fault rollback.
- The non-Windows SHA-256 implementation passed the GCC C++20 test build.
- Sponza real-mouse test: switch 0.42 ms, second drag 0.42 ms, leave/commit
  100.40 ms. This is a controller/UI test, not a full GPU frame measurement.
- Base Color picker sessions: commit 106.32/100.32 ms, maximum drag frame
  0.66/1.02 ms, with one publication and history entry per session.
- Complete authority text hashes and incremental/full canonical projection
  bytes, digests, mappings, and undo/redo results are equivalent.

- Debug, Release, Profile, and `G:\MGIF\build\Debug\Demo.exe` rebuilt cleanly.
- The usual Debug Demo rendered Sponza in a three-frame Vulkan smoke and exited
  with code 0, without reported VUID, load, or publication errors.

Build paths, final key-test results, and the executable SHA-256 are tracked in
`investigation.json`.
A new trace from the rebuilt Debug Demo is still needed for actual Main-thread
verification. A roughly 100 ms synchronous Sponza commit remains; this report
does not claim a completely stutter-free interaction.
