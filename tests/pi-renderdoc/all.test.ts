import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import test from "node:test";

import "./client.test.ts";
import "./inspect-draw.test.ts";
import "./investigation.test.ts";
import "./tool-policy.test.ts";
import "./extension.test.ts";

import {
  RdcProcessError,
  createRdcProcessRunner,
  runRdc,
  runRdcText,
} from "../../src/rdc/process.ts";

const fixturePath = fileURLToPath(new URL("./fixtures/rdc-process-fixture.mjs", import.meta.url));
const fixtureRunner = createRdcProcessRunner({
  executable: process.execPath,
  prefixArgs: [fixturePath],
});

test("process layer executes the installed rdc safely", async () => {
  const result = await runRdcText(["--version"]);
  assert.match(result.data, /^rdc, version 0\.6\.1$/);
  assert.equal(result.exitCode, 0);
});

test("process layer reports a structured error for an invalid rdc command", async () => {
  await assert.rejects(
    runRdc(["command-that-does-not-exist", "--json"]),
    (error: unknown) => {
      assert.ok(error instanceof RdcProcessError);
      assert.equal(error.kind, "exit");
      assert.notEqual(error.exitCode, 0);
      assert.match(`${error.stderr}\n${error.stdout}`, /No such command/i);
      return true;
    },
  );
});

test("process layer preserves arguments containing spaces", async () => {
  const spaced = "G:\\path with spaces\\捕获文件.rdc";
  const result = await fixtureRunner.runJson<{ args: string[] }>(["--capture", spaced]);
  assert.deepEqual(result.data.args, ["--capture", spaced]);
});

test("process layer keeps stderr separate from JSON stdout", async () => {
  const result = await fixtureRunner.runJson<{ args: string[] }>(["--stderr", "warning only"]);
  assert.deepEqual(result.data.args, ["--stderr", "warning only"]);
  assert.equal(result.stderr, "warning only");
});

test("process layer terminates on AbortSignal", async () => {
  const controller = new AbortController();
  const pending = fixtureRunner.runJson(["--sleep-ms", "5000"], { signal: controller.signal });
  setTimeout(() => controller.abort(), 50);
  await assert.rejects(pending, (error: unknown) => {
    assert.ok(error instanceof RdcProcessError);
    assert.equal(error.kind, "aborted");
    return true;
  });
});

test("process layer enforces its timeout", async () => {
  await assert.rejects(
    fixtureRunner.runJson(["--sleep-ms", "5000"], { timeoutMs: 50 }),
    (error: unknown) => {
      assert.ok(error instanceof RdcProcessError);
      assert.equal(error.kind, "timeout");
      return true;
    },
  );
});

test("process layer hard-codes shell false", async () => {
  const source = await readFile(new URL("../../src/rdc/process.ts", import.meta.url), "utf8");
  assert.match(source, /shell:\s*false/);
  assert.doesNotMatch(source, /exec\s*\(/);
});
