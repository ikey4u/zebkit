// The engine's DevTools manager delegate, with what
// Chrome's own adds and content has not:
//
// - Page.printToPDF, as headless Chrome serves it (print.cc), its stream
//   included (transferMode ReturnAsStream, read with IO.read);
// - Zebkit.media*, a page's media list and captures (media_capture.cc);
// - which clients are attached to each page, so that a page's dialogs wait
//   for a CDP client that may answer them (dialog.cc), as in headless
//   Chrome, rather than being dismissed;
// - browser contexts: Target.createBrowserContext, disposeBrowserContext and
//   getBrowserContexts, and Target.createTarget in one of them (which content
//   leaves to the embedder, as headless Chrome's own target handler does),
//   each a profile in memory (zebkit_context_new());
// - Browser.getWindowForTarget, getWindowBounds, setWindowBounds.

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted_memory.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/values.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/devtools_agent_host_client_channel.h"
#include "content/public/browser/devtools_manager_delegate.h"
#include "content/public/browser/web_contents.h"
#include "zebkit/engine/window.h"
#include "third_party/inspector_protocol/crdtp/dispatch.h"
#include "third_party/inspector_protocol/crdtp/json.h"
#include "third_party/inspector_protocol/crdtp/span.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "url/gurl.h"
#include "url/url_constants.h"
#include "base/command_line.h"
#include "base/task/sequenced_task_runner.h"
#include "zebkit/engine/include/zebkit/zebkit.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/media_capture.h"
#include "zebkit/engine/page_internal.h"
#include "zebkit/engine/view.h"

namespace zebkit {

namespace {

// Every session attached anywhere, the engine's own views' among them.
std::set<content::DevToolsAgentHostClientChannel*>& Channels() {
  static base::NoDestructor<std::set<content::DevToolsAgentHostClientChannel*>>
      channels;
  return *channels;
}

bool IsViewSession(content::DevToolsAgentHostClientChannel* channel) {
  content::DevToolsAgentHostClient* client = channel->GetRootClient();
  for (zebkit_view* view : zebkit_view::Views()) {
    if (static_cast<content::DevToolsAgentHostClient*>(view) == client)
      return true;
  }
  return false;
}

void Reply(content::DevToolsAgentHostClientChannel* channel,
           base::DictValue message) {
  const std::string json = base::WriteJson(message).value_or("{}");
  std::vector<uint8_t> cbor;
  if (!crdtp::json::ConvertJSONToCBOR(crdtp::SpanFrom(json), &cbor).ok())
    return;
  channel->DispatchProtocolMessageToClient(std::move(cbor));
}

void ReplyError(content::DevToolsAgentHostClientChannel* channel,
                int id,
                int code,
                const std::string& text) {
  base::DictValue error;
  error.Set("code", code);
  error.Set("message", text);
  base::DictValue message;
  message.Set("id", id);
  message.Set("error", std::move(error));
  Reply(channel, std::move(message));
}

class DevToolsManagerDelegate : public content::DevToolsManagerDelegate {
 public:
  explicit DevToolsManagerDelegate(content::BrowserContext* context)
      : browser_context_(context) {}

  content::BrowserContext* GetDefaultBrowserContext() override {
    return browser_context_;
  }

  scoped_refptr<content::DevToolsAgentHost> CreateNewTarget(
      const GURL& url,
      TargetType target_type,
      bool new_window) override {
    Window* window = Window::Create(browser_context_, url, nullptr, Window::DefaultSize());
    return target_type == kTab
               ? content::DevToolsAgentHost::GetOrCreateForTab(window->web_contents())
               : content::DevToolsAgentHost::GetOrCreateFor(window->web_contents());
  }

  // The page at CDP's HTTP address.
  std::string GetDiscoveryPageHTML() override {
    return R"(<!DOCTYPE html><title>Remote debugging</title>
<p>This is the CDP endpoint of an application on zebkit. Its targets are at
<a href="/json/list">/json/list</a>.)";
  }

  // The front end is not in the package: /json/list names the hosted one of this
  // version, as Chrome's builds without it do.
  bool HasBundledFrontendResources() override { return false; }

  void ClientAttached(content::DevToolsAgentHostClientChannel* channel) override {
    Channels().insert(channel);
  }

  void ClientDetached(content::DevToolsAgentHostClientChannel* channel) override {
    Channels().erase(channel);
    OnMediaClientDetached(channel);
  }

