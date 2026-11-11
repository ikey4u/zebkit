// The hooks through host JavaScript; run by tests/engine/hook_policies.py.
import {activated} from './helpers.mjs';

const env = JSON.parse(zebkit.args[0]);
const {ZEBKIT_HOOK_MODE: mode, ZEBKIT_HOOK_BASE: base, ZEBKIT_HOOK_TLS: tls, ZEBKIT_HOOK_FILES: files,
       ZEBKIT_HOOK_DOWNLOADS: downloads, cert, key, otherKey} = env;
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
function must(ok, what) { if (!ok) throw new Error(`${mode}: ${what}`); }
async function until(test, what, ms = 15000) {
  for (const end = Date.now() + ms; Date.now() < end; await sleep(5)) if (await test()) return;
  throw new Error(`${mode}: timed out: ${what}`);
}
function rejects(fn, what) {
  let thrown = false;
  try { fn(); } catch (error) { thrown = error instanceof TypeError; }
  must(thrown, `accepted: ${what}`);
}
const view = new zebkit.View('');
let closed = false, finished = false, failed = false;
view.on('close', () => { closed = true; });
view.on('load', (phase, url) => { if (phase === 'finished') finished = true; if (phase === 'failed') failed = true; });
async function load(url) { finished = failed = false; view.load(url); }
async function openPage() {
  await load(base + '/page');
  await until(() => finished && view.url === base + '/page', 'page');
  // The first document's own "finished" may be what was seen.
  for (let t = 0; t < 500 && await view.evaluate(`location.pathname + ' ' + document.readyState`) !== '/page complete'; t++)
    await new Promise(done => setTimeout(done, 10));
}
const fail = error => { console.log(`FAIL ${error && error.stack || error}`); zebkit.quit(); throw error; };

async function external() {
  await openPage();
  const told = [];
  if (mode !== 'external-none') view.onExternalProtocol(event => { told.push(event); if (mode === 'external-close') view.close(); });
  await view.evaluate(`location.href = 'zoommtg://join?x=1'; 1`);
  if (mode === 'external-none') {
    await sleep(300);
    must(await view.evaluate('location.pathname') === '/page', 'stayed');
    view.close();
    return;
  }
  await until(() => told.length, 'told');
  must(told[0].scheme === 'zoommtg' && told[0].mainFrame && told[0].initiatorOrigin === base, JSON.stringify(told[0]));
  if (mode === 'external-close') { await until(() => closed, 'closed'); return; }
  must(await view.evaluate('location.pathname') === '/page', 'stayed');
  view.onExternalProtocol(null);
  await view.evaluate(`location.href = 'mailto:a@example.com'; 1`);
  await sleep(300);
  must(told.length === 1, 'removed');
  view.close();
}

async function download() {
  const target = downloads + '/saved.bin';
  await zebkit.fs.writeFile(target, new TextEncoder().encode('old'));
  const size = async () => (await zebkit.fs.stat(target)).size;
  await openPage();
  const progress = [];
  let kept = null;
  view.onDownloadProgress(event => progress.push(event));
  if (mode !== 'download-default') view.onDownload(request => {
    kept = request;
    must(request.metadata.suggestedName === 'report.bin' && request.metadata.totalBytes === 65536, 'metadata');
    must(JSON.stringify(request.info) === JSON.stringify(request.metadata), 'info');
    if (mode === 'download-save') {
      must(!request.respond('save', 'relative.bin') && !request.respond('cancel', target), 'invalid replies');
      must(request.respond('save', target) && !request.respond('cancel') && request.info === null, 'once');
    } else if (mode === 'download-cancel') must(request.respond('cancel'), 'cancel');
    else if (mode === 'download-close') view.close();
  });
  if (mode === 'download-timeout') {
    rejects(() => view.setDownloadTimeout(0), 'deadline');
    view.setDownloadTimeout(50);
  }
  await view.evaluate(`location.href = '/download.bin'; 1`);
  if (mode === 'download-default') {
    await until(() => progress.some(e => e.done && e.state === 'complete' && e.path.replace(/\\/g, '/').endsWith('/Downloads/report.bin')), 'saved in the profile');
    view.close();
    return;
  }
  await until(() => kept, 'asked');
  if (mode === 'download-save') {
    await until(() => progress.some(e => e.state === 'complete'), 'complete');
    must(await size() === 65536, 'bytes');
  } else if (mode === 'download-cancel') {
    await until(() => progress.some(e => e.state === 'cancelled' && e.done), 'cancelled');
    must(await size() === 3, 'untouched');
  } else if (mode === 'download-timeout') {
    await until(() => progress.some(e => e.reason === 'timeout'), 'timeout');
    must(kept.info === null && !kept.respond('save', target) && await size() === 3, 'late');
  } else if (mode === 'download-close') {
    await until(() => closed, 'closed');
    must(kept.info === null, 'stale');
    return;
  } else if (mode === 'download-pending-close') {
    view.close();
    await until(() => closed, 'closed');
    must(kept.info === null && !kept.respond('save', target), 'stale');
    return;
  } else if (mode === 'download-replace') {
    view.onDownload(null);
    must(kept.info === null, 'stale');
    await until(() => progress.some(e => e.reason === 'handler-changed'), 'replaced');
  }
  view.close();
}

