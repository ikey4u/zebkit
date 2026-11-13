// zebkit-js: a bound function whose promise rejects, with a collection
// between the rejection and the reaction that answers the page
// (zebkit_call_reject()): the page gets the real error, not "the application
// dropped the call". Run with --js-flags=--expose-gc. Argument: the local
// server's base URL (tests/engine/pages).
import {finish, loaded, sleep, verify as check, within} from './helpers.mjs';

const [base] = zebkit.args;

check(typeof globalThis.gc === 'function', '--js-flags=--expose-gc gives the host script gc()');

const view = new zebkit.View('', {width: 200, height: 100});
view.grant(base, ['app']);
// Rejected from a timer, so that no handle of the call is on a stack; the
// job queued right after the rejection runs before the job that
// answers the page, and collects there.
view.bind('fails', () => new Promise((_, reject) => setTimeout(() => {
  reject(new Error('the real error'));
  Promise.resolve().then(() => { globalThis.gc(); globalThis.gc(); });
}, 10)));
view.bind('works', () => new Promise((resolve) => setTimeout(() => {
  resolve(42);
  Promise.resolve().then(() => globalThis.gc());
}, 10)));
const ready = loaded(view);
view.load(`${base}/bridge.html`);
await within(ready, 15000, 'bridge.html');

for (let round = 0; round < 5; ++round) {
  await view.evaluate("window.answer = null; zebkit.app.fails().then(() => { answer = 'resolved'; }, " +
                      "(e) => { answer = e.message; }); 1");
  let message = null;
  for (const end = Date.now() + 10000; Date.now() < end && !message; await sleep(20))
    message = await view.evaluate('answer');
  check(typeof message === 'string' && message.includes('the real error'),
        `round ${round}: a rejection with a collection before its reaction gives the page the real error`, message);
  await view.evaluate("window.value = null; zebkit.app.works().then((v) => { value = v; }, " +
                      "(e) => { value = e.message; }); 1");
  let value = null;
  for (const end = Date.now() + 10000; Date.now() < end && value === null; await sleep(20))
    value = await view.evaluate('value');
  check(value === 42, `round ${round}: a resolution with a collection after it gives the page the value`, value);
}
view.close();

finish('a bound call rejected with a collection before its reaction');
