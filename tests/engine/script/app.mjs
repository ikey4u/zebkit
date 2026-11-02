// An application in JavaScript, run by zebkit-js (scripts/engine-test.sh):
//
//   zebkit-js --headless --no-sandbox app.mjs <base URL> <empty directory>
//
// Throws, and so fails, when a check fails.
import {check, results, sleep} from './helpers.mjs';

const [base, directory] = zebkit.args;
const text = (bytes) => new TextDecoder().decode(bytes);
const hex = (bytes) => [...bytes].map((b) => b.toString(16).padStart(2, '0')).join('');

await check('zebkit.version and engineVersion', () =>
    typeof zebkit.version === 'string' && zebkit.engineVersion.startsWith('Chromium '));
await check('timers', async () => {
  const order = [];
  setTimeout(() => order.push('b'), 20);
  setTimeout(() => order.push('a'), 5);
  const id = setTimeout(() => order.push('never'), 10);
  clearTimeout(id);
  let ticks = 0;
  const interval = setInterval(() => { if (++ticks === 3) clearInterval(interval); }, 1);
  await sleep(60);
  return order.join() === 'a,b' && ticks === 3;
});
await check('TextEncoder and TextDecoder', () =>
    text(new TextEncoder().encode('héllo, 世界 🌍')) === 'héllo, 世界 🌍' &&
    new TextEncoder().encode('é').length === 2);
await check('compress: bytes are Uint8Arrays', async () => {
  const packed = await zebkit.compress('zstd', 'script '.repeat(100), 19);
  return packed instanceof Uint8Array && packed.length < 100 &&
      text(await zebkit.decompress('zstd', packed)) === 'script '.repeat(100);
});
await check('crypto: digest', async () =>
    hex(await zebkit.crypto.digest('SHA-256', 'abc')).startsWith('ba7816bf'));
await check('url: parse', async () =>
    (await zebkit.url.parse('../x', 'https://a.example/b/c')).href === 'https://a.example/x');
await check('image: encode and decode', async () => {
  const data = new Uint8Array(2 * 2 * 4).fill(100);
  const back = await zebkit.image.decode(await zebkit.image.encode('png', {width: 2, height: 2, data}));
  return back.width === 2 && back.data instanceof Uint8Array && back.data[3] === 100;
});
await check('fs: any path, for the application', async () => {
  const file = `${directory}/from-script.txt`;
  await zebkit.fs.writeFile(file, new TextEncoder().encode('bytes'));
  return await zebkit.fs.readText(file) === 'bytes' && (await zebkit.fs.stat(file)).size === 5;
});
await check('fetch', async () => {
  const response = await zebkit.fetch(`${base}/hello.txt`);
  return response.status === 200 && text(response.body) === 'hello\n';
});
await check('fetch: an HTTP error still returns its response', async () => {
  const response = await zebkit.fetch(`${base}/__zebkit_missing_file__`);
  return response.status === 404 && response.body.length > 0;
});
await check('fetch: a body above the 5 MiB string-download limit', async () => {
  const response = await zebkit.fetch(`${base}/__zebkit_large_body__`);
  return response.status === 200 && response.body.length === 6 * 1024 * 1024;
});
await check('fetch: the 256 MiB body limit rejects a larger response', async () => {
  try {
    await zebkit.fetch(`${base}/__zebkit_oversize_body__`);
    return false;
  } catch (error) {
    return /ERR_INSUFFICIENT_RESOURCES/.test(error.message);
  }
});
await check('a thrown error is a rejection', async () => {
  try { await zebkit.compress('nonsense', 'x'); return false; } catch (e) { return /unknown format/.test(e.message); }
});

// zebkit.pdf: a document made here, read, drawn, searched and changed.
const PDF = [
  '%PDF-1.7',
  '1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj',
  '2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj',
  '3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Contents 4 0 R' +
      ' /Resources << /Font << /F1 5 0 R >> >> >> endobj',
  '4 0 obj << >> stream', 'BT /F1 18 Tf 10 40 Td (Hello pdf) Tj ET', 'endstream endobj',
  '5 0 obj << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> endobj',
  'trailer << /Root 1 0 R >>', '%%EOF', ''].join('\n');
