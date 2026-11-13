// The script side of a library's isolate (library.cc): the Web APIs a
// library has (docs/JS_LIBRARIES.md, "Backend isolate"), over the native
// functions it is called with, and the runner of the application's calls.
// It runs once, before the library's own code; what it returns is the
// runner, which library.cc keeps. No zebkit object, no files, no DOM.

#ifndef ZEBKIT_ENGINE_LIBRARY_BOOTSTRAP_H_
#define ZEBKIT_ENGINE_LIBRARY_BOOTSTRAP_H_

namespace zebkit {

inline constexpr char kLibraryBootstrap[] = R"js((function (native) {
  'use strict';
  const define = (name, value) => Object.defineProperty(globalThis, name,
      {value, writable: true, configurable: true, enumerable: false});
  const CODE = native.codes;

  // ---- console ----
  const format = (value) => {
    if (typeof value === 'string') return value;
    if (value instanceof Error) return value.stack || String(value);
    if (ArrayBuffer.isView(value)) return `${value.constructor.name}(${value.length})`;
    try {
      const json = JSON.stringify(value);
      return json === undefined ? String(value) : json;
    } catch (e) {
      return String(value);
    }
  };
  const print = (level) => (...values) => native.print(level, values.map(format).join(' '));
  define('console', Object.freeze({
    log: print(0), info: print(0), debug: print(0), trace: print(0), warn: print(1), error: print(1),
    assert: (ok, ...values) => { if (!ok) print(1)('Assertion failed:', ...values); },
  }));

  // ---- timers and microtasks ----
  const timer = (repeat) => (fn, ms = 0, ...args) => {
    if (typeof fn !== 'function') throw new TypeError('a timer needs a function');
    return native.setTimer(args.length ? () => fn(...args) : fn, Math.max(0, +ms || 0), repeat);
  };
  define('setTimeout', timer(false));
  define('setInterval', timer(true));
  define('clearTimeout', (id) => { if (id !== undefined && id !== null) native.clearTimer(+id); });
  define('clearInterval', globalThis.clearTimeout);
  define('queueMicrotask', (fn) => {
    if (typeof fn !== 'function') throw new TypeError('queueMicrotask needs a function');
    native.queueMicrotask(fn);
  });

