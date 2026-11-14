// <zebkit/viewer.h>: the page's global zebkit in the system's web view
// (docs/VIEWER.md). The page's side is zebkit's own: zebkit.pdf's wrapper
// (pdf_script.h) over a send() that posts JSON through the web view; the
// calls are answered here with what the engine's bridge answers them with
// (page_api.h), so that a page sees the same zebkit in zebkit's views and in
// WKWebView or Android's WebView.

#ifdef UNSAFE_BUFFERS_BUILD
// The C API takes and returns C strings and arrays.
#pragma allow_unsafe_buffers
#endif

#include "zebkit/engine/include/zebkit/viewer.h"

#include <stdlib.h>
#include <string.h>

#include <map>
#include <optional>
#include <string>
#include <utility>

#include "base/files/file_path.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/synchronization/lock.h"
#include "base/values.h"
#include "zebkit/engine/include/zebkit/pdf.h"
#include "zebkit/engine/page_api.h"
#include "zebkit/engine/path_utf8.h"
#include "zebkit/engine/pdf_script.h"

namespace {

// The page's side, before the page's own script: @post sends a string to the
// application, @makePdf is zebkit.pdf's wrapper (pdf_script.h). Bytes go as
// {$bytes: base64} both ways; each call has an id, and the application
// answers it by running __zebkit_answer(id, ok, text) in the page.
constexpr char kViewerScript[] = R"js(((post, makePdf) => {
  if (globalThis.zebkit || typeof post !== 'function') return;
  // The page's own script may change these later.
  const {isView} = ArrayBuffer;
  const {stringify, parse} = JSON;
  const {isArray} = Array;
  const {entries, keys, freeze, fromEntries} = Object;
  const toBase64 = (bytes) => {
    let s = '';
    for (let i = 0; i < bytes.length; i += 0x8000)
      s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
    return btoa(s);
  };
  const fromBase64 = (text) => {
    const s = atob(text);
    const out = new Uint8Array(s.length);
    for (let i = 0; i < s.length; i++) out[i] = s.charCodeAt(i);
    return out;
  };
  const encode = (value) => {
    if (value instanceof ArrayBuffer || isView(value)) {
      const bytes = value instanceof ArrayBuffer ? new Uint8Array(value)
          : new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
      return {$bytes: toBase64(bytes)};
    }
    if (isArray(value)) return value.map(encode);
    if (value && typeof value === 'object') {
      const out = {};
      for (const [k, v] of entries(value)) out[k] = encode(v);
      return out;
    }
    return value === undefined ? null : value;
  };
  const decode = (value) => {
    if (isArray(value)) return value.map(decode);
    if (value && typeof value === 'object') {
      if (typeof value.$bytes === 'string' && keys(value).length === 1)
        return fromBase64(value.$bytes);
      const out = {};
      for (const [k, v] of entries(value)) out[k] = decode(v);
      return out;
    }
    return value;
  };
  const pending = new Map();
  let next = 1;
  Object.defineProperty(globalThis, '__zebkit_answer', {
    value: (id, ok, text) => {
      const call = pending.get(id);
      if (!call) return;
      pending.delete(id);
      if (!ok) call.reject(new Error(text));
      else call.resolve(text === null || text === '' ? undefined : decode(parse(text)));
    },
  });
  const send = (module, method, args) => new Promise((resolve, reject) => {
    const id = next++;
    pending.set(id, {resolve, reject});
    try {
      post(stringify({zebkit: 1, id, module, method, args: encode(args)}));
    } catch (error) {
      pending.delete(id);
      reject(error);
    }
  });
  const api = (module, methods) => freeze(fromEntries(
      methods.map((m) => [m, (...args) => send(module, m, args)])));
  const pdf = makePdf(send);
  const zebkit = {
    app: new Proxy({}, {get: (_, name) =>
        typeof name === 'string' ? (...args) => send('app', name, args) : undefined}),
    fetch: (url, options) => send('fetch', 'fetch', [String(url), options || {}]),
    fs: api('fs', ['readFile', 'readText', 'writeFile', 'readDir', 'stat', 'mkdir', 'remove']),
    compress: (format, bytes, level) => send('compress', 'compress', [format, bytes, level ?? -1]),
    decompress: (format, bytes) => send('compress', 'decompress', [format, bytes]),
    crypto: api('crypto', ['digest', 'hmac', 'randomBytes', 'aesGcmSeal', 'aesGcmOpen']),
    image: api('image', ['decode', 'encode']),
    url: api('url', ['parse', 'registrableDomain']),
    pdf,
    canvas: pdf.canvas,
    text: pdf.text,
    // Files dropped from another application are zebkit's views' alone.
    pathForFile: () => '',
  };
  Object.defineProperty(globalThis, 'zebkit', {value: freeze(zebkit)});
}))js";

