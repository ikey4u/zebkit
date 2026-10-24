// Printing to PDF (<zebkit/page.h>, and CDP's Page.printToPDF through
// devtools.cc), as headless Chrome prints: components/printing's renderer
// helper lays the page out for print and draws it into a PDF with Skia's
// PDF back end, in the renderer; the browser asks for it with the
// parameters print_to_pdf parses from CDP's (PdfPrintJob). There is no
// print compositor: frames of other sites are not printed, as in headless
// Chrome.
//
// window.print() is no print dialog and no printer: it is reported as a
// dialog of type print (dialog.cc), and returns at once.

#ifdef UNSAFE_BUFFERS_BUILD
// The C API takes and returns C strings and arrays.
#pragma allow_unsafe_buffers
#endif

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/memory/ref_counted_memory.h"
#include "base/values.h"
#include "components/printing/browser/print_manager.h"
#include "components/printing/browser/print_to_pdf/pdf_print_job.h"
#include "components/printing/browser/print_to_pdf/pdf_print_result.h"
#include "components/printing/browser/print_to_pdf/pdf_print_utils.h"
#include "components/printing/common/print.mojom.h"
#include "components/printing/renderer/print_render_frame_helper.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_user_data.h"
#include "content/public/renderer/render_frame.h"
#include "mojo/public/cpp/bindings/message.h"
#include "printing/buildflags/buildflags.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_registry.h"
#include "third_party/blink/public/web/web_element.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/page_internal.h"
#include "zebkit/engine/view.h"

