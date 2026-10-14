import assert from "node:assert/strict";
import test from "node:test";

import { SlpClient } from "../../src/slp/client.ts";
import type { SlpProcessResult, SlpProcessRunner, SlpRunOptions } from "../../src/slp/process.ts";

/**
 * Fake runner that records the argv slp.py would receive and returns canned
 * results. This tests the client's argument construction and post-processing
 * (e.g. folded-stack stripping) without invoking Python.
 */
function fakeRunner(respond: (args: readonly string[]) => unknown) {
  const calls: string[][] = [];
  const runner: SlpProcessRunner = {
    async runJson<T>(args: readonly string[]): Promise<SlpProcessResult<T>> {
      calls.push([...args]);
      return { data: respond(args) as T, stderr: "", exitCode: 0 };
    },
    async runText(args: readonly string[]): Promise<SlpProcessResult<string>> {
      calls.push([...args]);
      return { data: String(respond(args)), stderr: "", exitCode: 0 };
    },
  };
  return { runner, calls };
}

function clientWith(runner: SlpProcessRunner): SlpClient {
  return new SlpClient({ scriptPath: "/fake/slp.py", runner });
}

test("hotspots passes filters and always requests --json --quiet", async () => {
  const { runner, calls } = fakeRunner(() => ({ rows: [], totals: {} }));
  await clientWith(runner).hotspots("/t.slp", { thread: "Main", by: "total", top: 5, excludeValidation: true });
  const argv = calls[0]!;
  assert.equal(argv[0], "hotspots");
  assert.equal(argv[1], "/t.slp");
  assert.ok(argv.includes("--thread") && argv[argv.indexOf("--thread") + 1] === "Main");
  assert.ok(argv.includes("--by") && argv[argv.indexOf("--by") + 1] === "total");
  assert.ok(argv.includes("--exclude-validation"));
  assert.ok(argv.includes("--json") && argv.includes("--quiet"));
});

test("hotspots clamps top into [1,100]", async () => {
  const { runner, calls } = fakeRunner(() => ({ rows: [] }));
  await clientWith(runner).hotspots("/t.slp", { top: 9999 });
  const argv = calls[0]!;
  assert.equal(argv[argv.indexOf("--top") + 1], "100");
});

test("timeline rejects frameFunc + bucket together before invoking python", async () => {
  const { runner } = fakeRunner(() => ({}));
  await assert.rejects(
    clientWith(runner).timeline("/t.slp", { frameFunc: "drawFrame", bucket: 10 }),
    /mutually exclusive/,
  );
});

test("stacks strips folded output unless folded is explicitly requested", async () => {
  const payload = () => ({ distinct_stacks: 3, rows: [{ samples: 1 }], folded: ["a;b 1"] });

  const strip = fakeRunner(payload);
  const withoutFolded = await clientWith(strip.runner).stacks("/t.slp", {});
  assert.equal("folded" in withoutFolded, false);

  const keep = fakeRunner(payload);
  const withFolded = await clientWith(keep.runner).stacks("/t.slp", { folded: true });
  assert.ok(Array.isArray((withFolded as { folded?: unknown[] }).folded));
});

test("compare passes both trace paths and normalized compare flags", async () => {
  const { runner, calls } = fakeRunner(() => ({ improved: [], regressed: [], new: [], gone: [] }));
  await clientWith(runner).compare("/base.slp", "/cand.slp", { thread: "Main", top: 8, minAbsMs: 5 });
  const argv = calls[0]!;
  assert.deepEqual(argv.slice(0, 3), ["compare", "/base.slp", "/cand.slp"]);
  assert.ok(argv.includes("--min-abs-ms") && argv[argv.indexOf("--min-abs-ms") + 1] === "5");
});

test("stack rejects more than 50 ids and zero ids", async () => {
  const { runner } = fakeRunner(() => "text");
  const client = clientWith(runner);
  await assert.rejects(client.stack("/t.slp", []), /at least one/);
  await assert.rejects(client.stack("/t.slp", Array.from({ length: 51 }, (_, i) => i)), /at most 50/);
});

test("relations requires a non-empty function pattern", async () => {
  const { runner } = fakeRunner(() => ({ rows: [] }));
  await assert.rejects(clientWith(runner).relations("/t.slp", "callees", { function: "  " }), /cannot be empty/);
});

test("client construction requires a script path", () => {
  assert.throws(() => new SlpClient({ scriptPath: "" }), /SLP_PY or pass scriptPath/);
});
