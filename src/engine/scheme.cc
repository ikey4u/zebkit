// zebkit_scheme_*: URL schemes of the application's own, as Chromium's own
// non-network schemes are made (content/browser/data_url_loader_factory.cc):
// a URL loader factory per scheme in the browser process, whose loads the
// application answers.

#ifdef UNSAFE_BUFFERS_BUILD
// The C API takes and returns C strings and arrays.
#pragma allow_unsafe_buffers
#endif

#include "zebkit/engine/scheme.h"

#include <stdlib.h>
#include <string.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/memory/self_deleting.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/child_process_security_policy.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/data_pipe_producer.h"
#include "mojo/public/cpp/system/file_data_source.h"
#include "mojo/public/cpp/system/string_data_source.h"
#include "net/base/mime_util.h"
#include "net/http/http_byte_range.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_status_code.h"
#include "net/http/http_util.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/self_deleting_url_loader_factory.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/web/web_security_policy.h"
#include "zebkit/engine/include/zebkit/zebkit.h"

struct zebkit_request {
  std::string url;
  std::string method;
  std::string headers;
  std::vector<uint8_t> body;
  bool has_body = false;
  // Kept until the request is answered: a loader that goes away early is a
  // cancelled request to the page.
  mojo::PendingReceiver<network::mojom::URLLoader> loader;
  mojo::PendingRemote<network::mojom::URLLoaderClient> client;
};

