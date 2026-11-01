// tools/zebkit-mcp driven by the official MCP client (@modelcontextprotocol/sdk)
// over standard input and over HTTP: every tool, its refusals and the bounds'
// escapes, and raw requests for what a client would not send. The SDK speaks the
// handshake revisions; the current revision (2026-07-28), which no SDK of npm's
// speaks yet, is checked by hand here on both transports.
//
//   node mcp_server.mjs <dir with node_modules> <zebkit-mcp> [engine switches...]
//
// Exit 0 when every check passes. Run by scripts/engine-test.sh.

import { execFileSync, spawn } from 'node:child_process';
import crypto from 'node:crypto';
import dgram from 'node:dgram';
import fs from 'node:fs';
import http from 'node:http';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  check, finish, httpClient, loadSdk, makePdf, signedPdf, startHttp, stdioClient, stop, wrap,
} from './mcp_support.mjs';

const [toolsDir, binary, ...switches] = process.argv.slice(2);
const here = path.dirname(fileURLToPath(import.meta.url));
const sdk = loadSdk(toolsDir);
const engine = ['--', ...switches];
const work = fs.mkdtempSync(path.join(os.tmpdir(), 'zebkit-mcp-test-'));
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const transcript = [];

// ---- What the pages reach ----------------------------------------------------

const hits = [];                 // What reached the server no page may reach.
const provider = { polls: 0, requests: [], token: 'tok-' + crypto.randomBytes(16).toString('hex'), refresh: 'ref-' + crypto.randomBytes(16).toString('hex'), deviceCode: 'dev-' + crypto.randomBytes(16).toString('hex') };

const PAGE = `<!doctype html><title>Form page</title>
<h1>Test page</h1><p>Some <b>bold</b> text and <a href="/second">a link</a>.</p>
<ul><li>one</li><li>two</li></ul><table><tr><th>K</th><th>V</th></tr><tr><td>a</td><td>1</td></tr></table>
<form onsubmit="event.preventDefault(); document.title='sent ' + n.value + ' ' + s.value + ' ' + c.checked + ' ' + (f.files[0] ? f.files[0].name : '')">
<label>Name <input id=n name=n></label>
<label>Size <select id=s name=s><option value=small>Small<option value=big>Big</select></label>
<label><input type=checkbox id=c> Agree</label>
<label>File <input type=file id=f></label>
<button id=send>Send</button></form>
<button id=hover onmouseenter="this.textContent='hovered'">Hover me</button>
<button id=popup onclick="window.open('/second')">Popup</button>
<a id=download href="/file.bin" download>Download</a>
<button id=askcamera onclick="navigator.mediaDevices.getUserMedia({video:true}).catch(e=>{document.title='camera: '+e.name})">Camera</button>
<div style="height:4000px"></div><p id=end>The end</p>`;

function requestHandler(request, response) {
  const url = new URL(request.url, 'http://x');
  const send = (body, type = 'text/html') => { response.writeHead(200, { 'Content-Type': type }); response.end(body); };
  switch (url.pathname) {
    case '/page': return send(PAGE);
    case '/second': return send('<!doctype html><title>Second</title><h1>Second page</h1>');
    case '/file.bin': response.writeHead(200, { 'Content-Type': 'application/octet-stream', 'Content-Disposition': 'attachment' }); return response.end('bytes');
    case '/wide': return send('é'.repeat(100), 'text/plain; charset=utf-8');
    case '/data.json': return send('{"hello": "world"}', 'application/json');
    case '/to-outside': response.writeHead(302, { Location: `http://127.0.0.1:${outsidePort}/redirected` }); return response.end();
    case '/to-second': response.writeHead(302, { Location: '/second' }); return response.end();
    case '/worker.js': response.writeHead(200, { 'Content-Type': 'text/javascript' }); return response.end(`new WebSocket('ws://localhost:${outsidePort}/worker-socket');`);
    case '/ping': return send(`<!doctype html><a id=link href="/second" ping="http://localhost:${outsidePort}/ping">go</a>`);
    case '/repeat': return send(`<!doctype html><title>r</title><script>window.seen = 0;</script><button id=b onclick="window.seen++">B</button>`);
    case '/echo': { let body = ''; request.on('data', d => { body += d; }); request.on('end', () => send(`${request.method} ${body}`, 'text/plain')); return undefined; }
    default: response.writeHead(404); return response.end('no');
  }
}
const site = http.createServer(requestHandler);
const outside = http.createServer((request, response) => {
  hits.push({ url: request.url, authorization: request.headers.authorization, apiKey: request.headers['x-api-key'], contentType: request.headers['content-type'] });
  response.writeHead(200, { 'Content-Type': 'text/plain', 'Access-Control-Allow-Origin': '*' });
  response.end('outside');
});
outside.on('upgrade', (request, socket) => { hits.push({ url: request.url, websocket: true }); socket.destroy(); });

const oauth = http.createServer((request, response) => {
  let body = '';
  request.on('data', d => { body += d; });
  request.on('end', () => {
    const url = new URL(request.url, 'http://x');
    provider.requests.push({ path: url.pathname, body, authorization: request.headers.authorization });
    const json = (status, value) => { response.writeHead(status, { 'Content-Type': 'application/json' }); response.end(JSON.stringify(value)); };
    if (url.pathname === '/device') {
      return json(200, { device_code: provider.deviceCode, user_code: 'WDJB-MJHT', verification_uri: `http://127.0.0.1:${oauthPort}/verify`, expires_in: 600, interval: 1 });
    }
    if (url.pathname === '/token') {
      provider.polls++;
      if (provider.polls === 1) return json(400, { error: 'authorization_pending' });
      return json(200, { access_token: provider.token, refresh_token: provider.refresh, token_type: 'bearer', expires_in: 3600 });
    }
    if (url.pathname === '/huge') { response.writeHead(200, { 'Content-Type': 'application/json' }); return response.end(`{"device_code":"x","user_code":"U","verification_uri":"https://auth.example/v","expires_in":18446744073709551615,"interval":18446744073709551615}`); }
    if (url.pathname === '/long') return json(200, { device_code: 'x', user_code: 'U', verification_uri: 'https://auth.example/v', interval: 1 });
    if (url.pathname === '/long-token') { response.writeHead(200, { 'Content-Type': 'application/json' }); return response.end(`{"access_token":"${provider.token}","expires_in":18446744073709551615}`); }
    if (url.pathname === '/slow-token') {
      provider.slowPolls = (provider.slowPolls ?? 0) + 1;
      if (provider.slowPolls > 1) return json(400, { error: 'invalid_grant' });
      return setTimeout(() => json(200, { access_token: provider.token }), 2500);
    }
    if (url.pathname === '/api/echo-header') { response.writeHead(200, { 'Content-Type': 'text/plain', 'X-Echo': request.headers.authorization ?? '' }); return response.end('see the header'); }
    if (url.pathname === '/api/echo-binary') { response.writeHead(200); return response.end(Buffer.concat([Buffer.from([0xff, 0xfe]), Buffer.from(request.headers.authorization ?? '')])); }
    if (url.pathname === '/api/echo') { response.writeHead(200, { 'Content-Type': 'text/plain' }); return response.end(`header was ${request.headers.authorization}`); }
    if (url.pathname === '/api/redirect') { response.writeHead(302, { Location: `http://127.0.0.1:${outsidePort}/stolen` }); return response.end(); }
    return json(404, {});
  });
});
const listen = server => new Promise(resolve => server.listen(0, '127.0.0.1', () => resolve(server.address().port)));
const sitePort = await listen(site), outsidePort = await listen(outside), oauthPort = await listen(oauth);
const SITE = `http://127.0.0.1:${sitePort}`, OUTSIDE = `http://127.0.0.1:${outsidePort}`, OAUTH = `http://127.0.0.1:${oauthPort}`;

// ---- Files --------------------------------------------------------------------

const readDir = path.join(work, 'read'), writeDir = path.join(work, 'write'), outsideDir = path.join(work, 'outside');
for (const dir of [readDir, writeDir, outsideDir]) fs.mkdirSync(dir);
fs.writeFileSync(path.join(readDir, 'hello.txt'), 'hello file');
fs.writeFileSync(path.join(outsideDir, 'secret.txt'), 'the secret');
fs.symlinkSync(path.join(outsideDir, 'secret.txt'), path.join(readDir, 'link-out'));
fs.symlinkSync(outsideDir, path.join(readDir, 'dir-out'));
fs.symlinkSync(outsideDir, path.join(writeDir, 'dir-out'));
fs.symlinkSync(path.join(outsideDir, 'target.txt'), path.join(writeDir, 'dangling-out'));
fs.symlinkSync(path.join(readDir, 'hello.txt'), path.join(readDir, 'link-in'));
execFileSync('mkfifo', [path.join(readDir, 'pipe')]);
fs.copyFileSync(path.join(here, 'pages/media/clip.mp4'), path.join(readDir, 'clip.mp4'));
fs.writeFileSync(path.join(readDir, 'plain.pdf'), makePdf([['First page line', 'needle in here'], ['Second page'], ['Third page']], { form: true }));
fs.writeFileSync(path.join(readDir, 'other.pdf'), makePdf([['Other doc']]));
fs.writeFileSync(path.join(readDir, 'signed.pdf'), signedPdf(here));

const oauthConfig = path.join(work, 'oauth.json');
fs.writeFileSync(oauthConfig, JSON.stringify({
  test: { clientId: 'client-1', clientSecret: 'shh-secret', deviceEndpoint: `${OAUTH}/device`, tokenEndpoint: `${OAUTH}/token`, apiOrigins: [OAUTH], scopes: ['read', 'write'] },
  huge: { clientId: 'c', deviceEndpoint: `${OAUTH}/huge`, tokenEndpoint: `${OAUTH}/token`, apiOrigins: [OAUTH] },
  long: { clientId: 'c', deviceEndpoint: `${OAUTH}/long`, tokenEndpoint: `${OAUTH}/long-token`, apiOrigins: [OAUTH] },
  slow: { clientId: 'c', deviceEndpoint: `${OAUTH}/long`, tokenEndpoint: `${OAUTH}/slow-token`, apiOrigins: [OAUTH] },
}), { mode: 0o600 });

const fullBounds = [
  '--allow-origin', SITE, '--allow-origin', OAUTH, '--allow-read', readDir, '--allow-write', writeDir,
  '--oauth-config', oauthConfig,
];

// ---- Every tool, through a client ---------------------------------------------