async function certificate() {
  view.onCertificate(request => request.respond(true));
  const results = [];
  let kept = null;
  view.onClientCertificateResult(event => results.push(event));
  if (mode !== 'cert-default') view.onClientCertificate(request => {
    kept = request;
    must(request.metadata.host === 'localhost', 'host');
    if (mode === 'cert-provide') {
      must(!request.respond('provide', 'junk', key) && !request.respond('provide', cert, 'junk'), 'invalid');
      must(!request.respond('provide', cert, otherKey), 'stranger key');
      rejects(() => request.respond('sometimes'), 'action');
      must(request.respond('provide', cert, key) && !request.respond('none') && request.info === null, 'once');
    } else if (mode === 'cert-none') must(request.respond('none'), 'none');
    else if (mode === 'cert-cancel') must(request.respond('cancel'), 'cancel');
    else if (mode === 'cert-close') view.close();
  });
  if (mode === 'cert-timeout') view.setClientCertificateTimeout(50);
  await load(tls + '/secure');
  if (mode === 'cert-provide') await until(() => view.title === 'cert:zebkit-client', 'with it');
  else if (mode === 'cert-none' || mode === 'cert-default') await until(() => view.title === 'cert:none', 'without');
  else if (mode === 'cert-cancel' || mode === 'cert-timeout') {
    await until(() => failed && results.length, 'failed');
    must(results[0].state === 'cancelled', 'state');
  } else if (mode === 'cert-close') {
    await until(() => closed, 'closed');
    must(kept.info === null, 'stale');
    return;
  } else if (mode === 'cert-pending-close') {
    await until(() => kept, 'asked');
    view.close();
    await until(() => closed, 'closed');
    must(kept.info === null && !kept.respond('none'), 'stale');
    return;
  } else if (mode === 'cert-replace') {
    await until(() => kept, 'asked');
    view.onClientCertificate(null);
    must(kept.info === null, 'stale');
    await until(() => failed && results.length, 'failed');
    must(results[0].reason === 'handler-changed', 'reason');
  }
  view.close();
}

async function world() {
  const all = `var worldVar = 1; addEventListener('zkping', () => document.documentElement.setAttribute('data-all', typeof pageVar === 'undefined' ? 'isolated' : 'leak'));`;
  const main = `addEventListener('zkping', () => document.documentElement.setAttribute('data-main', typeof pageVar === 'undefined' ? 'isolated' : 'leak'));`;
  view.addWorldScript(all, {world: 'zebkit-test'});
  view.addWorldScript(main, {world: 'zebkit-main', mainFrameOnly: true});
  view.addWorldScript('window.mainWorldVar = 1;', {mainFrameOnly: true});
  rejects(() => view.addWorldScript('1', {world: 'x'.repeat(257)}), 'long name');
  await load(base + '/frames');
  await until(() => finished, 'loaded');
  await until(async () => await view.evaluate('window.reports && reports.length') === 2, 'frames');
  const top = await view.evaluate('report');
  must(top.all === 'isolated' && top.main === 'isolated' && top.global === 'undefined' && top.mainWorld === 'number', JSON.stringify(top));
  for (const frame of await view.evaluate('reports'))
    must(frame.all === 'isolated' && frame.main === null && frame.global === 'undefined' && frame.mainWorld === 'undefined', JSON.stringify(frame));
  view.close();
}

// What a call still waiting for its reply is told when the view closes (src/engine/view.cc).
const closing = ['the page target was detached', 'the page was closed', 'the view was closed'];