  // ---- DOMException, Event, EventTarget, AbortController ----
  const DOM_CODES = {IndexSizeError: 1, NotFoundError: 8, NotSupportedError: 9, InvalidStateError: 11, SyntaxError: 12,
                     InvalidCharacterError: 5, NetworkError: 19, AbortError: 20, TimeoutError: 23,
                     QuotaExceededError: 22, DataCloneError: 25};
  class DOMException extends Error {
    #name;
    constructor(message = '', name = 'Error') {
      super(String(message));
      this.#name = String(name);
    }
    get name() { return this.#name; }
    get code() { return DOM_CODES[this.#name] || 0; }
  }
  define('DOMException', DOMException);

  const EVENT = Symbol('event');
  class Event {
    constructor(type, init = {}) {
      if (arguments.length === 0) throw new TypeError('an Event needs a type');
      this[EVENT] = {type: String(type), bubbles: !!init.bubbles, cancelable: !!init.cancelable,
                     composed: !!init.composed, defaultPrevented: false, target: null, currentTarget: null,
                     stop: false, stopNow: false, timeStamp: performance.now()};
    }
    get type() { return this[EVENT].type; }
    get bubbles() { return this[EVENT].bubbles; }
    get cancelable() { return this[EVENT].cancelable; }
    get composed() { return this[EVENT].composed; }
    get defaultPrevented() { return this[EVENT].defaultPrevented; }
    get target() { return this[EVENT].target; }
    get currentTarget() { return this[EVENT].currentTarget; }
    get srcElement() { return this[EVENT].target; }
    get timeStamp() { return this[EVENT].timeStamp; }
    get isTrusted() { return false; }
    get eventPhase() { return this[EVENT].currentTarget ? 2 : 0; }
    composedPath() { return this[EVENT].currentTarget ? [this[EVENT].currentTarget] : []; }
    preventDefault() { if (this[EVENT].cancelable) this[EVENT].defaultPrevented = true; }
    stopPropagation() { this[EVENT].stop = true; }
    stopImmediatePropagation() { this[EVENT].stop = this[EVENT].stopNow = true; }
  }
  define('Event', Event);
  class CustomEvent extends Event {
    #detail;
    constructor(type, init = {}) { super(type, init); this.#detail = init.detail ?? null; }
    get detail() { return this.#detail; }
  }
  define('CustomEvent', CustomEvent);

  const LISTENERS = Symbol('listeners');
  class EventTarget {
    constructor() { this[LISTENERS] = new Map(); }
    addEventListener(type, listener, options = {}) {
      if (listener === null || listener === undefined) return;
      const capture = typeof options === 'boolean' ? options : !!options.capture;
      const once = typeof options === 'object' && !!options.once;
      const signal = typeof options === 'object' ? options.signal : undefined;
      if (signal && signal.aborted) return;
      const list = this[LISTENERS].get(String(type)) || [];
      if (list.some((entry) => entry.listener === listener && entry.capture === capture)) return;
      const entry = {listener, capture, once};
      list.push(entry);
      this[LISTENERS].set(String(type), list);
      if (signal) signal.addEventListener('abort', () => this.removeEventListener(type, listener, {capture}));
    }
    removeEventListener(type, listener, options = {}) {
      const capture = typeof options === 'boolean' ? options : !!(options && options.capture);
      const list = this[LISTENERS].get(String(type));
      if (!list) return;
      const at = list.findIndex((entry) => entry.listener === listener && entry.capture === capture);
      if (at >= 0) list.splice(at, 1);
    }
    dispatchEvent(event) {
      if (!(event instanceof Event)) throw new TypeError('dispatchEvent needs an Event');
      const state = event[EVENT];
      state.target = state.currentTarget = this;
      const handler = this['on' + state.type];
      const list = [...(this[LISTENERS].get(state.type) || [])];
      if (typeof handler === 'function') list.unshift({listener: handler, once: false, own: true});
      for (const entry of list) {
        if (state.stopNow) break;
        if (entry.once) this.removeEventListener(state.type, entry.listener, {capture: entry.capture});
        try {
          if (typeof entry.listener === 'function') entry.listener.call(this, event);
          else if (entry.listener && typeof entry.listener.handleEvent === 'function') entry.listener.handleEvent(event);
        } catch (error) {
          native.print(1, 'Uncaught ' + format(error));
        }
      }
      state.currentTarget = null;
      return !state.defaultPrevented;
    }
  }
  define('EventTarget', EventTarget);

  const SIGNAL = Symbol('signal');
  class AbortSignal extends EventTarget {
    constructor(key) {
      if (key !== SIGNAL) throw new TypeError('an AbortSignal comes from an AbortController');
      super();
      this.onabort = null;
      this[SIGNAL] = {aborted: false, reason: undefined};
    }
    get aborted() { return this[SIGNAL].aborted; }
    get reason() { return this[SIGNAL].reason; }
    throwIfAborted() { if (this[SIGNAL].aborted) throw this[SIGNAL].reason; }
    static abort(reason) {
      const signal = new AbortSignal(SIGNAL);
      abortSignal(signal, reason);
      return signal;
    }
    static timeout(ms) {
      const signal = new AbortSignal(SIGNAL);
      setTimeout(() => abortSignal(signal, new DOMException('the signal timed out', 'TimeoutError')), ms);
      return signal;
    }
    static any(signals) {
      const signal = new AbortSignal(SIGNAL);
      for (const other of signals) {
        if (other.aborted) { abortSignal(signal, other.reason); return signal; }
      }
      for (const other of signals) other.addEventListener('abort', () => abortSignal(signal, other.reason), {once: true});
      return signal;
    }
  }
  const abortSignal = (signal, reason) => {
    const state = signal[SIGNAL];
    if (state.aborted) return;
    state.aborted = true;
    state.reason = reason === undefined ? new DOMException('the operation was aborted', 'AbortError') : reason;
    signal.dispatchEvent(new Event('abort'));
  };
  define('AbortSignal', AbortSignal);
  class AbortController {
    #signal = new AbortSignal(SIGNAL);
    get signal() { return this.#signal; }
    abort(reason) { abortSignal(this.#signal, reason); }
  }
  define('AbortController', AbortController);

  // ---- text, base64 ----
  const UTF8_LABELS = new Set(['utf-8', 'utf8', 'unicode-1-1-utf-8', 'unicode11utf8', 'unicode20utf8', 'x-unicode20utf8']);
  const bytesOf = (input) => {
    if (input instanceof Uint8Array) return input;
    if (ArrayBuffer.isView(input)) return new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
    if (input instanceof ArrayBuffer || (typeof SharedArrayBuffer !== 'undefined' && input instanceof SharedArrayBuffer)) {
      return new Uint8Array(input);
    }
    throw new TypeError('not a buffer or a view of one');
  };
  class TextEncoder {
    get encoding() { return 'utf-8'; }
    encode(text = '') { return native.encode(String(text)); }
    encodeInto(text, into) {
      if (!(into instanceof Uint8Array)) throw new TypeError('encodeInto needs a Uint8Array');
      const [read, written] = native.encodeInto(String(text), into);
      return {read, written};
    }
  }
  define('TextEncoder', TextEncoder);
  // Bytes at the end of @b that may begin a sequence the next chunk ends.
  const incompleteTail = (b) => {
    for (let back = 1; back <= Math.min(3, b.length); ++back) {
      const c = b[b.length - back];
      if ((c & 0xc0) === 0x80) continue;
      const need = c >= 0xf0 && c < 0xf5 ? 4 : c >= 0xe0 && c < 0xf0 ? 3 : c >= 0xc2 && c < 0xe0 ? 2 : 1;
      return need > back ? back : 0;
    }
    return 0;
  };
  class TextDecoder {
    #fatal; #ignoreBOM; #pending = null; #started = false;
    constructor(label = 'utf-8', options = {}) {
      if (!UTF8_LABELS.has(String(label).trim().toLowerCase())) {
        throw new RangeError(`TextDecoder: "${label}" is not supported here (UTF-8 only)`);
      }
      this.#fatal = !!options.fatal;
      this.#ignoreBOM = !!options.ignoreBOM;
    }
    get encoding() { return 'utf-8'; }
    get fatal() { return this.#fatal; }
    get ignoreBOM() { return this.#ignoreBOM; }
    decode(input, options = {}) {
      let bytes = input === undefined ? new Uint8Array(0) : bytesOf(input);
      if (this.#pending) {
        const joined = new Uint8Array(this.#pending.length + bytes.length);
        joined.set(this.#pending);
        joined.set(bytes, this.#pending.length);
        bytes = joined;
        this.#pending = null;
      }
      const stream = !!(options && options.stream);
      if (stream) {
        const tail = incompleteTail(bytes);
        if (tail) {
          this.#pending = bytes.slice(bytes.length - tail);
          bytes = bytes.subarray(0, bytes.length - tail);
        }
      }
      const skipBOM = !this.#ignoreBOM && !this.#started;
      if (bytes.length) this.#started = true;
      const text = native.decode(bytes, this.#fatal, skipBOM);
      if (text === null) throw new TypeError('TextDecoder: the data is not valid UTF-8');
      if (!stream) this.#started = false;
      return text;
    }
  }
  define('TextDecoder', TextDecoder);
  define('btoa', (text) => {
    const out = native.btoa(String(text));
    if (out === null) throw new DOMException('btoa: a character outside Latin-1', 'InvalidCharacterError');
    return out;
  });
  define('atob', (text) => {
    const out = native.atob(String(text));
    if (out === null) throw new DOMException('atob: not valid base64', 'InvalidCharacterError');
    return out;
  });

  // ---- URL, URLSearchParams ----
  // application/x-www-form-urlencoded, as the URL standard has it.
  const formEncode = (text) => {
    const bytes = native.encode(String(text));
    let out = '';
    for (const b of bytes) {
      if (b === 0x20) out += '+';
      else if ((b >= 0x30 && b <= 0x39) || (b >= 0x41 && b <= 0x5a) || (b >= 0x61 && b <= 0x7a) ||
               b === 0x2a || b === 0x2d || b === 0x2e || b === 0x5f) out += String.fromCharCode(b);
      else out += '%' + b.toString(16).toUpperCase().padStart(2, '0');
    }
    return out;
  };
  const formDecode = (text) => {
    const s = text.replace(/\+/g, ' ');
    const bytes = [];
    for (let i = 0; i < s.length; ++i) {
      if (s[i] === '%' && /^[0-9a-fA-F]{2}$/.test(s.slice(i + 1, i + 3))) {
        bytes.push(parseInt(s.slice(i + 1, i + 3), 16));
        i += 2;
      } else {
        for (const b of native.encode(s[i] === '%' ? '%' : s.codePointAt(i) > 0xffff ? String.fromCodePoint(s.codePointAt(i)) : s[i])) bytes.push(b);
        if (s.codePointAt(i) > 0xffff) ++i;
      }
    }
    return native.decode(new Uint8Array(bytes), false, false);
  };
  const PARAMS_URL = Symbol('url');
  class URLSearchParams {
    #list = [];
    constructor(init = '') {
      this[PARAMS_URL] = null;
      if (init instanceof URLSearchParams) {
        this.#list = init.#list.map(([k, v]) => [k, v]);
      } else if (init !== null && typeof init === 'object') {
        if (typeof init[Symbol.iterator] === 'function') {
          for (const pair of init) {
            const entry = [...pair];
            if (entry.length !== 2) throw new TypeError('URLSearchParams: a pair needs a name and a value');
            this.#list.push([String(entry[0]), String(entry[1])]);
          }
        } else {
          for (const key of Object.keys(init)) this.#list.push([key, String(init[key])]);
        }
      } else {
        this.#parse(String(init));
      }
    }
    #parse(text) {
      this.#list = [];
      for (const piece of (text.startsWith('?') ? text.slice(1) : text).split('&')) {
        if (!piece) continue;
        const at = piece.indexOf('=');
        const name = at < 0 ? piece : piece.slice(0, at), value = at < 0 ? '' : piece.slice(at + 1);
        this.#list.push([formDecode(name), formDecode(value)]);
      }
    }
    static _reset(params, text) { params.#parse(text); }
    #update() { if (this[PARAMS_URL]) this[PARAMS_URL]._setQuery(this.toString()); }
    get size() { return this.#list.length; }
    append(name, value) { this.#list.push([String(name), String(value)]); this.#update(); }
    delete(name, value) {
      name = String(name);
      this.#list = this.#list.filter(([k, v]) => !(k === name && (value === undefined || v === String(value))));
      this.#update();
    }
    get(name) { const found = this.#list.find(([k]) => k === String(name)); return found ? found[1] : null; }
    getAll(name) { return this.#list.filter(([k]) => k === String(name)).map(([, v]) => v); }
    has(name, value) {
      return this.#list.some(([k, v]) => k === String(name) && (value === undefined || v === String(value)));
    }
    set(name, value) {
      name = String(name);
      const at = this.#list.findIndex(([k]) => k === name);
      if (at < 0) this.#list.push([name, String(value)]);
      else {
        this.#list[at][1] = String(value);
        this.#list = this.#list.filter(([k], i) => k !== name || i === at);
      }
      this.#update();
    }
    sort() {
      this.#list = this.#list.map((entry, i) => [entry, i]).sort((a, b) => {
        const x = a[0][0], y = b[0][0];
        return x < y ? -1 : x > y ? 1 : a[1] - b[1];
      }).map(([entry]) => entry);
      this.#update();
    }
    forEach(fn, self) { for (const [k, v] of this.#list) fn.call(self, v, k, this); }
    keys() { return this.#list.map(([k]) => k)[Symbol.iterator](); }
    values() { return this.#list.map(([, v]) => v)[Symbol.iterator](); }
    entries() { return this.#list.map(([k, v]) => [k, v])[Symbol.iterator](); }
    [Symbol.iterator]() { return this.entries(); }
    toString() { return this.#list.map(([k, v]) => formEncode(k) + '=' + formEncode(v)).join('&'); }
  }
  define('URLSearchParams', URLSearchParams);

  // native.url() and native.urlSet(): [href, protocol, username, password,
  // host, hostname, port, pathname, search, hash, origin], or null.
  const PARTS = ['href', 'protocol', 'username', 'password', 'host', 'hostname', 'port', 'pathname', 'search', 'hash', 'origin'];
  const URL_STATE = Symbol('url');
  class URL {
    constructor(url, base) {
      const parts = native.url(String(url), base === undefined ? null : String(base));
      if (!parts) throw new TypeError(`Invalid URL: ${url}`);
      const params = new URLSearchParams(parts[8]);
      params[PARAMS_URL] = this;
      this[URL_STATE] = {parts, params};
    }
    static canParse(url, base) { return native.url(String(url), base === undefined ? null : String(base)) !== null; }
    static parse(url, base) { try { return new URL(url, base); } catch (e) { return null; } }
    _setQuery(query) {
      const parts = native.urlSet(this[URL_STATE].parts[0], 'search', query);
      if (parts) this[URL_STATE].parts = parts;
    }
    get href() { return this[URL_STATE].parts[0]; }
    set href(value) {
      const parts = native.url(String(value), null);
      if (!parts) throw new TypeError(`Invalid URL: ${value}`);
      this[URL_STATE].parts = parts;
      URLSearchParams._reset(this[URL_STATE].params, parts[8]);
    }
    get origin() { return this[URL_STATE].parts[10]; }
    get searchParams() { return this[URL_STATE].params; }
    toString() { return this[URL_STATE].parts[0]; }
    toJSON() { return this[URL_STATE].parts[0]; }
  }
  PARTS.forEach((name, i) => {
    if (name === 'href' || name === 'origin') return;
    Object.defineProperty(URL.prototype, name, {
      get() { return this[URL_STATE].parts[i]; },
      set(value) {
        const parts = native.urlSet(this[URL_STATE].parts[0], name, String(value));
        if (!parts) return;
        this[URL_STATE].parts = parts;
        URLSearchParams._reset(this[URL_STATE].params, parts[8]);
      },
      configurable: true,
      enumerable: true,
    });
  });
  define('URL', URL);

  // ---- structuredClone, crypto, performance ----
  define('structuredClone', (value, options) => {
    if (options && options.transfer && options.transfer.length) {
      throw new DOMException('structuredClone: transfer is not supported here', 'DataCloneError');
    }
    return native.clone(value);
  });
  const INTEGER_ARRAYS = [Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array, Int32Array, Uint32Array,
                          BigInt64Array, BigUint64Array];
  define('crypto', Object.freeze({
    getRandomValues(array) {
      if (!INTEGER_ARRAYS.some((type) => array instanceof type)) {
        throw new DOMException('getRandomValues needs an integer typed array', 'TypeMismatchError');
      }
      if (array.byteLength > 65536) throw new DOMException('getRandomValues: more than 65536 bytes', 'QuotaExceededError');
      native.randomFill(array);
      return array;
    },
    randomUUID: () => native.uuid(),
  }));
  const timeOrigin = native.timeOrigin;
  define('performance', Object.freeze({
    now: () => native.now(),
    timeOrigin,
    toJSON: () => ({timeOrigin}),
  }));

  // ---- fetch: Headers, Response; only to the origins granted at load ----
  const HEADERS = Symbol('headers');
  class Headers {
    constructor(init = undefined) {
      this[HEADERS] = [];
      if (init instanceof Headers) this[HEADERS] = init[HEADERS].map(([k, v]) => [k, v]);
      else if (init && typeof init[Symbol.iterator] === 'function') for (const [k, v] of init) this.append(k, v);
      else if (init && typeof init === 'object') for (const key of Object.keys(init)) this.append(key, init[key]);
    }
    append(name, value) { this[HEADERS].push([String(name).toLowerCase(), String(value).trim()]); }
    delete(name) { name = String(name).toLowerCase(); this[HEADERS] = this[HEADERS].filter(([k]) => k !== name); }
    get(name) {
      name = String(name).toLowerCase();
      const values = this[HEADERS].filter(([k]) => k === name).map(([, v]) => v);
      return values.length ? values.join(', ') : null;
    }
    getSetCookie() { return this[HEADERS].filter(([k]) => k === 'set-cookie').map(([, v]) => v); }
    has(name) { name = String(name).toLowerCase(); return this[HEADERS].some(([k]) => k === name); }
    set(name, value) { this.delete(name); this.append(name, value); }
    forEach(fn, self) { for (const [k, v] of this.entries()) fn.call(self, v, k, this); }
    *entries() {
      const names = [...new Set(this[HEADERS].map(([k]) => k))].sort();
      for (const name of names) yield [name, this.get(name)];
    }
    keys() { return [...this.entries()].map(([k]) => k)[Symbol.iterator](); }
    values() { return [...this.entries()].map(([, v]) => v)[Symbol.iterator](); }
    [Symbol.iterator]() { return this.entries(); }
  }
  define('Headers', Headers);
  const bodyBytes = (body) => {
    if (body === undefined || body === null) return null;
    if (typeof body === 'string') return native.encode(body);
    if (body instanceof URLSearchParams) return native.encode(body.toString());
    if (ArrayBuffer.isView(body) || body instanceof ArrayBuffer) return new Uint8Array(bytesOf(body));
    throw new TypeError('fetch: a body is a string, a URLSearchParams, or bytes');
  };
  const RESPONSE = Symbol('response');
  class Response {
    constructor(body = null, init = {}) {
      this[RESPONSE] = {bytes: bodyBytes(body) || new Uint8Array(0), used: false, url: '', redirected: false};
      this.status = init.status ?? 200;
      this.statusText = init.statusText ?? '';
      this.headers = new Headers(init.headers);
      if (typeof body === 'string' && !this.headers.has('content-type')) this.headers.set('content-type', 'text/plain;charset=UTF-8');
    }
    get ok() { return this.status >= 200 && this.status < 300; }
    get url() { return this[RESPONSE].url; }
    get redirected() { return this[RESPONSE].redirected; }
    get bodyUsed() { return this[RESPONSE].used; }
    get type() { return 'basic'; }
    #take() {
      if (this[RESPONSE].used) return Promise.reject(new TypeError('the body was read already'));
      this[RESPONSE].used = true;
      return Promise.resolve(this[RESPONSE].bytes);
    }
    arrayBuffer() { return this.#take().then((b) => b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength)); }
    bytes() { return this.#take().then((b) => new Uint8Array(b)); }
    text() { return this.#take().then((b) => native.decode(b, false, true)); }
    json() { return this.text().then(JSON.parse); }
    clone() {
      if (this[RESPONSE].used) throw new TypeError('the body was read already');
      const copy = new Response(null, {status: this.status, statusText: this.statusText, headers: this.headers});
      copy[RESPONSE] = {...this[RESPONSE], bytes: new Uint8Array(this[RESPONSE].bytes)};
      return copy;
    }
    static json(data, init = {}) {
      const response = new Response(JSON.stringify(data), init);
      response.headers.set('content-type', 'application/json');
      return response;
    }
    static error() { const response = new Response(null, {status: 0}); return response; }
  }
  define('Response', Response);
  class Request {
    constructor(input, init = {}) {
      const from = input instanceof Request ? input : null;
      this.url = from ? from.url : new URL(String(input)).href;
      this.method = String(init.method ?? from?.method ?? 'GET').toUpperCase();
      this.headers = new Headers(init.headers ?? from?.headers);
      this.signal = init.signal ?? from?.signal ?? null;
      this._body = init.body !== undefined ? bodyBytes(init.body) : from ? from._body : null;
    }
  }
  define('Request', Request);
  define('fetch', (input, init = {}) => new Promise((resolve, reject) => {
    const request = new Request(input, init);
    const signal = request.signal;
    if (signal && signal.aborted) return reject(signal.reason);
    let done = false;
    const abort = () => { if (!done) { done = true; reject(signal.reason); } };
    if (signal) signal.addEventListener('abort', abort, {once: true});
    const headers = [...request.headers[HEADERS]].map(([k, v]) => `${k}: ${v}`).join('\r\n');
    native.fetch(request.method, request.url, headers, request._body, (error, status, url, lines, body, redirected) => {
      if (done) return;
      done = true;
      if (signal) signal.removeEventListener('abort', abort);
      if (error !== null) return reject(new TypeError('fetch failed: ' + error));
      const response = new Response(null, {status, headers: lines.split('\r\n').filter(Boolean).map((line) => {
        const at = line.indexOf(':');
        return [line.slice(0, at).trim(), line.slice(at + 1).trim()];
      })});
      response[RESPONSE].bytes = body;
      response[RESPONSE].url = url;
      response[RESPONSE].redirected = redirected;
      resolve(response);
    });
  }));

  // ---- the application's calls ----
  const MAX_DEPTH = 200;
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
    try { message = typeof error === 'string' ? error : JSON.stringify(error) ?? String(error); } catch (e) { message = String(error); }
    return {message, name: '', stack: ''};
  };
  const errorJson = (message, name = 'Error') => JSON.stringify({message, name, stack: ''});
  const hasOwn = Object.prototype.hasOwnProperty;

  // {"$part": n} in the arguments as parts[n], in place.
  const revive = (value, parts, depth) => {
    if (depth > MAX_DEPTH) throw new Refused(CODE.BAD_ARGUMENTS, `the arguments are nested deeper than ${MAX_DEPTH}`);
    if (value === null || typeof value !== 'object') return value;
    if (Array.isArray(value)) {
      for (let i = 0; i < value.length; ++i) {
        const item = value[i];
        if (item !== null && typeof item === 'object') value[i] = revive(item, parts, depth + 1);
      }
      return value;
    }
    if (hasOwn.call(value, '$part')) {
      const n = value.$part;
      if (Object.keys(value).length !== 1) {
        throw new Refused(CODE.BAD_ARGUMENTS, 'an object with "$part" has another key');
      }
      if (!Number.isInteger(n) || n < 0 || n >= parts.length) {
        throw new Refused(CODE.BAD_ARGUMENTS, `{"$part": ${JSON.stringify(n)}} names no part (${parts.length} given)`);
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
    if (ArrayBuffer.isView(value) || value instanceof ArrayBuffer) return true;
    if (depth > MAX_DEPTH) throw badResult(`the value is nested deeper than ${MAX_DEPTH}`);
    if (ancestors.includes(value)) throw badResult('the value has a cycle');
    ancestors.push(value);
    let bytes = false;
    if (Array.isArray(value)) {
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
    if (ArrayBuffer.isView(value)) {
      parts.push(new Uint8Array(value.buffer, value.byteOffset, value.byteLength));
      return {$part: parts.length - 1};
    }
    if (value instanceof ArrayBuffer) {
      parts.push(new Uint8Array(value));
      return {$part: parts.length - 1};
    }
    if (Array.isArray(value)) return value.map((item) => replaceParts(item, parts));
    const out = {};
    for (const key of Object.keys(value)) out[key] = replaceParts(value[key], parts);
    return out;
  };
  const NO_PARTS = [];
  const settle = (id, value) => {
    let json, parts = NO_PARTS;
    try {
      if (check(value, 0, [])) {
        parts = [];
        json = JSON.stringify(replaceParts(value, parts));
      } else {
        json = value === undefined ? 'null' : JSON.stringify(value);
      }
    } catch (error) {
      if (error instanceof Refused) return native.settle(id, error.code, errorJson(error.message, 'TypeError'), NO_PARTS);
      return native.settle(id, CODE.BAD_RESULT, JSON.stringify(describe(error)), NO_PARTS);
    }
    native.settle(id, CODE.OK, json, parts);
  };
  const thrown = (id) => (error) => native.settle(id, CODE.THROWN, JSON.stringify(describe(error)), NO_PARTS);

  let exports = null;
  return Object.freeze({
    // After the module's evaluation, which @evaluated is a promise of.
    loaded(evaluated, namespace) {
      Promise.resolve(evaluated).then(() => {
        exports = namespace;
        const functions = Object.keys(namespace).filter((name) => typeof namespace[name] === 'function');
        native.loaded(CODE.OK, JSON.stringify({functions}));
      }, (error) => native.loaded(CODE.LOAD_FAILED, JSON.stringify(describe(error))));
    },
    run(id, name, json, parts) {
      let args;
      try {
        args = JSON.parse(json);
      } catch (error) {
        return native.settle(id, CODE.BAD_ARGUMENTS, errorJson('the arguments are not JSON: ' + error.message, 'SyntaxError'), NO_PARTS);
      }
      if (!Array.isArray(args)) {
        return native.settle(id, CODE.BAD_ARGUMENTS, errorJson('the arguments are not a JSON array', 'TypeError'), NO_PARTS);
      }
      try {
        revive(args, parts, 0);
      } catch (error) {
        return native.settle(id, error.code ?? CODE.BAD_ARGUMENTS, errorJson(error.message, 'TypeError'), NO_PARTS);
      }
      const fn = exports && hasOwn.call(exports, name) ? exports[name] : undefined;
      if (typeof fn !== 'function') {
        return native.settle(id, CODE.NO_FUNCTION, errorJson(`the library exports no function "${name}"`, 'ReferenceError'), NO_PARTS);
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
    },
  });
}))js";

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_LIBRARY_BOOTSTRAP_H_
