---
name: superluminal-perf
description: Diagnose CPU-side rendering performance from Superluminal .slp sampling traces with evidence-first hotspot, caller/callee, timeline, and source analysis; distinguish CPU-bound from GPU-bound, map hot frames to source, apply a minimal fix, re-profile, and verify with a normalized before/after comparison. Use for low frame rate, frame-time stutter, high CPU cost in the render loop, or requests to profile and optimize CPU-side rendering.
---

# Superluminal CPU Performance

Follow this closed loop:

`Trace -> Overview -> Problem class -> Hotspot -> Caller/Callee -> Frame timeline -> Source -> Hypothesis -> Patch -> Re-profile -> Compare -> Verify`

This is the CPU-side counterpart to the `renderdoc-debug` skill (GPU side). Use this skill when the cost is CPU time in the render loop; use RenderDoc when the defect is wrong pixels or GPU state.

## Non-Negotiable Rules

- Use only the typed `slp_*` tools. Never run `slp.py dump-json` through bash (it prints megabytes); other bounded `slp.py` subcommands through bash are acceptable but the typed tools are preferred.
- Call `slp_info` on every new trace before analyzing. Record duration, thread list, and how many modules resolved symbols.
- Never base a conclusion on a frame name ending in `?`. That mark means the module's PDB did not resolve and the name is an export-table guess. Say "unresolved" instead.
- Aggregate in the tool, judge in your head. Do not try to reason over raw per-event output; use `slp_hotspots` / `slp_callers` / `slp_callees` / `slp_timeline` / `slp_stacks`, which return ranked, bounded results.
- Never compare absolute ms across two traces of different duration. Use `slp_compare`, which normalizes by each trace's own total.
- Cite evidence (tool name, trace, thread, window, and the specific number) in every hotspot claim and hypothesis.
- Distinguish measurement overhead from application cost: Debug builds carry Vulkan validation layers and the profiler's own ETW stack-walk cost. Report how much was excluded, never silently.
- Make the smallest source patch that tests the active hypothesis. A fix is confirmed only after rebuild, re-profile, and `slp_compare` verification.

## Workflow

### 1. Establish the measurement (do not skip)

Call `slp_info` on the trace. From the output record:

- duration and sample interval
- thread list (find the Main thread's internal ThreadID; it is usually 0)
- "modules with resolved symbols: N / M" — if the main executable did not resolve, stop and tell the user to configure PDBs and re-capture; analyzing unresolved symbols produces fiction.

Then check the validation-layer share: call `slp_hotspots` once with `by="module"` and look at `VkLayer_*`. If validation is a large share, note it — Debug-build CPU cost is dominated by it and Release will look very different.

### 2. Classify the problem

Run `slp_timeline` with `frameFunc` set to the frame-loop function (for MGIF: `"drawFrame"` or `"RendererFacade::render"`), on the Main thread. Read the per-frame percentiles:

| Symptom | Judge by | Route |
|---|---|---|
| Uniformly slow frames | high p50 wall_ms | 3A |
| Periodic stutter | p99 >> p50 | 3B |
| CPU is not the bottleneck | cpu_ms << wall_ms | 3C |

Report p50/p95/p99/max, not the average. A steady 60fps average with periodic 200ms stalls is a different problem from a uniform 45fps.

### 3A. Uniformly slow — find the steady-state hotspot

1. `slp_hotspots` on the Main thread, `by="self"`, to find code actually executing.
2. `slp_hotspots` again with `by="total"` to see which subsystem owns the time.
3. For the top 2-3 self-time functions, run `slp_callees` to split "its own code" (`(self)`) from child calls:
   - `(self)` dominates -> optimize the function body.
   - a child dominates -> drill into that child with another `slp_callees`.
4. Run `slp_callers` on a hot leaf to confirm which call path feeds it (the same leaf can be hot from one path only).

### 3B. Periodic stutter — find the spike

1. From `slp_timeline` frame mode, take the slowest frames (`slowest_frames`) and their `start_ms`.
2. For the worst window, run `slp_hotspots` with `startMs`/`endMs` bracketing one stall. Hotspots inside a stall differ from the global average; do not explain a local spike with global hotspots.
3. Typical causes: resource create/destroy, shader/pipeline compilation, texture or buffer upload, descriptor-set rebuild, GC/allocation spikes, a sync wait (`vkQueueWaitIdle`, fence).

### 3C. CPU is not the bottleneck

If `cpu_ms << wall_ms` per frame, the Main thread spends its time waiting (GPU-bound or sync-bound). Say so explicitly; do not invent CPU optimizations. Recommend a GPU-side investigation (RenderDoc) or checking present/wait stacks via `slp_stacks`.

### 4. Map the hot function to source

1. Try `slp_source` with the function name. Note: functioncache coverage is sparse (the GUI only caches functions someone clicked), so a miss is normal.
2. On a miss, `grep` the repo for the function name and `read` the definition. Trace source paths are Windows-style (`G:\MGIF\...`) and map to `/mnt/g/MGIF/...` under WSL; `slp_source` reports the local path and whether it exists.
3. Read the hot function with the profiler data in mind. Look for, in rough order of how often they cause render-loop CPU cost:
   - per-frame heap allocation inside a loop (`std::vector` construction, `make_shared`, string building)
   - per-frame rebuild of something cacheable (descriptor sets, pipelines, command buffers, argument tables)
   - cache-unfriendly traversal (pointer chasing, AoS where SoA is needed)
   - redundant validation-layer work (a Debug-build artifact, not a real cost)
   - synchronous GPU waits on the CPU timeline

### 5. Form a testable hypothesis

Use this format for every hypothesis:

```text
Hypothesis: H<n>
Claim: <single causal statement, e.g. "setArgumentTable rebuilds the descriptor set every draw">
Evidence: <slp tool + trace + thread + window + the number, e.g. "slp_callees drawFrame Main: PassExecutor::execute = 98.9%">
Expected: <what should change if true, e.g. "caching it removes ~6% Main-thread self time">
Verification: <re-profile + slp_compare against this baseline>
Confidence: low | medium | high
Status: proposed
```

Do not merge unrelated causes into one hypothesis. Record contradictory evidence and reject a hypothesis that fails verification.

### 6. Patch and build

Change only the code needed to test the hypothesis. Record the changed files and the causal summary. Build with the repository's existing command. Stop and fix a task-related build failure before re-profiling.

### 7. Re-profile and verify

1. Have the user re-capture the same scene with the same build config into a new `.slp` (you cannot run Superluminal capture). State this explicitly rather than pretending.
2. `slp_info` on the new trace to confirm it is comparable (similar duration, same thread set, same validation setting).
3. `slp_compare` baseline vs candidate on the Main thread.
4. Set `H<n>.status = confirmed` only when the target function's share dropped as predicted AND no unexpected REGRESSED/NEW entry offsets the gain. Otherwise keep it proposed or mark it rejected and continue.

Interpret compare carefully:

- `IMPROVED` with a large negative `delta_pp` — the intended win.
- `REGRESSED` / `NEW` — an optimization that added a hot path; investigate before declaring success.
- A function whose share fell but whose absolute `cand_ms` rose means total CPU grew faster — report both.
- Changes under ~2% are within single-capture noise; recommend repeat captures before treating them as real.

## Investigation Record

Write both files under `investigations/<id>/`:

- `investigation.json` as the source of truth
- `report.md` as the human-readable report

Include: measurement conditions (trace, duration, thread, validation share, symbol resolution), problem class, Evidence list, hypotheses, source mapping, patch, candidate trace, compare result, and final verdict. Every confirmed claim must trace to Evidence.
