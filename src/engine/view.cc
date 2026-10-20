// zebkit_view: a content shell window and the page in it, and the page's
// way to the application (zebkit.* in its script).

#ifdef UNSAFE_BUFFERS_BUILD
// The C API takes and returns C strings and arrays.
#pragma allow_unsafe_buffers
#endif

#include "zebkit/engine/view.h"

#include <stdlib.h>
#include <string.h>

#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/threading/thread_restrictions.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/reload_type.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/shell/browser/shell.h"
#include "ui/gfx/geometry/size.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "zebkit/engine/engine.h"

namespace {

char* Copy(const std::string& text) {
  return strdup(text.c_str());
}

// The page's side of zebkit.*: built on a binding the DevTools session
// adds (Runtime.addBinding, as Playwright's exposeFunction is), before any
// of the page's own script runs. Bytes travel as {"$bytes": base64}.
constexpr char kBridgeScript[] = R"js((() => {
  const call = globalThis.__zebkit_call;
  if (typeof call !== 'function' || globalThis.zebkit) return;
  delete globalThis.__zebkit_call;
  const pending = new Map();
  let next = 1;
  const toBase64 = (bytes) => {
    if (bytes.toBase64) return bytes.toBase64();
    let text = '';
    for (let i = 0; i < bytes.length; i += 0x8000)
      text += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
    return btoa(text);
  };
  const fromBase64 = (text) => {
    if (Uint8Array.fromBase64) return Uint8Array.fromBase64(text);
    const raw = atob(text);
    const bytes = new Uint8Array(raw.length);
    for (let i = 0; i < raw.length; ++i) bytes[i] = raw.charCodeAt(i);
    return bytes;
  };
  const encode = (value) => {
    if (value instanceof ArrayBuffer) return {$bytes: toBase64(new Uint8Array(value))};
    if (ArrayBuffer.isView(value))
      return {$bytes: toBase64(new Uint8Array(value.buffer, value.byteOffset, value.byteLength))};
    if (Array.isArray(value)) return value.map(encode);
    if (value && typeof value === 'object') {
      const out = {};
      for (const [k, v] of Object.entries(value)) out[k] = encode(v);
      return out;
    }
    return value === undefined ? null : value;
  };
  const decode = (value) => {
    if (Array.isArray(value)) return value.map(decode);
    if (value && typeof value === 'object') {
      if (typeof value.$bytes === 'string' && Object.keys(value).length === 1)
        return fromBase64(value.$bytes);
      const out = {};
      for (const [k, v] of Object.entries(value)) out[k] = decode(v);
      return out;
    }
    return value;
  };
  Object.defineProperty(globalThis, '__zebkit_reply', {
    value: (id, ok, value) => {
      const p = pending.get(id);
      if (!p) return;
      pending.delete(id);
      if (ok) p.resolve(decode(value)); else p.reject(new Error(value));
    },
  });
  const send = (module, method, args) => new Promise((resolve, reject) => {
    const id = next++;
    pending.set(id, {resolve, reject});
    try {
      call(JSON.stringify({id, module, method, args: encode(args)}));
    } catch (e) {
      pending.delete(id);
      reject(e);
    }
  });
  const api = (module, methods) => Object.freeze(Object.fromEntries(
      methods.map((m) => [m, (...args) => send(module, m, args)])));
  const zebkit = {
    app: new Proxy({}, {get: (_, name) =>
        typeof name === 'string' ? (...args) => send('app', name, args) : undefined}),
    fetch: (url, options) => send('fetch', 'fetch', [String(url), options || {}]),
    fs: api('fs', ['readFile', 'readText', 'writeFile', 'readDir', 'stat', 'mkdir', 'remove']),
    compress: (format, bytes, level) => send('compress', 'compress', [format, bytes, level ?? -1]),
    decompress: (format, bytes) => send('compress', 'decompress', [format, bytes]),
    crypto: api('crypto', ['digest', 'hmac', 'randomBytes']),
    image: api('image', ['decode', 'encode']),
    url: api('url', ['parse', 'registrableDomain']),
  };
  Object.defineProperty(globalThis, 'zebkit', {value: Object.freeze(zebkit), enumerable: false});
})();)js";

}  // namespace

zebkit_view::zebkit_view(content::Shell* shell)
    : content::WebContentsObserver(shell->web_contents()) {
  Views().insert(this);
}

