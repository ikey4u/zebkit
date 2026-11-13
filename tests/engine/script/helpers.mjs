// What the zebkit-js test scripts share. Modules load from files, relative to
// the importer: import {verify, finish} from './helpers.mjs'.
//
// The lines a script prints, as the Python tests' (tests/engine/harness.py):
//   ok   <what>              a check passed
//   FAIL <what>: <detail>    a check failed
//   skip <what>: <why>       a part not run here (counted, never a pass)
//   N skipped: ...           before the summary, when something was skipped
//   N of M checks passed     the summary
// and, from finish(), its own final "ok <sentence>" line. zebkit-js ends once
// nothing is pending, so a script whose promise never settles ends without
// either: scripts/engine-test.sh requires them (finished, finished_line).
export const results = [];
export const skipped = [];

function record(name, outcome) {
  results.push([name, outcome]);
  console.log(`${outcome === true ? 'ok   ' : 'FAIL '}${name}${outcome === true ? '' : ': ' + outcome}`);
}

// An asynchronous check: test() resolves to true, or the check fails with
// what it gave or threw.
export async function check(name, test) {
  try {
    const value = await test();
    record(name, value === true ? true : `unexpected: ${JSON.stringify(value)}`);
  } catch (e) {
    record(name, String(e && e.message || e));
  }
}

// A check of a value at hand; detail is shown when it fails. Returns ok.
export function verify(ok, what, detail) {
  record(what, ok ? true : detail === undefined ? 'false' : JSON.stringify(detail));
  return ok;
}

// A part not run on this machine: a "skip " line, never a pass.
export function skip(what, why) {
  skipped.push(what);
  console.log(`skip ${what}: ${why}`);
}

// The checks that failed so far.
export const failed = () => results.filter(([, outcome]) => outcome !== true);

// The summary line; then, when every check passed and there was one, "ok
// <sentence>"; otherwise it throws, so that zebkit-js ends with a failure.
export function finish(sentence) {
  const bad = failed();
  if (skipped.length) console.log(`${skipped.length} skipped: ${skipped.join('; ')}`);
  console.log(`${results.length - bad.length} of ${results.length} checks passed`);
  if (bad.length || !results.length) throw new Error(`${bad.length} of ${results.length} checks failed`);
  if (sentence) console.log(`ok ${sentence}`);
}

// How long a negative check waits for something that must not happen.
export const SETTLE_MS = 500;

export const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// promise, or a rejection naming what when it takes longer than ms.
export const within = (promise, ms, what) =>
  Promise.race([promise, sleep(ms).then(() => { throw new Error('timed out: ' + what); })]);

// A promise of the view's next finished load.
export const loaded = (view) => new Promise((resolve) => view.on('load', (event) => { if (event === 'finished') resolve(); }));

// Whether condition() became true within ms, asked every step ms.
export async function until(condition, ms = 10000, step = 10) {
  for (const end = Date.now() + ms; Date.now() < end; await sleep(step)) if (await condition()) return true;
  return !!(await condition());
}

// A page's user activation reaches the browser on a pipe of its own, after the
// evaluation that gave it; a gated call (fullscreen, pickers, device and
// display requests) that comes first is refused there. The script to evaluate
// for one: script run once it has.
export const ACTIVATION_DELAY_MS = 200;
export const activated = (script) => `new Promise(r => setTimeout(r, ${ACTIVATION_DELAY_MS})).then(() => ${script})`;

// Whether action() throws, and when kind is given, an instance of it.
export function throws(action, kind) {
  try { action(); } catch (error) { return !kind || error instanceof kind; }
  return false;
}