async function everyTool(label, c) {
  const say = what => `${label}: ${what}`;
  const used = new Set();
  const call = (name, args) => { used.add(name); return c.call(name, args); };
  const refused = async (name, args, pattern, what) => {
    const answer = await call(name, args);
    check(answer.isError && pattern.test(answer.text), say(what ?? `${name} refused`), answer.text);
  };

  const { tools } = await c.client.listTools();
  check(tools.length > 50 && tools.every(tool => tool.inputSchema?.type === 'object'), say(`tools/list: ${tools.length} tools with schemas`));

  // Arguments: nothing outside a tool's schema gets through, for every tool.
  let unknown = 0, missing = 0, wrong = 0;
  for (const tool of tools) {
    const bad = await c.call(tool.name, { no_such_argument: 1 });
    if (bad.isError && /no_such_argument: not an argument|is required/.test(bad.text)) unknown++;
    else check(false, say(`${tool.name} takes an unknown argument`), bad.text);
    if (tool.inputSchema.required?.length) {
      const none = await c.call(tool.name, {});
      if (none.isError && /is required/.test(none.text)) missing++;
      else check(false, say(`${tool.name} runs without its required argument`), none.text);
    }
    const [first, schema] = Object.entries(tool.inputSchema.properties ?? {})[0] ?? [];
    if (first && schema.type) {
      const value = schema.type === 'string' ? 12 : 'a string';
      const typed = await c.call(tool.name, { [first]: value });
      if (typed.isError && /expected|required/.test(typed.text)) wrong++;
      else check(false, say(`${tool.name} takes a wrong type for ${first}`), typed.text);
    }
  }
  check(unknown === tools.length, say(`every tool refuses an unknown argument (${unknown})`));
  check(missing > 0, say(`every tool needs its required arguments (${missing})`));
  check(wrong > 0, say(`every tool checks a type (${wrong})`));
  await refused('page_navigate', { page: 'p1', url: 'x'.repeat(70000) }, /longer than/, 'a string over its limit');
  await refused('pdf_render', { document: 'd1', scale: 1000 }, /over the maximum/, 'a number over its maximum');

  // ---- Pages
  await refused('page_open', { url: `${OUTSIDE}/page` }, /outside the origins/, 'page_open: a disallowed origin');
  await refused('page_open', { url: 'file:///etc/passwd' }, /scheme file/, 'page_open: file:');
  await refused('page_open', { url: 'data:text/html,hi' }, /scheme data/, 'page_open: data:');
  await refused('page_open', { window: true }, /windows are denied/, 'page_open: a window without --windows');
  const opened = await call('page_open', { url: `${SITE}/page`, width: 900, height: 700 });
  check(!opened.isError && opened.structured.status === 200 && opened.structured.title === 'Form page', say('page_open'), opened.text);
  const page = opened.structured.page;
  const other = await call('page_open', { url: `${SITE}/second`, profile: 'other' });
  check(other.structured.profile === 'other', say('page_open: a second profile'), other.text);
  const shared = await call('page_evaluate', { page: other.structured.page, script: "document.cookie = 'p=1'; document.cookie" });
  const isolated = await call('page_evaluate', { page, script: 'document.cookie' });
  check(shared.structured.value === 'p=1' && isolated.structured.value === '', say('profiles do not share cookies'), isolated.text);
  const list = await call('page_list', {});
  check(list.structured.pages?.length >= 2 && list.structured.windows === false, say('page_list'), list.text);

  const aria = await call('page_read', { page });
  const ref = role => aria.structured.text.match(new RegExp(`${role}[^\\n]*\\[ref=(e\\d+)\\]`))?.[1];
  check(ref('heading "Test page"') && ref('button "Send"'), say('page_read aria, with references'), aria.structured.text);
  const words = await call('page_read', { page, format: 'text' });
  check(/Test page/.test(words.text) && /The end/.test(words.text) && words.structured.untrusted, say('page_read text'));
  const markdown = await call('page_read', { page, format: 'markdown' });
  check(/^# Test page/.test(markdown.text) && /\*\*bold\*\*/.test(markdown.text) && /\[a link\]\(.*\/second\)/.test(markdown.text) && /- one/.test(markdown.text) && /\| K \| V \|/.test(markdown.text), say('page_read markdown'), markdown.text);
  const piece = await call('page_read', { page, format: 'text', maxChars: 5 });
  check(piece.structured.truncated && piece.structured.nextOffset === 5 && piece.structured.text.length === 5, say('page_read in pieces'));
  const element = await call('page_read', { page, format: 'text', ref: ref('heading "Test page"') });
  check(element.text === 'Test page', say('page_read an element by ref'), element.text);

  const typed = await call('page_type', { page, ref: ref('textbox "Name"'), text: 'Ada' });
  check(!typed.isError, say('page_type by ref'), typed.text);
  const select = await call('page_select', { page, ref: ref('combobox "Size"'), values: ['Big'] });
  check(!select.isError, say('page_select by ref'), select.text);
  const checked = await call('page_check', { page, ref: ref('checkbox "Agree"') });
  check(!checked.isError, say('page_check by ref'), checked.text);
  const size = await call('page_evaluate', { page, script: 'innerWidth + "x" + innerHeight' });
  check(size.structured.value === '900x700', say('page_open: the size asked for, and it stays'), size.text);
  await refused('page_upload', { page, selector: '#f', paths: [path.join(readDir, 'link-out')] }, /outside/, 'page_upload: a link out of the grant');
  await refused('page_upload', { page, selector: '#f', paths: [path.join(outsideDir, 'secret.txt')] }, /outside/, 'page_upload: outside');
  const upload = await call('page_upload', { page, selector: '#f', paths: [path.join(readDir, 'hello.txt')] });
  check(!upload.isError, say('page_upload'), upload.text);
  const uploaded = await call('page_evaluate', { page, script: "document.querySelector('#f').files[0].text()" });
  check(uploaded.structured.value === 'hello file', say('page_upload: the page can read the file'), uploaded.text);
  const sentUp = await call('page_evaluate', { page, script: "fetch('/echo', {method: 'POST', body: document.querySelector('#f').files[0]}).then(r => r.text())" });
  check(sentUp.structured.value === 'POST hello file', say('page_upload: the file goes to the server'), sentUp.text);
  const click = await call('page_click', { page, ref: ref('button "Send"') });
  const title = await call('page_evaluate', { page, script: 'document.title' });
  check(!click.isError && title.structured.value === 'sent Ada big true hello.txt', say('page_click submits what was filled'), title.text);
  const pressed = await call('page_press', { page, selector: '#n', key: 'End' });
  check(!pressed.isError, say('page_press on an element'), pressed.text);
  const pressedPage = await call('page_press', { page, key: 'Tab' });
  check(!pressedPage.isError, say('page_press on the page'), pressedPage.text);
  await call('page_hover', { page, selector: '#hover' });
  const hovered = await call('page_evaluate', { page, script: "document.querySelector('#hover').textContent" });
  check(hovered.structured.value === 'hovered', say('page_hover'), hovered.text);
  await call('page_scroll', { page, deltaY: 300 });
  await sleep(100);
  const scrolled = await call('page_evaluate', { page, script: 'scrollY' });
  check(scrolled.structured.value > 0, say('page_scroll by a distance'), scrolled.text);
  await call('page_scroll', { page, selector: '#end' });
  const further = await call('page_evaluate', { page, script: 'scrollY' });
  check(further.structured.value > scrolled.structured.value, say('page_scroll to an element'), further.text);
  const waited = await call('page_wait', { page, selector: '#end' });
  check(!waited.isError, say('page_wait for a selector'));
  check(!(await call('page_wait', { page, text: 'The end' })).isError, say('page_wait for text'));
  check(!(await call('page_wait', { page, until: 'load' })).isError, say('page_wait for a load state'));
  const never = await call('page_wait', { page, selector: '#nothing', timeout: 100 });
  check(never.isError && /Timeout/.test(never.text), say('page_wait times out'), never.text);
  const shot = await call('page_capture', { page });
  check(shot.image?.mimeType === 'image/png' && Buffer.from(shot.image.data, 'base64').subarray(1, 4).toString() === 'PNG', say('page_capture'));
  const small = await call('page_capture', { page, selector: '#send' });
  check(small.image && small.structured.bytes < shot.structured.bytes, say('page_capture an element'));
  await refused('page_capture', { page, path: path.join(outsideDir, 'shot.png') }, /outside/, 'page_capture: a path outside');
  const onFile = await call('page_capture', { page, path: path.join(writeDir, `shot-${label}.png`) });
  check(fs.existsSync(path.join(writeDir, `shot-${label}.png`)), say('page_capture into a granted file'), onFile.text);

  const evaluated = await call('page_evaluate', { page, script: '(a) => a + 1', arg: 41 });
  check(evaluated.structured.value === 42, say('page_evaluate with an argument'), evaluated.text);
  const thrown = await call('page_evaluate', { page, script: "throw new Error('boom')" });
  check(thrown.isError && /boom/.test(thrown.text), say('page_evaluate reports what is thrown'), thrown.text);
  await call('page_evaluate', { page, script: "console.log('from the page'); 1" });
  const consoleLog = await call('page_console', { page });
  check(JSON.stringify(consoleLog.structured.items).includes('from the page'), say('page_console'), consoleLog.text);
  const network = await call('page_network', { page });
  check(network.structured.items.some(item => item.url === `${SITE}/page` && item.status === 200), say('page_network'), network.text);

  await call('page_evaluate', { page, script: "setTimeout(() => { window.answer = confirm('sure?'); }, 0); 1" });
  let dialogs = 0;
  for (let i = 0; i < 100 && !dialogs; i++) {
    dialogs = (await c.call('page_list', {})).structured.pages.find(item => item.page === page)?.openDialogs;
    if (!dialogs) await sleep(50);
  }
  const answered = await call('page_dialog', { page, accept: true });
  const answer = await call('page_evaluate', { page, script: 'window.answer' });
  check(answered.structured.answered === 'sure?' && answer.structured.value === true, say('page_dialog'), answered.text);
  await refused('page_dialog', { page }, /no dialog/, 'page_dialog with none open');

  const nextPage = await call('page_navigate', { page, url: `${SITE}/second` });
  check(nextPage.structured.title === 'Second', say('page_navigate'), nextPage.text);
  const back = await call('page_history', { page, action: 'back' });
  check(back.structured.title === 'Form page', say('page_history back'), back.text);
  const forward = await call('page_history', { page, action: 'forward' });
  check(forward.structured.title === 'Second', say('page_history forward'), forward.text);
  const reload = await call('page_history', { page, action: 'reload' });
  check(!reload.isError, say('page_history reload'));
  await refused('page_navigate', { page, url: `${OUTSIDE}/x` }, /outside the origins/, 'page_navigate: a disallowed origin');
  await refused('page_click', { page: 'p999', selector: '#x' }, /no page/, 'a page that is not there');

  // ---- What a page tries: none of it widens the bounds.
  await call('page_navigate', { page, url: `${SITE}/page` });
  const hitsBefore = hits.length;
  const before = await call('page_network', { page, clear: true });
  check(!before.isError, say('page_network clear'));
  const typeofZebkit = await call('page_evaluate', { page, script: 'typeof zebkit' });
  check(typeofZebkit.structured.value === 'undefined', say('a page has no zebkit.*'), typeofZebkit.text);
  const fetchOut = await call('page_evaluate', { page, script: `fetch('${OUTSIDE}/fetch').then(r => 'reached ' + r.status, e => 'failed')` });
  check(fetchOut.structured.value === 'failed', say('a page: fetch to a disallowed origin'), fetchOut.text);
  await call('page_evaluate', { page, script: `new Image().src = '${OUTSIDE}/image'; 1` });
  await call('page_evaluate', { page, script: `const f = document.createElement('iframe'); f.src = '${OUTSIDE}/frame'; document.body.append(f); 1` });
  await call('page_evaluate', { page, script: `const w = new WebSocket('${OUTSIDE.replace('http', 'ws')}/socket'); 1` });
  await call('page_evaluate', { page, script: `const u = new WebSocket('ws://localhost:${outsidePort}/socket'); 1` });
  await call('page_evaluate', { page, script: `new Worker(URL.createObjectURL(new Blob(["fetch('${OUTSIDE}/worker').catch(() => 0)"]))); 1` });
  const sw = await call('page_evaluate', { page, script: "typeof navigator.serviceWorker + ' ' + typeof SharedWorker" });
  check(sw.structured.value === 'undefined undefined', say('a page has no service or shared workers'), sw.text);
  await call('page_evaluate', { page, script: `setTimeout(() => { location = '${OUTSIDE}/location'; }, 0); 1` });
  await sleep(1000);
  const stayed = await call('page_list', {});
  check(stayed.structured.pages.find(item => item.page === page).url === `${SITE}/page`, say('a page cannot navigate itself outside'), stayed.text);
  await call('page_evaluate', { page, script: `setTimeout(() => { location = 'file:///etc/passwd'; }, 0); 1` });
  await sleep(300);
  check(hits.length === hitsBefore, say('nothing reached the disallowed origin'), hits.slice(hitsBefore));
  const redirected = await call('page_navigate', { page, url: `${SITE}/to-outside` });
  check(redirected.isError && /outside the origins/.test(redirected.text) && hits.length === hitsBefore, say('a redirect out is stopped'), redirected.text);
  await call('page_navigate', { page, url: `${SITE}/page` });
  const refusedOf = async what => {
    let log;
    for (let i = 0; i < 100; i++) {
      log = await c.call('page_network', { page });
      if (log.structured.refusedByBounds?.some(item => item.what === what)) return log;
      await sleep(50);
    }
    return log;
  };
  await call('page_click', { page, selector: '#popup' });
  const log1 = await refusedOf('popup');
  check(log1.structured.refusedByBounds.some(item => item.what === 'popup'), say('a popup is refused and reported'), log1.text);
  const windows = (await call('page_list', {})).structured.pages.length;
  await call('page_click', { page, selector: '#download' });
  const log2 = await refusedOf('download');
  check(log2.structured.refusedByBounds.some(item => item.what === 'download'), say('a download is refused and reported'), log2.text);
  await call('page_click', { page, selector: '#f', timeout: 5000 });
  const log3 = await refusedOf('file chooser');
  check(log3.structured.refusedByBounds.some(item => item.what === 'file chooser'), say("a page's file chooser is refused"), log3.text);
  await call('page_click', { page, selector: '#askcamera' });
  let camera;
  for (let i = 0; i < 100; i++) {
    camera = await c.call('page_evaluate', { page, script: 'document.title' });
    if (/^camera: /.test(camera.structured.value)) break;
    await sleep(50);
  }
  check(/^camera: /.test(camera.structured.value), say('a page cannot have the camera'), camera.text);
  check(windows === (await call('page_list', {})).structured.pages.length, say('no window opened behind the agent'));

  // ---- PDF
  await refused('pdf_open', { path: path.join(readDir, 'link-out') }, /outside/, 'pdf_open: a link out of the grant');
  await refused('pdf_open', { path: path.join(outsideDir, 'secret.txt') }, /outside/, 'pdf_open: outside');
  await refused('pdf_open', { base64: Buffer.from('not a pdf').toString('base64') }, /./, 'pdf_open: not a PDF');
  const doc = await call('pdf_open', { path: path.join(readDir, 'plain.pdf') });
  check(doc.structured.document && doc.structured.info.pages === 3, say('pdf_open'), doc.text);
  const document = doc.structured.document;
  const info = await call('pdf_info', { document, pageIndex: 0 });
  check(info.structured.page?.width === 612, say('pdf_info'), info.text);
  const read = await call('pdf_read', { document, firstPage: 0, lastPage: 1 });
  check(/needle/.test(read.text) && /Second page/.test(read.text), say('pdf_read'), read.text);
  const lines = await call('pdf_read', { document, layout: 'lines' });
  check(JSON.stringify(lines.structured).includes('First page line'), say('pdf_read with its layout, lines'), lines.text);
  const chars = await call('pdf_read', { document, layout: 'chars' });
  check(chars.structured.pages[0].layout !== undefined && !chars.isError, say('pdf_read with its layout, characters'));
  await refused('pdf_read', { document, firstPage: 5 }, /document has pages/, 'pdf_read: a page not there');
  const found = await call('pdf_search', { document, query: 'needle' });
  check(found.structured.matches?.length === 1, say('pdf_search'), found.text);
  const render = await call('pdf_render', { document, pageIndex: 0 });
  check(render.image?.mimeType === 'image/png' && render.structured.width > 600, say('pdf_render'), render.text);
  await refused('pdf_render', { document, pageIndex: 0, scale: 16 }, /over the limit/, 'pdf_render: too many pixels');
  await refused('pdf_render', { document, pageIndex: 0, path: path.join(outsideDir, 'p.png') }, /outside/, 'pdf_render: a path outside');
  const outline = await call('pdf_outline', { document });
  check(JSON.stringify(outline.structured).includes('Chapter 2'), say('pdf_outline'), outline.text);
  const links = await call('pdf_links', { document, pageIndex: 0 });
  check(JSON.stringify(links.structured).includes('https://example.com/doc'), say('pdf_links'), links.text);
  const fields = await call('pdf_form_fields', { document });
  check(JSON.stringify(fields.structured).includes('"name"') && JSON.stringify(fields.structured).includes('old'), say('pdf_form_fields'), fields.text);
  const setField = await call('pdf_form_set', { document, fields: { name: 'new value' } });
  const afterSet = await call('pdf_form_fields', { document });
  check(setField.structured.set === 1 && JSON.stringify(afterSet.structured).includes('new value'), say('pdf_form_set'), setField.text);
  await refused('pdf_form_set', { document, fields: { nosuchfield: 'x' } }, /./, 'pdf_form_set: no such field');
  const added = await call('pdf_annotate', { document, pageIndex: 0, action: 'add', annotation: { type: 'square', rect: [50, 50, 150, 150], contents: 'a note' } });
  check(Number.isInteger(added.structured.index), say('pdf_annotate add'), added.text);
  const annotations = await call('pdf_annotations', { document, pageIndex: 0 });
  check(JSON.stringify(annotations.structured).includes('a note'), say('pdf_annotations'), annotations.text);
  const updated = await call('pdf_annotate', { document, pageIndex: 0, action: 'update', index: added.structured.index, annotation: { contents: 'changed' } });
  check(!updated.isError, say('pdf_annotate update'), updated.text);
  const removed = await call('pdf_annotate', { document, pageIndex: 0, action: 'remove', index: added.structured.index });
  check(!removed.isError, say('pdf_annotate remove'), removed.text);
  await refused('pdf_annotate', { document, pageIndex: 0, action: 'add', annotation: { type: 'nonsense' } }, /./, 'pdf_annotate: a type not there');
  const redacted = await call('pdf_redact', { document, pageIndex: 0, areas: [[0, 0, 612, 792]] });
  const gone = await call('pdf_read', { document });
  check(!redacted.isError && !/needle/.test(gone.text), say('pdf_redact takes the text out'), gone.text);
  const rotated = await call('pdf_edit_pages', { document, action: 'rotate', pageIndex: 1, rotation: 90 });
  check(!rotated.isError, say('pdf_edit_pages rotate'), rotated.text);
  const inserted = await call('pdf_edit_pages', { document, action: 'insert', at: 0 });
  check(inserted.structured.pages === 4, say('pdf_edit_pages insert'), inserted.text);
  const moved = await call('pdf_edit_pages', { document, action: 'move', pages: [0], at: 3 });
  check(!moved.isError, say('pdf_edit_pages move'), moved.text);
  const deleted = await call('pdf_edit_pages', { document, action: 'delete', pages: [3] });
  check(deleted.structured.pages === 3, say('pdf_edit_pages delete'), deleted.text);
  const otherDoc = await call('pdf_open', { path: path.join(readDir, 'other.pdf') });
  const imported = await call('pdf_edit_pages', { document, action: 'import', from: otherDoc.structured.document, pages: [0] });
  check(imported.structured.pages === 4, say('pdf_edit_pages import'), imported.text);
  const empty = await call('pdf_open', {});
  check(empty.structured.document, say('pdf_open: a new one'), empty.text);
  const unsignedSignatures = await call('pdf_signatures', { document });
  check(unsignedSignatures.structured.signatures?.length === 0, say('pdf_signatures: none'), unsignedSignatures.text);
  const signed = await call('pdf_open', { path: path.join(readDir, 'signed.pdf') });
  const signatures = await call('pdf_signatures', { document: signed.structured.document });
  check(signatures.structured.signatures?.[0]?.intact === true, say('pdf_signatures: intact'), signatures.text);
  await refused('pdf_save', { document, path: path.join(outsideDir, 'out.pdf') }, /outside/, 'pdf_save: outside');
  await refused('pdf_save', { document, path: path.join(writeDir, 'dangling-out') }, /symbolic link/, 'pdf_save: over a link');
  const savedPath = path.join(writeDir, `saved-${label}.pdf`);
  const saved = await call('pdf_save', { document, path: savedPath });
  check(saved.structured.bytes > 100 && fs.readFileSync(savedPath).subarray(0, 4).toString() === '%PDF', say('pdf_save'), saved.text);
  const again = await call('pdf_save', { document, path: savedPath });
  check(again.isError && /exists/.test(again.text), say('pdf_save will not replace without overwrite'), again.text);
  await call('page_navigate', { page, url: `${SITE}/second` });
  await refused('page_pdf', { page, path: path.join(outsideDir, 'p.pdf') }, /outside/, 'page_pdf: outside');
  const printed = await call('page_pdf', { page, path: path.join(writeDir, `printed-${label}.pdf`) });
  const printedText = await call('pdf_read', { document: printed.structured.document });
  check(/Second page/.test(printedText.text), say('page_pdf: a page printed and read back'), printed.text);
  const closed = await call('pdf_close', { document });
  check(!closed.isError, say('pdf_close'));
  await refused('pdf_info', { document }, /no document/, 'a closed document is gone');

  // ---- Files and data
  const hello = await call('file_read', { path: path.join(readDir, 'hello.txt') });
  check(hello.structured.text === 'hello file', say('file_read'), hello.text);
  const linkIn = await call('file_read', { path: path.join(readDir, 'link-in') });
  check(linkIn.structured.text === 'hello file', say('file_read: a link that stays inside'), linkIn.text);
  const encoded = await call('file_read', { path: path.join(readDir, 'hello.txt'), encoding: 'base64', offset: 6, maxBytes: 4 });
  check(Buffer.from(encoded.structured.base64, 'base64').toString() === 'file', say('file_read a piece, in base64'), encoded.text);
  await refused('file_read', { path: path.join(readDir, 'link-out') }, /outside/, 'file_read: a symbolic link out');
  await refused('file_read', { path: path.join(readDir, 'dir-out', 'secret.txt') }, /outside/, 'file_read: through a linked directory');
  await refused('file_read', { path: path.join(readDir) + '/../outside/secret.txt' }, /\.\. is refused/, 'file_read: ..');
  await refused('file_read', { path: path.join(outsideDir, 'secret.txt') }, /outside/, 'file_read: outside');
  await refused('file_read', { path: 'relative.txt' }, /absolute/, 'file_read: a relative path');
  await refused('file_read', { path: '/etc/passwd' }, /outside/, 'file_read: /etc/passwd');
  await refused('file_read', { path: path.join(readDir, 'pipe') }, /not a regular file/, 'file_read: a pipe, without blocking');
  await refused('file_read', { path: readDir }, /not a regular file/, 'file_read: a directory');
  await refused('file_write', { path: path.join(readDir, 'x.txt'), text: 'x' }, /outside/, 'file_write: a directory granted only for reading');
  await refused('file_write', { path: path.join(writeDir, 'dir-out', 'x.txt'), text: 'x' }, /outside/, 'file_write: through a linked directory');
  await refused('file_write', { path: path.join(writeDir, 'dangling-out'), text: 'x' }, /symbolic link/, 'file_write: a link to a new file outside');
  check(!fs.existsSync(path.join(outsideDir, 'target.txt')) && !fs.existsSync(path.join(outsideDir, 'x.txt')), say('nothing was written outside'));
  const fileName = path.join(writeDir, `new-${label}.txt`);
  const written = await call('file_write', { path: fileName, text: 'written' });
  check(fs.readFileSync(fileName, 'utf8') === 'written' && (fs.statSync(fileName).mode & 0o077) === 0, say('file_write, private'), written.text);
  await refused('file_write', { path: fileName, text: 'again' }, /exists/, 'file_write will not replace without overwrite');
  const replaced = await call('file_write', { path: fileName, base64: Buffer.from('bytes').toString('base64'), overwrite: true });
  check(fs.readFileSync(fileName, 'utf8') === 'bytes', say('file_write with overwrite'), replaced.text);
  check(fs.readdirSync(writeDir).every(name => !name.startsWith('.zebkit-mcp-')), say('no temporary files left'));
  const listed = await call('file_list', { path: readDir });
  check(listed.structured.entries.some(entry => entry.name === 'link-out' && entry.type === 'link'), say('file_list shows a link as a link'), listed.text);
  await refused('file_list', { path: path.join(readDir, 'dir-out') }, /outside/, 'file_list: a linked directory out');
  const stat = await call('file_stat', { path: path.join(readDir, 'hello.txt') });
  check(stat.structured.stat.size === 10, say('file_stat'), stat.text);
  const made = await call('file_mkdir', { path: path.join(writeDir, `dir-${label}`) });
  check(fs.statSync(path.join(writeDir, `dir-${label}`)).isDirectory(), say('file_mkdir'), made.text);
  await refused('file_mkdir', { path: path.join(outsideDir, 'dir') }, /outside/, 'file_mkdir: outside');

  const fetched = await call('net_fetch', { url: `${SITE}/data.json` });
  check(fetched.structured.status === 200 && fetched.structured.text === '{"hello": "world"}', say('net_fetch'), fetched.text);
  const posted = await call('net_fetch', { url: `${SITE}/echo`, method: 'POST', text: 'a body', headers: [{ name: 'Content-Type', value: 'text/plain' }] });
  check(posted.structured.text === 'POST a body', say('net_fetch POST'), posted.text);
  const follow = await call('net_fetch', { url: `${SITE}/to-second` });
  check(follow.structured.url === `${SITE}/second` && /Second page/.test(follow.structured.text), say('net_fetch follows a redirect inside'), follow.text);
  const fileOut = await call('net_fetch', { url: `${SITE}/data.json`, outputPath: path.join(writeDir, `fetched-${label}.json`) });
  check(fs.existsSync(path.join(writeDir, `fetched-${label}.json`)), say('net_fetch into a granted file'), fileOut.text);
  await refused('net_fetch', { url: `${SITE}/data.json`, outputPath: path.join(outsideDir, 'x') }, /outside/, 'net_fetch: output outside');
  await refused('net_fetch', { url: `${OUTSIDE}/direct` }, /outside the origins/, 'net_fetch: a disallowed origin');
  await refused('net_fetch', { url: 'file:///etc/passwd' }, /scheme file/, 'net_fetch: file:');
  await refused('net_fetch', { url: `${SITE}/x`, headers: [{ name: 'X-A', value: 'a\r\nInjected: 1' }] }, /not a valid header/, 'net_fetch: a header injection');
  await refused('net_fetch', { url: `${SITE}/x`, headers: [{ name: 'Host', value: 'other' }] }, /set by the network/, 'net_fetch: Host');
  await refused('net_fetch', { url: `${SITE}/x`, path: path.join(readDir, 'link-out'), method: 'POST' }, /outside/, 'net_fetch: a body from outside');
  const outHits = hits.length;
  const fetchRedirect = await call('net_fetch', { url: `${SITE}/to-outside` });
  check(fetchRedirect.isError && /outside the origins/.test(fetchRedirect.text) && hits.length === outHits, say('net_fetch: a redirect out is stopped before it goes'), { error: fetchRedirect.text, hits: hits.slice(outHits) });

  const packed = await call('data_compress', { format: 'zstd', text: 'hello hello hello hello' });
  const unpacked = await call('data_decompress', { format: 'zstd', base64: packed.structured.base64, outputPath: path.join(writeDir, `unpacked-${label}`) });
  check(fs.readFileSync(path.join(writeDir, `unpacked-${label}`), 'utf8') === 'hello hello hello hello', say('data_compress and data_decompress'), unpacked.text);
  const fromFile = await call('data_compress', { format: 'gzip', path: path.join(readDir, 'hello.txt') });
  check(fromFile.structured.base64, say('data_compress a file'), fromFile.text);
  await refused('data_compress', { format: 'gzip', path: path.join(readDir, 'link-out') }, /outside/, 'data_compress: a link out');
  await refused('data_decompress', { format: 'zstd', base64: packed.structured.base64, maxBytes: 3 }, /./, 'data_decompress: over its limit');
  await refused('data_compress', { format: 'gzip', text: 'a', base64: 'YQ==' }, /one of/, 'data_compress: two inputs');
  const hash = await call('data_hash', { text: 'abc' });
  check(hash.structured.hex === 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad', say('data_hash'), hash.text);
  const hashFile = await call('data_hash', { path: path.join(readDir, 'hello.txt'), algorithm: 'sha1' });
  check(hashFile.structured.hex === crypto.createHash('sha1').update('hello file').digest('hex'), say('data_hash a file'), hashFile.text);
  const png = shot.image.data;
  const decoded = await call('image_decode', { base64: png, infoOnly: true });
  check(decoded.structured.width === 900, say('image_decode'), decoded.text);
  await refused('image_decode', { text: 'not an image' }, /./, 'image_decode: not an image');
  const pixels = await call('image_decode', { base64: small.image.data });
  const made2 = await call('image_encode', { base64: pixels.structured.base64, width: pixels.structured.width, height: pixels.structured.height, format: 'webp' });
  check(Buffer.from(made2.structured.base64, 'base64').subarray(0, 4).toString() === 'RIFF', say('image_encode from pixels'), made2.text);
  const jpeg = await call('image_encode', { base64: png, format: 'jpeg', quality: 50 });
  check(Buffer.from(jpeg.structured.base64, 'base64')[0] === 0xff, say('image_encode converts'), jpeg.text);
  const parsed = await call('url_parse', { url: '../c?q=1#f', base: 'https://www.example.co.uk/a/b/' });
  check(parsed.structured.url === 'https://www.example.co.uk/a/c?q=1#f' && parsed.structured.registrableDomain === 'example.co.uk' && parsed.structured.origin === 'https://www.example.co.uk', say('url_parse'), parsed.text);
  await refused('url_parse', { url: 'http://' }, /not a valid/, 'url_parse: not a URL');

  // ---- Media
  const clip = path.join(readDir, 'clip.mp4');
  const probe = await call('media_probe', { path: clip });
  check(probe.structured.streams?.length >= 1 && probe.structured.format, say('media_probe'), probe.text);
  await refused('media_probe', { path: path.join(readDir, 'link-out') }, /outside/, 'media_probe: a link out');
  const mkv = path.join(writeDir, `clip-${label}.mkv`);
  const converted = await call('media_convert', { input: clip, output: mkv });
  check(!converted.isError && fs.statSync(mkv).size > 1000, say('media_convert'), converted.text);
  await refused('media_convert', { input: clip, output: path.join(outsideDir, 'c.mkv') }, /outside/, 'media_convert: an output outside');
  await refused('media_convert', { input: path.join(outsideDir, 'secret.txt'), output: mkv }, /outside/, 'media_convert: an input outside');
  await refused('media_convert', { input: clip, output: mkv }, /exists/, 'media_convert will not replace without overwrite');
  await refused('media_convert', { input: clip, output: path.join(writeDir, 'dangling-out') }, /symbolic link/, 'media_convert: over a link');
  const cutPath = path.join(writeDir, `cut-${label}.mp4`);
  const cut = await call('media_cut', { input: clip, output: cutPath, start: 0, end: 0.5 });
  const full = Number(probe.structured.format.duration);
  const cutProbe = await call('media_probe', { path: cutPath });
  check(!cut.isError && Number(cutProbe.structured.format?.duration) < full && Number(cutProbe.structured.format?.duration) > 0, say(`media_cut: ${full} s to ${cutProbe.structured.format?.duration} s`), cut.text + cutProbe.text);
  await refused('media_cut', { input: clip, output: path.join(writeDir, 'x.mp4'), start: 5, end: 1 }, /after start/, 'media_cut: end before start');
  await refused('media_cut', { input: clip, output: path.join(outsideDir, 'x.mp4'), end: 1 }, /outside/, 'media_cut: an output outside');
  const wav = path.join(writeDir, `audio-${label}.wav`);
  const audio = await call('media_extract_audio', { input: clip, output: wav, sampleRate: 16000, channels: 1 });
  check(!audio.isError && fs.readFileSync(wav).subarray(0, 4).toString() === 'RIFF', say('media_extract_audio'), audio.text);
  const rgba = path.join(writeDir, `frames-${label}.rgba`);
  const frames = await call('media_extract_frames', { input: clip, output: rgba, width: 32, height: 18, frames: 2 });
  check(!frames.isError && fs.statSync(rgba).size === 32 * 18 * 4 * 2, say('media_extract_frames'), frames.text);

  // ---- Native windows
  const ui = await call('ui_open', { title: 'Ask', width: 400, height: 300, content: { type: 'view', class: 'flex-col gap-2 p-4', children: [
    { type: 'text', children: ['Your name?'] },
    { type: 'input', name: 'who', placeholder: 'Name' },
    { type: 'button', name: 'ok', children: ['OK'] },
  ] } });
  check(ui.structured.tree?.title === 'Ask', say('ui_open'), ui.text);
  const windowId = ui.structured.window;
  const node = (tree, kind) => tree.type === kind ? tree : (tree.children ?? []).map(child => node(child, kind)).find(Boolean);
  const tree = await call('ui_tree', { window: windowId });
  check(node(tree.structured.tree, 'text').text === 'Your name?', say('ui_tree'), tree.text);
  const names = await call('ui_tree', {});
  check(names.structured.windows.includes(windowId), say('ui_tree: the windows open'), names.text);
  const typedUi = await call('ui_type', { window: windowId, node: node(tree.structured.tree, 'input').id, text: 'Ada' });
  check(node(typedUi.structured.tree, 'input').value === 'Ada', say('ui_type'), typedUi.text);
  const submitted = await call('ui_submit', { window: windowId, node: node(tree.structured.tree, 'input').id });
  check(!submitted.isError, say('ui_submit'), submitted.text);
  const clicked = await call('ui_click', { window: windowId, node: node(tree.structured.tree, 'button').id });
  check(!clicked.isError, say('ui_click'), clicked.text);
  const events = await call('ui_events', { window: windowId });
  check(events.structured.events.some(event => event.event === 'click' && event.name === 'ok') && events.structured.events.some(event => event.event === 'submit' && event.value === 'Ada'), say('ui_events'), events.text);
  const uiShot = await call('ui_capture', { window: windowId });
  check(uiShot.image?.mimeType === 'image/png', say('ui_capture'), uiShot.text);
  const nodeShot = await call('ui_capture', { window: windowId, node: node(tree.structured.tree, 'button').id });
  check(nodeShot.image, say('ui_capture a node'), nodeShot.text);
  await refused('ui_click', { window: windowId, node: 999999 }, /no node/, 'ui_click: a node not in that window');
  await refused('ui_tree', { window: 'w999' }, /no window/, 'ui_tree: a window not there');
  await refused('ui_open', { content: { type: 'web', url: `${OUTSIDE}/ui` } }, /web element is refused/, 'ui_open: a web element');
  await refused('ui_open', { content: { type: 'button', onClick: 'x' } }, /no property/, 'ui_open: a handler, or anything not a property');
  await refused('ui_open', { content: { type: 'view', constructor: 1 } }, /no property/, 'ui_open: a name from Object');
  await refused('ui_open', { content: { type: 'script' } }, /no element/, 'ui_open: an element not there');
  await call('ui_type', { window: windowId, node: node(tree.structured.tree, 'input').id, text: '"); zebkit.quit(); ("' });
  const stillThere = await call('ui_tree', { window: windowId });
  check(node(stillThere.structured.tree, 'input').value.endsWith('"); zebkit.quit(); ("'), say('text for a window stays text'), stillThere.text);
  const uiClosed = await call('ui_close', { window: windowId });
  check(!uiClosed.isError, say('ui_close'), uiClosed.text);

  // ---- The device flow
  provider.polls = 0;
  await refused('auth_device_start', { provider: 'nobody' }, /no provider/, 'auth_device_start: a provider not configured');
  await refused('auth_device_start', { provider: 'test', scopes: ['admin'] }, /not among/, 'auth_device_start: a scope the operator does not allow');
  const started = await call('auth_device_start', { provider: 'test', scopes: ['read'] });
  check(started.structured.userCode === 'WDJB-MJHT' && started.structured.verificationUri && !JSON.stringify(started.structured).includes(provider.deviceCode), say('auth_device_start: the code and the address, not the device code'), started.text);
  const flow = started.structured.flow;
  const sent = provider.requests.find(item => item.path === '/device')?.body ?? '';
  check(sent.includes('client_id=client-1') && sent.includes('scope=read') && !sent.includes('shh-secret'), say('the device request'), sent);
  await refused('net_fetch', { url: `${OAUTH}/api/echo`, auth: flow }, /not approved/, 'net_fetch with a flow not approved');
  const early = await call('auth_device_poll', { flow });
  check(early.structured.status === 'pending' && early.structured.retryAfter >= 1 && provider.polls === 0, say('auth_device_poll waits as long as the provider asks'), early.text);
  await sleep(1100);
  const pending = await call('auth_device_poll', { flow });
  check(pending.structured.status === 'pending' && provider.polls === 1, say('auth_device_poll: pending'), pending.text);
  await sleep(1100);
  const approved = await call('auth_device_poll', { flow });
  check(approved.structured.status === 'approved' && provider.polls === 2, say('auth_device_poll: approved'), approved.text);
  const tokenBody = provider.requests.find(item => item.path === '/token')?.body ?? '';
  check(tokenBody.includes('urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code') && tokenBody.includes(provider.deviceCode) && tokenBody.includes('shh-secret'), say('the token request'), tokenBody);
  const status = await call('auth_status', {});
  check(status.structured.providers.includes('test') && status.structured.flows.some(item => item.flow === flow && item.status === 'approved'), say('auth_status'), status.text);
  const api = await call('net_fetch', { url: `${OAUTH}/api/echo`, auth: flow });
  const serverSaw = provider.requests.filter(item => item.path === '/api/echo').pop();
  check(serverSaw.authorization === `Bearer ${provider.token}` && api.structured.text === 'header was Bearer [token]', say('net_fetch with the token: sent, and not shown'), api.text);
  await refused('net_fetch', { url: `${SITE}/data.json`, auth: flow }, /only sent to its API/, 'net_fetch: the token to another allowed origin');
  await refused('net_fetch', { url: `${OAUTH}/api/echo`, auth: flow, headers: [{ name: 'Authorization', value: 'x' }] }, /not with auth/, 'net_fetch: another Authorization with auth');
  const stolenBefore = hits.filter(hit => hit.url === '/stolen').length;
  const stolen = await call('net_fetch', { url: `${OAUTH}/api/redirect`, auth: flow });
  const stolenHits = hits.filter(hit => hit.url === '/stolen').slice(stolenBefore);
  check(stolen.isError && stolenHits.length === 0, say('a redirect from the API does not take the token along'), { error: stolen.text, stolenHits });
  const pageToken = await call('page_evaluate', { page, script: 'document.cookie + JSON.stringify(Object.keys(window).filter(k => /token/i.test(k)))' });
  check(pageToken.structured.value === '[]', say('a page sees nothing of it'), pageToken.text);
  const forgot = await call('auth_forget', { flow });
  check(!forgot.isError, say('auth_forget'), forgot.text);
  await refused('net_fetch', { url: `${OAUTH}/api/echo`, auth: flow }, /no flow/, 'net_fetch with a forgotten flow');

  // ---- Settings: read always; nothing changes that the operator did not name.
  const pageSettings = await call('settings_get', { page });
  check(pageSettings.structured.effective?.thirdPartyCookies === 'block' && pageSettings.structured.effective.newWindows === 'block' && pageSettings.structured.effective.downloads === 'ask' && pageSettings.structured.allowedKeys.length === 0, say('settings_get a page: as the bounds set it'), pageSettings.text);
  const engineSettings = await call('settings_get', {});
  check(engineSettings.structured.effective?.spareRenderer === false && engineSettings.structured.explicit && Object.keys(engineSettings.structured.explicit).length === 0, say('settings_get the engine'), engineSettings.text);
  await refused('settings_get', { page: 'p999' }, /no page/, 'settings_get: a page not there');
  await refused('settings_set', { page, settings: { dialogs: 'accept' } }, /--allow-settings/, 'settings_set: a key not allowed, for a page');
  await refused('settings_set', { settings: { thirdPartyCookies: 'allow' } }, /--allow-settings/, 'settings_set: a key not allowed, for the engine');
  const unchanged = await call('settings_get', { page });
  check(JSON.stringify(unchanged.structured.explicit) === JSON.stringify(pageSettings.structured.explicit), say('settings_set refused: nothing changed'), unchanged.text);

  // ---- Closing
  await refused('page_close', { page: 'p999' }, /no page/, 'page_close: a page not there');
  const pageClosed = await call('page_close', { page });
  check(!pageClosed.isError, say('page_close'), pageClosed.text);
  await refused('page_read', { page }, /no page/, 'a closed page is gone');
  await call('page_close', { page: other.structured.page });

  const notCovered = tools.map(tool => tool.name).filter(name => !used.has(name));
  check(notCovered.length === 0, say('every tool was called'), notCovered);
}

// ---- The current revision, by hand ---------------------------------------------

const META = { 'io.modelcontextprotocol/protocolVersion': '2026-07-28', 'io.modelcontextprotocol/clientInfo': { name: 't', version: '1' }, 'io.modelcontextprotocol/clientCapabilities': {} };

async function modernStdio() {
  const child = spawn(binary, [...fullBounds, ...engine], { stdio: ['pipe', 'pipe', 'pipe'] });
  let buffer = '';
  const waiting = [];
  child.stdout.on('data', data => {
    buffer += data;
    let end;
    while ((end = buffer.indexOf('\n')) >= 0) {
      const line = buffer.slice(0, end);
      buffer = buffer.slice(end + 1);
      waiting.shift()?.(line);
    }
  });
  child.stderr.on('data', () => {});
  const line = text => new Promise(resolve => { waiting.push(resolve); child.stdin.write(text + '\n'); });
  const rpc = async (method, params) => JSON.parse(await line(JSON.stringify({ jsonrpc: '2.0', id: 1, method, params })));
  const discover = await rpc('server/discover', { _meta: META });
  check(discover.result?.supportedVersions.includes('2026-07-28') && discover.result.resultType === 'complete' && discover.result.ttlMs > 0 && discover.result.cacheScope === 'private' && discover.result._meta['io.modelcontextprotocol/serverInfo'].name === 'zebkit-mcp', 'stdio 2026-07-28: server/discover', discover);
  const before = await rpc('tools/list', {});
  check(before.error?.code === -32600, 'stdio: no version, no handshake: refused', before);
  const tools = await rpc('tools/list', { _meta: META });
  check(tools.result?.tools.length > 50 && tools.result.resultType === 'complete' && tools.result.ttlMs > 0, 'stdio 2026-07-28: tools/list', tools.result?.resultType);
  const called = await rpc('tools/call', { name: 'data_hash', arguments: { text: 'abc' }, _meta: META });
  check(called.result?.structuredContent?.hex?.startsWith('ba7816bf') && called.result.resultType === 'complete', 'stdio 2026-07-28: tools/call', called);
  const old = await rpc('tools/list', { _meta: { ...META, 'io.modelcontextprotocol/protocolVersion': '1900-01-01' } });
  check(old.error?.code === -32022 && old.error.data.requested === '1900-01-01' && old.error.data.supported.includes('2026-07-28'), 'stdio: an unsupported version', old);
  const unknown = await rpc('resources/list', { _meta: META });
  check(unknown.error?.code === -32601, 'stdio: a method not there', unknown);
  const unknownTool = await rpc('tools/call', { name: 'nope', arguments: {}, _meta: META });
  check(unknownTool.error?.code === -32602, 'stdio: a tool not there', unknownTool);
  const parse = JSON.parse(await line('{not json'));
  check(parse.error?.code === -32700, 'stdio: not JSON', parse);
  const batch = JSON.parse(await line('[{"jsonrpc":"2.0","id":1,"method":"ping"}]'));
  check(batch.error?.code === -32600, 'stdio: a batch', batch);
  const nullId = JSON.parse(await line('{"jsonrpc":"2.0","id":null,"method":"ping"}'));
  check(nullId.error?.code === -32600, 'stdio: a null id', nullId);
  child.stdin.write(`${JSON.stringify({ jsonrpc: '2.0', method: 'notifications/cancelled', params: { requestId: 77 } })}\n`);
  const late = await Promise.race([line(JSON.stringify({ jsonrpc: '2.0', id: 77, method: 'ping', params: { _meta: META } })), sleep(10000).then(() => 'no answer')]);
  check(late !== 'no answer', 'stdio: cancelling what is not running does not drop a later answer', late);
  const deep = JSON.parse(await line('['.repeat(200000)));
  check(deep.error?.code === -32700, 'stdio: JSON nested without end', deep);
  const exited = new Promise(resolve => child.once('exit', resolve));
  child.stdin.end();
  const code = await Promise.race([exited, sleep(30000).then(() => 'still running')]);
  check(code !== 'still running', 'stdio: ends when its standard input ends', code);
  await stop(child);
}

function raw(port, { method = 'POST', headers = {}, body, requestPath = '/mcp' }) {
  return new Promise((resolve, reject) => {
    const sized = body === undefined || 'Content-Length' in headers ? headers : { ...headers, 'Content-Length': Buffer.byteLength(body) };
    const request = http.request({ host: '127.0.0.1', port, method, path: requestPath, headers: sized }, response => {
      let text = '';
      response.on('data', d => { text += d; });
      response.on('end', () => resolve({ status: response.statusCode, headers: response.headers, text, json: (() => { try { return JSON.parse(text); } catch { return null; } })() }));
    });
    request.on('error', reject);
    if (body !== undefined) request.write(body);
    request.end();
  });
}

async function httpSecurity(port, token) {
  const good = { Authorization: `Bearer ${token}`, 'Content-Type': 'application/json', Accept: 'application/json, text/event-stream' };
  const ping = JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'tools/list', params: { _meta: META } });
  const modern = { ...good, 'MCP-Protocol-Version': '2026-07-28', 'Mcp-Method': 'tools/list' };
  const asked = (extra = {}, body = ping) => raw(port, { headers: { ...modern, ...extra }, body });

  const fine = await asked();
  check(fine.status === 200 && fine.json.result.tools.length > 50 && fine.json.result.resultType === 'complete', 'HTTP 2026-07-28: tools/list', fine.status);
  const noToken = { ...modern }; delete noToken.Authorization;
  const missing = await raw(port, { headers: noToken, body: ping });
  check(missing.status === 401 && /Bearer/.test(missing.headers['www-authenticate']), 'HTTP: no token: 401', missing.status);
  check((await asked({ Authorization: 'Bearer wrong' })).status === 401, 'HTTP: a wrong token: 401');
  check((await asked({ Authorization: `Bearer ${token}x` })).status === 401, 'HTTP: a longer token: 401');
  check((await asked({ Authorization: token })).status === 401, 'HTTP: a token without its scheme: 401');
  check((await asked({ Authorization: `Basic ${token}` })).status === 401, 'HTTP: another scheme: 401');
  const inQuery = await raw(port, { headers: noToken, body: ping, requestPath: `/mcp?token=${token}` });
  check(inQuery.status !== 200, 'HTTP: a token in the URL is not read', inQuery.status);
  check((await asked({ Origin: 'https://evil.example' })).status === 403, 'HTTP: a bad Origin: 403');
  check((await asked({ Origin: 'null' })).status === 403, 'HTTP: Origin null: 403');
  check((await asked({ Origin: `http://127.0.0.1:${port}.evil.example` })).status === 403, 'HTTP: an Origin that only starts like this server: 403');
  const noTokenBadOrigin = await raw(port, { headers: { ...noToken, Origin: 'https://evil.example' }, body: ping });
  check(noTokenBadOrigin.status === 403, 'HTTP: a bad Origin is refused before the token is looked at', noTokenBadOrigin.status);
  check((await asked({ Origin: 'https://client.example' })).status === 200, 'HTTP: an Origin the operator allows');
  check((await asked({ Origin: `http://127.0.0.1:${port}` })).status === 200, "HTTP: this server's own Origin");
  check((await asked({ Host: 'evil.example' })).status === 403, 'HTTP: another Host (DNS rebinding): 403');
  check((await asked({ Host: `evil.example:${port}` })).status === 403, 'HTTP: another Host with the port: 403');
  check((await asked({ Host: 'localhost:1' })).status === 403, 'HTTP: another port in Host: 403');
  check((await asked({ Host: `localhost:${port}` })).status === 200, 'HTTP: localhost in Host');
  check((await asked({ Host: 'operator.example' })).status === 200, 'HTTP: a Host the operator allows');
  const options = await raw(port, { method: 'OPTIONS', headers: { Origin: 'https://evil.example', 'Access-Control-Request-Method': 'POST' } });
  check(options.status === 403 && !options.headers['access-control-allow-origin'], 'HTTP: a preflight gets nothing', options.status);
  check(!fine.headers['access-control-allow-origin'], 'HTTP: no CORS header');
  check((await raw(port, { method: 'OPTIONS', headers: { Authorization: good.Authorization } })).status === 405, 'HTTP: OPTIONS: 405');
  check((await raw(port, { method: 'GET', headers: { Authorization: good.Authorization } })).status === 405, 'HTTP: GET: 405');
  check((await raw(port, { requestPath: '/other', headers: modern, body: ping })).status === 404, 'HTTP: another path: 404');
  check((await asked({ 'Content-Type': 'text/plain' })).status === 415, 'HTTP: a form or text/plain body (what a page can send freely): 415');
  check((await asked({ Accept: 'text/html' })).status === 406, 'HTTP: an Accept without JSON: 406');
  const mismatch = await asked({ 'Mcp-Method': 'tools/call' });
  check(mismatch.status === 400 && mismatch.json.error.code === -32020, 'HTTP: Mcp-Method does not match: HeaderMismatch', mismatch.text);
  const noMethod = { ...modern }; delete noMethod['Mcp-Method'];
  const absent = await raw(port, { headers: noMethod, body: ping });
  check(absent.status === 400 && absent.json.error.code === -32020, 'HTTP: Mcp-Method missing: HeaderMismatch', absent.text);
  const wrongVersion = await asked({ 'MCP-Protocol-Version': '2025-11-25' });
  check(wrongVersion.status === 400 && wrongVersion.json.error.code === -32020, 'HTTP: MCP-Protocol-Version does not match', wrongVersion.text);
  const unsupported = await asked({ 'MCP-Protocol-Version': '1900-01-01' }, JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'tools/list', params: { _meta: { ...META, 'io.modelcontextprotocol/protocolVersion': '1900-01-01' } } }));
  check(unsupported.status === 400 && unsupported.json.error.code === -32022, 'HTTP: an unsupported version', unsupported.text);
  const call = (name, header, extra = {}) => asked({ 'Mcp-Method': 'tools/call', 'Mcp-Name': header, ...extra }, JSON.stringify({ jsonrpc: '2.0', id: 2, method: 'tools/call', params: { name, arguments: { text: 'abc' }, _meta: META } }));
  const named = await call('data_hash', 'data_hash');
  check(named.status === 200 && named.json.result.structuredContent.hex.startsWith('ba7816bf'), 'HTTP 2026-07-28: tools/call', named.text);
  const encoded = await call('data_hash', `=?base64?${Buffer.from('data_hash').toString('base64')}?=`);
  check(encoded.status === 200, 'HTTP: Mcp-Name in base64', encoded.text);
  const wrongName = await call('data_hash', 'file_write');
  check(wrongName.status === 400 && wrongName.json.error.code === -32020, 'HTTP: Mcp-Name is not the tool that would run', wrongName.text);
  const unknown = await asked({ 'Mcp-Method': 'nope' }, JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'nope', params: { _meta: META } }));
  check(unknown.status === 404 && unknown.json.error.code === -32601, 'HTTP: a method not there: 404', unknown.text);
  const notification = await asked({}, JSON.stringify({ jsonrpc: '2.0', method: 'notifications/cancelled', params: { requestId: 1 } }));
  check(notification.status === 202 && notification.text === '', 'HTTP: a notification: 202', notification.status);
  const bad = await asked({}, '{not json');
  check(bad.status === 400 && bad.json.error.code === -32700, 'HTTP: not JSON', bad.text);
  check((await asked({}, '[{"jsonrpc":"2.0","id":1,"method":"ping"}]')).status === 400, 'HTTP: a batch');
  const noSession = await raw(port, { headers: good, body: JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'tools/list' }) });
  check(noSession.status === 400, 'HTTP: no version and no session', noSession.status);
  const wrongSession = await raw(port, { headers: { ...good, 'Mcp-Session-Id': 'made-up' }, body: JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'tools/list' }) });
  check(wrongSession.status === 404, 'HTTP: a session it did not make', wrongSession.status);
  const init = await raw(port, { headers: good, body: JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'initialize', params: { protocolVersion: '2025-11-25', capabilities: {}, clientInfo: { name: 't', version: '1' } } }) });
  const session = init.headers['mcp-session-id'];
  check(init.status === 200 && init.json.result.protocolVersion === '2025-11-25' && /^[0-9a-f]{48}$/.test(session), 'HTTP: initialize makes a session', init.text);
  const inSession = await raw(port, { headers: { ...good, 'Mcp-Session-Id': session, 'MCP-Protocol-Version': '2025-11-25' }, body: JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'tools/list' }) });
  check(inSession.status === 200 && inSession.json.result.tools.length > 50 && !inSession.json.result.resultType, 'HTTP: in a session, as that revision answers', inSession.status);
  const gone = await raw(port, { method: 'DELETE', headers: { Authorization: good.Authorization, 'Mcp-Session-Id': session } });
  check(gone.status === 204, 'HTTP: DELETE ends a session', gone.status);
  const after = await raw(port, { headers: { ...good, 'Mcp-Session-Id': session }, body: JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'tools/list' }) });
  check(after.status === 404, 'HTTP: an ended session is gone', after.status);
  const huge = await raw(port, { headers: { ...modern, 'Content-Length': String(1024 * 1024 * 1024) }, body: '' }).catch(() => ({ status: 413 }));
  check(huge.status === 413, 'HTTP: a body over the limit', huge.status);
  const twoHosts = await new Promise(resolve => {
    const socket = net.connect(port, '127.0.0.1', () => socket.write(`POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nHost: evil.example\r\nContent-Length: 0\r\n\r\n`));
    let text = '';
    socket.on('data', d => { text += d; });
    socket.on('close', () => resolve(text));
  });
  check(/^HTTP\/1.1 403/.test(twoHosts), 'HTTP: two Host headers: 403', twoHosts);
  const sessions = [];
  for (let i = 0; i < 300; i++) {
    const made = await raw(port, { headers: good, body: JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'initialize', params: { protocolVersion: '2025-11-25', capabilities: {}, clientInfo: { name: 't', version: '1' } } }) });
    sessions.push([made.status, made.headers['mcp-session-id']]);
  }
  check(sessions.every(([status]) => status === 200), 'HTTP: clients that leave without a DELETE do not use up the sessions', sessions.filter(([status]) => status !== 200).length);
  const inSessionAt = at => raw(port, { headers: { ...good, 'Mcp-Session-Id': sessions[at][1] }, body: JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'tools/list' }) });
  check((await inSessionAt(299)).status === 200 && (await inSessionAt(0)).status === 404, 'HTTP: the newest is there, the oldest made way');
  const forHost = host => new Promise(resolve => {
    const socket = net.connect(port, '127.0.0.1', () => socket.write(`GET /mcp HTTP/1.1\r\nHost: ${host}\r\n\r\n`));
    let text = '';
    socket.on('data', d => { text += d; });
    socket.on('close', () => resolve(text));
  });
  for (const host of [`[::1]evil.example:${port}`, `localhost:${port}:${port}`, `localhost:`, `[::1`]) {
    check(/^HTTP\/1.1 403/.test(await forHost(host)), `HTTP: a Host that only looks local: ${host}`);
  }
  // A byte at a time, each in time for a read's timeout: 30 seconds for the whole.
  const started = Date.now();
  const dripped = await new Promise(resolve => {
    const socket = net.connect(port, '127.0.0.1');
    const drip = setInterval(() => socket.write('x'), 2000);
    socket.on('close', () => { clearInterval(drip); resolve(Date.now() - started); });
    socket.on('error', () => {});
    setTimeout(() => { clearInterval(drip); socket.destroy(); resolve(Infinity); }, 45000);
  });
  check(dripped < 40000, 'HTTP: a request sent slowly is cut off whatever it does', dripped);
  const longHead = await new Promise(resolve => {
    const socket = net.connect(port, '127.0.0.1', () => socket.write(`GET /mcp HTTP/1.1\r\nX: ${'a'.repeat(40000)}\r\n\r\n`));
    let text = '';
    socket.on('data', d => { text += d; });
    socket.on('close', () => resolve(text));
    socket.on('error', () => resolve('HTTP/1.1 431'));
  });
  check(/^HTTP\/1.1 431/.test(longHead), 'HTTP: headers over the limit', longHead.slice(0, 40));
}