zebkit_view::~zebkit_view() {
  Views().erase(this);
  if (devtools_)
    devtools_->DetachClient(this);
}

std::set<zebkit_view*>& zebkit_view::Views() {
  static base::NoDestructor<std::set<zebkit_view*>> views;
  return *views;
}

content::Shell* zebkit_view::shell() {
  return web_contents() ? content::Shell::FromWebContents(web_contents())
                        : nullptr;
}

void zebkit_view::Close() {
  if (content::Shell* s = shell())
    s->Close();
}

bool zebkit_view::EnsureSession() {
  if (devtools_)
    return true;
  if (!web_contents())
    return false;
  devtools_ = content::DevToolsAgentHost::GetOrCreateFor(web_contents());
  if (!devtools_->AttachClient(this)) {
    devtools_ = nullptr;
    return false;
  }
  return true;
}

void zebkit_view::Send(const std::string& method,
                       base::DictValue params,
                       Reply reply) {
  if (!EnsureSession()) {
    if (reply)
      std::move(reply).Run(nullptr, "the page cannot be inspected");
    return;
  }
  const int id = next_id_++;
  base::DictValue message;
  message.Set("id", id);
  message.Set("method", method);
  message.Set("params", std::move(params));
  if (reply)
    pending_[id] = std::move(reply);
  const std::string json = base::WriteJson(message).value_or("{}");
  devtools_->DispatchProtocolMessage(this, base::as_byte_span(json));
}

void zebkit_view::Evaluate(const char* script,
                           zebkit_result_func func,
                           void* data) {
  base::DictValue params;
  params.Set("expression", script ? script : "");
  params.Set("awaitPromise", true);
  params.Set("returnByValue", true);
  Send("Runtime.evaluate", std::move(params),
       base::BindOnce(
           [](zebkit_view* view, zebkit_result_func func, void* data,
              const base::DictValue* result, const std::string& error) {
             if (!func)
               return;
             if (!result) {
               func(view, nullptr, error.c_str(), data);
               return;
             }
             if (const base::DictValue* exception =
                     result->FindDict("exceptionDetails")) {
               std::string text = "exception";
               if (const std::string* description =
                       exception->FindStringByDottedPath(
                           "exception.description")) {
                 text = *description;
               } else if (const std::string* summary =
                              exception->FindString("text")) {
                 text = *summary;
               }
               func(view, nullptr, text.c_str(), data);
               return;
             }
             std::string json = "null";
             if (const base::DictValue* value = result->FindDict("result")) {
               if (const base::Value* by_value = value->Find("value")) {
                 json = base::WriteJson(*by_value).value_or("null");
               } else if (const std::string* unserializable =
                              value->FindString("unserializableValue")) {
                 // NaN, Infinity, -0, a BigInt: not JSON, so a string.
                 json = base::WriteJson(base::Value(*unserializable))
                            .value_or("null");
               }
             }
             func(view, json.c_str(), nullptr, data);
           },
           base::Unretained(this), func, data));
}

void zebkit_view::Grant(const std::string& origin,
                        unsigned allow,
                        const std::string& files_root) {
  if (!allow) {
    grants_.erase(origin);
    return;
  }
  base::FilePath root;
  if (!files_root.empty()) {
    base::ScopedAllowBlockingForTesting allow_blocking;
    root = base::MakeAbsoluteFilePath(base::FilePath(files_root));
  }
  grants_[origin] = {allow, root};
  InstallBridge();
}

void zebkit_view::Bind(const std::string& name,
                       zebkit_bind_func func,
                       void* data) {
  if (!func)
    bindings_.erase(name);
  else
    bindings_[name] = {func, data};
}

void zebkit_view::InstallBridge() {
  if (bridge_installed_)
    return;
  bridge_installed_ = true;
  // Execution contexts, for the frame (and so the origin) a call comes from.
  Send("Runtime.enable", base::DictValue());
  Send("Page.enable", base::DictValue());
  base::DictValue binding;
  binding.Set("name", "__zebkit_call");
  Send("Runtime.addBinding", std::move(binding));
  base::DictValue script;
  script.Set("source", kBridgeScript);
  script.Set("runImmediately", true);
  bridge_pending_ = true;
  // Answered, or failed when the page goes, either way once.
  Send("Page.addScriptToEvaluateOnNewDocument", std::move(script),
       base::BindOnce(
           [](base::WeakPtr<zebkit_view> view, const base::DictValue* result,
              const std::string& error) {
             if (!view)
               return;
             view->bridge_pending_ = false;
             if (std::optional<GURL> url = std::move(view->deferred_load_)) {
               view->deferred_load_.reset();
               view->LoadUrl(*url);
             }
           },
           weak_factory_.GetWeakPtr()));
}

