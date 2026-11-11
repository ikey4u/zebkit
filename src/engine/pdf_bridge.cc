// zebkit.pdf: <zebkit/pdf.h> for pages granted ZEBKIT_ALLOW_PDF and for the
// application's script (pdf_script.h makes the object). Documents are
// worked on in one sequence of the thread pool, in the browser process; a
// page uses the documents it opened, and they close with its view.

#include <stdlib.h>

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/weak_ptr.h"
#include "base/containers/circular_deque.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "content/public/browser/browser_thread.h"
#include "zebkit/engine/include/zebkit/pdf.h"
#include "zebkit/engine/view.h"

namespace zebkit {

namespace {

struct Document {
  zebkit_pdf* pdf = nullptr;
  // The view whose page opened it, null for the application's script, and
  // the page's origin.
  const zebkit_view* owner = nullptr;
  std::string origin;
  // The page's document that opened it, or the script; nothing else uses it.
  content::WeakDocumentPtr document;
  const void* script = nullptr;
};

std::map<int, Document>& Documents() {
  static base::NoDestructor<std::map<int, Document>> documents;
  return *documents;
}

// The application's scripts that have called zebkit.pdf and not ended, each
// with a number of its own: an open() that completes after its script ended
// (ClosePdfsOfScript()) closes what it opened, also when a later script has
// the same address.
std::map<const void*, uint64_t>& LiveScripts() {
  static base::NoDestructor<std::map<const void*, uint64_t>> scripts;
  return *scripts;
}
uint64_t ScriptNumber(const void* script) {
  static uint64_t next = 1;
  auto [it, added] = LiveScripts().try_emplace(script, next);
  if (added)
    ++next;
  return it->second;
}

// PDFium does one thing at a time: one sequence for all of it, which also
// closes a document only after what was asked of it before.
scoped_refptr<base::SequencedTaskRunner> PdfSequence() {
  static base::NoDestructor<scoped_refptr<base::SequencedTaskRunner>> runner(
      base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE}));
  return *runner;
}

struct Answer {
  bool ok = false;
  base::Value value;
};

Answer Ok(base::Value value) {
  return {true, std::move(value)};
}

Answer Failed(char* error, const std::string& fallback) {
  std::string message = error ? error : fallback;
  free(error);
  return {false, base::Value(message)};
}

