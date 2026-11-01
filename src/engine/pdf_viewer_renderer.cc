// PDF in pages: Chromium's own viewer plugin (//pdf, PDFium in this renderer,
// never in the browser process) with zebkit's viewer (pdf_viewer.js) in place of
// Chrome's extension. docs/ROADMAP.md has the design.
//
// The plugin is told where to draw and which pages it may need, and tells the
// viewer about the document, by messages. Chrome's viewer exchanges them over
// the element's postMessage; here the viewer is in an isolated world of the PDF
// document, in a closed shadow root (patches/1066), and the messages go to it
// directly, so the page sees neither it nor them.

#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "base/values.h"
#include "components/pdf/renderer/internal_plugin_renderer_helpers.h"
#include "components/pdf/renderer/pdf_view_web_plugin_client.h"
#include "content/public/common/isolated_world_ids.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/v8_value_converter.h"
#include "gin/function_template.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "pdf/mojom/pdf.mojom.h"
#include "pdf/pdf_view_web_plugin.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_element.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_plugin_container.h"
#include "third_party/blink/public/web/web_plugin_params.h"
#include "third_party/blink/public/web/web_plugin_script_forbidden_scope.h"
#include "v8/include/v8.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/pdf_viewer_script.h"

namespace zebkit {
namespace {

// After automation's (automation_renderer.cc).
constexpr int32_t kViewerWorldId = content::ISOLATED_WORLD_ID_CONTENT_END + 2;

// The only embedder //pdf accepts besides print preview; nothing else of it is used.
constexpr char kEmbedderOrigin[] = "chrome-extension://mhjfbmdgcfjbbpaeojofohoefgiehjai/";

class ViewerClient final : public pdf::PdfViewWebPluginClient {
 public:
  explicit ViewerClient(content::RenderFrame* frame)
      : pdf::PdfViewWebPluginClient(frame), frame_(frame) {}

  void SetPluginContainer(blink::WebPluginContainer* container) override {
    pdf::PdfViewWebPluginClient::SetPluginContainer(container);
    if (container) {
      PostOpen();
    } else {
      receive_.Reset();
      pending_.clear();
    }
  }

  std::string GetEmbedderOriginString() override { return kEmbedderOrigin; }

  void PostMessage(base::DictValue message) override {
    pending_.push_back(std::move(message));
    PostFlush();
  }

  void ReportFindInPageMatchCount(int identifier, int total,
                                  bool final_update) override {
    PostMessage(base::DictValue()
                    .Set("type", "zebkitFindCount")
                    .Set("total", total)
                    .Set("final", final_update));
    pdf::PdfViewWebPluginClient::ReportFindInPageMatchCount(identifier, total,
                                                            final_update);
  }

  void ReportFindInPageSelection(int identifier, int ordinal,
                                 bool final_update) override {
    PostMessage(base::DictValue()
                    .Set("type", "zebkitFindSelection")
                    .Set("ordinal", ordinal));
    pdf::PdfViewWebPluginClient::ReportFindInPageSelection(identifier, ordinal,
                                                           final_update);
  }

 private:
  chrome_pdf::PdfViewWebPlugin* Plugin() {
    blink::WebPluginContainer* container = PluginContainer();
    return container ? static_cast<chrome_pdf::PdfViewWebPlugin*>(container->Plugin())
                     : nullptr;
  }

  void PostOpen() {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&ViewerClient::Open, weak_factory_.GetWeakPtr()));
  }

