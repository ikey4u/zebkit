// zebkit_view: a window and the page in it, and the page's
// way to the application (zebkit.* in its script).

#ifdef UNSAFE_BUFFERS_BUILD
// The C API takes and returns C strings and arrays.
#pragma allow_unsafe_buffers
#endif

#include "zebkit/engine/view.h"
#include "zebkit/engine/http_auth.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <set>
#include <string>
#include <tuple>
#include <vector>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/check.h"
#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
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
#include "content/public/browser/render_process_host.h"
#include "zebkit/engine/profile.h"
#include "zebkit/engine/window.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "net/http/http_util.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "third_party/blink/public/mojom/devtools/console_message.mojom-shared.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/permissions.h"
#include "zebkit/engine/certificate_hooks.h"
#include "zebkit/engine/navigation_hooks.h"
#include "zebkit/engine/call.h"
#include "zebkit/engine/page_hooks.h"
#include "zebkit/engine/popup_metadata.h"
#include "zebkit/engine/settings.h"
#include "zebkit/engine/pdf_script.h"

namespace {

char* Copy(const std::string& text) {
  return strdup(text.c_str());
}

// The page's side of zebkit.*, before any of the page's own script runs:
// a call goes to the browser through __zebkit_call, a native function each
// frame's renderer puts in the page's world for this script alone to take
// (page_call_renderer.cc, call.cc). The arguments go as JSON, their bytes
// beside it as they are; zebkit.app's JSON goes to the application as the
// page wrote it, and the application's answer comes back the same way.
constexpr char kBridgeScript[] = R"js((() => {
  // zebkit.pdf's wrapper, put before this script (pdf_script.h).
  const makePdf = globalThis.__zebkit_pdf;
  delete globalThis.__zebkit_pdf;
  const native = globalThis.__zebkit_call;
  delete globalThis.__zebkit_call;
  if (typeof makePdf !== 'function' || typeof native !== 'function' ||
      globalThis.zebkit) {
    return;
  }
  // The page's own script may change these later.
  const {isView} = ArrayBuffer;
  const {stringify, parse} = JSON;
  const {isArray} = Array;
  const {entries, keys} = Object;
  const encode = (value, parts, at) => {
    if (value instanceof ArrayBuffer || isView(value)) {
      const bytes = value instanceof ArrayBuffer ? new Uint8Array(value)
          : new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
      parts.push(bytes);
      at.size += bytes.length;
      return {$part: [at.size - bytes.length, bytes.length]};
    }
    if (isArray(value)) return value.map((v) => encode(v, parts, at));
    if (value && typeof value === 'object') {
      const out = {};
      for (const [k, v] of entries(value)) out[k] = encode(v, parts, at);
      return out;
    }
    return value === undefined ? null : value;
  };
  const decode = (value, buffer) => {
    if (isArray(value)) return value.map((v) => decode(v, buffer));
    if (value && typeof value === 'object') {
      const part = value.$part;
      if (isArray(part) && keys(value).length === 1)
        return new Uint8Array(buffer, part[0], part[1]);
      const out = {};
      for (const [k, v] of entries(value)) out[k] = decode(v, buffer);
      return out;
    }
    return value;
  };
  const primitive = (args) => {
    for (let i = 0; i < args.length; i++) {
      const v = args[i];
      if (v !== null && v !== undefined && typeof v !== 'string' &&
          typeof v !== 'number' && typeof v !== 'boolean') {
        return false;
      }
    }
    return true;
  };
  const send = async (module, method, args) => {
    const parts = [];
    // Strings, numbers and the like the renderer writes as JSON itself,
    // with no copy of them made here.
    const json = primitive(args) ? args : stringify(encode(args, parts, {size: 0}));
    // [ok, the result's JSON or the message, its bytes]
    const answer = await native(module, method, json, parts);
    if (!answer[0]) throw new Error(answer[1]);
    const value = parse(answer[1]);
    return answer[2] ? decode(value, answer[2]) : value;
  };
  const api = (module, methods) => Object.freeze(Object.fromEntries(
      methods.map((m) => [m, (...args) => send(module, m, args)])));
  const pdf = makePdf(send);
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
    pdf,
    canvas: pdf.canvas,
    text: pdf.text,
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

zebkit_view::zebkit_view(zebkit::Window* window)
    : content::WebContentsObserver(window->web_contents()) {
  Views().insert(this);
}

zebkit_view::~zebkit_view() {
  protocol_closing_ = true;
  zebkit::CancelHttpAuthRequests(this, "cancelled");
  zebkit::CancelNavigationRequests(this, "cancelled");
  zebkit::CancelPermissionRequests(this, "cancelled");
  zebkit::CancelCertificateRequests(this, "cancelled");
  zebkit::CancelPageHookRequests(this);
  Views().erase(this);
  zebkit::ClosePdfsOf(this);
  zebkit::DetachView(this);
  zebkit::ForgetAttachedNative(this);
  if (devtools_)
    devtools_->DetachClient(this);
}

std::set<zebkit_view*>& zebkit_view::Views() {
  static base::NoDestructor<std::set<zebkit_view*>> views;
  return *views;
}

zebkit::Window* zebkit_view::window() {
  return web_contents() ? zebkit::Window::FromWebContents(web_contents())
                        : nullptr;
}

void zebkit_view::Close() {
  if (zebkit::Window* s = window())
    s->Close();
}

bool zebkit_view::EnsureSession() {
  if (protocol_closing_) return false;
  if (devtools_)
    return !protocol_attach_failed_;
  if (!web_contents())
    return false;
  devtools_ = content::DevToolsAgentHost::GetOrCreateForTab(web_contents());
  if (!devtools_->AttachClient(this)) {
    devtools_ = nullptr;
    return false;
  }
  base::DictValue params;
  params.Set("autoAttach", true);
  params.Set("waitForDebuggerOnStart", true);
  params.Set("flatten", true);
  base::ListValue filter;
  base::DictValue pages;
  pages.Set("type", "page");
  pages.Set("exclude", false);
  filter.Append(std::move(pages));
  base::DictValue rest;
  rest.Set("exclude", true);
  filter.Append(std::move(rest));
  params.Set("filter", std::move(filter));
  base::DictValue message;
  protocol_auto_attach_id_ = next_id_++;
  message.Set("id", protocol_auto_attach_id_);
  message.Set("method", "Target.setAutoAttach");
  message.Set("params", std::move(params));
  DispatchCommand(std::move(message));
  return true;
}

void zebkit_view::DispatchCommand(base::DictValue message) {
  if (!devtools_) return;
  const std::string json = base::WriteJson(message).value_or("{}");
  // Keep the host alive if a synchronous protocol callback closes the view.
  auto host = devtools_;
  host->DispatchProtocolMessage(this, base::as_byte_span(json));
}

void zebkit_view::FlushTargetCommands(const std::string& target) {
  auto session = protocol_sessions_.find(target);
  if (session == protocol_sessions_.end()) return;
  const std::string session_id = session->second;
  std::vector<base::DictValue> commands;
  for (auto it = protocol_queued_.begin(); it != protocol_queued_.end();) {
    if (it->first != target) { ++it; continue; }
    commands.push_back(std::move(it->second));
    it = protocol_queued_.erase(it);
  }
  auto weak = GetWeakPtr();
  for (auto& message : commands) {
    message.Set("sessionId", session_id);
    DispatchCommand(std::move(message));
    if (!weak) return;
  }
}

void zebkit_view::SendTargetCommand(const std::string& target, const std::string& method,
                                   base::DictValue params, Reply reply) {
  auto session = protocol_sessions_.find(target);
  if (session == protocol_sessions_.end()) return;
  const int id = next_id_++;
  base::DictValue message;
  message.Set("id", id);
  message.Set("method", method);
  message.Set("params", std::move(params));
  message.Set("sessionId", session->second);
  if (reply) pending_[id] = std::move(reply);
  protocol_pending_targets_[id] = target;
  DispatchCommand(std::move(message));
}

void zebkit_view::InitializeProtocolTarget(const std::string& target) {
  if (!protocol_sessions_.contains(target) || protocol_initializing_.contains(target)) return;
  const size_t applied = protocol_scripts_applied_[target];
  // A worker has no documents: none of the scripts, nor the user agent.
  const bool worker_target = protocol_worker_targets_.contains(target);
  const bool user_agent = !worker_target && protocol_user_agent_ &&
      protocol_user_agent_applied_[target] != protocol_user_agent_revision_;
  if (applied >= protocol_script_sources_.size() && !user_agent) {
    if (protocol_hold_start_) return;  // ReleaseStart()
    protocol_ready_targets_.insert(target);
    auto weak = GetWeakPtr();
    if (protocol_flush_before_run_) {
      // Held: what came meanwhile first (scripts with runImmediately, the
      // viewport), then the window's first script.
      protocol_flush_before_run_ = false;
      FlushTargetCommands(target);
      if (!weak) return;
      SendTargetCommand(target, "Runtime.runIfWaitingForDebugger", base::DictValue());
      if (weak) MaybeRunDeferredLoad();
      return;
    }
    // A frame's target attaches inside its navigation's start, as a
    // prerender's does (below): resumed once the navigation's DevTools
    // throttle is there, or its commit waits for good.
    if (protocol_frame_targets_.contains(target)) {
      FlushTargetCommands(target);
      if (!weak) return;
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
          base::BindOnce([](base::WeakPtr<zebkit_view> view, std::string target) {
            if (view && view->protocol_sessions_.contains(target))
              view->SendTargetCommand(target, "Runtime.runIfWaitingForDebugger", base::DictValue());
          }, weak, target));
      return;
    }
    SendTargetCommand(target, "Runtime.runIfWaitingForDebugger", base::DictValue());
    if (!weak) return;
    FlushTargetCommands(target);
    if (weak) MaybeRunDeferredLoad();
    return;
  }
  protocol_ready_targets_.erase(target);
  const size_t count = protocol_script_sources_.size();
  protocol_scripts_applied_[target] = count;
  const bool frame_target = protocol_frame_targets_.contains(target);
  auto skipped = [&](size_t index) {
    return worker_target || protocol_removed_scripts_.contains(index) ||
           (frame_target && !protocol_all_frame_scripts_.contains(index));
  };
  size_t scripts = 0;
  for (size_t index = applied; index < count; ++index)
    if (!skipped(index)) ++scripts;
  if (!scripts && !user_agent) { InitializeProtocolTarget(target); return; }
  protocol_initializing_[target] = scripts + (scripts ? 1 : 0) + (user_agent ? 1 : 0);
  auto weak = GetWeakPtr();
  auto completion = [](base::WeakPtr<zebkit_view> view, const std::string& target,
                       const base::DictValue*, const std::string& error) {
    if (view) view->ProtocolInitializationReply(target, error);
  };
  if (user_agent) {
    protocol_user_agent_applied_[target] = protocol_user_agent_revision_;
    SendTargetCommand(target, "Emulation.setUserAgentOverride", protocol_user_agent_->Clone(),
                      base::BindOnce(completion, weak, target));
    if (!weak) return;
  }
  if (scripts) {
    SendTargetCommand(target, "Page.enable", base::DictValue(), base::BindOnce(completion, weak, target));
    if (!weak) return;
  }
  for (size_t index = applied; index < count; ++index) {
    if (skipped(index)) continue;
    base::DictValue params;
    params.Set("source", protocol_script_sources_[index]);
    params.Set("runImmediately", true);
    if (auto world = protocol_script_worlds_.find(index); world != protocol_script_worlds_.end())
      params.Set("worldName", world->second);
    SendTargetCommand(target, "Page.addScriptToEvaluateOnNewDocument", std::move(params),
                      base::BindOnce([](base::WeakPtr<zebkit_view> view, std::string target,
                                        size_t index, const base::DictValue* result,
                                        const std::string& error) {
                        if (!view) return;
                        const bool recorded = view->RecordProtocolScript(target, index, result);
                        if (view) view->ProtocolInitializationReply(target,
                            recorded || !error.empty() ? error : "script registration returned no identifier");
                      }, weak, target, index));
    if (!weak) return;
  }
  // This notification occurs inside WillInitiatePrerender(), before Chromium
  // builds the navigation's DevTools request throttle. A synchronous resume
  // clears its waiting state too early and leaves renderer messages suspended
  // until commit. Let startup install the throttle and release those messages
  // before resuming. Acknowledgements still gate subsequent host commands.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
      base::BindOnce([](base::WeakPtr<zebkit_view> view, std::string target) {
        if (view && !view->protocol_hold_start_ && !view->protocol_flush_before_run_ &&
            view->protocol_sessions_.contains(target))
          view->SendTargetCommand(target, "Runtime.runIfWaitingForDebugger", base::DictValue());
      }, weak, target));
}

