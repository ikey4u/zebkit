// zebkit-js and an error nothing caught after the module ran: printed, the
// script runs on to its end, and zebkit-js exits 1. Run by
// tests/engine/script_uncaught.py with one argument, the case: "throw" (from a
// timer), "reject" (a promise with no handler), "hook" (a view's event
// handler that throws) or "none" (exit 0).
const [mode] = zebkit.args;

if (mode === 'throw') setTimeout(() => { throw new Error('thrown from a timer'); }, 10);
if (mode === 'reject') setTimeout(() => { Promise.reject(new Error('rejected with no handler')); }, 10);
if (mode === 'hook') {
  const view = new zebkit.View('', {width: 100, height: 100});
  view.on('load', (event) => {
    if (event !== 'finished') return;
    setTimeout(() => view.close(), 300);
    throw new Error('thrown from a load handler');
  });
  view.load('data:text/html,<title>x</title>');
}
// After the error: the script still runs.
setTimeout(() => console.log('ran on after it'), 200);
