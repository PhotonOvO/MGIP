import type { ExtensionAPI } from "@earendil-works/pi-coding-agent";
import { Type } from "typebox";

import {
  isEnvMissingError,
  SlpClient,
  SlpProcessError,
  traceLabel,
} from "../../src/slp/client.ts";
import {
  boundedToolResult,
  CONTEXT_FLOOD_SLP_REASON,
  isContextFloodSlpCommand,
  MAX_SLP_TOOL_OUTPUT_BYTES,
} from "../../src/slp/tool-policy.ts";
import { SLP_TOOL_NAMES } from "../../src/slp/types.ts";

export { SLP_TOOL_NAMES as SUPERLUMINAL_TOOL_NAMES };

const THREAD_DESC =
  "Thread filter: internal ThreadID, OS thread id, or thread name (e.g. \"Main\"). Omit for all threads.";

interface TextResult {
  content: [{ type: "text"; text: string }];
  details: { bytes: number; truncated: boolean; originalBytes: number };
}

/** Bound a plain-text tool result (info/stack/source) the same way JSON results are bounded. */
function boundedTextResult(text: string): TextResult {
  const originalBytes = Buffer.byteLength(text, "utf8");
  const truncated = originalBytes > MAX_SLP_TOOL_OUTPUT_BYTES;
  const body = truncated
    ? `${text.slice(0, MAX_SLP_TOOL_OUTPUT_BYTES)}\n\n[truncated ${originalBytes - MAX_SLP_TOOL_OUTPUT_BYTES} bytes; narrow the query]`
    : text;
  return {
    content: [{ type: "text", text: body }],
    details: { bytes: Buffer.byteLength(body, "utf8"), truncated, originalBytes },
  };
}

/**
 * Convert a runner error into a model-facing message. The key case is
 * env-missing (exit 3): slp.py already printed install steps to stderr, which
 * the runner folded into error.message. Prepend an explicit "do not retry"
 * instruction so the model fixes the environment instead of re-running.
 */
function toModelError(error: unknown): Error {
  if (isEnvMissingError(error)) {
    const detail = error instanceof SlpProcessError ? error.stderr.trim() || error.message : String(error);
    return new Error(
      "This analysis needs a zstd backend that is missing on this machine. " +
      "Do NOT retry the same command. Fix the environment first:\n" + detail,
    );
  }
  return error instanceof Error ? error : new Error(String(error));
}

/**
 * Register the CPU-side Superluminal profiler tools.
 *
 * Parallel to registerRenderDocExtension (GPU side). The client is built lazily
 * so the extension always loads even when SLP_PY is unset; the requirement is
 * surfaced only when a tool actually runs.
 */
