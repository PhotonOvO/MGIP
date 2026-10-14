import assert from "node:assert/strict";
import test from "node:test";

import type { ExtensionAPI } from "@earendil-works/pi-coding-agent";

import {
  RENDERDOC_TOOL_NAMES,
  registerRenderDocExtension,
  type RenderDocExtensionClient,
} from "../../.pi/extensions/renderdoc.ts";
import { DIRECT_RENDERDOC_REASON } from "../../src/rdc/tool-policy.ts";

interface TextResult {
  content: Array<{ type: string; text: string }>;
  details?: unknown;
}

interface RegisteredTool {
  name: string;
  execute(
    toolCallId: string,
    params: Record<string, unknown>,
    signal?: AbortSignal,
  ): Promise<TextResult>;
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

function createFakeClient() {
  let closeCalls = 0;
  const draws = Array.from({ length: 100 }, (_, index) => ({
    eid: index + 1,
    type: "Draw",
    triangles: 2,
    instances: 1,
    pass: index % 2 === 0 ? "Main Pass" : "Shadow Pass",
    marker: index % 2 === 0 ? "TexturedQuad/Main" : "Shadow",
  }));
  const client = {
    get closeCalls() {
      return closeCalls;
    },
    getSessionState: () => ({ capturePath: "G:\\MGIF\\captures\\bug.rdc", openedAt: "now" }),
    async openCapture() {
      return {
        capturePath: "G:\\MGIF\\captures\\bug.rdc",
        openedAt: "2026-08-24T00:00:00.000Z",
        info: { API: "D3D11", Events: 44, "Draw Calls": "1", Clears: 1, Copies: 0 },
      };
    },
    async closeCapture() {
      closeCalls += 1;
    },
    async getInfo() {
      return { API: "D3D11", Events: 44, "Draw Calls": "1" };
    },
    async getDraws() {
      return draws;
    },
    async getPasses() {
      return {
        passes: [
          { name: "Main Pass", begin_eid: 1, end_eid: 50, draws: 25 },
          { name: "Shadow Pass", begin_eid: 51, end_eid: 100, draws: 25 },
        ],
      };
    },
    async getPixelHistory() {
      return { x: 640, y: 360, modifications: [{ eid: 134, passed: true }] };
    },
    async diffCaptures() {
      return { draws: { changed: 0 }, framebuffer: { changedPixels: 100 } };
    },
    async assertPixel() {
      return { passed: false, exitCode: 1, details: { actual: [0, 0, 0, 1] }, stderr: "" };
    },
    async captureExecutable() {
      return { outputPath: "G:\\MGIF\\captures\\fixed.rdc", details: { captured: true } };
    },
    async getDraw() {
      return {};
    },
    async getPipeline() {
      return {};
    },
    async getPipelineSection() {
      return {};
    },
    async getBindings() {
      return [];
    },
    async getDescriptors() {
      return [];
    },
    async getPass() {
      return {};
    },
    async getShaders() {
      return [];
    },
    async getShader() {
      return {};
    },
    async getShaderConstants() {
      return {};
    },
    async getShaderSource() {
      return {};
    },
  };
  return client as unknown as RenderDocExtensionClient & { readonly closeCalls: number };
}

function parseResult(result: TextResult): Record<string, unknown> {
  const text = result.content[0]?.text;
  assert.ok(text);
  return JSON.parse(text) as Record<string, unknown>;
}

test("Pi extension registers only the bounded V0.1 RenderDoc tool surface", () => {
  const { pi, tools } = createFakePi();
  registerRenderDocExtension(pi, createFakeClient());
  assert.deepEqual([...tools.keys()], [...RENDERDOC_TOOL_NAMES]);
});

test("Pi open and frame overview tools return bounded semantic summaries", async () => {
  const { pi, tools } = createFakePi();
  registerRenderDocExtension(pi, createFakeClient());

  const open = parseResult(await tools.get("rdc_open")!.execute("open", { capture: "captures/bug.rdc" }));
  assert.equal(open.api, "D3D11");
  assert.equal(open.status, "open");
  assert.equal("raw" in open, false);

  const overview = parseResult(await tools.get("rdc_frame_overview")!.execute("frame", {
    limit: 3,
    filter: "TexturedQuad",
  }));
  const selected = overview.draws as unknown[];
  assert.equal(selected.length, 3);
  assert.ok(selected.every((draw) => JSON.stringify(draw).includes("TexturedQuad/Main")));
  assert.equal((overview.limits as { returnedDraws: number }).returnedDraws, 3);
});

test("Pi pixel assertion preserves assertion-failed semantics", async () => {
  const { pi, tools } = createFakePi();
  registerRenderDocExtension(pi, createFakeClient());
  const result = parseResult(await tools.get("rdc_assert_pixel")!.execute("assert", {
    eid: 134,
    x: 640,
    y: 360,
    expected: [1, 0, 0, 1],
    tolerance: 0.05,
  }));
  assert.equal(result.passed, false);
  assert.equal(result.exitCode, 1);
});

test("Pi bash preflight blocks direct RenderDoc execution with the required reason", async () => {
  const { pi, handlers } = createFakePi();
  registerRenderDocExtension(pi, createFakeClient());
  const preflight = handlers.get("tool_call")?.[0];
  assert.ok(preflight);

  const blocked = await preflight(
    { toolName: "bash", input: { command: "rdc info --json" } },
    {},
  ) as { block: boolean; reason: string };
  assert.deepEqual(blocked, { block: true, reason: DIRECT_RENDERDOC_REASON });

  const allowed = await preflight(
    { toolName: "bash", input: { command: "rg -n rdc src" } },
    {},
  );
  assert.equal(allowed, undefined);
});

test("Pi session shutdown closes the RenderDoc client", async () => {
  const { pi, handlers } = createFakePi();
  const client = createFakeClient();
  registerRenderDocExtension(pi, client);
  const shutdown = handlers.get("session_shutdown")?.[0];
  assert.ok(shutdown);
  await shutdown({}, {});
  assert.equal(client.closeCalls, 1);
});

