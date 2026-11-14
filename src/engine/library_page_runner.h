// The script side of a library's page (library_page.cc): the runner, the
// first script of the engine's document zebkit-library://<id>/, before any
// of the library's. It takes the frame's channel to the browser
// (__zebkit_call, page_call_renderer.cc), imports the library's entry, and
// then asks the browser for each call in turn: nothing is evaluated in the
// page, so nothing in it runs with a user gesture.
//
// Values are the isolate's (library_bootstrap.h, "the application's calls"):
// JSON with {"$part": n} for bytes, the same checks, the same codes.
//
// Messages, over __zebkit_call("library", method, text, parts):
//   loaded  "<code>\n<json>"                 the load's end
//   next    ""                               answered "<id>\n<sizes>\n<name as JSON>\n<args>",
//                                            the parts in the bytes; not ok: the library ended
//   done    "<id>\n<code>\n<sizes>\n<json>"  a call's end, its parts given
//   log     text                             an uncaught error, to standard error
// <sizes> is the parts' lengths, comma separated.

#ifndef ZEBKIT_ENGINE_LIBRARY_PAGE_RUNNER_H_
#define ZEBKIT_ENGINE_LIBRARY_PAGE_RUNNER_H_

namespace zebkit {

inline constexpr char kLibraryPageRunner[] = R"js((() => {
  'use strict';
  const send = globalThis.__zebkit_call;
  delete globalThis.__zebkit_call;
  const meta = document.querySelector('meta[name="zebkit-library-entry"]');
  const entry = meta.content;
  meta.remove();
  document.currentScript.remove();
  if (typeof send !== 'function') return;
  const call = (method, text, parts) => send('library', method, text, parts || []);
  const CODE = {OK: 0, THROWN: 1, LOAD_FAILED: 2, NO_FUNCTION: 3, BAD_ARGUMENTS: 4, BAD_RESULT: 5};
  const MAX_DEPTH = 200;
  const hasOwn = Object.prototype.hasOwnProperty;
  const stringify = JSON.stringify, parse = JSON.parse, isArray = Array.isArray, isView = ArrayBuffer.isView;

  class Refused extends Error {
    constructor(code, message) { super(message); this.code = code; }
  }
  const describe = (error) => {
    if (error instanceof Error) {
      let name = 'Error', message = '', stack = '';
      try { name = String(error.name); } catch (e) {}
      try { message = String(error.message); } catch (e) {}
      try { stack = String(error.stack || ''); } catch (e) {}
      return {message, name, stack};
    }
    let message;
    try { message = typeof error === 'string' ? error : stringify(error) ?? String(error); } catch (e) { message = String(error); }
    return {message, name: '', stack: ''};
  };
  const errorJson = (message, name = 'Error') => stringify({message, name, stack: ''});
  const log = (text) => { call('log', String(text)).catch(() => {}); };
  addEventListener('error', (event) => {
    const error = event.error;
    log('Uncaught ' + (error instanceof Error && error.stack ? error.stack : event.message));
  });
  addEventListener('unhandledrejection', (event) => {
    const d = describe(event.reason);
    log('Uncaught (in promise) ' + (d.stack || d.message));
  });

  // {"$part": n} in the arguments as parts[n], in place.
  const revive = (value, parts, depth) => {
    if (depth > MAX_DEPTH) throw new Refused(CODE.BAD_ARGUMENTS, `the arguments are nested deeper than ${MAX_DEPTH}`);
    if (value === null || typeof value !== 'object') return value;
    if (isArray(value)) {
      for (let i = 0; i < value.length; ++i) {
        const item = value[i];
        if (item !== null && typeof item === 'object') value[i] = revive(item, parts, depth + 1);
      }
      return value;
    }
    if (hasOwn.call(value, '$part')) {
      const n = value.$part;
      if (Object.keys(value).length !== 1) throw new Refused(CODE.BAD_ARGUMENTS, 'an object with "$part" has another key');
      if (!Number.isInteger(n) || n < 0 || n >= parts.length) {
        throw new Refused(CODE.BAD_ARGUMENTS, `{"$part": ${stringify(n)}} names no part (${parts.length} given)`);
      }
      return parts[n];
    }
    for (const key in value) {
      const item = value[key];
      if (item !== null && typeof item === 'object') value[key] = revive(item, parts, depth + 1);
    }
    return value;
  };

