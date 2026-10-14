# MGIF Editor-Parity Baseline Evidence

This record summarizes the isolated P0 baseline run for MGIF commit
`6c36567625b305286755a734a66188a22df18e77`. The run used an immutable source
snapshot and build trees below `%TEMP%`; it did not build in or modify the MGIF
working tree.

## Evidence Identity

- Captured: `2026-08-22` (`Asia/Shanghai`)
- MGIF commit: `6c36567625b305286755a734a66188a22df18e77`
- Baseline executable SHA-256:
  `92226d901a95d0e6b2abbcb5c33acc788f081473ae7dca11d45e8ababb1f08c8`
- Full raw report:
  `%LOCALAPPDATA%/Temp/mgif-baseline-6c36567-20260822-132137-13c71a43/baseline-evidence-report.md`
- Full raw report SHA-256:
  `06d58de11cc4375a545f0c229460e54ea9859baf5acd51086974f2f734b39cd7`
- Vulkan smoke summary SHA-256:
  `e97aa14474561b2b24a536920a1b6c19643c171024c70896eaf6fb3a81e66717`

The raw evidence directory is machine-local and is not a release artifact. A
P10 candidate must create a fresh, self-contained directory under the evidence
contract in section 16.5 of the task specification.

## Host

| Item | Value |
|---|---|
| OS | Windows 11 Enterprise `10.0.26100`, x64 |
| Visual Studio | Community 2022 `17.14.16` |
| MSVC | `19.44.35217.0` |
| Windows SDK | `10.0.26100.0` |
| CMake / Ninja | `4.1.2` / `1.13.2` |
| Vulkan SDK | `1.4.328.1` |
| NVIDIA adapter | GeForce RTX 4070, driver `580.88` |
| Intel adapter | Intel Graphics, driver `101.6129` |
| System DPI | 96 (100 percent) |

The application did not identify its selected physical device before the
validation break, so this record does not infer which enumerated adapter ran
the smoke test.

## Results

| Gate | Result | Detail |
|---|---|---|
| Full Ninja Debug configure/build | PASS | 243 build edges completed |
| Full CTest | PASS | 12/12 |
| Windows RHI contract, Ninja | PASS | Public contract and D3D12 object target built |
| Windows RHI contract, Visual Studio | PASS | Public contract and D3D12 object target built |
| Strict RHI boundary wrapper | PASS | 0 blocking; 38 pre-existing informational findings |
| Python contract discovery | PASS | 263/263 after an isolated build-discovery junction |
| RenderDoc protocol self-tests | PASS | Comparator and harness self-tests passed |
| Windows Vulkan validation smoke | FAIL | 0/5; deterministic pre-editor synchronization hazard |
| Metal contract | NOT RUN | No macOS executor was available in this run |
| Android isolation/build | NOT RUN | No Android Gradle/NDK run was performed |

## Existing Vulkan Failure

All five baseline smoke cases exited with `0x80000003` before `scene-ready`.
Each reported the same `SYNC-HAZARD-READ-AFTER-WRITE` in
`GPUDrivenClusteredLightCulling`. The executable hash remained unchanged before
and after every case. This is a reproducible defect in the fixed, unmodified
MGIF baseline, not an editor regression and not an environment launch blocker.

The editor acceptance gate remains red until the synchronization defect is
fixed and the complete editor Vulkan matrix passes. This record is not a
waiver and must not be used to suppress validation output.

## Reproduction Root

The raw report contains the complete commands, timings, resolved FetchContent
commits, stdout/stderr logs, five-case smoke script, and artifact index. Its
primary isolated root is:

```text
%LOCALAPPDATA%/Temp/mgif-baseline-6c36567-20260822-132137-13c71a43/
```

The baseline should be regenerated if that temporary directory is no longer
available. Do not combine its artifacts with a different executable, dirty
diff, commit, machine, or P10 candidate run.

## Current Renderer Repair Supplement

The immutable baseline above remains the historical result and is not
rewritten or waived. A later repaired working-tree build produced Demo
SHA-256 `789518c264c43d8426345576e43a7c138082eed9c67466c37c8562c5d8e26f1a`
and passed 5/5 focused Vulkan synchronization smoke cases in
`out/validation/csm-vulkan-smoke-20260822-220652-789518c264c4`.

The repair run's `summary.json` is 6,400 bytes with SHA-256
`a70b8f1ec6913644975ce48de07a096b9ff34bfd4e3b3fd919b2f4c40c4f5103`;
`identity.json` is 541 bytes with SHA-256
`5d98174227443004831c71101f88af1667ebf7e0328ee1e5c7f23509f79fa8e0`.
All five cases exited 0, emitted every required marker exactly once, retained
the same executable hash before and after execution, had empty stderr, and had
no recorded failure-pattern match. See `F39-REPAIR.md` and
`f39-repair-index.json` for the bounded evidence contract.

This supplement proves the focused renderer repair smoke on that executable;
it does not claim the complete editor F39 visual matrix, F42/R24, or final
release acceptance has passed.