  void PostFlush() {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&ViewerClient::Flush, weak_factory_.GetWeakPtr()));
  }

  // The viewer's script, called with the shadow root and what it may call.
  void Open() {
    blink::WebPluginContainer* container = PluginContainer();
    if (!container || !Plugin() || !receive_.IsEmpty()) {
      return;
    }
    blink::WebLocalFrame* frame = frame_->GetWebFrame();
    v8::Isolate* isolate = GetIsolate();
    v8::HandleScope handles(isolate);
    v8::Local<v8::Context> context =
        frame->GetScriptContextFromWorldId(isolate, kViewerWorldId);
    if (context.IsEmpty()) {
      return;
    }
    v8::Context::Scope context_scope(context);
    v8::Local<v8::Script> script;
    v8::Local<v8::Value> value;
    if (!v8::Script::Compile(context,
                             v8::String::NewFromUtf8(
                                 isolate, kPdfViewerScript, v8::NewStringType::kNormal)
                                 .ToLocalChecked())
             .ToLocal(&script) ||
        !script->Run(context).ToLocal(&value) || !value->IsFunction()) {
      return;
    }
    blink::WebDocument document = frame->GetDocument();
    blink::WebElement body = document.Body();
    if (body.IsNull()) {
      return;
    }
    v8::Local<v8::Object> native = v8::Object::New(isolate);
    const auto set = [&](const char* name, v8::Local<v8::Value> function) {
      native
          ->Set(context,
                v8::String::NewFromUtf8(isolate, name, v8::NewStringType::kInternalized)
                    .ToLocalChecked(),
                function)
          .Check();
    };
    const auto weak = weak_factory_.GetWeakPtr();
    set("plugin", Plugin()->V8ScriptableObject(isolate));
    set("find", gin::CreateFunctionTemplate(
                    isolate, base::BindRepeating(&ViewerClient::Find, weak))
                    ->GetFunction(context)
                    .ToLocalChecked());
    set("findNext", gin::CreateFunctionTemplate(
                        isolate, base::BindRepeating(&ViewerClient::FindNext, weak))
                        ->GetFunction(context)
                        .ToLocalChecked());
    set("findStop", gin::CreateFunctionTemplate(
                        isolate, base::BindRepeating(&ViewerClient::FindStop, weak))
                        ->GetFunction(context)
                        .ToLocalChecked());
    set("loaded",
        gin::CreateFunctionTemplate(
            isolate, base::BindRepeating(
                         [](base::WeakPtr<ViewerClient> client) {
                           return client ? client->LoadedPages() : 0;
                         },
                         weak))
            ->GetFunction(context)
            .ToLocalChecked());
    set("painted",
        gin::CreateFunctionTemplate(
            isolate, base::BindRepeating(
                         [](base::WeakPtr<ViewerClient> client) {
                           return client ? client->PaintedPages() : 0;
                         },
                         weak))
            ->GetFunction(context)
            .ToLocalChecked());
    v8::Local<v8::Value> arguments[] = {
        body.OpenOrClosedShadowRoot().ToV8Value(isolate), native};
    v8::TryCatch try_catch(isolate);
    v8::Local<v8::Value> receive;
    if (!frame
             ->CallFunctionEvenIfScriptDisabled(value.As<v8::Function>(),
                                                context->Global(), 2, arguments)
             .ToLocal(&receive) ||
        !receive->IsFunction()) {
      return;
    }
    receive_.Reset(isolate, receive.As<v8::Function>());
    Flush();
  }

  void Flush() {
    if (receive_.IsEmpty() || !PluginContainer() ||
        blink::WebPluginScriptForbiddenScope::IsForbidden()) {
      return;
    }
    if (!PluginContainer()->GetDocument().IsActive()) {
      return;
    }
    blink::WebLocalFrame* frame = frame_->GetWebFrame();
    v8::Isolate* isolate = GetIsolate();
    v8::HandleScope handles(isolate);
    v8::Local<v8::Context> context =
        frame->GetScriptContextFromWorldId(isolate, kViewerWorldId);
    if (context.IsEmpty()) {
      return;
    }
    v8::Context::Scope context_scope(context);
    std::vector<base::DictValue> messages;
    messages.swap(pending_);
    auto converter = content::V8ValueConverter::Create();
    const base::WeakPtr<ViewerClient> alive = weak_factory_.GetWeakPtr();
    for (base::DictValue& message : messages) {
      v8::Local<v8::Value> argument = converter->ToV8Value(message, context);
      std::ignore = frame->CallFunctionEvenIfScriptDisabled(
          receive_.Get(isolate), context->Global(), 1, &argument);
      if (!alive || receive_.IsEmpty()) {
        return;
      }
    }
  }

  void Find(std::string text, bool case_sensitive) {
    if (auto* plugin = Plugin()) {
      plugin->StartFind(blink::WebString::FromUtf8(text), case_sensitive, 1);
    }
  }
  void FindNext(bool forward) {
    if (auto* plugin = Plugin()) {
      plugin->SelectFindResult(forward, 1);
    }
  }
  void FindStop() {
    if (auto* plugin = Plugin()) {
      plugin->StopFind();
    }
  }
  int LoadedPages() {
    auto* plugin = Plugin();
    return plugin ? plugin->LoadedPageCount() : 0;
  }
  int PaintedPages() {
    auto* plugin = Plugin();
    return plugin ? plugin->PaintedPageCount() : 0;
  }

  const raw_ptr<content::RenderFrame> frame_;
  v8::Global<v8::Function> receive_;
  std::vector<base::DictValue> pending_;
  base::WeakPtrFactory<ViewerClient> weak_factory_{this};
};

}  // namespace

// As ChromeContentRendererClient: the browser process kills a PDF renderer that
// asks for storage or a worker, which script in its document could otherwise do
// (a driver's, DevTools'). localStorage is null, Worker throws a SecurityError.
bool RendererClient::IsDomStorageDisabled() const {
  return pdf::IsPdfRenderer();
}

bool RendererClient::AreDedicatedWorkersDisabled() const {
  return pdf::IsPdfRenderer();
}

bool RendererClient::OverrideCreatePlugin(content::RenderFrame* render_frame,
                                          const blink::WebPluginParams& params,
                                          blink::WebPlugin** plugin) {
  if (params.mime_type != "application/pdf" && params.mime_type != "text/pdf") {
    return content::ShellContentRendererClient::OverrideCreatePlugin(
        render_frame, params, plugin);
  }
  blink::WebPluginParams viewer_params = params;
  viewer_params.attribute_names.push_back(blink::WebString::FromAscii("src"));
  viewer_params.attribute_values.push_back(params.url.GetString());
  viewer_params.attribute_names.push_back(blink::WebString::FromAscii("full-frame"));
  viewer_params.attribute_values.push_back(blink::WebString());
  // rgb(40,40,40), the document's background in Chrome.
  viewer_params.attribute_names.push_back(blink::WebString::FromAscii("background-color"));
  viewer_params.attribute_values.push_back(blink::WebString::FromAscii("4280821800"));
  // Only the document in the frame holds the plugin (patches/1066); nothing else
  // gets to ask for it. A <embed> elsewhere is a frame.
  if (!params.load_manually) {
    *plugin = nullptr;
    return true;
  }
  mojo::AssociatedRemote<pdf::mojom::PdfHost> host;
  render_frame->GetRemoteAssociatedInterfaces()->GetInterface(
      host.BindNewEndpointAndPassReceiver());
  *plugin = new chrome_pdf::PdfViewWebPlugin(
      std::make_unique<ViewerClient>(render_frame), std::move(host),
      std::move(viewer_params));
  return true;
}

}  // namespace zebkit
