import assert from "node:assert/strict";
import test from "node:test";

import type { ExtensionAPI } from "@earendil-works/pi-coding-agent";

import {
  registerSuperluminalExtension,
  SUPERLUMINAL_TOOL_NAMES,
} from "../../.pi/extensions/superluminal.ts";
import { SlpProcessError } from "../../src/slp/process.ts";
import { CONTEXT_FLOOD_SLP_REASON } from "../../src/slp/tool-policy.ts";
import { SLP_TOOL_NAMES } from "../../src/slp/types.ts";

interface TextResult {
  content: Array<{ type: string; text: string }>;
  details?: unknown;
}

interface RegisteredTool {
  name: string;
  execute(toolCallId: string, params: Record<string, unknown>, signal?: AbortSignal): Promise<TextResult>;
}

type Handler = (...args: unknown[]) => unknown;

function createFakePi() {
  const tools = new Map<string, RegisteredTool>();
  const handlers = new Map<string, Handler[]>();
  const pi = {
    registerTool(definition: RegisteredTool) {
      tools.set(definition.name, definition);
    },
    on(event: string, handler: Handler) {
      handlers.set(event, [...(handlers.get(event) ?? []), handler]);
    },
  } as unknown as ExtensionAPI;
  return { pi, tools, handlers };
}

/** Fake SlpClient covering the methods the tools call. */
function createFakeClient(overrides: Partial<Record<string, unknown>> = {}) {
  return {
    async listTraces() {
      return [
        { path: "/mnt/f/Captures/b.slp", name: "b.slp", sizeBytes: 211_000_000, modifiedMs: 2000, kind: "slp" },
        { path: "/mnt/f/Captures/a", name: "a", sizeBytes: 0, modifiedMs: 1000, kind: "folder" },
      ];
    },
    async info() {
      return { stats: "== session ==\n  duration: 0 - 100 ms", threads: "  thread 0 (os 1) Main" };
    },
    async hotspots() {
      return {
        trace: "b.slp", by: "self", thread: 0, window_ms: [0, 100], sample_interval_ms: 0.12,
        samples_used: 1000, samples_total: 1200, unresolved_frame_ratio: 0.06,
        totals: { distinct: 5, self_ms: 120, self_pct: 100, total_cpu_ms: 120 },
        rows: [{ function: "Demo.exe!drawFrame", module: "Demo.exe", self_ms: 50, self_pct: 41, total_ms: 90, total_pct: 75, self_samples: 420, resolved: true }],
      };
    },
    async relations() {
      return {
        trace: "b.slp", pattern: "drawFrame", direction: "callees", matched_samples: 420,
        matched_ms: 50, self_ms: 5, samples_used: 1000,
        targets: [{ function: "Demo.exe!drawFrame", samples: 420, ms: 50, pct: 100 }],
        rows: [{ neighbor: "Demo.exe!PassExecutor::execute", samples: 400, ms: 48, pct: 95 }],
      };
    },
    async timeline() {
      return {
        trace: "b.slp", mode: "frame", frame_func: "drawFrame", thread: 0, samples_used: 1000,
        stats: { frame_count: 60, implied_fps: 60, cpu_ms: { p50: 8, p95: 12, p99: 20, max: 40, mean: 9 }, wall_ms: { p50: 16, p95: 17, p99: 30, max: 50, mean: 16 }, pre_frame_samples: 0 },
        slowest_frames: [{ index: 3, start_ms: 50, samples: 300, cpu_ms: 40, wall_ms: 50 }],
        rows: [],
      };
    },
    async stacks() {
      return { trace: "b.slp", distinct_stacks: 10, samples_used: 1000, coverage_top_pct: 40, rows: [{ samples: 400, pct: 40, depth: 5, stack: ["a", "b", "c"] }], folded: ["a;b;c 400"] };
    },
    async compare() {
      return {
        baseline: { label: "a.slp", samples_used: 1000, duration_ms: 100, total_cpu_ms: 90 },
        candidate: { label: "b.slp", samples_used: 2000, duration_ms: 200, total_cpu_ms: 150 },
        thread: 0,
        improved: [{ function: "Demo.exe!old", base_pct: 10, cand_pct: 4, delta_pp: -6, ratio: 0.4 }],
        regressed: [], new: [], gone: [],
      };
    },
    async stack() {
      return "id 5  depth=3\n  0  addr  Demo.exe!leaf\n";
    },
    async source() {
      return "function: Demo.exe!drawFrame\n  source: G:\\MGIF\\render\\x.cpp\n";
    },
    ...overrides,
  } as never;
}

function parseResult(result: TextResult): Record<string, unknown> {
  const text = result.content[0]?.text;
  assert.ok(text);
  return JSON.parse(text) as Record<string, unknown>;
}