struct Grant {
  unsigned allow = 0;
  base::FilePath files_root;
};

std::string Json(const base::Value& value) {
  std::string out;
  base::JSONWriter::Write(value, &out);
  return out;
}

char* Give(const std::string& text) {
  return strdup(text.c_str());
}

std::string Answer(double id, bool ok, const base::Value* result) {
  // The answer's text as a JavaScript string: JSON's, which JavaScript reads.
  const std::string text =
      !result ? "null"
      : ok    ? Json(base::Value(Json(zebkit::ToPageValue(result->Clone()))))
              : Json(base::Value(result->is_string() ? result->GetString()
                                                     : Json(*result)));
  return base::StrCat({"globalThis.__zebkit_answer && __zebkit_answer(",
                       base::NumberToString(id), ", ", ok ? "true" : "false",
                       ", ", text, ");"});
}

std::string Answer(double id, const zebkit::page_api::Result& result) {
  if (result.ok && result.value.is_none())
    return Answer(id, true, nullptr);
  return Answer(id, result.ok, &result.value);
}

}  // namespace

struct zebkit_viewer {
  base::Lock lock;
  std::map<std::string, Grant> grants;
  // The documents the viewer's pages opened, by the id the page has.
  std::map<int, zebkit_pdf*> documents;
  int next_document = 1;

  void CloseDocument(zebkit_pdf* pdf) {
    zebkit::page_api::RecordedOperations().erase(pdf);
    zebkit::page_api::ScriptHooks().erase(pdf);
    zebkit_pdf_close(pdf);
  }

  void CloseAll() {
    for (auto& [id, pdf] : documents)
      CloseDocument(pdf);
    documents.clear();
  }

  const Grant* GrantFor(const std::string& origin) const {
    auto it = grants.find(origin);
    if (it == grants.end())
      it = grants.find("*");
    return it == grants.end() ? nullptr : &it->second;
  }

  zebkit::page_api::Result Pdf(const std::string& method,
                               base::ListValue args) {
    using zebkit::page_api::Error;
    using zebkit::page_api::Ok;
    if (method == "open" || method == "create") {
      char* error = nullptr;
      zebkit_pdf* pdf = nullptr;
      if (method == "create") {
        pdf = zebkit_pdf_new(&error);
      } else {
        // A page gives bytes; a string from a page is not a path.
        const base::Value* source = args.empty() ? nullptr : &args[0];
        std::optional<std::string> bytes =
            source && !source->is_string() ? zebkit::GetBytes(source)
                                           : std::nullopt;
        if (!bytes)
          return Error("open() takes the PDF's bytes");
        const std::string password = args.size() > 1 && args[1].is_string()
                                         ? args[1].GetString()
                                         : std::string();
        pdf = zebkit_pdf_open(bytes->data(), bytes->size(),
                              password.empty() ? nullptr : password.c_str(),
                              &error);
      }
      if (!pdf) {
        std::string message = error ? error : "the document cannot be opened";
        free(error);
        return Error(message);
      }
      const int id = next_document++;
      documents[id] = pdf;
      return Ok(base::Value(id));
    }
    auto find = [&](size_t index) -> zebkit_pdf* {
      if (index >= args.size() || !args[index].is_int())
        return nullptr;
      auto it = documents.find(args[index].GetInt());
      return it == documents.end() ? nullptr : it->second;
    };
    zebkit_pdf* pdf = find(0);
    if (!pdf)
      return Error("no such document");
    zebkit_pdf* other = nullptr;
    const bool names_another = method == "importPages" ||
                               method == "copyAnnotation" ||
                               method == "stampFromPage";
    if (names_another) {
      other = find(1);
      if (!other)
        return Error("no such document");
    }
    if (method == "close") {
      documents.erase(args[0].GetInt());
      CloseDocument(pdf);
      return Ok(base::Value());
    }
    // As the engine's bridge: the document's id goes, and a second
    // document's id leaves a null.
    base::ListValue rest;
    for (size_t i = other ? 2 : 1; i < args.size(); ++i)
      rest.Append(args[i].Clone());
    if (other)
      rest.Insert(rest.begin(), base::Value());
    return zebkit::page_api::PdfWork(pdf, other, method, std::move(rest));
  }
};