// ---- The bounds at their defaults, and the operator's choices ----------------------

async function withClient(args, body, env) {
  const server = await stdioClient(sdk, binary, [...args, ...engine], env);
  try {
    await body(wrap(server.client, transcript), server);
  } finally {
    await server.close();
  }
}

async function defaults() {
  await withClient([], async c => {
    const denied = async (name, args, pattern, what) => {
      const answer = await c.call(name, args);
      check(answer.isError && pattern.test(answer.text), `no bounds given: ${what}`, answer.text);
    };
    await denied('file_read', { path: path.join(readDir, 'hello.txt') }, /denied/, 'reading is denied');
    await denied('file_list', { path: readDir }, /denied/, 'listing is denied');
    await denied('file_write', { path: path.join(writeDir, 'x'), text: 'x' }, /denied/, 'writing is denied');
    await denied('pdf_open', { path: path.join(readDir, 'plain.pdf') }, /denied/, 'PDF from a file is denied');
    await denied('media_probe', { path: path.join(readDir, 'clip.mp4') }, /denied/, 'media is denied');
    await denied('data_hash', { path: path.join(readDir, 'hello.txt') }, /denied/, 'data from a file is denied');
    await denied('page_open', { url: `${SITE}/page` }, /outside the origins/, 'no origin is allowed');
    await denied('net_fetch', { url: `${SITE}/page` }, /outside the origins/, 'net_fetch has no origin');
    await denied('auth_device_start', { provider: 'test' }, /no provider/, 'no provider');
    const blank = await c.call('page_open', {});
    check(!blank.isError, 'no bounds given: a blank page opens', blank.text);
    const set = await c.call('page_evaluate', { page: blank.structured.page, script: "document.body.innerHTML = '<p>x</p>'; document.body.textContent" });
    check(set.structured.value === 'x', 'no bounds given: a blank page runs script', set.text);
    const hash = await c.call('data_hash', { text: 'abc' });
    check(!hash.isError, 'no bounds given: data as arguments works', hash.text);
  });

  // --allow-settings: a key allowed takes effect, the others do not, and no value
  // gets past what the bounds refuse.
  await withClient(['--allow-origin', SITE, '--allow-settings', 'dialogs,downloads,newWindows,viewport'], async c => {
    const refused = async (args, pattern, what) => {
      const answer = await c.call('settings_set', args);
      check(answer.isError && pattern.test(answer.text), `--allow-settings: ${what}`, answer.text);
    };
    const opened = await c.call('page_open', { url: `${SITE}/page` });
    const page = opened.structured.page;
    const answered = script => c.call('page_evaluate', { page, script });
    await refused({ page, settings: { thirdPartyCookies: 'allow' } }, /--allow-settings/, 'a key not named');
    await refused({ page, settings: { dialogs: 'dismiss', thirdPartyCookies: 'allow' } }, /--allow-settings/, 'a batch with one key not named');
    const untouched = await c.call('settings_get', { page });
    check(untouched.structured.effective.dialogs === 'default' && untouched.structured.allowedKeys.includes('dialogs'), '--allow-settings: a refused batch changed nothing', untouched.text);
    await refused({ page, settings: { newWindows: 'allow' } }, /only "block"/, 'newWindows allow stays refused');
    await refused({ page, settings: { newWindows: 'block-without-gesture' } }, /only "block"/, 'newWindows without a gesture stays refused');
    await refused({ page, settings: { newWindows: null } }, /only "block"/, 'newWindows reset stays refused');
    await refused({ page, settings: { downloads: 'save' } }, /only "deny"/, 'downloads save stays refused');
    await refused({ settings: { newWindows: 'allow' } }, /only "block"/, 'newWindows allow stays refused for the engine');
    await refused({ settings: { downloads: 'save' } }, /only "deny"/, 'downloads save stays refused for the engine');
    await refused({ page, settings: { dialogs: 'nonsense' } }, /invalid runtime setting/, "the engine's own check");
    await refused({ settings: { viewport: { width: 500, height: 400 } } }, /of a page/, 'a viewport for no page');
    await refused({ page, settings: { viewport: { width: 0, height: 400 } } }, /viewport/, 'a viewport of no size');

    const set = await c.call('settings_set', { page, settings: { dialogs: 'dismiss', newWindows: 'block' } });
    check(!set.isError && set.structured.explicit.dialogs === 'dismiss', '--allow-settings: an allowed key is set', set.text);
    const confirmed = await answered("confirm('now?')");
    check(confirmed.structured.value === false, '--allow-settings: it takes effect in the page', confirmed.text);
    const listed = await c.call('page_list', {});
    check(listed.structured.pages.find(item => item.page === page).openDialogs === 0, '--allow-settings: a dialog the policy answered is not left waiting', listed.text);
    await c.call('settings_set', { page, settings: { dialogs: 'accept' } });
    const accepted = await answered("confirm('now?')");
    check(accepted.structured.value === true, '--allow-settings: changed again', accepted.text);
    const resized = await c.call('settings_set', { page, settings: { dialogs: null, viewport: { width: 500, height: 400 } } });
    const size = await answered('innerWidth + "x" + innerHeight');
    check(!resized.isError && size.structured.value === '500x400', '--allow-settings: the viewport', size.text);
    const engineWide = await c.call('settings_set', { settings: { downloads: 'deny', newWindows: 'block' } });
    check(!engineWide.isError && engineWide.structured.effective.downloads === 'deny', '--allow-settings: narrowing the engine', engineWide.text);
    await c.call('page_click', { page, selector: '#popup' });
    await c.call('page_click', { page, selector: '#download' });
    let log;
    for (let i = 0; i < 100; i++) {
      log = await c.call('page_network', { page });
      if (['popup', 'download'].every(what => log.structured.refusedByBounds?.some(item => item.what === what))) break;
      await sleep(50);
    }
    check(['popup', 'download'].every(what => log.structured.refusedByBounds?.some(item => item.what === what)), '--allow-settings: popups and downloads still go to the hooks', log.text);
  });

  await withClient(['--tools', 'pdf,data_hash', '--deny-tools', 'pdf_close'], async c => {
    const { tools } = await c.client.listTools();
    const names = tools.map(tool => tool.name);
    check(names.includes('pdf_open') && names.includes('data_hash') && !names.includes('pdf_close') && !names.includes('page_open') && !names.includes('file_read'), '--tools and --deny-tools choose what is listed', names);
    const page = await c.client.callTool({ name: 'page_open', arguments: {} }).then(result => JSON.stringify(result), error => String(error));
    check(/not allowed by this server's bounds/.test(page), 'a tool not allowed is refused', page);
    const unknown = await c.client.callTool({ name: 'no_such_tool', arguments: {} }).then(result => JSON.stringify(result), error => String(error));
    check(/Unknown tool/.test(unknown), 'a tool not there', unknown);
  });

  await withClient(['--allow-origin', '*'], async c => {
    for (const [what, url] of [
      ['127.0.0.1', `${SITE}/page`], ['localhost', `http://localhost:${sitePort}/page`], ['127.0.0.1 written as a number', `http://2130706433:${sitePort}/page`],
      ['127.1', `http://127.1:${sitePort}/page`], ['[::1]', `http://[::1]:${sitePort}/page`], ['[::ffff:127.0.0.1]', `http://[::ffff:127.0.0.1]:${sitePort}/page`],
      ['169.254.169.254, cloud metadata', 'http://169.254.169.254/latest'], ['10.0.0.1', 'http://10.0.0.1/'], ['192.168.0.1', 'http://192.168.0.1/'], ['a .local name', 'http://printer.local/'],
      ['a name with no dot', 'http://intranet/'], ['userinfo before it', `http://example.com@127.0.0.1:${sitePort}/`],
    ]) {
      const answer = await c.call('page_open', { url });
      check(answer.isError && /outside the origins/.test(answer.text), `--allow-origin *: not ${what}`, answer.text);
      const fetched = await c.call('net_fetch', { url });
      check(fetched.isError && /outside the origins/.test(fetched.text), `--allow-origin *: net_fetch not ${what}`, fetched.text);
    }
  });

  // Exact origins are exact: scheme and port too.
  await withClient(['--allow-origin', `http://localhost:${sitePort}`], async c => {
    const allowed = await c.call('page_open', { url: `http://localhost:${sitePort}/page` });
    check(!allowed.isError && allowed.structured.status === 200, 'an origin by name is allowed', allowed.text);
    const page = allowed.structured.page;
    const before = hits.length;
    const socket = await c.call('page_evaluate', { page, script: `new Promise(resolve => { const s = new WebSocket('ws://localhost:${outsidePort}/other-port'); s.onerror = () => resolve('failed'); s.onopen = () => resolve('open'); })` });
    check(socket.structured.value === 'failed' && hits.length === before, 'a WebSocket to the same name on another port is stopped', { socket: socket.text, hits: hits.slice(before) });
    for (const url of [`http://127.0.0.1:${sitePort}/page`, `https://localhost:${sitePort}/page`, `http://localhost:${outsidePort}/page`]) {
      const answer = await c.call('page_navigate', { page, url });
      check(answer.isError && /outside the origins/.test(answer.text), `exact origins: not ${url}`, answer.text);
    }
  });

  // Started wrongly, it does not start.
  const startFails = (args, pattern, what) => {
    let text = '';
    let code = 0;
    try {
      execFileSync(binary, args, { stdio: ['ignore', 'pipe', 'pipe'], timeout: 60000 });
    } catch (error) {
      code = error.status;
      text = String(error.stderr);
    }
    check(code === 2 && pattern.test(text), `refuses to start: ${what}`, { code, text });
  };
  startFails(['--http', '0', '--http-bind', '0.0.0.0'], /not a loopback/, 'a public address without saying so');
  startFails(['--allow-read', '/'], /whole file system/, 'granting /');
  startFails(['--allow-read', 'relative'], /absolute/, 'a relative grant');
  startFails(['--allow-read', path.join(work, 'not-there')], /No such file/, 'a grant not there');
  startFails(['--allow-origin', 'ftp://example.com'], /not an http or https origin/, 'an origin of another scheme');
  startFails(['--tools', 'nothing'], /neither a tool nor a group/, 'a tool not there');
  startFails(['--allow-settings', 'nothing'], /not a setting/, 'a setting not there');
  startFails(['--', '--remote-debugging-port=9222'], /refused/, 'CDP behind the bounds');
  startFails(['--', '--host-resolver-rules=x'], /refused/, 'its own resolver rules');
  startFails(['--', '--user-data-dir=/tmp/x'], /refused/, 'a profile through the engine');
  const wide = path.join(work, 'wide-token');
  fs.writeFileSync(wide, 'x'.repeat(32), { mode: 0o644 });
  startFails(['--http', '0', '--http-token-file', wide, ...engine], /readable by others/, 'a token file others can read');
  const wideOauth = path.join(work, 'wide-oauth.json');
  fs.writeFileSync(wideOauth, '{}', { mode: 0o644 });
  startFails(['--oauth-config', wideOauth], /readable by others/, 'an OAuth file others can read');
  const plainOauth = path.join(work, 'plain-oauth.json');
  fs.writeFileSync(plainOauth, JSON.stringify({ x: { clientId: 'a', deviceEndpoint: 'http://auth.example/d', tokenEndpoint: 'https://auth.example/t' } }), { mode: 0o600 });
  startFails(['--oauth-config', plainOauth], /https is required/, 'an OAuth endpoint without TLS');
  startFails(['--', '-remote-debugging-port=9222'], /refused/, 'CDP behind the bounds, with one dash');
  startFails(['--', '--host-rules=MAP * 127.0.0.1'], /refused/, 'the other resolver rules');
  startFails(['--', '--proxy-bypass-list=*'], /refused/, 'any proxy switch');
  startFails(['--', '--renderer-cmd-prefix=/bin/x'], /refused/, 'a program for the engine to start');
  startFails(['--', '--disable-features=x'], /refused/, 'its features, which hold part of the bounds');
  startFails(['--', 'http://example.com/'], /refused/, 'an address among the switches');

  // Whatever refuses a start leaves nothing, before the engine or inside it.
  const refusals = path.join(work, 'refusals');
  fs.mkdirSync(refusals);
  const leavesNothing = (args, pattern, what) => {
    let text = '', code = 0;
    try {
      execFileSync(binary, args, { stdio: ['ignore', 'pipe', 'pipe'], timeout: 120000, env: { ...process.env, TMPDIR: refusals } });
    } catch (error) {
      code = error.status;
      text = String(error.stderr);
    }
    check(code === 2 && pattern.test(text) && fs.readdirSync(refusals).length === 0, `refuses to start, leaving nothing: ${what}`, { code, text: text.slice(0, 200), left: fs.readdirSync(refusals) });
  };
  leavesNothing(['--http', '0', '--http-token-file', wide, ...engine], /readable by others/, 'a token file others can read');
  leavesNothing(['--allow-origin', 'https://example.com/a/path', ...engine], /not an http or https origin/, 'an origin the browser refuses');
  leavesNothing(['--http', String(outsidePort), ...engine], /in use/i, 'a port in use');
}

// ---- What a review found --------------------------------------------------------

async function reviewed() {
  const named = `http://localhost:${sitePort}`;
  const quiet = async (c, page, script, what, wait = 700) => {
    const before = hits.length;
    const answer = await c.call('page_evaluate', { page, script });
    await sleep(wait);
    check(!answer.isError && hits.length === before, what, { answer: answer.text, hits: hits.slice(before) });
  };
  // ---- The connect policy, wherever a document or a worker can open a socket.
  const stun = dgram.createSocket('udp4');
  let stunHits = 0;
  stun.on('message', () => { stunHits++; });
  await new Promise(resolve => stun.bind(0, '127.0.0.1', resolve));
  await withClient(['--allow-origin', named], async c => {
    const socket = path => `new WebSocket('ws://localhost:${outsidePort}/${path}'); 1`;
    const blank = (await c.call('page_open', {})).structured.page;
    await quiet(c, blank, socket('blank-opened'), 'a blank page has the connect policy too');
    const workers = await c.call('page_evaluate', { page: blank, script: "typeof SharedWorker + ' ' + typeof navigator.serviceWorker" });
    check(workers.structured.value === 'undefined undefined', 'a blank page has no service or shared workers', workers.text);
    await c.call('page_navigate', { page: blank, url: 'about:blank' });
    await quiet(c, blank, socket('blank-navigated'), 'a page navigated to blank has it too');
    const page = (await c.call('page_open', { url: `${named}/page` })).structured.page;
    await quiet(c, page, "new Worker('/worker.js'); 1", "a dedicated worker's script has it too");
    await quiet(c, page, "new Worker('/worker.js', {type: 'module'}); 1", 'and a module worker');
    await quiet(c, page, `new Worker('data:text/javascript,' + encodeURIComponent(${JSON.stringify(socket('data-worker'))})); 1`, 'and a worker of data:');
    await quiet(c, page, `const f = document.createElement('iframe'); f.srcdoc = "<script>" + ${JSON.stringify(socket('srcdoc'))} + "<\/script>"; document.body.append(f); 1`, 'and a frame of srcdoc');
    await quiet(c, page, `const d = document.createElement('iframe'); d.src = 'data:text/html,<script>' + encodeURIComponent(${JSON.stringify(socket('data-frame'))}) + '<\/script>'; document.body.append(d); 1`, 'and a frame of data:');
    await quiet(c, page, `navigator.sendBeacon('http://localhost:${outsidePort}/beacon', 'x')`, 'a beacon is held');
    const pinging = (await c.call('page_open', { url: `${named}/ping` })).structured.page;
    const beforePing = hits.length;
    await c.call('page_click', { page: pinging, selector: '#link' });
    await sleep(700);
    check(hits.length === beforePing, '<a ping> is held', hits.slice(beforePing));
    const iframe = await c.call('page_evaluate', { page, script: "const e = document.createElement('iframe'); document.body.append(e); typeof e.contentWindow.SharedWorker + ' ' + typeof e.contentWindow.navigator.serviceWorker" });
    check(iframe.structured.value === 'undefined undefined', 'a new frame has no service or shared workers', iframe.text);
    await c.call('page_evaluate', { page, script: `const pc = new RTCPeerConnection({iceServers: [{urls: 'stun:127.0.0.1:${stun.address().port}'}]}); pc.createDataChannel('x'); pc.createOffer().then(o => pc.setLocalDescription(o)); 1` });
    await sleep(2000);
    check(stunHits === 0, 'WebRTC sends no UDP of its own', stunHits);

    // ---- The logs, whose size a page chooses.
    await c.call('page_evaluate', { page, script: `console.log('x'.repeat(60000)); fetch('/second?' + 'q'.repeat(100000)); new Image().src = 'http://localhost:${outsidePort}/?' + 'r'.repeat(100000); 1` });
    await sleep(500);
    const logged = await c.call('page_network', { page });
    check(logged.structured.items.length > 0 && logged.structured.items.every(item => item.url.length < 9000) && logged.structured.refusedByBounds.every(item => item.url.length < 9000) && logged.structured.refusedByBounds.some(item => item.url.includes('(cut)')), 'long addresses are cut in the logs', logged.text.length);
    const consoled = await c.call('page_console', { page });
    check(consoled.text.length < 20000, 'long console messages are cut', consoled.text.length);

    // ---- A click right after a load reaches the page.
    const clicker = (await c.call('page_open', { url: `${named}/repeat?0` })).structured.page;
    let lost = 0;
    for (let i = 1; i <= 50; i++) {
      await c.call('page_navigate', { page: clicker, url: `${named}/repeat?${i}` });
      await c.call('page_click', { page: clicker, selector: '#b' });
      if ((await c.call('page_evaluate', { page: clicker, script: 'window.seen' })).structured.value !== 1) lost++;
    }
    check(lost === 0, 'a click right after a load is not lost', `${lost} of 50`);
    const alerted = await c.call('page_evaluate', { page: clicker, script: "setTimeout(() => alert('x'), 0); 1" });
    await sleep(300);
    const whileOpen = await c.call('page_click', { page: clicker, selector: '#b', timeout: 1500 });
    check(!alerted.isError && whileOpen.isError && /dialog|Timeout/i.test(whileOpen.text), 'an action with a dialog open still comes back', whileOpen.text);
    await c.call('page_dialog', { page: clicker });

    // ---- A page closed while something waits for it.
    const closing = (await c.call('page_open', { url: `${named}/second` })).structured.page;
    const waiting = c.call('page_wait', { page: closing, selector: '#never', timeout: 60000 });
    await sleep(200);
    await c.call('page_close', { page: closing });
    const answered = await Promise.race([waiting.then(() => 'answered'), sleep(20000).then(() => 'still waiting')]);
    check(answered === 'answered', 'closing a page ends what waits for it', answered);

    // ---- Profiles.
    const same = await Promise.all([1, 2, 3].map(() => c.call('page_open', { url: `${named}/second`, profile: 'at-once' })));
    await c.call('page_evaluate', { page: same[0].structured.page, script: "document.cookie = 'shared=1'; 1" });
    const cookie = await c.call('page_evaluate', { page: same[2].structured.page, script: 'document.cookie' });
    check(cookie.structured.value === 'shared=1', 'pages opened at once in a new profile are of one profile', cookie.text);
    for (let i = 0; i < 20; i++) {
      const opened = await c.call('page_open', { profile: `many-${i}` });
      if (!opened.isError) await c.call('page_close', { page: opened.structured.page });
    }
    const fetched = await c.call('net_fetch', { url: `${named}/data.json` });
    check(!fetched.isError && fetched.structured.status === 200, "an agent's profiles do not take net_fetch's", fetched.text);
  });
  stun.close();

  // ---- `*`: below the hooks, the resolver's rules. 127.0.0.2 is this machine too,
  // and neither the rules nor a policy knew it. Naming an origin keeps this testable.
  const second = http.createServer((request, response) => { hits.push({ url: request.url }); response.writeHead(200); response.end('x'); });
  second.on('upgrade', (request, socket) => { hits.push({ url: request.url, websocket: true }); socket.destroy(); });
  const secondPort = await new Promise(resolve => second.listen(0, '127.0.0.2', () => resolve(second.address().port)));
  await withClient(['--allow-origin', '*', '--allow-origin', named], async c => {
    const page = (await c.call('page_open', { url: `${named}/page` })).structured.page;
    await quiet(c, page, `new WebSocket('ws://127.0.0.2:${secondPort}/socket'); 1`, '--allow-origin *: a WebSocket to another address of this machine');
    const workers = await c.call('page_evaluate', { page, script: "typeof navigator.serviceWorker + ' ' + typeof SharedWorker" });
    check(workers.structured.value === 'undefined undefined', '--allow-origin *: no service or shared workers', workers.text);
    for (const url of [`http://[::7f00:1]:${sitePort}/`, `http://[64:ff9b::7f00:1]:${sitePort}/`, `http://[2002:7f00:1::]:${sitePort}/`, 'http://[fec0::1]/', 'http://[ff02::1]/', 'http://224.0.0.1/', 'http://255.255.255.255/', 'http://198.18.0.1/', 'http://192.0.0.1/', 'http://router.lan/', 'http://box.localdomain/', 'http://nas.home.arpa/']) {
      const answer = await c.call('net_fetch', { url });
      check(answer.isError && /outside the origins/.test(answer.text), `--allow-origin *: not ${url}`, answer.text);
    }
  });
  second.close();
  await withClient(['--allow-origin', 'http://[::1]:9'], async c => {
    const answer = await c.call('page_open', { url: 'http://[::1]:9/' });
    check(answer.isError && !/NAME_NOT_RESOLVED/.test(answer.text), 'an IPv6 origin named resolves', answer.text);
  });

  // ---- net_fetch across origins.
  await withClient(['--allow-origin', SITE, '--allow-origin', OUTSIDE], async c => {
    const before = hits.length;
    const fetched = await c.call('net_fetch', { url: `${SITE}/to-outside`, method: 'POST', text: 'b', headers: [{ name: 'Authorization', value: 'Bearer mine' }, { name: 'X-Api-Key', value: 'k' }, { name: 'Content-Type', value: 'text/plain' }] });
    const reached = hits.slice(before);
    check(!fetched.isError && reached.length === 1 && reached[0].url === '/redirected' && !reached[0].authorization && !reached[0].apiKey && !reached[0].contentType, "a redirect to another origin takes nothing of the first request's along", { text: fetched.text.slice(0, 200), reached });
  });

  // ---- Files, images, media, documents, windows, the device flow.
  await withClient(fullBounds, async c => {
    fs.writeFileSync(path.join(readDir, 'wide.txt'), 'é'.repeat(100));
    const cut = await c.call('file_read', { path: path.join(readDir, 'wide.txt'), maxBytes: 3 });
    check(cut.structured.text === 'é' && cut.structured.nextOffset === 2, 'file_read: a piece ending inside a character', cut.text);
    const next = await c.call('file_read', { path: path.join(readDir, 'wide.txt'), offset: 2, maxBytes: 4 });
    check(next.structured.text === 'éé', 'file_read: and the next piece', next.text);
    const cutFetch = await c.call('net_fetch', { url: `${SITE}/wide`, maxBytes: 3 });
    check(cutFetch.structured.text === 'é' && cutFetch.structured.truncated === true, 'net_fetch: a piece ending inside a character', cutFetch.text);

    for (const windows of ['C:\\Users\\x', '\\\\server\\share\\x', '..\\..\\x']) {
      const answer = await c.call('file_read', { path: windows });
      check(answer.isError && /absolute/.test(answer.text), `file_read: ${windows} is no absolute path here`, answer.text);
    }
    const backslashes = await c.call('file_read', { path: `${readDir}/..\\outside\\secret.txt` });
    check(backslashes.isError && !/the secret/.test(backslashes.text), 'file_read: backslashes are part of a name here', backslashes.text);

    const header = Buffer.from('89504e470d0a1a0a0000000d49484452', 'hex');
    const size = Buffer.alloc(8);
    size.writeUInt32BE(50000, 0);
    size.writeUInt32BE(50000, 4);
    const bomb = Buffer.concat([header, size, Buffer.from('08060000005c72a866', 'hex')]);
    const decoded = await c.call('image_decode', { base64: bomb.toString('base64') });
    check(decoded.isError && /over the limit/.test(decoded.text), 'image_decode: pixels without end are refused by the header', decoded.text);

    const frames = await c.call('media_extract_frames', { input: path.join(readDir, 'clip.mp4'), output: path.join(writeDir, 'huge.rgba'), width: 8192, height: 8192, frames: 100000 });
    check(frames.isError && /over the limit/.test(frames.text), 'media_extract_frames: an output without end is refused', frames.text);

    const pairs = await Promise.all(['plain.pdf', 'other.pdf'].map(name => c.call('pdf_open', { path: path.join(readDir, name) })));
    const [a, b] = pairs.map(pair => pair.structured.document);
    const crossing = Promise.all(Array.from({ length: 24 }, (_, i) => c.call('pdf_edit_pages', { document: i % 2 ? a : b, from: i % 2 ? b : a, action: 'import', pages: [0], at: 0 })));
    const crossed = await Promise.race([crossing.then(() => 'answered'), sleep(60000).then(() => 'stuck')]);
    check(crossed === 'answered', 'two imports each way round do not wait for each other', crossed);

    const small = (await c.call('ui_open', { width: 200, height: 100, content: { type: 'view' } })).structured.window;
    const large = (await c.call('ui_open', { width: 500, height: 400, content: { type: 'view' } })).structured.window;
    const pictures = await Promise.all(Array.from({ length: 6 }, (_, i) => c.call('ui_capture', { window: i % 2 ? large : small })));
    const widths = pictures.map(picture => Buffer.from(picture.image.data, 'base64').readUInt32BE(16));
    check(widths.every((width, i) => width === widths[i % 2]) && widths[0] !== widths[1], 'captures at once each get their own window', widths);

    // A provider's numbers must not end the server.
    const huge = await c.call('auth_device_start', { provider: 'huge' });
    check(!huge.isError && huge.structured.flow, 'a provider asking for a time without end', huge.text);
    const long = await c.call('auth_device_start', { provider: 'long' });
    await sleep(1100);
    const longPoll = await c.call('auth_device_poll', { flow: long.structured.flow });
    check(longPoll.structured.status === 'approved', 'a token that lasts for ever', longPoll.text);
    check(!(await c.call('data_hash', { text: 'a' })).isError, 'and the server is still there');

    // What echoes the token, however.
    const flow = long.structured.flow;
    const echoHeader = await c.call('net_fetch', { url: `${OAUTH}/api/echo-header`, auth: flow });
    check(!JSON.stringify(echoHeader.result).includes(provider.token) && JSON.stringify(echoHeader.structured.headers).includes('[token]'), 'the token echoed in a header is not shown', echoHeader.text.length);
    const echoBinary = await c.call('net_fetch', { url: `${OAUTH}/api/echo-binary`, auth: flow });
    check(!Buffer.from(echoBinary.structured.base64 ?? '', 'base64').includes(provider.token), 'the token echoed in binary is not shown');
    const echoFile = path.join(writeDir, 'echoed');
    await c.call('net_fetch', { url: `${OAUTH}/api/echo`, auth: flow, outputPath: echoFile });
    check(!fs.readFileSync(echoFile, 'utf8').includes(provider.token), 'the token echoed goes into no file');
    const echoCut = await c.call('net_fetch', { url: `${OAUTH}/api/echo`, auth: flow, maxBytes: 25 });
    check(!/Bearer tok-/.test(echoCut.structured.text), 'and maxBytes does not cut it past the redaction', echoCut.text);

    // A poll that overtakes another must not fail the flow.
    const slow = await c.call('auth_device_start', { provider: 'slow' });
    await sleep(1100);
    const first = c.call('auth_device_poll', { flow: slow.structured.flow });
    await sleep(1200);
    const overtaking = await c.call('auth_device_poll', { flow: slow.structured.flow });
    const done = await first;
    check(overtaking.structured.status === 'pending' && done.structured.status === 'approved', 'a poll while one is on its way waits for it', { overtaking: overtaking.text, done: done.text });
    const statuses = await c.call('auth_status', {});
    check(statuses.structured.flows.find(item => item.flow === slow.structured.flow)?.status === 'approved', 'and the flow stays approved', statuses.text);
  });
  fs.rmSync(path.join(readDir, 'wide.txt'), { force: true });
}

/** With a real window, under a private X server. */
async function withWindows() {
  const xvfb = spawn('Xvfb', ['-displayfd', '1', '-screen', '0', '1600x1200x24', '-nolisten', 'tcp'], { stdio: ['ignore', 'pipe', 'ignore'] });
  const display = await new Promise(resolve => {
    xvfb.on('error', () => resolve(null));
    xvfb.stdout.once('data', data => resolve(String(data).trim()));
    setTimeout(() => resolve(null), 15000);
  });
  if (!display) {
    console.log('note --windows: skipped (no Xvfb)');
    return;
  }
  try {
    const env = { DISPLAY: `:${display}`, WAYLAND_DISPLAY: '' };
    const windowed = engine.filter(item => item !== '--headless');
    const server = await stdioClient(sdk, binary, ['--windows', '--allow-origin', SITE, ...windowed], env);
    const c = wrap(server.client, transcript);
    const open = await c.call('page_open', { url: `${SITE}/page`, window: true });
    check(!open.isError && open.structured.title === 'Form page', '--windows: a page in a window', open.text);
    const refused = await c.call('page_open', { window: false });
    check(refused.isError && /--windows/.test(refused.text), '--windows: without a window is refused', refused.text);
    const list = await c.call('page_list', {});
    check(list.structured.windows === true, '--windows: page_list says so', list.text);
    const shot = await c.call('page_capture', { page: open.structured.page });
    check(!!shot.image, '--windows: captured', shot.text);
    const clicked = await c.call('page_click', { page: open.structured.page, selector: '#hover' });
    check(!clicked.isError, '--windows: input', clicked.text);
    const ui = await c.call('ui_open', { title: 'On screen', content: { type: 'button', name: 'b', children: ['Press'] } });
    check(!ui.isError, '--windows: a native window', ui.text);
    await server.close();
  } finally {
    xvfb.kill();
  }
}

// ---- Run ------------------------------------------------------------------------

let stderrLogs = '';
{
  const server = await stdioClient(sdk, binary, [...fullBounds, ...engine]);
  try {
    const info = server.client.getServerVersion();
    check(info.name === 'zebkit-mcp', 'stdio: the SDK connects', info);
    check(server.client.getServerCapabilities().tools !== undefined, 'stdio: capabilities');
    await everyTool('stdio', wrap(server.client, transcript));
  } finally {
    stderrLogs += server.log.text;
    await server.close();
  }
}

{
  const tokenFile = path.join(work, 'token');
  const server = startHttp(binary, [...fullBounds, '--http-token-file', tokenFile, '--http-allow-origin', 'https://client.example', '--http-allow-host', 'operator.example', ...engine]);
  try {
    const port = await server.port;
    const token = fs.readFileSync(tokenFile, 'utf8');
    check((fs.statSync(tokenFile).mode & 0o077) === 0 && token.length >= 32, 'HTTP: a token generated into a private file');
    check(!server.log.text.includes(token), 'HTTP: a token in a file is not printed');
    const client = await httpClient(sdk, port, token);
    await everyTool('http', wrap(client, transcript));
    await client.close();
    await httpSecurity(port, token);
    const noAuth = await httpClient(sdk, port, 'wrong').then(() => 'connected', error => String(error));
    check(noAuth !== 'connected', 'HTTP: the SDK with a wrong token cannot connect', noAuth);
  } finally {
    stderrLogs += server.log.text;
    await stop(server.child);
  }
}

{
  const server = startHttp(binary, [...engine]);
  const port = await server.port;
  const printed = server.log.text.match(/bearer token \(shown once\): ([0-9a-f]{64})/);
  check(!!printed, 'HTTP: without a file, a token is generated and shown once');
  const list = await raw(port, { headers: { Authorization: `Bearer ${printed?.[1]}`, 'Content-Type': 'application/json', Accept: 'application/json, text/event-stream', 'MCP-Protocol-Version': '2026-07-28', 'Mcp-Method': 'tools/list' }, body: JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'tools/list', params: { _meta: META } }) });
  check(list.status === 200, 'HTTP: and it works', list.status);
  check((await raw(port, { headers: { 'Content-Type': 'application/json', Accept: 'application/json, text/event-stream' }, body: '{}' })).status === 401, 'HTTP: there is no way in without it');
  await stop(server.child);
}