void zebkit_view::HoldStart() {
  protocol_hold_start_ = true;
  EnsureSession();
}

void zebkit_view::ReleaseStart() {
  if (!protocol_hold_start_) return;
  protocol_hold_start_ = false;
  protocol_flush_before_run_ = true;
  std::vector<std::string> targets;
  for (const auto& [target, session] : protocol_sessions_) {
    if (!protocol_ready_targets_.contains(target) && !protocol_initializing_.contains(target))
      targets.push_back(target);
  }
  auto weak = GetWeakPtr();
  for (const auto& target : targets) {
    if (!weak) return;
    InitializeProtocolTarget(target);
  }
}

bool zebkit_view::RecordProtocolScript(const std::string& target, size_t index,
                                      const base::DictValue* result) {
  const auto* identifier = result ? result->FindString("identifier") : nullptr;
  if (!identifier) return false;
  if (protocol_removed_scripts_.contains(index)) {
    // Disposal may race an acknowledgement from a newly attached renderer.
    base::DictValue params;
    params.Set("identifier", *identifier);
    SendTargetCommand(target, "Page.removeScriptToEvaluateOnNewDocument", std::move(params));
  } else {
    protocol_script_ids_[target][index] = *identifier;
  }
  return true;
}

void zebkit_view::RemoveProtocolScript(size_t index, Reply reply) {
  if (index >= protocol_script_sources_.size()) {
    if (reply) std::move(reply).Run(nullptr, "unknown initialization script");
    return;
  }
  protocol_removed_scripts_.insert(index);
  std::string().swap(protocol_script_sources_[index]);
  const std::string current = web_contents()
      ? content::DevToolsAgentHost::GetOrCreateFor(web_contents())->GetId() : "";
  std::vector<std::pair<std::string, std::string>> removals;
  std::optional<std::string> current_id;
  for (auto& [target, scripts] : protocol_script_ids_) {
    auto script = scripts.find(index);
    if (script == scripts.end()) continue;
    if (protocol_sessions_.contains(target)) {
      if (target == current) current_id = script->second;
      else removals.emplace_back(target, script->second);
    }
    scripts.erase(script);
  }
  auto weak = GetWeakPtr();
  for (const auto& [target, identifier] : removals) {
    base::DictValue params;
    params.Set("identifier", identifier);
    SendTargetCommand(target, "Page.removeScriptToEvaluateOnNewDocument", std::move(params));
    if (!weak) return;
  }
  if (current_id && protocol_sessions_.contains(current)) {
    base::DictValue params;
    params.Set("identifier", *current_id);
    SendTargetCommand(current, "Page.removeScriptToEvaluateOnNewDocument", std::move(params), std::move(reply));
  } else if (reply) {
    base::DictValue result;
    std::move(reply).Run(&result, std::string());
  }
}

