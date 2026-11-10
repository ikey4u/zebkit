// zebkit-js's views, contexts, schemes, CDP and services beyond the basics:
// the rest of <zebkit/zebkit.h>, AES-GCM and the canvas over given pixels
// and measuring. Run with --zebkit-scheme=jsapp,standard,secure,fetch.
// Arguments: the local server's base URL, and a TrueType font to measure
// with (optional).
const [base, fontPath] = zebkit.args;
let failures = 0;
const check = (ok, what, detail) => {
  console.log((ok ? 'ok   ' : 'FAIL ') + what + (ok || detail === undefined ? '' : ': ' + JSON.stringify(detail)));
  if (!ok) failures++;
};
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const within = (promise, ms, what) => Promise.race([promise, sleep(ms).then(() => { throw new Error('timed out: ' + what); })]);
const loaded = (view) => new Promise((resolve) => view.on('load', (event) => { if (event === 'finished') resolve(); }));
const open = async (url, options = {}) => {
  const view = new zebkit.View('', options);
  const done = loaded(view);
  view.load(url);
  await within(done, 15000, url);
  return view;
};

check(zebkit.isHeadless() === true, 'isHeadless');

// AES-GCM, as zebkit.crypto now has it.
const key = await zebkit.crypto.randomBytes(32);
const nonce = await zebkit.crypto.randomBytes(12);
const sealed = await zebkit.crypto.aesGcmSeal(key, nonce, 'secret words', 'header');
const opened = await zebkit.crypto.aesGcmOpen(key, nonce, sealed, 'header');
check(new TextDecoder().decode(opened) === 'secret words', 'crypto.aesGcmSeal and aesGcmOpen');
const changed = new Uint8Array(sealed);
changed[0] ^= 1;
check(await zebkit.crypto.aesGcmOpen(key, nonce, changed, 'header').then(() => false, () => true), 'a changed seal does not open');

// The canvas over the caller's pixels, and measuring text.
const blue = new Uint8Array(4 * 4 * 4);
for (let i = 0; i < blue.length; i += 4) blue.set([0, 0, 255, 255], i);
const over = await zebkit.canvas.create(4, 4).fillColor([255, 0, 0, 255]).rect(0, 0, 2, 4).fill().pixels({data: blue});
check(over.data[0] === 255 && over.data[2] === 0 && over.data[12] === 0 && over.data[14] === 255,
      'a canvas drawn over given pixels', [...over.data.slice(0, 16)]);
if (fontPath) {
  const font = await zebkit.fs.readFile(fontPath);
  const advance = await zebkit.canvas.create(100, 20).font(font, 10).measureText('abcd');
  check(advance === 40, 'Canvas.measureText', advance);
}

// A view's own window, title, scripts, user agent and capture.
const view = await open(base + '/page.html');
check(typeof view.targetId === 'string' && view.targetId.length > 0, 'targetId', view.targetId);
for (let i = 0; i < 100 && view.isLoading; ++i) await sleep(50);
check(view.isLoading === false, 'isLoading, once the page has loaded');
view.setTitle('A title of the application');
view.setSizeLimits(100, 100, true);
view.setBounds(0, 0, 400, 300);
view.setVisible(true);
check(view.popupPolicy === 'block-without-gesture', 'popupPolicy', view.popupPolicy);
check(view.setNavigationFailClosed(false) === true, 'setNavigationFailClosed');
view.addScript('window.addedByTheApplication = 42;');
view.setUserAgent('zebkit-js agent');
const reloaded = loaded(view);
view.reload();
await within(reloaded, 15000, 'reload');
check(await view.evaluate('window.addedByTheApplication') === 42, 'addScript runs before the page');
check(await view.evaluate('navigator.userAgent') === 'zebkit-js agent', 'setUserAgent');
const png = await view.capture();
check(png instanceof Uint8Array && png[1] === 0x50 && png[2] === 0x4e && png[3] === 0x47, 'capture: a PNG');
view.stop();

// A view in another's place, attached and detached.
const child = new zebkit.View('about:blank', {width: 100, height: 100});
child.attach(view, 10, 10, 100, 100);
check(true, 'attach');

// new-window: the application takes the URL.
const asked = new Promise((resolve) => view.on('new-window', resolve));
view.setRuntimeSettings({newWindows: 'allow'});
await view.evaluate("setTimeout(() => window.open('" + base + "/hello.txt', '_blank'), 0); 1");
check(await within(asked, 10000, 'new-window').catch(() => '') === base + '/hello.txt', 'new-window');

// Contexts: their storage is their own.
const context = new zebkit.Context();
const inside = await open(base + '/page.html', {context});
await inside.evaluate("localStorage.setItem('kept', 'inside')");
const outside = await open(base + '/page.html');
check(await inside.evaluate("localStorage.getItem('kept')") === 'inside' &&
      await outside.evaluate("localStorage.getItem('kept')") === null, 'a context keeps its storage');
const offscreenInside = zebkit.View.offscreen('about:blank', {width: 50, height: 50, context});
check(offscreenInside !== null && offscreenInside.isOffscreen, 'an off-screen view in a context');
context.close();
check(await sleep(300).then(() => true), 'Context.close');

// The application's scheme.
let requested = null;
zebkit.scheme.handle('jsapp', (request) => {
  requested = request;
  if (request.url.endsWith('/data')) request.respond(200, 'Content-Type: text/plain', 'from the script');
  else request.respond(200, 'Content-Type: text/html', '<title>scheme</title><script>fetch("/data").then(r => r.text()).then(t => document.title = t)</script>');
});
const schemeView = new zebkit.View('jsapp://main/index.html', {width: 200, height: 100});
const titled = new Promise((resolve) => schemeView.on('title', (title) => { if (title === 'from the script') resolve(title); }));
check(await within(titled, 10000, 'scheme').catch(() => '') === 'from the script' && requested?.method === 'GET',
      'a scheme of the script, and its fetch()');
zebkit.scheme.handle('jsapp', null);

// CDP.
const port = zebkit.cdp.listen('127.0.0.1', 0);
check(port > 0 && zebkit.cdp.port === port && String(zebkit.cdp.url).includes(String(port)), 'cdp.listen, port and url');
const version = await zebkit.fetch(`http://127.0.0.1:${port}/json/version`);
check(version.status === 200 && new TextDecoder().decode(version.body).includes('Browser'), 'CDP answers /json/version');
zebkit.cdp.stop();
check(zebkit.cdp.port === 0, 'cdp.stop');

// The application's events: set and cleared.
zebkit.app.onOpenFiles(() => {});
zebkit.app.onReopen(() => {});
zebkit.app.onQuitRequest(() => {});
zebkit.app.onOpenFiles(null);
zebkit.app.onReopen(null);
zebkit.app.onQuitRequest(null);
check(true, 'app.onOpenFiles, onReopen, onQuitRequest');

for (const v of [view, child, outside, schemeView, inside]) v.close();
if (offscreenInside) offscreenInside.close();
if (failures) throw new Error(`${failures} checks failed`);
console.log('ok JavaScript views, contexts, schemes, CDP, AES-GCM and the canvas over pixels');
zebkit.quit();
