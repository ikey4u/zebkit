// zebkit-js: a page's process going (view.onProcessGone()), a page that stops
// answering input and answers again (view.onResponsiveness()), an off-screen
// view that cannot be made (View.offscreen() throws the engine's reason), and
// a bound call whose result never settles, dropped once it is collected
// (zebkit_call_free()). Run with --enable-features=RendererHangWatcher:delay/2s
// and --js-flags=--expose-gc. Argument: the local server's base URL
// (tests/engine/pages).
import {finish, loaded, sleep, throws, until, verify as check, within} from './helpers.mjs';

const [base] = zebkit.args;

// The process: chrome://crash is told "crashed"; the view stays and loads again.
{
  const view = new zebkit.View('', {width: 200, height: 100});
  const gone = [];
  view.onProcessGone((event) => gone.push(event));
  const first = loaded(view);
  view.load('data:text/html,<title>first</title>');
  await within(first, 15000, 'the first page');
  view.load('chrome://crash');
  check(await until(() => gone.length > 0, 15000), 'onProcessGone() is told the page\'s process went', gone);
  check(gone[0] && gone[0].reason === 'crashed' && typeof gone[0].exitCode === 'number',
        'its reason is "crashed", with an exit code', gone[0]);
  const again = loaded(view);
  view.load('data:text/html,<title>again</title>');
  await within(again, 15000, 'the page after the crash');
  check(view.onProcessGone(null) === true, 'onProcessGone(null) removes the function');
  check(throws(() => view.onProcessGone(42), TypeError), 'onProcessGone() takes a function or null');
  view.close();
}

// Responsiveness: a page whose main thread is busy for 6 s with keys waiting
// on it, after an evaluation (the view's own DevTools session does not hold
// the hang monitor). Told false once, then true once.
{
  const view = zebkit.View.offscreen('about:blank', {width: 200, height: 100});
  const told = [];
  view.onResponsiveness((event) => told.push(event.responsive));
  const ready = loaded(view);
  view.load('data:text/html,<title>busy</title><script>addEventListener("keydown", () => {})</script>');
  await within(ready, 15000, 'the busy page');
  check(await view.evaluate('1 + 1') === 2, 'the view evaluated first');
  await view.evaluate('setTimeout(() => { const end = Date.now() + 6000; while (Date.now() < end); }, 100); 1');
  await sleep(300);
  for (const end = Date.now() + 8000; Date.now() < end && told.length < 2; await sleep(250)) {
    view.sendKey({type: 'down', keyCode: 0x41, code: 'KeyA', key: 'a'});
    view.sendKey({type: 'up', keyCode: 0x41, code: 'KeyA', key: 'a'});
  }
  await until(() => told.length >= 2, 3000);
  await sleep(500);
  check(JSON.stringify(told) === '[false,true]', 'onResponsiveness() is told false, then true once it answers', told);
  view.close();
}

// View.offscreen() throws where it cannot make the view.
{
  const context = new zebkit.Context();
  context.close();
  let error = null;
  try { zebkit.View.offscreen('about:blank', {width: 10, height: 10, context}); } catch (e) { error = e; }
  check(error instanceof Error && /closed/.test(error.message), 'View.offscreen() throws for a closed context',
        String(error));
  const made = zebkit.View.offscreen('about:blank', {width: 10, height: 10});
  check(made instanceof zebkit.View, 'View.offscreen() makes a view where it can');
  made.close();
}

// A bound function whose promise never settles: once collected, the page's
// promise is rejected (zebkit_call_free()), not left waiting.
{
  const view = new zebkit.View('', {width: 200, height: 100});
  view.grant(base, ['app']);
  view.bind('never', () => new Promise(() => {}));
  const ready = loaded(view);
  view.load(`${base}/bridge.html`);
  await within(ready, 15000, 'bridge.html');
  await view.evaluate("window.dropped = null; zebkit.app.never().then(() => { dropped = 'resolved'; }, " +
                "(e) => { dropped = e.message; }); 1");
  // --js-flags are the host's V8's too: gc() collects the result's chain.
  check(typeof globalThis.gc === 'function', '--js-flags=--expose-gc gives the host script gc()');
  let message = null;
  for (const end = Date.now() + 10000; Date.now() < end && !message; await sleep(50)) {
    if (typeof globalThis.gc === 'function') globalThis.gc();
    message = await view.evaluate('dropped');
  }
  check(message === 'the application dropped the call',
        'a bound call whose result is collected unsettled is rejected in the page', message);
  view.close();
}

finish('a page\'s process and responsiveness, off-screen errors and dropped calls');