async function chooser() {
  await openPage();
  const one = files + '/one.txt', two = files + '/two.txt';
  let kept = null;
  const results = [];
  view.onFileChooserResult(event => results.push(event));
  view.onFileChooser(request => {
    kept = request;
    must(request.metadata.origin === base, 'origin');
    if (mode === 'file-open') {
      must(!request.respond(['relative.txt']) && !request.respond([one, two]), 'invalid');
      rejects(() => request.respond([1]), 'not a string');
      must(request.respond([one]) && !request.respond([]) && request.info === null, 'once');
    } else if (mode === 'file-multiple') must(request.respond([one, two]), 'two');
    else if (mode === 'file-folder') must(request.respond([files + '/folder']), 'folder');
    else if (mode === 'file-cancel') must(request.respond([]), 'cancel');
    else if (mode === 'file-close') view.close();
  });
  if (mode === 'file-timeout') view.setFileChooserTimeout(50);
  const element = {'file-multiple': 'm', 'file-folder': 'd'}[mode] || 'i';
  // With file-close, the handler's close can come before the reply to this click, which then
  // fails as the page goes, as CDP fails a detached target's commands.
  await view.evaluate(`${element}.addEventListener('cancel', () => window.cancelled = 1); ${element}.click(); 1`)
    .catch(error => { if (mode !== 'file-close' || !closing.includes(error.message)) throw error; });
  await until(() => kept, 'asked');
  const count = n => until(async () => await view.evaluate(`${element}.files.length`) === n, 'files');
  if (mode === 'file-open') { await count(1); must(await view.evaluate('i.files[0].text()') === 'first file', 'bytes'); }
  else if (mode === 'file-multiple') { await count(2); must(await view.evaluate('m.files[1].text()') === 'second file', 'bytes'); }
  else if (mode === 'file-folder') { await count(2); must(await view.evaluate(`[...d.files].map(f => f.webkitRelativePath).sort().join()`) === 'folder/a.txt,folder/sub/b.txt', 'paths'); }
  else if (mode === 'file-cancel' || mode === 'file-timeout') {
    await until(() => results.length, 'told');
    must(results[0].state === 'cancelled' && results[0].reason === (mode === 'file-timeout' ? 'timeout' : 'answered'), 'result');
    await until(async () => await view.evaluate('window.cancelled === 1'), 'page told');
  } else if (mode === 'file-close') {
    await until(() => closed, 'closed');
    must(kept.info === null, 'stale');
    return;
  } else if (mode === 'file-pending-close') {
    view.close();
    await until(() => closed, 'closed');
    must(kept.info === null && !kept.respond([one]), 'stale');
    return;
  } else if (mode === 'file-replace') {
    view.onFileChooser(null);
    must(kept.info === null, 'stale');
    await until(() => results.length, 'told');
    must(results[0].reason === 'handler-changed', 'reason');
  }
  view.close();
}

async function media() {
  await openPage();
  let kept = null, answer = null;
  const results = [];
  view.onMediaAccessResult(event => results.push(event));
  view.onMediaAccess(request => {
    kept = request;
    must(request.metadata.video && !request.metadata.audio && request.metadata.videoDevices.length, 'metadata');
    if (mode === 'media-allow') must(request.respond(true) && !request.respond(false), 'once');
    else if (mode === 'media-deny') must(!request.respond(false, null, 'device') && request.respond(false), 'deny');
    else if (mode === 'media-close') view.close();
  });
  if (mode === 'media-timeout') view.setMediaAccessTimeout(50);
  view.evaluate(`navigator.mediaDevices.getUserMedia({video: true}).then(s => { const n = s.getTracks().length; s.getTracks().forEach(t => t.stop()); return 'ok' + n; }, e => e.name)`)
      .then(value => { answer = value; }, () => { answer = 'closed'; });
  await until(() => kept, 'asked');
  if (mode === 'media-allow') { await until(() => answer, 'answered'); must(answer === 'ok1', answer); }
  else if (mode === 'media-deny' || mode === 'media-timeout') {
    await until(() => answer && results.length, 'answered');
    must(answer === 'NotAllowedError' && results[0].state === 'denied', answer);
  } else if (mode === 'media-close') {
    await until(() => closed, 'closed');
    must(kept.info === null, 'stale');
    return;
  } else if (mode === 'media-pending-close') {
    view.close();
    await until(() => closed, 'closed');
    must(kept.info === null && !kept.respond(true), 'stale');
    return;
  } else if (mode === 'media-replace') {
    view.onMediaAccess(null);
    must(kept.info === null, 'stale');
    await until(() => answer, 'answered');
    must(answer === 'NotAllowedError', answer);
  }
  view.close();
}

