// zebkit.pdf: <zebkit/pdf.h> for pages granted ZEBKIT_ALLOW_PDF and for the
// application's script (pdf_script.h makes the object). Documents are
// worked on in one sequence of the thread pool, in the browser process; a
// page uses the documents it opened, and they close with its view.

#include <stdlib.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
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
};

std::map<int, Document>& Documents() {
  static base::NoDestructor<std::map<int, Document>> documents;
  return *documents;
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

// A JSON answer from <zebkit/pdf.h>, as a value.
Answer Json(char* json, char* error, const std::string& fallback) {
  if (!json)
    return Failed(error, fallback);
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

// What a document is asked for, on the PDF sequence. @args are the call's
// after the document's id.
Answer Work(zebkit_pdf* pdf, const std::string& method, base::ListValue args) {
  char* error = nullptr;
  if (method == "info")
    return Json(zebkit_pdf_info(pdf), nullptr, "no information");
  if (method == "setMetadata") {
    if (!zebkit_pdf_set_metadata(pdf, JsonArg(args, 0).c_str(), &error))
      return Failed(error, "the metadata cannot be set");
    return Ok(base::Value());
  }
  if (method == "page")
    return Json(zebkit_pdf_page(pdf, IntArg(args, 0), &error), error,
                "no page");
  if (method == "render") {
    zebkit_pdf_render_options options = {};
    options.flags = ZEBKIT_PDF_RENDER_ANNOTATIONS;
    options.background = 0xffffffff;
    if (args.size() > 1 && args[1].is_dict()) {
      const base::DictValue& given = args[1].GetDict();
      auto number = [&](const char* key, double fallback) {
        const base::Value* value = given.Find(key);
        return value && (value->is_int() || value->is_double())
                   ? value->GetDouble()
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
    }
    int width = 0, height = 0;
    uint8_t* pixels =
        zebkit_pdf_render(pdf, IntArg(args, 0), &options, &width, &height,
                          &error);
    if (!pixels)
      return Failed(error, "the page cannot be rendered");
    base::DictValue image;
    image.Set("width", width);
    image.Set("height", height);
    image.Set("data", BytesValue(pixels, static_cast<size_t>(width) * height * 4));
    free(pixels);
    return Ok(base::Value(std::move(image)));
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
    return Json(zebkit_pdf_text_layout(pdf, IntArg(args, 0), &error), error,
                "no text");
  if (method == "textRects") {
    return Json(zebkit_pdf_text_rects(pdf, IntArg(args, 0), IntArg(args, 1),
                                      IntArg(args, 2), &error),
                error, "no text");
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
                error, "the search failed");
  }
  if (method == "outline")
    return Json(zebkit_pdf_outline(pdf, &error), error, "no outline");
  if (method == "links")
    return Json(zebkit_pdf_links(pdf, IntArg(args, 0), &error), error,
                "no links");
  if (method == "annotations")
    return Json(zebkit_pdf_annotations(pdf, IntArg(args, 0), &error), error,
                "no annotations");
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
  if (method == "flatten") {
    const bool print = args.size() > 1 && args[1].is_bool() && args[1].GetBool();
    if (!zebkit_pdf_flatten(pdf, IntArg(args, 0), print, &error))
      return Failed(error, "the page cannot be flattened");
    return Ok(base::Value());
  }
  if (method == "attachments")
    return Json(zebkit_pdf_attachments(pdf, &error), error, "no attachments");
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

Opened Open(std::optional<std::string> bytes,
            std::string path,
            std::string password) {
  char* error = nullptr;
  const char* pass = password.empty() ? nullptr : password.c_str();
  zebkit_pdf* pdf =
      bytes ? zebkit_pdf_open(bytes->data(), bytes->size(), pass, &error)
            : zebkit_pdf_open_file(path.c_str(), pass, &error);
  Opened opened;
  opened.pdf = pdf;
  if (!pdf)
    opened.error = error ? error : "the document cannot be opened";
  free(error);
  return opened;
}

void CloseOnSequence(zebkit_pdf* pdf) {
  PdfSequence()->PostTask(FROM_HERE, base::BindOnce(&zebkit_pdf_close, pdf));
}

}  // namespace

void CallPdf(PageCall call) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (call.method == "open") {
    const base::Value* source = call.args.empty() ? nullptr : &call.args[0];
    std::optional<std::string> bytes;
    std::string path;
    if (source && source->is_string() && !call.view) {
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
    PdfSequence()->PostTaskAndReplyWithResult(
        FROM_HERE,
        base::BindOnce(&Open, std::move(bytes), std::move(path),
                       std::move(password)),
        base::BindOnce(
            [](base::WeakPtr<zebkit_view> view, bool from_page,
               std::string origin, CallReply reply, Opened opened) {
              if (!opened.pdf) {
                std::move(reply).Run(false, base::Value(opened.error));
                return;
              }
              // The page's view went while the document opened.
              if (from_page && !view) {
                CloseOnSequence(opened.pdf);
                return;
              }
              static int next_id = 1;
              const int id = next_id++;
              Documents()[id] = {opened.pdf, view.get(), std::move(origin)};
              std::move(reply).Run(true, base::Value(id));
            },
            std::move(view), from_page, call.origin, std::move(call.reply)));
    return;
  }

  const int id = call.args.empty() || !call.args[0].is_int()
                     ? 0
                     : call.args[0].GetInt();
  auto document = Documents().find(id);
  // A page has the documents its view and origin opened, and no others.
  if (document == Documents().end() ||
      document->second.owner != call.view ||
      (call.view && document->second.origin != call.origin)) {
    std::move(call.reply).Run(false, base::Value("no such document"));
    return;
  }
  zebkit_pdf* pdf = document->second.pdf;
  if (call.method == "close") {
    Documents().erase(document);
    CloseOnSequence(pdf);
    std::move(call.reply).Run(true, base::Value());
    return;
  }
  base::ListValue args;
  for (size_t i = 1; i < call.args.size(); ++i)
    args.Append(call.args[i].Clone());
  PdfSequence()->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&Work, pdf, call.method, std::move(args)),
      base::BindOnce(&Reply, std::move(call.reply)));
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
