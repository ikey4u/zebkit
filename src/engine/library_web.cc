// The native side of a library's Web APIs (library_bootstrap.h): UTF-8 text,
// base64, URL on GURL, structuredClone on V8's serializer, random values,
// the clock, and fetch() through the engine's network stack to the origins
// the library was granted, without cookies.

#ifdef UNSAFE_BUFFERS_BUILD
// Bytes of typed arrays, by pointer and length.
#pragma allow_unsafe_buffers
#endif

#include <stdlib.h>
#include <string.h>

#include <string>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/functional/bind.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "base/uuid.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/storage_partition.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_util.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "net/url_request/redirect_info.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/cpp/simple_url_loader_stream_consumer.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-container.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-typed-array.h"
#include "v8/include/v8-value-serializer.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/library.h"

namespace zebkit {

namespace {

v8::Local<v8::String> String8(v8::Isolate* isolate, std::string_view text) {
  return v8::String::NewFromUtf8(isolate, text.data(), v8::NewStringType::kNormal, static_cast<int>(text.size()))
      .ToLocalChecked();
}

std::string Utf8(v8::Isolate* isolate, v8::Local<v8::Value> value) {
  v8::String::Utf8Value text(isolate, value);
  return *text ? std::string(*text, text.length()) : std::string();
}

// The bytes a view covers.
base::span<uint8_t> ViewBytes(v8::Local<v8::ArrayBufferView> view) {
  uint8_t* data = static_cast<uint8_t*>(view->Buffer()->Data());
  return data ? base::span<uint8_t>(data + view->ByteOffset(), view->ByteLength()) : base::span<uint8_t>();
}

v8::Local<v8::Value> NewBytes(v8::Isolate* isolate, const void* data, size_t size) {
  v8::Local<v8::ArrayBuffer> buffer;
  if (!v8::ArrayBuffer::MaybeNew(isolate, size).ToLocal(&buffer)) {
    isolate->ThrowException(v8::Exception::RangeError(String8(isolate, "no memory within the library's heap limit")));
    return {};
  }
  if (size)
    memcpy(buffer->Data(), data, size);
  return v8::Uint8Array::New(buffer, 0, size);
}

// [href, protocol, username, password, host, hostname, port, pathname,
// search, hash, origin], as URL has them.
v8::Local<v8::Value> UrlParts(v8::Isolate* isolate, const GURL& url) {
  if (!url.is_valid())
    return v8::Null(isolate);
  const std::string hostname(url.host());
  const std::string port(url.port());
  const std::string query(url.query());
  const std::string ref(url.ref());
  const std::string parts[] = {
      url.spec(),
      std::string(url.scheme()) + ":",
      std::string(url.username()),
      std::string(url.password()),
      port.empty() ? hostname : hostname + ":" + port,
      hostname,
      port,
      std::string(url.path()),
      query.empty() ? std::string() : "?" + query,
      ref.empty() ? std::string() : "#" + ref,
      url::Origin::Create(url).Serialize(),
  };
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Array> array = v8::Array::New(isolate, std::size(parts));
  for (size_t i = 0; i < std::size(parts); ++i)
    std::ignore = array->Set(context, static_cast<uint32_t>(i), String8(isolate, parts[i]));
  return array;
}

class CloneDelegate : public v8::ValueSerializer::Delegate {
 public:
  explicit CloneDelegate(v8::Isolate* isolate) : isolate_(isolate) {}
  void ThrowDataCloneError(v8::Local<v8::String> message) override {
    v8::Local<v8::Value> error = v8::Exception::Error(message);
    std::ignore = error.As<v8::Object>()->Set(isolate_->GetCurrentContext(), String8(isolate_, "name"),
                                              String8(isolate_, "DataCloneError"));
    isolate_->ThrowException(error);
  }