void zebkit_view::LoadUrl(const GURL& url) {
  if (bridge_pending_) {
    deferred_load_ = url;
    return;
  }
  if (content::Shell* s = shell())
    s->LoadURL(url);
}

const zebkit_view::GrantEntry* zebkit_view::GrantFor(
    const url::Origin& origin) const {
  const std::string serialized =
      origin.scheme() == "file" ? std::string("file://") : origin.Serialize();
  auto it = grants_.find(serialized);
  if (it != grants_.end())
    return &it->second;
  it = grants_.find("*");
  return it != grants_.end() ? &it->second : nullptr;
}

content::RenderFrameHost* zebkit_view::FrameForContext(int context_id) {
  auto it = contexts_.find(context_id);
  if (it == contexts_.end() || !web_contents())
    return nullptr;
  content::RenderFrameHost* found = nullptr;
  web_contents()->ForEachRenderFrameHost([&](content::RenderFrameHost* rfh) {
    if (!found && rfh->GetDevToolsFrameToken().ToString() == it->second)
      found = rfh;
  });
  return found;
}

void zebkit_view::OnBindingCalled(const base::DictValue& params) {
  const std::string* name = params.FindString("name");
  const std::string* payload = params.FindString("payload");
  std::optional<int> context_id = params.FindInt("executionContextId");
  if (!name || *name != "__zebkit_call" || !payload || !context_id)
    return;
  std::optional<base::Value> parsed =
      base::JSONReader::Read(*payload, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict())
    return;
  base::DictValue& request = parsed->GetDict();
  std::optional<int> call_id = request.FindInt("id");
  const std::string* module = request.FindString("module");
  const std::string* method = request.FindString("method");
  base::ListValue* args = request.FindList("args");
  if (!call_id || !module || !method || !args)
    return;
  zebkit::PageCall call;
  call.view = this;
  call.context_id = *context_id;
  call.call_id = *call_id;
  call.reply = base::BindOnce(
      [](base::WeakPtr<zebkit_view> view, int context_id, int call_id,
         bool ok, base::Value value) {
        if (view)
          view->ReplyToPage(context_id, call_id, ok, std::move(value));
      },
      GetWeakPtr(), *context_id, *call_id);
  call.module = *module;
  call.method = *method;
  call.args = std::move(*args);
  // The origin is the browser's record of the frame, not what its script
  // says, and only a frame of the page itself, in its main world, is heard.
  content::RenderFrameHost* frame = FrameForContext(*context_id);
  if (!frame) {
    std::move(call.reply).Run(
        false, base::Value("zebkit is not available in this frame"));
    return;
  }
  const url::Origin origin = frame->GetLastCommittedOrigin();
  call.origin =
      origin.scheme() == "file" ? std::string("file://") : origin.Serialize();
  const GrantEntry* grant = GrantFor(origin);
  call.allow = grant ? grant->allow : 0;
  if (grant)
    call.files_root = grant->files_root;
  zebkit::HandlePageCall(std::move(call));
}

void zebkit_view::ReplyToPage(int context_id,
                              int call_id,
                              bool ok,
                              base::Value value) {
  const std::string json =
      base::WriteJson(zebkit::ToPageValue(std::move(value))).value_or("null");
  base::DictValue params;
  params.Set("expression", "globalThis.__zebkit_reply && __zebkit_reply(" +
                               base::NumberToString(call_id) + ", " +
                               (ok ? "true" : "false") + ", " + json + ")");
  params.Set("contextId", context_id);
  Send("Runtime.evaluate", std::move(params));
}

void zebkit_view::CallApp(zebkit::PageCall call) {
  auto it = bindings_.find(call.method);
  if (it == bindings_.end()) {
    std::move(call.reply).Run(
        false, base::Value("zebkit.app." + call.method +
                           " is not bound by the application"));
    return;
  }
  const std::string args = base::WriteJson(call.args).value_or("[]");
  auto* pending = new zebkit_call;
  pending->view = weak_factory_.GetWeakPtr();
  pending->context_id = call.context_id;
  pending->call_id = call.call_id;
  it->second.func(this, call.origin.c_str(), args.c_str(), pending,
                  it->second.data);
}

