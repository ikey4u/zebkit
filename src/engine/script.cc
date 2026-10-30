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

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/strings/string_number_conversions.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
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
#include "zebkit/engine/engine.h"
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
  class View {
    #id;
    #handlers = {load: [], title: [], close: [], 'popup-blocked': [], 'permission-result': [], 'navigation-result': [], 'window-open': [], console: [], 'certificate-result': [], 'http-auth-result': []};
    constructor(url = 'about:blank', {width = 0, height = 0} = {}, adoption = null) {
      this.#id = adoption?.key === VIEW_ADOPT ? adoption.id : native.viewNew(String(url), width, height);
      native.viewOn(this.#id, 'load', (event, url) => this.#emit('load', event, url));
      native.viewOn(this.#id, 'title', (title) => this.#emit('title', title));
      native.viewOn(this.#id, 'close', () => this.#emit('close'));
      native.viewOn(this.#id, 'popup-blocked', (event) => this.#emit('popup-blocked', event));
      native.viewOn(this.#id, 'permission-result', event => this.#emit('permission-result', event));
      native.viewOn(this.#id, 'navigation-result', event => this.#emit('navigation-result', event));
      native.viewOn(this.#id, 'window-open', event => this.#emit('window-open', event));
      native.viewOn(this.#id, 'console', event => this.#emit('console', event));
      native.viewOn(this.#id, 'certificate-result', event => this.#emit('certificate-result', event));
      native.viewOn(this.#id, 'http-auth-result', event => this.#emit('http-auth-result', event));
    }
    #emit(name, ...args) { for (const fn of this.#handlers[name]) fn(...args); }
    on(name, fn) {
      if (!this.#handlers[name]) throw new Error(`a view has no event "${name}"`);
      this.#handlers[name].push(fn);
      return this;
    }
    load(url) { native.viewLoad(this.#id, String(url)); }
    reload() { native.viewReload(this.#id); }
    back() { native.viewBack(this.#id); }
    forward() { native.viewForward(this.#id); }
    resize(width, height) { native.viewResize(this.#id, width, height); }
    setSettings(settings) { return native.viewSettings(this.#id, JSON.stringify(settings)); }
    get settings() { return JSON.parse(native.viewGetSettings(this.#id)); }
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

  globalThis.zebkit = Object.freeze({
    ui: native.ui,
    version: native.version,
    engineVersion: native.engineVersion,
    args: Object.freeze([...native.args]),
    quit: () => native.quit(),
    View,
    fetch: (url, options) => call('fetch', 'fetch', [String(url), options || {}]),
    fs: api('fs', ['readFile', 'readText', 'writeFile', 'readDir', 'stat', 'mkdir', 'remove']),
    compress: (format, bytes, level) => call('compress', 'compress', [format, bytes, level ?? -1]),
    decompress: (format, bytes) => call('compress', 'decompress', [format, bytes]),
    crypto: api('crypto', ['digest', 'hmac', 'randomBytes']),
    image: api('image', ['decode', 'encode']),
    url: api('url', ['parse', 'registrableDomain']),
    pdf: (__ZEBKIT_PDF__)(call),
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
  int WrapView(zebkit_view* view);
  static void SetViewFactory(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void ViewNew(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void ViewOn(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void ViewPermission(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void PermissionRequest(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void ViewCertificate(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void CertificateRequest(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void ViewHttpAuth(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void HttpAuthRequest(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void ViewNavigation(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void NavigationRequest(const v8::FunctionCallbackInfo<v8::Value>& info);
  enum class Action { kLoad, kReload, kBack, kForward, kResize, kClose, kUrl, kTitle, kPopupPolicy, kSettings, kGetSettings, kWindowOpenInfo };
  template <Action action>
  static void ViewAction(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void ViewEvaluate(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void ViewUserAgentOverride(const v8::FunctionCallbackInfo<v8::Value>& info);
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
  SetMethod(native, "setViewFactory", &SetViewFactory);
  SetMethod(native, "viewWindowOpenInfo", &ViewAction<Action::kWindowOpenInfo>);
  SetMethod(native, "viewOn", &ViewOn);
  SetMethod(native, "viewPermission", &ViewPermission);
  SetMethod(native, "permissionRequest", &PermissionRequest);
  SetMethod(native, "viewNavigation", &ViewNavigation);
  SetMethod(native, "viewCertificate", &ViewCertificate);
  SetMethod(native, "certificateRequest", &CertificateRequest);
  SetMethod(native, "viewHttpAuth", &ViewHttpAuth);
  SetMethod(native, "httpAuthRequest", &HttpAuthRequest);
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
  SetMethod(native, "viewPopupPolicy", &ViewAction<Action::kPopupPolicy>);
  SetMethod(native, "viewSettings", &ViewAction<Action::kSettings>);
  SetMethod(native, "viewGetSettings", &ViewAction<Action::kGetSettings>);
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
  call.files_root = base::FilePath("/");
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
  zebkit_view* view = zebkit_view_new(url.c_str(), width, height);
  if (!view) {
    isolate->ThrowException(
        v8::Exception::Error(String8(isolate, "cannot create a view")));
    return;
  }
  info.GetReturnValue().Set(self->WrapView(view));
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
      zebkit_call_resolve(call, Utf8(isolate, json).c_str());
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
  auto it = modules_.find(path.value());
  if (it != modules_.end())
    return it->second.Get(isolate());
  std::string text;
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    if (!base::ReadFileToString(path, &text)) {
      isolate()->ThrowException(v8::Exception::Error(
          String8(isolate(), "cannot read the module " + path.value())));
      return {};
    }
  }
  v8::ScriptOrigin origin(String8(isolate(), "file://" + path.value()), 0, 0,
                          false, -1, v8::Local<v8::Value>(), false, false,
                          /*is_module=*/true);
  v8::ScriptCompiler::Source source(String8(isolate(), text), origin);
  v8::Local<v8::Module> module;
  if (!v8::ScriptCompiler::CompileModule(isolate(), &source).ToLocal(&module))
    return {};
  modules_[path.value()].Reset(isolate(), module);
  module_paths_[module->GetIdentityHash()] = path.value();
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
                            ? base::FilePath(name)
                            : base::FilePath(from->second).DirName().Append(name);
  base::FilePath absolute;
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    absolute = base::MakeAbsoluteFilePath(path);
  }
  if (absolute.empty()) {
    isolate->ThrowException(v8::Exception::Error(
        String8(isolate, "no module " + path.value())));
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
    path = base::MakeAbsoluteFilePath(base::FilePath(file));
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