export function registerSuperluminalExtension(
  pi: ExtensionAPI,
  client?: SlpClient,
): void {
  let cached = client;
  const getClient = (): SlpClient => {
    cached ??= new SlpClient();
    return cached;
  };

  // Guard the one unbounded subcommand. Every other slp.py command is bounded by
  // its own --top/--limit and stays usable through bash.
  pi.on("tool_call", (event) => {
    if (event.toolName !== "bash") return undefined;
    const command = (event.input as { command?: unknown }).command;
    if (typeof command === "string" && isContextFloodSlpCommand(command)) {
      return { block: true, reason: CONTEXT_FLOOD_SLP_REASON };
    }
    return undefined;
  });

  pi.registerTool({
    name: "slp_traces",
    label: "List Superluminal Traces",
    description: "List available Superluminal traces (.slp files and GUI session folders) under the captures directory, newest first. Use the returned path as the trace argument to other slp_* tools.",
    promptSnippet: "List available CPU profiler traces",
    promptGuidelines: ["Call slp_traces first when the user did not give an explicit trace path."],
    parameters: Type.Object({}, { additionalProperties: false }),
    async execute(_id, _params, signal) {
      try {
        const traces = await getClient().listTraces(signal);
        return boundedToolResult({
          count: traces.length,
          capturesDirConfigured: traces.length > 0 || process.env.SLP_CAPTURES !== undefined,
          traces: traces.map((t) => ({
            name: t.name, path: t.path, kind: t.kind,
            sizeMb: t.kind === "slp" ? Math.round((t.sizeBytes / 1e6) * 10) / 10 : undefined,
            modified: new Date(t.modifiedMs).toISOString(),
          })),
        });
      } catch (error) {
        throw toModelError(error);
      }
    },
  });

  pi.registerTool({
    name: "slp_info",
    label: "Superluminal Trace Info",
    description: "Trace overview: duration, sample interval, thread list, module/function counts, callstack node count, event counts, and how many modules resolved PDB symbols. Call this first for any new trace.",
    promptSnippet: "Trace overview (duration, threads, symbol resolution)",
    promptGuidelines: [
      "Always call slp_info before analyzing an unfamiliar trace.",
      "If the main executable's symbols did not resolve (see 'modules with resolved symbols'), stack frame names are untrustworthy — say so and stop rather than analyzing garbage.",
      "Note the trace duration: never compare absolute ms across traces of different length; use slp_compare.",
    ],
    parameters: Type.Object({
      trace: Type.String({ minLength: 1, description: "Trace path or name (see slp_traces)." }),
    }, { additionalProperties: false }),
    async execute(_id, params, signal) {
      try {
        const info = await getClient().info(params.trace, signal);
        return boundedTextResult(`== ${traceLabel(params.trace)} ==\n\n${info.stats}\n\n${info.threads}`);
      } catch (error) {
        throw toModelError(error);
      }
    },
  });

  const windowProps = {
    thread: Type.Optional(Type.String({ description: THREAD_DESC })),
    startMs: Type.Optional(Type.Number({ description: "Window start in ms." })),
    endMs: Type.Optional(Type.Number({ description: "Window end in ms." })),
    excludeValidation: Type.Optional(Type.Boolean({ description: "Exclude VkLayer_* validation-layer samples (Debug builds). Default false." })),
  } as const;

  pi.registerTool({
    name: "slp_hotspots",
    label: "CPU Hotspots",
    description: "Aggregate CPU time by function, ranked. by=self shows leaf-frame cost (code actually running); by=total shows in-stack cost (which subsystem owns time); by=module and by=thread group coarsely. Percentages are of the analyzed window. Output is bounded and reports what was excluded.",
    promptSnippet: "Top CPU-consuming functions (self/total time)",
    promptGuidelines: [
      "Use thread=\"Main\" for frame-loop analysis; omit thread for whole-process CPU.",
      "Debug builds include Vulkan validation layers. Check their share first; pass excludeValidation=true to see real application cost, but report how much was excluded.",
      "Rank by self to find code to optimize; rank by total to find the owning subsystem, then drill in with slp_callees.",
      "A frame name with a trailing ? is from an unresolved module (export-table guess). Never base a conclusion on a ? name.",
    ],
    parameters: Type.Object({
      trace: Type.String({ minLength: 1 }),
      ...windowProps,
      by: Type.Optional(Type.Union([
        Type.Literal("self"), Type.Literal("total"), Type.Literal("module"), Type.Literal("thread"),
      ], { description: "Ranking metric. Default self." })),
      top: Type.Optional(Type.Integer({ minimum: 1, maximum: 100, description: "Rows to return. Default 20." })),
      minPct: Type.Optional(Type.Number({ description: "Hide rows below this percent." })),
      moduleFilter: Type.Optional(Type.String({ description: "Only modules whose name contains this substring, e.g. \"Demo.exe\"." })),
    }, { additionalProperties: false }),
    async execute(_id, params, signal) {
      try {
        const data = await getClient().hotspots(params.trace, {
          thread: params.thread, start: params.startMs, end: params.endMs,
          excludeValidation: params.excludeValidation, by: params.by,
          top: params.top, minPct: params.minPct, moduleFilter: params.moduleFilter,
        }, signal);
        return boundedToolResult(data);
      } catch (error) {
        throw toModelError(error);
      }
    },
  });

  for (const direction of ["callers", "callees"] as const) {
    const isCallers = direction === "callers";
    pi.registerTool({
      name: isCallers ? "slp_callers" : "slp_callees",
      label: isCallers ? "Function Callers" : "Function Callees",
      description: isCallers
        ? "For samples whose stack contains a function matching the pattern, aggregate the calling frame (who calls it, by CPU samples). Pattern is a case-insensitive substring or glob."
        : "For samples whose stack contains a function matching the pattern, aggregate the called frame (where its time goes), including a '(self)' row for samples where it is the leaf. Pattern is a case-insensitive substring or glob.",
      promptSnippet: isCallers ? "Who calls a hot function" : "Where a hot function's time goes",
      promptGuidelines: isCallers
        ? ["Use slp_callers to find which call path dominates a hot leaf function."]
        : [
          "Use slp_callees to see whether a hot function's time is its own code ('(self)') or a child call.",
          "If '(self)' dominates, optimize the function body; if a child dominates, drill into that child.",
        ],
      parameters: Type.Object({
        trace: Type.String({ minLength: 1 }),
        function: Type.String({ minLength: 1, description: "Function name pattern (substring or glob), e.g. \"drawFrame\" or \"*setArgumentTable*\"." }),
        ...windowProps,
        top: Type.Optional(Type.Integer({ minimum: 1, maximum: 100, description: "Rows to return. Default 15." })),
      }, { additionalProperties: false }),
      async execute(_id, params, signal) {
        try {
          const data = await getClient().relations(params.trace, direction, {
            function: params.function, thread: params.thread, start: params.startMs,
            end: params.endMs, excludeValidation: params.excludeValidation, top: params.top,
          }, signal);
          return boundedToolResult(data);
        } catch (error) {
          throw toModelError(error);
        }
      },
    });
  }

  pi.registerTool({
    name: "slp_timeline",
    label: "CPU Timeline",
    description: "CPU samples over time. With frameFunc set, buckets are real frames detected by that function's False->True stack transitions and per-frame cpu_ms/wall_ms percentiles (p50/p95/p99/max) are reported. Without it, samples fall into fixed time buckets to expose stalls.",
    promptSnippet: "CPU usage over time / per-frame timing",
    promptGuidelines: [
      "Run slp_timeline with frameFunc (e.g. \"drawFrame\") before slp_hotspots when the complaint is stutter or frame drops: find the bad frames first, then analyze hotspots inside their time window.",
      "Report p50/p95/p99/max frame time, not just the average. A steady average with periodic spikes is a different problem from uniform slowness.",
      "If cpu_ms ~= wall_ms per frame, the thread is CPU-bound. If wall >> cpu, it is waiting (GPU or sync) and CPU optimization will not help.",
    ],
    parameters: Type.Object({
      trace: Type.String({ minLength: 1 }),
      ...windowProps,
      frameFunc: Type.Optional(Type.String({ description: "Function marking frame boundaries, e.g. \"drawFrame\". Mutually exclusive with bucketMs." })),
      bucketMs: Type.Optional(Type.Number({ description: "Fixed bucket size in ms (default 16.667). Ignored when frameFunc is set." })),
      top: Type.Optional(Type.Integer({ minimum: 1, maximum: 200, description: "Buckets/frames to show. Default 20." })),
      topFuncs: Type.Optional(Type.Integer({ minimum: 1, maximum: 10, description: "Top leaf functions per bucket/frame. Default 3." })),
    }, { additionalProperties: false }),
    async execute(_id, params, signal) {
      try {
        const data = await getClient().timeline(params.trace, {
          thread: params.thread, start: params.startMs, end: params.endMs,
          excludeValidation: params.excludeValidation, frameFunc: params.frameFunc,
          bucket: params.bucketMs, top: params.top, topFuncs: params.topFuncs,
        }, signal);
        return boundedToolResult(data);
      } catch (error) {
        throw toModelError(error);
      }
    },
  });

  pi.registerTool({
    name: "slp_stacks",
    label: "Aggregate Call Stacks",
    description: "Aggregate distinct call stacks by sample count (a textual flame graph). Each row is one unique stack with its sample share; coverage tells what fraction the top N explain. Use to see the shape of hot call paths without a browser.",
    promptSnippet: "Distinct call stacks ranked by samples (text flame graph)",
    promptGuidelines: [
      "Check coverage_top_pct: if the top stacks cover only a small share, hotspots are diffuse and per-function slp_hotspots is the better lens.",
    ],
    parameters: Type.Object({
      trace: Type.String({ minLength: 1 }),
      ...windowProps,
      top: Type.Optional(Type.Integer({ minimum: 1, maximum: 100, description: "Distinct stacks to return. Default 20." })),
      minSamples: Type.Optional(Type.Integer({ minimum: 1, description: "Ignore stacks with fewer samples. Default 1." })),
    }, { additionalProperties: false }),
    async execute(_id, params, signal) {
      try {
        const data = await getClient().stacks(params.trace, {
          thread: params.thread, start: params.startMs, end: params.endMs,
          excludeValidation: params.excludeValidation, top: params.top, minSamples: params.minSamples,
        }, signal);
        return boundedToolResult(data);
      } catch (error) {
        throw toModelError(error);
      }
    },
  });

  pi.registerTool({
    name: "slp_compare",
    label: "Compare Traces",
    description: "Diff two traces by self-time share (normalized by each trace's own total, so different durations are comparable). Reports IMPROVED / REGRESSED / NEW / GONE functions plus per-trace totals. This is the primary before/after optimization tool.",
    promptSnippet: "Diff two traces: what got faster/slower",
    promptGuidelines: [
      "Use slp_compare for before/after optimization analysis. Never compare absolute ms across traces yourself.",
      "First verify both traces are comparable (same scene, same build config, similar frame count via slp_timeline). A 20s and a 74s trace of different scenes are not comparable — say so.",
      "Report changes in percentage points (delta_pp) of total CPU, alongside the ratio.",
      "Call out NEW entries: an optimization that introduces a new hot path is a regression risk.",
      "A function whose share dropped but whose absolute ms rose means total CPU grew faster — report both.",
    ],
    parameters: Type.Object({
      baseline: Type.String({ minLength: 1, description: "Baseline (before) trace path or name." }),
      candidate: Type.String({ minLength: 1, description: "Candidate (after) trace path or name." }),
      thread: Type.Optional(Type.String({ description: THREAD_DESC })),
      excludeValidation: Type.Optional(Type.Boolean()),
      minAbsMs: Type.Optional(Type.Number({ description: "Ignore changes smaller than this many ms." })),
      top: Type.Optional(Type.Integer({ minimum: 1, maximum: 100, description: "Rows per category. Default 15." })),
    }, { additionalProperties: false }),
    async execute(_id, params, signal) {
      try {
        const data = await getClient().compare(params.baseline, params.candidate, {
          thread: params.thread, excludeValidation: params.excludeValidation,
          minAbsMs: params.minAbsMs, top: params.top,
        }, signal);
        return boundedToolResult(data);
      } catch (error) {
        throw toModelError(error);
      }
    },
  });

  pi.registerTool({
    name: "slp_stack",
    label: "Symbolize Callstack IDs",
    description: "Symbolize specific callstack node IDs into full frame lists (leaf first). Frames from unresolved modules carry a trailing ? and must not be trusted. Get IDs from other tools' details, not by guessing.",
    promptSnippet: "Symbolize specific callstack node IDs",
    promptGuidelines: [
      "A trailing ? means that module's PDB was not resolved; the name is an export-table guess. Never conclude from a ? frame.",
    ],
    parameters: Type.Object({
      trace: Type.String({ minLength: 1 }),
      ids: Type.Array(Type.Integer({ minimum: 0 }), { minItems: 1, maxItems: 50, description: "Callstack node IDs (max 50)." }),
    }, { additionalProperties: false }),
    async execute(_id, params, signal) {
      try {
        const text = await getClient().stack(params.trace, params.ids, signal);
        return boundedTextResult(text);
      } catch (error) {
        throw toModelError(error);
      }
    },
  });

  pi.registerTool({
    name: "slp_source",
    label: "Function to Source Line",
    description: "Map a function to its source file and hottest lines using the trace's functioncache. Coverage is sparse (the cache only holds functions inspected in the Superluminal GUI), so a miss is normal; on a miss, grep the repo for the function name instead.",
    promptSnippet: "Map a hot function to source file and hot lines",
    promptGuidelines: [
      "functioncache coverage is typically tiny (tens of entries vs thousands of functions). If slp_source misses, grep the repo for the function name — do not assume there is no source.",
      "Trace source paths are Windows-style (G:\\...); they map to /mnt/g/... under WSL. The tool reports the local path and whether it exists.",
    ],
    parameters: Type.Object({
      trace: Type.String({ minLength: 1 }),
      function: Type.String({ minLength: 1, description: "Function name pattern (substring or glob)." }),
      limit: Type.Optional(Type.Integer({ minimum: 1, maximum: 50, description: "Max matching functions. Default 5." })),
      hotLines: Type.Optional(Type.Integer({ minimum: 1, maximum: 50, description: "Max hot lines per function. Default 5." })),
    }, { additionalProperties: false }),
    async execute(_id, params, signal) {
      try {
        const text = await getClient().source(params.trace, params.function, params.limit, params.hotLines, signal);
        return boundedTextResult(text);
      } catch (error) {
        throw toModelError(error);
      }
    },
  });
}

export default function superluminalExtension(pi: ExtensionAPI): void {
  registerSuperluminalExtension(pi);
}
