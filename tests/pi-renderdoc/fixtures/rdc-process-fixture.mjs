const args = process.argv.slice(2);
const sleepIndex = args.indexOf("--sleep-ms");
if (sleepIndex >= 0) {
  const milliseconds = Number(args[sleepIndex + 1]);
  await new Promise((resolve) => setTimeout(resolve, milliseconds));
}
const stderrIndex = args.indexOf("--stderr");
if (stderrIndex >= 0) process.stderr.write(args[stderrIndex + 1] ?? "fixture stderr");
const exitIndex = args.indexOf("--exit");
if (exitIndex >= 0) process.exitCode = Number(args[exitIndex + 1]);
process.stdout.write(JSON.stringify({ args }));