async function keys() {
  rejects(() => view.setReservedKeys(['Ctrl+Nonsense']), 'code');
  rejects(() => view.setReservedKeys('x'), 'not a list');
  view.setReservedKeys(['Ctrl+KeyK']);
  const told = [];
  view.onReservedKey(event => told.push(event));
  await load(base + '/input');
  await until(() => told.length, 'told');
  must(told[0].combo === 'Ctrl+KeyK' && told[0].code === 'KeyK', JSON.stringify(told[0]));
  await until(async () => await view.evaluate('keys.length') > 0, 'other key');
  must(await view.evaluate(`keys.every(k => k === 'KeyB')`), 'page did not get it');
  view.close();
}

async function menu() {
  const told = [];
  view.onContextMenu(event => told.push(event));
  await load(base + '/input');
  await until(() => told.length, 'told');
  must(told[0].x === 20 && told[0].y === 30 && told[0].linkUrl === base + '/page', JSON.stringify(told[0]));
  must(await view.evaluate('window.menu') === 1, 'event');
  view.close();
}

async function fullscreen() {
  // Once the evaluation's user activation has reached the browser (it goes on a
  // pipe of its own, and the call consumes it there).
  const FULL = activated(`document.documentElement.requestFullscreen()`) + `.then(() => 'in', e => e.name)`;
  await openPage();
  const told = [];
  view.onFullscreen(event => told.push(event));
  // Since Chromium 157 only a view with focus goes fullscreen.
  view.focus();
  rejects(() => view.setFullscreenPolicy('sometimes'), 'policy');
  if (mode === 'fullscreen-deny') {
    view.setFullscreenPolicy('deny');
    must(await view.evaluate(FULL) === 'TypeError', 'denied');
    await sleep(200);
    must(!told.length, 'not told');
    view.setFullscreenPolicy('allow');
  }
  must(await view.evaluate(FULL) === 'in', 'in');
  await until(() => told.length && told.at(-1).entered, 'entered');
  if (mode === 'fullscreen-deny') view.setFullscreenPolicy('deny');
  else await view.evaluate('document.exitFullscreen().then(() => 1)');
  await until(() => !told.at(-1).entered, 'left');
  view.close();
}

const wsScript = `new Promise(ok => { const s = window.s = new WebSocket(location.origin.replace('http', 'ws') + '/ws'); s.onmessage = e => { ok(e.data); s.close(); }; s.onerror = () => ok('failed'); })`;
const swScript = what => `navigator.serviceWorker.register('/net-sw.js').then(r => new Promise(ok => { const w = r.installing || r.waiting || r.active; if (w.state === 'activated') return ok(w); w.addEventListener('statechange', () => { if (w.state === 'activated') ok(w); }); })).then(w => new Promise(ok => { navigator.serviceWorker.onmessage = e => ok(e.data); w.postMessage('${what}'); }))`;
const sharedScript = what => `new Promise(ok => { const w = window.w = window.w || new SharedWorker('/net-shared.js'); w.port.onmessage = e => ok(e.data); w.port.postMessage('${what}'); })`;
const pageReads = url => `fetch('${url}').then(r => r.arrayBuffer()).then(b => b.byteLength, () => 'failed')`;
const told = (events, kind) => events.some(e => e.kind === kind);
const later = script => { const box = {done: false, value: null}; view.evaluate(script).then(value => { box.done = true; box.value = value; }, () => { box.done = true; }); return box; };