// A temporary profile, with what pages stored, does not outlive the server.
{
  const temporary = path.join(work, 'temporary');
  fs.mkdirSync(temporary);
  const env = { TMPDIR: temporary };
  const server = startHttp(binary, ['--allow-origin', SITE, ...engine], env);
  await server.port;
  check(fs.readdirSync(temporary).length > 0, 'a temporary profile while it runs');
  await stop(server.child);
  check(fs.readdirSync(temporary).length === 0, 'a temporary profile is gone after SIGTERM', fs.readdirSync(temporary));
  const piped = await stdioClient(sdk, binary, [...engine], env);
  await piped.close();
  for (let i = 0; i < 100 && fs.readdirSync(temporary).length; i++) await sleep(100);
  check(fs.readdirSync(temporary).length === 0, 'a temporary profile is gone when standard input ends', fs.readdirSync(temporary));

  // With a page that stored something: its context's own directory goes too.
  const paged = startHttp(binary, ['--allow-origin', SITE, '--http-token-file', path.join(work, 'token-paged'), ...engine], env);
  const pagedPort = await paged.port;
  const pagedClient = await httpClient(sdk, pagedPort, fs.readFileSync(path.join(work, 'token-paged'), 'utf8'));
  const stored = await wrap(pagedClient, transcript).call('page_open', { url: `${SITE}/page` });
  await wrap(pagedClient, transcript).call('page_evaluate', { page: stored.structured.page, script: "localStorage.setItem('kept', 'x'); 1" });
  await pagedClient.close();
  await stop(paged.child);
  check(fs.readdirSync(temporary).length === 0, 'what a page stored is gone at the end', fs.readdirSync(temporary));

  // A server killed cannot clean up: the next one does, and leaves those alive alone.
  const killed = startHttp(binary, [...engine], env);
  await killed.port;
  killed.child.kill('SIGKILL');
  await new Promise(resolve => (killed.child.exitCode !== null || killed.child.signalCode ? resolve() : killed.child.once('exit', resolve)));
  const orphan = fs.readdirSync(temporary);
  const alive = `zebkit-mcp-${process.pid}-alive`;
  fs.mkdirSync(path.join(temporary, alive));
  const next = startHttp(binary, [...engine], env);
  await next.port;
  const during = fs.readdirSync(temporary);
  check(orphan.length === 1 && !during.includes(orphan[0]) && during.includes(alive), 'a directory of a server killed goes at the next start, one with a process stays', { orphan, during });
  await stop(next.child);
  fs.rmSync(path.join(temporary, alive), { recursive: true });

  // What it says on starting is where it listens.
  const elsewhere = spawn(binary, ['--http', '0', '--http-bind', '127.0.0.2', ...engine], { stdio: ['ignore', 'ignore', 'pipe'], env: { ...process.env, ...env } });
  let said = '';
  elsewhere.stderr.on('data', data => { said += data; });
  for (let i = 0; i < 600 && !/listening/.test(said); i++) await sleep(100);
  check(/listening on http:\/\/127\.0\.0\.2:/.test(said), '--http-bind: it says the address it was given', said.slice(0, 200));
  await stop(elsewhere);
}

await modernStdio();
await defaults();
await reviewed();
await withWindows();

// Across everything every run printed and every tool answered:
const secrets = { 'the access token': provider.token, 'the refresh token': provider.refresh, 'the device code': provider.deviceCode, 'the client secret': 'shh-secret' };
for (const [what, value] of Object.entries(secrets)) {
  check(!stderrLogs.includes(value), `${what} is not in the logs`);
  check(!transcript.some(item => item.includes(value)), `${what} is in no tool's result`);
}
check(!transcript.some(item => item.includes('the secret')), 'the file outside the grants was never read');

site.close(); outside.close(); oauth.close();
site.closeAllConnections?.(); outside.closeAllConnections?.(); oauth.closeAllConnections?.();
fs.rmSync(work, { recursive: true, force: true });
process.exit(finish());
