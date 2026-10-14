---
name: renderdoc-debug
description: Diagnose rendering defects from RenderDoc .rdc captures with evidence-first pixel history, draw, pipeline, binding, shader, and constant inspection; map GPU evidence to source, apply a minimal fix, recapture, and verify it. Use for black or wrong pixels, missing geometry, bad resources, shader output errors, pipeline-state defects, or requests to investigate and fix a GPU frame capture.
---

# RenderDoc Debug

Follow this closed loop:

`Symptom -> Pixel -> Draw -> Pipeline -> Bindings -> Shader -> Constants -> Source -> Hypothesis -> Patch -> Recapture -> Verify`

## Non-Negotiable Rules

- Use only the typed `rdc_*` tools for RenderDoc operations. Never invoke `rdc` or `renderdoccmd` through `bash`.
- Collect at least one RenderDoc Evidence item before proposing any root cause.
- Start with `rdc_pixel_history` when the user supplies a failing pixel. Do not infer the responsible draw from its name alone.
- Cite Evidence IDs in every hypothesis and important root-cause claim.
- Inspect GPU state before searching source. Use GPU names and values to guide source search.
- Make only the smallest patch that tests the active hypothesis. Avoid unrelated refactors.
- Treat build failure, replay failure, and assertion failure as different outcomes. Never convert a tool/replay error into a passing assertion.
- Mark a hypothesis `confirmed` only after a different, post-patch capture passes RenderDoc verification.
- Close the active capture with `rdc_close` when the investigation ends or before opening another capture.

## Workflow

### 1. Establish the Symptom

Record the expected and actual image state, capture path, API, frame, and failing pixel if available. Open the capture with `rdc_open`, then call `rdc_frame_overview` with a small limit and a pass/name filter when possible.

### 2. Identify the Responsible Event

Call `rdc_pixel_history` for the failing coordinate. Examine modifications, passed/rejected state, shader output, post-modification color, depth, and failure flags. Select the event that actually produced or rejected the relevant fragment.

Create Evidence such as:

- `E1` pixel history identifying the responsible EID and result.
- `E2` draw identity and stable marker.

If no pixel is available, use the filtered frame overview to select a candidate draw, then state that the evidence is weaker.

### 3. Inspect the Draw

Call `rdc_inspect_draw` for the responsible EID. Check all returned domains:

- color and depth targets
- viewport and scissor
- primitive topology
- rasterizer, depth/stencil, and blend state
- reflected bindings and resolved descriptors/resources
- active vertex, pixel, or compute shaders
- relevant constant-buffer or push-constant values

Record separate Evidence IDs for facts that support or contradict a hypothesis. Preserve exact GPU values and resource/shader names in Evidence data.

### 4. Form a Testable Hypothesis

Use this format for every hypothesis:

```text
Hypothesis: H<n>
Claim: <single causal statement>
Evidence IDs: E<n>, E<n>...
Expected behavior: <what should change if the claim is correct>
Verification method: <new capture query and assertion>
Confidence: low | medium | high
Status: proposed
```

Do not combine unrelated causes into one hypothesis. Record contradictory evidence and reject a hypothesis when it fails verification.

### 5. Map Evidence to Source

Search source using values obtained from GPU evidence:

- shader or entry-point name
- debug marker or render-pass name
- descriptor set, binding, or resource name
- constant-buffer and variable name
- observed pipeline state

Record source location and CPU-side upload/setup logic as `source-code` Evidence. Do not treat source inspection alone as proof of runtime state.

### 6. Patch and Build

Modify only the code needed to test the hypothesis. Record the changed files and causal summary. Build with the repository's existing command and record the result as build Evidence. Stop and repair a task-related build failure before capture.

### 7. Recapture and Verify

Produce a new capture with `rdc_capture` when available. Never overwrite the original capture. Then:

1. Open the new capture with `rdc_open`.
2. Repeat `rdc_pixel_history` at the same coordinate.
3. Call `rdc_diff` for original versus fixed captures.
4. Call `rdc_assert_pixel` with the expected RGBA and explicit tolerance.
5. Record the new Evidence and assertion exit semantics.

Set `H<n>.status = confirmed` only when the assertion passes and the new evidence matches the hypothesis's expected behavior. Otherwise keep it proposed or mark it rejected and continue diagnosing.

## Investigation Record

Write both files under `investigations/<id>/`:

- `investigation.json` as the source of truth
- `report.md` as the human-readable report

Include symptom, original capture, target pixel, Evidence list, hypotheses, source mapping, patch, fixed capture, diff, assertion, and final result. Ensure every confirmed claim traces to Evidence IDs.
