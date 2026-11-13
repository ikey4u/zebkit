// A page's console through zebkit-js's View 'console' event, one mode an engine;
// run by tests/engine/console.py, which has the modes and the server.
import {finish, until, verify} from './helpers.mjs';

const [base, mode] = zebkit.args;
const load = async (view, path) => {
  const done = new Promise(resolve => view.on('load', (phase, url) => { if (phase === 'finished' && url === base + path) resolve(); }));
  view.load(base + path);
  await done;
};
const first = new zebkit.View(''), other = new zebkit.View('');
await load(first, '/initial');
await load(other, '/other');
const events = [];
first.on('console', event => events.push(event));
const messages = () => events.map(event => event.message);
if (mode === 'levels') {
  await first.evaluate("console.log('中文😀\\u0000tail'); console.warn('warning-text'); console.error('error-text'); true\n//# sourceURL=js-console-test.js");
  verify(await until(() => events.length >= 3), 'three messages', messages());
  const recorded = Object.fromEntries(events.map(event => [event.message, event]));
  verify(['中文😀\0tail', 'warning-text', 'error-text'].map(m => recorded[m]?.level).join() === 'info,warning,error',
    'their levels; Unicode and NUL kept', messages());
  const sample = recorded['warning-text'];
  verify(sample && sample.origin === base && sample.mainFrame === true && sample.line >= 1 &&
         sample.source.endsWith('js-console-test.js') && !sample.truncated, 'origin, frame, line and source', sample);
} else if (mode === 'bounds') {
  await first.evaluate("console.log('x'.repeat(65535)+'😀tail'); true");
  verify(await until(() => events.some(event => event.truncated)), 'a bounded message');
  verify(events.find(event => event.truncated).message === 'x'.repeat(65535), 'cut at 65535 characters');
} else if (mode === 'frame') {
  await load(first, '/frame');
  verify(await until(() => messages().includes('cross-site child')), 'the other site\'s frame\'s message', messages());
  const child = events.find(event => event.message === 'cross-site child');
  verify(child && !child.mainFrame && child.origin === base.replace('127.0.0.1', 'localhost'), 'its origin, not the main frame', child);
} else if (mode === 'isolation') {
  await other.evaluate("console.warn('other-view'); true");
  await first.evaluate("console.warn('this-view'); true");
  verify(await until(() => messages().includes('this-view')) && !messages().includes('other-view'),
    'this view\'s message, not the other\'s', messages());
} else if (mode === 'close') {
  const closed = new Promise(resolve => first.on('close', resolve));
  first.on('console', () => first.close());
  first.evaluate("console.log('close-now'); true").catch(() => {});
  await closed;
  verify(true, 'closed from the function');
}
const closing = [first, other].map(view => new Promise(resolve => view.on('close', resolve)));
if (mode !== 'close') first.close();
other.close();
await Promise.all(mode === 'close' ? [closing[1]] : closing);
finish('JavaScript console ' + mode);
