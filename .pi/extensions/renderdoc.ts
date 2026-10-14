import type { ExtensionAPI } from "@earendil-works/pi-coding-agent";
import { Type } from "typebox";

import { RdcClient } from "../../src/rdc/client.ts";
import { inspectDraw, type RdcInspectionClient } from "../../src/rdc/inspect-draw.ts";
import {
  boundedToolResult,
  DIRECT_RENDERDOC_REASON,
  isDirectRenderDocCommand,
} from "../../src/rdc/tool-policy.ts";
import type {
  RdcAssertPixelOptions,
  RdcCaptureExecutableOptions,
  RdcCaptureInfo,
  RdcCaptureResult,
  RdcConstantBufferQuery,
  RdcDiffOptions,
  RdcDraw,
  RdcDrawQuery,
  RdcEventQuery,
  RdcExportTextureOptions,
  RdcOpenResult,
  RdcPassTree,
  RdcPixelAssertionResult,
  RdcPixelQuery,
  RdcPickPixelQuery,
  RdcSessionState,
  RdcShaderDataQuery,
  RdcShaderStage,
  RdcTextureStatsQuery,
} from "../../src/rdc/types.ts";

export const RENDERDOC_TOOL_NAMES = [
  "rdc_open",
  "rdc_frame_overview",
  "rdc_pixel_history",
  "rdc_inspect_draw",
  "rdc_events",
  "rdc_texture_stats",
  "rdc_pick_pixel",
  "rdc_export_texture",
  "rdc_constants",
  "rdc_shader",
  "rdc_diff",
  "rdc_assert_pixel",
  "rdc_capture",
  "rdc_close",
] as const;

export interface RenderDocExtensionClient extends RdcInspectionClient {
  getSessionState(): Readonly<RdcSessionState>;
  openCapture(path: string, signal?: AbortSignal): Promise<RdcOpenResult>;
  closeCapture(signal?: AbortSignal, ignoreMissing?: boolean): Promise<void>;
  getInfo(signal?: AbortSignal): Promise<RdcCaptureInfo>;
  getDraws(query?: RdcDrawQuery, signal?: AbortSignal): Promise<RdcDraw[]>;
  getPasses(signal?: AbortSignal): Promise<RdcPassTree>;
  getPixelHistory(x: number, y: number, query?: RdcPixelQuery, signal?: AbortSignal): Promise<unknown>;
  getEvents(query?: RdcEventQuery, signal?: AbortSignal): Promise<unknown>;
  getTextureStats(resourceId: number, query?: RdcTextureStatsQuery, signal?: AbortSignal): Promise<unknown>;
  getPickPixel(x: number, y: number, query?: RdcPickPixelQuery, signal?: AbortSignal): Promise<unknown>;
  exportTexture(resourceId: number, options?: RdcExportTextureOptions, signal?: AbortSignal): Promise<unknown>;
  getConstantBuffer(eid: number, query?: RdcConstantBufferQuery, signal?: AbortSignal): Promise<unknown>;
  getShader(eid: number, stage: RdcShaderStage, query?: RdcShaderDataQuery, signal?: AbortSignal): Promise<unknown>;
  diffCaptures(before: string, after: string, options?: RdcDiffOptions, signal?: AbortSignal): Promise<unknown>;
  assertPixel(options: RdcAssertPixelOptions, signal?: AbortSignal): Promise<RdcPixelAssertionResult>;
  captureExecutable(options: RdcCaptureExecutableOptions, signal?: AbortSignal): Promise<RdcCaptureResult>;
}

function captureInfoSummary(opened: RdcOpenResult): unknown {
  return {
    capture: opened.capturePath,
    api: opened.info.API ?? opened.info.api,
    frame: {
      events: opened.info.Events,
      drawCalls: opened.info["Draw Calls"],
      clears: opened.info.Clears,
      copies: opened.info.Copies,
    },
    openedAt: opened.openedAt,
    status: "open",
  };
}

function drawMatches(draw: RdcDraw, filter: string): boolean {
  const needle = filter.toLowerCase();
  return [draw.marker, draw.pass, draw.type, draw.eid]
    .some((value) => String(value ?? "").toLowerCase().includes(needle));
}

