import test from "node:test";

// Fast unit suite (fakes only, no Python, no trace). Mirrors the RenderDoc
// split: integration tests that shell out to the real slp.py live in
// slp-integration.test.ts and pi-load.integration.test.ts and run separately
// (npm run test:slp:integration) because they need SLP_PY and a real trace.
import "./tool-policy.test.ts";
import "./process.test.ts";
import "./client.test.ts";
import "./extension.test.ts";

test("pi-superluminal unit suite loaded", () => {});