void zebkit_view::DispatchProtocolMessage(content::DevToolsAgentHost* host,
                                          base::span<const uint8_t> message) {
  std::optional<base::Value> parsed = base::JSONReader::Read(
      std::string_view(reinterpret_cast<const char*>(message.data()),
                       message.size()),
      base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!parsed || !parsed->is_dict())
    return;
  base::DictValue& reply = parsed->GetDict();
  if (std::optional<int> id = reply.FindInt("id")) {
    auto it = pending_.find(*id);
    if (it == pending_.end())
      return;
    Reply callback = std::move(it->second);
    pending_.erase(it);
    if (const base::DictValue* error = reply.FindDict("error")) {
      const std::string* text = error->FindString("message");
      std::move(callback).Run(nullptr, text ? *text : "error");
    } else if (const base::DictValue* result = reply.FindDict("result")) {
      std::move(callback).Run(result, std::string());
    } else {
      std::move(callback).Run(nullptr, "no result");
    }
    return;
  }
  const std::string* method = reply.FindString("method");
  const base::DictValue* params = reply.FindDict("params");
  if (!method || !params)
    return;
  if (*method == "Runtime.bindingCalled") {
    OnBindingCalled(*params);
  } else if (*method == "Runtime.executionContextCreated") {
    const base::DictValue* context = params->FindDict("context");
    if (!context)
      return;
    std::optional<int> id = context->FindInt("id");
    const base::DictValue* aux = context->FindDict("auxData");
    if (!id || !aux)
      return;
    const std::string* frame = aux->FindString("frameId");
    std::optional<bool> is_default = aux->FindBool("isDefault");
    if (frame && is_default.value_or(false))
      contexts_[*id] = *frame;
  } else if (*method == "Runtime.executionContextDestroyed") {
    if (std::optional<int> id = params->FindInt("executionContextId"))
      contexts_.erase(*id);
  } else if (*method == "Runtime.executionContextsCleared") {
    contexts_.clear();
  }
}

void zebkit_view::AgentHostClosed(content::DevToolsAgentHost* agent_host) {
  devtools_ = nullptr;
  bridge_installed_ = false;
  contexts_.clear();
  FailPending("the page was closed");
}

void zebkit_view::DidStartNavigation(content::NavigationHandle* navigation) {
  if (navigation->IsInPrimaryMainFrame() && !navigation->IsSameDocument())
    Load(ZEBKIT_LOAD_STARTED, navigation->GetURL());
}

void zebkit_view::DidFinishNavigation(content::NavigationHandle* navigation) {
  if (!navigation->IsInPrimaryMainFrame() || navigation->IsSameDocument())
    return;
  if (navigation->HasCommitted() && !navigation->IsErrorPage())
    Load(ZEBKIT_LOAD_COMMITTED, navigation->GetURL());
  else
    Load(ZEBKIT_LOAD_FAILED, navigation->GetURL());
}

void zebkit_view::DidFinishLoad(content::RenderFrameHost* frame,
                                const GURL& url) {
  if (frame->IsInPrimaryMainFrame())
    Load(ZEBKIT_LOAD_FINISHED, url);
}

void zebkit_view::DidFailLoad(content::RenderFrameHost* frame,
                              const GURL& url,
                              int error_code) {
  if (frame->IsInPrimaryMainFrame())
    Load(ZEBKIT_LOAD_FAILED, url);
}

void zebkit_view::TitleWasSet(content::NavigationEntry* entry) {
  if (on_title_) {
    const std::string title = base::UTF16ToUTF8(web_contents()->GetTitle());
    on_title_(this, title.c_str(), on_title_data_);
  }
}

void zebkit_view::WebContentsDestroyed() {
  if (devtools_) {
    devtools_->DetachClient(this);
    devtools_ = nullptr;
  }
  FailPending("the view was closed");
  Observe(nullptr);
  if (on_close_)
    on_close_(this, on_close_data_);
  delete this;
}

void zebkit_view::Load(zebkit_load_event event, const GURL& url) {
  if (on_load_)
    on_load_(this, event, url.spec().c_str(), on_load_data_);
}

void zebkit_view::FailPending(const std::string& why) {
  std::map<int, Reply> pending;
  pending.swap(pending_);
  for (auto& [id, reply] : pending) {
    if (reply)
      std::move(reply).Run(nullptr, why);
  }
}