export function registerRenderDocExtension(
  pi: ExtensionAPI,
  client: RenderDocExtensionClient = new RdcClient({ sessionName: "pi-renderdoc-" + process.pid }),
): void {
  pi.on("tool_call", (event) => {
    if (event.toolName !== "bash") return undefined;
    const command = (event.input as { command?: unknown }).command;
    if (typeof command === "string" && isDirectRenderDocCommand(command)) {
      return { block: true, reason: DIRECT_RENDERDOC_REASON };
    }
    return undefined;
  });

  pi.on("session_shutdown", async () => {
    await client.closeCapture(undefined, true).catch(() => undefined);
  });

  pi.registerTool({
    name: "rdc_open",
    label: "Open RenderDoc Capture",
    description: "Open one .rdc capture in the typed RenderDoc session and return only API/frame basics. Close the current capture before opening another.",
    promptSnippet: "Open a RenderDoc capture through the typed session",
    promptGuidelines: ["Use rdc_open before any capture-dependent rdc_* query."],
    parameters: Type.Object({
      capture: Type.String({ minLength: 1 }),
    }, { additionalProperties: false }),
    async execute(_toolCallId, params, signal) {
      const opened = await client.openCapture(params.capture, signal);
      return boundedToolResult(captureInfoSummary(opened));
    },
  });

  pi.registerTool({
    name: "rdc_frame_overview",
    label: "RenderDoc Frame Overview",
    description: "Return bounded frame, pass, and draw summaries. Use limit and optional pass/name filter to control context size.",
    parameters: Type.Object({
      limit: Type.Optional(Type.Integer({ minimum: 1, maximum: 100 })),
      filter: Type.Optional(Type.String({ minLength: 1, maxLength: 200 })),
      pass: Type.Optional(Type.String({ minLength: 1, maxLength: 200 })),
    }, { additionalProperties: false }),
    async execute(_toolCallId, params, signal) {
      const limit = params.limit ?? 20;
      const queryLimit = params.filter === undefined ? limit : 100;
      const [info, passTree, drawPool] = await Promise.all([
        client.getInfo(signal),
        client.getPasses(signal),
        client.getDraws({ pass: params.pass, limit: queryLimit }, signal),
      ]);
      const draws = (params.filter === undefined
        ? drawPool
        : drawPool.filter((draw) => drawMatches(draw, params.filter!))).slice(0, limit);
      const passes = (passTree.passes ?? [])
        .filter((pass) => params.pass === undefined
          || String(pass.name ?? "").toLowerCase().includes(params.pass.toLowerCase()))
        .slice(0, limit);
      return boundedToolResult({
        api: info.API ?? info.api,
        frame: { events: info.Events, drawCalls: info["Draw Calls"] },
        passes,
        draws,
        limits: { requested: limit, returnedDraws: draws.length, returnedPasses: passes.length },
      });
    },
  });

  pi.registerTool({
    name: "rdc_pixel_history",
    label: "RenderDoc Pixel History",
    description: "Return bounded pixel modifications with EID, shader output, post-modification color, pass/fail state, depth, and rejection flags. When eid is omitted, use the latest pass/draw event.",
    parameters: Type.Object({
      x: Type.Integer({ minimum: 0 }),
      y: Type.Integer({ minimum: 0 }),
      eid: Type.Optional(Type.Integer({ minimum: 0 })),
      target: Type.Optional(Type.Integer({ minimum: 0 })),
      sample: Type.Optional(Type.Integer({ minimum: 0 })),
    }, { additionalProperties: false }),
    async execute(_toolCallId, params, signal) {
      const history = await client.getPixelHistory(params.x, params.y, {
        eid: params.eid,
        target: params.target,
        sample: params.sample,
      }, signal);
      return boundedToolResult(history);
    },
  });

  pi.registerTool({
    name: "rdc_inspect_draw",
    label: "Inspect RenderDoc Draw",
    description: "Aggregate one EID into bounded draw, pass/targets, pipeline, resolved bindings, shaders, source summary, and constants.",
    parameters: Type.Object({
      eid: Type.Integer({ minimum: 0 }),
    }, { additionalProperties: false }),
    async execute(_toolCallId, params, signal) {
      return boundedToolResult(await inspectDraw(client, params.eid, signal));
    },
  });

  pi.registerTool({
    name: "rdc_events",
    label: "List RenderDoc Events",
    description: "List frame events including compute dispatches, which rdc_frame_overview draws omit. Filter by type (e.g. Dispatch), name glob, EID range N:M, and limit.",
    parameters: Type.Object({
      type: Type.Optional(Type.String({ minLength: 1, maxLength: 100 })),
      filter: Type.Optional(Type.String({ minLength: 1, maxLength: 200 })),
      range: Type.Optional(Type.String({ pattern: "^[0-9]+:[0-9]+$" })),
      limit: Type.Optional(Type.Integer({ minimum: 1, maximum: 500 })),
    }, { additionalProperties: false }),
    async execute(_toolCallId, params, signal) {
      return boundedToolResult(await client.getEvents({
        type: params.type,
        filter: params.filter,
        range: params.range,
        limit: params.limit,
      }, signal));
    },
  });

  pi.registerTool({
    name: "rdc_texture_stats",
    label: "RenderDoc Texture Stats",
    description: "Return per-channel min/max and an optional 256-bucket histogram for one texture resource. Use it to prove HDR overflow or clamping quantitatively instead of inferring it from a single pixel.",
    parameters: Type.Object({
      resourceId: Type.Integer({ minimum: 0 }),
      eid: Type.Optional(Type.Integer({ minimum: 0 })),
      mip: Type.Optional(Type.Integer({ minimum: 0 })),
      slice: Type.Optional(Type.Integer({ minimum: 0 })),
      histogram: Type.Optional(Type.Boolean()),
    }, { additionalProperties: false }),
    async execute(_toolCallId, params, signal) {
      return boundedToolResult(await client.getTextureStats(params.resourceId, {
        eid: params.eid,
        mip: params.mip,
        slice: params.slice,
        histogram: params.histogram,
      }, signal));
    },
  });

  pi.registerTool({
    name: "rdc_pick_pixel",
    label: "RenderDoc Pick Pixel",
    description: "Read the resolved RGBA value of one texel of the color target bound at one EID. Unlike rdc_pixel_history this returns the final value instead of the per-draw modification list. Requires a raster color target: dispatch-only EIDs are rejected, so use rdc_export_texture for compute outputs.",
    parameters: Type.Object({
      x: Type.Integer({ minimum: 0 }),
      y: Type.Integer({ minimum: 0 }),
      eid: Type.Optional(Type.Integer({ minimum: 0 })),
      target: Type.Optional(Type.Integer({ minimum: 0 })),
    }, { additionalProperties: false }),
    async execute(_toolCallId, params, signal) {
      return boundedToolResult(await client.getPickPixel(params.x, params.y, {
        eid: params.eid,
        target: params.target,
      }, signal));
    },
  });

  pi.registerTool({
    name: "rdc_export_texture",
    label: "Export RenderDoc Texture",
    description: "Export one texture resource to a PNG file. This is the only way to inspect the pixels of a compute output such as the FlaxGI debug atlas, because rdc_pick_pixel and rdc_pixel_history both require a raster color target at the EID. Combine with rdc_texture_stats: use stats to find the saturated resource, then export it to see where.",
    parameters: Type.Object({
      resourceId: Type.Integer({ minimum: 0 }),
      output: Type.String({ minLength: 1 }),
      mip: Type.Optional(Type.Integer({ minimum: 0 })),
    }, { additionalProperties: false }),
    async execute(_toolCallId, params, signal) {
      return boundedToolResult(await client.exportTexture(params.resourceId, {
        output: params.output,
        mip: params.mip,
      }, signal));
    },
  });

  pi.registerTool({
    name: "rdc_constants",
    label: "RenderDoc Shader Constants",
    description: "Read constant-buffer values at one EID by stage, descriptor set, and binding. Prefer this over the rdc_inspect_draw constants section when that aggregate reports not found.",
    parameters: Type.Object({
      eid: Type.Integer({ minimum: 0 }),
      stage: Type.Optional(Type.Union([
        Type.Literal("vs"), Type.Literal("hs"), Type.Literal("ds"),
        Type.Literal("gs"), Type.Literal("ps"), Type.Literal("cs"),
      ])),
      set: Type.Optional(Type.Integer({ minimum: 0 })),
      binding: Type.Optional(Type.Integer({ minimum: 0 })),
    }, { additionalProperties: false }),
    async execute(_toolCallId, params, signal) {
      return boundedToolResult(await client.getConstantBuffer(params.eid, {
        stage: params.stage,
        set: params.set,
        binding: params.binding,
      }, signal));
    },
  });

  pi.registerTool({
    name: "rdc_shader",
    label: "RenderDoc Shader Data",
    description: "Return shader reflection, constant values, debug source, or disassembly for one EID and stage. Pass disasmTarget (spirv/dxil/glsl) to disassemble.",
    parameters: Type.Object({
      eid: Type.Integer({ minimum: 0 }),
      stage: Type.Union([
        Type.Literal("vs"), Type.Literal("hs"), Type.Literal("ds"),
        Type.Literal("gs"), Type.Literal("ps"), Type.Literal("cs"),
      ]),
      constants: Type.Optional(Type.Boolean()),
      source: Type.Optional(Type.Boolean()),
      disasmTarget: Type.Optional(Type.String({ minLength: 1, maxLength: 40 })),
      reflect: Type.Optional(Type.Boolean()),
    }, { additionalProperties: false }),
    async execute(_toolCallId, params, signal) {
      return boundedToolResult(await client.getShader(params.eid, params.stage, {
        constants: params.constants,
        source: params.source,
        disasmTarget: params.disasmTarget,
        reflect: params.reflect,
      }, signal));
    },
  });

  pi.registerTool({
    name: "rdc_diff",
    label: "Diff RenderDoc Captures",
    description: "Compare before/after captures for bounded draw, pass, and framebuffer changes.",
    parameters: Type.Object({
      before: Type.String({ minLength: 1 }),
      after: Type.String({ minLength: 1 }),
      threshold: Type.Optional(Type.Number({ minimum: 0 })),
    }, { additionalProperties: false }),
    async execute(_toolCallId, params, signal) {
      const diff = await client.diffCaptures(params.before, params.after, {
        draws: true,
        passes: true,
        framebuffer: true,
        threshold: params.threshold,
      }, signal);
      return boundedToolResult(diff);
    },
  });

  pi.registerTool({
    name: "rdc_assert_pixel",
    label: "Assert RenderDoc Pixel",
    description: "Assert RGBA at one EID/pixel. Returns passed=false only for assertion exit code 1; replay/tool errors remain errors.",
    parameters: Type.Object({
      eid: Type.Integer({ minimum: 0 }),
      x: Type.Integer({ minimum: 0 }),
      y: Type.Integer({ minimum: 0 }),
      expected: Type.Tuple([
        Type.Number(), Type.Number(), Type.Number(), Type.Number(),
      ]),
      tolerance: Type.Optional(Type.Number({ minimum: 0 })),
      target: Type.Optional(Type.Integer({ minimum: 0 })),
    }, { additionalProperties: false }),
    async execute(_toolCallId, params, signal) {
      const assertion = await client.assertPixel({
        eid: params.eid,
        x: params.x,
        y: params.y,
        expected: params.expected,
        tolerance: params.tolerance,
        target: params.target,
      }, signal);
      return boundedToolResult(assertion);
    },
  });

  pi.registerTool({
    name: "rdc_capture",
    label: "Capture Executable with RenderDoc",
    description: "Run an executable through typed rdc capture and write a new .rdc file. Executable and app args remain separate process arguments.",
    parameters: Type.Object({
      executable: Type.String({ minLength: 1 }),
      args: Type.Optional(Type.Array(Type.String(), { maxItems: 128 })),
      output: Type.String({ minLength: 1 }),
      frame: Type.Optional(Type.Integer({ minimum: 0 })),
      timeoutSeconds: Type.Optional(Type.Number({ exclusiveMinimum: 0, maximum: 600 })),
      api: Type.Optional(Type.String({ minLength: 1, maxLength: 32 })),
      apiValidation: Type.Optional(Type.Boolean()),
      refAllResources: Type.Optional(Type.Boolean()),
    }, { additionalProperties: false }),
    async execute(_toolCallId, params, signal) {
      const capture = await client.captureExecutable({
        executable: params.executable,
        appArgs: params.args,
        output: params.output,
        frame: params.frame,
        timeoutSeconds: params.timeoutSeconds,
        api: params.api,
        apiValidation: params.apiValidation,
        refAllResources: params.refAllResources,
      }, signal);
      return boundedToolResult(capture);
    },
  });

  pi.registerTool({
    name: "rdc_close",
    label: "Close RenderDoc Capture",
    description: "Close the typed RenderDoc replay session and release GPU resources.",
    parameters: Type.Object({}, { additionalProperties: false }),
    async execute(_toolCallId, _params, signal) {
      const previous = client.getSessionState();
      await client.closeCapture(signal, true);
      return boundedToolResult({ status: "closed", capture: previous.capturePath });
    },
  });
}

export default function renderDocExtension(pi: ExtensionAPI): void {
  registerRenderDocExtension(pi);
}
