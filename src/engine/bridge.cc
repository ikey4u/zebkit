// What a page's zebkit.* calls do, once the browser has said which origin
// made them and what it may use: layer 2 of the API (services.h), and the
// application's own functions.

#ifdef UNSAFE_BUFFERS_BUILD
// The C API takes and returns C strings and arrays.
#pragma allow_unsafe_buffers
#endif

#include <stdlib.h>
#include <string.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/base64.h"
#include "base/files/file.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "zebkit/engine/path_utf8.h"
#include "zebkit/engine/include/zebkit/canvas.h"
#include "zebkit/engine/include/zebkit/services.h"
#include "zebkit/engine/include/zebkit/text.h"
#include "zebkit/engine/page_api.h"
#include "zebkit/engine/view.h"

namespace zebkit {


PageCall::PageCall() = default;
PageCall::PageCall(PageCall&&) = default;
PageCall& PageCall::operator=(PageCall&&) = default;
PageCall::~PageCall() = default;

namespace {

using page_api::Error;
using page_api::Ok;
using page_api::Result;

base::Value Bytes(const uint8_t* data, size_t size) {
  return base::Value(base::Value::BlobStorage(data, data + size));
}

base::Value Bytes(const std::string& data) {
  return Bytes(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}


const base::Value* Arg(const PageCall& call, size_t index) {
  return index < call.args.size() ? &call.args[index] : nullptr;
}

std::string StringArg(const PageCall& call, size_t index) {
  const base::Value* value = Arg(call, index);
  return value && value->is_string() ? value->GetString() : std::string();
}

// Takes a string the engine returned (zebkit_free) into a std::string.
std::string Take(char* text) {
  std::string out = text ? text : "";
  free(text);
  return out;
}

void Reply(CallReply reply, Result result) {
  std::move(reply).Run(result.ok, std::move(result.value));
}

// ---- zebkit.url: registrableDomain needs the engine's list of public
// suffixes (//net); parse is page_api's ----

Result Url(const PageCall& call) {
  if (call.method == "registrableDomain") {
    char* domain = zebkit_url_registrable_domain(StringArg(call, 0).c_str());
    if (!domain)
      return Ok(base::Value());
    return Ok(base::Value(Take(domain)));
  }
  return page_api::UrlParse(call.method, call.args);
}

// ---- zebkit.fetch ----

struct FetchCall {
  CallReply reply;
};

void FetchDone(const zebkit_response* response, void* data) {
  std::unique_ptr<FetchCall> call(static_cast<FetchCall*>(data));
  if (response->error) {
    Reply(std::move(call->reply), Error(response->error));
    return;
  }
  base::DictValue headers;
  std::string_view lines(response->headers ? response->headers : "");
  while (!lines.empty()) {
    size_t end = lines.find("\r\n");
    std::string_view line = lines.substr(0, end);
    lines = end == std::string_view::npos ? std::string_view()
                                          : lines.substr(end + 2);
    const size_t colon = line.find(": ");
    if (colon == std::string_view::npos)
      continue;
    std::string name = base::ToLowerASCII(line.substr(0, colon));
    std::string value(line.substr(colon + 2));
    if (const std::string* before = headers.FindString(name))
      value = *before + ", " + value;
    headers.Set(name, value);
  }
  base::DictValue result;
  result.Set("status", response->status);
  result.Set("url", response->url ? response->url : "");
  result.Set("headers", std::move(headers));
  result.Set("body", Bytes(response->body, response->body_size));
  Reply(std::move(call->reply), Ok(base::Value(std::move(result))));
}

void Fetch(PageCall call) {
  const std::string url = StringArg(call, 0);
  std::string method = "GET";
  std::string headers;
  std::optional<std::string> body;
  if (const base::Value* options = Arg(call, 1); options && options->is_dict()) {
    if (const std::string* m = options->GetDict().FindString("method"))
      method = *m;
    if (const base::DictValue* h = options->GetDict().FindDict("headers")) {
      for (auto [name, value] : *h) {
        if (!value.is_string())
          continue;
        if (!headers.empty())
          headers += "\r\n";
        headers += name + ": " + value.GetString();
      }
    }
    body = GetBytes(options->GetDict().Find("body"));
  }
  auto* pending = new FetchCall{std::move(call.reply)};
  zebkit_fetch(method.c_str(), url.c_str(),
               headers.empty() ? nullptr : headers.c_str(),
               body ? body->data() : nullptr, body ? body->size() : 0,
               &FetchDone, pending);
}

using page_api::Needs;

}  // namespace

void HandlePageCall(PageCall call) {
  const unsigned needs = Needs(call.module);
  if (!needs) {
    Reply(std::move(call.reply), Error("zebkit has no " + call.module));
    return;
  }
  if (!(call.allow & needs)) {
    Reply(std::move(call.reply),
          Error("zebkit." + call.module + " is not allowed for " +
                call.origin));
    return;
  }
  if (call.module == "app") {
    if (!call.view) {
      Reply(std::move(call.reply), Error("zebkit.app is for pages"));
      return;
    }
    call.view->CallApp(std::move(call));
    return;
  }
  if (call.module == "fetch") {
    Fetch(std::move(call));
    return;
  }
  if (call.module == "url") {
    Result result = Url(call);
    Reply(std::move(call.reply), std::move(result));
    return;
  }
  if (call.module == "pdf") {
    CallPdf(std::move(call));
    return;
  }
  // The rest reads files or does work that takes time: off the UI thread.
  CallReply reply = std::move(call.reply);
  base::OnceCallback<Result()> work;
  if (call.module == "fs") {
    const base::Value* data = Arg(call, 1);
    const base::Value* options = Arg(call, 1);
    const bool recursive =
        call.method == "remove" && options && options->is_dict() &&
        options->GetDict().FindBool("recursive").value_or(false);
    work = base::BindOnce(&page_api::Files, call.files_root, call.method,
                          StringArg(call, 0),
                          call.method == "writeFile" ? GetBytes(data)
                                                     : std::nullopt,
                          recursive);
  } else {
    work = base::BindOnce(&page_api::Compute, call.module, call.method,
                          std::move(call.args));
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      std::move(work), base::BindOnce(&Reply, std::move(reply)));
}

}  // namespace zebkit