void zebkit_view::ProtocolInitializationReply(const std::string& target,
                                             const std::string& error) {
  auto initializing = protocol_initializing_.find(target);
  if (initializing == protocol_initializing_.end()) return;
  if (!error.empty()) {
    protocol_initializing_.erase(initializing);
    auto weak = GetWeakPtr();
    // Ready, without its scripts: the failure is reported here, and a target
    // left waiting would hold every later command and load for good.
    protocol_ready_targets_.insert(target);
    FailPending("the page initialization scripts could not be installed");
    if (weak && web_contents()) web_contents()->Stop();
    if (weak) FlushTargetCommands(target);
    if (weak) MaybeRunDeferredLoad();
    return;
  }
  if (--initializing->second) return;
  protocol_initializing_.erase(initializing);
  // A host may have registered another script while these acknowledgements
  // were in flight. Install that script too before releasing the renderer.
  InitializeProtocolTarget(target);
}

void zebkit_view::Send(const std::string& method,
                       base::DictValue params,
                       Reply reply) {
  if (method == "Page.removeScriptToEvaluateOnNewDocument") {
    const auto* identifier = params.FindString("identifier");
    constexpr std::string_view prefix = "zebkit-init-";
    if (identifier && identifier->starts_with(prefix)) {
      size_t index;
      if (!base::StringToSizeT(std::string_view(*identifier).substr(prefix.size()), &index)) {
        if (reply) std::move(reply).Run(nullptr, "invalid initialization script identifier");
      } else RemoveProtocolScript(index, std::move(reply));
      return;
    }
  }
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
  const std::string target = content::DevToolsAgentHost::GetOrCreateFor(web_contents())->GetId();
  if (method == "Page.addScriptToEvaluateOnNewDocument")
    protocol_scripts_applied_[target] = protocol_script_sources_.size();
  protocol_pending_targets_[id] = target;
  auto session = protocol_sessions_.find(target);
  // A held popup's target takes commands at once (its renderer runs them
  // while it waits, the browser's apply before its first request); only its
  // release waits (ReleaseStart()).
  const bool held = protocol_hold_start_ && session != protocol_sessions_.end() &&
                    !protocol_initializing_.contains(target);
  if (session == protocol_sessions_.end() || (!protocol_ready_targets_.contains(target) && !held)) {
    protocol_queued_.emplace_back(target, std::move(message));
    return;
  }
  message.Set("sessionId", session->second);
  DispatchCommand(std::move(message));
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
    root = base::MakeAbsoluteFilePath(zebkit::PathFromUTF8(files_root));
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

void zebkit_view::AddScript(const std::string& source, Reply reply, bool all_frames,
                            bool run_immediately) {
  if (all_frames) {
    protocol_all_frame_scripts_.insert(protocol_script_sources_.size());
    AttachFrames();
  }
  Send("Page.enable", base::DictValue());
  SendScript(source, std::move(reply), std::string(), run_immediately);
}

void zebkit_view::AttachFrames() {
  if (protocol_attach_frames_ || !EnsureSession()) return;
  protocol_attach_frames_ = true;
  std::vector<std::string> targets;
  for (const auto& [target, session] : protocol_sessions_) targets.push_back(target);
  auto weak = GetWeakPtr();
  for (const auto& target : targets) {
    if (!weak) return;
    AttachFrameTargets(target);
  }
}

void zebkit_view::SendToSession(const std::string& session, const std::string& method,
                                base::DictValue params, Reply reply) {
  std::string target;
  for (const auto& [id, value] : protocol_sessions_)
    if (value == session) target = id;
  if (target.empty()) {
    if (reply) std::move(reply).Run(nullptr, "the session is gone");
    return;
  }
  SendTargetCommand(target, method, std::move(params), std::move(reply));
}

// A prerender attached before a later script or user agent change.
void zebkit_view::RefreshOtherProtocolTargets(const std::string& current) {
  std::vector<std::string> others;
  for (const auto& [target, session] : protocol_sessions_)
    if (target != current) others.push_back(target);
  auto weak = GetWeakPtr();
  for (const auto& target : others)
    if (weak) InitializeProtocolTarget(target);
}

void zebkit_view::AddWorldScript(const std::string& world, const std::string& source,
                                 bool main_frame_only) {
  // As content scripts' all_frames: false. window and top cannot be replaced
  // by a page, and a script of a frame has them before anything else runs.
  const std::string script =
      main_frame_only ? "if (window === window.top) {\n" + source + "\n}" : source;
  if (!main_frame_only) {
    protocol_all_frame_scripts_.insert(protocol_script_sources_.size());
    AttachFrames();
  }
  Send("Page.enable", base::DictValue());
  SendScript(script, Reply(), world);
}

void zebkit_view::AttachFrameTargets(const std::string& target) {
  base::DictValue params;
  params.Set("autoAttach", true);
  params.Set("waitForDebuggerOnStart", true);
  params.Set("flatten", true);
  base::ListValue filter;
  base::DictValue frames;
  frames.Set("type", "iframe");
  frames.Set("exclude", false);
  filter.Append(std::move(frames));
  // Dedicated workers too, as crPage.ts attaches them (their console, their
  // handles); a worker's own session attaches its nested workers.
  base::DictValue workers;
  workers.Set("type", "worker");
  workers.Set("exclude", false);
  filter.Append(std::move(workers));
  base::DictValue rest;
  rest.Set("exclude", true);
  filter.Append(std::move(rest));
  params.Set("filter", std::move(filter));
  SendTargetCommand(target, "Target.setAutoAttach", std::move(params));
}

void zebkit_view::SendScript(const std::string& source, Reply reply, const std::string& world,
                             bool run_immediately) {
  const size_t index = protocol_script_sources_.size();
  protocol_script_sources_.push_back(source);
  if (!world.empty()) protocol_script_worlds_[index] = world;
  const std::string target = web_contents()
      ? content::DevToolsAgentHost::GetOrCreateFor(web_contents())->GetId() : "";
  base::DictValue script;
  script.Set("source", source);
  script.Set("runImmediately", run_immediately);
  if (!world.empty()) script.Set("worldName", world);
  ++scripts_pending_;
  // Answered, or failed when the page goes, either way once.
  Send("Page.addScriptToEvaluateOnNewDocument", std::move(script),
       base::BindOnce(
           [](base::WeakPtr<zebkit_view> view, std::string target, size_t index,
              Reply reply, const base::DictValue* result,
              const std::string& error) {
             const bool recorded = view && view->RecordProtocolScript(target, index, result);
             if (view && !recorded) {
               view->protocol_removed_scripts_.insert(index);
               std::string().swap(view->protocol_script_sources_[index]);
             }
             const std::string failure = recorded || !error.empty()
                 ? error : "script registration returned no identifier";
             if (reply) {
               base::DictValue response = result ? result->Clone() : base::DictValue();
               if (recorded) response.Set("identifier", "zebkit-init-" + base::NumberToString(index));
               std::move(reply).Run(result ? &response : nullptr, failure);
             }
             if (!view) return;
             if (recorded) view->RefreshOtherProtocolTargets(target);
             if (!view) return;
             --view->scripts_pending_;
             view->MaybeRunDeferredLoad();
           },
           weak_factory_.GetWeakPtr(), target, index, std::move(reply)));
}

void zebkit_view::LoadUrl(const GURL& url) {
  const bool attaching = devtools_ && !protocol_attach_failed_ && web_contents() &&
      !protocol_ready_targets_.contains(content::DevToolsAgentHost::GetOrCreateFor(web_contents())->GetId());
  if (scripts_pending_ > 0 || protocol_setups_pending_ > 0 || attaching) {
    deferred_load_ = url;
    return;
  }
  // A caller can choose a replacement navigation inside a setup completion.
  // Once it can start immediately, it supersedes any older deferred URL.
  deferred_load_.reset();
  if (zebkit::Window* s = window())
    s->LoadURL(url);
}

void zebkit_view::MaybeRunDeferredLoad() {
  if (scripts_pending_ || protocol_setups_pending_ || !deferred_load_ || !web_contents()) return;
  GURL url = std::move(*deferred_load_);
  deferred_load_.reset();
  LoadUrl(url);
}

void zebkit_view::SetUserAgent(const char* user_agent) {
  const std::string value = user_agent ? user_agent : "";
  // Match EmulationHandler validation before changing replay state. A rejected
  // override must not prevent initialization of a future prerender target.
  if (!net::HttpUtil::IsValidHeaderValue(value)) return;
  base::DictValue params;
  params.Set("userAgent", value);
  SetUserAgentOverride(std::move(params));
}

void zebkit_view::SetUserAgentOverride(base::DictValue params, Reply reply) {
  const size_t submission = ++protocol_user_agent_submission_;
  auto saved = params.Clone();
  ++protocol_setups_pending_;
  Send("Emulation.setUserAgentOverride", std::move(params),
      base::BindOnce([](base::WeakPtr<zebkit_view> view, size_t submission,
                       base::DictValue saved, Reply reply,
                       const base::DictValue* result, const std::string& error) {
        if (!view) return;
        // Persist only successfully validated overrides. In-flight replies
        // must not replace a newer successful setting with an older one.
        if (error.empty() && submission > view->protocol_user_agent_committed_) {
          view->protocol_user_agent_committed_ = submission;
          view->protocol_user_agent_ = std::move(saved);
          ++view->protocol_user_agent_revision_;
          if (view->web_contents())
            view->protocol_user_agent_applied_[content::DevToolsAgentHost::GetOrCreateFor(view->web_contents())->GetId()] =
                view->protocol_user_agent_revision_;
        }
        --view->protocol_setups_pending_;
        if (error.empty() && view->web_contents())
          view->RefreshOtherProtocolTargets(
              content::DevToolsAgentHost::GetOrCreateFor(view->web_contents())->GetId());
        if (!view) return;
        // Finish the caller before releasing a deferred load, so it can
        // observe validation failure and change the next navigation.
        if (reply) std::move(reply).Run(result, error);
        if (!view || view->protocol_closing_) return;
        view->MaybeRunDeferredLoad();
      }, GetWeakPtr(), submission, std::move(saved), std::move(reply)));
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
  // The page's own JSON, when it had no bytes, as it is.
  const std::string args =
      call.args_json
          ? std::move(*call.args_json)
          : base::WriteJson(
                zebkit::ToPageValue(base::Value(std::move(call.args))))
                .value_or("[]");
  auto* pending = new zebkit_call;
  pending->reply = std::move(call.reply);
  pending->text_reply = std::move(call.text_reply);
  it->second.func(this, call.origin.c_str(), args.c_str(), pending,
                  it->second.data);
}

void zebkit_view::DispatchProtocolMessage(content::DevToolsAgentHost* host,
                                          base::span<const uint8_t> message) {
  if (protocol_closing_) return;
  std::optional<base::Value> parsed = base::JSONReader::Read(
      std::string_view(reinterpret_cast<const char*>(message.data()),
                       message.size()),
      base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!parsed || !parsed->is_dict())
    return;
  base::DictValue& reply = parsed->GetDict();
  if (const auto* method = reply.FindString("method")) {
    const auto* params = reply.FindDict("params");
    if (!params) return;
    const std::string* from = reply.FindString("sessionId");
    const std::string event_session = from ? *from : std::string();
    if (*method == "Target.attachedToTarget") {
      const auto* session = params->FindString("sessionId");
      const auto* info = params->FindDict("targetInfo");
      const auto* target = info ? info->FindString("targetId") : nullptr;
      if (session && target) {
        protocol_sessions_[*target] = *session;
        const auto* type = info->FindString("type");
        if (type && *type == "iframe") protocol_frame_targets_.insert(*target);
        if (type && *type == "worker") protocol_worker_targets_.insert(*target);
        // The handler's commands to the new target (SendToSession) go before
        // it is released below, as crPage.ts initializes a frame's session.
        auto weak = GetWeakPtr();
        if (protocol_events_) protocol_events_.Run(*method, *params, event_session);
        if (!weak) return;
        if (!protocol_sessions_.contains(*target)) return;
        if (protocol_attach_frames_) AttachFrameTargets(*target);
        InitializeProtocolTarget(*target);
        return;
      }
    }
    if (protocol_events_) protocol_events_.Run(*method, *params, event_session);
    if (*method == "Target.detachedFromTarget") {
      const auto* session = params->FindString("sessionId");
      if (!session) return;
      std::string target;
      for (auto it = protocol_sessions_.begin(); it != protocol_sessions_.end(); ++it) {
        if (it->second == *session) { target = it->first; protocol_sessions_.erase(it); break; }
      }
      if (target.empty()) return;
      protocol_ready_targets_.erase(target);
      protocol_frame_targets_.erase(target);
      protocol_worker_targets_.erase(target);
      protocol_initializing_.erase(target);
      protocol_scripts_applied_.erase(target);
      protocol_script_ids_.erase(target);
      protocol_user_agent_applied_.erase(target);
      std::vector<Reply> failed;
      for (auto it = protocol_pending_targets_.begin(); it != protocol_pending_targets_.end();) {
        if (it->second != target) { ++it; continue; }
        auto callback = pending_.find(it->first);
        if (callback != pending_.end()) {
          failed.push_back(std::move(callback->second));
          pending_.erase(callback);
        }
        it = protocol_pending_targets_.erase(it);
      }
      std::erase_if(protocol_queued_, [&](const auto& command) { return command.first == target; });
      for (auto& callback : failed)
        std::move(callback).Run(nullptr, "the page target was detached");
    }
    return;
  }
  if (std::optional<int> id = reply.FindInt("id")) {
    if (*id == protocol_auto_attach_id_) {
      protocol_auto_attach_id_ = 0;
      if (reply.FindDict("error")) {
        protocol_attach_failed_ = true;
        FailPending("the page tab session could not be initialized");
      }
      return;
    }
    protocol_pending_targets_.erase(*id);
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
  if (protocol_closing_) return;
  devtools_ = nullptr;
  protocol_sessions_.clear();
  protocol_frame_targets_.clear();
  protocol_worker_targets_.clear();
  protocol_ready_targets_.clear();
  protocol_initializing_.clear();
  protocol_scripts_applied_.clear();
  protocol_script_ids_.clear();
  protocol_user_agent_applied_.clear();
  protocol_auto_attach_id_ = 0;
  protocol_attach_failed_ = false;
  bridge_installed_ = false;
  FailPending("the page was closed");
}

void zebkit_view::OnDidAddMessageToConsole(content::RenderFrameHost* frame,
    blink::mojom::ConsoleMessageLevel level, const std::u16string& message,
    int32_t line, const std::u16string& source,
    const std::optional<std::u16string>& stack) {
  if (!on_console_) return;
  if (console_pending_ >= 256) { ++console_dropped_; return; }
  const char* name = "info";
  switch (level) {
    case blink::mojom::ConsoleMessageLevel::kVerbose: name = "debug"; break;
    case blink::mojom::ConsoleMessageLevel::kWarning: name = "warning"; break;
    case blink::mojom::ConsoleMessageLevel::kError: name = "error"; break;
    case blink::mojom::ConsoleMessageLevel::kInfo: break;
  }
  bool truncated = false;
  auto text = [&truncated](const std::u16string& value, size_t limit) {
    size_t count = std::min(value.size(), limit);
    if (count < value.size()) {
      truncated = true;
      if (count && value[count - 1] >= 0xd800 && value[count - 1] <= 0xdbff) --count;
    }
    return base::UTF16ToUTF8(std::u16string_view(value).substr(0, count));
  };
  base::DictValue event;
  event.Set("level", name); event.Set("message", text(message, 65536));
  event.Set("source", text(source, 16384)); event.Set("line", line);
  event.Set("frameId", frame ? "f" + base::NumberToString(frame->GetFrameTreeNodeId().value()) : "");
  event.Set("mainFrame", frame && frame->IsInPrimaryMainFrame());
  event.Set("origin", frame ? frame->GetLastCommittedOrigin().Serialize() : "null");
  // A first character of 1 marks console.error("Uncaught x") (patches/1071).
  if (stack) event.Set("stackTrace", text(stack->starts_with(u'\x01') ? stack->substr(1) : *stack, 65536));
  else event.Set("stackTrace", base::Value());
  event.Set("truncated", truncated);
  ++console_pending_;
  content::GetUIThreadTaskRunner({})->PostTask(FROM_HERE, base::BindOnce(
      [](base::WeakPtr<zebkit_view> view, uint64_t generation, base::DictValue event) {
        if (!view || view->console_generation_ != generation || !view->on_console_) return;
        --view->console_pending_;
        event.Set("dropped", base::NumberToString(view->console_dropped_));
        view->console_dropped_ = 0;
        const std::string json = base::WriteJson(event).value_or("{}");
        auto function = view->on_console_; void* data = view->on_console_data_;
        function(view.get(), json.c_str(), data);
      }, GetWeakPtr(), console_generation_, std::move(event)));
}

void zebkit_view::DidStartNavigation(content::NavigationHandle* navigation) {
  if (navigation->IsInPrimaryMainFrame() && !navigation->IsSameDocument()) {
    auto weak = GetWeakPtr();
    zebkit::CancelHttpAuthRequests(this, "navigation");
    if (!weak) return;
    zebkit::CancelCertificateRequests(this, "navigation");
    if (!weak) return;
  }
  if (navigation->IsInPrimaryMainFrame() && !navigation->IsSameDocument())
    Load(ZEBKIT_LOAD_STARTED, navigation->GetURL());
}

void zebkit_view::DidFinishNavigation(content::NavigationHandle* navigation) {
  if (navigation->IsInPrimaryMainFrame() && !navigation->IsSameDocument()) {
    auto weak = GetWeakPtr();
    zebkit::FinishCertificateNavigation(this, navigation->GetURL());
    if (!weak) return;
  }
  if (navigation->HasCommitted()) {
    auto weak = GetWeakPtr();
    zebkit::CheckPermissionDocuments(this);
    if (!weak) return;
    zebkit::PageHooksFrameGone(this, nullptr);
    if (!weak) return;
  }
  if (!navigation->IsInPrimaryMainFrame() || navigation->IsSameDocument())
    return;
  if (navigation->HasCommitted() && !navigation->IsErrorPage())
    Load(ZEBKIT_LOAD_COMMITTED, navigation->GetURL());
  else
    Load(ZEBKIT_LOAD_FAILED, navigation->GetURL());
}

void zebkit_view::RenderFrameDeleted(content::RenderFrameHost* frame) {
  zebkit::PermissionFrameDeleted(this, frame);
  zebkit::PageHooksFrameGone(this, frame);
}

void zebkit_view::DidFinishLoad(content::RenderFrameHost* frame,
                                const GURL& url) {
  if (frame->IsInPrimaryMainFrame()) {
    zebkit::SpareAfterLoad(web_contents()->GetBrowserContext());
    Load(ZEBKIT_LOAD_FINISHED, url);
  }
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
  protocol_closing_ = true;
  zebkit::CancelHttpAuthRequests(this, "cancelled");
  zebkit::CancelNavigationRequests(this, "cancelled");
  zebkit::CancelPermissionRequests(this, "cancelled");
  zebkit::CancelCertificateRequests(this, "cancelled");
  zebkit::CancelPageHookRequests(this);
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
  protocol_pending_targets_.clear();
  protocol_queued_.clear();
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
                 Window* popup,
                 const GURL& url,
                 WindowOpenDisposition disposition,
                 const blink::mojom::WindowFeatures& features,
                 bool user_gesture) {
  zebkit_view* view = ViewOf(opener);
  if (!view || !popup)
    return;
  // Always a view, with or without a window-open handler: without one the
  // popup would have no policy at all, and window.open() would escape it.
  zebkit_view* made = new zebkit_view(popup);
  made->policy_opener_ = view->GetWeakPtr();
  made->inherits_policy_ = true;
  made->popup_policy_ = view->popup_policy_;
  made->deny_downloads_ = view->deny_downloads_;
  made->navigation_fail_closed_ = view->navigation_fail_closed_;
  made->permission_timeout_ms_ = view->permission_timeout_ms_;
  made->navigation_timeout_ms_ = view->navigation_timeout_ms_;
  made->certificate_timeout_ms_ = view->certificate_timeout_ms_;
  made->http_auth_timeout_ms_ = view->http_auth_timeout_ms_;
  MakeOffscreenLike(view, made, features);
  // Held while automation sets it up, then released after what that posted.
  const bool held = PopupStartsHeld();
  if (held) {
    made->HoldStart();
  }
  if (PopupHoldFunc hook = PopupHoldHook())
    hook(nullptr, false);
  SetWindowOpenInfo(view, made, url, disposition, features, user_gesture);
  base::WeakPtr<zebkit_view> weak = made->GetWeakPtr();
  if (view->on_window_open_)
    view->on_window_open_(view, made, url.spec().c_str(), view->on_window_open_data_);
  if (held) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(
                       [](base::WeakPtr<zebkit_view> view) {
                         base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
                             FROM_HERE, base::BindOnce(
                                            [](base::WeakPtr<zebkit_view> view) {
                                              if (view) view->ReleaseStart();
                                            },
                                            view));
                       },
                       weak));
  }
}

