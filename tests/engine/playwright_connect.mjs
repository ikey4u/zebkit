// Playwright's own client against zebkit's endpoint (zebkit_automation_listen,
// docs/AUTOMATION.md): `chromium.connect(url)`, then Playwright's API as a
// test writes it. Run by scripts/engine-test.sh:
//
//   node playwright_connect.mjs <playwright-core dir> <zebkit-run> [switches...]
//
// Prints "ok <name>" or "FAIL <name>: <error>" for each check, and exits 1
// when one failed.

import { spawn } from 'node:child_process';
import { createRequire } from 'node:module';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

const [coreDir, executable, ...switches] = process.argv.slice(2);
const require = createRequire(path.resolve(coreDir, 'package.json'));
const { chromium } = require('playwright-core');

const engine = spawn(executable, [...switches, '--playwright-port=0'], { stdio: ['ignore', 'ignore', 'pipe'] });
const endpoint = await new Promise((resolve, reject) => {
  let text = '';
  engine.stderr.on('data', data => {
    text += data;
    const found = text.match(/Playwright listening on (ws:\/\/\S+)/);
    if (found)
      resolve(found[1]);
  });
  engine.on('exit', code => reject(new Error(`zebkit-run exited (${code}): ${text}`)));
  setTimeout(() => reject(new Error('zebkit-run serves no Playwright endpoint')), 30000);
});

let failed = 0;
async function check(name, body) {
  try {
    await body();
    console.log(`ok ${name}`);
  } catch (error) {
    ++failed;
    console.log(`FAIL ${name}: ${String(error && error.message || error).split('\n')[0]}`);
  }
}
function equal(actual, expected) {
  const a = JSON.stringify(actual), e = JSON.stringify(expected);
  if (a !== e)
    throw new Error(`expected ${e}, got ${a}`);
}

const page_html = `<!doctype html><title>Connect</title>
<h1 id=out>start</h1>
<input id=name aria-label=Name>
<button id=go onclick="document.querySelector('#out').textContent = 'clicked ' + document.querySelector('#name').value">Go</button>
<input id=file type=file>
<ul>${Array.from({ length: 5 }, (_, i) => `<li class=item>item ${i}</li>`).join('')}</ul>
<iframe id=child srcdoc="<p id=inner>inside</p>"></iframe>`;

let browser, context, page;
await check('connect', async () => {
  browser = await chromium.connect(endpoint);
  if (!browser.version())
    throw new Error('no version');
});
await check('newContext and newPage', async () => {
  context = await browser.newContext({ viewport: { width: 800, height: 600 } });
  page = await context.newPage();
  equal(context.pages().length, 1);
});
await check('route.fulfill serves goto', async () => {
  await page.route('http://zebkit.test/**', route => route.fulfill({ contentType: 'text/html', body: page_html }));
  const response = await page.goto('http://zebkit.test/index.html');
  equal(response.status(), 200);
  equal(await page.title(), 'Connect');
  equal(page.url(), 'http://zebkit.test/index.html');
});
await check('evaluate', async () => {
  equal(await page.evaluate(() => 1 + 1), 2);
  equal(await page.evaluate(x => x.a + x.b.length, { a: 1, b: [1, 2] }), 3);
  equal(await page.evaluate(() => ({ s: 'x', n: null, list: [true] })), { s: 'x', n: null, list: [true] });
});
await check('evaluateHandle and handles as arguments', async () => {
  const handle = await page.evaluateHandle(() => document.querySelector('#out'));
  equal(await page.evaluate(e => e.id, handle), 'out');
  equal(await handle.evaluate(e => e.tagName), 'H1');
  await handle.dispose();
});
await check('locator fill, click, textContent', async () => {
  await page.locator('#name').fill('zebkit');
  await page.getByRole('button', { name: 'Go' }).click();
  equal(await page.locator('#out').textContent(), 'clicked zebkit');
});
await check('allTextContents and count', async () => {
  equal(await page.locator('.item').count(), 5);
  equal((await page.locator('.item').allTextContents())[4], 'item 4');
});
await check('keyboard', async () => {
  await page.locator('#name').fill('');
  await page.locator('#name').pressSequentially('ab');
  await page.keyboard.press('Backspace');
  equal(await page.locator('#name').inputValue(), 'a');
});
await check('frames', async () => {
  const frame = page.frameLocator('#child');
  equal(await frame.locator('#inner').textContent(), 'inside');
  equal(page.frames().length, 2);
});
await check('console event', async () => {
  const [message] = await Promise.all([
    page.waitForEvent('console'),
    page.evaluate(() => console.log('hello', 1)),
  ]);
  equal(message.text(), 'hello 1');
});
await check('dialog', async () => {
  page.once('dialog', dialog => dialog.accept('yes'));
  equal(await page.evaluate(() => prompt('Name?')), 'yes');
});
await check('exposeFunction', async () => {
  await page.exposeFunction('twice', x => x * 2);
  equal(await page.evaluate(() => window.twice(21)), 42);
});
await check('request and response events', async () => {
  const [request] = await Promise.all([
    page.waitForEvent('request', r => r.url().endsWith('/data.json')),
    page.evaluate(() => fetch('/data.json').catch(() => null)),
  ]);
  equal(request.method(), 'GET');
});
await check('setInputFiles', async () => {
  const file = path.join(os.tmpdir(), `zebkit-connect-${process.pid}.txt`);
  fs.writeFileSync(file, 'hello');
  await page.locator('#file').setInputFiles(file);
  equal(await page.locator('#file').evaluate(e => e.files[0].name), path.basename(file));
  fs.unlinkSync(file);
});
await check('waitForSelector', async () => {
  page.evaluate(() => setTimeout(() => document.body.append(Object.assign(document.createElement('p'), { id: 'late', textContent: 'late' })), 50));
  const element = await page.waitForSelector('#late');
  equal(await element.evaluate(e => e.id), 'late');
});
await check('cookies', async () => {
  await context.addCookies([{ name: 'a', value: '1', url: 'http://zebkit.test/' }]);
  const cookies = await context.cookies('http://zebkit.test/');
  equal(cookies.map(c => c.name), ['a']);
});
await check('screenshot', async () => {
  const png = await page.screenshot();
  equal([...png.subarray(1, 4)].map(c => String.fromCharCode(c)).join(''), 'PNG');
});
await check('tracing', async () => {
  const file = path.join(os.tmpdir(), `zebkit-connect-${process.pid}.zip`);
  await context.tracing.start({ screenshots: true });
  await page.locator('#go').click();
  await context.tracing.stop({ path: file });
  equal(fs.readFileSync(file).subarray(0, 2).toString(), 'PK');
  fs.unlinkSync(file);
});
await check('close', async () => {
  await page.close();
  await context.close();
  await browser.close();
});

engine.kill();
process.exit(failed ? 1 : 0);