await check('pdf: open, info, render, search, annotate, save', async () => {
  const doc = await zebkit.pdf.open(new TextEncoder().encode(PDF));
  const info = await doc.info();
  const image = await doc.render(0, {scale: 2});
  const found = await doc.search('pdf');
  const index = await doc.addAnnotation(0, {type: 'highlight', rect: [8, 35, 100, 60], color: [255, 255, 0],
                                            quads: [[8, 60, 100, 60, 8, 35, 100, 35]]});
  const file = `${directory}/saved.pdf`;
  await zebkit.fs.writeFile(file, await doc.save());
  await doc.close();
  const again = await zebkit.pdf.open(file);
  const annotations = await again.annotations(0);
  const text = await again.text(0);
  await again.close();
  return info.pages === 1 && image.width === 400 && image.height === 200 &&
      image.data instanceof Uint8Array && image.data.length === 400 * 200 * 4 &&
      found.length === 1 && found[0].count === 3 && index === 0 &&
      annotations[0].type === 'highlight' && text === 'Hello pdf';
});
await check('pdf: edited outline persists and invalid replacement is atomic', async () => {
  const doc = await zebkit.pdf.open(new TextEncoder().encode(PDF));
  const tree = [{title: '第一章 π', page: 0, x: 12.5, y: 70, zoom: 1, open: true,
                 children: [{title: 'Child', url: 'https://example.com', open: false}]}];
  await doc.setOutline(tree);
  let rejected = false;
  try { await doc.setOutline([{title: 'Wrong', page: 10}]); } catch { rejected = true; }
  const current = await doc.outline();
  const again = await zebkit.pdf.open(await doc.save());
  const saved = await again.outline();
  await doc.setOutline([]);
  const empty = await doc.outline();
  await Promise.all([doc.close(), again.close()]);
  return rejected && JSON.stringify(current) === JSON.stringify(saved) &&
      current[0].title === tree[0].title && current[0].children[0].url === tree[0].children[0].url && empty.length === 0;
});
await check('pdf: pages copied, inserted, moved, turned and deleted', async () => {
  const source = await zebkit.pdf.open(new TextEncoder().encode(PDF));
  const built = await zebkit.pdf.create();
  await built.importPages(source, null, 0);
  await built.importPages(source, [0], 1);
  await built.insertPage(0, 100, 50);
  await built.movePages([0], 2);
  await built.rotatePage(2, 90);
  await built.deletePages([1]);
  const reopened = await zebkit.pdf.open(await built.save());
  const pages = (await reopened.info()).pages;
  const turned = await reopened.page(1);
  await Promise.all([source.close(), built.close(), reopened.close()]);
  return pages === 2 && turned.rotation === 90 && turned.width === 50;
});
await check('pdf: a closed document is gone', async () => {
  const doc = await zebkit.pdf.open(new TextEncoder().encode(PDF));
  await doc.close();
  try { await doc.info(); return false; } catch (e) { return /no such document/.test(e.message); }
});

// A view, its page, and a function the page calls.
await check('a view, and the page calling the script', () => new Promise((resolve, reject) => {
  const view = new zebkit.View(`${base}/bridge.html`, {width: 640, height: 480});
  view.grant(base, ['app', 'crypto']);
  view.bind('add', function (a, b) { return this.origin === base ? a + b : 'wrong origin'; });
  view.bind('later', async (x) => { await sleep(10); return `${x}!`; });
  view.bind('fail', () => { throw new Error('no'); });
  let closed = false;
  view.on('close', () => { closed = true; });
  view.on('load', async (event) => {
    if (event !== 'finished') return;
    try {
      const sum = await view.evaluate('zebkit.app.add(2, 3)');
      const later = await view.evaluate('zebkit.app.later("x")');
      const failed = await view.evaluate('zebkit.app.fail().then(() => "resolved", (e) => e.message)');
      const title = view.title;
      view.close();
      await sleep(50);
      resolve(sum === 5 && later === 'x!' && /no/.test(failed) && title === 'bridge' && closed);
    } catch (e) {
      reject(e);
    }
  });
}));

await check('pdf in a page granted it: bytes, not paths', () => new Promise((resolve, reject) => {
  const view = new zebkit.View(`${base}/bridge.html`, {width: 640, height: 480});
  view.grant(base, ['pdf']);
  view.on('load', async (event) => {
    if (event !== 'finished') return;
    try {
      const result = await view.evaluate(`(async () => {
        const pdf = ${JSON.stringify(PDF)};
        const doc = await zebkit.pdf.open(new TextEncoder().encode(pdf));
        const page = await doc.page(0);
        const image = await doc.render(0, {width: 50, height: 20, x: 0, y: 0});
        const text = await doc.text(0);
        let path;
        try { await zebkit.pdf.open('/etc/hostname'); path = 'opened'; } catch (e) { path = e.message; }
        return {width: page.width, image: [image.width, image.height, image.data.length], text, path};
      })()`);
      // Bytes that are not a PDF are refused.
      const other = await view.evaluate(`zebkit.pdf.open(new Uint8Array([1])).then(() => 'opened', (e) => e.message)`);
      view.close();
      resolve(result.width === 200 && result.image.join() === '50,20,4000' && result.text === 'Hello pdf' &&
              /bytes/.test(result.path) && /not a PDF/.test(other));
    } catch (e) {
      reject(e);
    }
  });
}));

