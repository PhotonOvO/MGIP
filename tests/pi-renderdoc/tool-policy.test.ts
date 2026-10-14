import assert from "node:assert/strict";
import test from "node:test";

import {
  boundedToolResult,
  isDirectRenderDocCommand,
  MAX_RDC_TOOL_OUTPUT_BYTES,
} from "../../src/rdc/tool-policy.ts";

test("direct RenderDoc shell invocations are blocked across common wrappers", () => {
  const blocked = [
    "rdc info --json",
    "RDC.EXE draws",
    "& 'C:\\Program Files\\RenderDoc\\renderdoccmd.exe' capture app.exe",
    "echo first && rdc pipeline 4",
    "cmd /c \"rdc info\"",
    "powershell -Command \"renderdoccmd.exe capture app.exe\"",
    "bash -lc 'rdc draws'",
    "python -m rdc info",
    "Start-Process rdc -ArgumentList info",
    "Write-Output $(rdc info)",
  ];
  for (const command of blocked) {
    assert.equal(isDirectRenderDocCommand(command), true, command);
  }
});

test("mentions of RenderDoc command names without execution remain allowed", () => {
  const allowed = [
    "echo rdc",
    "rg -n renderdoccmd src",
    "Get-Content docs/rdc-notes.md",
    "node scripts/check-rdc-policy.mjs",
    "Write-Output 'rdc info'",
  ];
  for (const command of allowed) {
    assert.equal(isDirectRenderDocCommand(command), false, command);
  }
});

test("tool output is bounded even for large raw values", () => {
  const result = boundedToolResult({
    rows: Array.from({ length: 2_000 }, (_, index) => ({ index, payload: "x".repeat(2_000) })),
  });
  assert.equal(result.details.truncated, true);
  assert.ok(result.details.originalBytes > MAX_RDC_TOOL_OUTPUT_BYTES);
  assert.ok(result.details.bytes <= MAX_RDC_TOOL_OUTPUT_BYTES);
  assert.ok(Buffer.byteLength(result.content[0].text, "utf8") <= MAX_RDC_TOOL_OUTPUT_BYTES);
});