  void HandleCommand(content::DevToolsAgentHostClientChannel* channel,
                     base::span<const uint8_t> message,
                     NotHandledCallback callback) override {
    crdtp::Dispatchable dispatchable(
        crdtp::SpanFrom(message), std::string_view(),
        [](int, crdtp::span<uint8_t>, crdtp::span<uint8_t>,
           std::string_view) {});
    if (dispatchable.ok()) {
      const std::string_view method(
          reinterpret_cast<const char*>(dispatchable.Method().data()),
          dispatchable.Method().size());
      // Media capture (media_capture.cc).
      if (IsMediaCommand(method)) {
        // ConvertCBORToJSON appends to what the string has.
        std::string params;
        if (dispatchable.Params().empty() ||
            !crdtp::json::ConvertCBORToJSON(dispatchable.Params(), &params)
                 .ok()) {
          params = "{}";
        }
        HandleMediaCommand(channel, dispatchable.CallId(), method, params);
        return;
      }
    }
    if (dispatchable.ok() &&
        crdtp::SpanEquals(crdtp::SpanFrom("Page.printToPDF"),
                          dispatchable.Method())) {
      PrintToPdfCommand(channel, dispatchable);
      return;
    }
    if (dispatchable.ok() &&
        crdtp::SpanEquals(crdtp::SpanFrom("Target.createTarget"),
                          dispatchable.Method()) &&
        CreateTargetInContext(channel, dispatchable)) {
      return;
    }
    if (dispatchable.ok() && WindowCommand(channel, dispatchable))
      return;
    // Browser.setDockTile: Chrome's browser handler takes it on every platform
    // and shows it only in macOS's Dock (Playwright's recorder sets its icon);
    // an engine in an application's process has no Dock tile of its own.
    if (dispatchable.ok() &&
        crdtp::SpanEquals(crdtp::SpanFrom("Browser.setDockTile"), dispatchable.Method())) {
      base::DictValue reply;
      reply.Set("id", dispatchable.CallId());
      reply.Set("result", base::DictValue());
      Reply(channel, std::move(reply));
      return;
    }
    // Browser.close, as Chrome's browser handler has it, for a host that is a
    // browser launched by its client (zebkit-run serving a CDP pipe asks for
    // it): the engine quits. Another host's engine is not a CDP client's to end.
    if (dispatchable.ok() &&
        crdtp::SpanEquals(crdtp::SpanFrom("Browser.close"), dispatchable.Method()) &&
        base::CommandLine::ForCurrentProcess()->HasSwitch("zebkit-quit-on-browser-close")) {
      base::DictValue reply;
      reply.Set("id", dispatchable.CallId());
      reply.Set("result", base::DictValue());
      Reply(channel, std::move(reply));
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, base::BindOnce([] { zebkit_quit(); }));
      return;
    }
    std::move(callback).Run(message);
  }

  std::vector<base::WeakPtr<content::BrowserContext>> GetBrowserContexts()
      override {
    std::vector<base::WeakPtr<content::BrowserContext>> contexts;
    for (const auto& [profile, context] : contexts_)
      contexts.push_back(profile->GetWeakPtr());
    return contexts;
  }

  content::BrowserContext* GetBrowserContext(
      const std::string& context_id) override {
    for (const auto& [profile, context] : contexts_) {
      if (profile->UniqueId() == context_id)
        return profile;
    }
    return nullptr;
  }

  content::BrowserContext* CreateBrowserContext() override {
    zebkit_context* context = zebkit_context_new();
    content::BrowserContext* profile = ProfileOf(context);
    contexts_[profile] = context;
    return profile;
  }

  void DisposeBrowserContext(content::BrowserContext* profile,
                             DisposeCallback callback) override {
    auto found = contexts_.find(profile);
    if (found == contexts_.end()) {
      std::move(callback).Run(false, "Failed to find context");
      return;
    }
    zebkit_context* context = found->second;
    contexts_.erase(found);
    // Its pages, Target.createTarget's and their popups: windows without a
    // view, which zebkit_context_close() does not know of.
    // Each from the list as it is now: closing one can close another (a host's
    // on_close), which a copy would still point to.
    std::set<Window*> closed;
    for (bool found = true; found;) {
      found = false;
      for (Window* window : Window::windows()) {
        if (!closed.contains(window) && window->web_contents() &&
            window->web_contents()->GetBrowserContext() == profile) {
          closed.insert(window);
          found = true;
          window->Close();
          break;
        }
      }
    }
    zebkit_context_close(context);
    std::move(callback).Run(true, std::string());
  }

