// zebkit_view: a content shell window and the page in it, and the page's
// way to the application (zebkit.* in its script).

#ifdef UNSAFE_BUFFERS_BUILD
// The C API takes and returns C strings and arrays.
#pragma allow_unsafe_buffers
#endif

#include "zebkit/engine/view.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/check.h"
#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/strings/strcat.h"
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
#include "content/public/browser/web_contents_view_delegate.h"
#include "content/public/common/drop_data.h"
#include "content/shell/browser/shell.h"
#include "ui/gfx/geometry/size.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/pdf_script.h"

namespace {

char* Copy(const std::string& text) {
  return strdup(text.c_str());
}

// The page's side of zebkit.*, before any of the page's own script runs:
// a call is a POST to zebkit://call, which the frame's own loader factory
// answers (call.cc). Bytes go as they are, in the frame after the JSON.
constexpr char kBridgeScript[] = R"js((() => {
  // zebkit.pdf's wrapper, put before this script (pdf_script.h).
  const makePdf = globalThis.__zebkit_pdf;
  delete globalThis.__zebkit_pdf;
  if (typeof makePdf !== 'function' || globalThis.zebkit) return;
  // The page's own script may change these later.
  const post = globalThis.fetch.bind(globalThis);
  const encoder = new TextEncoder();
  const decoder = new TextDecoder();
  const {isView} = ArrayBuffer;
  const {stringify, parse} = JSON;
  const frame = (call) => {
    const parts = [];
    let size = 0;
    const encode = (value) => {
      if (value instanceof ArrayBuffer || isView(value)) {
        const bytes = value instanceof ArrayBuffer ? new Uint8Array(value)
            : new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
        parts.push(bytes);
        size += bytes.length;
        return {$part: [size - bytes.length, bytes.length]};
      }
      if (Array.isArray(value)) return value.map(encode);
      if (value && typeof value === 'object') {
        const out = {};
        for (const [k, v] of Object.entries(value)) out[k] = encode(v);
        return out;
      }
      return value === undefined ? null : value;
    };
    const json = encoder.encode(stringify({...call, args: encode(call.args)}));
    const body = new Uint8Array(4 + json.length + size);
    new DataView(body.buffer).setUint32(0, json.length, true);
    body.set(json, 4);
    let at = 4 + json.length;
    for (const part of parts) {
      body.set(part, at);
      at += part.length;
    }
    return body;
  };
  const unframe = (buffer) => {
    const length = new DataView(buffer).getUint32(0, true);
    const start = 4 + length;
    const decode = (value) => {
      if (Array.isArray(value)) return value.map(decode);
      if (value && typeof value === 'object') {
        const part = value.$part;
        if (Array.isArray(part) && Object.keys(value).length === 1)
          return new Uint8Array(buffer, start + part[0], part[1]);
        const out = {};
        for (const [k, v] of Object.entries(value)) out[k] = decode(v);
        return out;
      }
      return value;
    };
    const answer = parse(decoder.decode(new Uint8Array(buffer, 4, length)));
    return {ok: answer.ok, value: decode(answer.value)};
  };
  const send = async (module, method, args) => {
    const response = await post('zebkit://call', {
      method: 'POST', body: frame({module, method, args}), cache: 'no-store',
    });
    if (!response.ok) throw new Error('zebkit is not available in this frame');
    const {ok, value} = unframe(await response.arrayBuffer());
    if (!ok) throw new Error(value);
    return value;
  };
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
    pdf: makePdf(send),
    // The path of a file dropped on the page from another application, when
    // the application asked for drops (zebkit_view_on_drop); otherwise ''.
    pathForFile: (file) => {
      if (!(file instanceof File)) return '';
      if (filePaths.has(file)) return filePaths.get(file);
      const path = dropped.find((p) => (p.split(/[\\/]/).pop() || p) === file.name) || '';
      if (path) filePaths.set(file, path);
      return path;
    },
  };
  const filePaths = new WeakMap();
  let dropped = [];
  Object.defineProperty(globalThis, '__zebkit_dropped', {
    value: (paths) => { dropped = Array.isArray(paths) ? paths.map(String) : []; },
  });
  Object.defineProperty(globalThis, 'zebkit', {value: Object.freeze(zebkit), enumerable: false});
})();)js";

}  // namespace

