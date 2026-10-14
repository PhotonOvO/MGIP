import assert from "node:assert/strict";
import test from "node:test";

import {
  compactRenderDocValue,
  inspectDraw,
  type RdcInspectionClient,
} from "../../src/rdc/inspect-draw.ts";
import type { RdcPipelineSection, RdcShaderStage } from "../../src/rdc/types.ts";

class InspectionFixtureClient implements RdcInspectionClient {
  failDescriptors = false;

  async getDraw(): Promise<unknown> {
    return { Event: 134, Type: "DrawIndexed(6)", Marker: "TexturedQuad/Main", Triangles: 2, Instances: 1 };
  }

  async getPipeline(): Promise<unknown> {
    return { eid: 134, api: "D3D11", topology: "TriangleList", graphics_pipeline: 91, compute_pipeline: 0 };
  }

  async getPipelineSection(_eid: number, section: RdcPipelineSection): Promise<unknown> {
    const sections: Record<string, unknown> = {
      viewport: { x: 0, y: 0, width: 1280, height: 720, minDepth: 0, maxDepth: 1 },
      scissor: { x: 0, y: 0, width: 1280, height: 720, enabled: true },
      blend: { blends: [{ rt: 0, enabled: false, writeMask: 15 }] },
      rasterizer: { fillMode: "Solid", cullMode: "Back", frontCCW: false },
      "depth-stencil": { depthTestEnable: false, depthWriteEnable: false },
    };
    return sections[section];
  }

  async getBindings(): Promise<unknown> {
    return [
      { eid: 134, stage: "ps", kind: "ro", set: 0, slot: 0, name: "BaseColorTexture" },
      { eid: 134, stage: "ps", kind: "sampler", set: 0, slot: 0, name: "LinearSampler" },
    ];
  }

  async getDescriptors(): Promise<unknown> {
    if (this.failDescriptors) throw new Error("descriptor query unavailable");
    return [{
      stage: "Pixel",
      binding: "BaseColorTexture",
      type: "ReadOnlyResource",
      resource_id: 7,
      resource_name: "DeterministicRed1x1",
      format: "R8G8B8A8_UNORM",
      width: 1,
      height: 1,
    }];
  }

  async getPasses(): Promise<{ passes: Array<Record<string, unknown>> }> {
    return { passes: [{ name: "TexturedQuad", begin_eid: 120, end_eid: 140, draws: 1 }] };
  }

  async getPass(): Promise<unknown> {
    return {
      name: "TexturedQuad",
      begin_eid: 120,
      end_eid: 140,
      color_targets: [{ id: 10, name: "BackBuffer", format: "R8G8B8A8_UNORM", width: 1280, height: 720 }],
      depth_target: null,
    };
  }

  async getShaders(): Promise<Array<{ stage: RdcShaderStage; shader: number; entry: string; ro: number; rw: number; cbuffers: number }>> {
    return [
      { stage: "vs", shader: 21, entry: "VSMain", ro: 0, rw: 0, cbuffers: 0 },
      { stage: "ps", shader: 22, entry: "PSMain", ro: 1, rw: 0, cbuffers: 1 },
    ];
  }

  async getShader(_eid: number, stage: RdcShaderStage): Promise<unknown> {
    return { eid: 134, stage, shader: stage === "vs" ? 21 : 22, reflection: { inputs: [], outputs: [] } };
  }

  async getShaderConstants(_eid: number, stage: RdcShaderStage): Promise<unknown> {
    return stage === "ps"
      ? { eid: 134, stage, constants: [{ name: "MaterialConstants", variables: [{ name: "BaseColorFactor", value: [0, 0, 0, 1] }] }] }
      : { eid: 134, stage, constants: [] };
  }

  async getShaderSource(_eid: number, stage: RdcShaderStage): Promise<unknown> {
    const source = stage === "ps"
      ? "Texture2D BaseColorTexture;\ncbuffer MaterialConstants { float4 BaseColorFactor; };\nreturn BaseColorTexture.Sample(LinearSampler, input.uv) * BaseColorFactor;"
      : "return float4(input.position, 1.0);";
    return { eid: 134, stage, has_debug_info: true, files: [{ filename: "textured_quad.hlsl", source }], source: "" };
  }
}

test("inspectDraw aggregates a bounded evidence-focused draw inspection", async () => {
  const result = await inspectDraw(new InspectionFixtureClient(), 134);
  assert.equal(result.draw.marker, "TexturedQuad/Main");
  assert.equal(result.pipeline.api, "D3D11");
  assert.deepEqual(result.pipeline.viewport, { x: 0, y: 0, width: 1280, height: 720, minDepth: 0, maxDepth: 1 });
  assert.deepEqual(result.pipeline.renderTargets, [{
    id: 10,
    name: "BackBuffer",
    format: "R8G8B8A8_UNORM",
    width: 1280,
    height: 720,
  }]);
  assert.match(JSON.stringify(result.bindings.descriptors), /DeterministicRed1x1/);
  assert.match(JSON.stringify(result.constants.pixel), /BaseColorFactor/);
  assert.deepEqual(result.constants.pixel, [{
    buffer: "MaterialConstants",
    bindPoint: undefined,
    name: "BaseColorFactor",
    type: undefined,
    rows: undefined,
    columns: undefined,
    value: [0, 0, 0, 1],
  }]);
  assert.match(result.shaders.pixel?.source?.relevantLines.join("\n") ?? "", /BaseColorFactor/);
  assert.equal(result.warnings.length, 0);
  assert.ok(JSON.stringify(result).length < 30_000);
});

test("inspectDraw preserves required data when an optional descriptor query fails", async () => {
  const client = new InspectionFixtureClient();
  client.failDescriptors = true;
  const result = await inspectDraw(client, 134);
  assert.equal(result.draw.event, 134);
  assert.match(result.warnings.join("\n"), /descriptor query unavailable/);
});

test("compactRenderDocValue limits arrays, strings, and object depth", () => {
  const value = compactRenderDocValue({
    long: "x".repeat(3_000),
    values: Array.from({ length: 100 }, (_, index) => index),
  }, { maxArray: 5, maxString: 20 });
  const serialized = JSON.stringify(value);
  assert.match(serialized, /truncated 95 items/);
  assert.match(serialized, /truncated 2980 chars/);
  assert.ok(serialized.length < 500);
});