// A JSON answer from <zebkit/pdf.h>, as a value. @error is read here, after
// the call that sets it has run: as an argument beside that call it could be
// read first, the order of arguments being unspecified.
Answer Json(char* json, char** error, const std::string& fallback) {
  if (!json)
    return Failed(error ? *error : nullptr, fallback);
  std::optional<base::Value> value =
      base::JSONReader::Read(json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  free(json);
  if (!value)
    return {false, base::Value("an answer that is not JSON")};
  return Ok(std::move(*value));
}

int IntArg(const base::ListValue& args, size_t index) {
  return index < args.size() && args[index].is_int() ? args[index].GetInt()
                                                     : -1;
}

double NumberArg(const base::ListValue& args, size_t index) {
  return index < args.size() &&
                 (args[index].is_int() || args[index].is_double())
             ? args[index].GetDouble()
             : 0;
}

std::string StringArg(const base::ListValue& args, size_t index) {
  return index < args.size() && args[index].is_string()
             ? args[index].GetString()
             : std::string();
}

std::string JsonArg(const base::ListValue& args, size_t index) {
  return index < args.size() ? base::WriteJson(args[index]).value_or("null")
                             : "null";
}

base::Value BytesValue(const uint8_t* data, size_t size) {
  return base::Value(base::Value::BlobStorage(data, data + size));
}

// Render options from a script's object, over these defaults.
zebkit_pdf_render_options RenderOptions(const base::Value* given,
                                        unsigned flags,
                                        uint32_t background) {
  zebkit_pdf_render_options options = {};
  options.flags = flags;
  options.background = background;
  if (!given || !given->is_dict())
    return options;
  auto number = [&](const char* key, double fallback) {
    const base::Value* value = given->GetDict().Find(key);
    return value && (value->is_int() || value->is_double()) ? value->GetDouble()
                                                            : fallback;
  };
  options.scale = static_cast<float>(number("scale", 1));
  options.rotation = static_cast<int>(number("rotation", 0));
  options.flags = static_cast<unsigned>(number("flags", options.flags));
  options.background =
      static_cast<uint32_t>(number("background", options.background));
  options.x = static_cast<int>(number("x", 0));
  options.y = static_cast<int>(number("y", 0));
  options.width = static_cast<int>(number("width", 0));
  options.height = static_cast<int>(number("height", 0));
  return options;
}

// {width, height, data} of RGBA @pixels, which are freed.
base::Value Image(uint8_t* pixels, int width, int height) {
  base::DictValue image;
  image.Set("width", width);
  image.Set("height", height);
  image.Set("data",
            BytesValue(pixels, static_cast<size_t>(width) * height * 4));
  free(pixels);
  return base::Value(std::move(image));
}

// What a document is asked for, on the PDF sequence. @args are the call's
// after the document's id.
std::vector<int> IntList(const base::ListValue& args, size_t index) {
  std::vector<int> list;
  if (index < args.size() && args[index].is_list()) {
    for (const base::Value& item : args[index].GetList())
      list.push_back(item.is_int() ? item.GetInt() : -1);
  }
  return list;
}

// A layer a script drew: its operations, on the page's bitmap.
void DrawLayerOps(zebkit_canvas* canvas,
                  zebkit_pdf*,
                  int,
                  const zebkit_pdf_layer_info*,
                  void* data) {
  zebkit_canvas_run(canvas, static_cast<std::string*>(data)->c_str(), nullptr);
}

// Operations waiting for a script, per document, on the PDF sequence.
struct Recorded {
  int hook = 0;
  std::vector<std::string> operations;
};

std::map<zebkit_pdf*, Recorded>& RecordedOperations() {
  static base::NoDestructor<std::map<zebkit_pdf*, Recorded>> recorded;
  return *recorded;
}

// The hooks a script registered on each document, which are all that its
// removeHook can reach: a page must not take away the application's.
std::map<zebkit_pdf*, std::set<int>>& ScriptHooks() {
  static base::NoDestructor<std::map<zebkit_pdf*, std::set<int>>> hooks;
  return *hooks;
}

void RecordOperation(zebkit_pdf* pdf, const char* operation, void*) {
  auto recorded = RecordedOperations().find(pdf);
  // A script that never asks must not grow this without end.
  if (recorded != RecordedOperations().end() && recorded->second.operations.size() < 4096)
    recorded->second.operations.push_back(operation);
}

// @other is a second document a call names (importPages), or null.
Answer Work(zebkit_pdf* pdf,
            zebkit_pdf* other,
            const std::string& method,
            base::ListValue args) {
  char* error = nullptr;
  if (method == "formFields")
    return Json(zebkit_pdf_form_fields(pdf, &error), &error, "no form");
  if (method == "setField") {
    if (!zebkit_pdf_form_set(pdf, StringArg(args, 0).c_str(),
                             StringArg(args, 1).c_str(), &error)) {
      return Failed(error, "the field cannot be set");
    }
    return Ok(base::Value());
  }
  if (method == "resetForm") {
    if (!zebkit_pdf_form_reset(pdf, &error))
      return Failed(error, "the form cannot be reset");
    return Ok(base::Value());
  }
  if (method == "rotatePage") {
    if (!zebkit_pdf_page_rotate(pdf, IntArg(args, 0), IntArg(args, 1), &error))
      return Failed(error, "the page cannot be turned");
    return Ok(base::Value());
  }
  if (method == "deletePages" || method == "movePages") {
    const std::vector<int> pages = IntList(args, 0);
    const bool ok =
        method == "deletePages"
            ? zebkit_pdf_pages_delete(pdf, pages.data(),
                                      static_cast<int>(pages.size()), &error)
            : zebkit_pdf_pages_move(pdf, pages.data(),
                                    static_cast<int>(pages.size()),
                                    IntArg(args, 1), &error);
    if (!ok)
      return Failed(error, "the pages cannot be changed");
    return Ok(base::Value());
  }
  if (method == "insertPage") {
    if (!zebkit_pdf_page_insert(pdf, IntArg(args, 0), NumberArg(args, 1),
                                NumberArg(args, 2), &error)) {
      return Failed(error, "the page cannot be made");
    }
    return Ok(base::Value());
  }
  if (method == "importPages") {
    const bool all = args.size() < 2 || args[1].is_none();
    const std::vector<int> pages = IntList(args, 1);
    if (!zebkit_pdf_pages_import(pdf, other, all ? nullptr : pages.data(),
                                 static_cast<int>(pages.size()),
                                 IntArg(args, 2), &error)) {
      return Failed(error, "the pages cannot be copied");
    }
    return Ok(base::Value());
  }
  if (method == "info")
    return Json(zebkit_pdf_info(pdf), nullptr, "no information");
  if (method == "setMetadata") {
    if (!zebkit_pdf_set_metadata(pdf, JsonArg(args, 0).c_str(), &error))
      return Failed(error, "the metadata cannot be set");
    return Ok(base::Value());
  }
  if (method == "page")
    return Json(zebkit_pdf_page(pdf, IntArg(args, 0), &error), &error,
                "no page");
  if (method == "pages") {
    // Every page's page(), in one call.
    base::ListValue pages;
    const int count = zebkit_pdf_page_count(pdf);
    for (int index = 0; index < count; ++index) {
      Answer page =
          Json(zebkit_pdf_page(pdf, index, &error), &error, "no page");
      if (!page.ok)
        return page;
      pages.Append(std::move(page.value));
    }
    return Ok(base::Value(std::move(pages)));
  }
  if (method == "render") {
    zebkit_pdf_render_options options = RenderOptions(
        args.size() > 1 ? &args[1] : nullptr, ZEBKIT_PDF_RENDER_ANNOTATIONS,
        0xffffffff);
    int width = 0, height = 0;
    // What the script's layers drew, as operations: hooks for this call.
    std::vector<int> layer_hooks;
    std::string below, above;
    if (args.size() > 2 && args[2].is_dict()) {
      if (const base::Value* ops = args[2].GetDict().Find("below"))
        below = base::WriteJson(*ops).value_or("[]");
      if (const base::Value* ops = args[2].GetDict().Find("above"))
        above = base::WriteJson(*ops).value_or("[]");
      if (!below.empty()) {
        layer_hooks.push_back(zebkit_pdf_hook_layer(pdf, ZEBKIT_PDF_LAYER_BELOW, &DrawLayerOps,
                                                    &below, nullptr));
      }
      if (!above.empty()) {
        layer_hooks.push_back(zebkit_pdf_hook_layer(pdf, ZEBKIT_PDF_LAYER_ABOVE, &DrawLayerOps,
                                                    &above, nullptr));
      }
    }
    uint8_t* pixels =
        zebkit_pdf_render(pdf, IntArg(args, 0), &options, &width, &height,
                          &error);
    for (int hook : layer_hooks)
      zebkit_pdf_hook_remove(hook);
    if (!pixels)
      return Failed(error, "the page cannot be rendered");
    return Ok(Image(pixels, width, height));
  }
  if (method == "renderAnnotation") {
    zebkit_pdf_render_options options =
        RenderOptions(args.size() > 3 ? &args[3] : nullptr, 0, 0);
    int width = 0, height = 0;
    uint8_t* pixels = zebkit_pdf_render_annotation(
        pdf, IntArg(args, 0), IntArg(args, 1), IntArg(args, 2), &options,
        &width, &height, &error);
    if (!pixels) {
      // An annotation without that appearance is not an error here.
      if (error && std::string_view(error) == "no appearance") {
        free(error);
        return Ok(base::Value());
      }
      return Failed(error, "the annotation cannot be rendered");
    }
    return Ok(Image(pixels, width, height));
  }
  if (method == "renderAnnotations") {
    // Every annotation's appearances, for a layer above a page rendered
    // without them: [{index, name, rect, normal, rollover, down}].
    const int page = IntArg(args, 0);
    zebkit_pdf_render_options options =
        RenderOptions(args.size() > 1 ? &args[1] : nullptr, 0, 0);
    char* json = zebkit_pdf_annotations(pdf, page, &error);
    if (!json)
      return Failed(error, "no page");
    std::optional<base::Value> annotations = base::JSONReader::Read(
        json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
    free(json);
    base::ListValue layer;
    if (!annotations || !annotations->is_list())
      return Ok(base::Value(std::move(layer)));
    for (const base::Value& annotation : annotations->GetList()) {
      const base::DictValue& item = annotation.GetDict();
      const base::ListValue* modes = item.FindList("appearances");
      if (!modes || modes->empty())
        continue;
      base::DictValue entry;
      entry.Set("index", item.FindInt("index").value_or(-1));
      if (const std::string* name = item.FindString("name"))
        entry.Set("name", *name);
      if (const base::ListValue* rect = item.FindList("rect"))
        entry.Set("rect", rect->Clone());
      constexpr const char* kModes[] = {"normal", "rollover", "down"};
      for (int mode = 0; mode < 3; ++mode) {
        if (std::ranges::find(*modes, base::Value(kModes[mode])) ==
            modes->end()) {
          continue;
        }
        int width = 0, height = 0;
        uint8_t* pixels = zebkit_pdf_render_annotation(
            pdf, page, item.FindInt("index").value_or(-1), mode, &options,
            &width, &height, nullptr);
        if (pixels)
          entry.Set(kModes[mode], Image(pixels, width, height));
      }
      layer.Append(std::move(entry));
    }
    return Ok(base::Value(std::move(layer)));
  }
  if (method == "text") {
    char* text = zebkit_pdf_text(pdf, IntArg(args, 0), &error);
    if (!text)
      return Failed(error, "no text");
    base::Value value(text);
    free(text);
    return Ok(std::move(value));
  }
  if (method == "textLayout")
    return Json(zebkit_pdf_text_layout(pdf, IntArg(args, 0), &error), &error,
                "no text");
  if (method == "textRects") {
    return Json(zebkit_pdf_text_rects(pdf, IntArg(args, 0), IntArg(args, 1),
                                      IntArg(args, 2), &error),
                &error, "no text");
  }
  if (method == "charAt") {
    return Ok(base::Value(zebkit_pdf_char_at(pdf, IntArg(args, 0),
                                             NumberArg(args, 1),
                                             NumberArg(args, 2),
                                             NumberArg(args, 3))));
  }
  if (method == "search") {
    return Json(zebkit_pdf_search(pdf, StringArg(args, 0).c_str(),
                                  static_cast<unsigned>(IntArg(args, 1)),
                                  IntArg(args, 2), IntArg(args, 3), &error),
                &error, "the search failed");
  }
  if (method == "signatures") {
    // With each one's contents, the PKCS#7 it holds, as bytes.
    Answer answer =
        Json(zebkit_pdf_signatures(pdf, &error), &error, "no signatures");
    if (!answer.ok || !answer.value.is_list())
      return answer;
    int index = 0;
    for (base::Value& item : answer.value.GetList()) {
      size_t size = 0;
      if (uint8_t* data =
              zebkit_pdf_signature_contents(pdf, index++, &size, nullptr)) {
        item.GetDict().Set(
            "contents", base::Value(base::Value::BlobStorage(data, data + size)));
        free(data);
      }
    }
    return answer;
  }
  if (method == "setOutline") {
    if (!zebkit_pdf_set_outline(pdf, JsonArg(args, 0).c_str(), &error))
      return Failed(error, "the outline cannot be set");
    return Ok(base::Value());
  }
  if (method == "outline")
    return Json(zebkit_pdf_outline(pdf, &error), &error, "no outline");
  if (method == "links")
    return Json(zebkit_pdf_links(pdf, IntArg(args, 0), &error), &error,
                "no links");
  if (method == "textLines")
    return Json(zebkit_pdf_text_lines(pdf, IntArg(args, 0), &error), &error,
                "no text");
  if (method == "images")
    return Json(zebkit_pdf_images(pdf, IntArg(args, 0), &error), &error,
                "no images");
  if (method == "imageData") {
    size_t size = 0;
    bool jpeg = false;
    uint8_t* data = zebkit_pdf_image_data(pdf, IntArg(args, 0),
                                          IntArg(args, 1), &size, &jpeg,
                                          &error);
    if (!data)
      return Failed(error, "no such image");
    base::DictValue image;
    image.Set("data", base::Value(base::Value::BlobStorage(data, data + size)));
    image.Set("type", jpeg ? "image/jpeg" : "image/png");
    free(data);
    return Ok(base::Value(std::move(image)));
  }
  if (method == "annotations")
    return Json(zebkit_pdf_annotations(pdf, IntArg(args, 0), &error), &error,
                "no annotations");
  if (method == "annotationsOfPages") {
    // annotations() of each page given, in one call.
    base::ListValue pages;
    for (int index : IntList(args, 0)) {
      Answer page = Json(zebkit_pdf_annotations(pdf, index, &error), &error,
                         "no annotations");
      if (!page.ok)
        return page;
      pages.Append(std::move(page.value));
    }
    return Ok(base::Value(std::move(pages)));
  }
  if (method == "addAnnotation") {
    const int index = zebkit_pdf_annotation_add(
        pdf, IntArg(args, 0), JsonArg(args, 1).c_str(), &error);
    if (index < 0)
      return Failed(error, "the annotation cannot be made");
    return Ok(base::Value(index));
  }
  if (method == "updateAnnotation") {
    if (!zebkit_pdf_annotation_update(pdf, IntArg(args, 0), IntArg(args, 1),
                                      JsonArg(args, 2).c_str(), &error)) {
      return Failed(error, "the annotation cannot be changed");
    }
    return Ok(base::Value());
  }
  if (method == "removeAnnotation") {
    if (!zebkit_pdf_annotation_remove(pdf, IntArg(args, 0), IntArg(args, 1),
                                      &error)) {
      return Failed(error, "the annotation cannot be removed");
    }
    return Ok(base::Value());
  }
  if (method == "setAnnotationImage") {
    // {width, height, data}: RGBA, rows top to bottom.
    const base::DictValue* image =
        args.size() > 2 && args[2].is_dict() ? &args[2].GetDict() : nullptr;
    std::optional<std::string> data =
        image ? GetBytes(image->Find("data")) : std::nullopt;
    const int width = image ? image->FindInt("width").value_or(0) : 0;
    const int height = image ? image->FindInt("height").value_or(0) : 0;
    if (!data || data->size() != static_cast<size_t>(width) * height * 4)
      return Failed(nullptr, "an image is {width, height, data} in RGBA");
    if (!zebkit_pdf_annotation_set_image(
            pdf, IntArg(args, 0), IntArg(args, 1),
            reinterpret_cast<const uint8_t*>(data->data()), width, height,
            &error)) {
      return Failed(error, "the stamp's image cannot be set");
    }
    return Ok(base::Value());
  }
  if (method == "flatten") {
    const bool print = args.size() > 1 && args[1].is_bool() && args[1].GetBool();
    if (!zebkit_pdf_flatten(pdf, IntArg(args, 0), print, &error))
      return Failed(error, "the page cannot be flattened");
    return Ok(base::Value());
  }
  if (method == "redact") {
    // (page, areas [[l, b, r, t], ...] or null for the page's marks, fill)
    std::string areas;
    if (args.size() > 1 && args[1].is_list())
      areas = base::WriteJson(args[1]).value_or("[]");
    const uint32_t fill =
        args.size() > 2 && (args[2].is_int() || args[2].is_double())
            ? static_cast<uint32_t>(args[2].GetDouble())
            : 0x000000ff;
    const int redacted = zebkit_pdf_redact(
        pdf, IntArg(args, 0), areas.empty() ? nullptr : areas.c_str(), fill,
        &error);
    if (redacted < 0)
      return Failed(error, "the page cannot be redacted");
    return Ok(base::Value(redacted));
  }
  if (method == "attachments")
    return Json(zebkit_pdf_attachments(pdf, &error), &error, "no attachments");
  if (method == "attachment") {
    size_t size = 0;
    uint8_t* data = zebkit_pdf_attachment(pdf, IntArg(args, 0), &size, &error);
    if (!data)
      return Failed(error, "no attachment");
    base::Value bytes = BytesValue(data, size);
    free(data);
    return Ok(std::move(bytes));
  }
  if (method == "attach") {
    std::optional<std::string> data =
        GetBytes(args.size() > 1 ? &args[1] : nullptr);
    if (!data)
      return {false, base::Value("no bytes to attach")};
    if (!zebkit_pdf_attach(pdf, StringArg(args, 0).c_str(), data->data(),
                           data->size(), &error)) {
      return Failed(error, "the file cannot be attached");
    }
    return Ok(base::Value());
  }
  if (method == "detach") {
    if (!zebkit_pdf_detach(pdf, IntArg(args, 0), &error))
      return Failed(error, "no attachment");
    return Ok(base::Value());
  }
  if (method == "annotationAt") {
    const int at = zebkit_pdf_annotation_at(pdf, IntArg(args, 0), NumberArg(args, 1), NumberArg(args, 2),
                                            NumberArg(args, 3), &error);
    if (at < 0 && error)
      return Failed(error, "no page");
    return Ok(base::Value(at));
  }
  if (method == "moveAnnotation") {
    if (!zebkit_pdf_annotation_move(pdf, IntArg(args, 0), IntArg(args, 1), IntArg(args, 2), &error))
      return Failed(error, "the annotation cannot be moved");
    return Ok(base::Value());
  }
  if (method == "copyAnnotation") {
    // (other document's place, page, annotation, target page)
    const int copied = zebkit_pdf_annotation_copy(pdf, IntArg(args, 1), IntArg(args, 2), other,
                                                  IntArg(args, 3), &error);
    if (copied < 0)
      return Failed(error, "the annotation cannot be copied");
    return Ok(base::Value(copied));
  }
  if (method == "flattenAnnotation") {
    if (!zebkit_pdf_annotation_flatten(pdf, IntArg(args, 0), IntArg(args, 1), &error))
      return Failed(error, "the annotation cannot be flattened");
    return Ok(base::Value());
  }
  if (method == "stampFromPage") {
    if (!zebkit_pdf_annotation_stamp_page(pdf, IntArg(args, 1), IntArg(args, 2), other, IntArg(args, 3), &error))
      return Failed(error, "the page cannot be made a stamp");
    return Ok(base::Value());
  }
  if (method == "attachFile") {
    std::optional<std::string> data = GetBytes(args.size() > 3 ? &args[3] : nullptr);
    if (!data)
      return {false, base::Value("no bytes to attach")};
    if (!zebkit_pdf_annotation_attach_file(pdf, IntArg(args, 0), IntArg(args, 1), StringArg(args, 2).c_str(),
                                           data->data(), data->size(), &error)) {
      return Failed(error, "the file cannot be attached");
    }
    return Ok(base::Value());
  }
  if (method == "annotationFile") {
    size_t size = 0;
    char* name = nullptr;
    uint8_t* data = zebkit_pdf_annotation_file(pdf, IntArg(args, 0), IntArg(args, 1), &name, &size, &error);
    if (!data)
      return Failed(error, "the annotation has no file");
    base::DictValue file;
    file.Set("name", name ? name : "");
    file.Set("data", BytesValue(data, size));
    free(name);
    free(data);
    return Ok(base::Value(std::move(file)));
  }
  if (method == "drawAnnotation") {
    if (!zebkit_pdf_annotation_draw(pdf, IntArg(args, 0), IntArg(args, 1), JsonArg(args, 2).c_str(), &error))
      return Failed(error, "the appearance cannot be drawn");
    return Ok(base::Value());
  }
  if (method == "annotationAction")
    return Json(zebkit_pdf_annotation_action(pdf, IntArg(args, 0), IntArg(args, 1), &error), &error, "no action");
  if (method == "exportAnnotations") {
    size_t size = 0;
    uint8_t* data = zebkit_pdf_annotations_export(
        pdf, StringArg(args, 0) == "fdf" ? ZEBKIT_PDF_FDF : ZEBKIT_PDF_XFDF, IntArg(args, 1), IntArg(args, 2),
        &size, &error);
    if (!data)
      return Failed(error, "the annotations cannot be written");
    base::Value bytes = BytesValue(data, size);
    free(data);
    return Ok(std::move(bytes));
  }
  if (method == "importAnnotations") {
    std::optional<std::string> data = GetBytes(args.size() > 1 ? &args[1] : nullptr);
    if (!data)
      return {false, base::Value("no bytes to read")};
    const int count = zebkit_pdf_annotations_import(
        pdf, StringArg(args, 0) == "fdf" ? ZEBKIT_PDF_FDF : ZEBKIT_PDF_XFDF, data->data(), data->size(), &error);
    if (count < 0)
      return Failed(error, "the annotations cannot be read");
    return Ok(base::Value(count));
  }
  if (method == "apply") {
    if (!zebkit_pdf_apply(pdf, JsonArg(args, 0).c_str(), &error))
      return Failed(error, "the operation cannot be run");
    return Ok(base::Value());
  }
  if (method == "addFont") {
    std::optional<std::string> data = GetBytes(args.size() > 1 ? &args[1] : nullptr);
    if (!data)
      return {false, base::Value("no font's bytes")};
    if (!zebkit_pdf_font_add(pdf, StringArg(args, 0).c_str(), data->data(), data->size(), &error))
      return Failed(error, "the font cannot be used");
    return Ok(base::Value());
  }
  if (method == "filterInput") {
    const std::string options = args.size() > 2 && !args[2].is_none() ? JsonArg(args, 2) : std::string();
    return Json(zebkit_pdf_filter_input(pdf, StringArg(args, 0).c_str(), JsonArg(args, 1).c_str(),
                                        options.empty() ? nullptr : options.c_str(), &error),
                &error, "the points cannot be filtered");
  }
  if (method == "pageData")
    return Json(zebkit_pdf_page_data(pdf, IntArg(args, 0), StringArg(args, 1).c_str(), &error), &error, "no data");
  if (method == "registerKind") {
    // A script's kind: the file's side of it; its check and drawing are the script's.
    const std::string name = StringArg(args, 0), prefix = StringArg(args, 1), subtype = StringArg(args, 2);
    const std::string schema = args.size() > 3 && !args[3].is_none() ? JsonArg(args, 3) : std::string();
    zebkit_pdf_kind kind = {};
    kind.name = name.c_str();
    kind.prefix = prefix.c_str();
    kind.subtype = subtype.empty() ? nullptr : subtype.c_str();
    kind.schema = schema.empty() ? nullptr : schema.c_str();
    const int id = zebkit_pdf_hook_kind(pdf, &kind, &error);
    if (id < 0)
      return Failed(error, "the kind cannot be registered");
    ScriptHooks()[pdf].insert(id);
    return Ok(base::Value(id));
  }
  if (method == "removeHook") {
    const int id = IntArg(args, 0);
    const auto hooks = ScriptHooks().find(pdf);
    if (hooks == ScriptHooks().end() || !hooks->second.erase(id))
      return Ok(base::Value(false));
    return Ok(base::Value(zebkit_pdf_hook_remove(id)));
  }
  if (method == "enableOperations") {
    // What happens to the document comes to a script's after hooks this way,
    // with what C's hooks get, asked for after each call.
    auto& recorded = RecordedOperations()[pdf];
    if (recorded.hook <= 0)
      recorded.hook = zebkit_pdf_hook_after(pdf, "*", &RecordOperation, nullptr, nullptr);
    return Ok(base::Value());
  }
  if (method == "takeOperations") {
    base::ListValue list;
    auto recorded = RecordedOperations().find(pdf);
    if (recorded != RecordedOperations().end()) {
      for (std::string& json : recorded->second.operations) {
        if (std::optional<base::Value> value = base::JSONReader::Read(json, base::JSON_PARSE_CHROMIUM_EXTENSIONS))
          list.Append(std::move(*value));
      }
      recorded->second.operations.clear();
    }
    return Ok(base::Value(std::move(list)));
  }
  if (method == "save") {
    size_t size = 0;
    uint8_t* data = zebkit_pdf_save(
        pdf, static_cast<unsigned>(std::max(0, IntArg(args, 0))), &size,
        &error);
    if (!data)
      return Failed(error, "the document cannot be saved");
    base::Value bytes = BytesValue(data, size);
    free(data);
    return Ok(std::move(bytes));
  }
  return {false, base::Value("zebkit.pdf has no " + method)};
}

void Reply(CallReply reply, Answer answer) {
  std::move(reply).Run(answer.ok, std::move(answer.value));
}

// Opens on the PDF sequence: bytes, or for the application's script a
// path. The document's id is made back on the UI thread.
struct Opened {
  zebkit_pdf* pdf = nullptr;
  std::string error;
};

Opened Open(bool create,
            std::optional<std::string> bytes,
            std::string path,
            std::string password) {
  char* error = nullptr;
  const char* pass = password.empty() ? nullptr : password.c_str();
  zebkit_pdf* pdf =
      create  ? zebkit_pdf_new(&error)
      : bytes ? zebkit_pdf_open(bytes->data(), bytes->size(), pass, &error)
              : zebkit_pdf_open_file(path.c_str(), pass, &error);
  Opened opened;
  opened.pdf = pdf;
  if (!pdf)
    opened.error = error ? error : "the document cannot be opened";
  free(error);
  return opened;
}

// Calls on documents wait here, by how soon they are wanted, and the PDF
// sequence takes the soonest each time: a page to show first, then the
// rest, then what reads a whole document (all its annotations, a search),
// so that a reader's first page is not drawn after its document's links.
enum Urgency { kNow, kSoon, kLater, kUrgencies };

Urgency UrgencyOf(const std::string& method) {
  static constexpr const char* kNowMethods[] = {
      "render", "renderAnnotation", "renderAnnotations", "charAt",
      "textRects", "page"};
  static constexpr const char* kLaterMethods[] = {
      "annotationsOfPages", "pages", "search", "formFields", "save",
      "attachments"};
  for (const char* name : kNowMethods) {
    if (method == name)
      return kNow;
  }
  for (const char* name : kLaterMethods) {
    if (method == name)
      return kLater;
  }
  return kSoon;
}

// Calls that only read a document; any other may change it.
bool OnlyReads(const std::string& method) {
  static constexpr const char* kReads[] = {
      "annotationAction", "annotationAt", "annotationFile", "annotations",
      "annotationsOfPages", "attachment", "attachments", "charAt",
      "exportAnnotations", "formFields", "imageData", "images", "info", "links",
      "outline", "page", "pageData", "pages", "render", "renderAnnotation",
      "renderAnnotations", "save", "search", "signatures", "text", "textLayout",
      "textLines", "textRects"};
  for (const char* name : kReads) {
    if (method == name)
      return true;
  }
  return false;
}

// A call waiting, with the documents it uses: one that changes a document
// is never overtaken by a later call on it, nor overtakes an earlier one, so
// that apply(); render() renders the change and save(); apply() saves without.
struct PdfCall {
  uint64_t order = 0;
  std::vector<const zebkit_pdf*> documents;
  bool changes = false;
  base::OnceClosure work;
};

struct Waiting {
  base::Lock lock;
  uint64_t next = 0;
  std::array<base::circular_deque<PdfCall>, kUrgencies> calls;
};

Waiting& Calls() {
  static base::NoDestructor<Waiting> waiting;
  return *waiting;
}

// Whether @call has to wait for an earlier one on a document they share.
bool Held(const Waiting& waiting, const PdfCall& call) {
  for (const auto& calls : waiting.calls) {
    for (const PdfCall& other : calls) {
      if (other.order >= call.order || (!other.changes && !call.changes))
        continue;
      for (const zebkit_pdf* document : call.documents) {
        if (std::ranges::contains(other.documents, document))
          return true;
      }
    }
  }
  return false;
}

void RunSoonest() {
  base::OnceClosure work;
  {
    base::AutoLock lock(Calls().lock);
    // The soonest that nothing earlier holds back; the earliest of all never is.
    for (auto& calls : Calls().calls) {
      for (auto it = calls.begin(); it != calls.end(); ++it) {
        if (!Held(Calls(), *it)) {
          work = std::move(it->work);
          calls.erase(it);
          break;
        }
      }
      if (work)
        break;
    }
  }
  if (work)
    std::move(work).Run();
}

// One task on the sequence for each call waiting: each runs the soonest
// call there is when it runs. Calls of the same urgency keep their order.
void Enqueue(Urgency urgency,
             std::vector<const zebkit_pdf*> documents,
             bool changes,
             base::OnceClosure work) {
  {
    base::AutoLock lock(Calls().lock);
    Calls().calls[urgency].push_back(
        PdfCall{Calls().next++, std::move(documents), changes, std::move(work)});
  }
  PdfSequence()->PostTask(FROM_HERE, base::BindOnce(&RunSoonest));
}

// After every call already waiting for the document: last of all, and
// none can come for it after (it is no longer among Documents()).
void CloseOnSequence(zebkit_pdf* pdf) {
  Enqueue(kLater, {pdf}, /*changes=*/true, base::BindOnce(
                      [](zebkit_pdf* document) {
                        RecordedOperations().erase(document);
                        ScriptHooks().erase(document);
                        zebkit_pdf_close(document);
                      },
                      pdf));
}

}  // namespace

void CallPdf(PageCall call) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (call.method == "open" || call.method == "create") {
    const bool create = call.method == "create";
    const base::Value* source = call.args.empty() ? nullptr : &call.args[0];
    std::optional<std::string> bytes;
    std::string path;
    if (create) {
      // Nothing to read.
    } else if (source && source->is_string() && !call.view) {
      path = source->GetString();
    } else {
      // A page gives bytes; a string from a page is not a path.
      bytes = source && !source->is_string() ? GetBytes(source) : std::nullopt;
      if (!bytes) {
        std::move(call.reply).Run(false,
                                  base::Value("open() takes the PDF's bytes"));
        return;
      }
    }
    std::string password = call.args.size() > 1 && call.args[1].is_string()
                               ? call.args[1].GetString()
                               : std::string();
    base::WeakPtr<zebkit_view> view =
        call.view ? call.view->GetWeakPtr() : nullptr;
    const bool from_page = call.view != nullptr;
    const uint64_t script_number = call.script ? ScriptNumber(call.script) : 0;
    PdfSequence()->PostTaskAndReplyWithResult(
        FROM_HERE,
        base::BindOnce(&Open, create, std::move(bytes), std::move(path),
                       std::move(password)),
        base::BindOnce(
            [](base::WeakPtr<zebkit_view> view, bool from_page,
               std::string origin, content::WeakDocumentPtr document,
               const void* script, uint64_t script_number, CallReply reply,
               Opened opened) {
              if (!opened.pdf) {
                std::move(reply).Run(false, base::Value(opened.error));
                return;
              }
              // The page's view, or its document, or the script, went while
              // it opened.
              auto live = script ? LiveScripts().find(script) : LiveScripts().end();
              if ((from_page && (!view || !document.AsRenderFrameHostIfValid())) ||
                  (script && (live == LiveScripts().end() || live->second != script_number))) {
                CloseOnSequence(opened.pdf);
                return;
              }
              static int next_id = 1;
              const int id = next_id++;
              Documents()[id] = {opened.pdf, view.get(), std::move(origin), document, script};
              std::move(reply).Run(true, base::Value(id));
            },
            std::move(view), from_page, call.origin, call.document, call.script,
            script_number, std::move(call.reply)));
    return;
  }

  // A page has the documents its view and origin opened, and no others.
  auto find = [&](const base::Value* id) -> zebkit_pdf* {
    auto document =
        id && id->is_int() ? Documents().find(id->GetInt()) : Documents().end();
    if (document == Documents().end() ||
        document->second.owner != call.view ||
        (call.view && document->second.origin != call.origin) ||
        document->second.script != call.script) {
      return nullptr;
    }
    if (call.view) {
      content::RenderFrameHost* opener = document->second.document.AsRenderFrameHostIfValid();
      if (!opener || opener != call.document.AsRenderFrameHostIfValid())
        return nullptr;
    }
    return document->second.pdf;
  };
  zebkit_pdf* pdf = find(call.args.empty() ? nullptr : &call.args[0]);
  if (!pdf) {
    std::move(call.reply).Run(false, base::Value("no such document"));
    return;
  }
  zebkit_pdf* other = nullptr;
  const bool names_another = call.method == "importPages" || call.method == "copyAnnotation" ||
                             call.method == "stampFromPage";
  if (names_another) {
    other = find(call.args.size() > 1 ? &call.args[1] : nullptr);
    if (!other) {
      std::move(call.reply).Run(false, base::Value("no such document"));
      return;
    }
  }
  if (call.method == "close") {
    Documents().erase(call.args[0].GetInt());
    CloseOnSequence(pdf);
    std::move(call.reply).Run(true, base::Value());
    return;
  }
  base::ListValue args;
  // A second argument that was the other document's id leaves a null.
  for (size_t i = other ? 2 : 1; i < call.args.size(); ++i)
    args.Append(call.args[i].Clone());
  if (other)
    args.Insert(args.begin(), base::Value());
  const Urgency urgency = UrgencyOf(call.method);
  std::vector<const zebkit_pdf*> documents = {pdf};
  if (other)
    documents.push_back(other);
  Enqueue(urgency, std::move(documents), !OnlyReads(call.method),
          base::BindOnce(
              [](zebkit_pdf* pdf, zebkit_pdf* other, std::string method,
                 base::ListValue args,
                 scoped_refptr<base::SequencedTaskRunner> caller,
                 CallReply reply) {
                caller->PostTask(
                    FROM_HERE,
                    base::BindOnce(&Reply, std::move(reply),
                                   Work(pdf, other, method, std::move(args))));
              },
              pdf, other, call.method, std::move(args),
              base::SequencedTaskRunner::GetCurrentDefault(),
              std::move(call.reply)));
}

void ClosePdfsOfGoneDocuments(const zebkit_view* view,
                              const content::RenderFrameHost* frame) {
  for (auto it = Documents().begin(); it != Documents().end();) {
    // While RenderFrameDeleted(@frame) runs its document is still valid.
    content::RenderFrameHost* opener = it->second.document.AsRenderFrameHostIfValid();
    if (it->second.owner == view && (!opener || opener == frame)) {
      CloseOnSequence(it->second.pdf);
      it = Documents().erase(it);
    } else {
      ++it;
    }
  }
}

void ClosePdfsOfScript(const void* script) {
  LiveScripts().erase(script);
  for (auto it = Documents().begin(); it != Documents().end();) {
    if (!it->second.owner && it->second.script == script) {
      CloseOnSequence(it->second.pdf);
      it = Documents().erase(it);
    } else {
      ++it;
    }
  }
}

void ClosePdfsOf(const zebkit_view* view) {
  for (auto it = Documents().begin(); it != Documents().end();) {
    if (it->second.owner == view) {
      CloseOnSequence(it->second.pdf);
      it = Documents().erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace zebkit