bool& PopupStartsHeld() {
  static bool held = false;
  return held;
}

PopupHoldFunc& PopupHoldHook() {
  static PopupHoldFunc hook = nullptr;
  return hook;
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

// What Chrome's ProfileDestroyer does: a closed context's profile goes when its
// pages and their processes have, which is after they are closed, as content wants
// it until then. @tries, at 100 ms: a process that does not go keeps the profile
// (never deleted under it), and a task the engine's end drops loses only memory.
void DestroyProfileWhenUnused(Profile* profile, int tries) {
  bool used = false;
  for (Window* window : Window::windows()) {
    if (window->web_contents() && window->web_contents()->GetBrowserContext() == profile)
      used = true;
  }
  if (!used) {
    // Lets go of the processes' keep-alives and of a spare.
    profile->NotifyWillBeDestroyed();
    for (auto hosts = content::RenderProcessHost::AllHostsIterator(); !hosts.IsAtEnd();
         hosts.Advance()) {
      if (hosts.GetCurrentValue()->GetBrowserContext() == profile)
        used = true;
    }
  }
  if (!used) {
    ForgetAutomationContext(profile);
    ForgetContextCookies(profile);
    ForgetWorkerPolicy(profile);
    delete profile;
    return;
  }
  if (tries > 0) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, base::BindOnce(&DestroyProfileWhenUnused, profile, tries - 1),
        base::Milliseconds(100));
  }
}

}  // namespace zebkit

