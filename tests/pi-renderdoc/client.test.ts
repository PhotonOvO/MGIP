import assert from "node:assert/strict";
import { access, mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { resolve } from "node:path";
import test from "node:test";

import { RdcClient } from "../../src/rdc/client.ts";
import {
  RdcProcessError,
  type RdcProcessResult,
  type RdcProcessRunner,
  type RdcRunOptions,
} from "../../src/rdc/process.ts";

interface RecordedCall {
  kind: "json" | "text";
  args: readonly string[];
  options?: RdcRunOptions;
}

class FakeRunner implements RdcProcessRunner {
  readonly calls: RecordedCall[] = [];
  readonly jsonResults: Array<unknown | Error> = [];
  readonly textResults: Array<string | Error> = [];

  async runJson<T>(
    args: readonly string[],
    options?: RdcRunOptions,
  ): Promise<RdcProcessResult<T>> {
    this.calls.push({ kind: "json", args: [...args], options });
    const value = this.jsonResults.shift() ?? {};
    if (value instanceof Error) throw value;
    return { data: value as T, stderr: "", exitCode: 0 };
  }

  async runText(
    args: readonly string[],
    options?: RdcRunOptions,
  ): Promise<RdcProcessResult<string>> {
    this.calls.push({ kind: "text", args: [...args], options });
    const value = this.textResults.shift() ?? "ok";
    if (value instanceof Error) throw value;
    return { data: value, stderr: "", exitCode: 0 };
  }
}

const existingFile = "TASK_PLAN_PI_RENDERDOC_V0_1.md";

async function openFakeClient(runner: FakeRunner): Promise<RdcClient> {
  runner.jsonResults.push({ API: "D3D11", Events: 42 });
  const client = new RdcClient({ runner, cwd: process.cwd(), sessionName: "unit-test" });
  await client.openCapture(existingFile);
  return client;
}

test("client requires an open capture for session queries", async () => {
  const runner = new FakeRunner();
  const client = new RdcClient({ runner });
  await assert.rejects(client.getInfo(), /No RenderDoc capture is open/);
  assert.equal(runner.calls.length, 0);
});

test("client opens, probes info, and closes a named session", async () => {
  const runner = new FakeRunner();
  const client = await openFakeClient(runner);
  const state = client.getSessionState();
  assert.equal(state.capturePath, resolve(existingFile));
  assert.ok(state.openedAt);
  assert.deepEqual(runner.calls[0]?.args, ["--session", "unit-test", "open", resolve(existingFile)]);
  assert.deepEqual(runner.calls[1]?.args, ["--session", "unit-test", "info", "--json"]);

  await client.closeCapture();
  assert.deepEqual(runner.calls[2]?.args, ["--session", "unit-test", "close", "--shutdown"]);
  assert.deepEqual(client.getSessionState(), {});
});

test("client maps query methods to the local rdc-cli 0.6.1 signatures", async () => {
  const runner = new FakeRunner();
  const client = await openFakeClient(runner);
  runner.calls.length = 0;

  await client.getDraws({ pass: "Main Pass", sort: "eid", limit: 12 });
  await client.getDraw(134);
  await client.getPipeline(134);
  await client.getBindings(134, { set: 2, binding: 3 });
  await client.getShader(134, "ps");
  await client.getShaderConstants(134, "ps");
  await client.getShaderSource(134, "cs");
  await client.getPixelHistory(640, 360, { eid: 134, target: 0, sample: 0 });

  assert.deepEqual(runner.calls.map((call) => call.args), [
    ["--session", "unit-test", "draws", "--pass", "Main Pass", "--sort", "eid", "--limit", "12", "--json"],
    ["--session", "unit-test", "draw", "134", "--json"],
    ["--session", "unit-test", "pipeline", "134", "--json"],
    ["--session", "unit-test", "bindings", "134", "--set", "2", "--binding", "3", "--json"],
    ["--session", "unit-test", "shader", "134", "ps", "--reflect", "--json"],
    // The VFS path /draws/<eid>/shader/<stage>/constants does not exist in
    // rdc-cli 0.6.1, so the old `cat` mapping always failed and every
    // inspect_draw reported constants/source as not found.
    ["--session", "unit-test", "shader", "134", "ps", "--constants", "--json"],
    ["--session", "unit-test", "shader", "134", "cs", "--source", "--json"],
    ["--session", "unit-test", "pixel", "640", "360", "134", "--target", "0", "--sample", "0", "--json"],
  ]);
});

test("client maps event, texture-stats, and constant-buffer queries to rdc-cli 0.6.1", async () => {
  const runner = new FakeRunner();
  const client = await openFakeClient(runner);
  runner.calls.length = 0;

  await client.getEvents({ type: "Dispatch", filter: "Flax*", range: "1634:2142", limit: 40 });
  await client.getEvents({ limit: 5 });
  await client.getTextureStats(2472, { eid: 3760, mip: 0, slice: 1, histogram: true });
  await client.getTextureStats(2472);
  await client.getConstantBuffer(3782, { stage: "ps", set: 1, binding: 3 });
  await client.getShader(3782, "cs", { disasmTarget: "spirv", reflect: false });
  await client.getPickPixel(550, 530, { eid: 5438, target: 0 });
  await client.exportTexture(2263, { output: "out.png", mip: 0 });

  assert.deepEqual(runner.calls.map((call) => call.args), [
    ["--session", "unit-test", "events", "--type", "Dispatch", "--filter", "Flax*",
     "--range", "1634:2142", "--limit", "40", "--json"],
    ["--session", "unit-test", "events", "--limit", "5", "--json"],
    ["--session", "unit-test", "tex-stats", "2472", "3760", "--mip", "0",
     "--slice", "1", "--histogram", "--json"],
    ["--session", "unit-test", "tex-stats", "2472", "--json"],
    ["--session", "unit-test", "cbuffer", "3782", "--stage", "ps", "--set", "1",
     "--binding", "3", "--json"],
    ["--session", "unit-test", "shader", "3782", "cs", "--target", "spirv", "--json"],
    ["--session", "unit-test", "pick-pixel", "550", "530", "5438", "--target", "0", "--json"],
    ["--session", "unit-test", "texture", "2263", "--mip", "0", "-o", resolve("out.png")],
  ]);
});

test("client rejects malformed event ranges and empty shader targets", async () => {
  const runner = new FakeRunner();
  const client = await openFakeClient(runner);
  runner.calls.length = 0;

  await assert.rejects(
    () => client.getEvents({ range: "1634-2142" }),
    /event range must use the N:M form/,
  );
  await assert.rejects(
    () => client.getEvents({ type: "   " }),
    /event type cannot be empty/,
  );
  await assert.rejects(
    () => client.getShader(134, "ps", { disasmTarget: " " }),
    /shader disassembly target cannot be empty/,
  );
  await assert.rejects(
    () => client.exportTexture(2263, { output: "  " }),
    /export output path cannot be empty/,
  );
  await assert.deepEqual(runner.calls, []);
});

test("client resolves omitted pixel-history EID from the latest pass", async () => {
  const runner = new FakeRunner();
  const client = await openFakeClient(runner);
  runner.calls.length = 0;
  runner.jsonResults.push({
    passes: [
      { name: "First", begin_eid: 1, end_eid: 20 },
      { name: "Last", begin_eid: 21, end_eid: 134 },
    ],
  });
  runner.jsonResults.push({ modifications: [{ eid: 134 }] });

  await client.getPixelHistory(640, 360);
  assert.deepEqual(runner.calls.map((call) => call.args), [
    ["--session", "unit-test", "passes", "--json"],
    ["--session", "unit-test", "pixel", "640", "360", "134", "--json"],
  ]);
});

function processExitError(exitCode: number, stdout: string): RdcProcessError {
  return new RdcProcessError({
    message: `exit ${exitCode}`,
    kind: "exit",
    executable: "rdc",
    args: [],
    exitCode,
    stdout,
    stderr: "",
  });
}

test("client treats only assert-pixel exit code 1 as an assertion failure", async () => {
  const runner = new FakeRunner();
  const client = await openFakeClient(runner);
  runner.jsonResults.push(processExitError(1, JSON.stringify({ passed: false, actual: [0, 0, 0, 1] })));
  const result = await client.assertPixel({
    eid: 134,
    x: 640,
    y: 360,
    expected: [1, 0, 0, 1],
  });
  assert.equal(result.passed, false);
  assert.equal(result.exitCode, 1);
  assert.deepEqual(result.details, { passed: false, actual: [0, 0, 0, 1] });

  runner.jsonResults.push(processExitError(2, JSON.stringify({ error: "replay failed" })));
  await assert.rejects(
    client.assertPixel({ eid: 134, x: 640, y: 360, expected: [1, 0, 0, 1] }),
    (error: unknown) => error instanceof RdcProcessError && error.exitCode === 2,
  );
});

test("client treats diff exit code 1 as a valid difference and exit code 2 as an error", async () => {
  const runner = new FakeRunner();
  const client = new RdcClient({ runner, cwd: process.cwd(), sessionName: "unit-test" });
  runner.jsonResults.push(processExitError(1, JSON.stringify({ identical: false, diff_pixels: 12 })));
  const changed = await client.diffCaptures("captures\\bug.rdc", "captures\\fixed.rdc", {
    framebuffer: true,
  }) as { modes: { framebuffer: unknown } };
  assert.deepEqual(changed.modes.framebuffer, { identical: false, diff_pixels: 12 });

  runner.jsonResults.push(processExitError(2, JSON.stringify({ error: "replay failed" })));
  await assert.rejects(
    client.diffCaptures("captures\\bug.rdc", "captures\\fixed.rdc", { framebuffer: true }),
    (error: unknown) => error instanceof RdcProcessError && error.exitCode === 2,
  );
});

test("client keeps paths as individual arguments and normalizes the reported capture path", async () => {
  const temporaryDirectory = await mkdtemp(`${tmpdir()}\\pi-rdc-client paths-`);
  const reportedCapture = resolve(temporaryDirectory, "bug frame_frame5.rdc");
  const reportedWaitCapture = resolve(temporaryDirectory, "explicit wait_frame0.rdc");
  const executable = resolve(temporaryDirectory, "Textured Quad.exe");
  await writeFile(reportedCapture, "capture");
  await writeFile(reportedWaitCapture, "capture");
  await writeFile(executable, "executable");

  const runner = new FakeRunner();
  runner.jsonResults.push(
    {},
    { success: true, path: reportedCapture },
    { success: true, path: reportedWaitCapture },
  );
  const client = new RdcClient({ runner, cwd: temporaryDirectory, sessionName: "unit-test" });
  try {
    await client.diffCaptures("bug frame.rdc", "fixed frame.rdc", {
      framebuffer: true,
    });
    const normalized = await client.captureExecutable({
      executable: "Textured Quad.exe",
      appArgs: ["--label=argument with spaces"],
      output: "bug frame.rdc",
      frame: 5,
    });

    const diff = runner.calls[0]?.args ?? [];
    assert.equal(diff[3], resolve(temporaryDirectory, "bug frame.rdc"));
    assert.equal(diff[4], resolve(temporaryDirectory, "fixed frame.rdc"));
    const capture = runner.calls[1]?.args ?? [];
    assert.ok(capture.includes(resolve(temporaryDirectory, "Textured Quad.exe")));
    assert.ok(capture.includes("--label=argument with spaces"));
    assert.equal(capture.includes("--wait-for-exit"), false);
    await access(resolve(temporaryDirectory, "bug frame.rdc"));
    await assert.rejects(access(reportedCapture));
    assert.equal((normalized.details as { path: string }).path, resolve(temporaryDirectory, "bug frame.rdc"));

    await client.captureExecutable({
      executable: "Textured Quad.exe",
      output: "explicit wait.rdc",
      waitForExit: true,
    });
    assert.ok(runner.calls[2]?.args.includes("--wait-for-exit"));
    await access(resolve(temporaryDirectory, "explicit wait.rdc"));
  } finally {
    await rm(temporaryDirectory, { recursive: true, force: true });
  }
});

test("client rejects missing executables and existing capture destinations before launch", async () => {
  const temporaryDirectory = await mkdtemp(`${tmpdir()}\\pi-rdc-client safety-`);
  const runner = new FakeRunner();
  const client = new RdcClient({ runner, cwd: temporaryDirectory, sessionName: "unit-test" });
  try {
    await assert.rejects(
      client.captureExecutable({ executable: "missing.exe", output: "capture.rdc" }),
      /Capture executable is not a file/,
    );
    await writeFile(resolve(temporaryDirectory, "demo.exe"), "executable");
    await writeFile(resolve(temporaryDirectory, "capture.rdc"), "existing capture");
    await assert.rejects(
      client.captureExecutable({ executable: "demo.exe", output: "capture.rdc" }),
      /Refusing to overwrite an existing capture/,
    );
    assert.equal(runner.calls.length, 0);
  } finally {
    await rm(temporaryDirectory, { recursive: true, force: true });
  }
});
