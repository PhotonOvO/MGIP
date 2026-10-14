import assert from "node:assert/strict";
import { readFile, stat } from "node:fs/promises";
import test from "node:test";

import {
  addEvidence,
  addHypothesis,
  createInvestigation,
  writeInvestigation,
} from "../../src/evidence/investigation.ts";
import { RdcClient } from "../../src/rdc/client.ts";
import { inspectDraw, type DrawInspection } from "../../src/rdc/inspect-draw.ts";
import type { RdcCaptureInfo, RdcDraw, RdcPixelAssertionResult } from "../../src/rdc/types.ts";

const bugCapture = "captures/bug.rdc";
const fixedCapture = "captures/fixed.rdc";
const targetPixel = { x: 640, y: 360 };

type JsonRecord = Record<string, unknown>;

interface CaptureEvidence {
  info: RdcCaptureInfo;
  draw: RdcDraw & { eid: number };
  pixelHistory: unknown;
  inspection: DrawInspection;
  assertion?: RdcPixelAssertionResult;
}

function isRecord(value: unknown): value is JsonRecord {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function colorTuple(value: unknown): [number, number, number, number] {
  assert.ok(isRecord(value));
  const tuple = [value.r, value.g, value.b, value.a];
  assert.ok(tuple.every((channel) => typeof channel === "number"));
  return tuple as [number, number, number, number];
}

function responsibleModification(pixelHistory: unknown, eid: number): JsonRecord {
  assert.ok(isRecord(pixelHistory));
  assert.ok(Array.isArray(pixelHistory.modifications));
  const modification = pixelHistory.modifications.find((candidate) =>
    isRecord(candidate) && candidate.eid === eid);
  assert.ok(isRecord(modification), `pixel history did not contain EID ${eid}`);
  assert.equal(modification.passed, true);
  return modification;
}

function findRecord(value: unknown, predicate: (record: JsonRecord) => boolean): JsonRecord {
  assert.ok(Array.isArray(value));
  const result = value.find((candidate) => isRecord(candidate) && predicate(candidate));
  assert.ok(isRecord(result));
  return result;
}

async function collectCaptureEvidence(
  capture: string,
  sessionName: string,
  expected?: readonly [number, number, number, number],
): Promise<CaptureEvidence> {
  const client = new RdcClient({ sessionName });
  try {
    const opened = await client.openCapture(capture);
    const draws = await client.getDraws({ limit: 20 });
    const draw = draws.find((candidate) => candidate.marker === "TexturedQuad/Main");
    assert.ok(draw && typeof draw.eid === "number");
    const inspection = await inspectDraw(client, draw.eid);
    const pixelHistory = await client.getPixelHistory(targetPixel.x, targetPixel.y, { eid: draw.eid });
    const assertion = expected === undefined
      ? undefined
      : await client.assertPixel({
        eid: draw.eid,
        x: targetPixel.x,
        y: targetPixel.y,
        expected,
        tolerance: 0.05,
      });
    return {
      info: opened.info,
      draw: draw as RdcDraw & { eid: number },
      pixelHistory,
      inspection,
      assertion,
    };
  } finally {
    await client.closeCapture(undefined, true);
  }
}

test("the textured-quad investigation closes the evidence-patch-recapture loop", async () => {
  const bug = await collectCaptureEvidence(bugCapture, "pi-v01-closed-loop-bug");
  const bugModification = responsibleModification(bug.pixelHistory, bug.draw.eid);
  assert.deepEqual(colorTuple(bugModification.post_mod), [0, 0, 0, 1]);

  const texture = findRecord(
    bug.inspection.bindings.descriptors,
    (record) => record.binding === "BaseColorTexture" && record.type === "Image",
  );
  assert.equal(texture.resource_name, "BaseColorTexture.Red1x1");
  assert.equal(texture.format, "R8G8B8A8_UNORM");
  assert.equal(texture.width, 1);
  assert.equal(texture.height, 1);

  const bugFactor = findRecord(
    bug.inspection.constants.pixel,
    (record) => record.name === "BaseColorFactor",
  );
  assert.deepEqual(bugFactor.value, [0, 0, 0, 1]);
  const shaderLines = bug.inspection.shaders.pixel?.source?.relevantLines ?? [];
  assert.ok(shaderLines.some((line) => line.includes("BaseColorTexture.Sample")));
  assert.ok(shaderLines.some((line) => line.includes("texColor * BaseColorFactor")));

  const fixed = await collectCaptureEvidence(
    fixedCapture,
    "pi-v01-closed-loop-fixed",
    [1, 0, 0, 1],
  );
  const fixedModification = responsibleModification(fixed.pixelHistory, fixed.draw.eid);
  assert.deepEqual(colorTuple(fixedModification.post_mod), [1, 0, 0, 1]);
  assert.equal(fixed.assertion?.passed, true);
  const fixedFactor = findRecord(
    fixed.inspection.constants.pixel,
    (record) => record.name === "BaseColorFactor",
  );
  assert.deepEqual(fixedFactor.value, [1, 1, 1, 1]);

  const diffClient = new RdcClient({ sessionName: "pi-v01-closed-loop-diff" });
  const diff = await diffClient.diffCaptures(bugCapture, fixedCapture, {
    draws: true,
    passes: true,
    framebuffer: true,
    threshold: 0.01,
  });
  assert.ok(isRecord(diff) && isRecord(diff.modes));
  assert.ok(Array.isArray(diff.modes.draws));
  const drawDiff = findRecord(diff.modes.draws, (record) => record.marker === "TexturedQuad/Main");
  assert.equal(drawDiff.status, "=");
  assert.ok(isRecord(diff.modes.framebuffer));
  assert.equal(diff.modes.framebuffer.identical, false);
  assert.equal(diff.modes.framebuffer.diff_pixels, 518_400);

  const sourcePath = "examples/textured-quad/main.cpp";
  const source = await readFile(sourcePath, "utf8");
  const sourceLines = source.split(/\r?\n/);
  const factorLineIndex = sourceLines.findIndex((line) =>
    line.includes("BaseColorFactor = {1.0F, 1.0F, 1.0F, 1.0F}"));
  assert.ok(factorLineIndex >= 0);
  const executablePath = "out/pi-renderdoc-demo/bin/Release/pi_renderdoc_textured_quad.exe";
  const [executableStats, bugStats, fixedStats] = await Promise.all([
    stat(executablePath),
    stat(bugCapture),
    stat(fixedCapture),
  ]);

  const investigation = createInvestigation({
    id: "textured-quad-black-center",
    symptom: "The textured quad should be red at the center of the frame, but captures/bug.rdc shows black.",
    capture: bugCapture,
    targetPixel,
  });
  addEvidence(investigation, {
    id: "E1",
    source: "renderdoc",
    kind: "pixel-history",
    description: `Pixel history identifies EID ${bug.draw.eid} (${bug.draw.marker}) as the draw that writes black at (640, 360).`,
    data: {
      eid: bug.draw.eid,
      marker: bug.draw.marker,
      shaderOutput: colorTuple(bugModification.shader_out),
      postModification: colorTuple(bugModification.post_mod),
      passed: bugModification.passed,
      flags: bugModification.flags,
    },
  });
  addEvidence(investigation, {
    id: "E2",
    source: "renderdoc",
    kind: "binding",
    description: "The responsible pixel shader has the deterministic red 1x1 RGBA8 texture bound as BaseColorTexture.",
    data: texture,
  });
  addEvidence(investigation, {
    id: "E3",
    source: "renderdoc",
    kind: "constant",
    description: "MaterialConstants.BaseColorFactor is [0, 0, 0, 1] at the responsible draw.",
    data: bugFactor,
  });
  addEvidence(investigation, {
    id: "E4",
    source: "renderdoc",
    kind: "shader",
    description: "Captured pixel-shader source multiplies the sampled texture by BaseColorFactor.",
    data: {
      shader: bug.inspection.shaders.pixel?.shader,
      sourceHash: bug.inspection.shaders.pixel?.source?.sha256,
      relevantLines: shaderLines,
    },
  });
  addEvidence(investigation, {
    id: "E5",
    source: "renderdoc",
    kind: "draw",
    description: `EID ${bug.draw.eid} is the TexturedQuad/Main draw on a 1280x720 R8G8B8A8_UNORM target.`,
    data: {
      draw: bug.inspection.draw,
      pass: bug.inspection.pass,
      topology: bug.inspection.pipeline.topology,
      viewport: bug.inspection.pipeline.viewport,
      blend: bug.inspection.pipeline.blend,
    },
  });
  addEvidence(investigation, {
    id: "E6",
    source: "source",
    kind: "source-code",
    description: "GPU symbol BaseColorFactor maps to the MaterialConstants initializer in the isolated demo source.",
    data: {
      file: sourcePath,
      line: factorLineIndex + 1,
      symbol: "MaterialConstants::BaseColorFactor",
      before: "{0.0F, 0.0F, 0.0F, 1.0F}",
      after: "{1.0F, 1.0F, 1.0F, 1.0F}",
    },
  });
  addEvidence(investigation, {
    id: "E7",
    source: "build",
    kind: "build-result",
    description: "The isolated Release target rebuilt successfully after the one-line constant fix.",
    data: {
      command: "cmake --build out/pi-renderdoc-demo --config Release",
      exitCode: 0,
      executable: executablePath,
      executableBytes: executableStats.size,
    },
  });
  addEvidence(investigation, {
    id: "E8",
    source: "renderdoc",
    kind: "verification",
    description: "The post-patch capture writes red, uses an identity factor, and passes the automated center-pixel assertion.",
    data: {
      capture: fixedCapture,
      eid: fixed.draw.eid,
      marker: fixed.draw.marker,
      postModification: colorTuple(fixedModification.post_mod),
      BaseColorFactor: fixedFactor.value,
      assertion: fixed.assertion,
      diff,
      artifacts: {
        bugBytes: bugStats.size,
        fixedBytes: fixedStats.size,
      },
    },
  });

  const hypothesis = addHypothesis(investigation, {
    id: "H1",
    claim: "The black output is caused by BaseColorFactor being uploaded as zero RGB, which multiplies the correctly bound red texture to black.",
    evidenceIds: ["E1", "E2", "E3", "E4"],
    expectedBehavior: "Changing only BaseColorFactor to [1, 1, 1, 1] preserves draw structure and changes the center output from black to red.",
    confidence: "high",
    verification: "Rebuild, recapture as captures/fixed.rdc, inspect EID 11, diff captures, and assert pixel (640, 360) equals [1, 0, 0, 1] within 0.05.",
    status: "proposed",
  });
  investigation.patch = {
    files: [sourcePath],
    summary: "Changed only MaterialConstants.BaseColorFactor from [0, 0, 0, 1] to [1, 1, 1, 1].",
  };
  investigation.verification = {
    capture: fixedCapture,
    passed: fixed.assertion?.passed === true,
    details: {
      evidenceId: "E8",
      assertion: fixed.assertion,
      centerPixel: colorTuple(fixedModification.post_mod),
      BaseColorFactor: fixedFactor.value,
      diff,
    },
  };
  hypothesis.status = investigation.verification.passed ? "confirmed" : "proposed";

  const written = await writeInvestigation(investigation);
  const [json, report] = await Promise.all([
    readFile(written.jsonPath, "utf8"),
    readFile(written.reportPath, "utf8"),
  ]);
  assert.match(json, /"status": "confirmed"/);
  assert.match(report, /Result: PASS/);
  assert.match(report, /H1: The black output is caused by BaseColorFactor/);
});