extern "C" {

zebkit_context* zebkit_context_new(void) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  auto* context = new zebkit_context;
  context->profile =
      std::make_unique<zebkit::Profile>(/*off_the_record=*/true);
  zebkit::ConfigureContext(context->profile.get());
  return context;
}

void zebkit_context_close(zebkit_context* context) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!context)
    return;
  std::vector<zebkit_view*> views;
  for (zebkit_view* view : zebkit_view::Views()) {
    if (view->web_contents() &&
        view->web_contents()->GetBrowserContext() == context->profile.get()) {
      views.push_back(view);
    }
  }
  for (zebkit_view* view : views)
    view->Close();
  zebkit::DestroyProfileWhenUnused(context->profile.release(), 300);
  delete context;
}

zebkit_view* zebkit_view_new(const char* url, int width, int height) {
  return zebkit_view_new_in(nullptr, url, width, height);
}

zebkit_view* zebkit_view_new_in(zebkit_context* context,
                                const char* url,
                                int width,
                                int height) {
  return zebkit::NewView(context, url, width, height, zebkit::Window::Kind::kNative);
}

}  // extern "C"

namespace zebkit {

zebkit_view* NewView(zebkit_context* context,
                     const char* url,
                     int width,
                     int height,
                     Window::Kind kind) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (context && !context->profile)
    return nullptr;
  const bool sized = width > 0 && height > 0;
  zebkit::Window* window = zebkit::Window::Create(
      zebkit::ProfileOf(context), GURL(), nullptr,
      gfx::Size(sized ? width : 0, sized ? height : 0), kind);
  if (!window)
    return nullptr;
  zebkit_view* view = new zebkit_view(window);
  // Without a URL, about:blank: it gives the view a renderer, which the
  // scripts for new documents are registered with, but its loading is not
  // the application's to hear of (a new WebContents has no navigation).
  view->quiet_blank_ = !(url && *url);
  window->LoadURL(GURL(url && *url ? url : "about:blank"));
  return view;
}

