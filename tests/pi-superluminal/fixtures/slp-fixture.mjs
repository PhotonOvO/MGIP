#!/usr/bin/env node
/**
 * Fixture standing in for slp.py in process-layer tests.
 *
 * Emits JSON on stdout like `slp.py <cmd> --json`, and can be told to exit with
 * a specific code (--exit N), write to stderr (--stderr TEXT), sleep
 * (--sleep-ms N), or emit a huge payload (--big N) to exercise output limits.
 * Mirrors tests/pi-renderdoc/fixtures/rdc-process-fixture.mjs.
 */
const args = process.argv.slice(2);

function flag(name) {
  const i = args.indexOf(name);
  return i >= 0 ? args[i + 1] : undefined;
}

const sleepMs = flag("--sleep-ms");
if (sleepMs !== undefined) {
  await new Promise((resolve) => setTimeout(resolve, Number(sleepMs)));
}

const stderr = flag("--stderr");
if (stderr !== undefined) process.stderr.write(stderr);

const big = flag("--big");
if (big !== undefined) {
  process.stdout.write(JSON.stringify({ rows: Array.from({ length: Number(big) }, (_, i) => ({ i, pad: "x".repeat(200) })) }));
} else {
  process.stdout.write(JSON.stringify({ args, ok: true }));
}

const exit = flag("--exit");
if (exit !== undefined) process.exitCode = Number(exit);