zebkit_view::zebkit_view(content::Shell* shell)
    : content::WebContentsObserver(shell->web_contents()) {
  Views().insert(this);
}

zebkit_view::~zebkit_view() {
  Views().erase(this);
  zebkit::ClosePdfsOf(this);
  zebkit::DetachView(this);
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
  // The application's script acts for the person, as DevTools' console and
  // Puppeteer's evaluate do: it may open a window, which a page's own
  // script may not without a click (BrowserClient::CanCreateWindow).
  params.Set("userGesture", true);
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
  Send("Page.enable", base::DictValue());
  SendScript(base::StrCat({"Object.defineProperty(globalThis, '__zebkit_pdf', "
                           "{value: ",
                           zebkit::kPdfScript,
                           ", configurable: true});\n", kBridgeScript}));
}

void zebkit_view::AddScript(const std::string& source) {
  Send("Page.enable", base::DictValue());
  SendScript(source);
}

void zebkit_view::SendScript(const std::string& source) {
  base::DictValue script;
  script.Set("source", source);
  script.Set("runImmediately", true);
  ++scripts_pending_;
  // Answered, or failed when the page goes, either way once.
  Send("Page.addScriptToEvaluateOnNewDocument", std::move(script),
       base::BindOnce(
           [](base::WeakPtr<zebkit_view> view, const base::DictValue* result,
              const std::string& error) {
             if (!view || --view->scripts_pending_ > 0)
               return;
             if (std::optional<GURL> url = std::move(view->deferred_load_)) {
               view->deferred_load_.reset();
               view->LoadUrl(*url);
             }
           },
           weak_factory_.GetWeakPtr()));
}