bool HasNoDisplay(content::WebContents* contents) {
  if (zebkit_is_headless())
    return true;
  Window* window = Window::FromWebContents(contents);
  return window && window->kind() == Window::Kind::kOffscreen;
}

}  // namespace zebkit

extern "C" {

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
  if (!zebkit::SetAttachedNativeBounds(view, gfx::Rect(x, y, width, height)))
    zebkit::SetAttachedBounds(view, gfx::Rect(x, y, width, height));
}

void zebkit_view_set_visible(zebkit_view* view, bool visible) {
  if (!zebkit::SetAttachedNativeVisible(view, visible))
    zebkit::SetAttachedVisible(view, visible);
}

bool zebkit_view_attach_native(zebkit_view* view,
                               uintptr_t native_parent,
                               int x,
                               int y,
                               int width,
                               int height,
                               char** error) {
  std::string failure;
  if (!content::BrowserThread::CurrentlyOn(content::BrowserThread::UI))
    failure = "call it on the thread that started the engine";
  else if (!view || !view->web_contents())
    failure = "the view is closed";
  else if (!native_parent)
    failure = "no window was given";
  else if (view->offscreen_view_)
    failure = "an off-screen view has no window to parent; its frames are the application's to draw";
  else if (zebkit::AttachViewNative(view, native_parent, gfx::Rect(x, y, width, height), &failure))
    return true;
  if (error)
    *error = strdup(failure.c_str());
  return false;
}

