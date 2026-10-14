import assert from "node:assert/strict";
import { existsSync } from "node:fs";
import { readdirSync, statSync } from "node:fs";
import { join } from "node:path";
import test, { skip } from "node:test";

import { SlpClient } from "../../src/slp/client.ts";

/**
 * End-to-end test against the real slp.py and a real trace.
 *
 * Skipped unless both SLP_PY points at slp.py and a trace exists, so the suite
 * still passes on machines without the profiler data. When it does run, it
 * verifies the JSON contract between the client and slp.py — the shapes the
 * typed result interfaces assume.
 */

const SLP_PY = process.env.SLP_PY ?? "";
const CAPTURES = process.env.SLP_CAPTURES ?? "/mnt/f/Captures";

function findTrace(): string | undefined {
  if (process.env.SLP_TRACE && existsSync(process.env.SLP_TRACE)) return process.env.SLP_TRACE;
  if (!existsSync(CAPTURES)) return undefined;
  const slp = readdirSync(CAPTURES)
    .filter((n) => n.toLowerCase().endsWith(".slp"))
    .map((n) => join(CAPTURES, n))
    .filter((p) => statSync(p).isFile());
  return slp[0];
}

const canRun = SLP_PY !== "" && existsSync(SLP_PY) && findTrace() !== undefined;

if (!canRun) {
  skip("slp.py integration: set SLP_PY and SLP_CAPTURES (or SLP_TRACE) to enable", () => {});
} else {
  const trace = findTrace()!;
  const client = new SlpClient({ scriptPath: SLP_PY, capturesDir: CAPTURES, analysisTimeoutMs: 300_000 });

  test("integration: hotspots returns the documented JSON shape", async () => {
    const data = await client.hotspots(trace, { thread: "Main", top: 5 });
    assert.equal(typeof data.trace, "string");
    assert.equal(typeof data.sample_interval_ms, "number");
    assert.ok(data.sample_interval_ms > 0);
    assert.ok(Array.isArray(data.rows));
    assert.ok(data.rows.length <= 5);
    // The invariant that makes percentages trustworthy: self% sums to ~100 over all rows.
    assert.ok(data.totals.self_pct > 99 && data.totals.self_pct < 100.0001,
      `totals.self_pct out of range: ${data.totals.self_pct}`);
    const row = data.rows[0];
    if (row) {
      assert.equal(typeof row.self_ms, "number");
      assert.equal(typeof row.self_pct, "number");
      assert.equal(typeof row.resolved, "boolean");
    }
  });

  test("integration: info reports symbol resolution status", async () => {
    const info = await client.info(trace);
    assert.match(info.stats, /sample interval/i);
    assert.match(info.stats, /modules with resolved symbols/i);
    assert.match(info.threads, /thread/i);
  });

  test("integration: timeline frame mode yields percentiles and frame count", async () => {
    // Use a frame function likely present in a renderer; if absent, frame_count is 0
    // and the test still validates the shape rather than a specific value.
    const data = await client.timeline(trace, { thread: "Main", frameFunc: "Frame" });
    assert.equal(data.mode, "frame");
    const stats = data.stats as { frame_count?: number; cpu_ms?: Record<string, number> };
    assert.equal(typeof stats.frame_count, "number");
    if (stats.cpu_ms) {
      for (const key of ["p50", "p95", "p99", "max", "mean"]) {
        assert.equal(typeof stats.cpu_ms[key], "number");
      }
    }
  });

  test("integration: stacks aggregates distinct stacks with coverage", async () => {
    const data = await client.stacks(trace, { thread: "Main", top: 3 });
    assert.equal(typeof data.distinct_stacks, "number");
    assert.equal(typeof data.coverage_top_pct, "number");
    assert.ok(data.rows.length <= 3);
    // folded must be stripped when not requested (client post-processing)
    assert.equal("folded" in data, false);
  });
}
