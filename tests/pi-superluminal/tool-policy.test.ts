import assert from "node:assert/strict";
import test from "node:test";

import {
  boundedToolResult,
  compactSlpValue,
  isContextFloodSlpCommand,
  MAX_SLP_TOOL_OUTPUT_BYTES,
} from "../../src/slp/tool-policy.ts";

test("dump-json invocations are blocked across wrappers and chaining", () => {
  const blocked = [
    "python3 slp.py dump-json trace.slp functioncache.dat",
    "python slp.py dump-json /mnt/f/Captures/x.slp 2026.session",
    "cd /mnt/f && python3 slp.py dump-json x .session",
    "echo hi && python3 slp.py dump-json x y",
    "python3 slp.py dump-json x y | head",
    'python3 "slp.py" dump-json x y',
    "python3 slp.py dump-json", // bare form: still blocked for consistency (it is a usage error anyway)
  ];
  for (const command of blocked) {
    assert.equal(isContextFloodSlpCommand(command), true, command);
  }
});

test("bounded slp.py subcommands and non-slp commands are allowed", () => {
  const allowed = [
    "python3 slp.py hotspots x.slp --thread Main",
    "python3 slp.py stats x.slp",
    "python3 slp.py source x.slp drawFrame",
    "rg -n dump-json src",
    "cat notes.md",
    "echo slp.py",
  ];
  for (const command of allowed) {
    assert.equal(isContextFloodSlpCommand(command), false, command);
  }
});

test("compactSlpValue caps arrays, keys, strings, and depth", () => {
  const value = {
    rows: Array.from({ length: 200 }, (_, i) => i),
    wide: Object.fromEntries(Array.from({ length: 100 }, (_, i) => [`k${i}`, i])),
    long: "x".repeat(5000),
    deep: { a: { b: { c: { d: { e: { f: { g: { h: "bottom" } } } } } } } },
  };
  const compacted = compactSlpValue(value, { maxArray: 16, maxKeys: 24, maxString: 512 }) as Record<string, unknown>;
  const rows = compacted.rows as unknown[];
  assert.equal(rows.length, 17); // 16 items + truncation marker
  assert.match(String(rows[16]), /truncated 184 items/);
  assert.ok((compacted.wide as Record<string, unknown>)._truncatedKeys === 76);
  assert.ok((compacted.long as string).length < 5000);
});

test("compactSlpValue stops descending at the depth limit", () => {
  // 8 levels of nesting: depth >= 7 short-circuits to a marker string.
  let deep: unknown = "bottom";
  for (let i = 0; i < 8; i += 1) deep = { [`l${i}`]: deep };
  const compacted = compactSlpValue(deep);
  assert.ok(JSON.stringify(compacted).includes("[depth limit]"));
});

test("boundedToolResult stays under the byte cap even for huge input", () => {
  // Default compaction caps arrays at 64, so strings must be long enough that
  // even 64 of them exceed the byte cap; this exercises the escalation path.
  const result = boundedToolResult({
    rows: Array.from({ length: 5_000 }, (_, i) => ({ i, fn: "module!func".repeat(400) })),
  });
  assert.equal(result.details.truncated, true);
  assert.ok(result.details.originalBytes > MAX_SLP_TOOL_OUTPUT_BYTES);
  assert.ok(result.details.bytes <= MAX_SLP_TOOL_OUTPUT_BYTES);
  assert.ok(Buffer.byteLength(result.content[0].text, "utf8") <= MAX_SLP_TOOL_OUTPUT_BYTES);
});

test("boundedToolResult passes small values through untruncated", () => {
  const result = boundedToolResult({ trace: "x.slp", rows: [{ function: "a!b", self_pct: 1 }] });
  assert.equal(result.details.truncated, false);
  const parsed = JSON.parse(result.content[0].text) as { trace: string };
  assert.equal(parsed.trace, "x.slp");
});