namespace zebkit {

namespace {

struct Handler {
  zebkit_scheme_func func = nullptr;
  void* data = nullptr;
};

std::vector<Scheme>& MutableSchemes() {
  static base::NoDestructor<std::vector<Scheme>> schemes;
  return *schemes;
}

std::map<std::string, Handler>& Handlers() {
  static base::NoDestructor<std::map<std::string, Handler>> handlers;
  return *handlers;
}

constexpr struct {
  const char* name;
  unsigned flag;
} kFlags[] = {
    {"standard", ZEBKIT_SCHEME_STANDARD},
    {"secure", ZEBKIT_SCHEME_SECURE},
    {"fetch", ZEBKIT_SCHEME_FETCH},
    {"bypass-csp", ZEBKIT_SCHEME_BYPASS_CSP},
};

// Schemes the engine has of its own, which an application cannot take.
bool IsReservedScheme(const std::string& name) {
  static constexpr const char* kReserved[] = {
      "http",  "https",      "ws",          "wss",   "file",  "data",
      "blob",  "about",      "javascript",  "chrome", "chrome-untrusted",
      "devtools", "filesystem", "ftp",      "mailto", "content", "view-source",
      "chrome-error", "chrome-extension",
      // The engine's own.
      "zebkit",
  };
  for (const char* reserved : kReserved) {
    if (name == reserved)
      return true;
  }
  return false;
}

bool IsValidName(const std::string& name) {
  if (name.empty() || !base::IsAsciiLower(name[0]))
    return false;
  for (char c : name) {
    if (!base::IsAsciiLower(c) && !base::IsAsciiDigit(c) && c != '+' &&
        c != '-' && c != '.') {
      return false;
    }
  }
  return true;
}

bool Add(const std::string& name, unsigned flags) {
  if (!IsValidName(name) || IsReservedScheme(name))
    return false;
  for (Scheme& scheme : MutableSchemes()) {
    if (scheme.name == name) {
      scheme.flags |= flags;
      return true;
    }
  }
  MutableSchemes().push_back({name, flags});
  return true;
}

// --zebkit-scheme=app,standard,secure;asset,standard
void ReadSwitch() {
  static bool read = false;
  if (read || !base::CommandLine::InitializedForCurrentProcess())
    return;
  read = true;
  const std::string value =
      base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
          kSchemeSwitch);
  for (const std::string& entry : base::SplitString(
           value, ";", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    std::vector<std::string> parts = base::SplitString(
        entry, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
    if (parts.empty())
      continue;
    unsigned flags = 0;
    for (size_t i = 1; i < parts.size(); ++i) {
      for (const auto& flag : kFlags) {
        if (parts[i] == flag.name)
          flags |= flag.flag;
      }
    }
    Add(base::ToLowerASCII(parts[0]), flags);
  }
}

std::string ReasonPhrase(int status) {
  return std::string(
      net::GetHttpReasonPhrase(static_cast<net::HttpStatusCode>(status)));
}

// The response head for @status and the application's "Name: value" lines.
network::mojom::URLResponseHeadPtr MakeHead(int status,
                                            const std::string& headers,
                                            int64_t length) {
  std::string raw =
      "HTTP/1.1 " + base::NumberToString(status) + " " + ReasonPhrase(status);
  for (const std::string& line : base::SplitString(
           headers, "\r\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    raw += "\n" + line;
  }
  auto head = network::mojom::URLResponseHead::New();
  head->headers = net::HttpResponseHeaders::TryToCreate(raw);
  if (!head->headers) {
    head->headers = base::MakeRefCounted<net::HttpResponseHeaders>(
        "HTTP/1.1 " + base::NumberToString(status));
  }
  if (length >= 0) {
    head->headers->SetHeader("Content-Length", base::NumberToString(length));
    head->content_length = length;
  }
  head->headers->GetMimeTypeAndCharset(&head->mime_type, &head->charset);
  return head;
}

std::string HeaderValue(const std::string& headers, std::string_view name) {
  for (const std::string& line : base::SplitString(
           headers, "\r\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    const size_t colon = line.find(':');
    if (colon != std::string::npos &&
        base::EqualsCaseInsensitiveASCII(line.substr(0, colon), name)) {
      return std::string(
          base::TrimWhitespaceASCII(line.substr(colon + 1), base::TRIM_ALL));
    }
  }
  return std::string();
}

// What is being sent: kept until the body is written.
struct Sending {
  std::unique_ptr<zebkit_request> request;
  mojo::Remote<network::mojom::URLLoaderClient> client;
  std::unique_ptr<mojo::DataPipeProducer> producer;
  std::string body;
  int64_t length = 0;
};

void Complete(std::unique_ptr<Sending> sending, MojoResult result) {
  network::URLLoaderCompletionStatus status(
      result == MOJO_RESULT_OK ? net::OK : net::ERR_FAILED);
  const base::ByteSize length(static_cast<uint64_t>(sending->length));
  status.encoded_data_length = length;
  status.encoded_body_length = length;
  status.decoded_body_length = length;
  sending->client->OnComplete(status);
}

// Sends a head and a body from @source, on the current sequence.
void Send(std::unique_ptr<zebkit_request> request,
          network::mojom::URLResponseHeadPtr head,
          std::unique_ptr<mojo::DataPipeProducer::DataSource> source,
          std::string owned_body,
          int64_t length) {
  auto sending = std::make_unique<Sending>();
  sending->client.Bind(std::move(request->client));
  sending->request = std::move(request);
  sending->body = std::move(owned_body);
  sending->length = length;
  mojo::ScopedDataPipeProducerHandle producer;
  mojo::ScopedDataPipeConsumerHandle consumer;
  if (mojo::CreateDataPipe(nullptr, producer, consumer) != MOJO_RESULT_OK) {
    sending->client->OnComplete(
        network::URLLoaderCompletionStatus(net::ERR_INSUFFICIENT_RESOURCES));
    return;
  }
  sending->client->OnReceiveResponse(std::move(head), std::move(consumer),
                                     std::nullopt);
  if (!source) {
    source = std::make_unique<mojo::StringDataSource>(
        sending->body, mojo::StringDataSource::AsyncWritingMode::
                           STRING_STAYS_VALID_UNTIL_COMPLETION);
  }
  sending->producer =
      std::make_unique<mojo::DataPipeProducer>(std::move(producer));
  mojo::DataPipeProducer* writer = sending->producer.get();
  writer->Write(std::move(source),
                base::BindOnce(&Complete, std::move(sending)));
}

scoped_refptr<base::SequencedTaskRunner> AnswerSequence() {
  return base::ThreadPool::CreateSequencedTaskRunner(
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE});
}

void SendFile(std::unique_ptr<zebkit_request> request,
              const base::FilePath& path,
              std::string headers) {
  // A path with "..", as an application gets by joining its directory and
  // the URL's path (/files/../../etc/passwd), is not served: the file a
  // page names this way is one the application never meant to give it.
  base::File file;
  if (!path.ReferencesParent())
    file = base::File(path, base::File::FLAG_OPEN | base::File::FLAG_READ);
  const int64_t size = file.IsValid() ? file.GetLength() : -1;
  if (size < 0) {
    std::string body = "not found";
    Send(std::move(request),
         MakeHead(404, "Content-Type: text/plain", body.size()), nullptr,
         std::move(body), 9);
    return;
  }
  if (HeaderValue(headers, "Content-Type").empty()) {
    std::string mime;
    if (net::GetMimeTypeFromFile(path, &mime)) {
      headers += "\r\nContent-Type: " + mime;
    }
  }
  headers += "\r\nAccept-Ranges: bytes";
  int status = 200;
  int64_t first = 0, last = size - 1;
  std::vector<net::HttpByteRange> ranges;
  const std::string range = HeaderValue(request->headers, "Range");
  if (!range.empty() && net::HttpUtil::ParseRangeHeader(range, &ranges) &&
      ranges.size() == 1) {
    if (!ranges[0].ComputeBounds(size)) {
      headers += "\r\nContent-Range: bytes */" + base::NumberToString(size);
      Send(std::move(request), MakeHead(416, headers, 0), nullptr,
           std::string(), 0);
      return;
    }
    status = 206;
    first = ranges[0].first_byte_position();
    last = ranges[0].last_byte_position();
    headers += "\r\nContent-Range: bytes " + base::NumberToString(first) +
               "-" + base::NumberToString(last) + "/" +
               base::NumberToString(size);
  }
  const int64_t length = size ? last - first + 1 : 0;
  auto source = std::make_unique<mojo::FileDataSource>(std::move(file));
  source->SetRange(first, first + length);
  Send(std::move(request), MakeHead(status, headers, length),
       std::move(source), std::string(), length);
}

class LoaderFactory : public network::SelfDeletingURLLoaderFactory {
 public:
  LoaderFactory(std::string scheme,
                mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
                base::SelfDeletingPassKey key)
      : network::SelfDeletingURLLoaderFactory(std::move(receiver), key),
        scheme_(std::move(scheme)) {}

  void CreateLoaderAndStart(
      mojo::PendingReceiver<network::mojom::URLLoader> loader,
      int32_t request_id,
      uint32_t options,
      const network::ResourceRequest& resource_request,
      mojo::PendingRemote<network::mojom::URLLoaderClient> client,
      const net::MutableNetworkTrafficAnnotationTag& traffic_annotation)
      override {
    auto request = std::make_unique<zebkit_request>();
    request->url = resource_request.url.spec();
    request->method = resource_request.method;
    request->headers = resource_request.headers.ToString();
    if (resource_request.request_body) {
      for (const network::DataElement& element :
           *resource_request.request_body->elements()) {
        if (element.type() != network::DataElement::Tag::kBytes)
          continue;
        const auto bytes = element.As<network::DataElementBytes>().bytes();
        request->body.insert(request->body.end(), bytes.begin(), bytes.end());
        request->has_body = true;
      }
    }
    request->loader = std::move(loader);
    request->client = std::move(client);
    auto handler = Handlers().find(scheme_);
    if (handler == Handlers().end() || !handler->second.func) {
      zebkit_request_respond(request.release(), 404,
                             "Content-Type: text/plain", "not found", 9);
      return;
    }
    // The application owns it until it answers.
    handler->second.func(request.release(), handler->second.data);
  }

 private:
  ~LoaderFactory() override = default;

  const std::string scheme_;
};

}  // namespace

const std::vector<Scheme>& Schemes() {
  ReadSwitch();
  return MutableSchemes();
}

void AddSchemes(content::ContentClient::Schemes* schemes) {
  for (const Scheme& scheme : Schemes()) {
    if (scheme.flags & ZEBKIT_SCHEME_STANDARD)
      schemes->standard_schemes.push_back(scheme.name);
    if (scheme.flags & ZEBKIT_SCHEME_SECURE)
      schemes->secure_schemes.push_back(scheme.name);
    if (scheme.flags & ZEBKIT_SCHEME_FETCH)
      schemes->cors_enabled_schemes.push_back(scheme.name);
    if (scheme.flags & ZEBKIT_SCHEME_BYPASS_CSP)
      schemes->csp_bypassing_schemes.push_back(scheme.name);
    schemes->savable_schemes.push_back(scheme.name);
  }
}

void RegisterSchemesInRenderer() {
  for (const Scheme& scheme : Schemes()) {
    // What bypasses a page's Content-Security-Policy, Blink takes from
    // content's list of schemes (AddSchemes()).
    if (scheme.flags & ZEBKIT_SCHEME_FETCH) {
      blink::WebSecurityPolicy::RegisterURLSchemeAsSupportingFetchAPI(
          blink::WebString::FromLatin1(scheme.name));
    }
  }
}

void AppendSchemeSwitch(base::CommandLine* command_line) {
  std::string value;
  for (const Scheme& scheme : Schemes()) {
    if (!value.empty())
      value += ";";
    value += scheme.name;
    for (const auto& flag : kFlags) {
      if (scheme.flags & flag.flag)
        value += std::string(",") + flag.name;
    }
  }
  if (!value.empty())
    command_line->AppendSwitchASCII(kSchemeSwitch, value);
}

void AllowSchemes() {
  for (const Scheme& scheme : Schemes()) {
    content::ChildProcessSecurityPolicy::GetInstance()->RegisterWebSafeScheme(
        scheme.name);
  }
}

bool IsApplicationScheme(std::string_view name) {
  for (const Scheme& scheme : Schemes()) {
    if (scheme.name == name)
      return true;
  }
  return false;
}

mojo::PendingRemote<network::mojom::URLLoaderFactory> CreateSchemeLoaderFactory(
    const std::string& scheme) {
  mojo::PendingRemote<network::mojom::URLLoaderFactory> remote;
  base::MakeSelfDeleting<LoaderFactory>(
      scheme, remote.InitWithNewPipeAndPassReceiver());
  return remote;
}

void AddSchemeLoaderFactories(
    content::ContentBrowserClient::NonNetworkURLLoaderFactoryMap* factories) {
  for (const Scheme& scheme : Schemes())
    factories->emplace(scheme.name, CreateSchemeLoaderFactory(scheme.name));
}

void RespondToLoader(
    mojo::PendingReceiver<network::mojom::URLLoader> loader,
    mojo::PendingRemote<network::mojom::URLLoaderClient> client,
    int status,
    std::string headers,
    std::string body) {
  auto request = std::make_unique<zebkit_request>();
  request->loader = std::move(loader);
  request->client = std::move(client);
  AnswerSequence()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](std::unique_ptr<zebkit_request> request, int status,
                        std::string headers, std::string body) {
                       const int64_t length = static_cast<int64_t>(body.size());
                       Send(std::move(request),
                            MakeHead(status, headers, length), nullptr,
                            std::move(body), length);
                     },
                     std::move(request), status, std::move(headers),
                     std::move(body)));
}

}  // namespace zebkit

