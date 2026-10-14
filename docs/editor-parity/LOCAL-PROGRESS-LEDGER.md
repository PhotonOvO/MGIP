# Local Progress Ledger Contract

## Status

This document defines the active progress protocol after P5. The legacy
progress pointer remains a frozen, content-addressed origin through the
accepted P5 engineering Gate. It is no longer an execution or task-status
service.

Active local progress is selected by:

```text
docs/editor-parity/progress-ledger-local-current.json
```

The local publisher and independent validator are:

```text
tools/local_editor_progress_ledger.py
tools/validate_editor_local_progress.py
```

Neither tool starts, queries, or imports an orchestration runtime. New local
documents cannot contain runtime, dispatch, worker-message, worker-result, or
Orca-result fields. The only permitted `legacy-orca` value is an explicit
source-kind label identifying frozen pre-migration evidence.

## Trust Boundary

The local ledger proves a deterministic filesystem evidence chain. It does
not claim that a content hash proves a reviewer's real-world identity.

Concurrent publication is defined only for publisher/recovery processes that
honor the repo-keyed OS advisory writer guard. The guard handle is retained
for the whole transaction and is released by the OS on process death; the
repository lock record remains diagnostic and binds a crash journal token.
All publishers for one repository must run on the same host/platform.

The advisory guard is the fixed repository file below. It is never removed or
used as task evidence; only its first byte is OS-locked:

```text
docs/editor-parity/.progress-ledger-local.guard
```

Arbitrary tools that edit pointer or mirror files without honoring this guard
are outside the concurrency threat model. Phase-boundary hash checks detect
such drift when observable, and guarded rollback refuses unknown hashes, but
ordinary filesystems do not provide an atomic compare-and-replace primitive
against a non-cooperating editor. Do not manually edit selected ledger files
while publication or recovery is running.

Each transition requires:

- the exact selected parent bundle and snapshot;
- the canonical pre-transition task hash;
- a task that is currently `ready` after deterministic dependency replay;
- a Markdown report and a closed JSON verdict with exact bytes and SHA-256;
- fixed policy preserving F42/R24, Metal, and release/distribution status;
- `reviewAuthority = local-automatic-review` and a completed review execution.

The publisher rewrites external artifact paths to repository-relative
content-addressed paths before hashing the durable receipt. Absolute temporary
paths therefore never enter the selected local snapshot.

A recovery task with `role = integration`, phase `P8`, and a `-R1` display
suffix may explicitly supersede one previously failed P8 integration task.
The superseded task remains immutable and failed; only a completed recovery
task satisfies descendants that named that failed task. The recovery task
must not depend on its superseded task, and it cannot replace a successful
task. This is the append-only retry path used to recover a failed integration
without rewriting its receipt or silently promoting a phase.

## State Transition

One publication contains exactly one task transition:

```text
ready -> completed  when outcome = succeeded
ready -> failed     when outcome = failed
```

After replay, pending tasks whose dependencies are all completed become
`ready`; tasks with a failed or blocked dependency become `blocked`. No
dispatch identity is synthesized for these derived states.

## Append-Only Recovery

Protocol v1 receipts and snapshots remain valid byte-for-byte. A v1 chain is
projected with its original semantics until a recovery receipt is appended.
The first recovery append creates a schema-version-2 snapshot and state while
retaining the exact v1 receipt prefix.

A v2 ordinary receipt continues to terminate an existing `ready` task. A v2
recovery receipt atomically introduces and terminates one new recovery task.
The two closed forms cannot be mixed or partially populated. A recovery task's
definition contains a new task ID, display name, sorted dependency IDs,
canonical creation time, phase, role, and optional superseded task ID. The
definition is authorized by a separately sealed JSON artifact containing:

```text
documentKind = local-recovery-task-authority
parentBundleId / parentSnapshotSha256
taskDefinitionSha256
authorizedAtUtc
fixedPolicy
```

The authority, report, verdict, and transition receipt are copied to the
content-addressed evidence store. The publisher rejects an unknown dependency,
duplicate task ID, cycle, stale parent, non-monotonic time, forged authority,
artifact mismatch, or definition that is not `ready` against the selected
parent. Recovery tasks never reset or overwrite a terminal task.

A recovery task with role `gate` also carries one explicit local Gate event:

```text
phase
gateVerdict = ENGINEERING PASS | FAIL/BLOCKED
supersedesEventId
```

The superseded event must be the latest unsuperseded event for the same phase,
and its task must match the recovery definition's `supersedesTaskId`. Historical
Gate events remain immutable. Phase status is derived from the latest
unsuperseded local event; the complete legacy and local Gate receipt history is
retained in the phase projection.

A Gate receipt may declare a closed dependency replacement naming an existing
blocked/pending task, one of its exact dependencies, and an accepted-Gate
predicate. The replacement is effective only while the declaring event is the
latest event for that phase and its verdict is `ENGINEERING PASS`. A failed
re-Gate therefore cannot unblock descendants. This permits a fresh Gate task
to supersede a failed Gate without rewriting either the failed task or the
downstream task definition.

The independent validator reconstructs recovery definitions, authorities,
Gate events, replacements, terminal overrides, task readiness, and phase state
from the complete receipt chain. Snapshot indexes and state/Markdown mirrors
must equal that replay exactly; they are never trusted as primary evidence.

The first transition is anchored to the frozen legacy pointer. Every later
transition names the selected local parent bundle, parent snapshot, and prior
transition receipt. Receipt lists are append-only and each selected parent
must contain the exact prior prefix.

## Atomic Publication

The publisher uses a local writer lock, content-addressed evidence and bundle
stores, mirror preimages, a publication journal, and a pointer-replacement
commit point. An independent validator reconstructs the effective task state
from the frozen origin and the complete receipt chain before the pointer is
replaced, then validates the selected pointer again after commit. Pointer
replacement is the sole commit point: recovery rolls back only before that
point and rolls forward when pointer and mirrors all match the journal's new
hashes. A post-commit confirmation failure cannot retract a prevalidated
pointer.

The frozen legacy files are never rewritten by a local publication:

```text
docs/editor-parity/progress-ledger-current.json
docs/editor-parity/progress-ledger-state.json
docs/gltf-scene-editor-parity-progress.md
docs/editor-parity/progress-ledger/bundles/sha256/...
```

The local mirrors are:

```text
docs/editor-parity/progress-ledger-local-state.json
docs/gltf-scene-editor-parity-local-progress.md
```

## Fixed Policy

- F42: `BLOCKED`
- R24: `BLOCKED`
- Metal: `Not Verified`
- release/distribution: `NOT PASS`

An engineering phase receipt cannot change these values or imply license,
Metal-runtime, release, or distribution approval.
