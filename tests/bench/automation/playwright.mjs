// Playwright's side of scripts/automation-bench.sh: the operations of
// docs/AUTOMATION.md's table through Playwright's client, against zebkit
// over CDP (connectOverCDP to tools/zebkit-run), zebkit's own Playwright
// endpoint (connect to tools/zebkit-run --playwright-port, no CDP), or
// Chrome over CDP (launched by Playwright).
//
//   node playwright.mjs <playwright-core dir> <page URL> <runs> zebkit <zebkit-run> [switches...]
//   node playwright.mjs <playwright-core dir> <page URL> <runs> zebkit-endpoint <zebkit-run> [switches...]
//   node playwright.mjs <playwright-core dir> <page URL> <runs> chrome <chrome executable>
//
// Prints one line of JSON: {engine, version, results: {name: {median, p90}}}.

import { spawn } from 'node:child_process';
import { createRequire } from 'node:module';
import path from 'node:path';

const [coreDir, url, runsText, engineName, executable, ...switches] = process.argv.slice(2);
const require = createRequire(path.join(coreDir, 'package.json'));
const { chromium } = require('playwright-core');
const runs = Number(runsText) || 50;

let browser, engine;
if (engineName === 'zebkit-endpoint') {
  engine = spawn(executable, [...switches, '--playwright-port=0'], { stdio: ['ignore', 'ignore', 'pipe'] });
  const ws = await new Promise((resolve, reject) => {
    let text = '';
    engine.stderr.on('data', data => {
      text += data;
      const found = text.match(/Playwright listening on (ws:\/\/\S+)/);
      if (found)
        resolve(found[1]);
    });
    setTimeout(() => reject(new Error('zebkit-run serves no Playwright endpoint')), 30000);
  });
  browser = await chromium.connect(ws);
} else if (engineName === 'zebkit') {
  engine = spawn(executable, [...switches, '--remote-debugging-port=0', 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] });
  const ws = await new Promise((resolve, reject) => {
    let text = '';
    engine.stderr.on('data', data => {
      text += data;
      const found = text.match(/ws:\/\/\S+/);
      if (found)
        resolve(found[0]);
    });
    setTimeout(() => reject(new Error('zebkit-run serves no CDP')), 30000);
  });
  browser = await chromium.connectOverCDP(ws);
} else {
  browser = await chromium.launch({ headless: true, executablePath: executable });
}
const context = browser.contexts()[0] || await browser.newContext();
const page = await context.newPage();
await page.setViewportSize({ width: 1280, height: 800 });

const operations = {
  'goto': () => page.goto(url),
  'evaluate 1+1': () => page.evaluate(() => 1 + 1),
  'locator.textContent': () => page.locator('#out').textContent(),
  'locator.click': () => page.locator('#go').click(),
  'locator.fill': () => page.locator('#name').fill('hello world'),
  'locator.press Enter': () => page.locator('#name').press('Enter'),
  'locator.isVisible': () => page.locator('#go').isVisible(),
  'locator.count (200)': () => page.locator('.item').count(),
  'allTextContents (200)': () => page.locator('.item').allTextContents(),
  'getByRole click': () => page.getByRole('button', { name: 'Go' }).click(),
  'screenshot': () => page.screenshot(),
};

await page.goto(url);
const results = {};
for (const [name, operation] of Object.entries(operations)) {
  const count = name === 'goto' || name === 'screenshot' ? Math.min(runs, 20) : runs;
  for (let i = 0; i < 3; ++i)
    await operation();
  const times = [];
  for (let i = 0; i < count; ++i) {
    const start = performance.now();
    await operation();
    times.push(performance.now() - start);
  }
  times.sort((a, b) => a - b);
  results[name] = { median: +times[count >> 1].toFixed(2), p90: +times[Math.floor(count * 0.9)].toFixed(2) };
}
console.log(JSON.stringify({ engine: { 'zebkit': 'zebkit over CDP', 'zebkit-endpoint': 'zebkit endpoint (Playwright protocol)' }[engineName] || 'Chrome over CDP', version: browser.version(), results }));
await browser.close().catch(() => {});
engine?.kill();
process.exit(0);