await check("a page cannot remove the application's PDF hooks", () => new Promise((resolve, reject) => {
  const view = new zebkit.View(`${base}/bridge.html`, {width: 640, height: 480});
  // What a page's own script could do if it got there first: keep the
  // channel zebkit.* uses, and call over it with any arguments.
  view.addWorldScript('globalThis.__raw = globalThis.__zebkit_call;', {mainFrameOnly: true});
  view.grant(base, ['pdf']);
  view.on('load', async (event) => {
    if (event !== 'finished') return;
    try {
      const own = await zebkit.pdf.create();
      await own.insertPage(0, 100, 100);
      await own.use({kinds: {App_Kept: {prefix: 'App_'}}});
      // Every id there can be.
      const removed = await view.evaluate(`(async () => {
        if (typeof __zebkit_call !== 'undefined') return -1;
        const call = async (method, args) => {
          const [ok, text] = await __raw('pdf', method, JSON.stringify(args), []);
          return ok ? JSON.parse(text) : null;
        };
        const doc = await call('create', []);
        let removed = 0;
        for (let id = 1; id < 256; ++id)
          if (await call('removeHook', [doc, id])) ++removed;
        return removed;
      })()`);
      const added = await own.addAnnotation(0, {type: 'App_Kept', rect: [1, 1, 9, 9], data: {}});
      await own.close();
      view.close();
      resolve(removed === 0 && added === 0);
    } catch (e) {
      reject(e);
    }
  });
}));

await check('canvas, text and a PDF plugin in a page: by its grants', () => new Promise((resolve, reject) => {
  const view = new zebkit.View(`${base}/bridge.html`, {width: 640, height: 480});
  view.grant(base, ['pdf', 'image']);
  view.on('load', async (event) => {
    if (event !== 'finished') return;
    try {
      const result = await view.evaluate(`(async () => {
        const canvas = zebkit.canvas.create(10, 10).fillColor(0xff0000ff).rect(0, 0, 10, 10).fill();
        const {data} = await canvas.pixels();
        const doc = await zebkit.pdf.open(new TextEncoder().encode(${JSON.stringify(PDF)}));
        await doc.use({
          kinds: {App_Mark: {prefix: 'App_', draw: (c, a, {width, height}) => { c.fillColor(0x00ff00ff).rect(0, 0, width, height).fill(); }}},
          before: {'annotation.remove': () => ({refuse: 'kept'})},
        });
        const at = await doc.addAnnotation(0, {type: 'App_Mark', rect: [0, 0, 20, 20], data: {a: 1}});
        const image = await doc.render(0);
        const pixel = image.data.subarray((image.height - 10) * image.width * 4 + 40, (image.height - 10) * image.width * 4 + 43);
        const refused = await doc.removeAnnotation(0, at).then(() => '', (e) => e.message);
        return {red: data[0], green: pixel[1], blue: pixel[0], refused};
      })()`);
      view.close();
      resolve(result.red === 255 && result.green === 255 && result.blue === 0 && result.refused === 'kept');
    } catch (e) {
      reject(e);
    }
  });
}));

await check('canvas in a page not granted it: refused', () => new Promise((resolve, reject) => {
  const view = new zebkit.View(`${base}/bridge.html`, {width: 640, height: 480});
  view.grant(base, ['pdf']);
  view.on('load', async (event) => {
    if (event !== 'finished') return;
    try {
      const answer = await view.evaluate(`zebkit.canvas.create(1, 1).pixels().then(() => 'drawn', (e) => e.message)`);
      view.close();
      resolve(answer !== 'drawn');
    } catch (e) {
      reject(e);
    }
  });
}));

const failed = results.filter(([, outcome]) => outcome !== true);
console.log(`${results.length - failed.length} of ${results.length} checks passed`);
if (failed.length) throw new Error(`${failed.length} checks failed`);
// Nothing is pending and no view is open: zebkit-js ends by itself.
