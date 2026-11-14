// zebkit.Library with the backend "page" from host JavaScript: a library in a
// page of its own (docs/JS_LIBRARIES.md), through the same load and calls.
// tests/engine/library_page.py has every case; this is the binding's.
//
//   zebkit-js library_page.mjs <tests/engine dir>
import {check, finish} from './helpers.mjs';

const here = zebkit.args[0];  // tests/engine
const lib = zebkit.Library.load(`${here}/library_page_fixture`, {backend: 'page', width: 500, height: 400});
await check('page: ready, the exported functions', async () => {
  const {functions} = await lib.ready;
  return ['measure', 'layout', 'svg'].every((name) => functions.includes(name)) || functions;
});
await check('page: a canvas measures text', async () => {
  const {width} = await lib.call('measure', 'Hello, world', '16px sans-serif');
  return width > 20 || width;
});
await check('page: its size, visible, no user activation', async () => {
  const info = await lib.call('page');
  const activation = await lib.call('activation');
  return info.width === 500 && info.height === 400 && info.visibility === 'visible' && !activation.isActive || [info, activation];
});
await check('page: JSON and a Uint8Array echoed, in place', async () => {
  const value = await lib.call('echo', {b: new Uint8Array([1, 2, 255])}, '日本語 🦓');
  return value[0].b instanceof Uint8Array && value[0].b.join() === '1,2,255' && value[1] === '日本語 🦓' || value;
});
await check('page: THROWN, with error.code', async () => {
  try {
    await lib.call('thrower');
    return 'resolved';
  } catch (error) {
    return error.code === 'THROWN' && error.name === 'RangeError' || [error.code, error.name];
  }
});
lib.close();
finish('the library page script');