extern "C" {

zebkit_viewer* zebkit_viewer_new(void) {
  return new zebkit_viewer();
}

void zebkit_viewer_free(zebkit_viewer* viewer) {
  if (!viewer)
    return;
  {
    base::AutoLock hold(viewer->lock);
    viewer->CloseAll();
  }
  delete viewer;
}

void zebkit_viewer_grant(zebkit_viewer* viewer,
                         const char* origin,
                         unsigned allow,
                         const char* files_root) {
  if (!viewer || !origin)
    return;
  base::AutoLock hold(viewer->lock);
  if (!allow) {
    viewer->grants.erase(origin);
    return;
  }
  Grant& grant = viewer->grants[origin];
  grant.allow = allow;
  grant.files_root =
      files_root ? zebkit::PathFromUTF8(files_root) : base::FilePath();
}

void zebkit_viewer_reset(zebkit_viewer* viewer) {
  if (!viewer)
    return;
  base::AutoLock hold(viewer->lock);
  viewer->CloseAll();
}

char* zebkit_viewer_script(const char* post) {
  return Give(base::StrCat({kViewerScript, "(", post ? post : "null", ", ",
                            zebkit::kPdfScript, ");\n"}));
}

char* zebkit_viewer_handle(zebkit_viewer* viewer,
                           const char* origin,
                           const char* message,
                           char** app_call) {
  using zebkit::page_api::Error;
  using zebkit::page_api::Result;
  if (app_call)
    *app_call = nullptr;
  if (!viewer || !message)
    return nullptr;
  std::optional<base::Value> parsed = base::JSONReader::Read(message, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict())
    return nullptr;
  base::DictValue& call = parsed->GetDict();
  const std::optional<double> id = call.FindDouble("id");
  const std::string* module = call.FindString("module");
  const std::string* method = call.FindString("method");
  base::ListValue* args = call.FindList("args");
  if (call.FindInt("zebkit").value_or(0) != 1 || !id || !module || !method ||
      !args) {
    return nullptr;
  }
  const std::string from = origin ? origin : "";

  base::AutoLock hold(viewer->lock);
  const Grant* grant = viewer->GrantFor(from);
  const unsigned needs = zebkit::page_api::Needs(*module);
  if (!needs)
    return Give(Answer(*id, Error("zebkit has no " + *module)));
  if (!grant || !(grant->allow & needs)) {
    return Give(
        Answer(*id, Error("zebkit." + *module + " is not allowed for " + from)));
  }
  if (*module == "app" || *module == "fetch") {
    // The application's: its function, the system's HTTP. The arguments go
    // as the page sent them, bytes as {$bytes}.
    base::DictValue out;
    out.Set("id", *id);
    out.Set("module", *module);
    out.Set("method", *method);
    out.Set("args", std::move(*args));
    if (app_call)
      *app_call = Give(Json(base::Value(std::move(out))));
    return nullptr;
  }
  base::Value values = zebkit::FromPageValue(base::Value(std::move(*args)));
  base::ListValue list = std::move(values.GetList());
  Result result;
  if (*module == "url") {
    result = *method == "registrableDomain"
                 ? Error("zebkit.url.registrableDomain needs zebkit's engine")
                 : zebkit::page_api::UrlParse(*method, list);
  } else if (*module == "pdf") {
    result = viewer->Pdf(*method, std::move(list));
  } else if (*module == "fs") {
    const std::string path =
        !list.empty() && list[0].is_string() ? list[0].GetString() : "";
    const base::Value* second = list.size() > 1 ? &list[1] : nullptr;
    const bool recursive =
        *method == "remove" && second && second->is_dict() &&
        second->GetDict().FindBool("recursive").value_or(false);
    result = zebkit::page_api::Files(
        grant->files_root, *method, path,
        *method == "writeFile" ? zebkit::GetBytes(second) : std::nullopt,
        recursive);
  } else {
    result = zebkit::page_api::Compute(*module, *method, std::move(list));
  }
  return Give(Answer(*id, result));
}

char* zebkit_viewer_answer(long long id, bool ok, const char* result_json) {
  if (!result_json)
    return Give(Answer(static_cast<double>(id), ok, nullptr));
  if (!ok) {
    base::Value message(result_json);
    return Give(Answer(static_cast<double>(id), false, &message));
  }
  // Already the page's JSON: passed on as it is, if it is JSON.
  std::optional<base::Value> value = base::JSONReader::Read(result_json, base::JSON_PARSE_RFC);
  if (!value) {
    base::Value message("the application's answer is not JSON");
    return Give(Answer(static_cast<double>(id), false, &message));
  }
  return Give(base::StrCat({"globalThis.__zebkit_answer && __zebkit_answer(",
                            base::NumberToString(id), ", true, ",
                            Json(base::Value(result_json)), ");"}));
}

}  // extern "C"
