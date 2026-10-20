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
await check('a thrown error is a rejection', async () => {
  try { await zebkit.compress('nonsense', 'x'); return false; } catch (e) { return /unknown format/.test(e.message); }
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

const failed = results.filter(([, outcome]) => outcome !== true);
console.log(`${results.length - failed.length} of ${results.length} checks passed`);
if (failed.length) throw new Error(`${failed.length} checks failed`);
// Nothing is pending and no view is open: zebkit-js ends by itself.
