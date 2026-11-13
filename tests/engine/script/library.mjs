// zebkit.Library: <zebkit/library.h> from host JavaScript (tests/engine/library.py
// has the C API's every case; this is the binding's). Uint8Arrays in place,
// a promise per call, library.cancel(promise), each code.
//
//   zebkit-js library.mjs <page server base URL> <tests/engine dir>
import {check, finish, verify, sleep} from './helpers.mjs';

const base = zebkit.args[0];
const here = zebkit.args[1];  // tests/engine
const fixture = `${here}/library_fixture`;

const codeOf = (promise) => promise.then(() => 'OK', (error) => error.code);

const lib = zebkit.Library.load(fixture);
await check('ready: the exported functions', async () => {
  const {functions} = await lib.ready;
  return ['echo', 'double', 'spin', 'get'].every((name) => functions.includes(name)) || functions;
});
await check('JSON and Uint8Arrays echoed, in place', async () => {
  const value = await lib.call('echo', {a: [1, null, true], b: new Uint8Array([1, 2, 255])}, 'x');
  return JSON.stringify(value[0].a) === '[1,null,true]' && value[0].b instanceof Uint8Array &&
      value[0].b.join() === '1,2,255' && value[1] === 'x' || value;
});
await check('a relative import inside the folder', async () => (await lib.call('double', 21)) === 42);
await check('non-ASCII text', async () => {
  const s = '日本語 · Ελληνικά · 🦓';
  const value = await lib.call('text', s);
  return value.same === s && value.length === s.length || value;
});
await check('parts from the library, and a 1 MiB part there and back', async () => {
  const {b} = await lib.call('bytes', 300);
  const big = new Uint8Array(1 << 20).map((_, i) => i * 7);
  const [size] = await lib.call('sizes', big);
  return b instanceof Uint8Array && b.length === 300 && b[299] === 43 && size === big.length || [b.length, size];
});
await check('THROWN: name, message and stack of the library', async () => {
  try {
    await lib.call('thrower');
    return 'resolved';
  } catch (error) {
    return error.code === 'THROWN' && error.name === 'RangeError' && error.message === 'too far' &&
        error.stack.includes('thrower') || [error.code, error.name, error.message];
  }
});
await check('THROWN for a rejected promise', async () => (await codeOf(lib.call('rejecter'))) === 'THROWN');
await check('NO_FUNCTION', async () => (await codeOf(lib.call('missing'))) === 'NO_FUNCTION');
await check('BAD_RESULT', async () => (await codeOf(lib.call('badResult'))) === 'BAD_RESULT');
await check('cancel a waiting call: CANCELLED, it never runs', async () => {
  const first = lib.call('later', 1, 200);
  const queued = lib.call('double', 1);
  lib.cancel(queued);
  return (await codeOf(queued)) === 'CANCELLED' && (await first) === 1;
});
await check('a waiting call past its deadline: TIMEOUT, the next one runs', async () => {
  const slow = lib.callWith('later', [1, 2000], {timeoutMs: 100});
  const next = lib.call('double', 2);
  return (await codeOf(slow)) === 'TIMEOUT' && (await next) === 4;
});
await check('fetch() refused without a grant', async () => {
  try {
    await lib.call('get', `${base}/hello.txt`);
    return 'fetched';
  } catch (error) {
    return error.code === 'THROWN' && error.name === 'TypeError' || [error.code, error.message];
  }
});
await check('fetch() to a granted origin', async () => {
  const net = zebkit.Library.load(fixture, {network: [base.replace(/\/+$/, '')]});
  const value = await net.call('get', `${base}/hello.txt`);
  net.close();
  return value.status === 200 && value.body.startsWith('hello') || value;
});
await check('QUEUE_FULL', async () => {
  const small = zebkit.Library.load(fixture, {queue: 1});
  await small.ready;
  // The first is running (taken from the queue) before the others come.
  const first = small.call('later', 0, 300);
  await sleep(100);
  const codes = await Promise.all([first, small.call('later', 1, 10), small.call('later', 2, 10)].map(codeOf));
  small.close();
  return codes.join() === 'OK,OK,QUEUE_FULL' || codes;
});
await check('a running script past its deadline: TIMEOUT, then GONE', async () => {
  const doomed = zebkit.Library.load(fixture);
  const spinning = doomed.callWith('spin', [], {timeoutMs: 200});
  const after = doomed.call('double', 1);
  const codes = [await codeOf(spinning), await codeOf(after), await codeOf(doomed.call('double', 1))];
  doomed.close();
  return codes.join() === 'TIMEOUT,GONE,GONE' || codes;
});
await check('heap exhaustion: OUT_OF_MEMORY', async () => {
  const hungry = zebkit.Library.load(fixture, {heapLimitMB: 32});
  const code = await codeOf(hungry.callWith('grow', [], {timeoutMs: -1}));
  hungry.close();
  return code === 'OUT_OF_MEMORY' || code;
});
await check('an import outside the folder: LOAD_FAILED', async () => {
  const bad = zebkit.Library.load(`${here}/library_forbidden`);
  const codes = [await codeOf(bad.ready), await codeOf(bad.call('echo'))];
  bad.close();
  return codes.join() === 'LOAD_FAILED,LOAD_FAILED' || codes;
});
await check('close with calls pending: GONE; a call after close is rejected', async () => {
  const closing = zebkit.Library.load(fixture);
  await closing.ready;
  const pending = [closing.call('later', 1, 1000), closing.call('double', 1)];
  await sleep(50);
  closing.close();
  const codes = await Promise.all(pending.map(codeOf));
  return codes.join() === 'GONE,GONE' && (await codeOf(closing.call('double', 1))) === 'GONE' || codes;
});
let threw = false;
try {
  zebkit.Library.load(`${here}/no-such-library`);
} catch (error) {
  threw = /no library/.test(error.message);
}
verify(threw, 'a bad path throws at load');
lib.close();
finish('the libraries script');