 private:
  raw_ptr<v8::Isolate> isolate_;
};

void Encode(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  v8::Local<v8::String> text = info[0]->IsString() ? info[0].As<v8::String>() : v8::String::Empty(isolate);
  const size_t size = text->Utf8LengthV2(isolate);
  v8::Local<v8::ArrayBuffer> buffer;
  if (!v8::ArrayBuffer::MaybeNew(isolate, size, v8::BackingStoreInitializationMode::kUninitialized)
           .ToLocal(&buffer)) {
    isolate->ThrowException(v8::Exception::RangeError(String8(isolate, "no memory within the library's heap limit")));
    return;
  }
  text->WriteUtf8V2(isolate, static_cast<char*>(buffer->Data()), size, v8::String::WriteFlags::kReplaceInvalidUtf8);
  info.GetReturnValue().Set(v8::Uint8Array::New(buffer, 0, size));
}

// encodeInto(text, into): [UTF-16 units read, bytes written].
void EncodeInto(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  if (!info[0]->IsString() || !info[1]->IsUint8Array())
    return;
  base::span<uint8_t> into = ViewBytes(info[1].As<v8::Uint8Array>());
  size_t read = 0;
  const size_t written = info[0].As<v8::String>()->WriteUtf8V2(
      isolate, reinterpret_cast<char*>(into.data()), into.size(), v8::String::WriteFlags::kReplaceInvalidUtf8, &read);
  v8::Local<v8::Value> pair[] = {v8::Number::New(isolate, static_cast<double>(read)),
                                 v8::Number::New(isolate, static_cast<double>(written))};
  info.GetReturnValue().Set(v8::Array::New(isolate, pair, 2));
}

// decode(bytes, fatal, skipBOM): the text, or null when fatal and not UTF-8.
void Decode(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  if (!info[0]->IsArrayBufferView())
    return;
  base::span<uint8_t> bytes = ViewBytes(info[0].As<v8::ArrayBufferView>());
  if (info[2]->BooleanValue(isolate) && bytes.size() >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb &&
      bytes[2] == 0xbf) {
    bytes = bytes.subspan(3u);
  }
  std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  if (info[1]->BooleanValue(isolate) && !base::IsStringUTF8AllowingNoncharacters(text)) {
    info.GetReturnValue().SetNull();
    return;
  }
  v8::Local<v8::String> out;
  if (!v8::String::NewFromUtf8(isolate, text.data(), v8::NewStringType::kNormal, static_cast<int>(text.size()))
           .ToLocal(&out)) {
    isolate->ThrowException(v8::Exception::RangeError(String8(isolate, "the text is too long for a string")));
    return;
  }
  info.GetReturnValue().Set(out);
}

void Btoa(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  v8::Local<v8::String> text = info[0].As<v8::String>();
  if (!text->ContainsOnlyOneByte()) {
    info.GetReturnValue().SetNull();
    return;
  }
  std::string bytes(text->Length(), '\0');
  text->WriteOneByteV2(isolate, 0, text->Length(), reinterpret_cast<uint8_t*>(bytes.data()));
  info.GetReturnValue().Set(String8(isolate, base::Base64Encode(bytes)));
}

void Atob(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  v8::Local<v8::String> text = info[0].As<v8::String>();
  if (!text->ContainsOnlyOneByte()) {
    info.GetReturnValue().SetNull();
    return;
  }
  std::string input(text->Length(), '\0');
  text->WriteOneByteV2(isolate, 0, text->Length(), reinterpret_cast<uint8_t*>(input.data()));
  std::string output;
  if (!base::Base64Decode(input, &output, base::Base64DecodePolicy::kForgiving)) {
    info.GetReturnValue().SetNull();
    return;
  }
  v8::Local<v8::String> out;
  if (v8::String::NewFromOneByte(isolate, reinterpret_cast<const uint8_t*>(output.data()),
                                 v8::NewStringType::kNormal, static_cast<int>(output.size()))
          .ToLocal(&out)) {
    info.GetReturnValue().Set(out);
  }
}

// url(input, base): URL's parts, or null.
void Url(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  const std::string input = Utf8(isolate, info[0]);
  if (info[1]->IsString()) {
    GURL base(Utf8(isolate, info[1]));
    info.GetReturnValue().Set(base.is_valid() ? UrlParts(isolate, base.Resolve(input)) : v8::Null(isolate).As<v8::Value>());
    return;
  }
  info.GetReturnValue().Set(UrlParts(isolate, GURL(input)));
}

// urlSet(href, part, value): the parts with that one changed, or null.
void UrlSet(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  GURL url(Utf8(isolate, info[0]));
  const std::string part = Utf8(isolate, info[1]);
  std::string value = Utf8(isolate, info[2]);
  if (!url.is_valid()) {
    info.GetReturnValue().SetNull();
    return;
  }
  GURL::Replacements change;
  std::string port;
  if (part == "protocol") {
    value = value.substr(0, value.find(':'));
    const bool special = url.SchemeIsHTTPOrHTTPS() || url.SchemeIs("ws") || url.SchemeIs("wss") ||
                         url.SchemeIs("ftp") || url.SchemeIsFile();
    GURL probe(value + "://x/");
    const bool to_special = probe.SchemeIsHTTPOrHTTPS() || probe.SchemeIs("ws") || probe.SchemeIs("wss") ||
                            probe.SchemeIs("ftp") || probe.SchemeIsFile();
    if (value.empty() || special != to_special) {
      info.GetReturnValue().SetNull();
      return;
    }
    change.SetSchemeStr(value);
  } else if (part == "username") {
    change.SetUsernameStr(value);
  } else if (part == "password") {
    change.SetPasswordStr(value);
  } else if (part == "host" || part == "hostname") {
    if (part == "host") {
      const size_t colon = value.rfind(':');
      if (colon != std::string::npos && value.find(']', colon) == std::string::npos) {
        port = value.substr(colon + 1);
        value = value.substr(0, colon);
      }
    }
    change.SetHostStr(value);
    if (!port.empty())
      change.SetPortStr(port);
  } else if (part == "port") {
    size_t digits = 0;
    while (digits < value.size() && base::IsAsciiDigit(value[digits]))
      ++digits;
    port = value.substr(0, digits);
    if (port.empty() && !value.empty()) {
      info.GetReturnValue().SetNull();
      return;
    }
    if (port.empty())
      change.ClearPort();
    else
      change.SetPortStr(port);
  } else if (part == "pathname") {
    change.SetPathStr(value);
  } else if (part == "search") {
    if (value.starts_with("?"))
      value.erase(0, 1);
    if (value.empty())
      change.ClearQuery();
    else
      change.SetQueryStr(value);
  } else if (part == "hash") {
    if (value.starts_with("#"))
      value.erase(0, 1);
    if (value.empty())
      change.ClearRef();
    else
      change.SetRefStr(value);
  } else {
    info.GetReturnValue().SetNull();
    return;
  }
  GURL changed = url.ReplaceComponents(change);
  info.GetReturnValue().Set(changed.is_valid() ? UrlParts(isolate, changed) : v8::Null(isolate).As<v8::Value>());
}

void Clone(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  CloneDelegate delegate(isolate);
  v8::ValueSerializer serializer(isolate, &delegate);
  serializer.WriteHeader();
  if (!serializer.WriteValue(context, info[0]).FromMaybe(false))
    return;
  std::pair<uint8_t*, size_t> buffer = serializer.Release();
  v8::ValueDeserializer deserializer(isolate, buffer.first, buffer.second);
  v8::Local<v8::Value> copy;
  if (deserializer.ReadHeader(context).FromMaybe(false) && deserializer.ReadValue(context).ToLocal(&copy))
    info.GetReturnValue().Set(copy);
  delegate.FreeBufferMemory(buffer.first);
}

void RandomFill(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (info[0]->IsArrayBufferView())
    base::RandBytes(ViewBytes(info[0].As<v8::ArrayBufferView>()));
}

void Uuid(const v8::FunctionCallbackInfo<v8::Value>& info) {
  info.GetReturnValue().Set(String8(info.GetIsolate(), base::Uuid::GenerateRandomV4().AsLowercaseString()));
}

void QueueMicrotask(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (!info[0]->IsFunction())
    return;
  v8::Isolate* isolate = info.GetIsolate();
  v8::MicrotaskQueue* queue = isolate->GetCurrentContext()->GetMicrotaskQueue();
  if (queue)
    queue->EnqueueMicrotask(isolate, info[0].As<v8::Function>());
}

// A request of a library's fetch(), on the engine's thread: through the
// engine's network stack, no cookies sent or kept, a redirect followed only to
// a granted origin; the answer to the library's thread.
class LibraryFetch : public network::SimpleURLLoaderStreamConsumer {
 public:
  using Done = base::OnceCallback<void(std::string error, int status, std::string url, std::string headers,
                                       std::string body, bool redirected)>;

