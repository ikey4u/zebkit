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
import http from 'node:http';
import { gzipSync } from 'node:zlib';

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
// The status of a WebSocket upgrade: 101 when the endpoint takes it.
function upgrade(url, headers = {}) {
  return new Promise((resolve, reject) => {
    const request = http.request(url.replace(/^ws:/, 'http:'), {
      headers: { Connection: 'Upgrade', Upgrade: 'websocket', 'Sec-WebSocket-Version': '13',
                 'Sec-WebSocket-Key': 'dGhlIHNhbXBsZSBub25jZQ==', ...headers },
    });
    request.on('upgrade', (response, socket) => { socket.destroy(); resolve(response.statusCode); });
    request.on('response', response => { response.resume(); request.destroy(); resolve(response.statusCode); });
    request.on('error', reject);
    request.end();
  });
}
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

// Open to no web page, and only to who has the url (automation_server.cc).
await check('the endpoint refuses a request with an Origin header', async () => {
  equal(await upgrade(endpoint, { Origin: 'https://evil.example' }), 403);
  equal(await upgrade(endpoint, { Origin: 'null' }), 403);
});
await check('the endpoint refuses a path without its token', async () => {
  const url = new URL(endpoint);
  if (url.pathname.length < 33)
    throw new Error(`no token in ${endpoint}`);
  equal(await upgrade(`ws://${url.host}/`), 403);
  equal(await upgrade(`ws://${url.host}${url.pathname}x`), 403);
});
await check('the endpoint takes its own url', async () => equal(await upgrade(endpoint), 101));

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
await check('permission grants and resets are isolated by browser context', async () => {
  const left = await browser.newContext(), right = await browser.newContext();
  try {
    const a = await left.newPage(), b = await right.newPage();
    for (const target of [a, b]) {
      await target.route('https://permissions.test/**', route => route.fulfill({contentType:'text/html', body:'<!doctype html>permissions'}));
      await target.goto('https://permissions.test/');
    }
    const status = target => target.evaluate(async () => (await navigator.permissions.query({name:'geolocation'})).state);
    equal(await status(a), 'prompt');
    equal(await status(b), 'prompt');
    await a.evaluate(async () => {
      window.permissionEvents = [];
      window.permissionStatus = await navigator.permissions.query({name:'geolocation'});
      permissionStatus.onchange = () => permissionEvents.push(permissionStatus.state);
    });
    const workerStatus = target => target.evaluate(() => new Promise((resolve, reject) => {
      const url = URL.createObjectURL(new Blob([`navigator.permissions.query({name:'geolocation'}).then(p=>postMessage(p.state))`], {type:'text/javascript'}));
      const worker = new Worker(url);
      worker.onmessage = event => { worker.terminate(); URL.revokeObjectURL(url); resolve(event.data); };
      worker.onerror = error => { worker.terminate(); URL.revokeObjectURL(url); reject(new Error(error.message)); };
    }));
    await left.grantPermissions(['geolocation'], {origin:'https://permissions.test'});
    equal(await status(a), 'granted');
    equal(await status(b), 'prompt');
    equal(await workerStatus(a), 'granted');
    equal(await workerStatus(b), 'prompt');
    await a.waitForFunction(() => permissionEvents.includes('granted'), null, {timeout:3000});
    await right.grantPermissions(['geolocation'], {origin:'https://permissions.test'});
    await left.clearPermissions();
    equal(await status(a), 'prompt');
    equal(await status(b), 'granted');
    equal(await workerStatus(a), 'prompt');
    equal(await workerStatus(b), 'granted');
    await a.waitForFunction(() => permissionEvents.includes('prompt'), null, {timeout:3000});
  } finally {
    await left.clearPermissions();
    await right.clearPermissions();
    await Promise.all([left.close(), right.close()]);
  }
});
await check('initial context permissions are applied and isolated', async () => {
  const granted = await browser.newContext({permissions:['geolocation']}), untouched = await browser.newContext();
  try {
    for (const [context, expected] of [[granted, 'granted'], [untouched, 'prompt']]) {
      const target = await context.newPage();
      await target.route('https://initial-permissions.test/**', route => route.fulfill({contentType:'text/html',body:'permissions'}));
      await target.goto('https://initial-permissions.test/');
      equal(await target.evaluate(async () => (await navigator.permissions.query({name:'geolocation'})).state), expected);
    }
  } finally { await Promise.all([granted.close(), untouched.close()]); }
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
await check('Disposable removes page init script independently', async () => {
  const temporary = await context.newPage();
  try {
    const first = await temporary._channel.addInitScript({ source: 'globalThis.removedInit = 1' });
    const second = await temporary._channel.addInitScript({ source: 'globalThis.keptInit = 2' });
    await first.disposable.dispose({});
    await temporary.goto('data:text/html,<title>disposable</title>');
    equal(await temporary.evaluate(() => [typeof removedInit, keptInit]), ['undefined', 2]);
    await second.disposable.dispose({});
  } finally { await temporary.close(); }
});
await check('Disposable removes context init script from current and future pages', async () => {
  const isolated = await browser.newContext();
  try {
    const result = await isolated._channel.addInitScript({ source: 'globalThis.contextInit = true' });
    const existing = await isolated.newPage();
    await existing.goto('data:text/html,first');
    equal(await existing.evaluate(() => contextInit), true);
    await result.disposable.dispose({});
    await existing.goto('data:text/html,second');
    equal(await existing.evaluate(() => typeof contextInit), 'undefined');
    const future = await isolated.newPage();
    await future.goto('data:text/html,third');
    equal(await future.evaluate(() => typeof contextInit), 'undefined');
  } finally { await isolated.close(); }
});
await check('Disposable removes binding now and after navigation', async () => {
  const temporary = await context.newPage();
  try {
    const result = await temporary._channel.exposeBinding({ name: 'removedBinding', needsHandle: false });
    equal(await temporary.evaluate(() => { globalThis.savedBinding = removedBinding; return typeof removedBinding; }), 'function');
    await result.disposable.dispose({});
    equal(await temporary.evaluate(() => savedBinding().then(() => 'resolved', e => e.message)), 'binding was disposed');
    equal(await temporary.evaluate(() => typeof removedBinding), 'undefined');
    const replacement = await temporary._channel.exposeBinding({ name: 'removedBinding', needsHandle: false });
    equal(await temporary.evaluate(() => typeof removedBinding), 'function');
    equal(await temporary.evaluate(() => savedBinding().then(() => 'resolved', e => e.message)), 'binding was disposed');
    await replacement.disposable.dispose({});
    await temporary.goto('data:text/html,binding');
    equal(await temporary.evaluate(() => typeof removedBinding), 'undefined');
  } finally { await temporary.close(); }
});
await check('Disposable removes context binding from existing and future pages', async () => {
  const isolated = await browser.newContext();
  try {
    const result = await isolated._channel.exposeBinding({ name: 'contextBinding', needsHandle: false });
    const existing = await isolated.newPage();
    await existing.goto('data:text/html,first');
    equal(await existing.evaluate(() => typeof contextBinding), 'function');
    await result.disposable.dispose({});
    equal(await existing.evaluate(() => typeof contextBinding), 'undefined');
    await existing.goto('data:text/html,second');
    equal(await existing.evaluate(() => typeof contextBinding), 'undefined');
    const future = await isolated.newPage();
    await future.goto('data:text/html,third');
    equal(await future.evaluate(() => typeof contextBinding), 'undefined');
  } finally { await isolated.close(); }
});
await check('failed context binding registration does not survive disposal', async () => {
  const isolated = await browser.newContext();
  try {
    const original = await isolated._channel.exposeBinding({ name: 'duplicateBinding', needsHandle: false });
    const existing = await isolated.newPage();
    let rejected = false;
    try { await isolated._channel.exposeBinding({ name: 'duplicateBinding', needsHandle: false }); }
    catch { rejected = true; }
    equal(rejected, true);
    await original.disposable.dispose({});
    equal(await existing.evaluate(() => typeof duplicateBinding), 'undefined');
    const future = await isolated.newPage();
    await future.goto('data:text/html,duplicate');
    equal(await future.evaluate(() => typeof duplicateBinding), 'undefined');
  } finally { await isolated.close(); }
});
await check('context binding conflicts are atomic with no pages and page bindings', async () => {
  const isolated = await browser.newContext();
  try {
    const original = await isolated._channel.exposeBinding({ name: 'emptyDuplicate', needsHandle: false });
    let rejected = false;
    try { await isolated._channel.exposeBinding({ name: 'emptyDuplicate', needsHandle: false }); }
    catch { rejected = true; }
    equal(rejected, true);
    await original.disposable.dispose({});
    const existing = await isolated.newPage();
    const own = await existing._channel.exposeBinding({ name: 'pageOnlyBinding', needsHandle: false });
    rejected = false;
    try { await isolated._channel.exposeBinding({ name: 'pageOnlyBinding', needsHandle: false }); }
    catch { rejected = true; }
    equal(rejected, true);
    const future = await isolated.newPage();
    equal(await future.evaluate(() => [typeof pageOnlyBinding, typeof emptyDuplicate]), ['undefined', 'undefined']);
    await own.disposable.dispose({});
  } finally { await isolated.close(); }
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
  try {
    await page.locator('#file').setInputFiles(file);
    equal(await page.locator('#file').evaluate(e => e.files[0].name), path.basename(file));
  } finally {
    fs.rmSync(file, { force: true });
  }
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
  try {
    await context.tracing.start({ screenshots: true });
    await page.locator('#go').click();
    await context.tracing.stop({ path: file });
    equal(fs.readFileSync(file).subarray(0, 2).toString(), 'PK');
  } finally {
    fs.rmSync(file, { force: true });
  }
});
await check('real network timing and encoded transfer sizes', async () => {
  const body = '<!doctype html><title>Network metrics</title>' + 'measured '.repeat(2000);
  const compressed = gzipSync(body);
  const server = http.createServer((request, response) => {
    setTimeout(() => {
      response.writeHead(200, { 'Content-Type': 'text/html', 'Content-Encoding': 'gzip',
        'Content-Length': compressed.length, 'Cache-Control': 'no-store' });
      response.flushHeaders();
      setTimeout(() => response.end(compressed), 40);
    }, 30);
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const isolated = await browser.newContext();
  try {
    const temporary = await isolated.newPage();
    const url = `http://127.0.0.1:${server.address().port}/metrics`;
    const finished = temporary.waitForEvent('requestfinished', r => r.url() === url);
    const response = await temporary.goto(url);
    const request = await finished;
    equal(await request.response() === response, true);
    equal(await temporary.title(), 'Network metrics');
    equal(await response.text(), body);
    const timing = request.timing();
    if (timing.startTime < 1000000000000 || timing.requestStart < 0 ||
        timing.responseStart < timing.requestStart || timing.responseEnd < timing.responseStart + 20)
      throw new Error(`invalid network timing: ${JSON.stringify(timing)}`);
    const sizes = await request.sizes();
    equal(sizes.responseBodySize, compressed.length);
    if (sizes.requestHeadersSize <= 0 || sizes.responseHeadersSize <= 0)
      throw new Error(`missing transfer sizes: ${JSON.stringify(sizes)}`);
    equal(sizes.requestBodySize, 0);
    const payload = 'upload metrics '.repeat(300);
    const posted = temporary.waitForEvent('requestfinished', r => r.method() === 'POST');
    await temporary.evaluate(async data => {
      const result = await fetch('/upload', { method: 'POST', body: data });
      await result.arrayBuffer();
    }, payload);
    const postRequest = await posted;
    const postSizes = await postRequest.sizes();
    equal(postSizes.requestBodySize, Buffer.byteLength(payload));
    equal(postSizes.responseBodySize, compressed.length);

  } finally {
    await isolated.close();
    await new Promise(resolve => server.close(resolve));
  }
});
await check('navigator.webdriver is true on a page a client drives', async () => {
  equal(await page.evaluate(() => navigator.webdriver), true);
});
await check('console.error("Uncaught x") is a console message, a throw a page error', async () => {
  const errors = [], messages = [];
  const onError = error => errors.push(error.message), onConsole = message => messages.push(message.text());
  page.on('pageerror', onError);
  page.on('console', onConsole);
  await page.evaluate(() => console.error('Uncaught forged'));
  await page.evaluate(() => { setTimeout(() => { throw new Error('real'); }); });
  await page.waitForTimeout(300);
  page.off('pageerror', onError);
  page.off('console', onConsole);
  equal(errors, ['real']);
  equal(messages.filter(text => text.includes('forged')), ['Uncaught forged']);
});
await check('a frame in a shadow root: its element, and clicks into it', async () => {
  const framed = await context.newPage();
  await framed.setContent('<div id=host></div>');
  await framed.evaluate(() => {
    const shadow = document.getElementById('host').attachShadow({ mode: 'closed' });
    shadow.innerHTML = `<iframe id=shadowed style="margin-left:200px" srcdoc="<button onclick='window.hit=1' style='width:100%;height:100px'>x</button>"></iframe>`;
    const light = document.createElement('iframe');
    light.id = 'light';
    light.srcdoc = '<p>light</p>';
    document.body.append(light);
  });
  await framed.waitForLoadState();
  const inner = framed.frames().find(f => f !== framed.mainFrame() && f.childFrames().length === 0 && !f.isDetached() && f === framed.frames()[1]);
  const element = await inner.frameElement();
  equal(await element.evaluate(e => e.id), 'shadowed');
  const other = await framed.frames()[2].frameElement();
  equal(await other.evaluate(e => e.id), 'light');
  await inner.locator('button').click();
  equal(await inner.evaluate(() => window.hit), 1);
  await framed.close();
});
await check('page.request has the context\'s baseURL and httpCredentials', async () => {
  const seen = [];
  const server = http.createServer((request, response) => {
    seen.push(request.headers.authorization || '');
    if (request.url === '/open') {
      response.end('open');
    } else if (request.headers.authorization) {
      response.end(request.headers.authorization);
    } else {
      response.writeHead(401, { 'WWW-Authenticate': 'Basic realm="r"' });
      response.end();
    }
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const base = `http://127.0.0.1:${server.address().port}`;
  const basic = `Basic ${Buffer.from('user:pass').toString('base64')}`;
  const made = [];
  try {
    const plain = await browser.newContext({ baseURL: base });
    made.push(plain);
    equal(await (await plain.request.get('/open')).text(), 'open');
    equal((await plain.request.get('/secret')).status(), 401);
    const asked = await browser.newContext({ baseURL: base, httpCredentials: { username: 'user', password: 'pass' } });
    made.push(asked);
    equal(await (await asked.request.get('/secret')).text(), basic);
    seen.length = 0;
    const always = await browser.newContext({ baseURL: base, httpCredentials: { username: 'user', password: 'pass', send: 'always' } });
    made.push(always);
    await always.request.get('/open');
    equal(seen, [basic]);
    const elsewhere = await browser.newContext({ baseURL: base, httpCredentials: { username: 'user', password: 'pass', origin: 'http://elsewhere.example' } });
    made.push(elsewhere);
    equal((await elsewhere.request.get('/secret')).status(), 401);
    // The request context's own tracing.
    await plain.request.storageState();
  } finally {
    for (const made_context of made)
      await made_context.close();
    await new Promise(resolve => server.close(resolve));
  }
});
await check('omitBackground does not stay after a screenshot that timed out', async () => {
  const shot = await context.newPage();
  await shot.setContent('<!doctype html><body></body>');
  // The second byte of IDAT's image type is not readable here: compare with a page that never had it.
  const before = await shot.screenshot();
  for (let timeout = 1; timeout < 40; timeout += 3)
    await shot.screenshot({ omitBackground: true, timeout }).catch(() => {});
  await shot.waitForTimeout(200);
  const after = await shot.screenshot();
  equal(after.equals(before), true);
  await shot.close();
});
await check('a screenshot right after a change of the page has the change', async () => {
  const shot = await context.newPage();
  await shot.setContent('<!doctype html><body style="background:rgb(1,2,3)"></body>');
  const first = await shot.screenshot();
  await shot.evaluate(() => { document.body.style.background = 'rgb(200,10,10)'; });
  const second = await shot.screenshot();
  equal(second.equals(first), false);
  await shot.evaluate(() => { document.body.style.background = 'rgb(1,2,3)'; });
  equal((await shot.screenshot()).equals(first), true);
  await shot.close();
});
await check('a file chooser of an input out of the document, and of one in a frame', async () => {
  const chooser = await context.newPage();
  // The input Blink names (Page.fileChooserOpened): a FileChooser without its
  // element would be refused by the client, which then drops the connection.
  const [detached] = await Promise.all([
    chooser.waitForEvent('filechooser', { timeout: 5000 }),
    chooser.evaluate(() => { const input = document.createElement('input'); input.type = 'file'; input.multiple = true; input.click(); }),
  ]);
  equal(detached.isMultiple(), true);
  equal(await detached.element().evaluate(e => e.isConnected), false);
  await chooser.setContent('<iframe srcdoc="<input type=file>"></iframe>');
  const frame = chooser.frames()[1];
  const [inFrame] = await Promise.all([
    chooser.waitForEvent('filechooser', { timeout: 5000 }),
    frame.click('input'),
  ]);
  equal(await (await inFrame.element().ownerFrame()) === frame, true);
  equal(chooser.isClosed(), false);
  await chooser.close();
});
await check('contexts closed and made again: profiles go, and addresses come back', async () => {
  for (let i = 0; i < 60; ++i) {
    const made = await browser.newContext();
    await (await made.newPage()).setContent('<p>x</p>');
    await made.close();
    if (i % 3 === 0)
      await new Promise(resolve => setTimeout(resolve, 150));
  }
});
await check('close', async () => {
  await page.close();
  await context.close();
  await browser.close();
});

engine.kill();
process.exit(failed ? 1 : 0);
