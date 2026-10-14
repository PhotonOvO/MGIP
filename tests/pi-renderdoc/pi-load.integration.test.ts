import assert from "node:assert/strict";
import test from "node:test";

import {
  createAgentSession,
  DefaultResourceLoader,
  getAgentDir,
  SessionManager,
  type AgentSession,
  type AgentToolResult,
} from "@earendil-works/pi-coding-agent";

import { RENDERDOC_TOOL_NAMES } from "../../.pi/extensions/renderdoc.ts";

const capture = "captures/bug.rdc";
const fixedCapture = "captures/fixed.rdc";

function isRecord(value: unknown): value is Record<string, unknown> {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function parseJsonResult(result: AgentToolResult<unknown>): Record<string, unknown> {
  const text = result.content.find((entry) => entry.type === "text")?.text;
  assert.ok(text, "typed Pi tool did not return text content");
  return JSON.parse(text) as Record<string, unknown>;
}

async function executeJsonTool(
  session: AgentSession,
  name: string,
  params: Record<string, unknown>,
): Promise<Record<string, unknown>> {
  const definition = session.getToolDefinition(name);
  assert.ok(definition, `Pi did not register ${name}`);
  const result = await definition.execute(
    `pi-load-integration-${name}`,
    params,
    undefined,
    undefined,
    undefined as never,
  );
  return parseJsonResult(result);
}

test("Pi discovers the project RenderDoc extension, skill, and typed replay tools", async () => {
  const cwd = process.cwd();
  const loader = new DefaultResourceLoader({ cwd, agentDir: getAgentDir() });
  await loader.reload({ resolveProjectTrust: async () => true });

  const extensionResult = loader.getExtensions();
  assert.deepEqual(extensionResult.errors, []);
  assert.ok(extensionResult.extensions.some((extension) =>
    extension.resolvedPath.endsWith(".pi\\extensions\\renderdoc.ts")
      || extension.resolvedPath.endsWith(".pi/extensions/renderdoc.ts")));

  const skills = loader.getSkills();
  assert.deepEqual(skills.diagnostics, []);
  const skill = skills.skills.find((candidate) => candidate.name === "renderdoc-debug");
  assert.ok(skill);
  assert.ok(skill.filePath.endsWith(".pi\\skills\\renderdoc-debug\\SKILL.md")
    || skill.filePath.endsWith(".pi/skills/renderdoc-debug/SKILL.md"));

  const { session } = await createAgentSession({
    cwd,
    resourceLoader: loader,
    sessionManager: SessionManager.inMemory(),
    noTools: "builtin",
  });

  try {
    const allRdcTools = session.getAllTools()
      .map((tool) => tool.name)
      .filter((name) => name.startsWith("rdc_"));
    assert.deepEqual(allRdcTools, [...RENDERDOC_TOOL_NAMES]);
    assert.deepEqual(
      session.getActiveToolNames().filter((name) => name.startsWith("rdc_")),
      [...RENDERDOC_TOOL_NAMES],
    );

    const opened = await executeJsonTool(session, "rdc_open", { capture });
    assert.equal(opened.api, "D3D11");
    assert.equal(opened.status, "open");

    const overview = await executeJsonTool(session, "rdc_frame_overview", {
      limit: 5,
      filter: "TexturedQuad/Main",
    });
    assert.ok(Array.isArray(overview.draws));
    assert.ok((overview.draws as unknown[]).length > 0);
    assert.ok(Buffer.byteLength(JSON.stringify(overview), "utf8") < 40 * 1024);

    const pixel = await executeJsonTool(session, "rdc_pixel_history", {
      x: 640,
      y: 360,
    });
    assert.equal(pixel.eid, 11);
    assert.ok(Array.isArray(pixel.modifications));
    assert.ok((pixel.modifications as unknown[]).some((entry) =>
      isRecord(entry) && entry.eid === 11 && entry.passed === true));

    const inspection = await executeJsonTool(session, "rdc_inspect_draw", { eid: 11 });
    const serializedInspection = JSON.stringify(inspection);
    assert.match(serializedInspection, /BaseColorTexture\.Red1x1/);
    assert.match(serializedInspection, /BaseColorFactor/);
    assert.ok(Buffer.byteLength(serializedInspection, "utf8") < 40 * 1024);

    const closed = await executeJsonTool(session, "rdc_close", {});
    assert.equal(closed.status, "closed");

    const diff = await executeJsonTool(session, "rdc_diff", {
      before: capture,
      after: fixedCapture,
      threshold: 0.01,
    });
    assert.ok(isRecord(diff.modes));
    assert.ok(isRecord(diff.modes.framebuffer));
    assert.equal(diff.modes.framebuffer.diff_pixels, 518_400);

    const fixedOpened = await executeJsonTool(session, "rdc_open", { capture: fixedCapture });
    assert.equal(fixedOpened.api, "D3D11");
    const assertion = await executeJsonTool(session, "rdc_assert_pixel", {
      eid: 11,
      x: 640,
      y: 360,
      expected: [1, 0, 0, 1],
      tolerance: 0.05,
    });
    assert.equal(assertion.passed, true);
    const fixedClosed = await executeJsonTool(session, "rdc_close", {});
    assert.equal(fixedClosed.status, "closed");
  } finally {
    const close = session.getToolDefinition("rdc_close");
    if (close) {
      await close.execute("pi-load-integration-final-close", {}, undefined, undefined, undefined as never)
        .catch(() => undefined);
    }
    session.dispose();
  }
});
