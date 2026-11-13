// zebkit.Library for the host script (script.cc): <zebkit/library.h> in
// JavaScript. Values carry Uint8Arrays in place, as {"$part": n} and parts
// between the two isolates; a call is a promise, cancelled with
// library.cancel(promise).

#ifndef ZEBKIT_ENGINE_LIBRARY_SCRIPT_H_
#define ZEBKIT_ENGINE_LIBRARY_SCRIPT_H_

namespace zebkit {

inline constexpr char kLibraryScript[] = R"js((function (native) {
  'use strict';
  const CODES = ['OK', 'THROWN', 'LOAD_FAILED', 'NO_FUNCTION', 'BAD_ARGUMENTS', 'BAD_RESULT', 'TIMEOUT',
                 'CANCELLED', 'QUEUE_FULL', 'OUT_OF_MEMORY', 'GONE'];
  // What a call or load failed with: code ('THROWN', 'TIMEOUT', ...), and the
  // library's name, message and stack when it threw.
  const failure = (code, json) => {
    let info = {};
    try { info = JSON.parse(json) || {}; } catch (e) {}
    const error = new Error(String(info.message ?? CODES[code]));
    error.name = info.name || 'LibraryError';
    error.code = CODES[code] ?? String(code);
    if (info.stack) error.stack = info.stack;
    return error;
  };
  const toWire = (args) => {
    const parts = [];
    const json = JSON.stringify(args, function (key, value) {
      if (ArrayBuffer.isView(value)) {
        parts.push(new Uint8Array(value.buffer, value.byteOffset, value.byteLength));
        return {$part: parts.length - 1};
      }
      if (value instanceof ArrayBuffer) {
        parts.push(new Uint8Array(value));
        return {$part: parts.length - 1};
      }
      return value;
    });
    return [json, parts];
  };
  const fromWire = (json, parts) => JSON.parse(json, parts.length ? function (key, value) {
    if (value !== null && typeof value === 'object' && !Array.isArray(value) && Number.isInteger(value.$part) &&
        Object.keys(value).length === 1) {
      return parts[value.$part];
    }
    return value;
  } : undefined);
  const CALLS = new WeakMap();

  class Library {
    #id; #ready;
    // A library: a folder with index.mjs, or one .mjs file; options as
    // zebkit_library_load() has them. Throws for bad options or path.
    static load(path, options = {}) { return new Library(path, options); }
    constructor(path, options = {}) {
      let settle;
      this.#ready = new Promise((resolve, reject) => { settle = {resolve, reject}; });
      // Not an uncaught rejection if nothing awaits it: the calls fail too.
      this.#ready.catch(() => {});
      this.#id = native.library('load', String(path), JSON.stringify(options ?? {}), (code, json) => {
        if (code === 0) settle.resolve(JSON.parse(json));
        else settle.reject(failure(code, json));
      });
    }
    // A promise of {functions: [...]}, the library's exported functions.
    get ready() { return this.#ready; }
    // call(name, ...args): a promise of the function's value.
    call(name, ...args) { return this.callWith(name, args); }
    // callWith(name, args, {timeoutMs}): timeoutMs 0 the library's, < 0 none.
    callWith(name, args = [], {timeoutMs = 0} = {}) {
      const [json, parts] = toWire([...args]);
      let settle;
      const promise = new Promise((resolve, reject) => { settle = {resolve, reject}; });
      const id = native.library('call', this.#id, String(name), json, parts, Number(timeoutMs) || 0,
        (code, json, parts) => {
          if (code === 0) settle.resolve(fromWire(json, parts));
          else settle.reject(failure(code, json));
        });
      if (!id) settle.reject(failure(10, JSON.stringify({message: 'the library is closed', name: 'LibraryError'})));
      else CALLS.set(promise, id);
      return promise;
    }
    // The call whose promise it is is rejected with code 'CANCELLED', unless settled already.
    cancel(promise) {
      const id = CALLS.get(promise);
      if (id) native.library('cancel', this.#id, id);
    }
    // Calls not settled are rejected with code 'GONE'.
    close() { native.library('close', this.#id); }
  }
  Library.CODES = Object.freeze([...CODES]);
  return Library;
}))js";

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_LIBRARY_SCRIPT_H_