namespace zebkit {

void CloseAllViews() {
  std::vector<zebkit_view*> views(zebkit_view::Views().begin(),
                                  zebkit_view::Views().end());
  for (zebkit_view* view : views) {
    if (zebkit_view::Views().contains(view))
      view->Close();
  }
}

}  // namespace zebkit

extern "C" {

zebkit_view* zebkit_view_new(const char* url, int width, int height) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  const bool sized = width > 0 && height > 0;
  content::Shell* shell = content::Shell::CreateNewWindow(
      zebkit::GetBrowserContext(), GURL(), nullptr,
      gfx::Size(sized ? width : 0, sized ? height : 0));
  if (!shell)
    return nullptr;
  zebkit_view* view = new zebkit_view(shell);
  shell->LoadURL(GURL(url && *url ? url : "about:blank"));
  return view;
}

void zebkit_view_load_url(zebkit_view* view, const char* url) {
  view->LoadUrl(GURL(url ? url : "about:blank"));
}

void zebkit_view_reload(zebkit_view* view) {
  if (view->web_contents())
    view->web_contents()->GetController().Reload(content::ReloadType::NORMAL,
                                                 false);
}

void zebkit_view_go_back(zebkit_view* view) {
  if (view->web_contents() && view->web_contents()->GetController().CanGoBack())
    view->web_contents()->GetController().GoBack();
}

void zebkit_view_go_forward(zebkit_view* view) {
  if (view->web_contents() &&
      view->web_contents()->GetController().CanGoForward()) {
    view->web_contents()->GetController().GoForward();
  }
}

void zebkit_view_resize(zebkit_view* view, int width, int height) {
  if (content::Shell* shell = view->shell())
    shell->ResizeWebContentForTests(gfx::Size(width, height));
}

void zebkit_view_close(zebkit_view* view) {
  // As a task: the caller may be in a callback of the page, which must not
  // be destroyed while it notifies.
  content::GetUIThreadTaskRunner({})->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<zebkit_view> view) {
                       if (view)
                         view->Close();
                     },
                     view->GetWeakPtr()));
}

char* zebkit_view_get_url(zebkit_view* view) {
  return Copy(view->web_contents()
                  ? view->web_contents()->GetLastCommittedURL().spec()
                  : std::string());
}

char* zebkit_view_get_title(zebkit_view* view) {
  return Copy(view->web_contents()
                  ? base::UTF16ToUTF8(view->web_contents()->GetTitle())
                  : std::string());
}

void zebkit_view_on_load(zebkit_view* view, zebkit_load_func func, void* data) {
  view->on_load_ = func;
  view->on_load_data_ = data;
}

void zebkit_view_on_title(zebkit_view* view,
                          zebkit_title_func func,
                          void* data) {
  view->on_title_ = func;
  view->on_title_data_ = data;
}

void zebkit_view_on_close(zebkit_view* view, zebkit_view_func func, void* data) {
  view->on_close_ = func;
  view->on_close_data_ = data;
}

void zebkit_view_evaluate(zebkit_view* view,
                          const char* script,
                          zebkit_result_func func,
                          void* data) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  view->Evaluate(script, func, data);
}

void zebkit_view_grant(zebkit_view* view,
                       const char* origin,
                       unsigned allow,
                       const char* files_root) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!origin)
    return;
  view->Grant(origin, allow & ZEBKIT_ALLOW_ALL, files_root ? files_root : "");
}

void zebkit_view_bind(zebkit_view* view,
                      const char* name,
                      zebkit_bind_func func,
                      void* data) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (name && *name)
    view->Bind(name, func, data);
}

void zebkit_call_resolve(zebkit_call* call, const char* json) {
  std::optional<base::Value> value;
  if (json) {
    value = base::JSONReader::Read(json, base::JSON_PARSE_RFC);
    if (!value) {
      zebkit_call_reject(call, "the application answered with invalid JSON");
      return;
    }
  }
  if (call->view) {
    call->view->ReplyToPage(call->context_id, call->call_id, true,
                            value ? std::move(*value) : base::Value());
  }
  delete call;
}

void zebkit_call_reject(zebkit_call* call, const char* message) {
  if (call->view) {
    call->view->ReplyToPage(call->context_id, call->call_id, false,
                            base::Value(message ? message : "rejected"));
  }
  delete call;
}

}  // extern "C"