  static void Start(std::string method, GURL url, std::string headers, std::string body, bool has_body,
                    std::vector<url::Origin> granted, size_t max_bytes, Done done) {
    content::BrowserContext* profile = GetBrowserContext();
    if (!profile) {
      std::move(done).Run("the engine is not running", 0, "", "", "", false);
      return;
    }
    auto request = std::make_unique<network::ResourceRequest>();
    request->url = url;
    request->method = method;
    request->credentials_mode = network::mojom::CredentialsMode::kOmit;
    std::string content_type;
    for (std::string_view line :
         base::SplitStringPieceUsingSubstr(headers, "\r\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
      const size_t colon = line.find(':');
      if (colon == std::string_view::npos)
        continue;
      std::string name(base::TrimWhitespaceASCII(line.substr(0, colon), base::TRIM_ALL));
      std::string value(base::TrimWhitespaceASCII(line.substr(colon + 1), base::TRIM_ALL));
      if (base::EqualsCaseInsensitiveASCII(name, "content-type"))
        content_type = value;
      else if (net::HttpUtil::IsValidHeaderName(name) && net::HttpUtil::IsValidHeaderValue(value))
        request->headers.SetHeader(name, value);
    }
    constexpr net::NetworkTrafficAnnotationTag kAnnotation = net::DefineNetworkTrafficAnnotation("zebkit_library_fetch", R"(
          semantics {
            sender: "zebkit"
            description: "A fetch() of a JavaScript library the application loaded."
            trigger: "The library calls fetch() for an origin the application granted it."
            data: "What the library sends."
            destination: OTHER
          }
          policy {
            cookies_allowed: NO
            setting: "Up to the application."
            policy_exception_justification: "Made by the application's library."
          })");
    auto* fetch = new LibraryFetch(std::move(granted), max_bytes, url, std::move(done));
    fetch->loader_ = network::SimpleURLLoader::Create(std::move(request), kAnnotation);
    fetch->loader_->SetOnResponseStartedCallback(
        base::BindOnce(&LibraryFetch::ResponseStarted, base::Unretained(fetch)));
    fetch->loader_->SetOnRedirectCallback(base::BindRepeating(&LibraryFetch::Redirected, base::Unretained(fetch)));
    if (has_body)
      fetch->loader_->AttachStringForUpload(std::move(body),
                                            content_type.empty() ? "application/octet-stream" : content_type);
    fetch->loader_->SetAllowHttpErrorResults(true);
    scoped_refptr<network::SharedURLLoaderFactory> factory =
        profile->GetDefaultStoragePartition()->GetURLLoaderFactoryForBrowserProcess();
    fetch->loader_->DownloadAsStream(factory.get(), fetch);
  }

 private:
  LibraryFetch(std::vector<url::Origin> granted, size_t max_bytes, GURL url, Done done)
      : granted_(std::move(granted)), max_bytes_(max_bytes), first_url_(std::move(url)), done_(std::move(done)) {}
  ~LibraryFetch() override = default;

  void ResponseStarted(const GURL& url, const network::mojom::URLResponseHead& head) {
    response_url_ = url.spec();
    response_headers_ = head.headers;
  }

  void Redirected(const GURL& from, const net::RedirectInfo& info, const network::mojom::URLResponseHead&,
                  std::vector<std::string>* removed) {
    const url::Origin to = url::Origin::Create(info.new_url);
    for (const url::Origin& origin : granted_) {
      if (origin.IsSameOriginWith(to))
        return;
    }
    Finish("redirected to " + to.Serialize() + ", which the library was not granted", false);
  }

  void OnDataReceived(std::string_view chunk, base::OnceClosure resume) override {
    if (chunk.size() > max_bytes_ - std::min(max_bytes_, body_.size())) {
      Finish("the response is larger than maxBytes", false);
      return;
    }
    body_.append(chunk);
    std::move(resume).Run();
  }

  void OnComplete(bool success) override {
    Finish(success ? std::string() : net::ErrorToString(loader_->NetError()), success);
  }

  void OnRetry(base::OnceClosure start_retry) override {
    body_.clear();
    std::move(start_retry).Run();
  }

  void Finish(std::string error, bool complete) {
    std::string headers;
    const net::HttpResponseHeaders* received = response_headers_.get();
    if (complete && loader_->ResponseInfo() && loader_->ResponseInfo()->headers)
      received = loader_->ResponseInfo()->headers.get();
    int status = 0;
    if (received && error.empty()) {
      status = received->response_code();
      size_t iter = 0;
      std::string name, value;
      while (received->EnumerateHeaderLines(&iter, &name, &value)) {
        if (!headers.empty())
          headers += "\r\n";
        headers += name + ": " + value;
      }
    }
    const std::string url = complete ? loader_->GetFinalURL().spec() : response_url_;
    std::move(done_).Run(std::move(error), status, url, std::move(headers),
                         error.empty() ? std::move(body_) : std::string(), url != first_url_.spec());
    delete this;
  }

  std::vector<url::Origin> granted_;
  size_t max_bytes_;
  GURL first_url_;
  Done done_;
  std::unique_ptr<network::SimpleURLLoader> loader_;
  std::string body_;
  std::string response_url_;
  scoped_refptr<net::HttpResponseHeaders> response_headers_;
};

}  // namespace

void LibraryIsolate::InstallWeb(v8::Local<v8::Object> native) {
  SetMethod(native, "encode", &Encode);
  SetMethod(native, "encodeInto", &EncodeInto);
  SetMethod(native, "decode", &Decode);
  SetMethod(native, "btoa", &Btoa);
  SetMethod(native, "atob", &Atob);
  SetMethod(native, "url", &Url);
  SetMethod(native, "urlSet", &UrlSet);
  SetMethod(native, "clone", &Clone);
  SetMethod(native, "randomFill", &RandomFill);
  SetMethod(native, "uuid", &Uuid);
  SetMethod(native, "queueMicrotask", &QueueMicrotask);
  SetMethod(native, "now", [](const v8::FunctionCallbackInfo<v8::Value>& info) {
    info.GetReturnValue().Set((base::TimeTicks::Now() - From(info)->start_).InMillisecondsF());
  });
  std::ignore = native->Set(context(), String8(isolate_, "timeOrigin"),
                            v8::Number::New(isolate_, base::Time::Now().InMillisecondsFSinceUnixEpoch()));
}

// fetch(method, url, headers, body, done): only to the granted origins;
// throws (a TypeError) otherwise, which rejects the script's promise.
void LibraryIsolate::Fetch(const v8::FunctionCallbackInfo<v8::Value>& info) {
  LibraryIsolate* self = From(info);
  v8::Isolate* isolate = info.GetIsolate();
  const std::string method = Utf8(isolate, info[0]);
  GURL url(Utf8(isolate, info[1]));
  auto refuse = [&](const std::string& why) {
    isolate->ThrowException(v8::Exception::TypeError(String8(isolate, "fetch: " + why)));
  };
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS())
    return refuse("only http and https URLs");
  if (!net::HttpUtil::IsToken(method))
    return refuse("not a method: " + method);
  const url::Origin origin = url::Origin::Create(url);
  const std::vector<url::Origin>& granted = self->library_->options.network;
  if (std::none_of(granted.begin(), granted.end(), [&](const url::Origin& o) { return o.IsSameOriginWith(origin); })) {
    return refuse(origin.Serialize() + " is not an origin the library was given (the load's \"network\")");
  }
  if (!info[4]->IsFunction())
    return refuse("no callback");
  std::string body;
  const bool has_body = info[3]->IsArrayBufferView();
  if (has_body) {
    base::span<uint8_t> bytes = ViewBytes(info[3].As<v8::ArrayBufferView>());
    body.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }
  const int id = self->next_fetch_++;
  self->fetches_[id].Reset(isolate, info[4].As<v8::Function>());
  scoped_refptr<base::SingleThreadTaskRunner> back = base::SingleThreadTaskRunner::GetCurrentDefault();
  LibraryFetch::Done done = base::BindOnce(
      [](scoped_refptr<base::SingleThreadTaskRunner> back, base::WeakPtr<LibraryIsolate> self, int id,
         std::string error, int status, std::string url, std::string headers, std::string body, bool redirected) {
        back->PostTask(FROM_HERE, base::BindOnce(&LibraryIsolate::FetchDone, self, id, std::move(error), status,
                                                 std::move(url), std::move(headers), std::move(body), redirected));
      },
      back, self->weak_factory_.GetWeakPtr(), id);
  if (!PostEngineTask(base::BindOnce(&LibraryFetch::Start, method, url, Utf8(isolate, info[2]), std::move(body),
                                     has_body, granted, self->library_->options.max_bytes, std::move(done)),
                      base::TimeDelta())) {
    self->fetches_.erase(id);
    return refuse("the engine is not running");
  }
}

void LibraryIsolate::FetchDone(int id, std::string error, int status, std::string url, std::string headers,
                               std::string body, bool redirected) {
  auto it = fetches_.find(id);
  if (it == fetches_.end() || ended_)
    return;
  Scope scope(this);
  v8::Local<v8::Function> done = it->second.Get(isolate_);
  fetches_.erase(it);
  v8::TryCatch try_catch(isolate_);
  v8::Local<v8::Value> bytes = NewBytes(isolate_, body.data(), body.size());
  if (bytes.IsEmpty()) {
    error = "the response does not fit in the library's heap";
    bytes = v8::Null(isolate_);
    try_catch.Reset();
  }
  v8::Local<v8::Value> argv[] = {error.empty() ? v8::Null(isolate_).As<v8::Value>() : String8(isolate_, error),
                                 v8::Integer::New(isolate_, status), String8(isolate_, url),
                                 String8(isolate_, headers), bytes, v8::Boolean::New(isolate_, redirected)};
  if (done->Call(context(), v8::Undefined(isolate_), 6, argv).IsEmpty() && !isolate_->IsExecutionTerminating())
    Uncaught("Uncaught " + Utf8(isolate_, try_catch.Exception()));
}

}  // namespace zebkit