  // Whether @value is what JSON and parts carry; true when it holds bytes.
  const badResult = (message) => new Refused(CODE.BAD_RESULT, message);
  const check = (value, depth, ancestors) => {
    switch (typeof value) {
      case 'string': case 'boolean': case 'undefined': return false;
      case 'number':
        if (!Number.isFinite(value)) throw badResult(`the value has ${value}, which JSON cannot carry`);
        return false;
      case 'bigint': throw badResult('the value has a BigInt');
      case 'function': throw badResult('the value has a function');
      case 'symbol': throw badResult('the value has a symbol');
    }
    if (value === null) return false;
    if (isView(value) || value instanceof ArrayBuffer) return true;
    if (depth > MAX_DEPTH) throw badResult(`the value is nested deeper than ${MAX_DEPTH}`);
    if (ancestors.includes(value)) throw badResult('the value has a cycle');
    ancestors.push(value);
    let bytes = false;
    if (isArray(value)) {
      for (let i = 0; i < value.length; ++i) bytes = check(value[i], depth + 1, ancestors) || bytes;
    } else {
      const proto = Object.getPrototypeOf(value);
      if (proto !== Object.prototype && proto !== null) {
        throw badResult(`the value has an instance of ${proto?.constructor?.name || 'a class'}, not a plain object`);
      }
      if (hasOwn.call(value, '$part')) throw badResult('the value has an object with a "$part" key');
      for (const key of Object.keys(value)) bytes = check(value[key], depth + 1, ancestors) || bytes;
    }
    ancestors.pop();
    return bytes;
  };
  const replaceParts = (value, parts) => {
    if (value === undefined) return null;
    if (value === null || typeof value !== 'object') return value;
    if (isView(value)) {
      parts.push(new Uint8Array(value.buffer, value.byteOffset, value.byteLength));
      return {$part: parts.length - 1};
    }
    if (value instanceof ArrayBuffer) {
      parts.push(new Uint8Array(value));
      return {$part: parts.length - 1};
    }
    if (isArray(value)) return value.map((item) => replaceParts(item, parts));
    const out = {};
    for (const key of Object.keys(value)) out[key] = replaceParts(value[key], parts);
    return out;
  };
  const NO_PARTS = [];
  const done = (id, code, json, parts) => {
    call('done', `${id}\n${code}\n${parts.map((p) => p.byteLength).join(',')}\n${json}`, parts).catch(() => {});
  };
  const settle = (id, value) => {
    let json, parts = NO_PARTS;
    try {
      if (check(value, 0, [])) {
        parts = [];
        json = stringify(replaceParts(value, parts));
      } else {
        json = value === undefined ? 'null' : stringify(value);
      }
    } catch (error) {
      if (error instanceof Refused) return done(id, error.code, errorJson(error.message, 'TypeError'), NO_PARTS);
      return done(id, CODE.BAD_RESULT, stringify(describe(error)), NO_PARTS);
    }
    done(id, CODE.OK, json, parts);
  };
  const thrown = (id) => (error) => done(id, CODE.THROWN, stringify(describe(error)), NO_PARTS);

  let exports = null;
  const run = (id, name, json, parts) => {
    let args;
    try {
      args = parse(json);
    } catch (error) {
      return done(id, CODE.BAD_ARGUMENTS, errorJson('the arguments are not JSON: ' + error.message, 'SyntaxError'), NO_PARTS);
    }
    if (!isArray(args)) return done(id, CODE.BAD_ARGUMENTS, errorJson('the arguments are not a JSON array', 'TypeError'), NO_PARTS);
    try {
      revive(args, parts, 0);
    } catch (error) {
      return done(id, error.code ?? CODE.BAD_ARGUMENTS, errorJson(error.message, 'TypeError'), NO_PARTS);
    }
    const fn = exports && hasOwn.call(exports, name) ? exports[name] : undefined;
    if (typeof fn !== 'function') {
      return done(id, CODE.NO_FUNCTION, errorJson(`the library exports no function "${name}"`, 'ReferenceError'), NO_PARTS);
    }
    let result;
    try {
      result = fn(...args);
    } catch (error) {
      return thrown(id)(error);
    }
    if (result !== null && (typeof result === 'object' || typeof result === 'function') && typeof result.then === 'function') {
      Promise.resolve(result).then((value) => settle(id, value), thrown(id));
    } else {
      settle(id, result);
    }
  };

  // One request for work always out while the page is free: the browser
  // tells a page that waits from one that spins by it.
  const loop = async () => {
    for (;;) {
      let answer;
      try {
        answer = await call('next', '');
      } catch (e) {
        return;
      }
      const [ok, text, bytes] = answer;
      if (!ok) return;
      const a = text.indexOf('\n'), b = text.indexOf('\n', a + 1), c = text.indexOf('\n', b + 1);
      const id = +text.slice(0, a), sizes = text.slice(a + 1, b), name = parse(text.slice(b + 1, c));
      const parts = [];
      if (sizes && bytes && sizes.indexOf(',') < 0 && +sizes === bytes.byteLength) {
        parts.push(new Uint8Array(bytes));  // one part: the buffer it came in, its own
      } else if (sizes) {
        let at = 0;
        for (const size of sizes.split(',')) {
          const n = +size;
          parts.push(n && bytes ? new Uint8Array(bytes.slice(at, at + n)) : new Uint8Array(0));
          at += n;
        }
      }
      run(id, name, text.slice(c + 1), parts);
    }
  };

  import(entry).then((namespace) => {
    exports = namespace;
    const functions = Object.keys(namespace).filter((name) => typeof namespace[name] === 'function');
    call('loaded', `${CODE.OK}\n${stringify({functions})}`).catch(() => {});
    loop();
  }, (error) => {
    call('loaded', `${CODE.LOAD_FAILED}\n${stringify(describe(error))}`).catch(() => {});
  });
})();
)js";

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_LIBRARY_PAGE_RUNNER_H_
