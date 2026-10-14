import assert from "node:assert/strict";
import { mkdtemp, readFile, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

import {
  addEvidence,
  addHypothesis,
  createInvestigation,
  InvestigationValidationError,
  validateInvestigation,
  writeInvestigation,
} from "../../src/evidence/investigation.ts";

function buildVerifiedInvestigation() {
  const investigation = createInvestigation({
    id: "textured-quad-v01",
    symptom: "The center quad should be red but is black.",
    capture: "captures/bug.rdc",
    targetPixel: { x: 640, y: 360 },
  });
  const pixel = addEvidence(investigation, {
    source: "renderdoc",
    kind: "pixel-history",
    description: "EID 134 writes black to the target pixel.",
    data: { eid: 134, post_mod: { r: 0, g: 0, b: 0, a: 1 } },
  });
  const constant = addEvidence(investigation, {
    source: "renderdoc",
    kind: "constant",
    description: "BaseColorFactor is zero in RGB.",
    data: { BaseColorFactor: [0, 0, 0, 1] },
  });
  addHypothesis(investigation, {
    claim: "The zero BaseColorFactor causes the black output.",
    evidenceIds: [pixel.id, constant.id],
    expectedBehavior: "Uploading [1,1,1,1] preserves the bound red texture.",
    confidence: "high",
    verification: "Rebuild, recapture, and assert the center pixel is red.",
    status: "confirmed",
  });
  investigation.patch = {
    files: ["examples/textured-quad/main.cpp"],
    summary: "Set BaseColorFactor RGB from zero to one.",
  };
  investigation.verification = {
    capture: "captures/fixed.rdc",
    passed: true,
    details: { pixelAssertion: "PASS" },
  };
  return investigation;
}

test("Evidence IDs are stable and immediately referenceable", () => {
  const investigation = createInvestigation({ id: "ids", symptom: "symptom", capture: "bug.rdc" });
  const first = addEvidence(investigation, { source: "renderdoc", kind: "draw", description: "draw", data: {} });
  const second = addEvidence(investigation, { source: "source", kind: "source-code", description: "source", data: {} });
  assert.equal(first.id, "E1");
  assert.equal(second.id, "E2");
  assert.equal(investigation.evidence.find((evidence) => evidence.id === first.id), first);
});

test("Hypothesis cannot reference missing Evidence", () => {
  const investigation = createInvestigation({ id: "missing-evidence", symptom: "symptom", capture: "bug.rdc" });
  addHypothesis(investigation, {
    claim: "claim",
    evidenceIds: ["E404"],
    expectedBehavior: "expected",
    confidence: "low",
    verification: "verify",
    status: "proposed",
  });
  assert.throws(
    () => validateInvestigation(investigation),
    (error: unknown) => error instanceof InvestigationValidationError && /missing Evidence ID E404/.test(error.message),
  );
});

test("confirmed Hypothesis requires a passed second capture", () => {
  const investigation = buildVerifiedInvestigation();
  investigation.verification = undefined;
  assert.throws(() => validateInvestigation(investigation), /cannot be confirmed without passed/);

  investigation.verification = { capture: investigation.capture, passed: true, details: {} };
  assert.throws(() => validateInvestigation(investigation), /must differ from the original capture/);
});

test("Investigation persists JSON source of truth and a traceable report", async (context) => {
  const temporaryRoot = await mkdtemp(join(tmpdir(), "pi-renderdoc-investigation-"));
  context.after(async () => rm(temporaryRoot, { recursive: true, force: true }));
  const investigation = buildVerifiedInvestigation();
  const paths = await writeInvestigation(investigation, temporaryRoot);
  const roundTrip = JSON.parse(await readFile(paths.jsonPath, "utf8")) as typeof investigation;
  const report = await readFile(paths.reportPath, "utf8");
  assert.deepEqual(roundTrip, investigation);
  assert.match(report, /# Investigation: textured-quad-v01/);
  assert.match(report, /H1: The zero BaseColorFactor causes the black output\. \(Evidence: E1, E2\)/);
  assert.match(report, /Result: PASS/);
});
