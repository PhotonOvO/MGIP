# F39 Renderer Repair Evidence

This record supplements, and does not replace, the immutable failure baseline
in `MGIF-BASELINE.md`. The fixed baseline at commit `6c36567625b305286755a734a66188a22df18e77`
used Demo SHA-256 `92226d901a95d0e6b2abbcb5c33acc788f081473ae7dca11d45e8ababb1f08c8`
and failed 0/5 cases with `SYNC-HAZARD-READ-AFTER-WRITE`; that historical fact
is still binding and is not waived.

The current repaired working-tree build produced Demo SHA-256
`789518c264c43d8426345576e43a7c138082eed9c67466c37c8562c5d8e26f1a`
and passed the five-case Vulkan synchronization smoke run at
`out/validation/csm-vulkan-smoke-20260822-220652-789518c264c4`. Every case
exited 0, emitted every required marker exactly once, retained the same
executable hash before and after execution, produced an empty stderr log, and
matched no validation/failure pattern. The DDGI case emitted its required SDF
marker exactly once.

The bound `summary.json` SHA-256 is
`a70b8f1ec6913644975ce48de07a096b9ff34bfd4e3b3fd919b2f4c40c4f5103`;
the bound `identity.json` SHA-256 is
`5d98174227443004831c71101f88af1667ebf7e0328ee1e5c7f23509f79fa8e0`.
The engineering validator reparses the summary and all referenced result and
log files instead of accepting this prose as the test oracle.

This is focused renderer-repair evidence, not a claim that the complete F39
editor visual matrix, F42, R24, or final product acceptance has passed.