async function networkWs() {
  await openPage();
  const events = [];
  let kept = null, asked = 0;
  view.onNetworkEvent(event => events.push(event));
  view.setNetworkFilter({'net-ws-none': [], 'net-ws-filter': [{url: '**/ws', resourceTypes: ['fetch']}],
                         'net-ws-continue': [{url: '**/ws', resourceTypes: ['websocket']}]}[mode] || [{url: '**/ws'}]);
  if (mode !== 'net-ws-nofunc') view.onNetwork(request => {
    kept = request; ++asked;
    const m = request.metadata;
    if (mode === 'net-ws-block') {
      must(m.resourceType === 'websocket' && m.url.startsWith('ws://') && m.stage === 'request', 'metadata');
      must(!request.respond({action: 'redirect', url: 'http://127.0.0.1/'}) && !request.respond({action: 'fulfill'}), 'refused');
      must(request.respondStream() === null && !request.respondFile('/nothing'), 'no answers');
      must(!request.respond({action: 'continue', headers: [{name: 'Origin', value: 'http://x'}]}), 'origin');
      must(request.respond({action: 'block'}), 'block');
    } else if (mode === 'net-ws-continue') must(request.respond({action: 'continue', headers: [{name: 'X-Zebkit', value: 'yes'}]}), 'continue');
    else if (mode === 'net-ws-close') view.close();
  });
  if (mode === 'net-ws-timeout') view.setNetworkTimeout(50);
  const box = later(wsScript);
  if (mode === 'net-ws-close') { await until(() => closed, 'closed'); must(kept.info === null, 'stale'); return; }
  if (mode === 'net-ws-abort') {
    await until(() => kept, 'asked');
    await view.evaluate('s.close()');
    await until(() => kept.info === null, 'closed by the page');
    await until(() => box.done, 'ended');
    view.close();
    return;
  }
  if (mode === 'net-ws-replace') {
    await until(() => kept, 'asked');
    view.onNetwork(null);
    must(kept.info === null, 'stale');
    await until(() => events.some(e => e.reason === 'handler-changed'), 'replaced');
  }
  await until(() => box.done, 'answered');
  must(box.value === (mode === 'net-ws-block' ? 'failed' : 'hello'), box.value);
  if (mode === 'net-ws-timeout') await until(() => events.some(e => e.reason === 'timeout'), 'timeout');
  if (['net-ws-none', 'net-ws-nofunc', 'net-ws-filter'].includes(mode)) must(!asked, 'held');
  view.close();
}

async function networkWorkers() {
  await openPage();
  let asked = 0;
  must(!view.setNetworkFilterForWorkers(true), 'no filter');
  view.setNetworkFilter([{url: mode === 'net-sw-ws' ? '**/ws' : '**/worker-held'}]);
  view.onNetwork(request => {
    ++asked;
    const m = request.metadata;
    const type = {'net-sw-ws': 'shared-or-service', 'net-sw-block': 'service', 'net-sw-running': 'service'}[mode] || 'shared';
    must(m.worker && m.frameId === '' && m.workerType === type, JSON.stringify(m));
    must(request.respond({action: 'block'}), 'block');
  });
  if (['net-sw-block', 'net-sw-ws', 'net-shared-block'].includes(mode)) must(view.setNetworkFilterForWorkers(true), 'set');
  const script = (mode.startsWith('net-shared') ? sharedScript : swScript)(mode === 'net-sw-ws' ? 'socket' : 'fetch');
  if (mode.endsWith('-running')) {
    must(await view.evaluate(script) === 'through' && !asked, 'before');
    must(view.setNetworkFilterForWorkers(true), 'set');
  }
  const answer = await view.evaluate(script);
  if (mode.endsWith('-none')) must(answer === 'through' && !asked, answer);
  else must(answer === 'failed' && asked === 1, answer);
  must(view.setNetworkFilterForWorkers(false), 'off');
  view.close();
}

