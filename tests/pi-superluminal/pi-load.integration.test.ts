import assert from "node:assert/strict";
import { existsSync, readdirSync, statSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

import { DefaultResourceLoader, getAgentDir } from "@earendil-works/pi-coding-agent";

import { SLP_TOOL_NAMES } from "../../src/slp/types.ts";

/**
 * Verifies Pi's real resource loader (jiti) discovers and loads the project
 * Superluminal extension and skill, and that the extension registers its full
 * typed tool surface. This catches import-path and TS-syntax errors that the
 * fake-Pi unit tests cannot, because they never go through jiti.
 *
 * Runs from the repo root (two levels above this file). Tool *execution* against
 * a real trace is covered by slp-integration.test.ts; here we assert discovery
 * and registration, and optionally execute slp_info when SLP_PY + a trace exist.
 */

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "../..");

const SLP_PY = process.env.SLP_PY ?? "";
const CAPTURES = process.env.SLP_CAPTURES ?? "/mnt/f/Captures";

function findTrace(): string | undefined {
  if (process.env.SLP_TRACE && existsSync(process.env.SLP_TRACE)) return process.env.SLP_TRACE;
  if (!existsSync(CAPTURES)) return undefined;
  return readdirSync(CAPTURES)
    .filter((n) => n.toLowerCase().endsWith(".slp"))
    .map((n) => resolve(CAPTURES, n))
    .find((p) => statSync(p).isFile());
}

test("Pi discovers the Superluminal extension and skill with no load errors", async () => {
  const loader = new DefaultResourceLoader({ cwd: repoRoot, agentDir: getAgentDir() });
  await loader.reload({ resolveProjectTrust: async () => true });

  const extensionsResult = loader.getExtensions();
  assert.deepEqual(extensionsResult.errors, []);

  const slpExtension = extensionsResult.extensions.find((e) =>
    e.resolvedPath.replace(/\\/g, "/").endsWith(".pi/extensions/superluminal.ts"));
  assert.ok(slpExtension, "superluminal.ts extension was not discovered");

  // The extension registers exactly the declared tool surface.
  const registered = [...slpExtension.tools.keys()].filter((name) => name.startsWith("slp_"));
  assert.deepEqual(registered.sort(), [...SLP_TOOL_NAMES].sort());

  // And installs the bash guard for the unbounded dump-json subcommand.
  assert.ok(slpExtension.handlers.has("tool_call"), "tool_call handler not registered");

  const skills = loader.getSkills();
  assert.deepEqual(skills.diagnostics, []);
  const skill = skills.skills.find((s) => s.name === "superluminal-perf");
  assert.ok(skill, "superluminal-perf skill was not discovered");
  assert.ok(skill.filePath.replace(/\\/g, "/").endsWith(".pi/skills/superluminal-perf/SKILL.md"));
});

test("Pi-loaded slp_info executes against a real trace (skipped without SLP_PY + trace)", async (t) => {
  const trace = findTrace();
  if (!SLP_PY || !existsSync(SLP_PY) || !trace) {
    t.skip("set SLP_PY and SLP_CAPTURES (or SLP_TRACE) to enable");
    return;
  }
  process.env.SLP_PY = SLP_PY;
  const loader = new DefaultResourceLoader({ cwd: repoRoot, agentDir: getAgentDir() });
  await loader.reload({ resolveProjectTrust: async () => true });
  const slpExtension = loader.getExtensions().extensions.find((e) =>
    e.resolvedPath.replace(/\\/g, "/").endsWith(".pi/extensions/superluminal.ts"));
  assert.ok(slpExtension);
  const registered = slpExtension.tools.get("slp_info");
  assert.ok(registered);
  const infoTool = registered.definition;
  assert.equal(typeof infoTool.execute, "function");

  const result = await infoTool.execute("pi-load-slp-info", { trace }, undefined, undefined,
    undefined as never) as { content: Array<{ type: string; text?: string }> };
  const text = result.content.find((c) => c.type === "text")?.text ?? "";
  assert.match(text, /sample interval/i);
  assert.ok(Buffer.byteLength(text, "utf8") < 40 * 1024, "slp_info output must stay bounded");
});
