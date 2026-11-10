// zebkit_script: JavaScript of the application's own, outside any page.
//
// A V8 isolate in the application's process, on the engine's thread, with
// zebkit.* bound: the services of layer 2 (through bridge.cc, as a page's
// calls go, but with everything allowed and bytes as Uint8Arrays), views
// to make and drive, timers and console. ES modules come from files.

#ifdef UNSAFE_BUFFERS_BUILD
// The C API takes and returns C strings and arrays.
#pragma allow_unsafe_buffers
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/strings/string_number_conversions.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/memory/ptr_util.h"
#include "base/memory/ref_counted.h"
#include "base/threading/thread_restrictions.h"
#include "base/time/time.h"
#include "base/values.h"
#include "content/public/browser/browser_thread.h"
#include "gin/array_buffer.h"
#include "gin/public/isolate_holder.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-external.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-json.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-promise.h"
#include "v8/include/v8-script.h"
#include "v8/include/v8-typed-array.h"
#include "zebkit/engine/path_utf8.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/include/zebkit/automation.h"
#include "zebkit/engine/include/zebkit/media.h"
#include "zebkit/engine/include/zebkit/media_tap.h"
#include "zebkit/engine/include/zebkit/media_tools.h"
#include "zebkit/engine/include/zebkit/offscreen.h"
#include "zebkit/engine/include/zebkit/page.h"
#include "zebkit/engine/include/zebkit/window.h"
#include "zebkit/engine/pdf_script.h"
#include "zebkit/engine/ui.h"
#include "zebkit/engine/ui_script.h"
#include "zebkit/engine/view.h"
#include "zebkit/engine/popup_metadata.h"

namespace {

// The script's side of zebkit.*, made from the native functions it is
// called with. It runs before the application's code.
constexpr char kBootstrap[] = R"js((function (native) {
  'use strict';
  const format = (value) => {
    if (typeof value === 'string') return value;
    if (value instanceof Error) return value.stack || String(value);
    if (value instanceof Uint8Array) return `Uint8Array(${value.length})`;
    try {
      const json = JSON.stringify(value);
      return json === undefined ? String(value) : json;
    } catch (e) {
      return String(value);
    }
  };
  const print = (level) => (...values) => native.print(level, values.map(format).join(' '));
  globalThis.console = Object.freeze({
    log: print(0), info: print(0), debug: print(0), warn: print(1), error: print(1),
  });
  globalThis.setTimeout = (fn, ms, ...args) => native.setTimer(() => fn(...args), +ms || 0, false);
  globalThis.setInterval = (fn, ms, ...args) => native.setTimer(() => fn(...args), +ms || 0, true);
  globalThis.clearTimeout = globalThis.clearInterval = (id) => native.clearTimer(id);

  // UTF-8, as the web's TextEncoder and TextDecoder have it.
  globalThis.TextEncoder = class TextEncoder {
    get encoding() { return 'utf-8'; }
    encode(text = '') {
      const s = String(text), out = [];
      for (let i = 0; i < s.length; ++i) {
        let c = s.codePointAt(i);
        if (c > 0xffff) ++i;
        if (c >= 0xd800 && c <= 0xdfff) c = 0xfffd;
        if (c < 0x80) out.push(c);
        else if (c < 0x800) out.push(0xc0 | c >> 6, 0x80 | c & 63);
        else if (c < 0x10000) out.push(0xe0 | c >> 12, 0x80 | c >> 6 & 63, 0x80 | c & 63);
        else out.push(0xf0 | c >> 18, 0x80 | c >> 12 & 63, 0x80 | c >> 6 & 63, 0x80 | c & 63);
      }
      return new Uint8Array(out);
    }
  };
  globalThis.TextDecoder = class TextDecoder {
    get encoding() { return 'utf-8'; }
    decode(input = new Uint8Array(0)) {
      const b = input instanceof Uint8Array ? input : new Uint8Array(input.buffer || input);
      let out = '';
      for (let i = 0; i < b.length;) {
        const c = b[i++];
        let n = 0, cp = 0xfffd;
        if (c < 0x80) cp = c;
        else if (c >= 0xc2 && c < 0xe0) { n = 1; cp = c & 31; }
        else if (c >= 0xe0 && c < 0xf0) { n = 2; cp = c & 15; }
        else if (c >= 0xf0 && c < 0xf5) { n = 3; cp = c & 7; }
        for (let k = 0; k < n; ++k) {
          if (i >= b.length || (b[i] & 0xc0) !== 0x80) { cp = 0xfffd; n = 0; break; }
          cp = cp << 6 | b[i++] & 63;
        }
        out += String.fromCodePoint(cp > 0x10ffff ? 0xfffd : cp);
      }
      return out;
    }
  };

  // Base64 of a binary string, as the web's btoa() and atob() have it.
  const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  globalThis.btoa = (text) => {
    const s = String(text);
    let out = '';
    for (let i = 0; i < s.length; i += 3) {
      const [a, b, c] = [s.charCodeAt(i), s.charCodeAt(i + 1), s.charCodeAt(i + 2)];
      if (a > 255 || b > 255 || c > 255) throw new Error('btoa: a character outside Latin-1');
      const n = a << 16 | (b || 0) << 8 | (c || 0);
      out += B64[n >> 18] + B64[n >> 12 & 63] +
          (i + 1 < s.length ? B64[n >> 6 & 63] : '=') + (i + 2 < s.length ? B64[n & 63] : '=');
    }
    return out;
  };
  globalThis.atob = (text) => {
    const s = String(text).replace(/[\s=]/g, '');
    let out = '';
    for (let i = 0; i < s.length; i += 4) {
      const n = [0, 1, 2, 3].reduce((v, k) => v << 6 | Math.max(0, B64.indexOf(s[i + k] || 'A')), 0);
      out += String.fromCharCode(n >> 16 & 255);
      if (i + 2 < s.length) out += String.fromCharCode(n >> 8 & 255);
      if (i + 3 < s.length) out += String.fromCharCode(n & 255);
    }
    return out;
  };

  const call = (module, method, args) => native.call(module, method, args);
  const api = (module, methods) => Object.freeze(Object.fromEntries(
      methods.map((m) => [m, (...args) => call(module, m, args)])));
  const ALLOW = {app: 1, fetch: 2, files: 4, compress: 8, crypto: 16, image: 32, url: 64, pdf: 128, all: 255};