bool zebkit_view_get_native_window(zebkit_view* view, zebkit_native_window* window) {
  if (!window)
    return false;
  *window = {};
  if (!content::BrowserThread::CurrentlyOn(content::BrowserThread::UI) ||
      !view || !view->web_contents() || view->offscreen_view_ || zebkit_is_headless())
    return false;
  const auto result = zebkit::NativeWindowForView(view);
  if (result.kind == ZEBKIT_NATIVE_WINDOW_NONE || !result.handle)
    return false;
  *window = result;
  return true;
}

bool zebkit_view_export_native_window(zebkit_view* view,
                                      zebkit_native_window_export_func callback,
                                      void* data) {
  if (!callback || !view ||
      !content::BrowserThread::CurrentlyOn(content::BrowserThread::UI))
    return false;
  // Once, after this returns, on this thread, also when the platform drops
  // the request (a window closed before the compositor answered).
  auto done = mojo::WrapCallbackWithDefaultInvokeIfNotRun(
      base::BindPostTaskToCurrentDefault(base::BindOnce(
          [](zebkit_native_window_export_func callback, void* data,
             std::string handle) {
            callback(data, handle.empty() ? nullptr : handle.c_str());
          },
          callback, data)),
      std::string());
  if (!view->web_contents() || view->offscreen_view_ || zebkit_is_headless()) {
    std::move(done).Run(std::string());
    return true;
  }
  zebkit::ExportNativeWindowForView(view, std::move(done));
  return true;
}