async function networkBody() {
  await openPage();
  const events = [];
  let bytes = 0, ends = 0, truncated = 0, late = 0, id = null, second = 0;
  rejects(() => view.setNetworkFilter([{body: true}]), 'request stage');
  rejects(() => view.setNetworkFilter([{stage: 'response', mode: 'hold', body: true}]), 'hold');
  view.setNetworkFilter(['/download.bin', '/big', '/cut', '/slow-text'].map(path => ({url: '**' + path, stage: 'response', body: true})));
  view.onNetworkEvent(event => events.push(event));
  if (mode !== 'net-body-none') view.onNetworkBody((requestId, data, end, cut) => {
    must(data instanceof ArrayBuffer && !ends, 'a chunk');
    if (data.byteLength && mode === 'net-body') must(new Uint8Array(data)[0] === 0x7a, 'bytes');
    id = requestId; bytes += data.byteLength;
    if (end) { ++ends; if (cut) ++truncated; if (told(events, 'finished') || told(events, 'failed')) ++late; }
    if (data.byteLength && mode === 'net-body-close') view.close();
    if (data.byteLength && mode === 'net-body-replace') view.onNetworkBody(() => { ++second; });
  });
  if (mode === 'net-body' || mode === 'net-body-none') {
    must(await view.evaluate(pageReads('/download.bin')) === 65536, 'page');
    await until(() => told(events, 'finished'), 'finished');
    if (mode === 'net-body') {
      must(bytes === 65536 && ends === 1 && !truncated && !late, `${bytes} ${ends}`);
      must(id === events.find(e => e.kind === 'response').requestId, 'id');
    } else must(!ends, 'told');
  } else if (mode === 'net-body-big') {
    must(await view.evaluate(pageReads('/big')) === 65 << 20, 'page');
    must(bytes === 64 << 20 && ends === 1 && truncated === 1, `${bytes} ${ends} ${truncated}`);
  } else if (mode === 'net-body-backpressure') {
    must(await view.evaluate(`fetch('/big').then(r => { window.r = r; return 1; })`) === 1, 'response');
    await sleep(1000);
    must(!ends, `${bytes}`); // Blink's fetch reads ahead itself, so no bound can be seen from here.
    must(await view.evaluate('r.arrayBuffer().then(b => b.byteLength)') === 65 << 20, 'page');
    must(bytes === 64 << 20 && truncated === 1, `${bytes}`);
  } else if (mode === 'net-body-error') {
    must(await view.evaluate(pageReads('/cut')) === 'failed', 'page');
    await until(() => told(events, 'failed'), 'failed');
    must(bytes === 1000 && ends === 1 && !truncated && !late, `${bytes} ${ends}`);
  } else if (mode === 'net-body-abort') {
    const box = later(`(window.a = new AbortController(), fetch('/slow-text', {signal: a.signal}).then(r => r.arrayBuffer()).then(b => b.byteLength, () => 'failed'))`);
    await until(() => bytes > 0, 'first chunk');
    await view.evaluate('a.abort()');
    await until(() => box.done, 'aborted');
    must(box.value === 'failed', box.value);
    await until(() => told(events, 'failed'), 'failed');
    must(ends === 1 && !truncated && !late, `${ends}`);
  } else if (mode === 'net-body-close') {
    later(pageReads('/slow-text'));
    await until(() => closed, 'closed');
    const at = bytes;
    await sleep(200);
    must(bytes === at, 'told after closing');
    return;
  } else if (mode === 'net-body-replace') {
    must(await view.evaluate(pageReads('/big')) === 65 << 20, 'page');
    must(!second && !ends, 'told to the wrong one');
    must(await view.evaluate(pageReads('/download.bin')) === 65536, 'page');
    must(second >= 2, 'the new one');
  }
  view.close();
}

async function networkAnswers() {
  await openPage();
  const events = [];
  let stream = null, blocked = false;
  const text = [{name: 'Content-Type', value: 'text/plain'}, {name: 'X-Added', value: 'by-host'}];
  view.onNetworkEvent(event => events.push(event));
  view.setNetworkFilter([{url: '**/held'}]);
  view.onNetwork(request => {
    if (mode === 'net-file' || mode === 'net-file-close') {
      const path = files + '/big.bin';
      must(!request.respondFile('relative.bin') && !request.respondFile(path, {status: 302}), 'invalid');
      must(request.respondFile(path, {status: 201, headers: [{name: 'Content-Type', value: 'application/x-zebkit'}, {name: 'X-Added', value: 'by-host'}]}), 'file');
      if (mode === 'net-file-close') view.close();
    } else if (mode === 'net-file-missing') must(request.respondFile(files + '/nothing.bin'), 'answered');
    else {
      must(request.respondStream({status: 302}) === null, 'status');
      stream = request.respondStream({status: 201, headers: text});
      must(stream && stream.isOpen && request.info === null, 'stream');
      if (mode === 'net-stream') {
        must(stream.write('ab') && stream.write(new TextEncoder().encode('cd')), 'write');
        stream.end();
        must(!stream.isOpen && !stream.write('x'), 'ended');
        stream.end();
      } else if (mode === 'net-stream-fail') { must(stream.write('ab'), 'write'); stream.end(true); }
      else if (mode === 'net-stream-full') {
        must(!stream.write(new Uint8Array((1 << 20) + 1)), 'too much');
        // Blocks of 1 MiB, the most one write takes, 16 MiB in all: with smaller
        // ones a fast reader drains the engine's 1 MiB buffer between writes and
        // none is refused (hook_policies.c).
        const block = new Uint8Array(1 << 20).fill(0x66);
        let left = 16 << 20;
        const pour = () => {
          while (left > 0) {
            if (!stream.write(block)) { blocked = true; return; }
            left -= block.length;
          }
          stream.end();
        };
        stream.onWritable(pour);
        pour();
      } else view.close();
    }
  });
  if (mode === 'net-file') {
    const answer = await view.evaluate(`fetch('/held').then(async r => [r.status, r.headers.get('x-added'), r.headers.get('content-type'), (await r.arrayBuffer()).byteLength].join('|'))`);
    must(answer === '201|by-host|application/x-zebkit|3145728', answer);
  } else if (mode === 'net-file-missing' || mode === 'net-stream-fail') {
    must(await view.evaluate(pageReads('/held')) === 'failed', 'page');
    await until(() => told(events, 'failed'), 'failed');
  } else if (mode === 'net-file-close' || mode === 'net-stream-close') {
    later(pageReads('/held'));
    await until(() => closed, 'closed');
    if (stream) {
      await until(() => !stream.write('x'), 'nobody reads');
      must(!stream.isOpen, 'open');
      stream.end();
    }
    return;
  } else if (mode === 'net-stream') {
    const answer = await view.evaluate(`fetch('/held').then(async r => [r.status, r.headers.get('x-added'), await r.text()].join('|'))`);
    must(answer === '201|by-host|abcd', answer);
  } else if (mode === 'net-stream-full') {
    must(await view.evaluate(pageReads('/held')) === 16 << 20, 'page');
    must(blocked, 'a write was refused while the stream was full');
  }
  view.close();
}