namespace zebkit {

namespace {

// The browser's side of a page's printing: printing to PDF for the
// application and CDP; window.print() reported, then refused.
class PrintManager : public printing::PrintManager,
                     public content::WebContentsUserData<PrintManager> {
 public:
  ~PrintManager() override = default;

  static PrintManager* For(content::WebContents* contents) {
    CreateForWebContents(contents);
    return FromWebContents(contents);
  }

  void PrintToPdf(content::RenderFrameHost* frame,
                  const std::string& page_ranges,
                  printing::mojom::PrintPagesParamsPtr params,
                  print_to_pdf::PdfPrintJob::PrintToPdfCallback callback) {
    print_to_pdf::PdfPrintJob::StartJob(web_contents(), frame,
                                        GetPrintRenderFrame(frame), page_ranges,
                                        std::move(params), std::move(callback));
  }

 private:
  friend class content::WebContentsUserData<PrintManager>;
  explicit PrintManager(content::WebContents* contents)
      : printing::PrintManager(contents),
        content::WebContentsUserData<PrintManager>(*contents) {}

  // printing::mojom::PrintManagerHost
  void GetDefaultPrintSettings(GetDefaultPrintSettingsCallback callback) override {
    // window.print(): the application hears of it, the page gets no printer.
    ReportPrintDialog(web_contents(), &CurrentTargetFrame());
    std::move(callback).Run(nullptr);
  }
  void ScriptedPrint(printing::mojom::ScriptedPrintParamsPtr params,
                     ScriptedPrintCallback callback) override {
    std::move(callback).Run(nullptr);
  }
#if BUILDFLAG(ENABLE_PRINT_PREVIEW)
  void GetPrintPreviewParams(GetPrintPreviewParamsCallback callback) override {
    mojo::ReportBadMessage("no print preview");
  }
  void SetupScriptedPrintPreview(
      SetupScriptedPrintPreviewCallback callback) override {
    std::move(callback).Run();
  }
  void ShowScriptedPrintPreview() override {}
  void RequestPrintPreview(
      printing::mojom::RequestPrintPreviewParamsPtr params) override {
    mojo::ReportBadMessage("no print preview");
  }
  void CheckForCancel(const base::UnguessableToken& preview_ui_id,
                      int32_t request_id,
                      CheckForCancelCallback callback) override {
    mojo::ReportBadMessage("no print preview");
  }
  void SetAccessibilityTree(int32_t cookie,
                            const ui::AXTreeUpdate& accessibility_tree) override {
    mojo::ReportBadMessage("no print preview");
  }
#endif
#if BUILDFLAG(IS_ANDROID)
  void SetupScriptedPrintAndroid(
      SetupScriptedPrintAndroidCallback callback) override {
    std::move(callback).Run();
  }
  void PdfWritingDone(int page_count) override {}
#endif

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(PrintManager);

// The renderer's: no print preview, tagged PDFs as headless Chrome makes
// them, window.print() asked of the browser as usual (which reports it).
class PrintDelegate : public printing::PrintRenderFrameHelper::Delegate {
 public:
  blink::WebElement GetPdfElement(blink::WebLocalFrame* frame) override {
    return blink::WebElement();
  }
  bool IsPrintPreviewEnabled() override { return false; }
  bool ShouldGenerateTaggedPDF() override { return true; }
  bool OverridePrint(blink::WebLocalFrame* frame) override { return false; }
};

std::optional<bool> OptionalBool(const base::DictValue& dict, const char* key) {
  return dict.FindBool(key);
}
std::optional<double> OptionalDouble(const base::DictValue& dict,
                                     const char* key) {
  return dict.FindDouble(key);
}
std::optional<std::string> OptionalString(const base::DictValue& dict,
                                          const char* key) {
  const std::string* value = dict.FindString(key);
  return value ? std::optional<std::string>(*value) : std::nullopt;
}

bool IsParameterResult(print_to_pdf::PdfPrintResult result) {
  return result == print_to_pdf::PdfPrintResult::kPageRangeSyntaxError ||
         result == print_to_pdf::PdfPrintResult::kPageRangeInvalidRange ||
         result == print_to_pdf::PdfPrintResult::kPageCountExceeded;
}

}  // namespace

void PrintToPdf(content::WebContents* contents,
                const std::string& options_json,
                PdfCallback callback) {
  if (!contents) {
    std::move(callback).Run(nullptr, "the view is closed", false);
    return;
  }
  base::DictValue options;
  if (!options_json.empty()) {
    std::optional<base::Value> parsed = base::JSONReader::Read(
        options_json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
    if (!parsed || !parsed->is_dict()) {
      std::move(callback).Run(nullptr, "the options are not a JSON object",
                              true);
      return;
    }
    options = std::move(parsed->GetDict());
  }
  content::RenderFrameHost* frame = contents->GetPrimaryMainFrame();
  std::variant<printing::mojom::PrintPagesParamsPtr, std::string> params =
      print_to_pdf::GetPrintPagesParams(
          frame->GetLastCommittedURL(), OptionalBool(options, "landscape"),
          OptionalBool(options, "displayHeaderFooter"),
          OptionalBool(options, "printBackground"),
          OptionalDouble(options, "scale"),
          OptionalDouble(options, "paperWidth"),
          OptionalDouble(options, "paperHeight"),
          OptionalDouble(options, "marginTop"),
          OptionalDouble(options, "marginBottom"),
          OptionalDouble(options, "marginLeft"),
          OptionalDouble(options, "marginRight"),
          OptionalString(options, "headerTemplate"),
          OptionalString(options, "footerTemplate"),
          OptionalBool(options, "preferCSSPageSize"),
          OptionalBool(options, "generateTaggedPDF"),
          OptionalBool(options, "generateDocumentOutline"));
  if (std::holds_alternative<std::string>(params)) {
    std::move(callback).Run(nullptr, std::get<std::string>(params), true);
    return;
  }
  const std::string page_ranges =
      OptionalString(options, "pageRanges").value_or("");
  PrintManager::For(contents)->PrintToPdf(
      frame, page_ranges,
      std::move(std::get<printing::mojom::PrintPagesParamsPtr>(params)),
      base::BindOnce(
          [](PdfCallback callback, print_to_pdf::PdfPrintResult result,
             scoped_refptr<base::RefCountedMemory> data) {
            if (result != print_to_pdf::PdfPrintResult::kPrintSuccess ||
                !data) {
              std::move(callback).Run(
                  nullptr, print_to_pdf::PdfPrintResultToString(result),
                  IsParameterResult(result));
              return;
            }
            std::move(callback).Run(std::move(data), std::string(), false);
          },
          std::move(callback)));
}

void RendererClient::RenderFrameCreated(content::RenderFrame* render_frame) {
  content::ShellContentRendererClient::RenderFrameCreated(render_frame);
  // Deletes itself with the frame.
  new printing::PrintRenderFrameHelper(render_frame,
                                       std::make_unique<PrintDelegate>());
}

void BrowserClient::RegisterAssociatedInterfaceBindersForRenderFrameHost(
    content::RenderFrameHost& render_frame_host,
    blink::AssociatedInterfaceRegistry& associated_registry) {
  content::ShellContentBrowserClient::
      RegisterAssociatedInterfaceBindersForRenderFrameHost(render_frame_host,
                                                           associated_registry);
  associated_registry.AddInterface<printing::mojom::PrintManagerHost>(
      base::BindRepeating(
          [](content::RenderFrameHost* frame,
             mojo::PendingAssociatedReceiver<printing::mojom::PrintManagerHost>
                 receiver) {
            content::WebContents* contents =
                content::WebContents::FromRenderFrameHost(frame);
            if (contents)
              PrintManager::For(contents)->BindReceiver(std::move(receiver),
                                                        frame);
          },
          &render_frame_host));
}

}  // namespace zebkit

extern "C" {

void zebkit_view_print_to_pdf(zebkit_view* view,
                              const char* options_json,
                              zebkit_pdf_func func,
                              void* data) {
  zebkit::PrintToPdf(
      view->web_contents(), options_json ? options_json : "",
      base::BindOnce(
          [](base::WeakPtr<zebkit_view> view, zebkit_pdf_func func, void* data,
             scoped_refptr<base::RefCountedMemory> pdf,
             const std::string& error, bool invalid_params) {
            if (!func)
              return;
            // A view closed meanwhile is still the pointer the application
            // has, and is told.
            zebkit_view* raw = view.get();
            if (!pdf) {
              func(raw, nullptr, 0, error.c_str(), data);
              return;
            }
            func(raw, pdf->data(), pdf->size(), nullptr, data);
          },
          view->GetWeakPtr(), func, data));
}

}  // extern "C"
