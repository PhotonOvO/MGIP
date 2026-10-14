# O09 Real-UI Reference Evidence Mirror

## Status

This is a compact pointer to the sealed external run at
`G:\ReferenceOracleRuns\vkgr-50373c7b-o09-real-ui-20260822T132518Z`.
No source, executable, runtime library, screenshot, log, fixture, or
`nvpro_core2`/`nvcore`/`nvapp`/`nvgui`/`nvvk`/`nvvkgltf` artifact was copied
into MGIF. The external run remains the evidence source of record.

O09 remains `native_partial`. All 298 registered D20 cells have a
fresh-process real GLFW/Win32, Dear ImGui, and Vulkan action-path capture plus
a numeric process exit identity, but that is not 298 complete UI state
replays. Exact application-serialized all-scene topology exists for 7 cells
and 20 checkpoints; current scene is visible-only for 298 cells, selection is
visible-only for 294 cells, complete visible history/Undo/Redo exists for 290
cells, and there are no private state readbacks.

## Identity And Seals

| Item | Bound value |
|---|---|
| Reference commit | `50373c7b0ff58078727af2c2aac70e29d5e74752` |
| Reference tree | `03e44de44217b109ab71dfbf6aee3989593d76b0` |
| Executable SHA-256 | `a02572b63f4af50bbaa26cf1909e1938bf7d72a7ae28904766d7e83394fd326b` |
| PDB SHA-256 | `6185aea9eb3b2fb7c27adf7db96443a69a478340f9a15a98f4c5d60aef693273` |
| `REPORT.md` | `7d676c34c16e892ff261d30c158c5b1756eee436bac08a8101a6acb9a2282221` |
| `INDEX.json` | `3c52c80daab9690224666d054c763de428b678e0f3898996d852938a73671b02` |
| `ARTIFACT-INDEX.json` | `6aefe5474f64e7a0442d865bf0a4249f31833110aced48543bd845b811700d64` |
| `INTEGRITY-REVERIFICATION.json` | `0448376ae2264562e9484dfa2166b644e7132f0bd0734984a5de57f68f92a86f` |
| `FINAL-SEAL.json` | `df21d2f4e90c022ba23f0d37737a5b5e4dcf90536b0dea5737f4d45b2afd1c69` |
| detached `SHA256SUMS.txt` attestation | `13943eac4d6ce8ce58ac28f539ff76fe102af0056a8bcb841a5b457a93ab654b` |

The engineering gate independently parses the 298-cell external index and
cross-checks every cell against the repository D20 registry. It compares case
ID, requirement, dimensions, expected outcome/status, raw trace hash, numeric
exit and timeout expectation, and exact source envelope; it then checks that
the selected native attempt is fresh, reference-bound, action-dispatched, and
truthfully classified as UI-driven without promoting visible state to private
state.

## Coverage Ceiling

| Observation | Count |
|---|---:|
| Registered cells / action paths / numeric exits | 298 / 298 / 298 |
| Clean selected exits | 290 |
| Expected `0xC00000FD` exits | 2 |
| Unexpected selected `0xC0000005` exits | 6 |
| CPU expectation and minimum-phase matches | 292 |
| Complete visible history sequences | 290 |
| Exact serialized topology cells / checkpoints | 7 / 20 |
| Visible-only current scene / selection | 298 / 294 |
| Private state readbacks | 0 |
| Screenshots / checksum entries | 2,186 / 8,660 |
| Integrity verification failures | 0 |

The six access-violation outcomes and two expected stack overflows remain
failures/conflicts in the reference evidence. They are not rewritten as
passes. The selected `mgif_safety_exception` is an owner decision recorded in
the parity manifest, not a conclusion or authorization created by this
evidence run.

## Remaining Debt

Later acceptance must test MGIF's clicked-occurrence routing, atomic rejection,
all-scene snapshots, shared-node occurrence consistency, iterative traversal,
and exact UI labels/layout/shortcuts/modal/focus/enablement/history text. This
mirror does not claim implementation completion, F42/R24 release readiness, or
full reference-state observability.