extern "C" {

bool zebkit_scheme_register(const char* scheme, unsigned flags) {
  return scheme && zebkit::Add(base::ToLowerASCII(scheme), flags);
}

void zebkit_scheme_handle(const char* scheme,
                          zebkit_scheme_func func,
                          void* data) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!scheme)
    return;
  zebkit::Handlers()[base::ToLowerASCII(scheme)] = {func, data};
}

const char* zebkit_request_url(zebkit_request* request) {
  return request->url.c_str();
}

const char* zebkit_request_method(zebkit_request* request) {
  return request->method.c_str();
}

const char* zebkit_request_headers(zebkit_request* request) {
  return request->headers.c_str();
}

const uint8_t* zebkit_request_body(zebkit_request* request, size_t* size) {
  if (size)
    *size = request->body.size();
  return request->has_body ? request->body.data() : nullptr;
}

void zebkit_request_respond(zebkit_request* request,
                            int status,
                            const char* headers,
                            const void* body,
                            size_t size) {
  std::string copy;
  if (body && size)
    copy.assign(static_cast<const char*>(body), size);
  const std::string lines = headers ? headers : "";
  zebkit::AnswerSequence()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](std::unique_ptr<zebkit_request> request, int status,
             std::string headers, std::string body) {
            const int64_t length = static_cast<int64_t>(body.size());
            zebkit::Send(std::move(request),
                         zebkit::MakeHead(status, headers, length), nullptr,
                         std::move(body), length);
          },
          base::WrapUnique(request), status, lines, std::move(copy)));
}

void zebkit_request_respond_file(zebkit_request* request,
                                 const char* path,
                                 const char* headers) {
  zebkit::AnswerSequence()->PostTask(
      FROM_HERE,
      base::BindOnce(&zebkit::SendFile, base::WrapUnique(request),
                     base::FilePath::FromUTF8Unsafe(path ? path : ""),
                     std::string(headers ? headers : "")));
}

}  // extern "C"