bool zebkit_view_detach_native(zebkit_view* view) {
  return content::BrowserThread::CurrentlyOn(content::BrowserThread::UI) && view &&
         zebkit::DetachViewNative(view);
}

void zebkit_view_set_title(zebkit_view* view, const char* title) {
  if (title)
    view->window_title_ = title;
  else
    view->window_title_.reset();
  if (zebkit::Window* window = view->window())
    window->UpdateTitle();
}

void zebkit_view_set_size_limits(zebkit_view* view,
                                 int min_width,
                                 int min_height,
                                 bool resizable) {
  if (zebkit::Window* window = view->window()) {
    zebkit::SetWindowSizeLimits(
        window, gfx::Size(std::max(min_width, 0), std::max(min_height, 0)),
        resizable);
  }
}

void zebkit_view_focus(zebkit_view* view) {
  zebkit::Window* window = view->window();
  if (!window)
    return;
  if (!zebkit::FocusAttachedNative(view))
    zebkit::FocusWindow(window);
  else if (window->web_contents())
    window->web_contents()->Focus();
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
  view->SetUserAgent(user_agent);
}

bool zebkit_view_set_user_agent_override(zebkit_view* view, const char* json,
                                         zebkit_result_func func, void* data) {
  if (!content::BrowserThread::CurrentlyOn(content::BrowserThread::UI)) return false;
  if (!view || !view->web_contents()) {
    // Told, as for every other failure: its callers release what they gave it there.
    if (func) func(view, nullptr, "the view is closed", data);
    return false;
  }
  const size_t size = json ? strnlen(json, 65537) : 0;
  auto parsed = size && size <= 65536
      ? base::JSONReader::Read(std::string_view(json, size), base::JSON_PARSE_RFC)
      : std::nullopt;
  const char* error = nullptr;
  if (!parsed || !parsed->is_dict() || !parsed->GetDict().FindString("userAgent")) {
    error = "expected a JSON object with userAgent, at most 65536 UTF-8 bytes";
  } else {
    for (const auto entry : parsed->GetDict()) {
      if (entry.first != "userAgent" && entry.first != "acceptLanguage" && entry.first != "platform" &&
          entry.first != "userAgentMetadata") {
        error = "unknown user agent override option";
        break;
      }
    }
  }
  if (error) {
    if (func) func(view, nullptr, error, data);
    return false;
  }
  view->SetUserAgentOverride(std::move(parsed->GetDict()),
      base::BindOnce([](base::WeakPtr<zebkit_view> view, zebkit_result_func func,
                       void* data, const base::DictValue*, const std::string& error) {
        if (view && func)
          func(view.get(), error.empty() ? "{}" : nullptr,
               error.empty() ? nullptr : error.c_str(), data);
      }, view->GetWeakPtr(), func, data));
  return true;
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
  if (zebkit::SetAttachedNativeSize(view, gfx::Size(width, height)))
    return;
  if (zebkit::Window* window = view->window())
    window->ResizeContents(gfx::Size(width, height));
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
  if (!json) {
    std::move(call->reply).Run(true, base::Value());
    delete call;
    return;
  }
  zebkit_call_resolve_json(call, json, strlen(json));
}

void zebkit_call_resolve_json(zebkit_call* call, const char* json,
                              size_t length) {
  if (!json) {
    zebkit_call_resolve(call, nullptr);
    return;
  }
  const std::string_view text(json, length);
  // To a page as it is, when nothing in it is bytes.
  bool bytes = false;
  if (call->text_reply && zebkit::IsPlainJson(text, false, &bytes) &&
      !bytes) {
    std::move(call->text_reply).Run(true, text);
    delete call;
    return;
  }
  std::optional<base::Value> value =
      base::JSONReader::Read(text, base::JSON_PARSE_RFC);
  if (!value) {
    zebkit_call_reject(call, "the application answered with invalid JSON");
    return;
  }
  std::move(call->reply).Run(true, zebkit::FromPageValue(std::move(*value)));
  delete call;
}

void zebkit_call_reject(zebkit_call* call, const char* message) {
  std::move(call->reply).Run(false,
                             base::Value(message ? message : "rejected"));
  delete call;
}

}  // extern "C"

extern "C" bool zebkit_view_add_world_script(zebkit_view* view, const char* world,
                                             const char* source, bool main_frame_only,
                                             char** error) {
  auto fail = [&](const char* message) { if (error) *error = strdup(message); return false; };
  if (!view || !view->web_contents()) return fail("the view is closed");
  if (!source || !base::IsStringUTF8(source)) return fail("the script must be UTF-8");
  const std::string name = world ? world : "";
  if (name.size() > 256 || !base::IsStringUTF8(name)) return fail("a world's name is UTF-8 of at most 256 bytes");
  view->AddWorldScript(name, source, main_frame_only);
  return true;
}

extern "C" void zebkit_view_on_console(zebkit_view* view, zebkit_console_func function, void* data) {
  if (!content::BrowserThread::CurrentlyOn(content::BrowserThread::UI) || !view || !view->web_contents()) return;
  ++view->console_generation_;
  view->console_pending_ = 0; view->console_dropped_ = 0;
  view->on_console_ = function; view->on_console_data_ = data;
}
