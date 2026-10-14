import assert from "node:assert/strict";
import { fileURLToPath } from "node:url";
import test from "node:test";

import {
  createSlpProcessRunner,
  SLP_EXIT_BAD_ARGS,
  SLP_EXIT_ENV_MISSING,
  SlpProcessError,
} from "../../src/slp/process.ts";

const fixturePath = fileURLToPath(new URL("./fixtures/slp-fixture.mjs", import.meta.url));

function fixtureRunner() {
  return createSlpProcessRunner({
    executable: process.execPath,
    scriptPath: fixturePath,
    defaultTimeoutMs: 10_000,
  });
}

test("runner parses JSON stdout on success", async () => {
  const runner = fixtureRunner();
  const result = await runner.runJson<{ args: string[]; ok: boolean }>(["stats", "trace.slp", "--json"]);
  assert.equal(result.exitCode, 0);
  assert.equal(result.data.ok, true);
  assert.deepEqual(result.data.args.slice(0, 2), ["stats", "trace.slp"]);
});

test("exit code 3 maps to env-missing kind, not a generic exit", async () => {
  const runner = fixtureRunner();
  await assert.rejects(
    runner.runJson(["hotspots", "trace.slp", "--exit", String(SLP_EXIT_ENV_MISSING), "--stderr", "no zstd backend"]),
    (error: unknown) => {
      assert.ok(error instanceof SlpProcessError);
      assert.equal(error.kind, "env-missing");
      assert.equal(error.exitCode, SLP_EXIT_ENV_MISSING);
      assert.match(error.message, /no zstd backend/);
      return true;
    },
  );
});

test("exit code 2 maps to bad-args kind", async () => {
  const runner = fixtureRunner();
  await assert.rejects(
    runner.runJson(["timeline", "--exit", String(SLP_EXIT_BAD_ARGS), "--stderr", "conflicting flags"]),
    (error: unknown) => {
      assert.ok(error instanceof SlpProcessError);
      assert.equal(error.kind, "bad-args");
      return true;
    },
  );
});

test("non-zero, non-2/3 exit maps to generic exit kind", async () => {
  const runner = fixtureRunner();
  await assert.rejects(
    runner.runJson(["hotspots", "--exit", "1", "--stderr", "cannot open trace"]),
    (error: unknown) => {
      assert.ok(error instanceof SlpProcessError);
      assert.equal(error.kind, "exit");
      assert.equal(error.exitCode, 1);
      return true;
    },
  );
});

test("invalid JSON on stdout is reported as invalid-json", async () => {
  const runner = createSlpProcessRunner({
    executable: process.execPath,
    // -e prints non-JSON so runJson's parse fails
    scriptPath: "-e",
    defaultTimeoutMs: 10_000,
  });
  await assert.rejects(
    runner.runJson(["process.stdout.write('not json')"]),
    (error: unknown) => {
      assert.ok(error instanceof SlpProcessError);
      assert.equal(error.kind, "invalid-json");
      return true;
    },
  );
});

test("abort before start rejects without spawning", async () => {
  const runner = fixtureRunner();
  const controller = new AbortController();
  controller.abort();
  await assert.rejects(
    runner.runJson(["stats", "--sleep-ms", "5000"], { signal: controller.signal }),
    (error: unknown) => {
      assert.ok(error instanceof SlpProcessError);
      assert.equal(error.kind, "aborted");
      return true;
    },
  );
});

test("timeout kills a long-running process", async () => {
  const runner = createSlpProcessRunner({
    executable: process.execPath,
    scriptPath: fixturePath,
    defaultTimeoutMs: 200,
  });
  await assert.rejects(
    runner.runJson(["stats", "--sleep-ms", "5000"]),
    (error: unknown) => {
      assert.ok(error instanceof SlpProcessError);
      assert.equal(error.kind, "timeout");
      return true;
    },
  );
});

test("output limit stops a runaway payload", async () => {
  const runner = createSlpProcessRunner({
    executable: process.execPath,
    scriptPath: fixturePath,
    defaultTimeoutMs: 10_000,
    maxOutputBytes: 4 * 1024, // fixture --big 100 emits ~21KB
  });
  await assert.rejects(
    runner.runJson(["hotspots", "--big", "100"]),
    (error: unknown) => {
      assert.ok(error instanceof SlpProcessError);
      assert.equal(error.kind, "output-limit");
      return true;
    },
  );
});

test("scriptPath is required", () => {
  assert.throws(() => createSlpProcessRunner({ scriptPath: "" }), /slp\.py path is required/);
});
