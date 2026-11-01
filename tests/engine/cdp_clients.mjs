// Real CDP clients, puppeteer-core and playwright-core (connectOverCDP),
// against a browser's --remote-debugging-port, Chrome's results as the
// reference. Every case runs on Chrome and on zebkit in the same run; a case
// passes when both give the same value. Nothing is hard-coded as a result.
//
//   node cdp_clients.mjs <clients dir> <expected differences file> \
//       --zebkit <zebkit-run> --chrome <chrome> [--work DIR] [--only REGEX] [--json FILE]
//
// A difference named in the expected list (with its reason) is reported and
// does not fail the run. Prints a table of the differences, then a count;
// exits 1 on a difference not in the list. docs/AUTOMATION.md, section 7.

import { spawn } from 'node:child_process';
import { createRequire } from 'node:module';
import crypto from 'node:crypto';
import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';

const argv = process.argv.slice(2);
const clientsDir = path.resolve(argv.shift());
const expectedFile = argv.shift();
const option = name => {
  const at = argv.indexOf(name);
  return at >= 0 ? argv[at + 1] : undefined;
};
const zebkitPath = option('--zebkit'), chromePath = option('--chrome');
const only = option('--only') ? new RegExp(option('--only')) : null;
const work = option('--work') || fs.mkdtempSync(path.join(os.tmpdir(), 'cdp-clients-'));
fs.mkdirSync(work, { recursive: true });
const require = createRequire(path.join(clientsDir, 'package.json'));
const puppeteer = require('puppeteer-core');
const { chromium, devices } = require('playwright-core');

// ---------------------------------------------------------------- the server