async function networkRedirectModes() {
  await openPage();
  let asked = 0;
  view.setNetworkFilter([{url: '**/held'}]);
  view.onNetwork(request => { ++asked; must(request.respond({action: 'redirect', url: base + '/echo?redirected'}), 'redirect'); });
  const which = mode === 'net-redirect-manual' ? 'manual' : 'error';
  const answer = await view.evaluate(`fetch('/held', {redirect: '${which}'}).then(r => r.type, () => 'failed')`);
  must(answer === (which === 'manual' ? 'opaqueredirect' : 'failed') && asked === 1, answer);
  view.close();
}

async function network() {
  if (mode.startsWith('net-ws-')) return networkWs();
  if (mode.startsWith('net-sw-') || mode.startsWith('net-shared-')) return networkWorkers();
  if (mode.startsWith('net-body')) return networkBody();
  if (mode.startsWith('net-file') || mode.startsWith('net-stream')) return networkAnswers();
  if (mode === 'net-redirect-manual' || mode === 'net-redirect-error') return networkRedirectModes();
  await openPage();
  const rules = {'net-response': [{url: '**/echo', stage: 'response'}], 'net-response-block': [{url: '**/echo', stage: 'response'}],
    'net-observe': [{url: '**/echo', mode: 'observe'}, {url: '**/echo', mode: 'observe', stage: 'response'}], 'net-none': [],
    'net-filter': [{url: '**/echo', resourceTypes: ['image']}, {url: '**/echo', frames: 'sub'}],
    'net-server-redirect': [{url: '**/echo'}], 'net-post': [{url: '**/echo'}], 'net-headers': [{url: '**/echo'}],
    'net-worker': [{url: '**/echo?worker'}], 'net-navigation': [{url: '**/held', frames: 'main', resourceTypes: ['document']}]}[mode] || [{url: '**/held'}];
  rejects(() => view.setNetworkFilter([{mode: 'sometimes'}]), 'mode');
  view.setNetworkFilter(rules);
  let kept = null, asked = 0;
  const events = [];
  view.onNetworkEvent(event => events.push(event));
  const text = {name: 'Content-Type', value: 'text/plain'};
  view.onNetwork(request => {
    kept = request; ++asked;
    const m = request.metadata;
    must(JSON.stringify(request.info) === JSON.stringify(m), 'info');
    if (mode === 'net-block') {
      must(m.stage === 'request' && m.resourceType === 'fetch', 'metadata');
      must(!request.respond({action: 'nonsense'}) && !request.respond({action: 'continue'}, 'x'), 'invalid');
      must(!request.respond({action: 'redirect', url: 'file:///etc/passwd'}), 'scheme');
      must(request.respond({action: 'block'}) && !request.respond({action: 'continue'}) && request.info === null, 'once');
    } else if (mode === 'net-headers') {
      must(!request.respond({action: 'continue', headers: [{name: 'Trailer', value: 'x'}]}), 'forbidden');
      must(request.respond({action: 'continue', headers: [...m.headers, {name: 'X-Zebkit', value: 'yes'}]}), 'headers');
    } else if (mode === 'net-redirect') must(request.respond({action: 'redirect', url: base + '/echo?redirected'}), 'redirect');
    else if (mode === 'net-fulfill') {
      must(!request.respond({action: 'fulfill', status: 302}), 'status');
      const bytes = new Uint8Array(768); bytes.forEach((_, i) => { bytes[i] = i; });
      must(request.respond({action: 'fulfill', status: 201, headers: [{name: 'Content-Type', value: 'application/octet-stream'}, {name: 'X-Added', value: 'by-host'}]}, bytes), 'fulfill');
    } else if (mode === 'net-navigation') {
      must(m.navigation && m.mainFrame, 'navigation');
      must(request.respond({action: 'fulfill', headers: [{name: 'Content-Type', value: 'text/html'}]}, '<title>from-host</title>'), 'fulfill');
    } else if (mode === 'net-response') {
      must(m.stage === 'response' && m.status === 200, 'stage');
      must(request.respond({action: 'continue', headers: [...m.responseHeaders, {name: 'X-Added', value: 'late'}]}), 'continue');
    } else if (mode === 'net-response-block') must(request.respond({action: 'block', error: 'failed'}), 'block');
    else if (mode === 'net-close') view.close();
    else if (mode === 'net-server-redirect') { must(m.url.endsWith('/echo'), m.url); must(request.respond({action: 'block'}), 'block'); }
    else if (mode === 'net-worker') { must(m.worker, 'worker'); must(request.respond({action: 'fulfill', headers: [text]}, 'from-host'), 'fulfill'); }
    else if (mode === 'net-post') {
      must(m.method === 'POST' && m.bodySize === 6 && atob(m.body) === 'posted', 'body');
      must(request.respond({action: 'continue'}), 'continue');
    }
  });
  if (mode === 'net-timeout') view.setNetworkTimeout(50);
  if (mode === 'net-navigation') {
    await load(base + '/held');
    await until(() => view.title === 'from-host', 'answered');
    view.close();
    return;
  }
  const url = {'net-none': '/echo', 'net-observe': '/echo', 'net-response': '/echo', 'net-response-block': '/echo', 'net-filter': '/echo',
               'net-headers': '/echo', 'net-post': '/echo', 'net-server-redirect': '/server-redirect'}[mode] || '/held';
  let answer = null;
  const script = mode === 'net-worker'
      ? `new Promise(ok => { const w = new Worker('/net-worker'); w.onmessage = e => ok(e.data); })`
      : `fetch('${url}'${mode === 'net-post' ? `, {method: 'POST', body: 'posted'}` : ''}).then(async r => JSON.stringify({status: r.status, redirected: r.redirected, added: r.headers.get('x-added'), body: await r.text()}), e => 'failed')`;
  view.evaluate(script).then(value => { answer = value; }, () => { answer = 'closed'; });
  if (mode === 'net-close') { await until(() => closed, 'closed'); must(kept.info === null, 'stale'); return; }
  if (mode === 'net-pending-close') {
    await until(() => kept, 'asked');
    view.close();
    await until(() => closed, 'closed');
    must(kept.info === null && !kept.respond({action: 'continue'}), 'stale');
    return;
  }
  if (mode === 'net-replace') {
    await until(() => kept, 'asked');
    view.onNetwork(null);
    must(kept.info === null, 'stale');
    await until(() => events.some(e => e.reason === 'handler-changed'), 'replaced');
  }
  await until(() => answer !== null, 'fetch ended');
  if (mode === 'net-worker') { must(answer === 'from-host', answer); view.close(); return; }
  const r = answer === 'failed' ? null : JSON.parse(answer);
  if (mode === 'net-none' || mode === 'net-filter') must(r && r.status === 200 && !asked, 'passed');
  else if (['net-block', 'net-response-block', 'net-server-redirect'].includes(mode)) must(r === null && asked === 1, 'failed');
  else if (mode === 'net-headers') must(JSON.parse(r.body)['x-zebkit'] === 'yes', r.body);
  else if (mode === 'net-redirect') must(r.status === 200 && r.redirected, 'redirected');
  else if (mode === 'net-fulfill') must(r.status === 201 && r.added === 'by-host', answer);
  else if (mode === 'net-response') must(r.added === 'late', answer);
  else if (mode === 'net-observe') {
    must(!asked, 'held');
    await until(() => ['request', 'response', 'finished'].every(kind => events.some(e => e.kind === kind)), 'told');
  } else if (mode === 'net-timeout') {
    await until(() => events.some(e => e.reason === 'timeout'), 'timeout');
    must(kept.info === null && !kept.respond({action: 'block'}), 'late');
  } else if (mode === 'net-post') must(r && r.status === 200, answer);
  // net-replace: its checks are above, the request stale and the handler-changed event.
  else if (mode !== 'net-replace') must(false, 'a network mode with nothing to check');
  view.close();
}

const group = mode.split('-')[0];
await ({external, download, cert: certificate, world, file: chooser, media, keys, menu, fullscreen, net: network})[group]().catch(fail);
await sleep(50);
console.log(`ok javascript ${mode}`);
zebkit.quit();
