// The engine's DevTools manager delegate: the content shell's, with what
// Chrome's own adds and content has not:
//
// - Page.printToPDF, as headless Chrome serves it (print.cc), its stream
//   included (transferMode ReturnAsStream, read with IO.read);
// - which clients are attached to each page, so that a page's dialogs wait
//   for a CDP client that may answer them (dialog.cc), as in headless
//   Chrome, rather than being dismissed.

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
#include "content/shell/browser/shell_content_browser_client.h"
#include "content/shell/browser/shell_devtools_manager_delegate.h"
#include "third_party/inspector_protocol/crdtp/dispatch.h"
#include "third_party/inspector_protocol/crdtp/json.h"
#include "third_party/inspector_protocol/crdtp/span.h"
#include "zebkit/engine/engine.h"
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
  content::DevToolsAgentHostClient* client = channel->GetClient();
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

class DevToolsManagerDelegate : public content::ShellDevToolsManagerDelegate {
 public:
  explicit DevToolsManagerDelegate(content::BrowserContext* context)
      : content::ShellDevToolsManagerDelegate(context) {}

  void ClientAttached(content::DevToolsAgentHostClientChannel* channel) override {
    Channels().insert(channel);
    content::ShellDevToolsManagerDelegate::ClientAttached(channel);
  }

  void ClientDetached(content::DevToolsAgentHostClientChannel* channel) override {
    Channels().erase(channel);
    content::ShellDevToolsManagerDelegate::ClientDetached(channel);
  }

  void HandleCommand(content::DevToolsAgentHostClientChannel* channel,
                     base::span<const uint8_t> message,
                     NotHandledCallback callback) override {
    crdtp::Dispatchable dispatchable(
        crdtp::SpanFrom(message), std::string_view(),
        [](int, crdtp::span<uint8_t>, crdtp::span<uint8_t>,
           std::string_view) {});
    if (dispatchable.ok() &&
        crdtp::SpanEquals(crdtp::SpanFrom("Page.printToPDF"),
                          dispatchable.Method())) {
      PrintToPdfCommand(channel, dispatchable);
      return;
    }
    content::ShellDevToolsManagerDelegate::HandleCommand(channel, message,
                                                         std::move(callback));
  }

 private:
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

  base::WeakPtrFactory<DevToolsManagerDelegate> weak_factory_{this};
};

}  // namespace

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