test("extension registers exactly the declared slp tool surface", () => {
  const { pi, tools } = createFakePi();
  registerSuperluminalExtension(pi, createFakeClient());
  assert.deepEqual([...tools.keys()], [...SLP_TOOL_NAMES]);
  assert.deepEqual([...SUPERLUMINAL_TOOL_NAMES], [...SLP_TOOL_NAMES]);
});

test("slp_info returns bounded text (not JSON) with stats and threads", async () => {
  const { pi, tools } = createFakePi();
  registerSuperluminalExtension(pi, createFakeClient());
  const result = await tools.get("slp_info")!.execute("info", { trace: "/mnt/f/Captures/b.slp" });
  const text = result.content[0]!.text;
  assert.match(text, /== b\.slp ==/);
  assert.match(text, /duration: 0 - 100 ms/);
  assert.match(text, /thread 0 \(os 1\) Main/);
});

test("slp_hotspots returns ranked rows and the unresolved-symbol warning input", async () => {
  const { pi, tools } = createFakePi();
  registerSuperluminalExtension(pi, createFakeClient());
  const data = parseResult(await tools.get("slp_hotspots")!.execute("h", { trace: "b.slp", thread: "Main" }));
  const rows = data.rows as Array<Record<string, unknown>>;
  assert.equal(rows.length, 1);
  assert.equal(rows[0]!.function, "Demo.exe!drawFrame");
  assert.equal(data.unresolved_frame_ratio, 0.06);
});

test("slp_stacks returns aggregated distinct stacks", async () => {
  const { pi, tools } = createFakePi();
  registerSuperluminalExtension(pi, createFakeClient());
  const data = parseResult(await tools.get("slp_stacks")!.execute("s", { trace: "b.slp" }));
  assert.ok(Array.isArray(data.rows));
  assert.equal(data.distinct_stacks, 10);
  assert.equal(data.coverage_top_pct, 40);
});

test("slp_compare surfaces baseline/candidate totals for comparability checks", async () => {
  const { pi, tools } = createFakePi();
  registerSuperluminalExtension(pi, createFakeClient());
  const data = parseResult(await tools.get("slp_compare")!.execute("c", { baseline: "a.slp", candidate: "b.slp" }));
  assert.equal((data.baseline as Record<string, unknown>).label, "a.slp");
  assert.equal((data.improved as unknown[]).length, 1);
});

test("bash preflight blocks slp.py dump-json but allows bounded subcommands", async () => {
  const { pi, handlers } = createFakePi();
  registerSuperluminalExtension(pi, createFakeClient());
  const preflight = handlers.get("tool_call")?.[0];
  assert.ok(preflight);

  const blocked = await preflight(
    { toolName: "bash", input: { command: "python3 slp.py dump-json b.slp functioncache.dat" } },
    {},
  ) as { block: boolean; reason: string };
  assert.deepEqual(blocked, { block: true, reason: CONTEXT_FLOOD_SLP_REASON });

  const allowed = await preflight({ toolName: "bash", input: { command: "python3 slp.py hotspots b.slp" } }, {});
  assert.equal(allowed, undefined);

  // Non-bash tools are never intercepted by this handler.
  const other = await preflight({ toolName: "read", input: { path: "x" } }, {});
  assert.equal(other, undefined);
});

test("env-missing errors are reworded to 'do not retry, fix environment'", async () => {
  const { pi, tools } = createFakePi();
  const failingClient = createFakeClient({
    async hotspots() {
      throw new SlpProcessError({
        message: "slp exited with code 3: no zstd backend",
        kind: "env-missing",
        args: ["hotspots"],
        exitCode: 3,
        stderr: "no zstd backend available. Fix it one of these ways: ...",
      });
    },
  });
  registerSuperluminalExtension(pi, failingClient);
  await assert.rejects(
    tools.get("slp_hotspots")!.execute("h", { trace: "b.slp" }),
    (error: unknown) => {
      assert.ok(error instanceof Error);
      assert.match(error.message, /Do NOT retry/);
      assert.match(error.message, /no zstd backend/);
      return true;
    },
  );
});

test("a generic error passes through unchanged", async () => {
  const { pi, tools } = createFakePi();
  const failingClient = createFakeClient({
    async info() {
      throw new Error("cannot open trace");
    },
  });
  registerSuperluminalExtension(pi, failingClient);
  await assert.rejects(
    tools.get("slp_info")!.execute("i", { trace: "b.slp" }),
    (error: unknown) => {
      assert.ok(error instanceof Error);
      assert.equal(error.message, "cannot open trace");
      return true;
    },
  );
});

test("lazy client construction: extension loads without SLP_PY until a tool runs", () => {
  const { pi, tools } = createFakePi();
  // No client injected and SLP_PY unset -> registration itself must not throw.
  const savedSlpPy = process.env.SLP_PY;
  delete process.env.SLP_PY;
  try {
    registerSuperluminalExtension(pi);
    assert.equal(tools.size, SLP_TOOL_NAMES.length);
  } finally {
    if (savedSlpPy !== undefined) process.env.SLP_PY = savedSlpPy;
  }
});