  const VIEW_ADOPT = Symbol();
  // A browser context of its own: cookies, storage and cache apart
  // (zebkit_context_new); close() closes its views.
  const contextIds = new WeakMap();
  class Context {
    constructor() { contextIds.set(this, native.more('context-new')); }
    close() {
      const id = contextIds.get(this);
      if (id) native.more('context-close', id);
      contextIds.set(this, 0);
    }
  }
  const contextId = (context) => {
    const id = contextIds.get(context);
    if (!id) throw new Error('the context is closed');
    return id;
  };
  // Views by their number, for what the engine hands back (automation's pages).
  const viewIds = new WeakMap();
  const views = new Map();
  // Events whose engine callback is set the first time the script listens.
  const LAZY_EVENTS = new Set(['new-window', 'drop', 'dialog', 'dialog-closed', 'drag-start', 'drag-end', 'frame',
                               'caret', 'cursor', 'popup', 'authentication', 'automation']);
  const MOUSE = {move: 0, down: 1, up: 2, leave: 3};
  const BUTTON = {none: 0, left: 1, middle: 2, right: 3, back: 4, forward: 5};
  const WHEEL_PHASE = {none: 0, began: 1, changed: 2, ended: 3, momentum: 4};
  const DRAG = Object.freeze({none: 0, copy: 1, link: 2, move: 16, all: 19});
  const MODIFIERS = Object.freeze({shift: 1, control: 2, alt: 4, meta: 8, capsLock: 16, numLock: 32,
                                   leftButton: 64, middleButton: 128, rightButton: 256, keypad: 512, autoRepeat: 1024});
  // An off-screen frame: the engine's until released, which happens after
  // the 'frame' handlers return unless one calls keep().
  const makeFrame = (info, id) => {
    let done = false;
    return Object.assign(info, {
      kept: false,
      keep() { this.kept = true; return this; },
      // Shared memory's pixels (stride * height bytes), else null.
      pixels() { return done ? null : native.more('frame', 'pixels', id); },
      iosurfaceMachPort() { return done ? 0 : native.more('frame', 'mach-port', id); },
      release() {
        if (done) return;
        done = true;
        native.more('frame', 'release', id);
      },
    });
  };
  class View {
    #id;
    #listening = new Set();
    #handlers = {load: [], title: [], close: [], 'popup-blocked': [], 'permission-result': [], 'navigation-result': [], 'window-open': [], console: [], 'certificate-result': [], 'http-auth-result': [], 'window-state': [],
                 'new-window': [], drop: [], dialog: [], 'dialog-closed': [], 'drag-start': [], 'drag-end': [], frame: [], caret: [], cursor: [], popup: [], authentication: [], automation: []};
    // {window: {...}}: <zebkit/window.h>'s options, before the window is made;
    // {context}: a Context of its own.
    constructor(url = 'about:blank', {width = 0, height = 0, window = null, context = null} = {}, adoption = null) {
      const options = window === null ? undefined
          : JSON.stringify({...window, ...(width ? {width} : {}), ...(height ? {height} : {})});
      this.#id = adoption?.key === VIEW_ADOPT ? adoption.id
          : context ? native.more('view-new-in', contextId(context), String(url), width, height, options)
          : native.viewNew(String(url), width, height, options);
      viewIds.set(this, this.#id);
      views.set(this.#id, this);
      native.viewOn(this.#id, 'close', () => views.delete(this.#id));
      native.viewOn(this.#id, 'load', (event, url) => this.#emit('load', event, url));
      native.viewOn(this.#id, 'title', (title) => this.#emit('title', title));
      native.viewOn(this.#id, 'close', () => this.#emit('close'));
      native.viewOn(this.#id, 'popup-blocked', (event) => this.#emit('popup-blocked', event));
      native.viewOn(this.#id, 'permission-result', event => this.#emit('permission-result', event));
      native.viewOn(this.#id, 'navigation-result', event => this.#emit('navigation-result', event));
      native.viewOn(this.#id, 'window-open', event => this.#emit('window-open', event));
      native.viewOn(this.#id, 'console', event => this.#emit('console', event));
      native.viewOn(this.#id, 'window-state', event => this.#emit('window-state', event));
      native.viewOn(this.#id, 'certificate-result', event => this.#emit('certificate-result', event));
      native.viewOn(this.#id, 'http-auth-result', event => this.#emit('http-auth-result', event));
    }
    #emit(name, ...args) { for (const fn of this.#handlers[name]) fn(...args); }
    on(name, fn) {
      if (!this.#handlers[name]) throw new Error(`a view has no event "${name}"`);
      this.#handlers[name].push(fn);
      if (LAZY_EVENTS.has(name)) this.#listen(name);
      return this;
    }
    #listen(name) {
      if (this.#listening.has(name)) return;
      this.#listening.add(name);
      if (name === 'frame') {
        native.viewOn(this.#id, 'frame', (info, id) => {
          const frame = makeFrame(info, id);
          try { this.#emit('frame', frame); } finally { if (!frame.kept) frame.release(); }
        });
      } else {
        native.viewOn(this.#id, name, (...args) => this.#emit(name, ...args));
      }
      native.viewMore(this.#id, 'listen', name);
    }
    // A view without a window, its frames to the 'frame' event; null on
    // Windows without --headless.
    static offscreen(url = 'about:blank', {width = 800, height = 600, scale = 1, context = null} = {}) {
      const id = native.more('offscreen-new', context ? contextId(context) : 0, String(url), width, height, scale);
      return id === null ? null : new View('', {}, {key: VIEW_ADOPT, id});
    }
    #more(kind, ...args) { return native.viewMore(this.#id, kind, ...args); }
    // The view as a window of its own: limits, place, visibility, title.
    get targetId() { return this.#more('target-id'); }
    setSizeLimits(minWidth, minHeight, resizable = true) { this.#more('size-limits', minWidth, minHeight, !!resizable); }
    attach(parent, x, y, width, height) { this.#more('attach', parent.#id, x, y, width, height); }
    setBounds(x, y, width, height) { this.#more('bounds', x, y, width, height); }
    setVisible(visible) { this.#more('visible', !!visible); }
    // A window of the application's: an X11 window id, an NSView*, an HWND,
    // as a number or a decimal string.
    attachNative(handle, x, y, width, height) { return this.#more('attach-native', typeof handle === 'bigint' ? String(handle) : handle, x, y, width, height); }
    detachNative() { return this.#more('detach-native'); }
    setTitle(title) { this.#more('title', String(title)); }
    addScript(source) { this.#more('add-script', String(source)); }
    stop() { this.#more('stop'); }
    get isLoading() { return this.#more('is-loading'); }
    setUserAgent(userAgent) { this.#more('user-agent', userAgent == null ? null : String(userAgent)); }
    // A PNG of what the view shows.
    capture() { return this.#more('capture'); }
    setNavigationFailClosed(failClosed) { return this.#more('navigation-fail-closed', !!failClosed); }
    get popupPolicy() { return this.#more('popup-policy'); }
    // <zebkit/page.h>: dialogs ('dialog', 'dialog-closed'), closing, printing, drag and drop.
    setDialogPolicy(policy) { return this.#more('dialog-policy', String(policy)); }
    get dialogPolicy() { return this.#more('dialog-policy'); }
    respondDialog(id, accept = true, promptText = null) { return this.#more('dialog-respond', Number(id), !!accept, promptText == null ? null : String(promptText)); }
    get dialogs() { return this.#more('dialogs'); }
    requestClose() { this.#more('request-close'); }
    printToPdf(options = null) { return this.#more('print-to-pdf', options === null ? null : JSON.stringify(options)); }
    get isDragging() { return this.#more('is-dragging'); }
    dragEnter(data, x, y, operations = DRAG.all, modifiers = 0) { this.#more('drag-enter', JSON.stringify(data), x, y, operations, modifiers); }
    dragOver(x, y, operations = DRAG.all, modifiers = 0) { return this.#more('drag-over', x, y, operations, modifiers); }
    dragLeave() { this.#more('drag-leave'); }
    drop(x, y, modifiers = 0) { return this.#more('drop', x, y, modifiers); }
    dragSourceEnd(x, y, operation) { this.#more('drag-source-end', x, y, operation); }
    // <zebkit/offscreen.h>: size, frames ('frame'), input; 'caret', 'cursor', 'popup'.
    get isOffscreen() { return this.#more('is-offscreen'); }
    setSize(width, height, scale = 0) { this.#more('set-size', width, height, scale); }
    setFrameRate(fps) { this.#more('frame-rate', fps); }
    setMaxFramesInFlight(count) { this.#more('max-frames', count); }
    setFrameBuffers(buffers) { this.#more('frame-buffers', String(buffers)); }
    requestFrame() { this.#more('request-frame'); }
    sendMouse(type, x, y, {button = 'none', clickCount = 1, modifiers = 0} = {}) {
      this.#more('mouse', MOUSE[type] ?? 0, x, y, BUTTON[button] ?? 0, clickCount, modifiers);
    }
    sendWheel(x, y, deltaX, deltaY, {precise = false, phase = 'none', modifiers = 0} = {}) {
      this.#more('wheel', x, y, deltaX, deltaY, !!precise, WHEEL_PHASE[phase] ?? 0, modifiers);
    }
    // {type: 'down' | 'up' | 'char', modifiers, keyCode, nativeKeyCode, code, key, text, unmodifiedText}
    sendKey(event) { this.#more('key', event); }
    sendKeyWithCommands(event, commands) { return this.#more('key-commands', event, commands); }
    imeSetComposition(text, selectionStart, selectionEnd) { this.#more('ime-composition', String(text), selectionStart, selectionEnd); }
    imeCommit(text) { this.#more('ime-commit', String(text)); }
    imeCancel() { this.#more('ime-cancel'); }
    setFocus(focused) { this.#more('focus-state', !!focused); }
    setAuthenticationUi(enabled) { return this.#more('authentication-ui', !!enabled); }
    // WebAuthn's requests ('authentication' events), answered with actions.
    authenticationAction(action) { return this.#more('authentication-action', JSON.stringify(action)); }
    // <zebkit/media.h>: what the page plays, saved as it plays, or tapped.
    mediaList() { return this.#more('media-list'); }
    mediaAt(x, y) { return this.#more('media-at', x, y); }
    mediaCapture(mediaId, path, options = {}, {onProgress = null, onDone = null} = {}) {
      const id = native.more('media-capture', this.#id, String(mediaId), String(path), JSON.stringify(options), onProgress, onDone);
      return Object.freeze({
        id,
        stop: () => native.more('capture-stop', id),
        get status() { return native.more('capture-status', id); },
        free: () => native.more('capture-free', id),
      });
    }
    mediaTap(mediaId, options, fn) {
      const id = native.more('media-tap', this.#id, String(mediaId), options == null ? null : JSON.stringify(options), fn);
      return Object.freeze({id, stop: () => native.more('tap-stop', id), free: () => native.more('tap-free', id)});
    }
    load(url) { native.viewLoad(this.#id, String(url)); }
    reload() { native.viewReload(this.#id); }
    back() { native.viewBack(this.#id); }
    forward() { native.viewForward(this.#id); }
    resize(width, height) { native.viewResize(this.#id, width, height); }
    // Focus to the view, as when a person clicks it: a page goes fullscreen
    // only from a view with focus (zebkit_view_focus).
    focus() { native.viewFocus(this.#id); }
    // The window (<zebkit/window.h>): its state, runtime options, actions and
    // drag regions; 'window-state' events tell its changes.
    get window() { return native.viewWindow(this.#id, 'get'); }
    setWindow(options) { return native.viewWindow(this.#id, 'set', JSON.stringify(options)); }
    windowAction(action) { return native.viewWindow(this.#id, 'action', String(action)); }
    setDragRegions(regions) { return native.viewWindow(this.#id, 'regions', JSON.stringify(regions)); }
    setPopupWindowOptions(options) {
      return native.viewWindow(this.#id, 'popup', options === null ? null : JSON.stringify(options));
    }
    setSettings(settings) { return native.viewSettings(this.#id, JSON.stringify(settings)); }
    get settings() { return JSON.parse(native.viewGetSettings(this.#id)); }
    setRuntimeSettings(settings) { return native.viewRuntimeSettings(this.#id, JSON.stringify(settings)); }
    get runtimeSettings() { return JSON.parse(native.viewGetRuntimeSettings(this.#id)); }
    setPermission(permission, state, origin = null) {
      return native.viewPermission(this.#id, 'set', origin, permission, state);
    }
    getPermission(permission, origin = null) {
      return native.viewPermission(this.#id, 'get', origin, permission);
    }
    resetPermissions(origin = null) { return native.viewPermission(this.#id, 'reset', origin); }
    setPermissionTimeout(milliseconds) { return native.viewPermission(this.#id, 'timeout', milliseconds); }
    onPermission(fn) {
      if (fn !== null && typeof fn !== 'function') throw new TypeError('permission handler must be a function or null');
      return native.viewPermission(this.#id, 'handler', fn === null ? null : metadata => fn(Object.freeze({
        metadata: Object.freeze(metadata),
        get info() { return native.permissionRequest('info', metadata.id); },
        respond(state, remember = false) { return native.permissionRequest('respond', metadata.id, state, remember); }
      })));
    }
    setCertificateTimeout(milliseconds) { return native.viewCertificate(this.#id, 'timeout', milliseconds); }
    onCertificate(fn) {
      if (fn !== null && typeof fn !== 'function') throw new TypeError('certificate handler must be a function or null');
      return native.viewCertificate(this.#id, 'handler', fn === null ? null : metadata => {
        const id = metadata.id;
        fn(Object.freeze({metadata: Object.freeze(metadata),
          get info() { return native.certificateRequest('info', id); },
          respond(allow = false) { return native.certificateRequest('respond', id, allow); }
        }));
      });
    }
    // Hooks of <zebkit/hooks.h>: an asked hook's function gets a request with
    // metadata, info and respond(...); a told hook's function gets the event.
    #asked(kind, fn, responder, more = () => ({})) {
      if (fn !== null && typeof fn !== 'function') throw new TypeError(`${kind} handler must be a function or null`);
      return native.viewHook(this.#id, kind, fn === null ? null : metadata => {
        const id = metadata.id;
        fn(Object.freeze({metadata: Object.freeze(metadata),
          get info() { return native.hookRequest(kind, 'info', id); },
          respond(...args) { return responder(id, ...args); },
          ...more(id)
        }));
      });
    }
    #writables = null;
    #networkStream(handle) {
      const view = this;
      return Object.freeze({
        write(bytes) { return native.networkStream('write', handle, bytes); },
        onWritable(fn) {
          if (typeof fn !== 'function') throw new TypeError('a writable handler must be a function');
          if (!view.#writables) {
            view.#writables = new Map();
            native.viewHook(view.#id, 'network-stream-writable', id => { const call = view.#writables.get(id); if (call) call(); });
          }
          view.#writables.set(handle, fn);
          return native.viewHookCall(view.#id, 'network-stream-writable', handle);
        },
        get isOpen() { return native.networkStream('open', handle); },
        end(failed = false) {
          if (view.#writables) view.#writables.delete(handle);
          return native.networkStream('end', handle, !!failed);
        }
      });
    }
    #told(kind, fn) {
      if (fn !== null && typeof fn !== 'function') throw new TypeError(`${kind} observer must be a function or null`);
      return native.viewHook(this.#id, kind, fn);
    }
    setNetworkFilter(rules) { return native.viewHookCall(this.#id, 'network-filter', JSON.stringify(rules)); }
    setNetworkFilterForWorkers(on) { return native.viewHookCall(this.#id, 'network-filter-workers', !!on); }
    onNetwork(fn) {
      return this.#asked('network', fn, (id, reply, body = null) => native.hookRequest('network', 'respond', id, JSON.stringify(reply), body), id => ({
        respondFile: (path, reply = {}) => native.hookRequest('network', 'respond-file', id, JSON.stringify(reply), String(path)),
        respondStream: (reply = {}) => {
          const handle = native.hookRequest('network', 'respond-stream', id, JSON.stringify(reply));
          return handle === null ? null : this.#networkStream(handle);
        }
      }));
    }
    onNetworkBody(fn) {
      if (fn !== null && typeof fn !== 'function') throw new TypeError('network-body observer must be a function or null');
      return native.viewHook(this.#id, 'network-body', fn);
    }
    onNetworkEvent(fn) { return this.#told('network-event', fn); }
    setNetworkTimeout(ms) { return native.viewHookTimeout(this.#id, 'network', ms); }
    onExternalProtocol(fn) { return this.#told('external-protocol', fn); }
    onDownload(fn) { return this.#asked('download', fn, (id, action, path = null) => native.hookRequest('download', 'respond', id, String(action), path)); }
    onDownloadProgress(fn) { return this.#told('download-progress', fn); }
    setDownloadTimeout(ms) { return native.viewHookTimeout(this.#id, 'download', ms); }
    cancelDownload(id) { return native.viewHookCall(this.#id, 'cancel-download', String(id)); }
    onClientCertificate(fn) { return this.#asked('client-certificate', fn, (id, action, certificate = null, key = null) => native.hookRequest('client-certificate', 'respond', id, String(action), certificate, key)); }
    onClientCertificateResult(fn) { return this.#told('client-certificate-result', fn); }
    setClientCertificateTimeout(ms) { return native.viewHookTimeout(this.#id, 'client-certificate', ms); }
    addWorldScript(source, {world = '', mainFrameOnly = false} = {}) { return native.viewHookCall(this.#id, 'world-script', String(source), String(world), !!mainFrameOnly); }
    onFileChooser(fn) { return this.#asked('file-chooser', fn, (id, paths = []) => native.hookRequest('file-chooser', 'respond', id, paths)); }
    onFileChooserResult(fn) { return this.#told('file-chooser-result', fn); }
    setFileChooserTimeout(ms) { return native.viewHookTimeout(this.#id, 'file-chooser', ms); }
    onMediaAccess(fn) { return this.#asked('media-access', fn, (id, allow = false, audioDevice = null, videoDevice = null) => native.hookRequest('media-access', 'respond', id, !!allow, audioDevice, videoDevice)); }
    onMediaAccessResult(fn) { return this.#told('media-access-result', fn); }
    setMediaAccessTimeout(ms) { return native.viewHookTimeout(this.#id, 'media-access', ms); }
    onEyeDropper(fn) { return this.#asked('eye-dropper', fn, (id, color = null) => native.hookRequest('eye-dropper', 'respond', id, color)); }
    onEyeDropperResult(fn) { return this.#told('eye-dropper-result', fn); }
    setEyeDropperTimeout(ms) { return native.viewHookTimeout(this.#id, 'eye-dropper', ms); }
    onShare(fn) { return this.#asked('share', fn, (id, shared = false) => native.hookRequest('share', 'respond', id, !!shared)); }
    onShareResult(fn) { return this.#told('share-result', fn); }
    setShareTimeout(ms) { return native.viewHookTimeout(this.#id, 'share', ms); }
    onDeviceChooser(fn) { return this.#asked('device-chooser', fn, (id, deviceId = null) => native.hookRequest('device-chooser', 'respond', id, deviceId)); }
    onDeviceChooserResult(fn) { return this.#told('device-chooser-result', fn); }
    setDeviceChooserTimeout(ms) { return native.viewHookTimeout(this.#id, 'device-chooser', ms); }
    contextMenuRun(menuId, command) { return native.viewHookCall(this.#id, 'context-menu-run', String(menuId), String(command)); }
    contextMenuShow(menuId, items = null, fn = null) {
      if (fn !== null && typeof fn !== 'function') throw new TypeError('a context menu function must be a function or null');
      return native.viewHookCall(this.#id, 'context-menu-show', String(menuId), items === null ? null : JSON.stringify(items), fn);
    }
    setReservedKeys(keys) { return native.viewHookCall(this.#id, 'reserved-keys', JSON.stringify(keys)); }
    onReservedKey(fn) { return this.#told('reserved-key', fn); }
    onContextMenu(fn) { return this.#told('context-menu', fn); }
    setFullscreenPolicy(policy) { return native.viewHookCall(this.#id, 'fullscreen-policy', String(policy)); }
    onFullscreen(fn) { return this.#told('fullscreen', fn); }
    setHttpAuthTimeout(milliseconds) { return native.viewHttpAuth(this.#id, 'timeout', milliseconds); }
    onHttpAuth(fn) {
      if (fn !== null && typeof fn !== 'function') throw new TypeError('HTTP authentication handler must be a function or null');
      return native.viewHttpAuth(this.#id, 'handler', fn === null ? null : metadata => {
        const id = metadata.id;
        fn(Object.freeze({metadata: Object.freeze(metadata),
          get info() { return native.httpAuthRequest('info', id); },
          respond(username = null, password = null) { return native.httpAuthRequest('respond', id, username, password); }
        }));
      });
    }
    setNavigationTimeout(milliseconds) { return native.viewNavigation(this.#id, 'timeout', milliseconds); }
    onNavigation(fn) {
      if (fn !== null && typeof fn !== 'function') throw new TypeError('navigation handler must be a function or null');
      return native.viewNavigation(this.#id, 'handler', fn === null ? null : metadata => {
        const id = metadata.id;
        fn(Object.freeze({metadata: Object.freeze(metadata),
          get info() { return native.navigationRequest('info', id); },
          respond(action = 'continue', replacementUrl = null) { return native.navigationRequest('respond', id, action, replacementUrl); }
        }));
      });
    }
    setPopupPolicy(policy) { return native.viewPopupPolicy(this.#id, String(policy)); }
    close() { native.viewClose(this.#id); }
    get url() { return native.viewUrl(this.#id); }
    get title() { return native.viewTitle(this.#id); }
    get windowOpenInfo() { return JSON.parse(native.viewWindowOpenInfo(this.#id)); }
    get nativeWindow() { return native.viewNativeWindow(this.#id); }
    exportNativeWindow() { return native.viewExportNativeWindow(this.#id); }
    evaluate(script) { return native.viewEvaluate(this.#id, String(script)); }
    setUserAgentOverride(options) { return native.viewUserAgentOverride(this.#id, JSON.stringify(options)); }
    grant(origin, allow, filesRoot) {
      const names = Array.isArray(allow) ? allow : [allow];
      let bits = 0;
      for (const name of names) {
        if (!(name in ALLOW)) throw new Error(`unknown capability "${name}"`);
        bits |= ALLOW[name];
      }
      native.viewGrant(this.#id, String(origin), bits, filesRoot ? String(filesRoot) : '');
    }
    bind(name, fn) { native.viewBind(this.#id, String(name), fn); }
  }

  native.setViewFactory(id => new View('', {}, {key: VIEW_ADOPT, id}));
  const viewFor = (id) => views.get(id) ?? new View('', {}, {key: VIEW_ADOPT, id});
  const viewId = (view) => {
    const id = viewIds.get(view);
    if (!id) throw new TypeError('a View');
    return id;
  };
  // A request to one of the application's schemes, answered once.
  const makeRequest = (raw) => Object.freeze({
    url: raw.url, method: raw.method, headers: raw.headers, body: raw.body,
    respond: (status = 200, headers = null, body = new Uint8Array(0)) =>
        native.more('request', 'respond', raw.id, status, headers == null ? null : String(headers), body),
    respondFile: (path, headers = null) =>
        native.more('request', 'file', raw.id, String(path), headers == null ? null : String(headers)),
  });

  const pdf = (__ZEBKIT_PDF__)(call);
  globalThis.zebkit = Object.freeze({
    ui: native.ui,
    version: native.version,
    engineVersion: native.engineVersion,
    args: Object.freeze([...native.args]),
    quit: () => native.quit(),
    // What this build can do on this system and display (<zebkit/window.h>).
    capabilities: () => native.capabilities(),
    // What the system says of how things should look; its changes to fn.
    environment: () => native.environment(),
    onEnvironmentChanged: (fn) => native.onEnvironmentChanged(typeof fn === 'function' ? fn : null),
    // The system's services for the application (docs/SYSTEM_INTEGRATION.md, 12 to 14).
    displays: () => native.system('displays'),
    onDisplaysChanged: (fn) => native.onSystem('displays', typeof fn === 'function' ? fn : null),
    onPowerEvent: (fn) => native.onSystem('power', typeof fn === 'function' ? fn : null),
    preventSleep: (reason, keepDisplayOn = false) => native.system('prevent-sleep', String(reason), !!keepDisplayOn),
    allowSleep: (token) => native.system('allow-sleep', String(token)),
    openExternal: (url) => native.system('open-external', String(url)),
    showInFolder: (path) => native.system('show-in-folder', String(path)),
    onNotification: (fn) => native.onSystem('notification', typeof fn === 'function' ? fn : null),
    notificationClicked: (id, action = -1) => native.system('notification-clicked', String(id), Number(action)),
    notificationClosed: (id, byUser = true) => native.system('notification-closed', String(id), !!byUser),
    app: Object.freeze({
      setMenu: (menu, fn) => native.system('set-menu', JSON.stringify(menu), fn),
      updateMenuItem: (id, state) => native.system('update-menu-item', String(id), JSON.stringify(state)),
      menu: () => native.system('menu'),
      setBadge: (text) => native.system('set-badge', text == null ? '' : String(text)),
      setProgress: (progress) => native.system('set-progress', progress == null ? -1 : Number(progress)),
      requestAttention: (critical = false) => native.system('request-attention', !!critical),
      setDockMenu: (items, fn) => native.system('set-dock-menu', JSON.stringify(items), fn),
      // Files the system asks the running application to open (macOS),
      // the Dock's reopen, and the person's quit (fn decides: zebkit.quit()).
      onOpenFiles: (fn) => native.more('app-open-files', typeof fn === 'function' ? fn : null),
      onReopen: (fn) => native.more('app-reopen', typeof fn === 'function' ? fn : null),
      onQuitRequest: (fn) => native.more('app-quit-request', typeof fn === 'function' ? fn : null),
    }),
    isHeadless: () => native.more('is-headless'),
    Context,
    DRAG,
    MODIFIERS,
    // CDP, as Chrome serves it (zebkit_cdp_listen): the port, 0 for any.
    cdp: Object.freeze({
      listen: (address = '127.0.0.1', port = 0) => native.more('cdp-listen', address == null ? null : String(address), port),
      get port() { return native.more('cdp-port'); },
      get url() { return native.more('cdp-url'); },
      stop: () => native.more('cdp-stop'),
    }),
    // The application's URL schemes: registered with the switch
    // --zebkit-scheme=NAME,standard,secure,fetch,bypass-csp (before the
    // engine starts); handled here, each request answered once.
    scheme: Object.freeze({
      handle: (name, fn) => native.more('scheme-handle', String(name), typeof fn === 'function' ? (raw) => fn(makeRequest(raw)) : null),
    }),
    // Playwright's API inside the engine (<zebkit/automation.h>); a view's
    // events are its 'automation' event.
    automation: Object.freeze({
      call: (view, method, params = {}) => native.more('automation-call', viewId(view), String(method), JSON.stringify(params)),
      screenshotPixels: (view, method = 'Page.screenshot', params = {}) =>
          native.more('automation-pixels', viewId(view), String(method), JSON.stringify(params)),
      page: (pageId) => {
        const id = native.more('automation-page', String(pageId));
        return id === null ? null : viewFor(id);
      },
      pageId: (view) => native.more('automation-page-id', viewId(view)),
      listen: (port = 0) => native.more('automation-listen', port),
      commands: () => native.more('automation-commands'),
    }),
    // FFmpeg inside the engine, on a worker thread (<zebkit/media_tools.h>):
    // a progress function that returns false cancels ffmpeg.
    media: Object.freeze({
      keepHistory: (keep) => native.more('media-keep-history', !!keep),
      ffprobe: (args) => native.more('ffprobe', [...args].map(String)),
      probe: (path) => native.more('media-probe', String(path)),
      ffmpeg: (args, onProgress = null) => native.more('ffmpeg', [...args].map(String), typeof onProgress === 'function' ? onProgress : null),
    }),
    // A tray icon: a PNG's bytes and a tooltip; its menu takes app.setMenu()'s items.
    Tray: class Tray {
      #id;
      constructor(png, tooltip = '') { this.#id = native.system('tray-new', png, String(tooltip)); }
      setIcon(png) { return native.system('tray-icon', this.#id, png); }
      setTooltip(text) { return native.system('tray-tooltip', this.#id, String(text)); }
      setMenu(items, fn) { return native.system('tray-menu', this.#id, JSON.stringify(items), fn); }
      onClick(fn) { return native.system('tray-click', this.#id, typeof fn === 'function' ? fn : null); }
      close() { return native.system('tray-close', this.#id); }
    },
    View,
    setEngineSettings: settings => native.setEngineSettings(JSON.stringify(settings)),
    get engineSettings() { return JSON.parse(native.engineSettings()); },
    fetch: (url, options) => call('fetch', 'fetch', [String(url), options || {}]),
    fs: api('fs', ['readFile', 'readText', 'writeFile', 'readDir', 'stat', 'mkdir', 'remove']),
    compress: (format, bytes, level) => call('compress', 'compress', [format, bytes, level ?? -1]),
    decompress: (format, bytes) => call('compress', 'decompress', [format, bytes]),
    crypto: api('crypto', ['digest', 'hmac', 'randomBytes', 'aesGcmSeal', 'aesGcmOpen']),
    image: api('image', ['decode', 'encode']),
    url: api('url', ['parse', 'registrableDomain']),
    pdf,
    canvas: pdf.canvas,
    text: pdf.text,
  });
}))js";

// The bootstrap with zebkit.pdf's wrapper in it (pdf_script.h).
std::string Bootstrap() {
  std::string source = kBootstrap;
  const std::string marker = "__ZEBKIT_PDF__";
  source.replace(source.find(marker), marker.size(), zebkit::kPdfScript);
  return source;
}

constexpr int kMaxDepth = 64;

v8::Local<v8::String> String8(v8::Isolate* isolate, std::string_view text) {
  return v8::String::NewFromUtf8(isolate, text.data(),
                                 v8::NewStringType::kNormal,
                                 static_cast<int>(text.size()))
      .ToLocalChecked();
}

std::string Utf8(v8::Isolate* isolate, v8::Local<v8::Value> value) {
  v8::String::Utf8Value text(isolate, value);
  return *text ? std::string(*text, text.length()) : std::string();
}

// base::Value from a script value: bytes for typed arrays and buffers.
base::Value ToValue(v8::Isolate* isolate,
                    v8::Local<v8::Context> context,
                    v8::Local<v8::Value> value,
                    int depth = 0) {
  if (depth > kMaxDepth || value.IsEmpty() || value->IsUndefined() ||
      value->IsNull() || value->IsFunction() || value->IsSymbol()) {
    return base::Value();
  }
  if (value->IsBoolean())
    return base::Value(value->BooleanValue(isolate));
  if (value->IsInt32())
    return base::Value(value.As<v8::Int32>()->Value());
  if (value->IsNumber())
    return base::Value(value.As<v8::Number>()->Value());
  if (value->IsString())
    return base::Value(Utf8(isolate, value));
  if (value->IsArrayBufferView()) {
    v8::Local<v8::ArrayBufferView> view = value.As<v8::ArrayBufferView>();
    base::Value::BlobStorage bytes(view->ByteLength());
    view->CopyContents(bytes.data(), bytes.size());
    return base::Value(std::move(bytes));
  }
  if (value->IsArrayBuffer()) {
    v8::Local<v8::ArrayBuffer> buffer = value.As<v8::ArrayBuffer>();
    const uint8_t* data = static_cast<const uint8_t*>(buffer->Data());
    return base::Value(
        base::Value::BlobStorage(data, data + buffer->ByteLength()));
  }
  if (value->IsArray()) {
    v8::Local<v8::Array> array = value.As<v8::Array>();
    base::ListValue list;
    for (uint32_t i = 0; i < array->Length(); ++i) {
      v8::Local<v8::Value> item;
      if (array->Get(context, i).ToLocal(&item))
        list.Append(ToValue(isolate, context, item, depth + 1));
    }
    return base::Value(std::move(list));
  }
  if (value->IsObject()) {
    v8::Local<v8::Object> object = value.As<v8::Object>();
    v8::Local<v8::Array> keys;
    if (!object->GetOwnPropertyNames(context).ToLocal(&keys))
      return base::Value();
    base::DictValue dict;
    for (uint32_t i = 0; i < keys->Length(); ++i) {
      v8::Local<v8::Value> key, item;
      if (!keys->Get(context, i).ToLocal(&key) ||
          !object->Get(context, key).ToLocal(&item) || item->IsUndefined()) {
        continue;
      }
      dict.Set(Utf8(isolate, key), ToValue(isolate, context, item, depth + 1));
    }
    return base::Value(std::move(dict));
  }
  return base::Value(Utf8(isolate, value));
}

// A script value from a base::Value: Uint8Arrays for bytes.
v8::Local<v8::Value> FromValue(v8::Isolate* isolate,
                               v8::Local<v8::Context> context,
                               const base::Value& value) {
  switch (value.type()) {
    case base::Value::Type::NONE:
      return v8::Null(isolate);
    case base::Value::Type::BOOLEAN:
      return v8::Boolean::New(isolate, value.GetBool());
    case base::Value::Type::INTEGER:
      return v8::Integer::New(isolate, value.GetInt());
    case base::Value::Type::DOUBLE:
      return v8::Number::New(isolate, value.GetDouble());
    case base::Value::Type::STRING:
      return String8(isolate, value.GetString());
    case base::Value::Type::BINARY: {
      const base::Value::BlobStorage& blob = value.GetBlob();
      v8::Local<v8::ArrayBuffer> buffer =
          v8::ArrayBuffer::New(isolate, blob.size());
      if (!blob.empty())
        memcpy(buffer->Data(), blob.data(), blob.size());
      return v8::Uint8Array::New(buffer, 0, blob.size());
    }
    case base::Value::Type::LIST: {
      v8::Local<v8::Array> array =
          v8::Array::New(isolate, static_cast<int>(value.GetList().size()));
      for (size_t i = 0; i < value.GetList().size(); ++i) {
        std::ignore = array->Set(context, static_cast<uint32_t>(i),
                                 FromValue(isolate, context, value.GetList()[i]));
      }
      return array;
    }
    case base::Value::Type::DICT: {
      v8::Local<v8::Object> object = v8::Object::New(isolate);
      for (auto [key, item] : value.GetDict()) {
        std::ignore = object->Set(context, String8(isolate, key),
                                  FromValue(isolate, context, item));
      }
      return object;
    }
  }
  return v8::Null(isolate);
}

// What was thrown, with its stack when there is one.
std::string Describe(v8::Isolate* isolate,
                     v8::Local<v8::Context> context,
                     v8::Local<v8::Value> exception) {
  if (exception.IsEmpty())
    return "exception";
  if (exception->IsObject()) {
    v8::Local<v8::Value> stack;
    if (exception.As<v8::Object>()
            ->Get(context, String8(isolate, "stack"))
            .ToLocal(&stack) &&
        stack->IsString()) {
      return Utf8(isolate, stack);
    }
  }
  return Utf8(isolate, exception);
}

}  // namespace

struct zebkit_script {
  explicit zebkit_script(std::vector<std::string> args);
  ~zebkit_script();

  v8::Isolate* isolate() { return holder_->isolate(); }
  v8::Local<v8::Context> context() { return context_.Get(isolate()); }

  void RunModule(const std::string& path, zebkit_result_func func, void* data);
  void Evaluate(const std::string& source, zebkit_result_func func, void* data);
  void OnIdle(zebkit_task_func func, void* data) {
    idle_func_ = func;
    idle_data_ = data;
    CheckIdle();
  }

 private:
  // Enters the isolate and the context for a call into script. Leaving
  // the outermost one runs the promise jobs the call queued: a promise
  // settled from C++ continues the script then, not at some later call.
  struct Scope {
    explicit Scope(zebkit_script* script)
        : script(script),
          isolate_scope(script->isolate()),
          handles(script->isolate()),
          context_scope(script->context()) {
      ++script->depth_;
    }
    ~Scope() {
      if (--script->depth_ == 0)
        script->isolate()->PerformMicrotaskCheckpoint();
    }
    raw_ptr<zebkit_script> script;
    v8::Isolate::Scope isolate_scope;
    v8::HandleScope handles;
    v8::Context::Scope context_scope;
  };

  struct Timer {
    v8::Global<v8::Function> function;
    bool repeat = false;
    int ms = 0;
  };

  struct ViewEntry {
    zebkit_view* view = nullptr;
    void* hook = nullptr;
    v8::Global<v8::Object> wrapper;
    std::map<std::string, v8::Global<v8::Function>> handlers;
    std::vector<std::unique_ptr<std::pair<base::WeakPtr<zebkit_script>,
                                          v8::Global<v8::Function>>>>
        bindings;
  };

  // A promise the script waits for, settled from C++.
  struct Pending {
    v8::Global<v8::Promise::Resolver> resolver;
  };

  static zebkit_script* From(const v8::FunctionCallbackInfo<v8::Value>& info) {
    return static_cast<zebkit_script*>(info.Data().As<v8::External>()->Value(
        v8::kExternalPointerTypeTagDefault));
  }

  void Install();
  void SetMethod(v8::Local<v8::Object> object,
                 const char* name,
                 v8::FunctionCallback callback);

  // Something the script waits for: a timer, a call, a view.
  void Hold() { ++holds_; }
  void Release() {
    --holds_;
    CheckIdle();
  }
  void CheckIdle();

  int NewPromise(v8::Local<v8::Promise>* promise);
  void Settle(int id, bool ok, base::Value value);
  void SettleJson(int id, const char* json, const char* error);

  void CallFunction(v8::Local<v8::Function> function,
                    std::vector<v8::Local<v8::Value>> args);
  void Report(v8::TryCatch& try_catch);

  v8::MaybeLocal<v8::Module> LoadModule(const base::FilePath& path);
  static v8::MaybeLocal<v8::Module> Resolve(
      v8::Local<v8::Context> context,
      v8::Local<v8::String> specifier,
      v8::Local<v8::FixedArray> attributes,
      v8::Local<v8::Module> referrer);
  void Finish(zebkit_result_func func,
              void* data,
              v8::Local<v8::Value> result,
              bool as_module);
  static void Settled(const v8::FunctionCallbackInfo<v8::Value>& info,
                      bool ok);

  // Native functions, as zebkit.* sees them.
  static void Call(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void Print(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void SetTimer(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void ClearTimer(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void Quit(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void SetEngineSettings(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void GetEngineSettings(const v8::FunctionCallbackInfo<v8::Value>& info);
  int WrapView(zebkit_view* view);
  static void SetViewFactory(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void ViewNew(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void ViewWindow(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void Capabilities(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void Environment(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void OnEnvironmentChanged(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void System(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void OnSystem(const v8::FunctionCallbackInfo<v8::Value>& info);
  // A system event or a chosen menu item to the script's function for @kind.
  static void DeliverSystem(void* data, const std::string& kind, const char* text, bool json);
  v8::Global<v8::Function> environment_function_;
  // onSystem()'s functions, and the menus' and trays' ("app-menu", "tray-menu:<id>", ...).
  std::map<std::string, v8::Global<v8::Function>> system_functions_;
  std::map<uint64_t, zebkit_tray*> trays_;
  uint64_t next_tray_ = 1;
  // The rest of the C API (native.more, native.viewMore): contexts, schemes'
  // requests, off-screen frames, media captures and taps, by number.
  static void More(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void ViewMore(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void ViewListen(zebkit_script* self, ViewEntry* entry, const std::string& name);
  // A callback's target that outlives nothing: the script, and what it is for.
  struct Target {
    base::WeakPtr<zebkit_script> script;
    std::string key;
    int id = 0;
  };
  Target* NewTarget(const std::string& key, int id = 0);
  void Deliver(const std::string& key, std::vector<v8::Local<v8::Value>> args);
  std::map<int, zebkit_context*> contexts_;
  int next_context_ = 1;
  std::map<int, zebkit_request*> requests_;
  int next_request_ = 1;
  std::map<int, const zebkit_frame*> frames_;
  int next_frame_ = 1;
  std::map<int, zebkit_media_capture*> captures_;
  std::map<int, zebkit_media_tap*> taps_;
  int next_media_ = 1;
  std::vector<std::string> schemes_;
  std::vector<std::unique_ptr<Target>> targets_;
  static void ViewOn(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void ViewPermission(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void PermissionRequest(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void ViewCertificate(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void CertificateRequest(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void ViewHttpAuth(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void HttpAuthRequest(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void DeliverHook(void* data, const char* name, const char* json);
  static void ViewHookFunction(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void ViewHookTimeout(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void ViewHookCall(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void HookRequest(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void NetworkStreamCall(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void ViewNavigation(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void NavigationRequest(const v8::FunctionCallbackInfo<v8::Value>& info);
  enum class Action { kLoad, kReload, kBack, kForward, kResize, kFocus, kClose, kUrl, kTitle, kPopupPolicy, kSettings, kGetSettings, kRuntimeSettings, kGetRuntimeSettings, kWindowOpenInfo };
  template <Action action>
  static void ViewAction(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void ViewEvaluate(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void ViewUserAgentOverride(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void ViewNativeWindow(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void ViewExportNativeWindow(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void ViewGrant(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void ViewBind(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void UiCommit(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void UiDump(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void UiAct(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void UiListen(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void UiCapture(const v8::FunctionCallbackInfo<v8::Value>& info);
  zebkit::UiTree* Ui();

  ViewEntry* FindView(const v8::FunctionCallbackInfo<v8::Value>& info);
  void FireTimer(int id);
  void ViewEvent(int id, const std::string& name,
                 std::vector<v8::Local<v8::Value>> args);
  void ViewClosed(int id);

  std::unique_ptr<gin::IsolateHolder> holder_;
  v8::Global<v8::Context> context_;
  v8::Global<v8::Function> view_factory_;
  std::vector<std::string> args_;

  std::map<int, v8::Global<v8::Module>> modules_by_hash_;
  std::map<std::string, v8::Global<v8::Module>> modules_;
  std::map<int, std::string> module_paths_;

  std::map<int, Timer> timers_;
  int next_timer_ = 1;
  std::map<int, Pending> pending_;
  int next_pending_ = 1;
  std::map<int, ViewEntry> views_;
  int next_view_ = 1;

  int holds_ = 0;
  int depth_ = 0;
  // zebkit.ui's views, made when first used, and the function it hands
  // their events to.
  std::unique_ptr<zebkit::UiTree> ui_;
  v8::Global<v8::Function> ui_listener_;
  bool started_ = false;
  bool evaluating_ = false;
  zebkit_task_func idle_func_ = nullptr;
  void* idle_data_ = nullptr;
  bool idle_posted_ = false;

  base::WeakPtrFactory<zebkit_script> weak_factory_{this};
};

zebkit_script::zebkit_script(std::vector<std::string> args)
    : args_(std::move(args)) {
  static bool initialized = false;
  if (!initialized) {
    gin::IsolateHolder::Initialize(gin::IsolateHolder::kStrictMode,
                                   gin::ArrayBufferAllocator::SharedInstance());
    initialized = true;
  }
  holder_ = std::make_unique<gin::IsolateHolder>(
      base::SingleThreadTaskRunner::GetCurrentDefault(),
      gin::IsolateHolder::kSingleThread,
      gin::IsolateHolder::IsolateType::kUtility);
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handles(isolate());
  // Promise jobs run when an entry from C++ into script ends (Scope).
  isolate()->SetMicrotasksPolicy(v8::MicrotasksPolicy::kExplicit);
  isolate()->SetPromiseRejectCallback([](v8::PromiseRejectMessage message) {
    if (message.GetEvent() != v8::kPromiseRejectWithNoHandler)
      return;
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    v8::Local<v8::Context> context = isolate->GetCurrentContext();
    fprintf(stderr, "Uncaught (in promise) %s\n",
            Describe(isolate, context, message.GetValue()).c_str());
  });
  v8::Local<v8::Context> context = v8::Context::New(isolate());
  context->SetAlignedPointerInEmbedderData(1, this,
                                           v8::kEmbedderDataTypeTagDefault);
  context_.Reset(isolate(), context);
  Install();
}

zebkit_script::~zebkit_script() {
  // The engine's function for the environment holds this script.
  if (!environment_function_.IsEmpty())
    zebkit_on_environment_changed(nullptr, nullptr);
  // What the system would tell this script, and its trays.
  if (system_functions_.contains("power")) zebkit_on_power_event(nullptr, nullptr);
  if (system_functions_.contains("displays")) zebkit_on_displays_changed(nullptr, nullptr);
  if (system_functions_.contains("notification")) zebkit_on_notification(nullptr, nullptr);
  if (system_functions_.contains("app-menu")) zebkit_app_set_menu("[]", nullptr, nullptr, nullptr);
  if (system_functions_.contains("dock-menu")) zebkit_app_set_dock_menu("[]", nullptr, nullptr, nullptr);
  for (const auto& [id, tray] : trays_) zebkit_tray_close(tray);
  trays_.clear();
  // What the rest of the C API holds for it: media it captures and taps,
  // frames it keeps, its schemes and the application's events.
  for (const auto& [id, capture] : captures_) {
    zebkit_media_capture_stop(capture);
    zebkit_media_capture_free(capture);
  }
  captures_.clear();
  for (const auto& [id, tap] : taps_) {
    zebkit_media_tap_stop(tap);
    zebkit_media_tap_free(tap);
  }
  taps_.clear();
  for (const auto& [id, frame] : frames_) zebkit_frame_release(frame);
  frames_.clear();
  for (const std::string& scheme : schemes_) zebkit_scheme_handle(scheme.c_str(), nullptr, nullptr);
  if (system_functions_.contains("app-open-files")) zebkit_app_on_open_files(nullptr, nullptr);
  if (system_functions_.contains("app-reopen")) zebkit_app_on_reopen(nullptr, nullptr);
  if (system_functions_.contains("app-quit-request")) zebkit_app_on_quit_request(nullptr, nullptr);
  // Its views stay, and must not wait on a handler that is gone: a pending
  // navigation would stall until its deadline. A setter can close a view.
  std::vector<base::WeakPtr<zebkit_view>> kept;
  for (const auto& [id, entry] : views_)
    if (entry.view) kept.push_back(entry.view->GetWeakPtr());
  weak_factory_.InvalidateWeakPtrs();
  for (const auto& view : kept) {
    if (view) zebkit_view_on_navigation(view.get(), nullptr, nullptr);
    if (view) zebkit_view_on_permission(view.get(), nullptr, nullptr);
    if (view) zebkit_view_on_certificate(view.get(), nullptr, nullptr);
    if (view) zebkit_view_on_http_auth(view.get(), nullptr, nullptr);
    if (view) zebkit_view_on_network(view.get(), nullptr, nullptr);
    if (view) zebkit_view_on_network_body(view.get(), nullptr, nullptr);
    if (view) zebkit_view_on_download(view.get(), nullptr, nullptr);
    if (view) zebkit_view_on_client_certificate(view.get(), nullptr, nullptr);
    if (view) zebkit_view_on_file_chooser(view.get(), nullptr, nullptr);
    if (view) zebkit_view_on_media_access(view.get(), nullptr, nullptr);
    if (view) zebkit_view_on_context_menu(view.get(), nullptr, nullptr);
  }
  ui_.reset();
  {
    v8::Isolate::Scope isolate_scope(isolate());
    v8::HandleScope handles(isolate());
    timers_.clear();
    pending_.clear();
    views_.clear();
    modules_.clear();
    modules_by_hash_.clear();
    // Every handle goes while the isolate is there: members destroyed after
    // this would release theirs into an isolate that is gone.
    ui_listener_.Reset();
    view_factory_.Reset();
    system_functions_.clear();
    context_.Reset();
  }
  holder_.reset();
}

void zebkit_script::SetMethod(v8::Local<v8::Object> object,
                              const char* name,
                              v8::FunctionCallback callback) {
  v8::Local<v8::External> self = v8::External::New(
      isolate(), this, v8::kExternalPointerTypeTagDefault);
  v8::Local<v8::Function> function =
      v8::Function::New(context(), callback, self).ToLocalChecked();
  std::ignore = object->Set(context(), String8(isolate(), name), function);
}

void zebkit_script::Install() {
  v8::Local<v8::Context> context = this->context();
  v8::Context::Scope context_scope(context);
  v8::Local<v8::Object> native = v8::Object::New(isolate());
  SetMethod(native, "call", &Call);
  SetMethod(native, "print", &Print);
  SetMethod(native, "setTimer", &SetTimer);
  SetMethod(native, "clearTimer", &ClearTimer);
  SetMethod(native, "quit", &Quit);
  SetMethod(native, "viewNew", &ViewNew);
  SetMethod(native, "viewWindow", &ViewWindow);
  SetMethod(native, "capabilities", &Capabilities);
  SetMethod(native, "environment", &Environment);
  SetMethod(native, "onEnvironmentChanged", &OnEnvironmentChanged);
  SetMethod(native, "system", &System);
  SetMethod(native, "onSystem", &OnSystem);
  SetMethod(native, "more", &More);
  SetMethod(native, "viewMore", &ViewMore);
  SetMethod(native, "setViewFactory", &SetViewFactory);
  SetMethod(native, "viewWindowOpenInfo", &ViewAction<Action::kWindowOpenInfo>);
  SetMethod(native, "viewNativeWindow", &ViewNativeWindow);
  SetMethod(native, "viewExportNativeWindow", &ViewExportNativeWindow);
  SetMethod(native, "viewOn", &ViewOn);
  SetMethod(native, "viewPermission", &ViewPermission);
  SetMethod(native, "permissionRequest", &PermissionRequest);
  SetMethod(native, "viewNavigation", &ViewNavigation);
  SetMethod(native, "viewCertificate", &ViewCertificate);
  SetMethod(native, "certificateRequest", &CertificateRequest);
  SetMethod(native, "viewHttpAuth", &ViewHttpAuth);
  SetMethod(native, "httpAuthRequest", &HttpAuthRequest);
  SetMethod(native, "viewHook", &ViewHookFunction);
  SetMethod(native, "viewHookTimeout", &ViewHookTimeout);
  SetMethod(native, "viewHookCall", &ViewHookCall);
  SetMethod(native, "hookRequest", &HookRequest);
  SetMethod(native, "networkStream", &NetworkStreamCall);
  SetMethod(native, "navigationRequest", &NavigationRequest);
  SetMethod(native, "viewEvaluate", &ViewEvaluate);
  SetMethod(native, "viewUserAgentOverride", &ViewUserAgentOverride);
  SetMethod(native, "viewGrant", &ViewGrant);
  SetMethod(native, "viewBind", &ViewBind);
  SetMethod(native, "uiCommit", &UiCommit);
  SetMethod(native, "uiDump", &UiDump);
  SetMethod(native, "uiAct", &UiAct);
  SetMethod(native, "uiListen", &UiListen);
  SetMethod(native, "uiCapture", &UiCapture);
  SetMethod(native, "viewLoad", &ViewAction<Action::kLoad>);
  SetMethod(native, "viewReload", &ViewAction<Action::kReload>);
  SetMethod(native, "viewBack", &ViewAction<Action::kBack>);
  SetMethod(native, "viewForward", &ViewAction<Action::kForward>);
  SetMethod(native, "viewResize", &ViewAction<Action::kResize>);
  SetMethod(native, "viewFocus", &ViewAction<Action::kFocus>);
  SetMethod(native, "viewPopupPolicy", &ViewAction<Action::kPopupPolicy>);
  SetMethod(native, "viewSettings", &ViewAction<Action::kSettings>);
  SetMethod(native, "viewGetSettings", &ViewAction<Action::kGetSettings>);
  SetMethod(native, "viewRuntimeSettings", &ViewAction<Action::kRuntimeSettings>);
  SetMethod(native, "viewGetRuntimeSettings", &ViewAction<Action::kGetRuntimeSettings>);
  SetMethod(native, "setEngineSettings", &SetEngineSettings);
  SetMethod(native, "engineSettings", &GetEngineSettings);
  SetMethod(native, "viewClose", &ViewAction<Action::kClose>);
  SetMethod(native, "viewUrl", &ViewAction<Action::kUrl>);
  SetMethod(native, "viewTitle", &ViewAction<Action::kTitle>);
  v8::Local<v8::Array> args =
      v8::Array::New(isolate(), static_cast<int>(args_.size()));
  for (size_t i = 0; i < args_.size(); ++i)
    std::ignore = args->Set(context, i, String8(isolate(), args_[i]));
  std::ignore = native->Set(context, String8(isolate(), "args"), args);
  std::ignore = native->Set(context, String8(isolate(), "version"),
                            String8(isolate(), zebkit_version()));
  std::ignore = native->Set(context, String8(isolate(), "engineVersion"),
                            String8(isolate(), zebkit_engine_version()));

  v8::TryCatch try_catch(isolate());
  // zebkit.ui first: the bootstrap puts it in zebkit.
  v8::Local<v8::Value> make_ui =
      v8::Script::Compile(context, String8(isolate(), zebkit::kUiScript))
          .ToLocalChecked()
          ->Run(context)
          .ToLocalChecked();
  v8::Local<v8::Value> ui_argv[] = {native};
  v8::Local<v8::Value> ui;
  if (!make_ui.As<v8::Function>()
           ->Call(context, v8::Undefined(isolate()), 1, ui_argv)
           .ToLocal(&ui)) {
    Report(try_catch);
    return;
  }
  std::ignore = native->Set(context, String8(isolate(), "ui"), ui);
  v8::Local<v8::Script> script =
      v8::Script::Compile(context, String8(isolate(), Bootstrap()))
          .ToLocalChecked();
  v8::Local<v8::Value> bootstrap = script->Run(context).ToLocalChecked();
  v8::Local<v8::Value> argv[] = {native};
  if (bootstrap.As<v8::Function>()
          ->Call(context, v8::Undefined(isolate()), 1, argv)
          .IsEmpty()) {
    Report(try_catch);
  }
}

void zebkit_script::CheckIdle() {
  if (!started_ || evaluating_ || holds_ > 0 || !views_.empty() ||
      (ui_ && ui_->open_windows()) || !idle_func_ || idle_posted_) {
    return;
  }
  // Not from inside script: the application may destroy the script.
  idle_posted_ = true;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<zebkit_script> script) {
                       if (!script)
                         return;
                       script->idle_posted_ = false;
                       if (script->holds_ == 0 && script->views_.empty() &&
                           !(script->ui_ && script->ui_->open_windows()) &&
                           !script->evaluating_ && script->idle_func_) {
                         script->idle_func_(script->idle_data_);
                       }
                     },
                     weak_factory_.GetWeakPtr()));
}

int zebkit_script::NewPromise(v8::Local<v8::Promise>* promise) {
  v8::Local<v8::Promise::Resolver> resolver =
      v8::Promise::Resolver::New(context()).ToLocalChecked();
  *promise = resolver->GetPromise();
  const int id = next_pending_++;
  pending_[id].resolver.Reset(isolate(), resolver);
  Hold();
  return id;
}

void zebkit_script::Settle(int id, bool ok, base::Value value) {
  auto it = pending_.find(id);
  if (it == pending_.end())
    return;
  {
    Scope scope(this);
    v8::Local<v8::Promise::Resolver> resolver = it->second.resolver.Get(isolate());
    pending_.erase(it);
    if (ok) {
      std::ignore =
          resolver->Resolve(context(), FromValue(isolate(), context(), value));
    } else {
      std::ignore = resolver->Reject(
          context(), v8::Exception::Error(String8(
                         isolate(), value.is_string() ? value.GetString()
                                                      : std::string("error"))));
    }
  }
  Release();
}

void zebkit_script::SettleJson(int id, const char* json, const char* error) {
  auto it = pending_.find(id);
  if (it == pending_.end())
    return;
  {
    Scope scope(this);
    v8::Local<v8::Promise::Resolver> resolver = it->second.resolver.Get(isolate());
    pending_.erase(it);
    v8::Local<v8::Value> value;
    if (error) {
      std::ignore = resolver->Reject(
          context(), v8::Exception::Error(String8(isolate(), error)));
    } else if (v8::JSON::Parse(context(), String8(isolate(), json ? json : "null"))
                   .ToLocal(&value)) {
      std::ignore = resolver->Resolve(context(), value);
    } else {
      std::ignore = resolver->Resolve(context(), v8::Null(isolate()));
    }
  }
  Release();
}

void zebkit_script::Report(v8::TryCatch& try_catch) {
  fprintf(stderr, "Uncaught %s\n",
          Describe(isolate(), context(), try_catch.Exception()).c_str());
}

void zebkit_script::CallFunction(v8::Local<v8::Function> function,
                                 std::vector<v8::Local<v8::Value>> args) {
  v8::TryCatch try_catch(isolate());
  if (function
          ->Call(context(), v8::Undefined(isolate()),
                 static_cast<int>(args.size()), args.data())
          .IsEmpty()) {
    Report(try_catch);
  }
}

// ---- Native functions ----

void zebkit_script::Call(const v8::FunctionCallbackInfo<v8::Value>& info) {
  zebkit_script* self = From(info);
  v8::Isolate* isolate = info.GetIsolate();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Promise> promise;
  const int id = self->NewPromise(&promise);
  info.GetReturnValue().Set(promise);
  zebkit::PageCall call;
  call.module = Utf8(isolate, info[0]);
  call.method = Utf8(isolate, info[1]);
  base::Value args = ToValue(isolate, context, info[2]);
  if (args.is_list())
    call.args = std::move(args.GetList());
  call.origin = "the application";
  call.allow = ZEBKIT_ALLOW_ALL & ~ZEBKIT_ALLOW_APP;
  call.files_root = zebkit::PathFromUTF8("/");
  call.reply = base::BindOnce(
      [](base::WeakPtr<zebkit_script> script, int id, bool ok,
         base::Value value) {
        if (script)
          script->Settle(id, ok, std::move(value));
      },
      self->weak_factory_.GetWeakPtr(), id);
  zebkit::HandlePageCall(std::move(call));
}

void zebkit_script::Print(const v8::FunctionCallbackInfo<v8::Value>& info) {
  const std::string text = Utf8(info.GetIsolate(), info[1]);
  FILE* out = info[0]->IsInt32() && info[0].As<v8::Int32>()->Value() ? stderr
                                                                     : stdout;
  fprintf(out, "%s\n", text.c_str());
  fflush(out);
}

void zebkit_script::SetTimer(const v8::FunctionCallbackInfo<v8::Value>& info) {
  zebkit_script* self = From(info);
  if (!info[0]->IsFunction())
    return;
  const int id = self->next_timer_++;
  Timer& timer = self->timers_[id];
  timer.function.Reset(info.GetIsolate(), info[0].As<v8::Function>());
  timer.repeat = info[2]->BooleanValue(info.GetIsolate());
  timer.ms = std::max(0, static_cast<int>(info[1].As<v8::Number>()->Value()));
  self->Hold();
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&zebkit_script::FireTimer,
                     self->weak_factory_.GetWeakPtr(), id),
      base::Milliseconds(timer.ms));
  info.GetReturnValue().Set(id);
}

void zebkit_script::FireTimer(int id) {
  auto it = timers_.find(id);
  if (it == timers_.end())
    return;
  {
    Scope scope(this);
    v8::Local<v8::Function> function = it->second.function.Get(isolate());
    const bool repeat = it->second.repeat;
    const int ms = it->second.ms;
    if (repeat) {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE,
          base::BindOnce(&zebkit_script::FireTimer, weak_factory_.GetWeakPtr(),
                         id),
          base::Milliseconds(std::max(ms, 1)));
    } else {
      timers_.erase(it);
    }
    CallFunction(function, {});
    // A repeating timer is released by clearInterval(), here or later.
    if (!repeat) {
      Release();
      return;
    }
  }
}

void zebkit_script::ClearTimer(const v8::FunctionCallbackInfo<v8::Value>& info) {
  zebkit_script* self = From(info);
  if (!info[0]->IsInt32())
    return;
  if (self->timers_.erase(info[0].As<v8::Int32>()->Value()))
    self->Release();
}

void zebkit_script::Quit(const v8::FunctionCallbackInfo<v8::Value>& info) {
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce([] { zebkit_quit(); }));
}

void zebkit_script::SetEngineSettings(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  char* error = nullptr;
  const bool ok = zebkit_set_engine_settings(Utf8(isolate, info[0]).c_str(), &error);
  if (!ok) isolate->ThrowException(v8::Exception::TypeError(String8(isolate, error ? error : "runtime settings rejected")));
  zebkit_free(error);
  info.GetReturnValue().Set(ok);
}

void zebkit_script::GetEngineSettings(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  char* json = zebkit_get_engine_settings();
  info.GetReturnValue().Set(String8(isolate, json ? json : "{}"));
  zebkit_free(json);
}

// ---- Views ----

zebkit_script::ViewEntry* zebkit_script::FindView(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (!info[0]->IsInt32())
    return nullptr;
  auto it = views_.find(info[0].As<v8::Int32>()->Value());
  return it == views_.end() ? nullptr : &it->second;
}

namespace {

struct ViewHook {
  base::WeakPtr<zebkit_script> script;
  int id;
};

}  // namespace

void zebkit_script::ViewNew(const v8::FunctionCallbackInfo<v8::Value>& info) {
  zebkit_script* self = From(info);
  v8::Isolate* isolate = info.GetIsolate();
  const std::string url = Utf8(isolate, info[0]);
  const int width = info[1]->IsNumber() ? info[1].As<v8::Number>()->Value() : 0;
  const int height = info[2]->IsNumber() ? info[2].As<v8::Number>()->Value() : 0;
  zebkit_view* view = nullptr;
  // new zebkit.View(url, {window: {...}}): <zebkit/window.h>'s options.
  if (info[3]->IsString()) {
    char* error = nullptr;
    view = zebkit_view_new_with_options(nullptr, url.c_str(), Utf8(isolate, info[3]).c_str(), &error);
    if (!view) {
      isolate->ThrowException(v8::Exception::TypeError(String8(isolate, error ? error : "window options rejected")));
      zebkit_free(error);
      return;
    }
  } else {
    view = zebkit_view_new(url.c_str(), width, height);
  }
  if (!view) {
    isolate->ThrowException(
        v8::Exception::Error(String8(isolate, "cannot create a view")));
    return;
  }
  info.GetReturnValue().Set(self->WrapView(view));
}

// native.viewWindow(id, op, argument): <zebkit/window.h> for a view.
void zebkit_script::ViewWindow(const v8::FunctionCallbackInfo<v8::Value>& info) {
  zebkit_script* self = From(info);
  v8::Isolate* isolate = info.GetIsolate();
  ViewEntry* entry = self->FindView(info);
  if (!entry)
    return;
  zebkit_view* view = entry->view;
  const std::string op = Utf8(isolate, info[1]);
  char* error = nullptr;
  bool ok = true;
  if (op == "get") {
    char* json = zebkit_view_get_window(view);
    v8::Local<v8::Value> value = v8::Null(isolate);
    if (json) {
      v8::Local<v8::Value> parsed;
      if (v8::JSON::Parse(isolate->GetCurrentContext(), String8(isolate, json)).ToLocal(&parsed)) value = parsed;
    }
    zebkit_free(json);
    info.GetReturnValue().Set(value);
    return;
  } else if (op == "set") {
    ok = zebkit_view_set_window(view, Utf8(isolate, info[2]).c_str(), &error);
  } else if (op == "action") {
    ok = zebkit_view_window_action(view, Utf8(isolate, info[2]).c_str(), &error);
  } else if (op == "regions") {
    ok = zebkit_view_set_drag_regions(view, Utf8(isolate, info[2]).c_str(), &error);
  } else if (op == "popup") {
    ok = info[2]->IsString()
             ? zebkit_view_set_popup_window_options(view, Utf8(isolate, info[2]).c_str(), &error)
             : zebkit_view_set_popup_window_options(view, nullptr, &error);
  } else {
    ok = false;
    error = strdup("unknown window operation");
  }
  if (!ok)
    isolate->ThrowException(v8::Exception::TypeError(String8(isolate, error ? error : "window call refused")));
  zebkit_free(error);
  info.GetReturnValue().Set(ok);
}

void zebkit_script::Environment(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  char* json = zebkit_get_environment();
  v8::Local<v8::Value> value = v8::Null(isolate);
  if (json) {
    v8::Local<v8::Value> parsed;
    if (v8::JSON::Parse(isolate->GetCurrentContext(), String8(isolate, json)).ToLocal(&parsed)) value = parsed;
  }
  zebkit_free(json);
  info.GetReturnValue().Set(value);
}

void zebkit_script::OnEnvironmentChanged(const v8::FunctionCallbackInfo<v8::Value>& info) {
  zebkit_script* self = From(info);
  if (!info[0]->IsFunction()) {
    self->environment_function_.Reset();
    zebkit_on_environment_changed(nullptr, nullptr);
    return;
  }
  self->environment_function_.Reset(info.GetIsolate(), info[0].As<v8::Function>());
  zebkit_on_environment_changed(
      [](const char* json, void* data) {
        auto* script = static_cast<zebkit_script*>(data);
        if (script->environment_function_.IsEmpty()) return;
        Scope scope(script);
        v8::Local<v8::Value> value;
        if (v8::JSON::Parse(script->context(), String8(script->isolate(), json)).ToLocal(&value))
          script->CallFunction(script->environment_function_.Get(script->isolate()), {value});
      },
      self);
}

namespace {
void PermissionTypeError(v8::Isolate* isolate, const char* text);
}  // namespace

void zebkit_script::DeliverSystem(void* data, const std::string& kind, const char* text, bool json) {
  auto* script = static_cast<zebkit_script*>(data);
  auto it = script->system_functions_.find(kind);
  if (it == script->system_functions_.end() || it->second.IsEmpty()) return;
  Scope scope(script);
  auto* isolate = script->isolate();
  v8::Local<v8::Value> argument = String8(isolate, text ? text : "");
  if (json && !v8::JSON::Parse(script->context(), argument.As<v8::String>()).ToLocal(&argument))
    argument = v8::Null(isolate);
  script->CallFunction(it->second.Get(isolate), {argument});
}

namespace {

// A PNG's bytes from a Uint8Array or an ArrayBuffer.
bool Bytes(v8::Local<v8::Value> value, std::string* out) {
  if (value->IsArrayBufferView()) {
    auto view = value.As<v8::ArrayBufferView>();
    out->resize(view->ByteLength());
    view->CopyContents(out->data(), out->size());
    return true;
  }
  if (value->IsArrayBuffer()) {
    auto buffer = value.As<v8::ArrayBuffer>();
    out->assign(static_cast<const char*>(buffer->Data()), buffer->ByteLength());
    return true;
  }
  return false;
}

}  // namespace

void zebkit_script::OnSystem(const v8::FunctionCallbackInfo<v8::Value>& info) {
  zebkit_script* self = From(info);
  auto* isolate = info.GetIsolate();
  const std::string kind = Utf8(isolate, info[0]);
  const bool set = info[1]->IsFunction();
  if (set)
    self->system_functions_[kind].Reset(isolate, info[1].As<v8::Function>());
  else
    self->system_functions_.erase(kind);
  void* data = set ? self : nullptr;
  if (kind == "power") {
    zebkit_on_power_event(set ? +[](const char* json, void* data) { DeliverSystem(data, "power", json, true); }
                              : nullptr, data);
  } else if (kind == "displays") {
    zebkit_on_displays_changed(set ? +[](const char* json, void* data) { DeliverSystem(data, "displays", json, true); }
                                   : nullptr, data);
  } else if (kind == "notification") {
    zebkit_on_notification(set ? +[](const char* json, void* data) { DeliverSystem(data, "notification", json, true); }
                               : nullptr, data);
  } else {
    self->system_functions_.erase(kind);
    PermissionTypeError(isolate, "unknown system event");
    return;
  }
  info.GetReturnValue().Set(true);
}

void zebkit_script::System(const v8::FunctionCallbackInfo<v8::Value>& info) {
  zebkit_script* self = From(info);
  auto* isolate = info.GetIsolate();
  const std::string kind = Utf8(isolate, info[0]);
  char* error = nullptr;
  auto finish = [&](bool ok, const char* otherwise) {
    if (!ok) {
      PermissionTypeError(isolate, error ? error : otherwise);
      zebkit_free(error);
      return;
    }
    info.GetReturnValue().Set(true);
  };
  auto json = [&](char* text) {
    v8::Local<v8::Value> value = v8::Null(isolate);
    v8::Local<v8::Value> parsed;
    if (text && v8::JSON::Parse(isolate->GetCurrentContext(), String8(isolate, text)).ToLocal(&parsed)) value = parsed;
    zebkit_free(text);
    info.GetReturnValue().Set(value);
  };
  // A menu's function, kept as @key's; a chosen item's id reaches it.
  auto menu_function = [&](int index, const std::string& key) {
    if (info[index]->IsFunction())
      self->system_functions_[key].Reset(isolate, info[index].As<v8::Function>());
    else
      self->system_functions_.erase(key);
  };
  auto tray = [&]() -> zebkit_tray* {
    uint64_t id = info[1]->IsNumber() ? static_cast<uint64_t>(info[1].As<v8::Number>()->Value()) : 0;
    auto it = self->trays_.find(id);
    if (it == self->trays_.end()) { PermissionTypeError(isolate, "the tray is closed"); return nullptr; }
    return it->second;
  };
  if (kind == "displays") return json(zebkit_get_displays());
  if (kind == "menu") return json(zebkit_app_get_menu());
  if (kind == "prevent-sleep") {
    const uint64_t token = zebkit_prevent_sleep(info[2]->IsTrue(), Utf8(isolate, info[1]).c_str());
    if (!token) { PermissionTypeError(isolate, "the system was not kept awake"); return; }
    info.GetReturnValue().Set(String8(isolate, base::NumberToString(token).c_str()));
    return;
  }
  if (kind == "allow-sleep") {
    uint64_t token = 0;
    info.GetReturnValue().Set(base::StringToUint64(Utf8(isolate, info[1]), &token) && zebkit_allow_sleep(token));
    return;
  }
  if (kind == "open-external") return finish(zebkit_open_external(Utf8(isolate, info[1]).c_str(), &error), "not opened");
  if (kind == "show-in-folder") return finish(zebkit_show_in_folder(Utf8(isolate, info[1]).c_str(), &error), "not shown");
  if (kind == "notification-clicked") {
    const int action = info[2]->IsNumber() ? static_cast<int>(info[2].As<v8::Number>()->Value()) : -1;
    info.GetReturnValue().Set(zebkit_notification_clicked(Utf8(isolate, info[1]).c_str(), action));
    return;
  }
  if (kind == "notification-closed") {
    info.GetReturnValue().Set(zebkit_notification_closed(Utf8(isolate, info[1]).c_str(), info[2]->IsTrue()));
    return;
  }
  if (kind == "set-badge") { info.GetReturnValue().Set(zebkit_app_set_badge(Utf8(isolate, info[1]).c_str())); return; }
  if (kind == "set-progress") {
    info.GetReturnValue().Set(zebkit_app_set_progress(info[1]->IsNumber() ? info[1].As<v8::Number>()->Value() : -1));
    return;
  }
  if (kind == "request-attention") { zebkit_app_request_attention(info[1]->IsTrue()); info.GetReturnValue().Set(true); return; }
  if (kind == "set-menu") {
    menu_function(2, "app-menu");
    return finish(zebkit_app_set_menu(Utf8(isolate, info[1]).c_str(),
                                      [](const char* id, void* data) { DeliverSystem(data, "app-menu", id, false); },
                                      self, &error), "the menu was refused");
  }
  if (kind == "set-dock-menu") {
    menu_function(2, "dock-menu");
    return finish(zebkit_app_set_dock_menu(Utf8(isolate, info[1]).c_str(),
                                           [](const char* id, void* data) { DeliverSystem(data, "dock-menu", id, false); },
                                           self, &error), "the Dock's menu was refused");
  }
  if (kind == "update-menu-item")
    return finish(zebkit_app_update_menu_item(Utf8(isolate, info[1]).c_str(), Utf8(isolate, info[2]).c_str(), &error),
                  "the menu item was not changed");
  if (kind == "tray-new") {
    std::string png;
    if (!Bytes(info[1], &png)) { PermissionTypeError(isolate, "a tray's icon is a PNG's bytes"); return; }
    zebkit_tray* made = zebkit_tray_new(reinterpret_cast<const uint8_t*>(png.data()), png.size(),
                                        Utf8(isolate, info[2]).c_str(), &error);
    if (!made) return finish(false, "the tray was refused");
    const uint64_t id = self->next_tray_++;
    self->trays_[id] = made;
    info.GetReturnValue().Set(static_cast<double>(id));
    return;
  }
  if (kind == "tray-icon") {
    zebkit_tray* found = tray();
    std::string png;
    if (!found) return;
    if (!Bytes(info[2], &png)) { PermissionTypeError(isolate, "a tray's icon is a PNG's bytes"); return; }
    return finish(zebkit_tray_set_icon(found, reinterpret_cast<const uint8_t*>(png.data()), png.size(), &error),
                  "the icon was refused");
  }
  if (kind == "tray-tooltip") {
    zebkit_tray* found = tray();
    if (!found) return;
    zebkit_tray_set_tooltip(found, Utf8(isolate, info[2]).c_str());
    info.GetReturnValue().Set(true);
    return;
  }
  if (kind == "tray-menu" || kind == "tray-click") {
    zebkit_tray* found = tray();
    if (!found) return;
    // One function of each per tray, by the tray's number.
    const std::string key = kind + ":" + base::NumberToString(static_cast<uint64_t>(info[1].As<v8::Number>()->Value()));
    struct Target { base::WeakPtr<zebkit_script> script; std::string key; };
    static base::NoDestructor<std::map<std::string, Target>> targets;
    menu_function(kind == "tray-menu" ? 3 : 2, key);
    Target& target = (*targets)[key];
    target = {self->weak_factory_.GetWeakPtr(), key};
    if (kind == "tray-click") {
      zebkit_tray_on_click(found, [](void* data) {
        auto* target = static_cast<Target*>(data);
        if (target->script) DeliverSystem(target->script.get(), target->key, "", false);
      }, &target);
      info.GetReturnValue().Set(true);
      return;
    }
    return finish(zebkit_tray_set_menu(found, Utf8(isolate, info[2]).c_str(), [](const char* id, void* data) {
      auto* target = static_cast<Target*>(data);
      if (target->script) DeliverSystem(target->script.get(), target->key, id, false);
    }, &target, &error), "the menu was refused");
  }
  if (kind == "tray-close") {
    zebkit_tray* found = tray();
    if (!found) return;
    zebkit_tray_close(found);
    self->trays_.erase(static_cast<uint64_t>(info[1].As<v8::Number>()->Value()));
    info.GetReturnValue().Set(true);
    return;
  }
  PermissionTypeError(isolate, "unknown system call");
}

// ---- The rest of the C API: native.more() and native.viewMore() ----

namespace {

v8::Local<v8::Value> JsonValueOf(v8::Isolate* isolate, v8::Local<v8::Context> context, const char* json) {
  v8::Local<v8::Value> value;
  if (!json || !v8::JSON::Parse(context, String8(isolate, json)).ToLocal(&value))
    return v8::Null(isolate);
  return value;
}

v8::Local<v8::Value> BytesValue(v8::Isolate* isolate, const void* data, size_t size) {
  v8::Local<v8::ArrayBuffer> buffer = v8::ArrayBuffer::New(isolate, size);
  if (size && data)
    memcpy(buffer->Data(), data, size);
  return v8::Uint8Array::New(buffer, 0, size);
}

void SetField(v8::Isolate* isolate, v8::Local<v8::Object> object, const char* name, v8::Local<v8::Value> value) {
  std::ignore = object->Set(isolate->GetCurrentContext(), String8(isolate, name), value);
}

v8::Local<v8::Value> RectValue(v8::Isolate* isolate, const zebkit_rect* rect) {
  if (!rect)
    return v8::Null(isolate);
  v8::Local<v8::Object> object = v8::Object::New(isolate);
  SetField(isolate, object, "x", v8::Integer::New(isolate, rect->x));
  SetField(isolate, object, "y", v8::Integer::New(isolate, rect->y));
  SetField(isolate, object, "width", v8::Integer::New(isolate, rect->width));
  SetField(isolate, object, "height", v8::Integer::New(isolate, rect->height));
  return object;
}

// A promise of the script's, settled by a callback that may come after the
// script has gone.
struct PendingCall {
  base::WeakPtr<zebkit_script> script;
  int id;
};

// ffmpeg's progress and its cancelling, between a worker thread and the
// engine's.
struct MediaJob : base::RefCountedThreadSafe<MediaJob> {
  std::atomic<bool> cancel{false};
  scoped_refptr<base::SingleThreadTaskRunner> engine;
  base::RepeatingCallback<void(int64_t, int64_t)> progress;

 private:
  friend class base::RefCountedThreadSafe<MediaJob>;
  ~MediaJob() = default;
};

struct MediaResult {
  int code = 0;
  std::string json;
  std::string error;
  bool failed = false;
};

std::vector<std::string> StringList(v8::Isolate* isolate, v8::Local<v8::Value> value) {
  std::vector<std::string> out;
  if (!value->IsArray())
    return out;
  v8::Local<v8::Array> array = value.As<v8::Array>();
  for (uint32_t i = 0; i < array->Length(); ++i) {
    v8::Local<v8::Value> item;
    if (array->Get(isolate->GetCurrentContext(), i).ToLocal(&item))
      out.push_back(Utf8(isolate, item));
  }
  return out;
}

const char* const kDialogTypes[] = {"alert", "confirm", "prompt", "beforeunload", "print"};
const char* const kDialogPolicies[] = {"default", "hold", "accept", "dismiss"};
const char* const kPopupPolicies[] = {"block-without-gesture", "allow", "block"};

}  // namespace

zebkit_script::Target* zebkit_script::NewTarget(const std::string& key, int id) {
  targets_.push_back(std::make_unique<Target>(Target{weak_factory_.GetWeakPtr(), key, id}));
  return targets_.back().get();
}

void zebkit_script::Deliver(const std::string& key, std::vector<v8::Local<v8::Value>> args) {
  auto it = system_functions_.find(key);
  if (it == system_functions_.end() || it->second.IsEmpty())
    return;
  CallFunction(it->second.Get(isolate()), std::move(args));
}

void zebkit_script::More(const v8::FunctionCallbackInfo<v8::Value>& info) {
  zebkit_script* self = From(info);
  v8::Isolate* isolate = info.GetIsolate();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const std::string kind = Utf8(isolate, info[0]);
  auto number = [&](int i, double fallback = 0) {
    return info[i]->IsNumber() ? info[i].As<v8::Number>()->Value() : fallback;
  };
  auto text = [&](int i) { return Utf8(isolate, info[i]); };
  auto fail = [&](const char* message) {
    isolate->ThrowException(v8::Exception::Error(String8(isolate, message)));
  };
  auto take_error = [&](char* error, const char* otherwise) {
    fail(error ? error : otherwise);
    zebkit_free(error);
  };
  auto set_string = [&](char* owned) {
    if (!owned) {
      info.GetReturnValue().SetNull();
      return;
    }
    info.GetReturnValue().Set(String8(isolate, owned));
    zebkit_free(owned);
  };
  auto set_json = [&](char* owned) {
    info.GetReturnValue().Set(JsonValueOf(isolate, context, owned));
    zebkit_free(owned);
  };
  auto view_at = [&](int i) -> zebkit_view* {
    if (!info[i]->IsInt32())
      return nullptr;
    auto it = self->views_.find(info[i].As<v8::Int32>()->Value());
    return it == self->views_.end() ? nullptr : it->second.view;
  };
  auto function_at = [&](int i, const std::string& key) {
    if (info[i]->IsFunction())
      self->system_functions_[key].Reset(isolate, info[i].As<v8::Function>());
    else
      self->system_functions_.erase(key);
    return info[i]->IsFunction();
  };
  auto promise = [&]() {
    v8::Local<v8::Promise> made;
    const int id = self->NewPromise(&made);
    info.GetReturnValue().Set(made);
    return new PendingCall{self->weak_factory_.GetWeakPtr(), id};
  };

  if (kind == "is-headless") {
    info.GetReturnValue().Set(zebkit_is_headless());
  } else if (kind == "context-new") {
    zebkit_context* made = zebkit_context_new();
    if (!made)
      return fail("the context was not made");
    const int id = self->next_context_++;
    self->contexts_[id] = made;
    info.GetReturnValue().Set(id);
  } else if (kind == "context-close") {
    auto it = self->contexts_.find(static_cast<int>(number(1)));
    if (it != self->contexts_.end()) {
      zebkit_context_close(it->second);
      self->contexts_.erase(it);
    }
  } else if (kind == "view-new-in" || kind == "offscreen-new") {
    // (context, url, width, height, options or scale)
    zebkit_context* in = nullptr;
    if (number(1)) {
      auto it = self->contexts_.find(static_cast<int>(number(1)));
      if (it == self->contexts_.end())
        return fail("the context is closed");
      in = it->second;
    }
    const std::string url = text(2);
    const int width = static_cast<int>(number(3)), height = static_cast<int>(number(4));
    zebkit_view* view = nullptr;
    if (kind == "offscreen-new") {
      const float scale = static_cast<float>(number(5, 1));
      view = in ? zebkit_view_new_offscreen_in(in, url.c_str(), width, height, scale)
                : zebkit_view_new_offscreen(url.c_str(), width, height, scale);
      // Not on Windows without --headless.
      if (!view)
        return info.GetReturnValue().SetNull();
    } else if (info[5]->IsString()) {
      char* error = nullptr;
      view = zebkit_view_new_with_options(in, url.c_str(), text(5).c_str(), &error);
      if (!view)
        return take_error(error, "window options rejected");
    } else {
      view = zebkit_view_new_in(in, url.c_str(), width, height);
    }
    if (!view)
      return fail("cannot create a view");
    info.GetReturnValue().Set(self->WrapView(view));
  } else if (kind == "cdp-listen") {
    char* error = nullptr;
    const std::string address = text(1);
    const unsigned port = zebkit_cdp_listen(info[1]->IsString() ? address.c_str() : nullptr,
                                            static_cast<unsigned>(number(2)), &error);
    if (!port)
      return take_error(error, "CDP did not listen");
    info.GetReturnValue().Set(port);
  } else if (kind == "cdp-port") {
    info.GetReturnValue().Set(zebkit_cdp_get_port());
  } else if (kind == "cdp-url") {
    set_string(zebkit_cdp_get_url());
  } else if (kind == "cdp-stop") {
    zebkit_cdp_stop();
  } else if (kind == "scheme-handle") {
    const std::string scheme = text(1);
    const std::string key = "scheme:" + scheme;
    if (!function_at(2, key)) {
      zebkit_scheme_handle(scheme.c_str(), nullptr, nullptr);
      std::erase(self->schemes_, scheme);
      return;
    }
    Target* target = self->NewTarget(key);
    zebkit_scheme_handle(scheme.c_str(), [](zebkit_request* request, void* data) {
      auto* target = static_cast<Target*>(data);
      zebkit_script* script = target->script.get();
      if (!script || !script->system_functions_.contains(target->key)) {
        zebkit_request_respond(request, 404, nullptr, nullptr, 0);
        return;
      }
      Scope scope(script);
      v8::Isolate* isolate = script->isolate();
      const int id = script->next_request_++;
      script->requests_[id] = request;
      v8::Local<v8::Object> object = v8::Object::New(isolate);
      SetField(isolate, object, "id", v8::Integer::New(isolate, id));
      SetField(isolate, object, "url", String8(isolate, zebkit_request_url(request)));
      SetField(isolate, object, "method", String8(isolate, zebkit_request_method(request)));
      const char* headers = zebkit_request_headers(request);
      SetField(isolate, object, "headers", String8(isolate, headers ? headers : ""));
      size_t size = 0;
      const uint8_t* body = zebkit_request_body(request, &size);
      SetField(isolate, object, "body", BytesValue(isolate, body, size));
      script->Deliver(target->key, {object});
    }, target);
    if (!std::ranges::contains(self->schemes_, scheme))
      self->schemes_.push_back(scheme);
  } else if (kind == "request") {
    // (op, id, ...): answered once.
    auto it = self->requests_.find(static_cast<int>(number(2)));
    if (it == self->requests_.end())
      return info.GetReturnValue().Set(false);
    zebkit_request* request = it->second;
    self->requests_.erase(it);
    // ("respond", id, status, headers, body) or ("file", id, path, headers)
    const std::string headers = text(4);
    if (text(1) == "file") {
      zebkit_request_respond_file(request, text(3).c_str(), info[4]->IsString() ? headers.c_str() : nullptr);
    } else {
      base::Value body = ToValue(isolate, context, info[5]);
      std::string bytes;
      if (body.is_blob())
        bytes.assign(body.GetBlob().begin(), body.GetBlob().end());
      else if (body.is_string())
        bytes = body.GetString();
      zebkit_request_respond(request, static_cast<int>(number(3, 200)),
                             info[4]->IsString() ? headers.c_str() : nullptr, bytes.data(), bytes.size());
    }
    info.GetReturnValue().Set(true);
  } else if (kind == "automation-call" || kind == "automation-pixels") {
    zebkit_view* view = view_at(1);
    if (!view)
      return fail("the view is closed");
    PendingCall* pending = promise();
    const std::string method = text(2), params = info[3]->IsString() ? text(3) : "{}";
    if (kind == "automation-call") {
      zebkit_automation_call(view, method.c_str(), params.c_str(),
          [](zebkit_view*, const char* json, const char* error, void* data) {
            std::unique_ptr<PendingCall> pending(static_cast<PendingCall*>(data));
            if (pending->script)
              pending->script->SettleJson(pending->id, json, error);
          }, pending);
    } else {
      zebkit_automation_screenshot_pixels(view, method.c_str(), params.c_str(),
          [](zebkit_view*, const unsigned char* pixels, int width, int height, int stride, const char* error,
             void* data) {
            std::unique_ptr<PendingCall> pending(static_cast<PendingCall*>(data));
            if (!pending->script)
              return;
            if (error || !pixels) {
              pending->script->Settle(pending->id, false, base::Value(error ? error : "no pixels"));
              return;
            }
            base::DictValue image;
            image.Set("width", width);
            image.Set("height", height);
            image.Set("stride", stride);
            image.Set("data", base::Value(base::Value::BlobStorage(pixels, pixels + static_cast<size_t>(stride) * height)));
            pending->script->Settle(pending->id, true, base::Value(std::move(image)));
          }, pending);
    }
  } else if (kind == "automation-page") {
    zebkit_view* view = zebkit_automation_page(text(1).c_str());
    if (!view)
      return info.GetReturnValue().SetNull();
    info.GetReturnValue().Set(self->WrapView(view));
  } else if (kind == "automation-page-id") {
    zebkit_view* view = view_at(1);
    if (!view)
      return fail("the view is closed");
    set_string(zebkit_automation_page_id(view));
  } else if (kind == "automation-listen") {
    PendingCall* pending = promise();
    zebkit_automation_listen(static_cast<int>(number(1)), [](const char* url, const char* error, void* data) {
      std::unique_ptr<PendingCall> pending(static_cast<PendingCall*>(data));
      if (!pending->script)
        return;
      if (error || !url)
        pending->script->Settle(pending->id, false, base::Value(error ? error : "not listening"));
      else
        pending->script->Settle(pending->id, true, base::Value(url));
    }, pending);
  } else if (kind == "automation-commands") {
    set_json(zebkit_automation_commands());
  } else if (kind == "media-keep-history") {
    zebkit_media_keep_history(info[1]->IsTrue());
  } else if (kind == "ffprobe" || kind == "ffmpeg" || kind == "media-probe") {
    // On a worker thread: they can take long, and may run side by side.
    std::vector<std::string> args = kind == "media-probe" ? std::vector<std::string>{text(1)} : StringList(isolate, info[1]);
    auto job = base::MakeRefCounted<MediaJob>();
    job->engine = base::SingleThreadTaskRunner::GetCurrentDefault();
    if (kind == "ffmpeg" && info[2]->IsFunction()) {
      const std::string key = "ffmpeg-progress:" + base::NumberToString(self->next_media_++);
      self->system_functions_[key].Reset(isolate, info[2].As<v8::Function>());
      // A progress function that returns false cancels.
      job->progress = base::BindRepeating(
          [](base::WeakPtr<zebkit_script> script, scoped_refptr<MediaJob> job, std::string key, int64_t packets,
             int64_t time_us) {
            if (!script)
              return;
            auto it = script->system_functions_.find(key);
            if (it == script->system_functions_.end())
              return;
            Scope scope(script.get());
            v8::Isolate* isolate = script->isolate();
            v8::Local<v8::Value> argv[] = {v8::Number::New(isolate, static_cast<double>(packets)),
                                           v8::Number::New(isolate, static_cast<double>(time_us))};
            v8::TryCatch try_catch(isolate);
            v8::Local<v8::Value> result;
            if (!it->second.Get(isolate)->Call(script->context(), v8::Undefined(isolate), 2, argv).ToLocal(&result))
              script->Report(try_catch);
            else if (result->IsFalse())
              job->cancel = true;
          },
          self->weak_factory_.GetWeakPtr(), job, key);
    }
    PendingCall* pending = promise();
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock()},
        base::BindOnce(
            [](std::string kind, std::vector<std::string> args, scoped_refptr<MediaJob> job) {
              MediaResult result;
              char* json = nullptr;
              char* error = nullptr;
              if (kind == "media-probe") {
                result.code = zebkit_media_probe(args[0].c_str(), &json, &error);
              } else {
                std::vector<const char*> argv;
                for (const std::string& arg : args)
                  argv.push_back(arg.c_str());
                if (kind == "ffprobe") {
                  result.code = zebkit_ffprobe(static_cast<int>(argv.size()), argv.data(), &json, &error);
                } else {
                  result.code = zebkit_ffmpeg(static_cast<int>(argv.size()), argv.data(),
                      [](int64_t packets, int64_t time_us, void* data) -> int {
                        auto* job = static_cast<MediaJob*>(data);
                        if (job->progress)
                          job->engine->PostTask(FROM_HERE, base::BindOnce(job->progress, packets, time_us));
                        // Nonzero continues, zero cancels.
                        return job->cancel ? 0 : 1;
                      },
                      job.get(), &error);
                }
              }
              if (json)
                result.json = json;
              if (error) {
                result.error = error;
                result.failed = true;
              }
              zebkit_free(json);
              zebkit_free(error);
              return result;
            },
            kind, std::move(args), job),
        base::BindOnce(
            [](std::unique_ptr<PendingCall> pending, std::string kind, MediaResult result) {
              if (!pending->script)
                return;
              // ffmpeg: 0 completed, 1 cancelled (by the progress function).
              if (kind == "ffmpeg" && (result.code == 0 || result.code == 1)) {
                pending->script->Settle(pending->id, true, base::Value(result.code == 0 ? "completed" : "cancelled"));
              } else if (result.failed || result.code != 0) {
                pending->script->Settle(pending->id, false,
                                        base::Value(result.error.empty() ? "the media tool failed" : result.error));
              } else {
                pending->script->SettleJson(pending->id, result.json.c_str(), nullptr);
              }
            },
            base::WrapUnique(pending), kind));
  } else if (kind == "media-capture") {
    // (view, media id, path, options, progress function, done function)
    zebkit_view* view = view_at(1);
    if (!view)
      return fail("the view is closed");
    char* error = nullptr;
    zebkit_media_capture* capture = zebkit_media_capture_start(
        view, text(2).c_str(), text(3).c_str(), info[4]->IsString() ? text(4).c_str() : nullptr, &error);
    if (!capture)
      return take_error(error, "the capture did not start");
    const int id = self->next_media_++;
    self->captures_[id] = capture;
    const std::string key = base::NumberToString(id);
    auto told = [](zebkit_media_capture*, const char* json, void* data) {
      auto* target = static_cast<Target*>(data);
      zebkit_script* script = target->script.get();
      if (!script)
        return;
      Scope scope(script);
      script->Deliver(target->key, {JsonValueOf(script->isolate(), script->context(), json)});
    };
    if (function_at(5, "capture-progress:" + key))
      zebkit_media_capture_on_progress(capture, told, self->NewTarget("capture-progress:" + key));
    if (function_at(6, "capture-done:" + key))
      zebkit_media_capture_on_done(capture, told, self->NewTarget("capture-done:" + key));
    info.GetReturnValue().Set(id);
  } else if (kind == "capture-stop" || kind == "capture-status" || kind == "capture-free") {
    auto it = self->captures_.find(static_cast<int>(number(1)));
    if (it == self->captures_.end())
      return info.GetReturnValue().SetNull();
    if (kind == "capture-stop") {
      zebkit_media_capture_stop(it->second);
    } else if (kind == "capture-status") {
      set_json(zebkit_media_capture_status(it->second));
    } else {
      zebkit_media_capture_free(it->second);
      self->captures_.erase(it);
    }
  } else if (kind == "media-tap") {
    // (view, media id, options, function(info, bytes))
    zebkit_view* view = view_at(1);
    if (!view)
      return fail("the view is closed");
    const int id = self->next_media_++;
    const std::string key = "tap:" + base::NumberToString(id);
    function_at(4, key);
    char* error = nullptr;
    zebkit_media_tap* tap = zebkit_media_tap_start(
        view, text(2).c_str(), info[3]->IsString() ? text(3).c_str() : nullptr,
        [](zebkit_media_tap*, const char* json, const uint8_t* bytes, size_t size, void* data) {
          auto* target = static_cast<Target*>(data);
          zebkit_script* script = target->script.get();
          if (!script)
            return;
          Scope scope(script);
          v8::Isolate* isolate = script->isolate();
          script->Deliver(target->key, {JsonValueOf(isolate, script->context(), json), BytesValue(isolate, bytes, size)});
        },
        self->NewTarget(key), &error);
    if (!tap) {
      self->system_functions_.erase(key);
      return take_error(error, "the tap did not start");
    }
    self->taps_[id] = tap;
    info.GetReturnValue().Set(id);
  } else if (kind == "tap-stop" || kind == "tap-free") {
    auto it = self->taps_.find(static_cast<int>(number(1)));
    if (it == self->taps_.end())
      return;
    if (kind == "tap-stop") {
      zebkit_media_tap_stop(it->second);
    } else {
      zebkit_media_tap_free(it->second);
      self->taps_.erase(it);
    }
  } else if (kind == "frame") {
    // (op, id): a frame of an off-screen view, held until released.
    auto it = self->frames_.find(static_cast<int>(number(2)));
    if (it == self->frames_.end())
      return info.GetReturnValue().SetNull();
    const zebkit_frame* frame = it->second;
    const std::string op = text(1);
    if (op == "pixels") {
      if (frame->kind != ZEBKIT_FRAME_SHARED_MEMORY || !frame->pixels)
        return info.GetReturnValue().SetNull();
      info.GetReturnValue().Set(BytesValue(isolate, frame->pixels, frame->stride * static_cast<size_t>(frame->height)));
    } else if (op == "mach-port") {
      info.GetReturnValue().Set(zebkit_frame_iosurface_mach_port(frame));
    } else if (op == "release") {
      zebkit_frame_release(frame);
      self->frames_.erase(it);
    }
  } else if (kind == "app-open-files" || kind == "app-reopen" || kind == "app-quit-request") {
    const bool set = function_at(1, kind);
    Target* target = set ? self->NewTarget(kind) : nullptr;
    auto plain = [](void* data) {
      auto* target = static_cast<Target*>(data);
      zebkit_script* script = target->script.get();
      if (!script)
        return;
      Scope scope(script);
      script->Deliver(target->key, {});
    };
    if (kind == "app-open-files") {
      zebkit_app_on_open_files(set ? +[](const char* paths, void* data) {
        auto* target = static_cast<Target*>(data);
        zebkit_script* script = target->script.get();
        if (!script)
          return;
        Scope scope(script);
        script->Deliver(target->key, {JsonValueOf(script->isolate(), script->context(), paths)});
      } : nullptr, target);
    } else if (kind == "app-reopen") {
      zebkit_app_on_reopen(set ? +plain : nullptr, target);
    } else {
      zebkit_app_on_quit_request(set ? +plain : nullptr, target);
    }
  } else {
    fail("unknown call");
  }
}

void zebkit_script::ViewMore(const v8::FunctionCallbackInfo<v8::Value>& info) {
  zebkit_script* self = From(info);
  v8::Isolate* isolate = info.GetIsolate();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  ViewEntry* entry = self->FindView(info);
  if (!entry || !entry->view) {
    isolate->ThrowException(v8::Exception::Error(String8(isolate, "the view is closed")));
    return;
  }
  zebkit_view* view = entry->view;
  const std::string kind = Utf8(isolate, info[1]);
  auto number = [&](int i, double fallback = 0) {
    return info[i]->IsNumber() ? info[i].As<v8::Number>()->Value() : fallback;
  };
  auto integer = [&](int i, int fallback = 0) { return static_cast<int>(number(i, fallback)); };
  auto real = [&](int i, double fallback = 0) { return static_cast<float>(number(i, fallback)); };
  auto text = [&](int i) { return Utf8(isolate, info[i]); };
  auto flag = [&](int i) { return info[i]->BooleanValue(isolate); };
  auto set_string = [&](char* owned) {
    if (!owned)
      return info.GetReturnValue().SetNull();
    info.GetReturnValue().Set(String8(isolate, owned));
    zebkit_free(owned);
  };
  auto set_json = [&](char* owned) {
    info.GetReturnValue().Set(JsonValueOf(isolate, context, owned));
    zebkit_free(owned);
  };
  auto promise = [&]() {
    v8::Local<v8::Promise> made;
    const int id = self->NewPromise(&made);
    info.GetReturnValue().Set(made);
    return new PendingCall{self->weak_factory_.GetWeakPtr(), id};
  };
  // What a drag's operation promise is told.
  auto operation_done = [](zebkit_view*, unsigned operation, void* data) {
    std::unique_ptr<PendingCall> pending(static_cast<PendingCall*>(data));
    if (pending->script)
      pending->script->Settle(pending->id, true, base::Value(static_cast<int>(operation)));
  };

  if (kind == "listen") {
    ViewListen(self, entry, text(2));
  } else if (kind == "target-id") {
    set_string(zebkit_view_get_target_id(view));
  } else if (kind == "size-limits") {
    zebkit_view_set_size_limits(view, integer(2), integer(3), info[4]->IsUndefined() || flag(4));
  } else if (kind == "attach") {
    // (parent view, x, y, width, height)
    auto parent = info[2]->IsInt32() ? self->views_.find(info[2].As<v8::Int32>()->Value()) : self->views_.end();
    if (parent == self->views_.end() || !parent->second.view) {
      isolate->ThrowException(v8::Exception::Error(String8(isolate, "the parent view is closed")));
      return;
    }
    zebkit_view_attach(view, parent->second.view, integer(3), integer(4), integer(5), integer(6));
  } else if (kind == "bounds") {
    zebkit_view_set_bounds(view, integer(2), integer(3), integer(4), integer(5));
  } else if (kind == "visible") {
    zebkit_view_set_visible(view, flag(2));
  } else if (kind == "attach-native") {
    // (handle as a number or a decimal string, x, y, width, height)
    uint64_t handle = 0;
    if (info[2]->IsString())
      base::StringToUint64(text(2), &handle);
    else
      handle = static_cast<uint64_t>(number(2));
    char* error = nullptr;
    if (!zebkit_view_attach_native(view, static_cast<uintptr_t>(handle), integer(3), integer(4), integer(5),
                                   integer(6), &error)) {
      isolate->ThrowException(v8::Exception::Error(String8(isolate, error ? error : "not attached")));
      zebkit_free(error);
      return;
    }
    info.GetReturnValue().Set(true);
  } else if (kind == "detach-native") {
    info.GetReturnValue().Set(zebkit_view_detach_native(view));
  } else if (kind == "title") {
    zebkit_view_set_title(view, text(2).c_str());
  } else if (kind == "add-script") {
    zebkit_view_add_script(view, text(2).c_str());
  } else if (kind == "stop") {
    zebkit_view_stop(view);
  } else if (kind == "is-loading") {
    info.GetReturnValue().Set(zebkit_view_is_loading(view));
  } else if (kind == "user-agent") {
    zebkit_view_set_user_agent(view, info[2]->IsString() ? text(2).c_str() : nullptr);
  } else if (kind == "capture" || kind == "print-to-pdf") {
    PendingCall* pending = promise();
    auto bytes_done = [](zebkit_view*, const uint8_t* bytes, size_t size, const char* error, void* data) {
      std::unique_ptr<PendingCall> pending(static_cast<PendingCall*>(data));
      if (!pending->script)
        return;
      if (error || !bytes)
        pending->script->Settle(pending->id, false, base::Value(error ? error : "nothing was made"));
      else
        pending->script->Settle(pending->id, true, base::Value(base::Value::BlobStorage(bytes, bytes + size)));
    };
    if (kind == "capture")
      zebkit_view_capture(view, bytes_done, pending);
    else
      zebkit_view_print_to_pdf(view, info[2]->IsString() ? text(2).c_str() : nullptr, bytes_done, pending);
  } else if (kind == "navigation-fail-closed") {
    info.GetReturnValue().Set(zebkit_view_set_navigation_fail_closed(view, flag(2)));
  } else if (kind == "popup-policy") {
    const int policy = zebkit_view_get_popup_policy(view);
    info.GetReturnValue().Set(String8(isolate, policy >= 0 && policy < 3 ? kPopupPolicies[policy] : "block-without-gesture"));
  } else if (kind == "dialog-policy") {
    if (info[2]->IsString()) {
      const std::string wanted = text(2);
      for (int i = 0; i < 4; ++i)
        if (wanted == kDialogPolicies[i])
          zebkit_view_set_dialog_policy(view, static_cast<zebkit_dialog_policy>(i));
    }
    const int policy = zebkit_view_get_dialog_policy(view);
    info.GetReturnValue().Set(String8(isolate, policy >= 0 && policy < 4 ? kDialogPolicies[policy] : "default"));
  } else if (kind == "dialog-respond") {
    // (id, accept, prompt text or null)
    const std::string prompt = text(4);
    info.GetReturnValue().Set(zebkit_view_dialog_respond(view, static_cast<uint64_t>(number(2)), flag(3),
                                                         info[4]->IsString() ? prompt.c_str() : nullptr));
  } else if (kind == "dialogs") {
    set_json(zebkit_view_dialogs(view));
  } else if (kind == "request-close") {
    zebkit_view_request_close(view);
  } else if (kind == "is-dragging") {
    info.GetReturnValue().Set(zebkit_view_is_dragging(view));
  } else if (kind == "drag-enter") {
    // (data as JSON, x, y, operations, modifiers)
    zebkit_view_drag_enter(view, text(2).c_str(), real(3), real(4), static_cast<unsigned>(number(5, ZEBKIT_DRAG_ALL)),
                           static_cast<unsigned>(number(6)));
  } else if (kind == "drag-over") {
    PendingCall* pending = promise();
    zebkit_view_drag_over(view, real(2), real(3), static_cast<unsigned>(number(4, ZEBKIT_DRAG_ALL)),
                          static_cast<unsigned>(number(5)), operation_done, pending);
  } else if (kind == "drag-leave") {
    zebkit_view_drag_leave(view);
  } else if (kind == "drop") {
    PendingCall* pending = promise();
    zebkit_view_drop(view, real(2), real(3), static_cast<unsigned>(number(4)), operation_done, pending);
  } else if (kind == "drag-source-end") {
    zebkit_view_drag_source_end(view, real(2), real(3), static_cast<unsigned>(number(4)));
  } else if (kind == "is-offscreen") {
    info.GetReturnValue().Set(zebkit_view_is_offscreen(view));
  } else if (kind == "set-size") {
    zebkit_view_set_size(view, integer(2), integer(3), real(4));
  } else if (kind == "frame-rate") {
    zebkit_view_set_frame_rate(view, number(2));
  } else if (kind == "max-frames") {
    zebkit_view_set_max_frames_in_flight(view, integer(2, 1));
  } else if (kind == "frame-buffers") {
    const std::string buffers = text(2);
    zebkit_view_set_frame_buffers(view, buffers == "gpu"             ? ZEBKIT_BUFFERS_GPU
                                        : buffers == "shared-memory" ? ZEBKIT_BUFFERS_SHARED_MEMORY
                                                                     : ZEBKIT_BUFFERS_AUTO);
  } else if (kind == "request-frame") {
    zebkit_view_request_frame(view);
  } else if (kind == "mouse") {
    // (type, x, y, button, click count, modifiers)
    zebkit_view_send_mouse(view, static_cast<zebkit_mouse_type>(integer(2)), real(3), real(4),
                           static_cast<zebkit_mouse_button>(integer(5)), integer(6), static_cast<unsigned>(number(7)));
  } else if (kind == "wheel") {
    // (x, y, delta x, delta y, precise, phase, modifiers)
    zebkit_view_send_wheel(view, real(2), real(3), real(4), real(5), flag(6),
                           static_cast<zebkit_wheel_phase>(integer(7)), static_cast<unsigned>(number(8)));
  } else if (kind == "key" || kind == "key-commands") {
    // ({type, modifiers, keyCode, nativeKeyCode, code, key, text, unmodifiedText}, [{name, value}])
    if (!info[2]->IsObject())
      return;
    v8::Local<v8::Object> object = info[2].As<v8::Object>();
    auto field = [&](const char* name) {
      v8::Local<v8::Value> value;
      if (!object->Get(context, String8(isolate, name)).ToLocal(&value))
        return v8::Local<v8::Value>(v8::Undefined(isolate));
      return value;
    };
    std::map<std::string, std::string> strings;
    for (const char* name : {"code", "key", "text", "unmodifiedText"})
      if (field(name)->IsString())
        strings[name] = Utf8(isolate, field(name));
    auto optional = [&](const char* name) { return strings.contains(name) ? strings[name].c_str() : nullptr; };
    const std::string type = Utf8(isolate, field("type"));
    auto as_int = [&](const char* name) {
      return field(name)->IsNumber() ? static_cast<int>(field(name).As<v8::Number>()->Value()) : 0;
    };
    zebkit_key_event event = {};
    event.type = type == "up" ? ZEBKIT_KEY_UP : type == "char" ? ZEBKIT_KEY_CHAR : ZEBKIT_KEY_DOWN;
    event.modifiers = static_cast<unsigned>(as_int("modifiers"));
    event.key_code = as_int("keyCode");
    event.native_key_code = as_int("nativeKeyCode");
    event.code = optional("code");
    event.key = optional("key");
    event.text = optional("text");
    event.unmodified_text = optional("unmodifiedText");
    if (kind == "key") {
      zebkit_view_send_key(view, &event);
      return;
    }
    std::vector<std::pair<std::string, std::string>> named;
    if (info[3]->IsArray()) {
      v8::Local<v8::Array> list = info[3].As<v8::Array>();
      for (uint32_t i = 0; i < list->Length(); ++i) {
        v8::Local<v8::Value> item, name, value;
        if (!list->Get(context, i).ToLocal(&item) || !item->IsObject())
          continue;
        item.As<v8::Object>()->Get(context, String8(isolate, "name")).ToLocal(&name);
        item.As<v8::Object>()->Get(context, String8(isolate, "value")).ToLocal(&value);
        named.emplace_back(Utf8(isolate, name), value.IsEmpty() || value->IsUndefined() ? "" : Utf8(isolate, value));
      }
    }
    std::vector<zebkit_edit_command> commands;
    for (const auto& [name, value] : named)
      commands.push_back({name.c_str(), value.c_str()});
    info.GetReturnValue().Set(zebkit_view_send_key_with_commands(view, &event, commands.data(), commands.size()));
  } else if (kind == "ime-composition") {
    zebkit_view_ime_set_composition(view, text(2).c_str(), integer(3), integer(4));
  } else if (kind == "ime-commit") {
    zebkit_view_ime_commit(view, text(2).c_str());
  } else if (kind == "ime-cancel") {
    zebkit_view_ime_cancel(view);
  } else if (kind == "focus-state") {
    zebkit_view_set_focus(view, flag(2));
  } else if (kind == "authentication-ui") {
    info.GetReturnValue().Set(zebkit_view_set_authentication_ui(view, flag(2)));
  } else if (kind == "authentication-action") {
    info.GetReturnValue().Set(zebkit_view_authentication_action(view, text(2).c_str()));
  } else if (kind == "media-list" || kind == "media-at") {
    PendingCall* pending = promise();
    auto done = [](zebkit_view*, const char* json, const char* error, void* data) {
      std::unique_ptr<PendingCall> pending(static_cast<PendingCall*>(data));
      if (pending->script)
        pending->script->SettleJson(pending->id, json, error);
    };
    if (kind == "media-list")
      zebkit_view_media_list(view, done, pending);
    else
      zebkit_view_media_at(view, real(2), real(3), done, pending);
  } else {
    isolate->ThrowException(v8::Exception::Error(String8(isolate, "unknown view call")));
  }
}

// The engine's callback for a view event the script listens to, set the first
// time: each calls the view's handler for the event (native.viewOn).
void zebkit_script::ViewListen(zebkit_script* self, ViewEntry* entry, const std::string& name) {
  zebkit_view* view = entry->view;
  void* hook = entry->hook;
  // The script and the view's number, inside a scope, or nothing when gone.
  struct Call {
    explicit Call(void* data) : hook(static_cast<ViewHook*>(data)) {}
    zebkit_script* script() const { return hook->script.get(); }
    v8::Isolate* isolate() const { return hook->script->isolate(); }
    void Event(const char* name, std::vector<v8::Local<v8::Value>> args) const {
      hook->script->ViewEvent(hook->id, name, std::move(args));
    }
    raw_ptr<ViewHook> hook;
  };
  if (name == "new-window") {
    zebkit_view_on_new_window(view, [](zebkit_view*, const char* url, void* data) {
      Call call(data);
      if (!call.script())
        return;
      Scope scope(call.script());
      call.Event("new-window", {String8(call.isolate(), url ? url : "")});
    }, hook);
  } else if (name == "drop") {
    zebkit_view_on_drop(view, [](zebkit_view*, const char* paths, void* data) {
      Call call(data);
      if (!call.script())
        return;
      Scope scope(call.script());
      call.Event("drop", {JsonValueOf(call.isolate(), call.script()->context(), paths)});
    }, hook);
  } else if (name == "dialog") {
    zebkit_view_on_dialog(view, [](zebkit_view*, const zebkit_dialog* dialog, void* data) {
      Call call(data);
      if (!call.script())
        return;
      Scope scope(call.script());
      v8::Isolate* isolate = call.isolate();
      v8::Local<v8::Object> object = v8::Object::New(isolate);
      SetField(isolate, object, "id", v8::Number::New(isolate, static_cast<double>(dialog->id)));
      SetField(isolate, object, "type",
               String8(isolate, dialog->type >= 0 && dialog->type < 5 ? kDialogTypes[dialog->type] : "alert"));
      SetField(isolate, object, "message", String8(isolate, dialog->message ? dialog->message : ""));
      SetField(isolate, object, "defaultPrompt", String8(isolate, dialog->default_prompt ? dialog->default_prompt : ""));
      SetField(isolate, object, "url", String8(isolate, dialog->url ? dialog->url : ""));
      SetField(isolate, object, "isReload", v8::Boolean::New(isolate, dialog->is_reload));
      SetField(isolate, object, "held", v8::Boolean::New(isolate, dialog->held));
      call.Event("dialog", {object});
    }, hook);
  } else if (name == "dialog-closed") {
    zebkit_view_on_dialog_closed(view, [](zebkit_view*, uint64_t id, bool accepted, const char* input, void* data) {
      Call call(data);
      if (!call.script())
        return;
      Scope scope(call.script());
      v8::Isolate* isolate = call.isolate();
      call.Event("dialog-closed", {v8::Number::New(isolate, static_cast<double>(id)), v8::Boolean::New(isolate, accepted),
                                   input ? v8::Local<v8::Value>(String8(isolate, input)) : v8::Null(isolate)});
    }, hook);
  } else if (name == "drag-start") {
    // The application takes the drag: it is told its end (drag-end).
    zebkit_view_on_drag_start(view, [](zebkit_view*, const zebkit_drag* drag, void* data) -> bool {
      Call call(data);
      if (!call.script())
        return false;
      Scope scope(call.script());
      v8::Isolate* isolate = call.isolate();
      v8::Local<v8::Object> object = v8::Object::New(isolate);
      SetField(isolate, object, "data", JsonValueOf(isolate, call.script()->context(), drag->data_json));
      SetField(isolate, object, "operations", v8::Integer::NewFromUnsigned(isolate, drag->operations));
      SetField(isolate, object, "x", v8::Number::New(isolate, drag->x));
      SetField(isolate, object, "y", v8::Number::New(isolate, drag->y));
      if (drag->image) {
        v8::Local<v8::Object> image = v8::Object::New(isolate);
        SetField(isolate, image, "width", v8::Integer::New(isolate, drag->image_width));
        SetField(isolate, image, "height", v8::Integer::New(isolate, drag->image_height));
        SetField(isolate, image, "stride", v8::Integer::New(isolate, drag->image_stride));
        SetField(isolate, image, "scale", v8::Number::New(isolate, drag->image_scale));
        SetField(isolate, image, "offsetX", v8::Number::New(isolate, drag->image_offset_x));
        SetField(isolate, image, "offsetY", v8::Number::New(isolate, drag->image_offset_y));
        SetField(isolate, image, "data",
                 BytesValue(isolate, drag->image, static_cast<size_t>(drag->image_stride) * drag->image_height));
        SetField(isolate, object, "image", image);
      } else {
        SetField(isolate, object, "image", v8::Null(isolate));
      }
      call.Event("drag-start", {object});
      return true;
    }, hook);
  } else if (name == "drag-end") {
    zebkit_view_on_drag_end(view, [](zebkit_view*, unsigned operation, void* data) {
      Call call(data);
      if (!call.script())
        return;
      Scope scope(call.script());
      call.Event("drag-end", {v8::Integer::NewFromUnsigned(call.isolate(), operation)});
    }, hook);
  } else if (name == "frame") {
    zebkit_view_on_frame(view, [](zebkit_view*, const zebkit_frame* frame, void* data) {
      Call call(data);
      zebkit_script* script = call.script();
      if (!script) {
        zebkit_frame_release(frame);
        return;
      }
      Scope scope(script);
      v8::Isolate* isolate = call.isolate();
      const int id = script->next_frame_++;
      script->frames_[id] = frame;
      static const char* const kKinds[] = {"", "shared-memory", "iosurface", "dmabuf", "d3d11"};
      v8::Local<v8::Object> object = v8::Object::New(isolate);
      auto num = [&](const char* field, double value) { SetField(isolate, object, field, v8::Number::New(isolate, value)); };
      num("sequence", static_cast<double>(frame->sequence));
      SetField(isolate, object, "layer", String8(isolate, frame->layer == ZEBKIT_LAYER_POPUP ? "popup" : "view"));
      SetField(isolate, object, "bounds", RectValue(isolate, &frame->bounds));
      num("width", frame->width);
      num("height", frame->height);
      num("scale", frame->scale);
      v8::Local<v8::Array> damage = v8::Array::New(isolate, static_cast<int>(frame->damage_count));
      for (size_t i = 0; i < frame->damage_count; ++i)
        std::ignore = damage->Set(script->context(), static_cast<uint32_t>(i), RectValue(isolate, &frame->damage[i]));
      SetField(isolate, object, "damage", damage);
      num("drawnUs", static_cast<double>(frame->drawn_us));
      num("readyUs", static_cast<double>(frame->ready_us));
      SetField(isolate, object, "kind",
               String8(isolate, frame->kind >= 1 && frame->kind <= 4 ? kKinds[frame->kind] : "shared-memory"));
      SetField(isolate, object, "format", String8(isolate, frame->format == ZEBKIT_FORMAT_RGBA8 ? "rgba8" : "bgra8"));
      SetField(isolate, object, "bufferId", v8::BigInt::NewFromUnsigned(isolate, frame->buffer_id));
      num("stride", static_cast<double>(frame->stride));
      num("fd", frame->fd);
      num("size", static_cast<double>(frame->size));
      num("offset", static_cast<double>(frame->offset));
      num("planeCount", frame->plane_count);
      v8::Local<v8::Array> fds = v8::Array::New(isolate, frame->plane_count);
      v8::Local<v8::Array> strides = v8::Array::New(isolate, frame->plane_count);
      v8::Local<v8::Array> offsets = v8::Array::New(isolate, frame->plane_count);
      for (int i = 0; i < frame->plane_count && i < 4; ++i) {
        std::ignore = fds->Set(script->context(), i, v8::Integer::New(isolate, frame->fds[i]));
        std::ignore = strides->Set(script->context(), i, v8::Integer::NewFromUnsigned(isolate, frame->strides[i]));
        std::ignore = offsets->Set(script->context(), i, v8::Integer::NewFromUnsigned(isolate, frame->offsets[i]));
      }
      SetField(isolate, object, "fds", fds);
      SetField(isolate, object, "strides", strides);
      SetField(isolate, object, "offsets", offsets);
      SetField(isolate, object, "modifier", v8::BigInt::NewFromUnsigned(isolate, frame->modifier));
      num("fourcc", frame->fourcc);
      SetField(isolate, object, "handle",
               v8::BigInt::NewFromUnsigned(isolate, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(frame->handle))));
      SetField(isolate, object, "iosurface",
               v8::BigInt::NewFromUnsigned(isolate, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(frame->iosurface))));
      call.Event("frame", {object, v8::Integer::New(isolate, id)});
    }, hook);
  } else if (name == "caret") {
    zebkit_view_on_caret(view, [](zebkit_view*, const zebkit_rect* caret, void* data) {
      Call call(data);
      if (!call.script())
        return;
      Scope scope(call.script());
      call.Event("caret", {RectValue(call.isolate(), caret)});
    }, hook);
  } else if (name == "cursor") {
    zebkit_view_on_cursor(view, [](zebkit_view*, const char* cursor, const zebkit_cursor_image* image, void* data) {
      Call call(data);
      if (!call.script())
        return;
      Scope scope(call.script());
      v8::Isolate* isolate = call.isolate();
      v8::Local<v8::Value> picture = v8::Null(isolate);
      if (image && image->pixels) {
        v8::Local<v8::Object> object = v8::Object::New(isolate);
        SetField(isolate, object, "width", v8::Integer::New(isolate, image->width));
        SetField(isolate, object, "height", v8::Integer::New(isolate, image->height));
        SetField(isolate, object, "hotX", v8::Integer::New(isolate, image->hot_x));
        SetField(isolate, object, "hotY", v8::Integer::New(isolate, image->hot_y));
        SetField(isolate, object, "scale", v8::Number::New(isolate, image->scale));
        SetField(isolate, object, "data",
                 BytesValue(isolate, image->pixels, static_cast<size_t>(image->width) * image->height * 4));
        picture = object;
      }
      call.Event("cursor", {String8(isolate, cursor ? cursor : ""), picture});
    }, hook);
  } else if (name == "popup") {
    zebkit_view_on_popup(view, [](zebkit_view*, bool shown, const zebkit_rect* bounds, void* data) {
      Call call(data);
      if (!call.script())
        return;
      Scope scope(call.script());
      call.Event("popup", {v8::Boolean::New(call.isolate(), shown), RectValue(call.isolate(), bounds)});
    }, hook);
  } else if (name == "authentication") {
    zebkit_view_on_authentication(view, [](zebkit_view*, const char* json, void* data) {
      Call call(data);
      if (!call.script())
        return;
      Scope scope(call.script());
      call.Event("authentication", {JsonValueOf(call.isolate(), call.script()->context(), json)});
    }, hook);
  } else if (name == "automation") {
    zebkit_automation_on_event(view, [](zebkit_view*, const char* event, const char* json, void* data) {
      Call call(data);
      if (!call.script())
        return;
      Scope scope(call.script());
      call.Event("automation", {String8(call.isolate(), event ? event : ""),
                                JsonValueOf(call.isolate(), call.script()->context(), json)});
    }, hook);
  }
  (void)self;
}

void zebkit_script::Capabilities(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  char* json = zebkit_get_capabilities();
  v8::Local<v8::Value> value = v8::Null(isolate);
  if (json) {
    v8::Local<v8::Value> parsed;
    if (v8::JSON::Parse(isolate->GetCurrentContext(), String8(isolate, json)).ToLocal(&parsed)) value = parsed;
  }
  zebkit_free(json);
  info.GetReturnValue().Set(value);
}

void zebkit_script::SetViewFactory(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (info[0]->IsFunction()) From(info)->view_factory_.Reset(info.GetIsolate(), info[0].As<v8::Function>());
}

int zebkit_script::WrapView(zebkit_view* view) {
  for (const auto& [id, entry] : views_) if (entry.view == view) return id;
  const int id = this->next_view_++;
  this->views_[id].view = view;
  // The hook lives as long as the view: freed when it closes.
  auto* hook = new ViewHook{this->weak_factory_.GetWeakPtr(), id};
  this->views_[id].hook = hook;
  zebkit_view_on_load(
      view,
      [](zebkit_view*, zebkit_load_event event, const char* url, void* data) {
        auto* hook = static_cast<ViewHook*>(data);
        if (!hook->script)
          return;
        static const char* const kNames[] = {"started", "committed",
                                             "finished", "failed"};
        Scope scope(hook->script.get());
        v8::Isolate* isolate = hook->script->isolate();
        hook->script->ViewEvent(hook->id, "load",
                                {String8(isolate, kNames[event]),
                                 String8(isolate, url ? url : "")});
      },
      hook);
  zebkit_view_on_title(
      view,
      [](zebkit_view*, const char* title, void* data) {
        auto* hook = static_cast<ViewHook*>(data);
        if (!hook->script)
          return;
        Scope scope(hook->script.get());
        hook->script->ViewEvent(
            hook->id, "title",
            {String8(hook->script->isolate(), title ? title : "")});
      },
      hook);
  zebkit_view_on_close(
      view,
      [](zebkit_view*, void* data) {
        std::unique_ptr<ViewHook> hook(static_cast<ViewHook*>(data));
        if (hook->script)
          hook->script->ViewClosed(hook->id);
      },
      hook);
  zebkit_view_on_popup_blocked(
      view,
      [](zebkit_view*, const char* json, void* data) {
        auto* hook = static_cast<ViewHook*>(data);
        if (!hook->script)
          return;
        Scope scope(hook->script.get());
        v8::Local<v8::Value> value;
        if (v8::JSON::Parse(hook->script->context(), String8(hook->script->isolate(), json)).ToLocal(&value))
          hook->script->ViewEvent(hook->id, "popup-blocked", {value});
      },
      hook);
  zebkit_view_on_permission_result(view,
      [](zebkit_view*, const char* json, void* data) {
        auto* hook = static_cast<ViewHook*>(data);
        if (!hook->script) return;
        Scope scope(hook->script.get());
        v8::Local<v8::Value> value;
        if (v8::JSON::Parse(hook->script->context(), String8(hook->script->isolate(), json)).ToLocal(&value))
          hook->script->ViewEvent(hook->id, "permission-result", {value});
      }, hook);
  zebkit_view_on_navigation_result(view,
      [](zebkit_view*, const char* json, void* data) {
        auto* hook = static_cast<ViewHook*>(data);
        if (!hook->script) return;
        Scope scope(hook->script.get());
        v8::Local<v8::Value> value;
        if (v8::JSON::Parse(hook->script->context(), String8(hook->script->isolate(), json)).ToLocal(&value))
          hook->script->ViewEvent(hook->id, "navigation-result", {value});
      }, hook);
  zebkit_view_on_certificate_result(view,
      [](zebkit_view*, const char* json, void* data) {
        auto* hook = static_cast<ViewHook*>(data);
        if (!hook->script) return;
        Scope scope(hook->script.get());
        v8::Local<v8::Value> value;
        if (v8::JSON::Parse(hook->script->context(), String8(hook->script->isolate(), json)).ToLocal(&value))
          hook->script->ViewEvent(hook->id, "certificate-result", {value});
      }, hook);
  zebkit_view_on_http_auth_result(view,
      [](zebkit_view*, const char* json, void* data) {
        auto* hook = static_cast<ViewHook*>(data);
        if (!hook->script) return;
        Scope scope(hook->script.get());
        v8::Local<v8::Value> value;
        if (v8::JSON::Parse(hook->script->context(), String8(hook->script->isolate(), json)).ToLocal(&value))
          hook->script->ViewEvent(hook->id, "http-auth-result", {value});
      }, hook);
  zebkit_view_on_console(view,
      [](zebkit_view*, const char* json, void* data) {
        auto* hook = static_cast<ViewHook*>(data);
        if (!hook->script) return;
        Scope scope(hook->script.get());
        v8::Local<v8::Value> value;
        if (v8::JSON::Parse(hook->script->context(), String8(hook->script->isolate(), json)).ToLocal(&value))
          hook->script->ViewEvent(hook->id, "console", {value});
      }, hook);
  zebkit_view_on_window_state(view,
      [](zebkit_view*, const char* json, void* data) {
        auto* hook = static_cast<ViewHook*>(data);
        if (!hook->script) return;
        Scope scope(hook->script.get());
        v8::Local<v8::Value> value;
        if (v8::JSON::Parse(hook->script->context(), String8(hook->script->isolate(), json)).ToLocal(&value))
          hook->script->ViewEvent(hook->id, "window-state", {value});
      }, hook);
  zebkit_view_on_window_open(view,
      [](zebkit_view*, zebkit_view* child, const char*, void* data) {
        auto* hook = static_cast<ViewHook*>(data);
        if (!hook->script) return;
        auto* script = hook->script.get();
        Scope scope(script);
        const int id = script->WrapView(child);
        v8::Local<v8::Value> argument = v8::Integer::New(script->isolate(), id);
        v8::Local<v8::Value> wrapper;
        v8::TryCatch caught(script->isolate());
        if (script->view_factory_.Get(script->isolate())->Call(script->context(),
            v8::Undefined(script->isolate()), 1, &argument).ToLocal(&wrapper) && wrapper->IsObject()) {
          auto found = script->views_.find(id);
          if (found != script->views_.end()) found->second.wrapper.Reset(script->isolate(), wrapper.As<v8::Object>());
        } else if (caught.HasCaught()) script->Report(caught);
      }, hook);
  zebkit_view_on_window_open_event(view,
      [](zebkit_view*, const char* json, void* data) {
        auto* hook = static_cast<ViewHook*>(data);
        if (!hook->script) return;
        auto* script = hook->script.get();
        Scope scope(script);
        v8::Local<v8::Value> value;
        if (!v8::JSON::Parse(script->context(), String8(script->isolate(), json)).ToLocal(&value) || !value->IsObject()) return;
        auto object = value.As<v8::Object>();
        v8::Local<v8::Value> target;
        if (object->Get(script->context(), String8(script->isolate(), "targetId")).ToLocal(&target) && target->IsString()) {
          auto* child = zebkit::WindowViewForTarget(Utf8(script->isolate(), target));
          std::ignore = object->Set(script->context(), String8(script->isolate(), "view"), v8::Null(script->isolate()));
          for (auto& [child_id, entry] : script->views_) {
            if (child && entry.view == child && !entry.wrapper.IsEmpty()) {
              std::ignore = object->Set(script->context(), String8(script->isolate(), "view"), entry.wrapper.Get(script->isolate()));
              break;
            }
          }
        }
        script->ViewEvent(hook->id, "window-open", {value});
      }, hook);
  return id;
}

void zebkit_script::ViewEvent(int id,
                              const std::string& name,
                              std::vector<v8::Local<v8::Value>> args) {
  auto it = views_.find(id);
  if (it == views_.end())
    return;
  auto handler = it->second.handlers.find(name);
  if (handler == it->second.handlers.end())
    return;
  CallFunction(handler->second.Get(isolate()), std::move(args));
}

void zebkit_script::ViewClosed(int id) {
  {
    Scope scope(this);
    ViewEvent(id, "close", {});
  }
  views_.erase(id);
  CheckIdle();
}

void zebkit_script::ViewOn(const v8::FunctionCallbackInfo<v8::Value>& info) {
  zebkit_script* self = From(info);
  ViewEntry* entry = self->FindView(info);
  if (!entry || !info[2]->IsFunction())
    return;
  entry->handlers[Utf8(info.GetIsolate(), info[1])].Reset(
      info.GetIsolate(), info[2].As<v8::Function>());
}

namespace {
std::optional<zebkit_permission_state> ScriptPermissionState(v8::Isolate* isolate, v8::Local<v8::Value> value) {
  if (!value->IsString()) return std::nullopt;
  const auto state = Utf8(isolate, value);
  if (state == "prompt") return ZEBKIT_PERMISSION_PROMPT;
  if (state == "granted") return ZEBKIT_PERMISSION_GRANTED;
  if (state == "denied") return ZEBKIT_PERMISSION_DENIED;
  return std::nullopt;
}
void PermissionTypeError(v8::Isolate* isolate, const char* text) {
  isolate->ThrowException(v8::Exception::TypeError(String8(isolate, text)));
}
}  // namespace

void zebkit_script::PermissionRequest(const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto* isolate = info.GetIsolate();
  uint64_t id = 0;
  if (!info[1]->IsString() || !base::StringToUint64(Utf8(isolate, info[1]), &id) || !id || id > UINTPTR_MAX) {
    PermissionTypeError(isolate, "invalid permission request id"); return;
  }
  auto* request = reinterpret_cast<zebkit_permission_request*>(static_cast<uintptr_t>(id));
  const auto action = Utf8(isolate, info[0]);
  if (action == "info") {
    char* json = zebkit_permission_request_info(request);
    v8::Local<v8::Value> value = v8::Null(isolate);
    if (json) {
      v8::Local<v8::Value> parsed;
      if (v8::JSON::Parse(isolate->GetCurrentContext(), String8(isolate, json)).ToLocal(&parsed)) value = parsed;
    }
    zebkit_free(json);
    info.GetReturnValue().Set(value);
  } else if (action == "respond") {
    auto state = ScriptPermissionState(isolate, info[2]);
    if (!state || !info[3]->IsBoolean()) { PermissionTypeError(isolate, "invalid permission reply"); return; }
    info.GetReturnValue().Set(zebkit_permission_respond(request, *state, info[3]->IsTrue()));
  }
}

void zebkit_script::ViewPermission(const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto* self = From(info);
  auto* isolate = info.GetIsolate();
  auto* entry = self->FindView(info);
  if (!entry) { PermissionTypeError(isolate, "the view is closed"); return; }
  const auto action = Utf8(isolate, info[1]);
  if (action == "handler") {
    if (!info[2]->IsNull() && !info[2]->IsFunction()) { PermissionTypeError(isolate, "invalid permission handler"); return; }
    {
      // Clearing settles pending prompts, whose handlers can close the view.
      auto weak = entry->view->GetWeakPtr();
      zebkit_view_on_permission(entry->view, nullptr, nullptr);
      if (!weak) return;
    }
    entry->handlers.erase("permission");
    if (info[2]->IsFunction()) {
      entry->handlers["permission"].Reset(isolate, info[2].As<v8::Function>());
      zebkit_view_on_permission(entry->view,
          [](zebkit_view*, zebkit_permission_request*, const char* json, void* data) {
            auto* hook = static_cast<ViewHook*>(data);
            if (!hook->script) return;
            Scope scope(hook->script.get());
            v8::Local<v8::Value> value;
            if (v8::JSON::Parse(hook->script->context(), String8(hook->script->isolate(), json)).ToLocal(&value))
              hook->script->ViewEvent(hook->id, "permission", {value});
          }, entry->hook);
    }
    info.GetReturnValue().Set(true); return;
  }
  if (action == "timeout") {
    if (!info[2]->IsUint32()) { PermissionTypeError(isolate, "invalid permission timeout"); return; }
    const bool ok = zebkit_view_set_permission_timeout(entry->view, info[2].As<v8::Uint32>()->Value());
    if (!ok) PermissionTypeError(isolate, "permission timeout must be from 1 to 300000");
    info.GetReturnValue().Set(ok); return;
  }
  if (!info[2]->IsNull() && !info[2]->IsString()) { PermissionTypeError(isolate, "permission origin must be a string or null"); return; }
  const auto origin = info[2]->IsNull() ? std::string() : Utf8(isolate, info[2]);
  if (origin.find('\0') != std::string::npos) { PermissionTypeError(isolate, "permission origin contains NUL"); return; }
  const char* scope = info[2]->IsNull() ? nullptr : origin.c_str();
  char* error = nullptr;
  bool ok = false;
  if (action == "reset") {
    ok = zebkit_view_reset_permissions(entry->view, scope, &error);
  } else {
    if (!info[3]->IsString()) { PermissionTypeError(isolate, "permission name must be a string"); return; }
    const auto name = Utf8(isolate, info[3]);
    if (name.find('\0') != std::string::npos) { PermissionTypeError(isolate, "permission name contains NUL"); return; }
    if (action == "get") {
      const auto state = zebkit_view_get_permission(entry->view, scope, name.c_str());
      if (state == ZEBKIT_PERMISSION_INVALID) { PermissionTypeError(isolate, "invalid permission or origin"); return; }
      info.GetReturnValue().Set(String8(isolate, state == ZEBKIT_PERMISSION_GRANTED ? "granted" : state == ZEBKIT_PERMISSION_DENIED ? "denied" : "prompt")); return;
    }
    const auto state = ScriptPermissionState(isolate, info[4]);
    if (action != "set" || !state) { PermissionTypeError(isolate, "invalid permission state"); return; }
    ok = zebkit_view_set_permission(entry->view, scope, name.c_str(), *state, &error);
  }
  if (!ok) PermissionTypeError(isolate, error ? error : "permission policy rejected");
  zebkit_free(error);
  info.GetReturnValue().Set(ok);
}

void zebkit_script::ViewCertificate(const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto* self = From(info); auto* isolate = info.GetIsolate();
  auto* entry = self->FindView(info);
  if (!entry) { PermissionTypeError(isolate, "the view is closed"); return; }
  const auto action = Utf8(isolate, info[1]);
  if (action == "timeout") {
    if (!info[2]->IsUint32()) { PermissionTypeError(isolate, "invalid certificate timeout"); return; }
    const bool ok = zebkit_view_set_certificate_timeout(entry->view, info[2].As<v8::Uint32>()->Value());
    if (!ok) PermissionTypeError(isolate, "certificate timeout must be from 1 to 300000");
    info.GetReturnValue().Set(ok); return;
  }
  if (action != "handler" || (!info[2]->IsNull() && !info[2]->IsFunction())) {
    PermissionTypeError(isolate, "invalid certificate handler"); return;
  }
  zebkit_view_on_certificate(entry->view, nullptr, nullptr);
  entry = self->FindView(info);
  if (!entry) { info.GetReturnValue().Set(false); return; }
  entry->handlers.erase("certificate");
  if (info[2]->IsFunction()) {
    entry->handlers["certificate"].Reset(isolate, info[2].As<v8::Function>());
    zebkit_view_on_certificate(entry->view,
        [](zebkit_view*, zebkit_certificate_request*, const char* json, void* data) {
          auto* hook = static_cast<ViewHook*>(data);
          if (!hook->script) return;
          Scope scope(hook->script.get());
          v8::Local<v8::Value> value;
          if (v8::JSON::Parse(hook->script->context(), String8(hook->script->isolate(), json)).ToLocal(&value))
            hook->script->ViewEvent(hook->id, "certificate", {value});
        }, entry->hook);
  }
  info.GetReturnValue().Set(true);
}

void zebkit_script::CertificateRequest(const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto* isolate = info.GetIsolate(); uint64_t id = 0;
  if (!info[1]->IsString() || !base::StringToUint64(Utf8(isolate, info[1]), &id) || !id || id > UINTPTR_MAX) {
    PermissionTypeError(isolate, "invalid certificate request id"); return;
  }
  auto* request = reinterpret_cast<zebkit_certificate_request*>(static_cast<uintptr_t>(id));
  const auto action = Utf8(isolate, info[0]);
  if (action == "info") {
    char* json = zebkit_certificate_request_info(request);
    v8::Local<v8::Value> value = v8::Null(isolate);
    if (json) {
      v8::Local<v8::Value> parsed;
      if (v8::JSON::Parse(isolate->GetCurrentContext(), String8(isolate, json)).ToLocal(&parsed)) value = parsed;
    }
    zebkit_free(json); info.GetReturnValue().Set(value); return;
  }
  if (action != "respond" || !info[2]->IsBoolean()) {
    PermissionTypeError(isolate, "certificate reply must be a boolean"); return;
  }
  info.GetReturnValue().Set(zebkit_certificate_respond(request, info[2]->IsTrue()));
}

void zebkit_script::ViewHttpAuth(const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto* self = From(info); auto* isolate = info.GetIsolate();
  auto* entry = self->FindView(info);
  if (!entry) { PermissionTypeError(isolate, "the view is closed"); return; }
  const auto action = Utf8(isolate, info[1]);
  if (action == "timeout") {
    if (!info[2]->IsUint32()) { PermissionTypeError(isolate, "invalid HTTP authentication timeout"); return; }
    const bool ok = zebkit_view_set_http_auth_timeout(entry->view, info[2].As<v8::Uint32>()->Value());
    if (!ok) PermissionTypeError(isolate, "HTTP authentication timeout must be from 1 to 300000");
    info.GetReturnValue().Set(ok); return;
  }
  if (action != "handler" || (!info[2]->IsNull() && !info[2]->IsFunction())) {
    PermissionTypeError(isolate, "invalid HTTP authentication handler"); return;
  }
  zebkit_view_on_http_auth(entry->view, nullptr, nullptr);
  entry = self->FindView(info);
  if (!entry) { info.GetReturnValue().Set(false); return; }
  entry->handlers.erase("http-auth");
  if (info[2]->IsFunction()) {
    entry->handlers["http-auth"].Reset(isolate, info[2].As<v8::Function>());
    zebkit_view_on_http_auth(entry->view,
        [](zebkit_view*, zebkit_http_auth_request*, const char* json, void* data) {
          auto* hook = static_cast<ViewHook*>(data);
          if (!hook->script) return;
          Scope scope(hook->script.get());
          v8::Local<v8::Value> value;
          if (v8::JSON::Parse(hook->script->context(), String8(hook->script->isolate(), json)).ToLocal(&value))
            hook->script->ViewEvent(hook->id, "http-auth", {value});
        }, entry->hook);
  }
  info.GetReturnValue().Set(true);
}

void zebkit_script::HttpAuthRequest(const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto* isolate = info.GetIsolate(); uint64_t id = 0;
  if (!info[1]->IsString() || !base::StringToUint64(Utf8(isolate, info[1]), &id) || !id || id > UINTPTR_MAX) {
    PermissionTypeError(isolate, "invalid HTTP authentication request id"); return;
  }
  auto* request = reinterpret_cast<zebkit_http_auth_request*>(static_cast<uintptr_t>(id));
  const auto action = Utf8(isolate, info[0]);
  if (action == "info") {
    char* json = zebkit_http_auth_request_info(request);
    v8::Local<v8::Value> value = v8::Null(isolate);
    if (json) {
      v8::Local<v8::Value> parsed;
      if (v8::JSON::Parse(isolate->GetCurrentContext(), String8(isolate, json)).ToLocal(&parsed)) value = parsed;
    }
    zebkit_free(json); info.GetReturnValue().Set(value); return;
  }
  if (action != "respond") { PermissionTypeError(isolate, "invalid HTTP authentication action"); return; }
  if (info[2]->IsNull() && info[3]->IsNull()) {
    info.GetReturnValue().Set(zebkit_http_auth_respond(request, nullptr, nullptr)); return;
  }
  if (!info[2]->IsString() || !info[3]->IsString()) {
    PermissionTypeError(isolate, "HTTP authentication needs two strings or two null values"); return;
  }
  if (info[2].As<v8::String>()->Length() > 65536 || info[3].As<v8::String>()->Length() > 65536) {
    PermissionTypeError(isolate, "HTTP authentication credentials are limited to 65536 UTF-8 bytes"); return;
  }
  const auto username = Utf8(isolate, info[2]), password = Utf8(isolate, info[3]);
  if (username.find('\0') != std::string::npos || password.find('\0') != std::string::npos ||
      username.size() > 65536 || password.size() > 65536) {
    PermissionTypeError(isolate, "HTTP authentication credentials must be NUL-free and at most 65536 UTF-8 bytes"); return;
  }
  info.GetReturnValue().Set(zebkit_http_auth_respond(request, username.c_str(), password.c_str()));
}

// One function per hook name, so that a plain C function reaches its handler.
void zebkit_script::DeliverHook(void* data, const char* name, const char* json) {
  auto* hook = static_cast<ViewHook*>(data);
  if (!hook->script) return;
  Scope scope(hook->script.get());
  v8::Local<v8::Value> value;
  if (v8::JSON::Parse(hook->script->context(), String8(hook->script->isolate(), json)).ToLocal(&value))
    hook->script->ViewEvent(hook->id, name, {value});
}

#define ZEBKIT_TOLD_HOOK(NAME, SETTER)                                        \
  if (kind == NAME) {                                                         \
    SETTER(entry->view, nullptr, nullptr);                                    \
    entry = self->FindView(info);                                             \
    if (!entry) { info.GetReturnValue().Set(false); return; }                 \
    entry->handlers.erase(NAME);                                              \
    if (info[2]->IsFunction()) {                                              \
      entry->handlers[NAME].Reset(isolate, info[2].As<v8::Function>());       \
      SETTER(entry->view, [](zebkit_view*, const char* json, void* data) {    \
        DeliverHook(data, NAME, json);                                        \
      }, entry->hook);                                                        \
    }                                                                         \
    info.GetReturnValue().Set(true);                                          \
    return;                                                                   \
  }
#define ZEBKIT_ASKED_HOOK(NAME, SETTER, REQUEST)                              \
  if (kind == NAME) {                                                         \
    SETTER(entry->view, nullptr, nullptr);                                    \
    entry = self->FindView(info);                                             \
    if (!entry) { info.GetReturnValue().Set(false); return; }                 \
    entry->handlers.erase(NAME);                                              \
    if (info[2]->IsFunction()) {                                              \
      entry->handlers[NAME].Reset(isolate, info[2].As<v8::Function>());       \
      SETTER(entry->view, [](zebkit_view*, REQUEST*, const char* json, void* data) { \
        DeliverHook(data, NAME, json);                                        \
      }, entry->hook);                                                        \
    }                                                                         \
    info.GetReturnValue().Set(true);                                          \
    return;                                                                   \
  }

void zebkit_script::ViewHookFunction(const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto* self = From(info); auto* isolate = info.GetIsolate();
  auto* entry = self->FindView(info);
  if (!entry) { PermissionTypeError(isolate, "the view is closed"); return; }
  const auto kind = Utf8(isolate, info[1]);
  if (kind == "network-body") {
    zebkit_view_on_network_body(entry->view, nullptr, nullptr);
    entry->handlers.erase(kind);
    if (info[2]->IsFunction()) {
      entry->handlers[kind].Reset(isolate, info[2].As<v8::Function>());
      zebkit_view_on_network_body(entry->view, [](zebkit_view*, const char* id, const void* bytes, size_t size,
                                                  bool end, bool truncated, void* data) {
        auto* hook = static_cast<ViewHook*>(data);
        if (!hook->script) return;
        Scope scope(hook->script.get());
        auto* isolate = hook->script->isolate();
        v8::Local<v8::ArrayBuffer> buffer = v8::ArrayBuffer::New(isolate, size);
        if (size) memcpy(buffer->Data(), bytes, size);
        hook->script->ViewEvent(hook->id, "network-body",
                                {String8(isolate, id), buffer, v8::Boolean::New(isolate, end),
                                 v8::Boolean::New(isolate, truncated)});
      }, entry->hook);
    }
    info.GetReturnValue().Set(true);
    return;
  }
  // What a stream's onWritable calls; the stream is told by its number.
  if (kind == "network-stream-writable") {
    entry->handlers.erase(kind);
    if (info[2]->IsFunction()) entry->handlers[kind].Reset(isolate, info[2].As<v8::Function>());
    info.GetReturnValue().Set(true);
    return;
  }
  ZEBKIT_ASKED_HOOK("network", zebkit_view_on_network, zebkit_network_request)
  ZEBKIT_TOLD_HOOK("network-event", zebkit_view_on_network_event)
  ZEBKIT_TOLD_HOOK("external-protocol", zebkit_view_on_external_protocol)
  ZEBKIT_ASKED_HOOK("download", zebkit_view_on_download, zebkit_download_request)
  ZEBKIT_TOLD_HOOK("download-progress", zebkit_view_on_download_progress)
  ZEBKIT_ASKED_HOOK("client-certificate", zebkit_view_on_client_certificate, zebkit_client_certificate_request)
  ZEBKIT_TOLD_HOOK("client-certificate-result", zebkit_view_on_client_certificate_result)
  ZEBKIT_ASKED_HOOK("file-chooser", zebkit_view_on_file_chooser, zebkit_file_chooser_request)
  ZEBKIT_TOLD_HOOK("file-chooser-result", zebkit_view_on_file_chooser_result)
  ZEBKIT_ASKED_HOOK("media-access", zebkit_view_on_media_access, zebkit_media_access_request)
  ZEBKIT_TOLD_HOOK("media-access-result", zebkit_view_on_media_access_result)
  ZEBKIT_ASKED_HOOK("eye-dropper", zebkit_view_on_eye_dropper, zebkit_eye_dropper_request)
  ZEBKIT_TOLD_HOOK("eye-dropper-result", zebkit_view_on_eye_dropper_result)
  ZEBKIT_ASKED_HOOK("share", zebkit_view_on_share, zebkit_share_request)
  ZEBKIT_TOLD_HOOK("share-result", zebkit_view_on_share_result)
  ZEBKIT_ASKED_HOOK("device-chooser", zebkit_view_on_device_chooser, zebkit_device_chooser_request)
  ZEBKIT_TOLD_HOOK("device-chooser-result", zebkit_view_on_device_chooser_result)
  ZEBKIT_TOLD_HOOK("reserved-key", zebkit_view_on_reserved_key)
  ZEBKIT_TOLD_HOOK("context-menu", zebkit_view_on_context_menu)
  ZEBKIT_TOLD_HOOK("fullscreen", zebkit_view_on_fullscreen)
  PermissionTypeError(isolate, "unknown hook");
}
#undef ZEBKIT_TOLD_HOOK
#undef ZEBKIT_ASKED_HOOK

void zebkit_script::ViewHookTimeout(const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto* self = From(info); auto* isolate = info.GetIsolate();
  auto* entry = self->FindView(info);
  if (!entry) { PermissionTypeError(isolate, "the view is closed"); return; }
  const auto kind = Utf8(isolate, info[1]);
  const uint32_t ms = info[2]->IsUint32() ? info[2].As<v8::Uint32>()->Value() : 0;
  const bool ok = kind == "network" ? zebkit_view_set_network_timeout(entry->view, ms)
      : kind == "download" ? zebkit_view_set_download_timeout(entry->view, ms)
      : kind == "client-certificate" ? zebkit_view_set_client_certificate_timeout(entry->view, ms)
      : kind == "file-chooser" ? zebkit_view_set_file_chooser_timeout(entry->view, ms)
      : kind == "media-access" ? zebkit_view_set_media_access_timeout(entry->view, ms)
      : kind == "eye-dropper" ? zebkit_view_set_eye_dropper_timeout(entry->view, ms)
      : kind == "share" ? zebkit_view_set_share_timeout(entry->view, ms)
      : kind == "device-chooser" ? zebkit_view_set_device_chooser_timeout(entry->view, ms) : false;
  if (!ok) { PermissionTypeError(isolate, "a deadline is from 1 to 300000"); return; }
  info.GetReturnValue().Set(true);
}

void zebkit_script::ViewHookCall(const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto* self = From(info); auto* isolate = info.GetIsolate();
  auto* entry = self->FindView(info);
  if (!entry) { PermissionTypeError(isolate, "the view is closed"); return; }
  const auto kind = Utf8(isolate, info[1]);
  char* error = nullptr;
  bool ok = false;
  if (kind == "cancel-download") {
    info.GetReturnValue().Set(zebkit_view_cancel_download(entry->view, Utf8(isolate, info[2]).c_str()));
    return;
  } else if (kind == "world-script") {
    ok = zebkit_view_add_world_script(entry->view, Utf8(isolate, info[3]).c_str(), Utf8(isolate, info[2]).c_str(),
                                      info[4]->IsTrue(), &error);
  } else if (kind == "network-filter") {
    ok = zebkit_view_set_network_filter(entry->view, Utf8(isolate, info[2]).c_str(), &error);
  } else if (kind == "network-filter-workers") {
    info.GetReturnValue().Set(zebkit_view_set_network_filter_for_workers(entry->view, info[2]->IsTrue()));
    return;
  } else if (kind == "network-stream-writable") {
    uint64_t handle = 0;
    if (!base::StringToUint64(Utf8(isolate, info[2]), &handle) || !handle) {
      PermissionTypeError(isolate, "invalid stream");
      return;
    }
    zebkit_network_stream_on_writable(reinterpret_cast<zebkit_network_stream*>(static_cast<uintptr_t>(handle)),
                                      [](zebkit_network_stream* stream, void* data) {
      const std::string json = "\"" + base::NumberToString(reinterpret_cast<uintptr_t>(stream)) + "\"";
      DeliverHook(data, "network-stream-writable", json.c_str());
    }, entry->hook);
    info.GetReturnValue().Set(true);
    return;
  } else if (kind == "reserved-keys") {
    ok = zebkit_view_set_reserved_keys(entry->view, Utf8(isolate, info[2]).c_str(), &error);
  } else if (kind == "context-menu-run" || kind == "context-menu-show") {
    uint64_t menu = 0;
    if (!base::StringToUint64(Utf8(isolate, info[2]), &menu)) { PermissionTypeError(isolate, "invalid menu id"); return; }
    if (kind == "context-menu-run") {
      ok = zebkit_view_context_menu_run(entry->view, menu, Utf8(isolate, info[3]).c_str(), &error);
    } else {
      // The application's items' choice to fn, as a told hook's event.
      const std::string items = info[3]->IsString() ? Utf8(isolate, info[3]) : std::string();
      entry->handlers.erase("context-menu-item");
      if (info[4]->IsFunction()) entry->handlers["context-menu-item"].Reset(isolate, info[4].As<v8::Function>());
      ok = zebkit_view_context_menu_show(
          entry->view, menu, info[3]->IsString() ? items.c_str() : nullptr,
          info[4]->IsFunction() ? +[](zebkit_view*, const char* item, void* data) {
            const std::string json = base::WriteJson(base::Value(item ? item : "")).value_or("\"\"");
            DeliverHook(data, "context-menu-item", json.c_str());
          } : nullptr,
          entry->hook, &error);
    }
  } else if (kind == "fullscreen-policy") {
    const auto policy = Utf8(isolate, info[2]);
    ok = (policy == "allow" || policy == "deny") &&
         zebkit_view_set_fullscreen_policy(entry->view, policy == "allow" ? ZEBKIT_FULLSCREEN_ALLOW : ZEBKIT_FULLSCREEN_DENY);
  }
  if (!ok) {
    PermissionTypeError(isolate, error ? error : "invalid value");
    zebkit_free(error);
    return;
  }
  info.GetReturnValue().Set(true);
}

void zebkit_script::NetworkStreamCall(const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto* isolate = info.GetIsolate();
  const auto action = Utf8(isolate, info[0]);
  uint64_t handle = 0;
  if (!base::StringToUint64(Utf8(isolate, info[1]), &handle) || !handle) {
    PermissionTypeError(isolate, "invalid stream");
    return;
  }
  auto* stream = reinterpret_cast<zebkit_network_stream*>(static_cast<uintptr_t>(handle));
  if (action == "write") {
    // Bytes are a Uint8Array or a string, taken as its UTF-8.
    std::string bytes;
    if (info[2]->IsArrayBufferView()) {
      auto view = info[2].As<v8::ArrayBufferView>();
      bytes.resize(view->ByteLength());
      view->CopyContents(bytes.data(), bytes.size());
    } else if (info[2]->IsString()) {
      bytes = Utf8(isolate, info[2]);
    }
    info.GetReturnValue().Set(zebkit_network_stream_write(stream, bytes.data(), bytes.size()));
  } else if (action == "open") {
    info.GetReturnValue().Set(zebkit_network_stream_is_open(stream));
  } else if (action == "end") {
    zebkit_network_stream_end(stream, info[2]->IsTrue());
    info.GetReturnValue().Set(true);
  }
}

void zebkit_script::HookRequest(const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto* isolate = info.GetIsolate(); uint64_t id = 0;
  const auto kind = Utf8(isolate, info[0]);
  const auto action = Utf8(isolate, info[1]);
  if (!info[2]->IsString() || !base::StringToUint64(Utf8(isolate, info[2]), &id) || !id || id > UINTPTR_MAX) {
    PermissionTypeError(isolate, "invalid request id"); return;
  }
  void* request = reinterpret_cast<void*>(static_cast<uintptr_t>(id));
  if (action == "info") {
    char* json = kind == "network" ? zebkit_network_request_info(static_cast<zebkit_network_request*>(request))
        : kind == "download" ? zebkit_download_request_info(static_cast<zebkit_download_request*>(request))
        : kind == "client-certificate" ? zebkit_client_certificate_request_info(static_cast<zebkit_client_certificate_request*>(request))
        : kind == "file-chooser" ? zebkit_file_chooser_request_info(static_cast<zebkit_file_chooser_request*>(request))
        : kind == "media-access" ? zebkit_media_access_request_info(static_cast<zebkit_media_access_request*>(request))
        : kind == "eye-dropper" ? zebkit_eye_dropper_request_info(static_cast<zebkit_eye_dropper_request*>(request))
        : kind == "share" ? zebkit_share_request_info(static_cast<zebkit_share_request*>(request))
        : kind == "device-chooser" ? zebkit_device_chooser_request_info(static_cast<zebkit_device_chooser_request*>(request))
        : nullptr;
    v8::Local<v8::Value> value = v8::Null(isolate);
    if (json) {
      v8::Local<v8::Value> parsed;
      if (v8::JSON::Parse(isolate->GetCurrentContext(), String8(isolate, json)).ToLocal(&parsed)) value = parsed;
    }
    zebkit_free(json); info.GetReturnValue().Set(value); return;
  }
  // A string, or nothing for null; C strings cannot hold what else there is.
  auto optional = [&](int index, std::string* out) {
    if (info[index]->IsNull() || info[index]->IsUndefined()) return true;
    if (!info[index]->IsString()) return false;
    *out = Utf8(isolate, info[index]);
    return out->find('\0') == std::string::npos;
  };
  if (kind == "network" && action == "respond-file") {
    info.GetReturnValue().Set(zebkit_network_respond_file(static_cast<zebkit_network_request*>(request),
                                                          Utf8(isolate, info[3]).c_str(), Utf8(isolate, info[4]).c_str(),
                                                          nullptr));
    return;
  }
  if (kind == "network" && action == "respond-stream") {
    zebkit_network_stream* stream =
        zebkit_network_respond_stream(static_cast<zebkit_network_request*>(request), Utf8(isolate, info[3]).c_str(), nullptr);
    if (stream)
      info.GetReturnValue().Set(String8(isolate, base::NumberToString(reinterpret_cast<uintptr_t>(stream)).c_str()));
    else
      info.GetReturnValue().SetNull();
    return;
  }
  std::string first, second;
  bool ok = false;
  if (kind == "network") {
    // Bytes are a Uint8Array or a string, taken as its UTF-8.
    std::string bytes;
    if (info[4]->IsArrayBufferView()) {
      auto view = info[4].As<v8::ArrayBufferView>();
      bytes.resize(view->ByteLength());
      view->CopyContents(bytes.data(), bytes.size());
    } else if (info[4]->IsString()) {
      bytes = Utf8(isolate, info[4]);
    }
    ok = zebkit_network_respond(static_cast<zebkit_network_request*>(request), Utf8(isolate, info[3]).c_str(),
                                bytes.data(), bytes.size(), nullptr);
  } else if (kind == "download") {
    const auto verb = Utf8(isolate, info[3]);
    if ((verb != "save" && verb != "cancel") || !optional(4, &first)) { PermissionTypeError(isolate, "a download is saved or cancelled"); return; }
    ok = zebkit_download_respond(static_cast<zebkit_download_request*>(request),
                                 verb == "save" ? ZEBKIT_DOWNLOAD_SAVE : ZEBKIT_DOWNLOAD_CANCEL,
                                 info[4]->IsString() ? first.c_str() : nullptr, nullptr);
  } else if (kind == "client-certificate" && Utf8(isolate, info[3]) == "choose") {
    // One of the system store's identities, by its index.
    if (!info[4]->IsNumber()) { PermissionTypeError(isolate, "choose takes an identity's index"); return; }
    const double index = info[4].As<v8::Number>()->Value();
    if (index < 0 || index > 65536) { PermissionTypeError(isolate, "choose takes an identity's index"); return; }
    ok = zebkit_client_certificate_choose(static_cast<zebkit_client_certificate_request*>(request),
                                          static_cast<size_t>(index), nullptr);
  } else if (kind == "client-certificate") {
    const auto verb = Utf8(isolate, info[3]);
    if ((verb != "none" && verb != "provide" && verb != "cancel") || !optional(4, &first) || !optional(5, &second)) {
      PermissionTypeError(isolate, "a client certificate is none, provide or cancel"); return;
    }
    ok = zebkit_client_certificate_respond(static_cast<zebkit_client_certificate_request*>(request),
        verb == "none" ? ZEBKIT_CLIENT_CERTIFICATE_NONE : verb == "provide" ? ZEBKIT_CLIENT_CERTIFICATE_PROVIDE : ZEBKIT_CLIENT_CERTIFICATE_CANCEL,
        info[4]->IsString() ? first.c_str() : nullptr, info[5]->IsString() ? second.c_str() : nullptr, nullptr);
  } else if (kind == "file-chooser") {
    if (!info[3]->IsArray()) { PermissionTypeError(isolate, "paths is an array"); return; }
    auto array = info[3].As<v8::Array>();
    std::vector<std::string> paths;
    std::vector<const char*> pointers;
    for (uint32_t i = 0; i < array->Length() && i <= 65536; ++i) {
      v8::Local<v8::Value> path;
      if (!array->Get(isolate->GetCurrentContext(), i).ToLocal(&path) || !path->IsString()) { PermissionTypeError(isolate, "a path is a string"); return; }
      paths.push_back(Utf8(isolate, path));
    }
    for (const auto& path : paths) pointers.push_back(path.c_str());
    ok = zebkit_file_chooser_respond(static_cast<zebkit_file_chooser_request*>(request), pointers.data(), pointers.size(), nullptr);
  } else if (kind == "media-access") {
    if (!optional(4, &first) || !optional(5, &second)) { PermissionTypeError(isolate, "a device id is a string"); return; }
    ok = zebkit_media_access_respond(static_cast<zebkit_media_access_request*>(request), info[3]->IsTrue(),
        info[4]->IsString() ? first.c_str() : nullptr, info[5]->IsString() ? second.c_str() : nullptr);
  } else if (kind == "eye-dropper") {
    if (!optional(3, &first)) { PermissionTypeError(isolate, "a colour is \"#rrggbb\" or null"); return; }
    ok = zebkit_eye_dropper_respond(static_cast<zebkit_eye_dropper_request*>(request),
                                    info[3]->IsString() ? first.c_str() : nullptr);
  } else if (kind == "share") {
    ok = zebkit_share_respond(static_cast<zebkit_share_request*>(request), info[3]->IsTrue());
  } else if (kind == "device-chooser") {
    if (!optional(3, &first)) { PermissionTypeError(isolate, "a device id is a string or null"); return; }
    ok = zebkit_device_chooser_respond(static_cast<zebkit_device_chooser_request*>(request),
                                       info[3]->IsString() ? first.c_str() : nullptr);
  }
  info.GetReturnValue().Set(ok);
}

void zebkit_script::ViewNavigation(const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto* self = From(info);
  auto* isolate = info.GetIsolate();
  auto* entry = self->FindView(info);
  if (!entry) { PermissionTypeError(isolate, "the view is closed"); return; }
  const auto action = Utf8(isolate, info[1]);
  if (action == "timeout") {
    if (!info[2]->IsUint32()) { PermissionTypeError(isolate, "invalid navigation timeout"); return; }
    const bool ok = zebkit_view_set_navigation_timeout(entry->view, info[2].As<v8::Uint32>()->Value());
    if (!ok) PermissionTypeError(isolate, "navigation timeout must be from 1 to 300000");
    info.GetReturnValue().Set(ok); return;
  }
  if (action != "handler" || (!info[2]->IsNull() && !info[2]->IsFunction())) {
    PermissionTypeError(isolate, "invalid navigation handler"); return;
  }
  {
    // Clearing settles pending stages, whose handlers can close the view.
    auto weak = entry->view->GetWeakPtr();
    zebkit_view_on_navigation(entry->view, nullptr, nullptr);
    if (!weak) return;
  }
  entry->handlers.erase("navigation");
  if (info[2]->IsFunction()) {
    entry->handlers["navigation"].Reset(isolate, info[2].As<v8::Function>());
    zebkit_view_on_navigation(entry->view,
        [](zebkit_view*, zebkit_navigation_request*, const char* json, void* data) {
          auto* hook = static_cast<ViewHook*>(data);
          if (!hook->script) return;
          Scope scope(hook->script.get());
          v8::Local<v8::Value> value;
          if (v8::JSON::Parse(hook->script->context(), String8(hook->script->isolate(), json)).ToLocal(&value))
            hook->script->ViewEvent(hook->id, "navigation", {value});
        }, entry->hook);
  }
  info.GetReturnValue().Set(true);
}

void zebkit_script::NavigationRequest(const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto* isolate = info.GetIsolate();
  uint64_t id = 0;
  if (!info[1]->IsString() || !base::StringToUint64(Utf8(isolate, info[1]), &id) || !id || id > UINTPTR_MAX) {
    PermissionTypeError(isolate, "invalid navigation request id"); return;
  }
  auto* request = reinterpret_cast<zebkit_navigation_request*>(static_cast<uintptr_t>(id));
  const auto action = Utf8(isolate, info[0]);
  if (action == "info") {
    char* json = zebkit_navigation_request_info(request);
    v8::Local<v8::Value> value = v8::Null(isolate);
    if (json) {
      v8::Local<v8::Value> parsed;
      if (v8::JSON::Parse(isolate->GetCurrentContext(), String8(isolate, json)).ToLocal(&parsed)) value = parsed;
    }
    zebkit_free(json);
    info.GetReturnValue().Set(value); return;
  }
  if (action != "respond" || !info[2]->IsString() || (!info[3]->IsNull() && !info[3]->IsString())) {
    PermissionTypeError(isolate, "invalid navigation reply"); return;
  }
  const auto state = Utf8(isolate, info[2]);
  if (state != "continue" && state != "cancel" && state != "redirect") {
    PermissionTypeError(isolate, "unknown navigation action"); return;
  }
  const auto url = info[3]->IsNull() ? std::string() : Utf8(isolate, info[3]);
  if (url.find('\0') != std::string::npos) { PermissionTypeError(isolate, "replacement URL contains NUL"); return; }
  info.GetReturnValue().Set(zebkit_navigation_respond(request,
      state == "continue" ? ZEBKIT_NAVIGATION_CONTINUE : state == "cancel" ? ZEBKIT_NAVIGATION_CANCEL : ZEBKIT_NAVIGATION_REDIRECT,
      info[3]->IsNull() ? nullptr : url.c_str(), nullptr));
}

template <zebkit_script::Action action>
void zebkit_script::ViewAction(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  zebkit_script* self = From(info);
  v8::Isolate* isolate = info.GetIsolate();
  ViewEntry* entry = self->FindView(info);
  if (!entry)
    return;
  zebkit_view* view = entry->view;
  switch (action) {
    case Action::kLoad:
      zebkit_view_load_url(view, Utf8(isolate, info[1]).c_str());
      break;
    case Action::kReload:
      zebkit_view_reload(view);
      break;
    case Action::kBack:
      zebkit_view_go_back(view);
      break;
    case Action::kForward:
      zebkit_view_go_forward(view);
      break;
    case Action::kResize:
      if (info[1]->IsNumber() && info[2]->IsNumber()) {
        zebkit_view_resize(view, info[1].As<v8::Number>()->Value(),
                           info[2].As<v8::Number>()->Value());
      }
      break;
    case Action::kFocus:
      zebkit_view_focus(view);
      break;
    case Action::kClose:
      zebkit_view_close(view);
      break;
    case Action::kSettings: {
      char* error = nullptr;
      const bool ok = zebkit_view_set_settings(view, Utf8(isolate, info[1]).c_str(), &error);
      if (!ok) isolate->ThrowException(v8::Exception::TypeError(String8(isolate, error ? error : "view settings rejected")));
      zebkit_free(error);
      info.GetReturnValue().Set(ok);
      break;
    }
    case Action::kWindowOpenInfo: {
      char* json = zebkit_view_get_window_open_info(view);
      info.GetReturnValue().Set(String8(isolate, json ? json : "{}"));
      zebkit_free(json);
      break;
    }
    case Action::kGetSettings: {
      char* json = zebkit_view_get_settings(view);
      info.GetReturnValue().Set(String8(isolate, json ? json : "{}"));
      zebkit_free(json);
      break;
    }
    case Action::kRuntimeSettings: {
      char* error = nullptr;
      const bool ok = zebkit_view_set_runtime_settings(view, Utf8(isolate, info[1]).c_str(), &error);
      if (!ok) isolate->ThrowException(v8::Exception::TypeError(String8(isolate, error ? error : "runtime settings rejected")));
      zebkit_free(error);
      info.GetReturnValue().Set(ok);
      break;
    }
    case Action::kGetRuntimeSettings: {
      char* json = zebkit_view_get_runtime_settings(view);
      info.GetReturnValue().Set(String8(isolate, json ? json : "{}"));
      zebkit_free(json);
      break;
    }
    case Action::kPopupPolicy: {
      const std::string policy = Utf8(isolate, info[1]);
      if (policy != "allow" && policy != "block" && policy != "block-without-gesture") {
        isolate->ThrowException(v8::Exception::TypeError(String8(isolate, "unknown popup policy")));
        return;
      }
      info.GetReturnValue().Set(zebkit_view_set_popup_policy(view,
          policy == "allow" ? ZEBKIT_POPUP_POLICY_ALLOW :
          policy == "block" ? ZEBKIT_POPUP_POLICY_BLOCK : ZEBKIT_POPUP_POLICY_BLOCK_WITHOUT_GESTURE));
      break;
    }
    case Action::kUrl:
    case Action::kTitle: {
      char* text = action == Action::kUrl ? zebkit_view_get_url(view)
                                          : zebkit_view_get_title(view);
      info.GetReturnValue().Set(String8(isolate, text ? text : ""));
      zebkit_free(text);
      break;
    }
  }
}

void zebkit_script::ViewEvaluate(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  zebkit_script* self = From(info);
  ViewEntry* entry = self->FindView(info);
  if (!entry)
    return;
  v8::Local<v8::Promise> promise;
  const int id = self->NewPromise(&promise);
  info.GetReturnValue().Set(promise);
  auto* hook = new ViewHook{self->weak_factory_.GetWeakPtr(), id};
  zebkit_view_evaluate(
      entry->view, Utf8(info.GetIsolate(), info[1]).c_str(),
      [](zebkit_view*, const char* json, const char* error, void* data) {
        std::unique_ptr<ViewHook> hook(static_cast<ViewHook*>(data));
        if (hook->script)
          hook->script->SettleJson(hook->id, json, error);
      },
      hook);
}

// A promise of the window's handle for the system's portals, or null.
void zebkit_script::ViewExportNativeWindow(const v8::FunctionCallbackInfo<v8::Value>& info) {
  zebkit_script* self = From(info);
  ViewEntry* entry = self->FindView(info);
  if (!entry)
    return;
  v8::Local<v8::Promise> promise;
  const int id = self->NewPromise(&promise);
  info.GetReturnValue().Set(promise);
  auto* hook = new ViewHook{self->weak_factory_.GetWeakPtr(), id};
  auto settle = [](void* data, const char* handle) {
    std::unique_ptr<ViewHook> hook(static_cast<ViewHook*>(data));
    if (!hook->script)
      return;
    std::string json = "null";
    if (handle)
      base::JSONWriter::Write(base::Value(handle), &json);
    hook->script->SettleJson(hook->id, json.c_str(), nullptr);
  };
  if (!zebkit_view_export_native_window(entry->view, settle, hook))
    settle(hook, nullptr);
}

void zebkit_script::ViewNativeWindow(const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto* self = From(info);
  auto* entry = self->FindView(info);
  if (!entry)
    return;
  zebkit_native_window window{};
  if (!zebkit_view_get_native_window(entry->view, &window)) {
    info.GetReturnValue().SetNull();
    return;
  }
  const char* kind = window.kind == ZEBKIT_NATIVE_WINDOW_X11 ? "x11"
                      : window.kind == ZEBKIT_NATIVE_WINDOW_WIN32 ? "win32" : "appkit";
  auto value = v8::Object::New(self->isolate());
  std::ignore = value->Set(self->context(), String8(self->isolate(), "kind"), String8(self->isolate(), kind));
  std::ignore = value->Set(self->context(), String8(self->isolate(), "handle"),
                         v8::BigInt::NewFromUnsigned(self->isolate(), window.handle));
  info.GetReturnValue().Set(value);
}

void zebkit_script::ViewUserAgentOverride(const v8::FunctionCallbackInfo<v8::Value>& info) {
  zebkit_script* self = From(info);
  ViewEntry* entry = self->FindView(info);
  if (!entry) return;
  v8::Local<v8::Promise> promise;
  const int id = self->NewPromise(&promise);
  info.GetReturnValue().Set(promise);
  if (!info[1]->IsString() || info[1].As<v8::String>()->Length() > 65536) {
    self->SettleJson(id, nullptr, "expected user agent options, at most 65536 UTF-8 bytes");
    return;
  }
  auto* hook = new ViewHook{self->weak_factory_.GetWeakPtr(), id};
  zebkit_view_set_user_agent_override(entry->view, Utf8(info.GetIsolate(), info[1]).c_str(),
      [](zebkit_view*, const char* json, const char* error, void* data) {
        std::unique_ptr<ViewHook> hook(static_cast<ViewHook*>(data));
        if (hook->script) hook->script->SettleJson(hook->id, json, error);
      }, hook);
}

void zebkit_script::ViewGrant(const v8::FunctionCallbackInfo<v8::Value>& info) {
  zebkit_script* self = From(info);
  v8::Isolate* isolate = info.GetIsolate();
  ViewEntry* entry = self->FindView(info);
  if (!entry)
    return;
  const std::string root = Utf8(isolate, info[3]);
  zebkit_view_grant(entry->view, Utf8(isolate, info[1]).c_str(),
                    info[2]->IsInt32() ? info[2].As<v8::Int32>()->Value() : 0,
                    root.empty() ? nullptr : root.c_str());
}

namespace {

// A call from a page to a function the script bound, waiting for the
// function's promise.
struct PageCallToScript {
  zebkit_call* call;
};

void SettleCall(const v8::FunctionCallbackInfo<v8::Value>& info, bool ok) {
  auto* pending = static_cast<PageCallToScript*>(
      info.Data().As<v8::External>()->Value(
          v8::kExternalPointerTypeTagDefault));
  if (!pending->call)
    return;
  v8::Isolate* isolate = info.GetIsolate();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  zebkit_call* call = pending->call;
  pending->call = nullptr;
  if (ok) {
    v8::Local<v8::String> json;
    if (info[0]->IsUndefined() ||
        !v8::JSON::Stringify(context, info[0]).ToLocal(&json)) {
      zebkit_call_resolve(call, nullptr);
    } else {
      const std::string text = Utf8(isolate, json);
      zebkit_call_resolve_json(call, text.data(), text.size());
    }
  } else {
    zebkit_call_reject(call, Describe(isolate, context, info[0]).c_str());
  }
}

}  // namespace

void zebkit_script::ViewBind(const v8::FunctionCallbackInfo<v8::Value>& info) {
  zebkit_script* self = From(info);
  v8::Isolate* isolate = info.GetIsolate();
  ViewEntry* entry = self->FindView(info);
  if (!entry || !info[2]->IsFunction())
    return;
  auto binding = std::make_unique<
      std::pair<base::WeakPtr<zebkit_script>, v8::Global<v8::Function>>>(
      self->weak_factory_.GetWeakPtr(),
      v8::Global<v8::Function>(isolate, info[2].As<v8::Function>()));
  void* data = binding.get();
  entry->bindings.push_back(std::move(binding));
  zebkit_view_bind(
      entry->view, Utf8(isolate, info[1]).c_str(),
      [](zebkit_view*, const char* origin, const char* args_json,
         zebkit_call* call, void* data) {
        auto* binding = static_cast<std::pair<base::WeakPtr<zebkit_script>,
                                              v8::Global<v8::Function>>*>(
            data);
        zebkit_script* script = binding->first.get();
        if (!script) {
          zebkit_call_reject(call, "the script has ended");
          return;
        }
        Scope scope(script);
        v8::Isolate* isolate = script->isolate();
        v8::Local<v8::Context> context = script->context();
        v8::TryCatch try_catch(isolate);
        v8::Local<v8::Value> args;
        if (!v8::JSON::Parse(context, String8(isolate, args_json))
                 .ToLocal(&args) ||
            !args->IsArray()) {
          zebkit_call_reject(call, "bad arguments");
          return;
        }
        std::vector<v8::Local<v8::Value>> argv;
        v8::Local<v8::Array> array = args.As<v8::Array>();
        for (uint32_t i = 0; i < array->Length(); ++i)
          argv.push_back(array->Get(context, i).ToLocalChecked());
        // The page's origin, as `this.origin` in the function.
        v8::Local<v8::Object> receiver = v8::Object::New(isolate);
        std::ignore = receiver->Set(context, String8(isolate, "origin"),
                                    String8(isolate, origin));
        v8::Local<v8::Value> result;
        if (!binding->second.Get(isolate)
                 ->Call(context, receiver, static_cast<int>(argv.size()),
                        argv.data())
                 .ToLocal(&result)) {
          zebkit_call_reject(
              call, Describe(isolate, context, try_catch.Exception()).c_str());
          return;
        }
        // Settled when the result does, promise or not.
        auto* pending = new PageCallToScript{call};
        v8::Local<v8::External> external = v8::External::New(
            isolate, pending, v8::kExternalPointerTypeTagDefault);
        v8::Local<v8::Promise::Resolver> resolver =
            v8::Promise::Resolver::New(context).ToLocalChecked();
        std::ignore = resolver->Resolve(context, result);
        std::ignore = resolver->GetPromise()->Then(
            context,
            v8::Function::New(
                context,
                [](const v8::FunctionCallbackInfo<v8::Value>& info) {
                  SettleCall(info, true);
                },
                external)
                .ToLocalChecked(),
            v8::Function::New(
                context,
                [](const v8::FunctionCallbackInfo<v8::Value>& info) {
                  SettleCall(info, false);
                },
                external)
                .ToLocalChecked());
      },
      data);
}

// ---- zebkit.ui ----

zebkit::UiTree* zebkit_script::Ui() {
  if (!ui_) {
    ui_ = std::make_unique<zebkit::UiTree>(
        base::BindRepeating(
            [](base::WeakPtr<zebkit_script> script, int id,
               const std::string& event, base::Value value) {
              if (!script || script->ui_listener_.IsEmpty())
                return;
              Scope scope(script.get());
              v8::Isolate* isolate = script->isolate();
              script->CallFunction(
                  script->ui_listener_.Get(isolate),
                  {v8::Integer::New(isolate, id), String8(isolate, event),
                   FromValue(isolate, script->context(), value)});
            },
            weak_factory_.GetWeakPtr()),
        base::BindRepeating(&zebkit_script::CheckIdle,
                            weak_factory_.GetWeakPtr()));
  }
  return ui_.get();
}

void zebkit_script::UiCommit(const v8::FunctionCallbackInfo<v8::Value>& info) {
  zebkit_script* self = From(info);
  v8::Isolate* isolate = info.GetIsolate();
  base::Value operations =
      ToValue(isolate, isolate->GetCurrentContext(), info[0]);
  if (!operations.is_list())
    return;
  const std::string error = self->Ui()->Commit(operations.GetList());
  if (!error.empty()) {
    isolate->ThrowException(
        v8::Exception::Error(String8(isolate, "zebkit.ui: " + error)));
  }
}

void zebkit_script::UiDump(const v8::FunctionCallbackInfo<v8::Value>& info) {
  zebkit_script* self = From(info);
  v8::Isolate* isolate = info.GetIsolate();
  if (!info[0]->IsInt32())
    return;
  info.GetReturnValue().Set(FromValue(
      isolate, isolate->GetCurrentContext(),
      self->Ui()->Dump(info[0].As<v8::Int32>()->Value())));
}

void zebkit_script::UiAct(const v8::FunctionCallbackInfo<v8::Value>& info) {
  zebkit_script* self = From(info);
  v8::Isolate* isolate = info.GetIsolate();
  if (!info[0]->IsInt32())
    return;
  const std::string error = self->Ui()->Act(
      info[0].As<v8::Int32>()->Value(), Utf8(isolate, info[1]),
      ToValue(isolate, isolate->GetCurrentContext(), info[2]));
  if (!error.empty()) {
    isolate->ThrowException(
        v8::Exception::Error(String8(isolate, "zebkit.ui: " + error)));
  }
}

void zebkit_script::UiCapture(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  zebkit_script* self = From(info);
  v8::Isolate* isolate = info.GetIsolate();
  if (!info[0]->IsInt32())
    return;
  v8::Local<v8::Promise> promise;
  const int id = self->NewPromise(&promise);
  info.GetReturnValue().Set(promise);
  self->Ui()->Capture(
      info[0].As<v8::Int32>()->Value(),
      base::BindOnce(
          [](base::WeakPtr<zebkit_script> script, int id,
             std::vector<uint8_t> png) {
            if (script) {
              script->Settle(id, true,
                             base::Value(base::Value::BlobStorage(
                                 png.begin(), png.end())));
            }
          },
          self->weak_factory_.GetWeakPtr(), id));
  std::ignore = isolate;
}

void zebkit_script::UiListen(const v8::FunctionCallbackInfo<v8::Value>& info) {
  zebkit_script* self = From(info);
  if (info[0]->IsFunction())
    self->ui_listener_.Reset(info.GetIsolate(), info[0].As<v8::Function>());
}

// ---- Modules and scripts ----

v8::MaybeLocal<v8::Module> zebkit_script::LoadModule(
    const base::FilePath& path) {
  auto it = modules_.find(zebkit::PathToUTF8(path));
  if (it != modules_.end())
    return it->second.Get(isolate());
  std::string text;
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    if (!base::ReadFileToString(path, &text)) {
      isolate()->ThrowException(v8::Exception::Error(
          String8(isolate(), "cannot read the module " + zebkit::PathToUTF8(path))));
      return {};
    }
  }
  v8::ScriptOrigin origin(String8(isolate(), "file://" + zebkit::PathToUTF8(path)), 0, 0,
                          false, -1, v8::Local<v8::Value>(), false, false,
                          /*is_module=*/true);
  v8::ScriptCompiler::Source source(String8(isolate(), text), origin);
  v8::Local<v8::Module> module;
  if (!v8::ScriptCompiler::CompileModule(isolate(), &source).ToLocal(&module))
    return {};
  modules_[zebkit::PathToUTF8(path)].Reset(isolate(), module);
  module_paths_[module->GetIdentityHash()] = zebkit::PathToUTF8(path);
  return module;
}

v8::MaybeLocal<v8::Module> zebkit_script::Resolve(
    v8::Local<v8::Context> context,
    v8::Local<v8::String> specifier,
    v8::Local<v8::FixedArray> attributes,
    v8::Local<v8::Module> referrer) {
  auto* self = static_cast<zebkit_script*>(
      context->GetAlignedPointerFromEmbedderData(
          1, v8::kEmbedderDataTypeTagDefault));
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  const std::string name = Utf8(isolate, specifier);
  auto from = self->module_paths_.find(referrer->GetIdentityHash());
  if (from == self->module_paths_.end() ||
      !(name.starts_with("./") || name.starts_with("../") ||
        name.starts_with("/"))) {
    isolate->ThrowException(v8::Exception::Error(String8(
        isolate, "cannot import \"" + name +
                     "\": only files, by a path starting with ./, ../ or /")));
    return {};
  }
  base::FilePath path = name.starts_with("/")
                            ? zebkit::PathFromUTF8(name)
                            : zebkit::PathFromUTF8(from->second).DirName().Append(zebkit::PathFromUTF8(name));
  base::FilePath absolute;
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    absolute = base::MakeAbsoluteFilePath(path);
  }
  if (absolute.empty()) {
    isolate->ThrowException(v8::Exception::Error(
        String8(isolate, "no module " + zebkit::PathToUTF8(path))));
    return {};
  }
  return self->LoadModule(absolute);
}

namespace {

// What zebkit_script_run_module() and zebkit_script_evaluate() wait for.
struct Finished {
  base::WeakPtr<zebkit_script> script;
  zebkit_result_func func;
  void* data;
  bool as_module;
  int id;
};

}  // namespace

void zebkit_script::Settled(const v8::FunctionCallbackInfo<v8::Value>& info,
                            bool ok) {
  std::unique_ptr<Finished> done(static_cast<Finished*>(
      info.Data().As<v8::External>()->Value(
          v8::kExternalPointerTypeTagDefault)));
  v8::Isolate* isolate = info.GetIsolate();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  std::string json = "null";
  std::string error;
  if (!ok) {
    error = Describe(isolate, context, info[0]);
    if (!done->func)
      fprintf(stderr, "Uncaught %s\n", error.c_str());
  } else if (!done->as_module && !info[0]->IsUndefined()) {
    v8::Local<v8::String> text;
    if (v8::JSON::Stringify(context, info[0]).ToLocal(&text))
      json = Utf8(isolate, text);
  }
  zebkit_script* script = done->script.get();
  if (script) {
    script->evaluating_ = false;
    script->pending_.erase(done->id);
  }
  if (done->func) {
    done->func(nullptr, ok ? json.c_str() : nullptr,
               ok ? nullptr : error.c_str(), done->data);
  }
  if (script)
    script->Release();
}

void zebkit_script::Finish(zebkit_result_func func,
                           void* data,
                           v8::Local<v8::Value> result,
                           bool as_module) {
  // Held until the result settles: a module's evaluation is a promise.
  v8::Local<v8::Promise> held;
  const int id = NewPromise(&held);
  auto* done =
      new Finished{weak_factory_.GetWeakPtr(), func, data, as_module, id};
  v8::Local<v8::External> external =
      v8::External::New(isolate(), done, v8::kExternalPointerTypeTagDefault);
  v8::Local<v8::Promise::Resolver> resolver =
      v8::Promise::Resolver::New(context()).ToLocalChecked();
  std::ignore = resolver->Resolve(context(), result);
  std::ignore = resolver->GetPromise()->Then(
      context(),
      v8::Function::New(context(),
                        [](const v8::FunctionCallbackInfo<v8::Value>& info) {
                          Settled(info, true);
                        },
                        external)
          .ToLocalChecked(),
      v8::Function::New(context(),
                        [](const v8::FunctionCallbackInfo<v8::Value>& info) {
                          Settled(info, false);
                        },
                        external)
          .ToLocalChecked());
}

void zebkit_script::RunModule(const std::string& file,
                              zebkit_result_func func,
                              void* data) {
  Scope scope(this);
  started_ = true;
  evaluating_ = true;
  v8::TryCatch try_catch(isolate());
  base::FilePath path;
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    path = base::MakeAbsoluteFilePath(zebkit::PathFromUTF8(file));
  }
  v8::Local<v8::Module> module;
  v8::Local<v8::Value> result;
  if (path.empty()) {
    evaluating_ = false;
    const std::string error = "no module " + file;
    if (func)
      func(nullptr, nullptr, error.c_str(), data);
    else
      fprintf(stderr, "%s\n", error.c_str());
    CheckIdle();
    return;
  }
  if (!LoadModule(path).ToLocal(&module) ||
      !module->InstantiateModule(context(), &Resolve).FromMaybe(false) ||
      !module->Evaluate(context()).ToLocal(&result)) {
    evaluating_ = false;
    const std::string error =
        Describe(isolate(), context(), try_catch.Exception());
    if (func)
      func(nullptr, nullptr, error.c_str(), data);
    else
      fprintf(stderr, "Uncaught %s\n", error.c_str());
    CheckIdle();
    return;
  }
  Finish(func, data, result, /*as_module=*/true);
}

void zebkit_script::Evaluate(const std::string& source,
                             zebkit_result_func func,
                             void* data) {
  Scope scope(this);
  started_ = true;
  evaluating_ = true;
  v8::TryCatch try_catch(isolate());
  v8::Local<v8::Script> script;
  v8::Local<v8::Value> result;
  if (!v8::Script::Compile(context(), String8(isolate(), source))
           .ToLocal(&script) ||
      !script->Run(context()).ToLocal(&result)) {
    evaluating_ = false;
    const std::string error =
        Describe(isolate(), context(), try_catch.Exception());
    if (func)
      func(nullptr, nullptr, error.c_str(), data);
    CheckIdle();
    return;
  }
  Finish(func, data, result, /*as_module=*/false);
}

extern "C" {

zebkit_script* zebkit_script_new(int argc, const char* const* argv) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  std::vector<std::string> args;
  for (int i = 0; argv && i < argc; ++i)
    args.push_back(argv[i] ? argv[i] : "");
  return new zebkit_script(std::move(args));
}

void zebkit_script_run_module(zebkit_script* script,
                              const char* path,
                              zebkit_result_func func,
                              void* data) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  script->RunModule(path ? path : "", func, data);
}

void zebkit_script_evaluate(zebkit_script* script,
                            const char* source,
                            zebkit_result_func func,
                            void* data) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  script->Evaluate(source ? source : "", func, data);
}

void zebkit_script_on_idle(zebkit_script* script,
                           zebkit_task_func func,
                           void* data) {
  script->OnIdle(func, data);
}

void zebkit_script_destroy(zebkit_script* script) {
  delete script;
}

}  // extern "C"