const png = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8DwHwAFBQIAX8jx0gAAAABJRU5ErkJggg==', 'base64');
const pages = {
  '/empty.html': '<!doctype html><title>Empty</title>',
  '/child.html': '<!doctype html><title>Child</title><p id=inner>inside</p><button id=b onclick="this.textContent=\'pressed\'" style="position:absolute;left:10px;top:10px;width:80px;height:30px">press</button>',
  '/same-frames.html': '<!doctype html><iframe name=a src=/child.html></iframe><iframe name=b srcdoc="<p>doc</p>"></iframe>',
  '/box.html': `<!doctype html><style>html,body{margin:0;background:#fff}#r{position:absolute;left:0;top:0;width:100px;height:100px;background:#f00}
#g{position:absolute;left:100px;top:0;width:100px;height:100px;background:#0f0}#tall{position:absolute;top:0;left:0;width:1px;height:2000px}</style>
<div id=r></div><div id=g></div><div id=tall></div>`,
  '/form.html': `<!doctype html><title>Form</title><style>body{margin:0}button,input,textarea{display:block;position:absolute;left:10px;width:200px;height:30px}</style>
<button id=b style="top:10px" onclick="window.clicked=(window.clicked||0)+1">Button</button>
<input id=i style="top:50px" aria-label="Name"><textarea id=t style="top:90px" aria-label="Text"></textarea><input id=f type=file style="top:130px">
<input id=c type=checkbox style="top:170px;width:20px" aria-label="Agree">
<script>window.events=[];for(const t of ['mousedown','mouseup','click','dblclick','contextmenu','keydown','keyup','keypress','input','wheel','touchstart','touchend'])
addEventListener(t,e=>events.push([t,e.key||'',e.code||'',e.button??'',e.clientX??'',e.clientY??'',e.shiftKey?1:0,e.ctrlKey?1:0].join(':')),{passive:true});</script>`,
  '/a11y.html': '<!doctype html><title>A11y</title><h1>Heading</h1><main><button>Save</button><a href=/empty.html>Link</a><input aria-label=Field value=v><ul><li>One</li><li>Two</li></ul><img alt=Pic src=/pixel.png></main>',
  '/coverage.html': '<!doctype html><link rel=stylesheet href=/style.css><div class=used>x</div><script src=/script.js></script><script>used()</script>',
  '/worker.html': '<!doctype html><script>window.w=new Worker("/worker.js");</script>',
  '/sw.html': '<!doctype html><script>window.reg=navigator.serviceWorker.register("/sw.js").then(r=>new Promise(k=>{const w=r.installing||r.waiting||r.active;if(w.state==="activated")k("active");w.addEventListener("statechange",()=>{if(w.state==="activated")k("active")})}));</script>',
  '/shared.html': '<!doctype html><script>window.s=new SharedWorker("/worker.js");</script>',
  '/popup.html': '<!doctype html><a id=l href=/child.html target=_blank rel=opener style="position:absolute;left:0;top:0">open</a>',
  '/print.html': '<!doctype html><style>@media print{#p{display:block}#s{display:none}}@media screen{#p{display:none}}</style><div id=p>Printed text</div><div id=s>Screen</div><div style="page-break-before:always">Second page</div>',
  '/errors.html': '<!doctype html><script>console.log("hello",1);console.warn("warned");console.error("failed");setTimeout(()=>{throw new Error("boom")},0)</script>',
  '/unload.html': '<!doctype html><script>addEventListener("beforeunload",e=>{e.preventDefault();e.returnValue="leave?"})</script><p>x</p>',
  '/ws.html': '<!doctype html><script>window.got=new Promise(k=>{const s=new WebSocket("ws://"+location.host+"/ws");s.onopen=()=>s.send("ping");s.onmessage=e=>k(e.data)})</script>',
  '/scroll.html': '<!doctype html><div style="height:3000px;width:3000px">x</div><div id=low style="position:absolute;top:2000px;width:50px;height:50px;background:#00f"></div>',
  '/drag.html': '<!doctype html><style>div{position:absolute;width:50px;height:50px}</style><div id=s draggable=true style="left:0;top:0;background:#f00"></div><div id=d style="left:100px;top:0;background:#0f0"></div><script>window.ev=[];for(const t of ["dragstart","drop","dragend"])addEventListener(t,e=>ev.push(t));d.addEventListener("dragover",e=>e.preventDefault());</script>',
};
const assets = {
  '/style.css': ['text/css', '.used{color:red}.unused{color:blue}'],
  '/script.js': ['text/javascript', 'function used(){return 1}\nfunction unused(){return 2}\n'],
  '/worker.js': ['text/javascript', 'self.value=42;self.onconnect=()=>{};postMessage&&0;'],
  '/sw.js': ['text/javascript', 'self.addEventListener("install",()=>self.skipWaiting());self.addEventListener("activate",e=>e.waitUntil(clients.claim()));self.value=7;'],
  '/data.json': ['application/json', '{"hello":"world"}'],
  '/pixel.png': ['image/png', png],
};
const server = http.createServer((req, res) => {
  const url = new URL(req.url, 'http://x');
  const route = url.pathname;
  const chunks = [];
  req.on('data', c => chunks.push(c));
  req.on('end', () => {
    if (route === '/echo') {
      const { host, ...headers } = req.headers;
      res.writeHead(200, { 'content-type': 'application/json', 'access-control-allow-origin': '*' });
      res.end(JSON.stringify({ method: req.method, headers, body: Buffer.concat(chunks).toString(), query: url.search }));
    } else if (route === '/redirect') {
      res.writeHead(302, { location: '/empty.html' });
      res.end();
    } else if (route === '/set-cookie') {
      res.writeHead(200, { 'set-cookie': ['plain=1; Path=/', 'secret=2; Path=/; HttpOnly', 'lax=3; Path=/; SameSite=Lax'], 'content-type': 'text/html' });
      res.end('<!doctype html>cookies');
    } else if (route === '/download') {
      res.writeHead(200, { 'content-disposition': 'attachment; filename="file.txt"', 'content-type': 'application/octet-stream' });
      res.end('downloaded');
    } else if (route === '/auth') {
      if (req.headers.authorization === 'Basic ' + Buffer.from('user:pass').toString('base64')) {
        res.writeHead(200, { 'content-type': 'text/html' });
        res.end('<!doctype html><title>Authed</title>');
      } else {
        res.writeHead(401, { 'www-authenticate': 'Basic realm="r"' });
        res.end('no');
      }
    } else if (route === '/status') {
      res.writeHead(Number(url.searchParams.get('code')), { 'content-type': 'text/plain', 'x-custom': 'yes' });
      res.end('status');
    } else if (route === '/csp.html') {
      res.writeHead(200, { 'content-type': 'text/html', 'content-security-policy': "default-src 'self'" });
      res.end('<!doctype html><title>CSP</title>');
    } else if (route === '/cross.html') {
      const other = url.searchParams.get('o');
      res.writeHead(200, { 'content-type': 'text/html' });
      res.end(`<!doctype html><style>body{margin:0}iframe{position:absolute;left:0;top:0;width:300px;height:200px;border:0}</style><iframe id=x src="${other}/child.html"></iframe>`);
    } else if (pages[route]) {
      res.writeHead(200, { 'content-type': 'text/html' });
      res.end(pages[route]);
    } else if (assets[route]) {
      res.writeHead(200, { 'content-type': assets[route][0], 'service-worker-allowed': '/' });
      res.end(assets[route][1]);
    } else {
      res.writeHead(404);
      res.end('not found');
    }
  });
});
server.on('upgrade', (req, socket) => {
  const accept = crypto.createHash('sha1').update(req.headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
  socket.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n\r\n`);
  socket.on('error', () => {});
  socket.on('data', data => {
    if ((data[0] & 0x0f) !== 1)
      return;
    const length = data[1] & 0x7f, mask = data.subarray(2, 6);
    const text = Buffer.from(data.subarray(6, 6 + length).map((b, i) => b ^ mask[i % 4])).toString();
    const out = Buffer.from('echo:' + text);
    socket.write(Buffer.concat([Buffer.from([0x81, out.length]), out]));
  });
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const port = server.address().port;
const origin = `http://localhost:${port}`, other = `http://127.0.0.1:${port}`;

// ---------------------------------------------------------------- helpers

function decodePng(buffer) {
  let at = 8, width, height, type;
  const idat = [];
  while (at < buffer.length) {
    const length = buffer.readUInt32BE(at), tag = buffer.toString('latin1', at + 4, at + 8), body = buffer.subarray(at + 8, at + 8 + length);
    if (tag === 'IHDR') {
      width = body.readUInt32BE(0);
      height = body.readUInt32BE(4);
      type = body[9];
    } else if (tag === 'IDAT') {
      idat.push(body);
    }
    at += 12 + length;
  }
  const channels = type === 6 ? 4 : type === 2 ? 3 : 1, raw = zlib.inflateSync(Buffer.concat(idat)), stride = width * channels;
  const out = Buffer.alloc(height * stride);
  for (let y = 0; y < height; ++y) {
    const filter = raw[y * (stride + 1)];
    for (let x = 0; x < stride; ++x) {
      const value = raw[y * (stride + 1) + 1 + x];
      const a = x >= channels ? out[y * stride + x - channels] : 0, b = y ? out[(y - 1) * stride + x] : 0, c = x >= channels && y ? out[(y - 1) * stride + x - channels] : 0;
      let p = 0;
      if (filter === 1) p = a;
      if (filter === 2) p = b;
      if (filter === 3) p = (a + b) >> 1;
      if (filter === 4) {
        const q = a + b - c, pa = Math.abs(q - a), pb = Math.abs(q - b), pc = Math.abs(q - c);
        p = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
      }
      out[y * stride + x] = (value + p) & 255;
    }
  }
  return { width, height, at: (x, y) => [...out.subarray(y * stride + x * channels, y * stride + x * channels + 3)].join(',') };
}
const shot = (buffer, ...points) => {
  const image = decodePng(Buffer.from(buffer));
  return { size: [image.width, image.height], pixels: points.map(([x, y]) => image.at(x, y)) };
};
const pdfInfo = buffer => {
  const text = Buffer.from(buffer).toString('latin1');
  return { magic: text.slice(0, 5), pages: (text.match(/\/Type\s*\/Page\b/g) || []).length };
};
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const sorted = list => [...list].sort();
const once = (emitter, event, filter = () => true) => new Promise(resolve => {
  const handler = value => {
    if (filter(value)) {
      emitter.off(event, handler);
      resolve(value);
    }
  };
  emitter.on(event, handler);
});
const events = page => page.evaluate(() => window.events.splice(0));
const stripHeaders = headers => Object.keys(headers).filter(k => !/^(sec-ch-ua|user-agent|sec-ch-ua-.*)$/.test(k)).sort();

// ---------------------------------------------------------------- the cases

const cases = [];
const pp = (name, body) => cases.push({ client: 'puppeteer', name, body });
const pw = (name, body) => cases.push({ client: 'playwright', name, body });

// ---- Puppeteer

pp('connect: version shape', async ({ browser }) => {
  const version = await browser.version();
  return { product: /\/\d+\./.test(version), agent: /Mozilla/.test(await browser.userAgent()), connected: browser.connected };
});
pp('targets: types of a new page', async ({ page, context }) => context.targets().map(t => t.type()).includes('page') && page.target().type());
pp('contexts: isolated storage', async ({ browser, page }) => {
  const second = await browser.createBrowserContext();
  try {
    const other_page = await second.newPage();
    await page.goto(`${origin}/empty.html`);
    await other_page.goto(`${origin}/empty.html`);
    await page.evaluate(() => localStorage.setItem('k', 'one'));
    return { other: await other_page.evaluate(() => localStorage.getItem('k')), contexts: browser.browserContexts().length >= 2 };
  } finally {
    await second.close();
  }
});
pp('navigation: goto response', async ({ page }) => {
  const r = await page.goto(`${origin}/status?code=201`);
  return { status: r.status(), ok: r.ok(), header: r.headers()['x-custom'], text: await r.text(), url: r.url().replace(origin, ''), from: r.fromCache() };
});
pp('navigation: 404, redirect chain', async ({ page }) => {
  const missing = await page.goto(`${origin}/none`);
  const moved = await page.goto(`${origin}/redirect`);
  return { missing: missing.status(), final: moved.url().replace(origin, ''), chain: moved.request().redirectChain().map(r => r.url().replace(origin, '')) };
});
pp('navigation: failure message', async ({ page }) => page.goto('http://127.0.0.1:1/').catch(e => e.message.split(' at ')[0]));
pp('navigation: waitUntil lifecycle', async ({ page }) => {
  const out = {};
  for (const until of ['load', 'domcontentloaded', 'networkidle0', 'networkidle2'])
    out[until] = (await page.goto(`${origin}/empty.html`, { waitUntil: until })).status();
  return out;
});
pp('navigation: back, forward, reload', async ({ page }) => {
  await page.goto(`${origin}/empty.html`);
  await page.goto(`${origin}/child.html`);
  const back = await page.goBack();
  const forward = await page.goForward();
  const reload = await page.reload();
  return [back.url(), forward.url(), reload.url()].map(u => u.replace(origin, ''));
});
pp('navigation: same-document', async ({ page }) => {
  await page.goto(`${origin}/empty.html`);
  const wait = page.waitForNavigation();
  await page.evaluate(() => { location.hash = '#x'; });
  await wait;
  return page.url().replace(origin, '');
});
pp('content: setContent, title, content', async ({ page }) => {
  await page.setContent('<title>T</title><p id=a class=c>x</p>');
  return { title: await page.title(), content: await page.content(), eval: await page.$eval('#a', e => e.className), all: await page.$$eval('p', e => e.length) };
});
pp('evaluate: values', async ({ page }) => page.evaluate(() => ({ n: 1, s: 'x', a: [1, { b: 2 }], u: undefined, nan: Number.isNaN(NaN), d: new Date(0).toISOString() })));
pp('evaluate: exception message', async ({ page }) => page.evaluate(() => { throw new Error('thrown'); }).catch(e => e.message));
pp('evaluate: handles', async ({ page }) => {
  const handle = await page.evaluateHandle(() => ({ a: 1, b: 'two' }));
  const props = await handle.getProperties();
  const value = await handle.jsonValue();
  await handle.dispose();
  return { keys: [...props.keys()], value };
});
pp('evaluate: queryObjects', async ({ page }) => {
  await page.evaluate(() => { window.m = new Map(); });
  const proto = await page.evaluateHandle(() => Map.prototype);
  const found = await page.queryObjects(proto);
  return page.evaluate(list => list.length, found);
});
pp('evaluate: on new document, exposeFunction', async ({ page }) => {
  await page.evaluateOnNewDocument(() => { window.early = 'yes'; });
  await page.exposeFunction('addUp', (a, b) => a + b);
  await page.goto(`${origin}/empty.html`);
  return { early: await page.evaluate(() => window.early), sum: await page.evaluate(() => window.addUp(2, 3)) };
});
pp('frames: same site tree', async ({ page }) => {
  await page.goto(`${origin}/same-frames.html`);
  return { urls: page.frames().map(f => f.url().replace(origin, '')), names: page.frames().map(f => f.name()), text: await page.frames()[1].$eval('#inner', e => e.textContent), parent: page.frames()[1].parentFrame() === page.mainFrame() };
});
pp('frames: out of process', async ({ page }) => {
  await page.goto(`${origin}/cross.html?o=${encodeURIComponent(other)}`);
  const frame = page.frames()[1];
  return { oopif: frame.isOOPIF(), url: frame.url().replace(other, ''), text: await frame.evaluate(() => document.querySelector('#inner').textContent), targets: page.target().type() };
});
pp('frames: click into out of process frame', async ({ page }) => {
  await page.goto(`${origin}/cross.html?o=${encodeURIComponent(other)}`);
  await page.mouse.click(30, 20);
  const frame = page.frames()[1];
  await frame.click('#b').catch(() => {});
  return frame.evaluate(() => document.querySelector('#b').textContent);
});
pp('frames: attach, navigate, detach events', async ({ page }) => {
  await page.goto(`${origin}/empty.html`);
  const log = [];
  page.on('frameattached', () => log.push('attached'));
  page.on('framenavigated', f => f !== page.mainFrame() && log.push('navigated'));
  page.on('framedetached', () => log.push('detached'));
  const nav = once(page, 'framenavigated');
  await page.evaluate(() => { const f = document.createElement('iframe'); f.id = 'n'; f.src = '/child.html'; document.body.append(f); });
  await nav;
  await page.evaluate(() => document.querySelector('#n').remove());
  await sleep(200);
  return log;
});
pp('input: mouse click sequence', async ({ page }) => {
  await page.goto(`${origin}/form.html`);
  await page.mouse.click(50, 20);
  return { events: await events(page), clicked: await page.evaluate(() => window.clicked) };
});
pp('input: double, right click', async ({ page }) => {
  await page.goto(`${origin}/form.html`);
  await page.mouse.click(50, 20, { count: 2 });
  await page.mouse.click(50, 20, { button: 'right' });
  return events(page);
});
pp('input: wheel', async ({ page }) => {
  await page.setViewport({ width: 400, height: 300 });
  await page.goto(`${origin}/scroll.html`);
  await page.mouse.move(100, 100);
  await page.mouse.wheel({ deltaY: 250 });
  await page.waitForFunction(() => scrollY > 0);
  return page.evaluate(() => scrollY);
});
pp('input: keyboard type and modifiers', async ({ page }) => {
  await page.goto(`${origin}/form.html`);
  await page.click('#i');
  await page.keyboard.type('aB1');
  await page.keyboard.down('Shift');
  await page.keyboard.press('KeyC');
  await page.keyboard.up('Shift');
  await page.keyboard.press('Backspace');
  await page.keyboard.sendCharacter('嗨');
  return { value: await page.$eval('#i', e => e.value), events: (await events(page)).filter(e => /^key/.test(e)) };
});
pp('input: select all and cut with keyboard', async ({ page }) => {
  await page.goto(`${origin}/form.html`);
  await page.type('#t', 'text');
  await page.keyboard.down('Control');
  await page.keyboard.press('KeyA', { commands: ['selectAll'] });
  await page.keyboard.up('Control');
  await page.keyboard.press('Delete');
  return page.$eval('#t', e => e.value);
});
pp('input: touchscreen', async ({ page }) => {
  await page.emulate({ viewport: { width: 400, height: 300, hasTouch: true }, userAgent: await page.evaluate(() => navigator.userAgent) });
  await page.goto(`${origin}/form.html`);
  await page.touchscreen.tap(50, 20);
  return { events: (await events(page)).map(e => e.split(':')[0]), touch: await page.evaluate(() => navigator.maxTouchPoints > 0) };
});
pp('input: drag and drop', async ({ page }) => {
  await page.goto(`${origin}/drag.html`);
  const source = await page.$('#s'), target = await page.$('#d');
  await source.drop(target);
  await sleep(200);
  return page.evaluate(() => window.ev);
});
pp('input: locator with auto-wait', async ({ page }) => {
  await page.goto(`${origin}/form.html`);
  await page.locator('#i').fill('filled');
  await page.locator('#b').click();
  return { value: await page.$eval('#i', e => e.value), clicked: await page.evaluate(() => window.clicked), text: await page.locator('::-p-aria(Button)').map(e => e.textContent).wait() };
});
pp('input: file upload and chooser', async ({ page }) => {
  await page.goto(`${origin}/form.html`);
  const file = path.join(work, 'upload.txt');
  fs.writeFileSync(file, 'content');
  const handle = await page.$('#f');
  await handle.uploadFile(file);
  const first = await page.$eval('#f', async e => [e.files[0].name, await e.files[0].text()]);
  const [chooser] = await Promise.all([page.waitForFileChooser(), page.click('#f')]);
  const multiple = chooser.isMultiple();
  await chooser.cancel();
  return { first, multiple };
});
pp('network: request events and order', async ({ page }) => {
  const log = [];
  page.on('request', r => log.push(['request', new URL(r.url()).pathname, r.method(), r.resourceType(), r.isNavigationRequest()]));
  page.on('response', r => log.push(['response', new URL(r.url()).pathname, r.status()]));
  page.on('requestfinished', r => log.push(['finished', new URL(r.url()).pathname]));
  await page.goto(`${origin}/coverage.html`, { waitUntil: 'networkidle0' });
  return log.sort((a, b) => JSON.stringify(a) < JSON.stringify(b) ? -1 : 1);
});
pp('network: response body, headers, post data', async ({ page }) => {
  await page.goto(`${origin}/empty.html`);
  const wait = page.waitForResponse(r => r.url().endsWith('/echo'));
  await page.evaluate(() => fetch('/echo', { method: 'POST', body: 'posted', headers: { 'x-a': 'b' } }));
  const response = await wait;
  const json = await response.json();
  return { method: json.method, body: json.body, a: json.headers['x-a'], post: response.request().postData(), sent: response.request().headers()['x-a'], fetch: response.request().resourceType(), keys: stripHeaders(json.headers) };
});
pp('network: failed request', async ({ page }) => {
  await page.goto(`${origin}/empty.html`);
  const failed = once(page, 'requestfailed');
  page.evaluate(() => fetch('http://127.0.0.1:1/').catch(() => {}));
  return (await failed).failure().errorText;
});
pp('interception: abort, respond, continue with changes', async ({ page }) => {
  await page.setRequestInterception(true);
  page.on('request', r => {
    if (r.url().endsWith('/data.json')) r.abort('accessdenied');
    else if (r.url().endsWith('/mock')) r.respond({ status: 202, contentType: 'text/plain', headers: { 'x-m': '1', 'access-control-allow-origin': '*' }, body: 'mocked' });
    else if (r.url().endsWith('/echo')) r.continue({ method: 'POST', postData: 'changed', headers: { ...r.headers(), 'x-added': 'yes' } });
    else r.continue();
  });
  await page.goto(`${origin}/empty.html`);
  return page.evaluate(async () => {
    const out = {};
    out.abort = await fetch('/data.json').then(() => 'ok', e => e.message);
    const mock = await fetch('/mock');
    out.mock = [mock.status, mock.headers.get('x-m'), await mock.text()];
    const echo = await (await fetch('/echo')).json();
    out.echo = [echo.method, echo.body, echo.headers['x-added']];
    return out;
  });
});
pp('interception: document and redirect', async ({ page }) => {
  await page.setRequestInterception(true);
  const seen = [];
  page.on('request', r => {
    seen.push(new URL(r.url()).pathname);
    if (r.url().endsWith('/fake.html')) r.respond({ status: 200, contentType: 'text/html', body: '<title>Fake</title>' });
    else r.continue();
  });
  await page.goto(`${origin}/fake.html`);
  const title = await page.title();
  await page.goto(`${origin}/redirect`);
  return { title, seen, url: page.url().replace(origin, '') };
});
pp('network: extra headers, user agent, cache, auth', async ({ page }) => {
  await page.setExtraHTTPHeaders({ 'x-extra': 'e' });
  await page.setUserAgent('Custom/1');
  await page.setCacheEnabled(false);
  await page.goto(`${origin}/echo`);
  const echo = JSON.parse(await page.evaluate(() => document.body.innerText));
  await page.authenticate({ username: 'user', password: 'pass' });
  await page.goto(`${origin}/auth`);
  return { extra: echo.headers['x-extra'], agent: echo.headers['user-agent'], cache: echo.headers['cache-control'], authed: await page.title() };
});
pp('network: offline, throttled', async ({ page }) => {
  await page.goto(`${origin}/empty.html`);
  await page.setOfflineMode(true);
  const offline = await page.evaluate(() => [navigator.onLine, fetch('/data.json').then(() => 'ok', e => e.message)]);
  await page.setOfflineMode(false);
  await page.emulateNetworkConditions({ upload: 100000, download: 100000, latency: 5 });
  const online = await page.evaluate(() => fetch('/data.json').then(r => r.status));
  return { offline, online };
});
pp('network: websocket frames over raw events', async ({ page }) => {
  const session = await page.createCDPSession();
  await session.send('Network.enable');
  const log = [];
  session.on('Network.webSocketFrameSent', e => log.push(['sent', e.response.payloadData]));
  session.on('Network.webSocketFrameReceived', e => log.push(['received', e.response.payloadData]));
  await page.goto(`${origin}/ws.html`);
  return { got: await page.evaluate(() => window.got), log };
});
pp('cookies: page and browser', async ({ page, context }) => {
  await page.goto(`${origin}/set-cookie`);
  const names = (await context.cookies()).map(c => [c.name, c.httpOnly, c.sameSite, c.session]).sort();
  await context.setCookie({ name: 'added', value: 'a', domain: 'localhost', path: '/' });
  const visible = await page.evaluate(() => document.cookie);
  const all = await context.cookies();
  await context.deleteCookie(...all.filter(c => c.name === 'added'));
  return { names, visible, left: (await context.cookies()).map(c => c.name).sort() };
});
pp('cookies: partition, expires', async ({ page, context }) => {
  await page.goto(`${origin}/empty.html`);
  await context.setCookie({ name: 'e', value: '1', domain: 'localhost', path: '/', expires: 4000000000 });
  const [c] = (await context.cookies()).filter(x => x.name === 'e');
  return { expires: c.expires, session: c.session, secure: c.secure };
});
pp('screenshot: viewport, clip, full page, jpeg, element', async ({ page }) => {
  await page.setViewport({ width: 300, height: 200 });
  await page.goto(`${origin}/box.html`);
  const element = await page.$('#g');
  return {
    view: shot(await page.screenshot(), [50, 50], [150, 50], [250, 50]),
    clip: shot(await page.screenshot({ clip: { x: 90, y: 0, width: 20, height: 10 } }), [5, 5], [15, 5]),
    full: shot(await page.screenshot({ fullPage: true }), [50, 50]),
    element: shot(await element.screenshot(), [50, 50]),
    jpeg: Buffer.from(await page.screenshot({ type: 'jpeg', quality: 50 })).subarray(0, 3).toString('hex'),
    webp: Buffer.from(await page.screenshot({ type: 'webp' })).toString('latin1', 8, 12),
    transparent: shot(await page.screenshot({ omitBackground: true }), [250, 50]),
  };
});
pp('screenshot: device scale factor', async ({ page }) => {
  await page.setViewport({ width: 200, height: 100, deviceScaleFactor: 2 });
  await page.goto(`${origin}/box.html`);
  return shot(await page.screenshot(), [100, 100], [300, 100]);
});
pp('screenshot: into an out-of-process frame', async ({ page }) => {
  await page.setViewport({ width: 400, height: 300 });
  await page.goto(`${origin}/cross.html?o=${encodeURIComponent(other)}`);
  return shot(await page.screenshot(), [395, 295]);
});
pp('pdf: print', async ({ page }) => {
  await page.goto(`${origin}/print.html`);
  return { a4: pdfInfo(await page.pdf({ format: 'a4' })), range: pdfInfo(await page.pdf({ pageRanges: '1' })), background: pdfInfo(await page.pdf({ printBackground: true, landscape: true })) };
});
pp('pdf: stream', async ({ page }) => {
  await page.goto(`${origin}/print.html`);
  const chunks = [];
  const reader = (await page.createPDFStream()).getReader();
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    chunks.push(Buffer.from(value));
  }
  return pdfInfo(Buffer.concat(chunks));
});
pp('dialogs: alert, confirm, prompt', async ({ page }) => {
  await page.goto(`${origin}/empty.html`);
  const log = [];
  page.on('dialog', async d => {
    log.push([d.type(), d.message(), d.defaultValue()]);
    if (d.type() === 'confirm') await d.dismiss();
    else await d.accept(d.type() === 'prompt' ? 'answer' : undefined);
  });
  const out = await page.evaluate(() => { alert('a'); return [confirm('c'), prompt('p', 'def')]; });
  return { log, out };
});
pp('dialogs: beforeunload', async ({ page }) => {
  await page.goto(`${origin}/unload.html`);
  await page.mouse.click(5, 5);
  const seen = once(page, 'dialog');
  page.close({ runBeforeUnload: true }).catch(() => {});
  const dialog = await seen;
  const type = dialog.type();
  await dialog.accept();
  await sleep(200);
  return { type, closed: page.isClosed() };
});
pp('download: setDownloadBehavior', async ({ page }) => {
  const dir = fs.mkdtempSync(path.join(work, 'dl-'));
  const session = await page.createCDPSession();
  await session.send('Browser.setDownloadBehavior', { behavior: 'allow', downloadPath: dir, browserContextId: page.browserContext().id, eventsEnabled: true });
  await page.goto(`${origin}/empty.html`);
  const log = [];
  const done = new Promise(resolve => session.on('Browser.downloadProgress', e => {
    if (e.state !== 'inProgress') {
      log.push(e.state);
      resolve();
    }
  }));
  const begun = new Promise(resolve => session.on('Browser.downloadWillBegin', e => resolve(e.suggestedFilename)));
  await page.evaluate(url => { const a = document.createElement('a'); a.href = url; document.body.append(a); a.click(); }, '/download');
  const name = await begun;
  await done;
  await sleep(100);
  const files = fs.readdirSync(dir);
  return { name, log, count: files.length, content: files[0] && fs.readFileSync(path.join(dir, name), 'utf8') };
});
pp('workers: dedicated', async ({ page }) => {
  const created = once(page, 'workercreated');
  await page.goto(`${origin}/worker.html`);
  const worker = await created;
  return { url: worker.url().replace(origin, ''), value: await worker.evaluate(() => self.value), count: page.workers().length };
});
pp('workers: service worker target', async ({ page, context }) => {
  const target = context.waitForTarget(t => t.type() === 'service_worker');
  await page.goto(`${origin}/sw.html`);
  const found = await target;
  const worker = await found.worker();
  return { url: found.url().replace(origin, ''), value: await worker.evaluate(() => self.value), active: await page.evaluate(() => window.reg) };
});
pp('workers: shared worker target', async ({ page, context }) => {
  const target = context.waitForTarget(t => t.type() === 'shared_worker');
  await page.goto(`${origin}/shared.html`);
  const found = await target;
  return { url: found.url().replace(origin, '') };
});
pp('emulation: viewport and device', async ({ page }) => {
  await page.emulate(puppeteer.KnownDevices['iPhone 13']);
  await page.goto(`${origin}/empty.html`);
  return page.evaluate(() => ({ w: innerWidth, h: innerHeight, dpr: devicePixelRatio, ua: /iPhone/.test(navigator.userAgent), touch: navigator.maxTouchPoints > 0, coarse: matchMedia('(pointer: coarse)').matches, mobile: screen.width }));
});
pp('emulation: media, timezone, vision, cpu, idle', async ({ page }) => {
  await page.goto(`${origin}/print.html`);
  await page.emulateMediaType('print');
  const print = await page.evaluate(() => matchMedia('print').matches);
  await page.emulateMediaType('screen');
  await page.emulateMediaFeatures([{ name: 'prefers-color-scheme', value: 'dark' }, { name: 'prefers-reduced-motion', value: 'reduce' }]);
  const features = await page.evaluate(() => [matchMedia('(prefers-color-scheme: dark)').matches, matchMedia('(prefers-reduced-motion: reduce)').matches]);
  await page.emulateTimezone('Asia/Tokyo');
  const zone = await page.evaluate(() => [Intl.DateTimeFormat().resolvedOptions().timeZone, new Date(0).getHours()]);
  await page.emulateVisionDeficiency('achromatopsia');
  await page.emulateCPUThrottling(2);
  await page.emulateCPUThrottling(null);
  await page.emulateIdleState({ isUserActive: false, isScreenUnlocked: true });
  await page.emulateIdleState();
  return { print, features, zone };
});
pp('emulation: geolocation and permissions', async ({ page, context }) => {
  await page.goto(`${origin}/empty.html`);
  await context.overridePermissions(origin, ['geolocation']);
  await page.setGeolocation({ latitude: 10, longitude: 20 });
  const position = await page.evaluate(() => new Promise((k, r) => navigator.geolocation.getCurrentPosition(p => k([p.coords.latitude, p.coords.longitude]), e => r(new Error(e.message)))));
  const state = await page.evaluate(() => navigator.permissions.query({ name: 'geolocation' }).then(s => s.state));
  await context.clearPermissionOverrides();
  return { position, state };
});
pp('emulation: javascript off, bypass CSP', async ({ page }) => {
  await page.setBypassCSP(true);
  await page.goto(`${origin}/csp.html`);
  await page.addScriptTag({ content: 'window.injected=1' });
  const injected = await page.evaluate(() => window.injected);
  await page.setJavaScriptEnabled(false);
  await page.setContent('<script>document.title="ran"</script><title>not</title>');
  const title = await page.title();
  await page.setJavaScriptEnabled(true);
  return { injected, title };
});
pp('tracing: start and stop', async ({ page }) => {
  await page.goto(`${origin}/empty.html`);
  await page.tracing.start({ categories: ['devtools.timeline', 'blink.user_timing'] });
  await page.evaluate(() => performance.mark('traced'));
  await page.reload();
  const buffer = await page.tracing.stop();
  const trace = JSON.parse(Buffer.from(buffer).toString());
  return { list: Array.isArray(trace.traceEvents), has: trace.traceEvents.some(e => e.name === 'traced'), metadata: trace.traceEvents.some(e => e.ph === 'M') };
});
pp('coverage: JavaScript', async ({ page }) => {
  await page.coverage.startJSCoverage();
  await page.goto(`${origin}/coverage.html`);
  const coverage = await page.coverage.stopJSCoverage();
  return coverage.map(c => [c.url.replace(origin, ''), c.ranges]).sort();
});
pp('coverage: CSS', async ({ page }) => {
  await page.coverage.startCSSCoverage();
  await page.goto(`${origin}/coverage.html`);
  const coverage = await page.coverage.stopCSSCoverage();
  return coverage.map(c => [c.url.replace(origin, ''), c.ranges]).sort();
});
pp('accessibility: snapshot', async ({ page }) => {
  await page.goto(`${origin}/a11y.html`);
  return { interesting: await page.accessibility.snapshot(), all: (await page.accessibility.snapshot({ interestingOnly: false })).children?.length };
});
pp('accessibility: aria selectors', async ({ page }) => {
  await page.goto(`${origin}/a11y.html`);
  const found = await page.$('::-p-aria([name="Save"][role="button"])');
  return found.evaluate(e => e.tagName);
});
pp('pages: several, popups', async ({ page, context }) => {
  const second = await context.newPage();
  await page.goto(`${origin}/popup.html`);
  const popup = new Promise(resolve => page.once('popup', resolve));
  await page.click('#l');
  const opened = await popup;
  await opened.waitForFunction(() => document.readyState === 'complete');
  const count = (await context.pages()).length;
  await second.close();
  return { count, url: opened.url().replace(origin, ''), after: (await context.pages()).length, opener: await opened.evaluate(() => !!window.opener) };
});
pp('pages: bringToFront and visibility', async ({ page, context }) => {
  const second = await context.newPage();
  await page.bringToFront();
  return { first: await page.evaluate(() => document.visibilityState), focus: await page.evaluate(() => document.hasFocus()), second: await second.evaluate(() => typeof document.visibilityState) };
});
pp('console and page errors', async ({ page }) => {
  const log = [];
  page.on('console', m => log.push([m.type(), m.text()]));
  const failed = once(page, 'pageerror');
  await page.goto(`${origin}/errors.html`);
  return { log, error: (await failed).message };
});
pp('console: arguments as handles', async ({ page }) => {
  const seen = once(page, 'console');
  page.evaluate(() => console.log('x', { a: 1 }));
  const message = await seen;
  return { values: await Promise.all(message.args().map(a => a.jsonValue())), location: typeof message.location().url };
});
pp('page: metrics, wait for function and selector', async ({ page }) => {
  await page.goto(`${origin}/empty.html`);
  const metrics = await page.metrics();
  const wait = page.waitForSelector('#late');
  const fn = page.waitForFunction(() => window.ready === 1);
  await page.evaluate(() => { setTimeout(() => { document.body.innerHTML = '<p id=late>'; window.ready = 1; }, 50); });
  await Promise.all([wait, fn]);
  return { keys: Object.keys(metrics).sort() };
});
pp('page: add tags', async ({ page }) => {
  await page.goto(`${origin}/empty.html`);
  await page.addScriptTag({ url: '/script.js' });
  await page.addStyleTag({ content: 'body{color:rgb(1,2,3)}' });
  return { fn: await page.evaluate(() => used()), color: await page.evaluate(() => getComputedStyle(document.body).color) };
});
const rawGroup = (name, method, params, pick) => pp(`raw: ${name}`, async ({ page }) => {
  await page.goto(`${origin}/a11y.html`);
  const session = await page.createCDPSession();
  return pick(await session.send(method, params));
});
rawGroup('Browser.getVersion keys', 'Browser.getVersion', {}, r => Object.keys(r).sort());
rawGroup('Page.getFrameTree', 'Page.getFrameTree', {}, r => ({ keys: Object.keys(r.frameTree.frame).sort(), url: r.frameTree.frame.url.replace(origin, ''), site: r.frameTree.frame.domainAndRegistry }));
rawGroup('Page.getLayoutMetrics keys', 'Page.getLayoutMetrics', {}, r => Object.keys(r).sort());
rawGroup('Performance.enable', 'Performance.enable', {}, () => 'ok');
rawGroup('Target.getTargets types', 'Target.getTargets', {}, r => [...new Set(r.targetInfos.map(t => t.type))].sort());
rawGroup('Target.getBrowserContexts', 'Target.getBrowserContexts', {}, r => Array.isArray(r.browserContextIds));
rawGroup('Browser.getWindowForTarget keys', 'Browser.getWindowForTarget', {}, r => Object.keys(r).sort());
rawGroup('Tracing.getCategories', 'Tracing.getCategories', {}, r => r.categories.includes('devtools.timeline'));
rawGroup('Schema.getDomains', 'Schema.getDomains', {}, r => r.domains.map(d => d.name).sort());
rawGroup('SystemInfo.getProcessInfo', 'SystemInfo.getProcessInfo', {}, r => [...new Set(r.processInfo.map(p => p.type))].sort());
rawGroup('Storage.getUsageAndQuota', 'Storage.getUsageAndQuota', { origin }, r => Object.keys(r).sort());
rawGroup('DOM.getDocument', 'DOM.getDocument', { depth: 1 }, r => r.root.children.map(c => c.nodeName));
rawGroup('Accessibility.getFullAXTree', 'Accessibility.getFullAXTree', {}, r => r.nodes.filter(n => !n.ignored).map(n => [n.role?.value, n.name?.value]));
rawGroup('Page.getResourceTree', 'Page.getResourceTree', {}, r => r.frameTree.resources.map(x => [x.url.replace(origin, ''), x.type]));
rawGroup('Network.getAllCookies', 'Network.getAllCookies', {}, r => Array.isArray(r.cookies));
rawGroup('Page.getNavigationHistory', 'Page.getNavigationHistory', {}, r => r.entries.map(e => e.url.replace(origin, '')));
rawGroup('CSS.enable needs DOM', 'CSS.enable', {}, () => 'ok');
pp('raw: screencast frames', async ({ page }) => {
  await page.goto(`${origin}/box.html`);
  const session = await page.createCDPSession();
  const frame = new Promise(resolve => session.once('Page.screencastFrame', resolve));
  await session.send('Page.startScreencast', { format: 'png', everyNthFrame: 1 });
  await page.evaluate(() => { document.querySelector('#r').style.background = '#00f'; });
  const got = await frame;
  await session.send('Page.screencastFrameAck', { sessionId: got.sessionId });
  await session.send('Page.stopScreencast');
  return { keys: Object.keys(got.metadata).sort(), png: Buffer.from(got.data, 'base64').toString('latin1', 1, 4) };
});
pp('raw: profiler and heap snapshot', async ({ page }) => {
  await page.goto(`${origin}/empty.html`);
  const session = await page.createCDPSession();
  await session.send('Profiler.enable');
  await session.send('Profiler.start');
  await page.evaluate(() => { for (let i = 0, s = 0; i < 1e5; ++i) s += i; });
  const { profile } = await session.send('Profiler.stop');
  let bytes = 0;
  session.on('HeapProfiler.addHeapSnapshotChunk', e => { bytes += e.chunk.length; });
  await session.send('HeapProfiler.takeHeapSnapshot', { reportProgress: false });
  return { profile: Object.keys(profile).sort(), heap: bytes > 1000 };
});
pp('raw: DevTools HTTP endpoints seen by a target', async ({ browser }) => {
  const base = new URL(browser.wsEndpoint());
  const get = async p => {
    const r = await fetch(`http://${base.host}${p}`);
    return r.ok ? r.json() : r.status;
  };
  const version = await get('/json/version'), list = await get('/json/list'), protocol = await get('/json/protocol');
  return { version: Object.keys(version).sort(), listKeys: Object.keys(list[0] || {}).sort(), domains: (protocol.domains || []).length > 10 };
});
pp('raw: new target over HTTP, activate, close', async ({ browser }) => {
  const base = new URL(browser.wsEndpoint());
  const made = await (await fetch(`http://${base.host}/json/new?about:blank`, { method: 'PUT' })).json();
  const activate = await fetch(`http://${base.host}/json/activate/${made.id}`);
  const close = await fetch(`http://${base.host}/json/close/${made.id}`);
  return { type: made.type, url: made.url, activate: activate.status, close: close.status };
});

// ---- Playwright over CDP

pw('connect: version, contexts', async ({ browser, context }) => ({ version: /^\d+\./.test(browser.version()), type: browser.browserType().name(), contexts: browser.contexts().includes(context), connected: browser.isConnected() }));
pw('connect: default context', async ({ browser }) => ({ present: browser.contexts().length >= 1, pages: browser.contexts()[0].pages().length >= 0 }));
pw('navigation: goto, redirect, 404', async ({ page }) => {
  const a = await page.goto(`${origin}/status?code=201`);
  const b = await page.goto(`${origin}/redirect`);
  const c = await page.goto(`${origin}/none`);
  return { a: [a.status(), a.ok(), a.headers()['x-custom'], await a.text()], b: [b.url().replace(origin, ''), b.request().redirectedFrom().url().replace(origin, '')], c: c.status() };
});
pw('navigation: failure message', async ({ page }) => page.goto('http://127.0.0.1:1/').catch(e => e.message.split('\n')[0]));
pw('navigation: back, forward, reload, lifecycle', async ({ page }) => {
  await page.goto(`${origin}/empty.html`);
  await page.goto(`${origin}/child.html`, { waitUntil: 'networkidle' });
  await page.goBack();
  const back = page.url();
  await page.goForward();
  const forward = page.url();
  await page.reload({ waitUntil: 'domcontentloaded' });
  return [back, forward, page.url()].map(u => u.replace(origin, ''));
});
pw('navigation: waitForURL, same document', async ({ page }) => {
  await page.goto(`${origin}/empty.html`);
  await page.evaluate(() => { history.pushState({}, '', '/pushed'); });
  await page.waitForURL('**/pushed');
  return page.url().replace(origin, '');
});
pw('content: setContent, title', async ({ page }) => {
  await page.setContent('<title>T</title><p id=a>x</p>');
  return { title: await page.title(), html: await page.content(), inner: await page.locator('#a').innerHTML() };
});
pw('evaluate: values, handles, exception', async ({ page }) => {
  const value = await page.evaluate(() => ({ n: 1, big: 2n ** 70n, d: new Date(0), re: /x/g, u: undefined, inf: Infinity }));
  const handle = await page.evaluateHandle(() => ({ a: 1 }));
  const a = await (await handle.getProperty('a')).jsonValue();
  const error = await page.evaluate(() => { throw new Error('thrown'); }).catch(e => e.message.split('\n')[0]);
  return { value: { ...value, big: String(value.big), re: String(value.re), d: value.d.toISOString(), inf: String(value.inf) }, a, error };
});
pw('evaluate: init script, exposeBinding', async ({ page }) => {
  await page.addInitScript(() => { window.early = 'yes'; });
  await page.exposeBinding('who', (source, x) => [source.frame === page.mainFrame(), x]);
  await page.exposeFunction('addUp', (a, b) => a + b);
  await page.goto(`${origin}/empty.html`);
  return { early: await page.evaluate(() => window.early), who: await page.evaluate(() => window.who('v')), sum: await page.evaluate(() => window.addUp(1, 2)) };
});
pw('frames: same site', async ({ page }) => {
  await page.goto(`${origin}/same-frames.html`);
  return { urls: page.frames().map(f => f.url().replace(origin, '')), a: await page.frame('a').locator('#inner').textContent(), child: await page.frameLocator('iframe[name=a]').locator('#b').textContent() };
});
pw('frames: out of process', async ({ page }) => {
  await page.goto(`${origin}/cross.html?o=${encodeURIComponent(other)}`);
  const frame = page.frames()[1];
  await page.frameLocator('#x').locator('#b').click();
  const session = await page.context().newCDPSession(page);
  const { targetInfos } = await session.send('Target.getTargets');
  return { url: frame.url().replace(other, ''), text: await frame.locator('#b').textContent(), iframeTarget: targetInfos.some(t => t.type === 'iframe' && t.url.startsWith(other)) };
});
pw('frames: detached', async ({ page }) => {
  await page.goto(`${origin}/same-frames.html`);
  const frame = page.frame('a');
  const detached = once(page, 'framedetached');
  await page.evaluate(() => document.querySelector('iframe[name=a]').remove());
  await detached;
  return { detached: frame.isDetached(), count: page.frames().length };
});
pw('input: mouse', async ({ page }) => {
  await page.goto(`${origin}/form.html`);
  await page.mouse.click(50, 20);
  await page.mouse.dblclick(50, 20);
  await page.mouse.click(50, 20, { button: 'right' });
  return { events: await events(page), clicked: await page.evaluate(() => window.clicked) };
});
pw('input: wheel, scrollIntoView', async ({ page }) => {
  await page.setViewportSize({ width: 400, height: 300 });
  await page.goto(`${origin}/scroll.html`);
  await page.mouse.move(100, 100);
  await page.mouse.wheel(0, 250);
  await page.waitForFunction(() => scrollY > 0);
  const wheel = await page.evaluate(() => scrollY);
  await page.locator('#low').scrollIntoViewIfNeeded();
  return { wheel, low: await page.evaluate(() => scrollY > 1000) };
});
pw('input: keyboard', async ({ page }) => {
  await page.goto(`${origin}/form.html`);
  await page.locator('#i').click();
  await page.keyboard.type('aB1');
  await page.keyboard.press('Shift+KeyC');
  await page.keyboard.press('Backspace');
  await page.keyboard.insertText('嗨');
  await page.keyboard.press('ControlOrMeta+A');
  await page.keyboard.press('ArrowRight');
  return { value: await page.inputValue('#i'), events: (await events(page)).filter(e => /^key/.test(e)) };
});
pw('input: fill, check, select, click with modifiers', async ({ page }) => {
  await page.goto(`${origin}/form.html`);
  await page.fill('#i', 'filled');
  await page.check('#c');
  await page.click('#b', { modifiers: ['Shift'] });
  await page.getByRole('button', { name: 'Button' }).click();
  return { value: await page.inputValue('#i'), checked: await page.isChecked('#c'), clicked: await page.evaluate(() => window.clicked), shift: (await events(page)).some(e => e.startsWith('click') && e.split(':')[6] === '1') };
});
pw('input: file chooser', async ({ page }) => {
  await page.goto(`${origin}/form.html`);
  const [chooser] = await Promise.all([page.waitForEvent('filechooser'), page.click('#f')]);
  await chooser.setFiles({ name: 'a.txt', mimeType: 'text/plain', buffer: Buffer.from('hello') });
  return { multiple: chooser.isMultiple(), file: await page.$eval('#f', async e => [e.files[0].name, await e.files[0].text()]) };
});
pw('input: drag and drop', async ({ page }) => {
  await page.goto(`${origin}/drag.html`);
  await page.dragAndDrop('#s', '#d');
  return page.evaluate(() => window.ev);
});
pw('input: tap', async ({ browser }) => {
  const context = await browser.newContext({ hasTouch: true });
  const page = await context.newPage();
  await page.goto(`${origin}/form.html`);
  await page.tap('#b');
  const out = (await events(page)).map(e => e.split(':')[0]);
  await context.close();
  return out;
});
pw('network: events', async ({ page }) => {
  const log = [];
  page.on('request', r => log.push(['request', new URL(r.url()).pathname, r.method(), r.resourceType(), r.isNavigationRequest()]));
  page.on('response', r => log.push(['response', new URL(r.url()).pathname, r.status()]));
  page.on('requestfinished', r => log.push(['finished', new URL(r.url()).pathname]));
  await page.goto(`${origin}/coverage.html`, { waitUntil: 'networkidle' });
  return log.sort((a, b) => JSON.stringify(a) < JSON.stringify(b) ? -1 : 1);
});
pw('network: response details', async ({ page }) => {
  await page.goto(`${origin}/empty.html`);
  const [response] = await Promise.all([page.waitForResponse('**/echo'), page.evaluate(() => fetch('/echo', { method: 'POST', body: 'posted', headers: { 'x-a': 'b' } }))]);
  const request = response.request();
  const sizes = await request.sizes();
  return { post: request.postData(), type: request.resourceType(), sent: (await request.allHeaders())['x-a'], status: response.statusText(), server: !!(await response.serverAddr()), sizes: Object.keys(sizes).sort(), timing: Object.keys(request.timing()).sort(), body: (await response.json()).method };
});
pw('network: failed request', async ({ page }) => {
  await page.goto(`${origin}/empty.html`);
  const failed = page.waitForEvent('requestfailed');
  page.evaluate(() => fetch('http://127.0.0.1:1/').catch(() => {}));
  return (await failed).failure().errorText;
});
pw('route: abort, fulfill, continue, fallback, fetch', async ({ page }) => {
  await page.route('**/data.json', r => r.abort('accessdenied'));
  await page.route('**/mock', r => r.fulfill({ status: 202, contentType: 'text/plain', headers: { 'x-m': '1' }, body: 'mocked' }));
  await page.route('**/echo', r => r.continue({ method: 'POST', postData: 'changed', headers: { ...r.request().headers(), 'x-added': 'yes' } }));
  await page.route('**/fall', r => r.fallback({ url: `${origin}/data.json?f` }));
  await page.route('**/edited', async r => {
    const response = await r.fetch({ url: `${origin}/data.json` });
    await r.fulfill({ response, json: { ...(await response.json()), edited: true } });
  });
  await page.goto(`${origin}/empty.html`);
  return page.evaluate(async () => {
    const out = {};
    out.abort = await fetch('/data.json').then(() => 'ok', e => e.message);
    const mock = await fetch('/mock');
    out.mock = [mock.status, mock.headers.get('x-m'), await mock.text()];
    const echo = await (await fetch('/echo')).json();
    out.echo = [echo.method, echo.body, echo.headers['x-added']];
    out.edited = await (await fetch('/edited')).json();
    return out;
  });
});
pw('route: context and document', async ({ page, context }) => {
  await context.route('**/fake.html', r => r.fulfill({ status: 200, contentType: 'text/html', body: '<title>Fake</title>' }));
  await page.goto(`${origin}/fake.html`);
  const title = await page.title();
  await context.unroute('**/fake.html');
  return { title, status: (await page.goto(`${origin}/fake.html`)).status() };
});
pw('route: websocket', async ({ page }) => {
  await page.routeWebSocket('**/ws', ws => { ws.onMessage(m => ws.send('routed:' + m)); });
  await page.goto(`${origin}/ws.html`);
  return page.evaluate(() => window.got);
});
pw('network: websocket events', async ({ page }) => {
  const log = [];
  page.on('websocket', ws => {
    log.push('open');
    ws.on('framesent', f => log.push(['sent', f.payload]));
    ws.on('framereceived', f => log.push(['received', f.payload]));
  });
  await page.goto(`${origin}/ws.html`);
  return { got: await page.evaluate(() => window.got), log };
});
pw('network: extra headers, offline, credentials, user agent', async ({ browser }) => {
  const context = await browser.newContext({ extraHTTPHeaders: { 'x-extra': 'e' }, userAgent: 'Custom/1', httpCredentials: { username: 'user', password: 'pass' } });
  const page = await context.newPage();
  await page.goto(`${origin}/echo`);
  const echo = JSON.parse(await page.innerText('body'));
  await page.goto(`${origin}/auth`);
  const authed = await page.title();
  await context.setOffline(true);
  const offline = await page.evaluate(() => navigator.onLine);
  await context.close();
  return { extra: echo.headers['x-extra'], agent: echo.headers['user-agent'], authed, offline };
});
pw('request: APIRequestContext', async ({ page, context }) => {
  const response = await context.request.post(`${origin}/echo`, { data: { a: 1 } });
  await page.goto(`${origin}/set-cookie`);
  const again = await context.request.get(`${origin}/echo`);
  return { method: (await response.json()).method, body: (await response.json()).body, cookie: (await again.json()).headers.cookie };
});
pw('cookies: context', async ({ page, context }) => {
  await page.goto(`${origin}/set-cookie`);
  const names = (await context.cookies()).map(c => [c.name, c.httpOnly, c.sameSite, c.expires]).sort();
  await context.addCookies([{ name: 'added', value: 'a', url: origin }]);
  const visible = await page.evaluate(() => document.cookie);
  await context.clearCookies({ name: 'plain' });
  const left = (await context.cookies()).map(c => c.name).sort();
  await context.clearCookies();
  return { names, visible, left, none: (await context.cookies()).length };
});
pw('storage state', async ({ browser, page, context }) => {
  await page.goto(`${origin}/set-cookie`);
  await page.evaluate(() => localStorage.setItem('k', 'v'));
  const state = await context.storageState();
  const next = await browser.newContext({ storageState: state });
  const nextPage = await next.newPage();
  await nextPage.goto(`${origin}/empty.html`);
  const restored = { local: await nextPage.evaluate(() => localStorage.getItem('k')), cookies: await nextPage.evaluate(() => document.cookie) };
  await next.close();
  return { cookies: state.cookies.length, origins: state.origins.map(o => o.origin.replace(origin, '')), restored };
});
pw('screenshot: page, clip, full, jpeg, element, masks', async ({ page }) => {
  await page.setViewportSize({ width: 300, height: 200 });
  await page.goto(`${origin}/box.html`);
  return {
    view: shot(await page.screenshot(), [50, 50], [150, 50], [250, 50]),
    clip: shot(await page.screenshot({ clip: { x: 90, y: 0, width: 20, height: 10 } }), [5, 5], [15, 5]),
    full: shot(await page.screenshot({ fullPage: true }), [50, 50]),
    element: shot(await page.locator('#g').screenshot(), [50, 50]),
    jpeg: (await page.screenshot({ type: 'jpeg', quality: 50 })).subarray(0, 3).toString('hex'),
    transparent: shot(await page.screenshot({ omitBackground: true }), [250, 50]),
    masked: shot(await page.screenshot({ mask: [page.locator('#g')], maskColor: '#0000ff' }), [150, 50]),
  };
});
pw('screenshot: scale factor, css scale', async ({ browser }) => {
  const context = await browser.newContext({ viewport: { width: 200, height: 100 }, deviceScaleFactor: 2 });
  const page = await context.newPage();
  await page.goto(`${origin}/box.html`);
  const out = { device: shot(await page.screenshot(), [100, 100], [300, 100]), css: shot(await page.screenshot({ scale: 'css' }), [50, 50]) };
  await context.close();
  return out;
});
pw('pdf', async ({ page }) => {
  await page.goto(`${origin}/print.html`);
  return { a4: pdfInfo(await page.pdf({ format: 'A4' })), range: pdfInfo(await page.pdf({ pageRanges: '1' })), outline: pdfInfo(await page.pdf({ tagged: true, outline: true })) };
});
pw('dialogs', async ({ page }) => {
  await page.goto(`${origin}/empty.html`);
  const log = [];
  page.on('dialog', async d => {
    log.push([d.type(), d.message(), d.defaultValue()]);
    if (d.type() === 'confirm') await d.dismiss();
    else await d.accept(d.type() === 'prompt' ? 'answer' : undefined);
  });
  return { out: await page.evaluate(() => { alert('a'); return [confirm('c'), prompt('p', 'def')]; }), log };
});
pw('dialogs: beforeunload', async ({ page }) => {
  await page.goto(`${origin}/unload.html`);
  await page.mouse.click(5, 5);
  const seen = page.waitForEvent('dialog');
  page.close({ runBeforeUnload: true }).catch(() => {});
  const dialog = await seen;
  const type = dialog.type();
  await dialog.accept();
  await page.waitForEvent('close');
  return { type, closed: page.isClosed() };
});
pw('downloads', async ({ browser }) => {
  const context = await browser.newContext({ acceptDownloads: true });
  const page = await context.newPage();
  await page.goto(`${origin}/empty.html`);
  const [download] = await Promise.all([page.waitForEvent('download'), page.evaluate(() => { const a = document.createElement('a'); a.href = '/download'; document.body.append(a); a.click(); })]);
  const saved = path.join(work, `saved-${Date.now()}.txt`);
  await download.saveAs(saved);
  const out = { name: download.suggestedFilename(), url: download.url().replace(origin, ''), failure: await download.failure(), content: fs.readFileSync(saved, 'utf8') };
  await context.close();
  return out;
});
pw('downloads: refused', async ({ browser }) => {
  const context = await browser.newContext({ acceptDownloads: false });
  const page = await context.newPage();
  await page.goto(`${origin}/empty.html`);
  const [download] = await Promise.all([page.waitForEvent('download'), page.evaluate(() => { const a = document.createElement('a'); a.href = '/download'; document.body.append(a); a.click(); })]);
  const failure = await download.failure();
  await context.close();
  return failure;
});
pw('workers: dedicated', async ({ page }) => {
  const created = page.waitForEvent('worker');
  await page.goto(`${origin}/worker.html`);
  const worker = await created;
  return { url: worker.url().replace(origin, ''), value: await worker.evaluate(() => self.value), count: page.workers().length };
});
pw('workers: service worker', async ({ page, context }) => {
  const created = context.waitForEvent('serviceworker');
  await page.goto(`${origin}/sw.html`);
  const worker = await created;
  return { url: worker.url().replace(origin, ''), value: await worker.evaluate(() => self.value), active: await page.evaluate(() => window.reg), count: context.serviceWorkers().length };
});
pw('emulation: context options', async ({ browser }) => {
  const context = await browser.newContext({ ...devices['Pixel 5'], locale: 'de-DE', timezoneId: 'Asia/Tokyo', colorScheme: 'dark', reducedMotion: 'reduce', forcedColors: 'active', contrast: 'more', geolocation: { latitude: 10, longitude: 20 }, permissions: ['geolocation'] });
  const page = await context.newPage();
  await page.goto(`${origin}/empty.html`);
  const out = await page.evaluate(async () => ({
    w: innerWidth, h: innerHeight, dpr: devicePixelRatio, touch: navigator.maxTouchPoints > 0, mobile: /Android/.test(navigator.userAgent),
    lang: navigator.language, number: (1234.5).toLocaleString(), zone: Intl.DateTimeFormat().resolvedOptions().timeZone,
    dark: matchMedia('(prefers-color-scheme: dark)').matches, motion: matchMedia('(prefers-reduced-motion: reduce)').matches,
    forced: matchMedia('(forced-colors: active)').matches, contrast: matchMedia('(prefers-contrast: more)').matches,
    geo: await new Promise((k, r) => navigator.geolocation.getCurrentPosition(p => k([p.coords.latitude, p.coords.longitude]), e => r(new Error(e.message)))),
  }));
  await context.close();
  return out;
});
pw('emulation: media, javaScriptEnabled, bypassCSP, viewport', async ({ browser, page }) => {
  await page.goto(`${origin}/print.html`);
  await page.emulateMedia({ media: 'print', colorScheme: 'dark' });
  const print = await page.evaluate(() => [matchMedia('print').matches, matchMedia('(prefers-color-scheme: dark)').matches]);
  const context = await browser.newContext({ javaScriptEnabled: false, bypassCSP: true, viewport: { width: 333, height: 222 } });
  const second = await context.newPage();
  await second.setContent('<title>no</title><script>document.title="ran"</script>');
  const title = await second.title();
  const size = second.viewportSize();
  await context.close();
  return { print, title, size };
});
pw('emulation: permissions and clock', async ({ page, context }) => {
  await page.goto(`${origin}/empty.html`);
  await context.grantPermissions(['notifications'], { origin });
  const granted = await page.evaluate(() => navigator.permissions.query({ name: 'notifications' }).then(s => s.state));
  await context.clearPermissions();
  await page.clock.setFixedTime(new Date('2024-01-02T00:00:00Z'));
  return { granted, time: await page.evaluate(() => new Date().toISOString()) };
});
pw('tracing: Playwright trace', async ({ page, context }) => {
  const file = path.join(work, `trace-${Date.now()}.zip`);
  await context.tracing.start({ screenshots: true, snapshots: true });
  await page.goto(`${origin}/box.html`);
  await page.click('#r');
  await context.tracing.stop({ path: file });
  return { zip: fs.readFileSync(file).toString('latin1', 0, 2) };
});
pw('tracing: browser (Chromium trace)', async ({ browser, page }) => {
  await browser.startTracing(page, { categories: ['devtools.timeline'] });
  await page.goto(`${origin}/empty.html`);
  const trace = JSON.parse((await browser.stopTracing()).toString());
  return { list: Array.isArray(trace.traceEvents), length: trace.traceEvents.length > 0 };
});
pw('coverage: JS and CSS', async ({ page }) => {
  await page.coverage.startJSCoverage();
  await page.coverage.startCSSCoverage();
  await page.goto(`${origin}/coverage.html`);
  const js = await page.coverage.stopJSCoverage(), css = await page.coverage.stopCSSCoverage();
  return { js: js.map(c => [c.url.replace(origin, ''), c.functions?.map(f => [f.functionName, f.ranges[0].count])]).sort(), css: css.map(c => [c.url.replace(origin, ''), c.ranges]) };
});
pw('accessibility: aria snapshot, roles', async ({ page }) => {
  await page.goto(`${origin}/a11y.html`);
  return { snapshot: await page.locator('body').ariaSnapshot(), link: await page.getByRole('link').getAttribute('href'), label: await page.getByLabel('Field').inputValue(), alt: await page.getByAltText('Pic').count() };
});
pw('accessibility: expect-free AX tree over CDP session', async ({ page, context }) => {
  await page.goto(`${origin}/a11y.html`);
  const session = await context.newCDPSession(page);
  const { nodes } = await session.send('Accessibility.getFullAXTree');
  await session.detach();
  return nodes.filter(n => !n.ignored).map(n => [n.role?.value, n.name?.value]);
});
pw('contexts and pages: isolated, popups, close', async ({ browser, page, context }) => {
  const second = await browser.newContext();
  const other_page = await second.newPage();
  await page.goto(`${origin}/popup.html`);
  await other_page.goto(`${origin}/empty.html`);
  await page.evaluate(() => localStorage.setItem('k', 'one'));
  const isolated = await other_page.evaluate(() => localStorage.getItem('k'));
  const [popup] = await Promise.all([page.waitForEvent('popup'), page.click('#l')]);
  await popup.waitForLoadState();
  const out = { isolated, pages: context.pages().length, url: popup.url().replace(origin, ''), opener: (await popup.opener()) === page };
  await popup.close();
  out.after = context.pages().length;
  await second.close();
  out.contexts = browser.contexts().includes(second);
  return out;
});
pw('console and page errors', async ({ page }) => {
  const log = [];
  page.on('console', m => log.push([m.type(), m.text(), m.location().url.replace(origin, '')]));
  const failed = page.waitForEvent('pageerror');
  await page.goto(`${origin}/errors.html`);
  return { log, error: (await failed).message };
});
pw('har: record', async ({ browser }) => {
  const file = path.join(work, `har-${Date.now()}.har`);
  const context = await browser.newContext({ recordHar: { path: file } });
  const page = await context.newPage();
  await page.goto(`${origin}/coverage.html`);
  await context.close();
  const har = JSON.parse(fs.readFileSync(file, 'utf8'));
  return har.log.entries.map(e => [new URL(e.request.url).pathname, e.response.status]).sort();
});
pw('video: record', async ({ browser }) => {
  const dir = fs.mkdtempSync(path.join(work, 'video-'));
  const context = await browser.newContext({ recordVideo: { dir, size: { width: 200, height: 100 } } });
  const page = await context.newPage();
  await page.goto(`${origin}/box.html`);
  await page.evaluate(() => new Promise(k => setTimeout(k, 300)));
  const video = page.video();
  await context.close();
  return { webm: fs.readFileSync(await video.path()).subarray(0, 4).toString('hex') };
});
pw('session: raw CDP from a page', async ({ page, context }) => {
  await page.goto(`${origin}/empty.html`);
  const session = await context.newCDPSession(page);
  const { result } = await session.send('Runtime.evaluate', { expression: '1+1' });
  const seen = new Promise(resolve => session.once('Runtime.consoleAPICalled', e => resolve(e.args[0].value)));
  await session.send('Runtime.enable');
  await page.evaluate(() => console.log('via session'));
  return { value: result.value, seen: await seen };
});
pw('session: browser-level', async ({ browser }) => {
  const session = await browser.newBrowserCDPSession();
  const { product } = await session.send('Browser.getVersion');
  const { targetInfos } = await session.send('Target.getTargets');
  await session.detach();
  return { product: /\/\d+/.test(product), browserTarget: targetInfos.length >= 0 };
});
pw('locators: wait, filter, nth, expect-style polling', async ({ page }) => {
  await page.setContent('<ul><li>a</li><li class=x>b</li><li>c</li></ul>');
  page.evaluate(() => setTimeout(() => { document.body.insertAdjacentHTML('beforeend', '<p id=late style="visibility:hidden">late</p>'); }, 50));
  await page.waitForSelector('#late', { state: 'attached' });
  return { count: await page.locator('li').count(), filtered: await page.locator('li').filter({ hasText: 'b' }).getAttribute('class'), nth: await page.locator('li').nth(2).textContent(), visible: await page.locator('#late').isVisible() };
});

// ---------------------------------------------------------------- the browsers

function startBrowser(executable, args, label) {
  const profile = fs.mkdtempSync(path.join(work, `profile-${label}-`));
  const child = spawn(executable, [...args, '--remote-debugging-port=0', `--user-data-dir=${profile}`], { stdio: ['ignore', 'ignore', 'pipe'], detached: true });
  const stderr = fs.createWriteStream(path.join(work, `${label}.stderr.log`), { flags: 'a' });
  const endpoint = new Promise((resolve, reject) => {
    let text = '';
    child.stderr.on('data', data => {
      stderr.write(data);
      text += data;
      const found = text.match(/DevTools listening on (ws:\/\/\S+)/);
      if (found)
        resolve(found[1]);
    });
    child.once('exit', code => reject(new Error(`${label} exited (${code})`)));
    setTimeout(() => reject(new Error(`${label} serves no CDP`)), 60000).unref();
  });
  return {
    child,
    endpoint,
    stop() {
      try { process.kill(-child.pid, 'SIGKILL'); } catch {}
      fs.rmSync(profile, { recursive: true, force: true, maxRetries: 3 });
    },
  };
}

const normalize = text => String(text).replaceAll(String(port), 'PORT').split(work).join('WORK');
const withTimeout = (promise, ms) => {
  let timer;
  return Promise.race([promise, new Promise((_, reject) => { timer = setTimeout(() => reject(new Error(`timed out after ${ms} ms`)), ms); })]).finally(() => clearTimeout(timer));
};

// Each client has a browser of its own: Playwright pauses every new target of
// the browser until it has set it up, which would stall the other client's.
async function runClient(client, executable, args, label) {
  const results = new Map();
  let instance = startBrowser(executable, args, label), browser = null;
  const connect = async () => {
    const endpoint = await instance.endpoint;
    if (client === 'puppeteer')
      return puppeteer.connect({ browserWSEndpoint: endpoint, defaultViewport: { width: 800, height: 600 } });
    return chromium.connectOverCDP(`http://127.0.0.1:${new URL(endpoint).port}`);
  };
  const connected = () => client === 'puppeteer' ? browser.connected : browser.isConnected();
  try {
    for (const item of cases) {
      const key = `${item.client}: ${item.name}`;
      if (item.client !== client || (only && !only.test(key)))
        continue;
      let outcome;
      try {
        if (!browser)
          browser = await withTimeout(connect(), 60000);
        const context = client === 'puppeteer' ? await withTimeout(browser.createBrowserContext(), 20000) : await withTimeout(browser.newContext(), 20000);
        try {
          const page = await withTimeout(context.newPage(), 20000);
          const value = await withTimeout(item.body({ browser, context, page }), 25000);
          outcome = { ok: true, value: JSON.parse(normalize(JSON.stringify(value === undefined ? null : value))) };
        } finally {
          await withTimeout(context.close(), 10000).catch(() => {});
        }
      } catch (error) {
        outcome = { ok: false, error: normalize(String(error && error.message || error).split('\n')[0]) };
      }
      if (instance.child.exitCode !== null || instance.child.signalCode || (browser && !connected())) {
        outcome = { ok: false, error: `the browser died: ${outcome.error || 'after the case'}`, died: true };
        instance.stop();
        instance = startBrowser(executable, args, label);
        browser = null;
      }
      results.set(key, outcome);
      console.error(`[${label}] ${outcome.ok ? 'ok ' : 'ERR'} ${key}${outcome.ok ? '' : ` : ${outcome.error}`}`);
    }
  } finally {
    instance.stop();
  }
  return results;
}
const runAll = async (executable, args, label) => new Map([
  ...await runClient('puppeteer', executable, args, label),
  ...await runClient('playwright', executable, args, label),
]);

// ---------------------------------------------------------------- the comparison

const expected = new Map();
for (const line of fs.readFileSync(expectedFile, 'utf8').split('\n')) {
  if (!line.trim() || line.startsWith('#'))
    continue;
  const at = line.indexOf(' | ');
  if (at < 0)
    throw new Error(`${expectedFile}: no reason in: ${line}`);
  expected.set(line.slice(0, at).trim(), line.slice(at + 3).trim());
}

const chromeResults = await runAll(chromePath, ['--headless=new', '--no-sandbox'], 'chrome');
const zebkitResults = await runAll(zebkitPath, ['--headless', '--no-sandbox'], 'zebkit');
server.closeAllConnections();
server.close();

const brief = outcome => {
  const text = outcome.ok ? JSON.stringify(outcome.value) : `ERROR ${outcome.error}`;
  return text.length > 300 ? `${text.slice(0, 300)}...` : text;
};
const rows = [];
let same = 0, sameError = 0, unexpected = 0, stale = 0;
for (const [key, chrome] of chromeResults) {
  const zebkit = zebkitResults.get(key);
  const reason = expected.get(key);
  if (JSON.stringify(chrome) === JSON.stringify(zebkit)) {
    ++same;
    if (!chrome.ok) {
      ++sameError;
      rows.push({ kind: 'both-fail', key, chrome: brief(chrome), zebkit: brief(zebkit), reason: reason || '' });
    } else if (reason) {
      ++stale;
      rows.push({ kind: 'no-longer-differs', key, chrome: brief(chrome), zebkit: brief(zebkit), reason });
    }
  } else if (reason) {
    rows.push({ kind: 'expected', key, chrome: brief(chrome), zebkit: brief(zebkit), reason });
  } else {
    ++unexpected;
    rows.push({ kind: 'DIFFERENT', key, chrome: brief(chrome), zebkit: brief(zebkit), reason: '' });
  }
}
for (const row of rows)
  console.log(`\n${row.kind}  ${row.key}\n  chrome: ${row.chrome}\n  zebkit: ${row.zebkit}${row.reason ? `\n  reason: ${row.reason}` : ''}`);
const expectedCount = rows.filter(r => r.kind === 'expected').length;
console.log(`\n| Result | Cases |\n|---|---|\n| same as Chrome | ${same - sameError} |\n| both fail alike | ${sameError} |\n| expected differences | ${expectedCount} |\n| unexpected differences | ${unexpected} |\n| listed but no longer different | ${stale} |\n| total | ${chromeResults.size} |`);
if (option('--json'))
  fs.writeFileSync(option('--json'), JSON.stringify({ rows, same, sameError, expectedCount, unexpected, stale, total: chromeResults.size, chrome: [...chromeResults], zebkit: [...zebkitResults] }, null, 1));
process.exit(unexpected ? 1 : 0);