void zebkit_view::LoadUrl(const GURL& url) {
  if (scripts_pending_ > 0) {
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

void zebkit_view::TakeCall(content::RenderFrameHost* frame,
                           zebkit::PageCall call) {
  // The origin is the browser's record of the frame, not what its script
  // says.
  const url::Origin origin = frame->GetLastCommittedOrigin();
  call.origin =
      origin.scheme() == "file" ? std::string("file://") : origin.Serialize();
  const GrantEntry* grant = GrantFor(origin);
  call.allow = grant ? grant->allow : 0;
  if (grant)
    call.files_root = grant->files_root;
  zebkit::HandlePageCall(std::move(call));
}

void zebkit_view::CallApp(zebkit::PageCall call) {
  auto it = bindings_.find(call.method);
  if (it == bindings_.end()) {
    std::move(call.reply).Run(
        false, base::Value("zebkit.app." + call.method +
                           " is not bound by the application"));
    return;
  }
  // The application has the arguments as JSON: bytes as {"$bytes": base64}.
  const std::string args =
      base::WriteJson(zebkit::ToPageValue(base::Value(std::move(call.args))))
          .value_or("[]");
  auto* pending = new zebkit_call;
  pending->reply = std::move(call.reply);
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
  }
}

void zebkit_view::AgentHostClosed(content::DevToolsAgentHost* agent_host) {
  devtools_ = nullptr;
  bridge_installed_ = false;
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
  on_authentication_ = nullptr;
  on_authentication_data_ = nullptr;
  FailPending("the view was closed");
  Observe(nullptr);
  if (on_close_)
    on_close_(this, on_close_data_);
  delete this;
}

void zebkit_view::Load(zebkit_load_event event, const GURL& url) {
  if (quiet_blank_) {
    if (url.IsAboutBlank())
      return;
    quiet_blank_ = false;
  }
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

void HandlePopup(content::WebContents* opener,
                 content::Shell* popup,
                 const GURL& url) {
  zebkit_view* view = ViewOf(opener);
  if (!view || !view->on_window_open_ || !popup)
    return;
  zebkit_view* made = new zebkit_view(popup);
  MakeOffscreenLike(view, made);
  const GURL opened = view->opening_url_.is_empty() ? url : view->opening_url_;
  view->opening_url_ = GURL();
  view->on_window_open_(view, made, opened.spec().c_str(),
                        view->on_window_open_data_);
}

zebkit_view* ViewOf(content::WebContents* contents) {
  for (zebkit_view* view : zebkit_view::Views()) {
    if (view->web_contents() == contents)
      return view;
  }
  return nullptr;
}

bool HandleNewWindow(content::WebContents* opener, const GURL& url) {
  zebkit_view* view = ViewOf(opener);
  if (view)
    view->opening_url_ = url;
  if (!view || !view->on_new_window_)
    return false;
  view->on_new_window_(view, url.spec().c_str(), view->on_new_window_data_);
  return true;
}

namespace {

class ViewDelegate : public content::WebContentsViewDelegate {
 public:
  ViewDelegate(content::WebContents* contents,
               std::unique_ptr<content::WebContentsViewDelegate> inner)
      : contents_(contents), inner_(std::move(inner)) {}

  void OnPerformingDrop(const content::DropData& drop_data,
                        DropCompletionCallback callback) override {
    zebkit_view* view = ViewOf(contents_);
    if (!view || !view->on_drop_ || drop_data.filenames.empty()) {
      Drop(drop_data, std::move(callback));
      return;
    }
    base::ListValue paths;
    for (const ui::FileInfo& file : drop_data.filenames)
      paths.Append(file.path.AsUTF8Unsafe());
    const std::string json =
        base::WriteJson(base::Value(std::move(paths))).value_or("[]");
    view->on_drop_(view, json.c_str(), view->on_drop_data_);
    // The page's zebkit.pathForFile() knows the paths before the page gets
    // the drop: the drop goes on when the page has them.
    base::DictValue params;
    params.Set("expression",
               "globalThis.__zebkit_dropped && __zebkit_dropped(" + json + ")");
    view->Send("Runtime.evaluate", std::move(params),
               base::BindOnce(
                   [](base::WeakPtr<ViewDelegate> self,
                      content::DropData drop_data,
                      DropCompletionCallback callback,
                      const base::DictValue*, const std::string&) {
                     if (self)
                       self->Drop(drop_data, std::move(callback));
                   },
                   weak_factory_.GetWeakPtr(), drop_data, std::move(callback)));
  }

  // The rest is the content shell's.
  gfx::NativeWindow GetNativeWindow() override {
    return inner_ ? inner_->GetNativeWindow() : gfx::NativeWindow();
  }
  content::WebDragDestDelegate* GetDragDestDelegate() override {
    return inner_ ? inner_->GetDragDestDelegate() : nullptr;
  }
  void ShowContextMenu(content::RenderFrameHost& render_frame_host,
                       const content::ContextMenuParams& params) override {
    if (inner_)
      inner_->ShowContextMenu(render_frame_host, params);
  }
  void DismissContextMenu() override {
    if (inner_)
      inner_->DismissContextMenu();
  }
  void StoreFocus() override {
    if (inner_)
      inner_->StoreFocus();
  }
  bool RestoreFocus() override { return inner_ && inner_->RestoreFocus(); }
  void ResetStoredFocus() override {
    if (inner_)
      inner_->ResetStoredFocus();
  }
  bool Focus() override { return inner_ && inner_->Focus(); }
  bool TakeFocus(bool reverse) override {
    return inner_ && inner_->TakeFocus(reverse);
  }
  void* GetDelegateForHost(content::RenderWidgetHost* render_widget_host,
                           bool is_popup) override {
    return inner_ ? inner_->GetDelegateForHost(render_widget_host, is_popup)
                  : nullptr;
  }
  void WebContentsDragEnded() override {
    if (inner_)
      inner_->WebContentsDragEnded();
  }

 private:
  void Drop(const content::DropData& drop_data, DropCompletionCallback callback) {
    if (inner_)
      inner_->OnPerformingDrop(drop_data, std::move(callback));
    else
      std::move(callback).Run(drop_data);
  }

  const raw_ptr<content::WebContents> contents_;
  std::unique_ptr<content::WebContentsViewDelegate> inner_;
  base::WeakPtrFactory<ViewDelegate> weak_factory_{this};
};

}  // namespace

std::unique_ptr<content::WebContentsViewDelegate> WrapViewDelegate(
    content::WebContents* contents,
    std::unique_ptr<content::WebContentsViewDelegate> inner) {
  return std::make_unique<ViewDelegate>(contents, std::move(inner));
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
  // Without a URL, about:blank: it gives the view a renderer, which the
  // scripts for new documents are registered with, but its loading is not
  // the application's to hear of (a new WebContents has no navigation).
  view->quiet_blank_ = !(url && *url);
  shell->LoadURL(GURL(url && *url ? url : "about:blank"));
  return view;
}

void zebkit_view_load_url(zebkit_view* view, const char* url) {
  view->LoadUrl(GURL(url ? url : "about:blank"));
}

void zebkit_view_attach(zebkit_view* view,
                        zebkit_view* parent,
                        int x,
                        int y,
                        int width,
                        int height) {
  zebkit::AttachView(view, parent, gfx::Rect(x, y, width, height));
}

void zebkit_view_set_bounds(zebkit_view* view,
                            int x,
                            int y,
                            int width,
                            int height) {
  zebkit::SetAttachedBounds(view, gfx::Rect(x, y, width, height));
}

void zebkit_view_set_visible(zebkit_view* view, bool visible) {
  zebkit::SetAttachedVisible(view, visible);
}

void zebkit_view_set_title(zebkit_view* view, const char* title) {
  if (title)
    view->window_title_ = title;
  else
    view->window_title_.reset();
  if (content::Shell* shell = view->shell())
    zebkit::UpdateWindowTitle(shell);
}

void zebkit_view_set_size_limits(zebkit_view* view,
                                 int min_width,
                                 int min_height,
                                 bool resizable) {
  if (content::Shell* shell = view->shell()) {
    zebkit::SetWindowSizeLimits(
        shell, gfx::Size(std::max(min_width, 0), std::max(min_height, 0)),
        resizable);
  }
}

void zebkit_view_focus(zebkit_view* view) {
  if (content::Shell* shell = view->shell())
    zebkit::FocusWindow(shell);
}

void zebkit_view_on_new_window(zebkit_view* view,
                               zebkit_url_func func,
                               void* data) {
  view->on_new_window_ = func;
  view->on_new_window_data_ = data;
}

char* zebkit_view_get_target_id(zebkit_view* view) {
  if (!view || !view->web_contents())
    return nullptr;
  return strdup(content::DevToolsAgentHost::GetOrCreateFor(view->web_contents())
                    ->GetId().c_str());
}

void zebkit_view_on_window_open(zebkit_view* view,
                                zebkit_window_open_func func,
                                void* data) {
  view->on_window_open_ = func;
  view->on_window_open_data_ = data;
}

void zebkit_view_on_drop(zebkit_view* view, zebkit_drop_func func, void* data) {
  view->on_drop_ = func;
  view->on_drop_data_ = data;
}

void zebkit_view_add_script(zebkit_view* view, const char* source) {
  if (source && *source)
    view->AddScript(source);
}

void zebkit_view_stop(zebkit_view* view) {
  if (view->web_contents())
    view->web_contents()->Stop();
}

bool zebkit_view_is_loading(zebkit_view* view) {
  return view->web_contents() && view->web_contents()->IsLoading();
}

void zebkit_view_set_user_agent(zebkit_view* view, const char* user_agent) {
  // For the view's session, as Chrome's own override for a tab: its
  // requests, navigator.userAgent and workers.
  base::DictValue params;
  params.Set("userAgent", user_agent ? user_agent : "");
  view->Send("Emulation.setUserAgentOverride", std::move(params));
}

void zebkit_view_capture(zebkit_view* view,
                         zebkit_capture_func func,
                         void* data) {
  base::DictValue params;
  params.Set("format", "png");
  view->Send(
      "Page.captureScreenshot", std::move(params),
      base::BindOnce(
          [](zebkit_view* view, zebkit_capture_func func, void* data,
             const base::DictValue* result, const std::string& error) {
            const std::string* encoded =
                result ? result->FindString("data") : nullptr;
            std::string png;
            if (!encoded || !base::Base64Decode(*encoded, &png)) {
              func(view, nullptr, 0,
                   error.empty() ? "no picture" : error.c_str(), data);
              return;
            }
            func(view, reinterpret_cast<const uint8_t*>(png.data()),
                 png.size(), nullptr, data);
          },
          base::Unretained(view), func, data));
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
  std::move(call->reply).Run(
      true, value ? zebkit::FromPageValue(std::move(*value)) : base::Value());
  delete call;
}

void zebkit_call_reject(zebkit_call* call, const char* message) {
  std::move(call->reply).Run(false,
                             base::Value(message ? message : "rejected"));
  delete call;
}

}  // extern "C"
