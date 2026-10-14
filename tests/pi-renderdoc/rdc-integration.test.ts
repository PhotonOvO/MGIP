import assert from "node:assert/strict";
import test from "node:test";

import { RdcClient } from "../../src/rdc/client.ts";
import { inspectDraw } from "../../src/rdc/inspect-draw.ts";

const capture = "captures/rhi-public-contract-refactor/post-refactor/d3d12-marker-check/"
  + "csm-translate-stop__no-post__no-ddgi/first-still.rdc";

test("typed client opens, queries, and closes a real capture", async () => {
  const staleProbe = new RdcClient({ sessionName: "pi-v01-shapes" });
  await staleProbe.closeCapture(undefined, true);

  const client = new RdcClient({ sessionName: "pi-v01-client-integration" });
  try {
    const opened = await client.openCapture(capture);
    assert.equal(opened.info.API, "D3D12");
    assert.ok(Number(opened.info.Events) > 0);
    const draws = await client.getDraws({ limit: 3 });
    assert.ok(Array.isArray(draws));
    assert.ok(draws.length > 0 && draws.length <= 3);
    assert.ok(draws.every((draw) => Number.isSafeInteger(draw.eid)));
    const eid = draws[0]?.eid;
    assert.ok(eid !== undefined);
    const inspection = await inspectDraw(client, eid);
    assert.equal(inspection.eid, eid);
    assert.ok(inspection.draw.marker);
    assert.ok(inspection.pipeline.viewport);
    assert.ok(inspection.pipeline.blend);
    assert.ok(inspection.shaders.pixel);
    const serializedBytes = Buffer.byteLength(JSON.stringify(inspection), "utf8");
    assert.ok(serializedBytes < 100_000, `inspection is too large: ${serializedBytes} bytes`);
    console.log(JSON.stringify({
      api: opened.info.API,
      events: opened.info.Events,
      draws,
      inspectedEid: inspection.eid,
      marker: inspection.draw.marker,
      pass: inspection.pass?.name,
      pipelineSections: Object.entries(inspection.pipeline)
        .filter(([, value]) => value !== undefined)
        .map(([key]) => key),
      shaderStages: Object.keys(inspection.shaders),
      constantStages: Object.keys(inspection.constants),
      warnings: inspection.warnings,
      serializedBytes,
    }, null, 2));
  } finally {
    await client.closeCapture(undefined, true);
  }
});
