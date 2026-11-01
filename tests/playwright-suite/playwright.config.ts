// Playwright's own tests (tests/page, tests/library of the pinned tag under
// .cache) against zebkit's endpoint or against Chrome. scripts/playwright-suite.sh
// sets the environment; this file imports nothing, since it lives outside
// Playwright's tree.
//
// mode 'driver' is Playwright's own mode for a client that is not in the
// server's process: its fixtures skip what reaches into the server objects
// (toImpl) and launchPersistent and startRemoteServer, as they do upstream.
const repo = process.env.PW_SUITE_REPO!;
const out = process.env.PW_SUITE_OUT!;
const basePort = Number(process.env.PW_SUITE_BASE_PORT || 0);
if (basePort) {
  // Each worker has an engine of its own, so a crash costs one worker's test.
  // The worker's fixtures read this (connectOptionsFromEnv) in the worker. The
  // url has the engine's token: engines.mjs keeps it in a file while the engine
  // lives, and a worker started after a crash waits for the next engine's.
  const file = `${out}/engine-${Number(process.env.TEST_PARALLEL_INDEX || 0)}.url`;
  const fs = process.getBuiltinModule('node:fs');
  const sleeper = new Int32Array(new SharedArrayBuffer(4));
  for (let waited = 0; waited < 300 && !fs.existsSync(file); ++waited)
    Atomics.wait(sleeper, 0, 0, 100);
  process.env.PW_TEST_CONNECT_WS_ENDPOINT = fs.readFileSync(file, 'utf8').trim();
}
process.env.PWTEST_UNDER_TEST = '1';
const executablePath = process.env.PW_SUITE_CHROME || undefined;

const use: any = {
  mode: 'driver',
  browserName: 'chromium',
  // Fixtures treat "no channel, headless" as the headless shell. Neither
  // browser here is: Chrome runs --headless=new and zebkit is the full engine.
  channel: 'chromium',
  headless: true,
  launchOptions: { executablePath, args: ['--no-sandbox'] },
};
const metadata = { platform: process.platform, headless: 'headless', browserName: 'chromium', channel: 'chromium', mode: 'driver' };
const snapshotPathTemplate = '{testDir}/{testFileDir}/{testFileName}-snapshots/{arg}-chromium{ext}';
const testIgnore = [/firefox/, /webkit/];

export default {
  outputDir: `${out}/test-results`,
  expect: { timeout: 10000 },
  timeout: Number(process.env.PW_SUITE_TIMEOUT || 30000),
  globalTimeout: Number(process.env.PW_SUITE_GLOBAL_TIMEOUT || 7200000),
  workers: Number(process.env.PW_SUITE_WORKERS || 4),
  fullyParallel: true,
  retries: 0,
  reporter: [[`${process.env.PW_SUITE_DIR}/reporter.mjs`, { output: `${out}/results.jsonl` }]],
  projects: [
    { name: 'page', testDir: `${repo}/tests/page`, testIgnore, snapshotPathTemplate, use, metadata },
    { name: 'library', testDir: `${repo}/tests/library`, testIgnore, snapshotPathTemplate, use, metadata },
  ],
};