 private:
  // Browser.getWindowForTarget, getWindowBounds and setWindowBounds, as Chrome
  // serves them: an id of its own for each window. Playwright over CDP sizes
  // the window of each new page with them.
  bool WindowCommand(content::DevToolsAgentHostClientChannel* channel,
                     const crdtp::Dispatchable& dispatchable) {
    const bool for_target = crdtp::SpanEquals(
        crdtp::SpanFrom("Browser.getWindowForTarget"), dispatchable.Method());
    const bool get_bounds = crdtp::SpanEquals(
        crdtp::SpanFrom("Browser.getWindowBounds"), dispatchable.Method());
    const bool set_bounds = crdtp::SpanEquals(
        crdtp::SpanFrom("Browser.setWindowBounds"), dispatchable.Method());
    if (!for_target && !get_bounds && !set_bounds)
      return false;
    base::DictValue params;
    std::string json;
    if (!dispatchable.Params().empty() &&
        crdtp::json::ConvertCBORToJSON(dispatchable.Params(), &json).ok()) {
      std::optional<base::Value> parsed =
          base::JSONReader::Read(json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
      if (parsed && parsed->is_dict())
        params = std::move(parsed->GetDict());
    }
    const int id = dispatchable.CallId();
    Window* shell = nullptr;
    int window_id = 0;
    if (for_target) {
      const std::string* target = params.FindString("targetId");
      scoped_refptr<content::DevToolsAgentHost> host =
          target ? content::DevToolsAgentHost::GetForId(*target)
                 : scoped_refptr<content::DevToolsAgentHost>(channel->GetAgentHost());
      content::WebContents* contents = host ? host->GetWebContents() : nullptr;
      if (!contents) {
        ReplyError(channel, id, -32602, "No target with given id found");
        return true;
      }
      for (Window* window : Window::windows()) {
        if (window->web_contents() == contents)
          shell = window;
      }
      if (!shell) {
        ReplyError(channel, id, -32000, "Browser window not found");
        return true;
      }
      auto [found, added] = window_ids_.try_emplace(shell, next_window_id_);
      if (added)
        ++next_window_id_;
      window_id = found->second;
    } else {
      window_id = params.FindInt("windowId").value_or(0);
      for (const auto& [window, known] : window_ids_) {
        if (known == window_id && std::ranges::contains(Window::windows(), window))
          shell = window;
      }
      if (!shell) {
        ReplyError(channel, id, -32000, "Browser window not found");
        return true;
      }
    }
    if (set_bounds) {
      const base::DictValue* bounds = params.FindDict("bounds");
      const std::optional<int> width = bounds ? bounds->FindInt("width") : std::nullopt;
      const std::optional<int> height = bounds ? bounds->FindInt("height") : std::nullopt;
      if (width && height)
        shell->ResizeContents(gfx::Size(*width, *height));
    }
    base::DictValue result;
    if (for_target)
      result.Set("windowId", window_id);
    if (!set_bounds) {
      const gfx::Rect rect = shell->web_contents()->GetContainerBounds();
      base::DictValue bounds;
      bounds.Set("left", rect.x());
      bounds.Set("top", rect.y());
      bounds.Set("width", rect.width());
      bounds.Set("height", rect.height());
      bounds.Set("windowState", "normal");
      result.Set("bounds", std::move(bounds));
    }
    base::DictValue answer;
    answer.Set("id", id);
    answer.Set("result", std::move(result));
    Reply(channel, std::move(answer));
    return true;
  }

  // Target.createTarget with a browserContextId of ours; false for the rest,
  // which content's handler serves.
  bool CreateTargetInContext(content::DevToolsAgentHostClientChannel* channel,
                             const crdtp::Dispatchable& dispatchable) {
    std::string json;
    if (dispatchable.Params().empty() ||
        !crdtp::json::ConvertCBORToJSON(dispatchable.Params(), &json).ok()) {
      return false;
    }
    std::optional<base::Value> params =
        base::JSONReader::Read(json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
    if (!params || !params->is_dict())
      return false;
    const base::DictValue& dict = params->GetDict();
    const std::string* context_id = dict.FindString("browserContextId");
    if (!context_id || dict.FindBool("hidden").value_or(false))
      return false;
    content::BrowserContext* profile = GetBrowserContext(*context_id);
    if (!profile) {
      if (GetDefaultBrowserContext() &&
          GetDefaultBrowserContext()->UniqueId() == *context_id) {
        return false;
      }
      ReplyError(channel, dispatchable.CallId(), -32602,
                 "Failed to find browser context with id " + *context_id);
      return true;
    }
    const std::string* url_text = dict.FindString("url");
    GURL url(url_text ? *url_text : std::string());
    if (url.is_empty())
      url = GURL(url::kAboutBlankURL);
    const int width = dict.FindInt("width").value_or(0);
    const int height = dict.FindInt("height").value_or(0);
    Window* shell = Window::Create(profile, url, nullptr,
                                   width > 0 && height > 0 ? gfx::Size(width, height)
                                                           : Window::DefaultSize());
    if (!shell) {
      ReplyError(channel, dispatchable.CallId(), -32000, "Not supported");
      return true;
    }
    scoped_refptr<content::DevToolsAgentHost> host =
        dict.FindBool("forTab").value_or(false)
            ? content::DevToolsAgentHost::GetOrCreateForTab(
                  shell->web_contents())
            : content::DevToolsAgentHost::GetOrCreateFor(
                  shell->web_contents());
    base::DictValue result;
    result.Set("targetId", host->GetId());
    base::DictValue answer;
    answer.Set("id", dispatchable.CallId());
    answer.Set("result", std::move(result));
    Reply(channel, std::move(answer));
    return true;
  }

  void PrintToPdfCommand(content::DevToolsAgentHostClientChannel* channel,
                         const crdtp::Dispatchable& dispatchable) {
    const int id = dispatchable.CallId();
    content::DevToolsAgentHost* host = channel->GetAgentHost();
    content::WebContents* contents = host ? host->GetWebContents() : nullptr;
    if (!contents || host->GetType() != content::DevToolsAgentHost::kTypePage) {
      ReplyError(channel, id, -32000, "Command can only be executed on a page");
      return;
    }
    std::string params_json = "{}";
    if (!dispatchable.Params().empty()) {
      std::string json;
      if (crdtp::json::ConvertCBORToJSON(dispatchable.Params(), &json).ok())
        params_json = std::move(json);
    }
    bool stream = false;
    if (std::optional<base::Value> params = base::JSONReader::Read(
            params_json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
        params && params->is_dict()) {
      if (const std::string* mode = params->GetDict().FindString("transferMode"))
        stream = *mode == "ReturnAsStream";
    }
    scoped_refptr<content::DevToolsAgentHost> keep(host);
    PrintToPdf(
        contents, params_json,
        base::BindOnce(
            [](base::WeakPtr<DevToolsManagerDelegate> self,
               content::DevToolsAgentHostClientChannel* channel,
               scoped_refptr<content::DevToolsAgentHost> host, int id,
               bool stream, scoped_refptr<base::RefCountedMemory> pdf,
               const std::string& error, bool invalid_params) {
              // The client may have gone while the page printed.
              if (!self || !Channels().contains(channel))
                return;
              if (!pdf) {
                ReplyError(channel, id,
                           invalid_params ? -32602 : -32000,
                           error);
                return;
              }
              base::DictValue result;
              if (stream) {
                result.Set("data", "");
                result.Set("stream", host->CreateIOStreamFromData(pdf));
              } else {
                result.Set("data", base::Base64Encode(*pdf));
              }
              base::DictValue message;
              message.Set("id", id);
              message.Set("result", std::move(result));
              Reply(channel, std::move(message));
            },
            weak_factory_.GetWeakPtr(), channel, std::move(keep), id, stream));
  }

  // Browser.getWindowForTarget's ids; a window gone stays, unmatched.
  std::map<Window*, int> window_ids_;
  const raw_ptr<content::BrowserContext> browser_context_;
  int next_window_id_ = 1;
  // Target.createBrowserContext's, until disposed.
  std::map<content::BrowserContext*, raw_ptr<zebkit_context>> contexts_;
  base::WeakPtrFactory<DevToolsManagerDelegate> weak_factory_{this};
};

}  // namespace

bool SendToDevToolsClient(content::DevToolsAgentHostClientChannel* channel,
                          base::DictValue message) {
  if (!Channels().contains(channel))
    return false;
  Reply(channel, std::move(message));
  return true;
}

bool HasExternalDevToolsClient(content::WebContents* contents) {
  for (content::DevToolsAgentHostClientChannel* channel : Channels()) {
    content::DevToolsAgentHost* host = channel->GetAgentHost();
    if (host && host->GetWebContents() == contents && !IsViewSession(channel))
      return true;
  }
  return false;
}

std::unique_ptr<content::DevToolsManagerDelegate>
BrowserClient::CreateDevToolsManagerDelegate() {
  return std::make_unique<DevToolsManagerDelegate>(GetBrowserContext());
}

}  // namespace zebkit
