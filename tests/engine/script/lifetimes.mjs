// zebkit-js: what the review of hooks and the script runtime fixed. A bound
// function that answers undefined, a view's 'close' heard by each listener, a
// scheme handler that throws, and PDF calls on one document kept in order.
// Run with --zebkit-scheme=jsfail,standard,secure,fetch. Argument: the local
// server's base URL (tests/engine/pages).
import {finish, loaded, sleep, until, verify as check, within} from './helpers.mjs';

const [base] = zebkit.args;

// zebkit_call_resolve(call, NULL) is undefined in the page, not null.
const view = new zebkit.View('', {width: 320, height: 240});
view.grant(base, ['app']);
view.bind('nothing', () => undefined);
view.bind('nil', () => null);
const done = loaded(view);
view.load(`${base}/bridge.html`);
await within(done, 15000, 'bridge.html');
check(await view.evaluate("zebkit.app.nothing().then(v => v === undefined ? 'undefined' : String(v))") === 'undefined',
      'a bound function that returns undefined resolves with undefined');
check(await view.evaluate("zebkit.app.nil().then(v => v === null ? 'null' : String(v))") === 'null',
      'a bound function that returns null resolves with null');

// Every 'close' listener is told, and the view's own bookkeeping with them.
const heard = [];
view.on('close', () => heard.push('a'));
view.on('close', () => heard.push('b'));
view.close();
check(await until(() => heard.length === 2), "each 'close' listener hears the view close", heard);

// A scheme handler that throws answers 500; the page does not wait for ever.
zebkit.scheme.handle('jsfail', (request) => {
  if (request.url.endsWith('/bad')) throw new Error('the handler failed on purpose');
  if (request.url.endsWith('/later')) return Promise.reject(new Error('the handler rejected on purpose'));
  request.respond(200, 'Content-Type: text/html', '<title>scheme</title>');
});
const schemeView = new zebkit.View('', {width: 200, height: 100});
const schemeLoaded = loaded(schemeView);
schemeView.load('jsfail://main/index.html');
await within(schemeLoaded, 15000, 'jsfail://main/index.html');
check(await schemeView.evaluate("fetch('/bad').then(r => r.status, () => 'failed')") === 500,
      'a scheme handler that throws answers 500');
check(await schemeView.evaluate("fetch('/later').then(r => r.status, () => 'failed')") === 500,
      'a scheme handler that rejects answers 500');
schemeView.close();
zebkit.scheme.handle('jsfail', null);

// A change to a document is not overtaken by a later read of it: page() is
// asked for sooner than rotatePage(), yet sees the page turned.
const doc = await zebkit.pdf.create();
await doc.insertPage(0, 100, 50);
const turned = doc.rotatePage(0, 90);
const page = doc.page(0);
await turned;
check((await page).rotation === 90, 'a read after a change sees the change', await page);
const saved = doc.save();
const removed = doc.deletePages([0]);
const bytes = await saved;
await removed;
const reopened = await zebkit.pdf.open(bytes);
check((await reopened.info()).pages === 1, 'a save before a change does not have the change');
await Promise.all([doc.close(), reopened.close()]);

// A view closed by its context's close(): evaluate() settles, whether asked
// before the close has landed or after, and never waits for ever.
const context = new zebkit.Context();
const inContext = new zebkit.View('', {width: 200, height: 100, context});
const contextLoaded = loaded(inContext);
inContext.load(`${base}/bridge.html`);
await within(contextLoaded, 15000, 'bridge.html in a context');
const contextClosed = new Promise((resolve) => inContext.on('close', resolve));
const settled = (promise) => promise.then(() => 'resolved', () => 'rejected');
const pending = inContext.evaluate('new Promise(() => {})');
context.close();
const early = inContext.evaluate('1 + 1');
check(await within(settled(pending), 15000, 'a pending evaluation of a closed view') === 'rejected',
      "an evaluation pending as the context closes rejects");
check(await within(settled(early), 15000, 'an evaluation as the context closes') === 'rejected',
      "an evaluation asked as the context closes rejects");
await within(contextClosed, 15000, "the context's view closes");
check(await within(settled(inContext.evaluate('1 + 1')), 15000, 'an evaluation of a closed view') === 'rejected',
      'an evaluation of a closed view rejects');
await sleep(500);
check(await within(settled(inContext.evaluate('1 + 1')), 15000, 'an evaluation of a view long closed') === 'rejected',
      'an evaluation of a view long closed rejects too');

await sleep(50);
finish('JavaScript lifetimes and orders');
