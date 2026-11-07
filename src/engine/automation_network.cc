// zebkit's automation, the network (automation_network.h): a page's
// requests through a URL loader factory in front of the network's, as
// extensions' webRequest and DevTools' Fetch domain put one, for
// Playwright's network events and routes.

#include "zebkit/engine/automation_network.h"

#include <algorithm>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <set>
#include <vector>

#include "base/base64.h"
#include "base/byte_size.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/barrier_closure.h"
#include "base/strings/string_split.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_access_result.h"
#include "net/cookies/cookie_partition_key_collection.h"
#include "net/cookies/cookie_util.h"
#include "net/cookies/cookie_options.h"
#include "services/network/public/mojom/cookie_manager.mojom.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_user_data.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/data_pipe_drainer.h"
#include "mojo/public/cpp/system/simple_watcher.h"
#include "net/base/load_flags.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_status_code.h"
#include "net/http/http_util.h"
#include "net/url_request/redirect_info.h"
#include "net/cert/x509_certificate.h"
#include "net/ssl/ssl_cipher_suite_names.h"
#include "net/ssl/ssl_connection_status_flags.h"
#include "net/ssl/ssl_info.h"
#include "services/network/public/cpp/cors/cors.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/resource_request_body.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/cpp/url_loader_factory_builder.h"
#include "services/network/public/mojom/early_hints.mojom.h"
#include "services/network/public/mojom/fetch_api.mojom-shared.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "third_party/re2/src/re2/re2.h"
#include "url/url_constants.h"

namespace zebkit {

namespace {

using network::mojom::RequestDestination;

// Playwright's resourceType (crNetworkManager.ts names CDP's types).
const char* ResourceType(const network::ResourceRequest& request) {
  switch (request.destination) {
    case RequestDestination::kDocument:
    case RequestDestination::kIframe:
    case RequestDestination::kFrame:
      return "document";
    case RequestDestination::kStyle:
      return "stylesheet";
    case RequestDestination::kImage:
      return "image";
    case RequestDestination::kAudio:
    case RequestDestination::kVideo:
    case RequestDestination::kTrack:
      return "media";
    case RequestDestination::kFont:
      return "font";
    case RequestDestination::kScript:
    case RequestDestination::kWorker:
    case RequestDestination::kSharedWorker:
    case RequestDestination::kServiceWorker:
      return "script";
    case RequestDestination::kManifest:
      return "manifest";
    case RequestDestination::kEmpty: {
      // XMLHttpRequest, fetch() and EventSource have no destination; an
      // EventSource asks for its stream type, fetch() is fetch-like.
      std::optional<std::string> accept = request.headers.GetHeader(net::HttpRequestHeaders::kAccept);
      if (accept == "text/event-stream")
        return "eventsource";
      if (request.resource_type == 14)  // kPing
        return "ping";
      if (request.resource_type == 16)  // kCspReport
        return "cspviolationreport";
      // XMLHttpRequest and fetch() reach the network alike (both fetch-like,
      // no destination): the network cannot tell them apart, DevTools learns
      // it from Blink. Both are "fetch" here.
      return request.is_fetch_like_api || request.keepalive ? "fetch" : "xhr";
    }
    default:
      break;
  }
  // What Blink says the request is (blink.mojom.ResourceType), as DevTools'
  // types: an XMLHttpRequest, a fetch(), an EventSource, a beacon, ...
  switch (request.resource_type) {
    case 13: {  // kXhr: XMLHttpRequest, fetch() and EventSource.
      std::optional<std::string> accept = request.headers.GetHeader(net::HttpRequestHeaders::kAccept);
      if (accept == "text/event-stream")
        return "eventsource";
      return request.is_fetch_like_api ? "fetch" : "xhr";
    }
    case 14:  // kPing
      return "ping";
    case 16:  // kCspReport
      return "cspviolationreport";
    case 11:  // kPrefetch
      return "prefetch";
    case 6:   // kSubResource
    case 12:  // kFavicon
      return "other";
    default:
      return "fetch";
  }
}

// Route.abort's error codes (crNetworkManager.ts's errorReasons).
int NetError(const std::string& code) {
  static const std::map<std::string, int> kErrors = {
      {"aborted", net::ERR_ABORTED},
      {"accessdenied", net::ERR_ACCESS_DENIED},
      {"addressunreachable", net::ERR_ADDRESS_UNREACHABLE},
      {"blockedbyclient", net::ERR_BLOCKED_BY_CLIENT},
      {"blockedbyresponse", net::ERR_BLOCKED_BY_RESPONSE},
      {"connectionaborted", net::ERR_CONNECTION_ABORTED},
      {"connectionclosed", net::ERR_CONNECTION_CLOSED},
      {"connectionfailed", net::ERR_CONNECTION_FAILED},
      {"connectionrefused", net::ERR_CONNECTION_REFUSED},
      {"connectionreset", net::ERR_CONNECTION_RESET},
      {"internetdisconnected", net::ERR_INTERNET_DISCONNECTED},
      {"namenotresolved", net::ERR_NAME_NOT_RESOLVED},
      {"timedout", net::ERR_TIMED_OUT},
      {"failed", net::ERR_FAILED},
  };
  auto found = kErrors.find(code);
  return found == kErrors.end() ? net::ERR_FAILED : found->second;
}

base::ListValue RequestHeaders(const net::HttpRequestHeaders& headers) {
  base::ListValue list;
  for (const net::HttpRequestHeaders::HeaderKeyValuePair& header :
       headers.GetHeaderVector()) {
    base::DictValue entry;
    entry.Set("name", header.key);
    entry.Set("value", header.value);
    list.Append(std::move(entry));
  }
  return list;
}

base::ListValue ResponseHeaders(const net::HttpResponseHeaders* headers) {
  base::ListValue list;
  if (!headers)
    return list;
  size_t iterator = 0;
  std::string name, value;
  while (headers->EnumerateHeaderLines(&iterator, &name, &value)) {
    base::DictValue entry;
    entry.Set("name", name);
    entry.Set("value", value);
    list.Append(std::move(entry));
  }
  return list;
}

// Playwright's globToRegexPattern() (isomorphic/urlMatch.ts).
std::string GlobToRegex(const std::string& glob) {
  static const std::string kEscaped = "$^+.*()|\\?{}[]";
  std::string regex = "^";
  bool in_group = false;
  for (size_t i = 0; i < glob.size(); ++i) {
    const char c = glob[i];
    if (c == '\\' && i + 1 < glob.size()) {
      const char next = glob[++i];
      if (kEscaped.find(next) != std::string::npos)
        regex += '\\';
      regex += next;
      continue;
    }
    if (c == '*') {
      const char before = i ? glob[i - 1] : 0;
      int stars = 1;
      while (i + 1 < glob.size() && glob[i + 1] == '*') {
        ++stars;
        ++i;
      }
      if (stars > 1) {
        if (i + 1 < glob.size() && glob[i + 1] == '/') {
          regex += before == '/' ? "((.+/)|)" : "(.*/)";
          ++i;
        } else {
          regex += "(.*)";
        }
      } else {
        regex += "([^/]*)";
      }
      continue;
    }
    switch (c) {
      case '{':
        in_group = true;
        regex += '(';
        break;
      case '}':
        in_group = false;
        regex += ')';
        break;
      case ',':
        regex += in_group ? "|" : "\\,";
        break;
      default:
        if (kEscaped.find(c) != std::string::npos)
          regex += '\\';
        regex += c;
    }
  }
  return regex + "$";
}

class NetworkImpl;

constexpr size_t kBodyLimit = 10 * 1024 * 1024;
constexpr size_t kBodiesKept = 200;

// A response's body on its way to the page, kept as it passes
// (Response.body): read from the network's pipe, written to the page's.
class BodyTee : public mojo::DataPipeDrainer::Client {
 public:
  using Done = base::OnceCallback<void(std::string body, bool complete)>;

  BodyTee(mojo::ScopedDataPipeConsumerHandle from,
          mojo::ScopedDataPipeProducerHandle to,
          Done done)
      : to_(std::move(to)),
        watcher_(FROM_HERE, mojo::SimpleWatcher::ArmingPolicy::MANUAL),
        done_(std::move(done)) {
    watcher_.Watch(to_.get(), MOJO_HANDLE_SIGNAL_WRITABLE,
                   base::BindRepeating(&BodyTee::Writable,
                                       base::Unretained(this)));
    drainer_ = std::make_unique<mojo::DataPipeDrainer>(this, std::move(from));
  }

  // mojo::DataPipeDrainer::Client
  void OnDataAvailable(base::span<const uint8_t> data) override {
    if (kept_.size() + data.size() <= kBodyLimit)
      kept_.append(reinterpret_cast<const char*>(data.data()), data.size());
    else
      complete_ = false;
    pending_.emplace_back(data.begin(), data.end());
    Write();
  }
  void OnDataComplete() override {
    ended_ = true;
    Write();
  }

 private:
  void Writable(MojoResult result) {
    if (result != MOJO_RESULT_OK) {
      Finish();
      return;
    }
    Write();
  }

  void Write() {
    while (!pending_.empty()) {
      std::vector<uint8_t>& chunk = pending_.front();
      size_t written = 0;
      const MojoResult result = to_->WriteData(
          base::span(chunk).subspan(offset_), MOJO_WRITE_DATA_FLAG_NONE, written);
      if (result == MOJO_RESULT_SHOULD_WAIT) {
        watcher_.ArmOrNotify();
        return;
      }
      if (result != MOJO_RESULT_OK) {
        // The page stopped reading.
        Finish();
        return;
      }
      offset_ += written;
      if (offset_ >= chunk.size()) {
        pending_.pop_front();
        offset_ = 0;
      }
    }
    if (ended_)
      Finish();
  }

  void Finish() {
    if (!done_)
      return;
    watcher_.Cancel();
    to_.reset();
    std::move(done_).Run(std::move(kept_), complete_);
    // The drainer is the caller's: delete later.
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE, this);
  }

  mojo::ScopedDataPipeProducerHandle to_;
  mojo::SimpleWatcher watcher_;
  std::unique_ptr<mojo::DataPipeDrainer> drainer_;
  std::deque<std::vector<uint8_t>> pending_;
  size_t offset_ = 0;
  std::string kept_;
  bool complete_ = true;
  bool ended_ = false;
  Done done_;
};

// A fulfilled response's status text, as Playwright gives Fetch.fulfillRequest
// its responsePhrase (network.ts's statusText()): IANA's, "Unknown" else.
std::string FulfillStatusText(int status) {
  static constexpr struct {
    int status;
    const char* text;
  } kTexts[] = {
      {100, "Continue"},
      {101, "Switching Protocols"},
      {102, "Processing"},
      {103, "Early Hints"},
      {200, "OK"},
      {201, "Created"},
      {202, "Accepted"},
      {203, "Non-Authoritative Information"},
      {204, "No Content"},
      {205, "Reset Content"},
      {206, "Partial Content"},
      {207, "Multi-Status"},
      {208, "Already Reported"},
      {226, "IM Used"},
      {300, "Multiple Choices"},
      {301, "Moved Permanently"},
      {302, "Found"},
      {303, "See Other"},
      {304, "Not Modified"},
      {305, "Use Proxy"},
      {306, "Switch Proxy"},
      {307, "Temporary Redirect"},
      {308, "Permanent Redirect"},
      {400, "Bad Request"},
      {401, "Unauthorized"},
      {402, "Payment Required"},
      {403, "Forbidden"},
      {404, "Not Found"},
      {405, "Method Not Allowed"},
      {406, "Not Acceptable"},
      {407, "Proxy Authentication Required"},
      {408, "Request Timeout"},
      {409, "Conflict"},
      {410, "Gone"},
      {411, "Length Required"},
      {412, "Precondition Failed"},
      {413, "Payload Too Large"},
      {414, "URI Too Long"},
      {415, "Unsupported Media Type"},
      {416, "Range Not Satisfiable"},
      {417, "Expectation Failed"},
      {418, "I'm a teapot"},
      {421, "Misdirected Request"},
      {422, "Unprocessable Entity"},
      {423, "Locked"},
      {424, "Failed Dependency"},
      {425, "Too Early"},
      {426, "Upgrade Required"},
      {428, "Precondition Required"},
      {429, "Too Many Requests"},
      {431, "Request Header Fields Too Large"},
      {451, "Unavailable For Legal Reasons"},
      {500, "Internal Server Error"},
      {501, "Not Implemented"},
      {502, "Bad Gateway"},
      {503, "Service Unavailable"},
      {504, "Gateway Timeout"},
      {505, "HTTP Version Not Supported"},
      {506, "Variant Also Negotiates"},
      {507, "Insufficient Storage"},
      {508, "Loop Detected"},
      {510, "Not Extended"},
      {511, "Network Authentication Required"},
  };
  for (const auto& entry : kTexts) {
    if (entry.status == status)
      return entry.text;
  }
  return "Unknown";
}

// One request of the page, between the page's loader and the network's.
class Loader : public network::mojom::URLLoader,
               public network::mojom::URLLoaderClient {
 public:
  Loader(base::WeakPtr<NetworkImpl> network,
         std::string id,
         std::string frame,
         bool navigation,
         mojo::PendingReceiver<network::mojom::URLLoader> loader,
         int32_t request_id,
         uint32_t options,
         const network::ResourceRequest& request,
         mojo::PendingRemote<network::mojom::URLLoaderClient> client,
         const net::MutableNetworkTrafficAnnotationTag& annotation,
         network::mojom::URLLoaderFactory* target)
      : network_(std::move(network)),
        id_(std::move(id)),
        frame_(std::move(frame)),
        navigation_(navigation),
        request_id_(request_id),
        options_(options),
        request_(request),
        annotation_(annotation),
        target_(target),
        loader_receiver_(this, std::move(loader)),
        client_(std::move(client)) {
    request_.enable_load_timing = true;
    loader_receiver_.set_disconnect_handler(
        base::BindOnce(&Loader::Gone, base::Unretained(this)));
  }

  void Start();
  // Its next hop after a fulfilled redirect: routed, or on to the network.
  void Reroute();

  const GURL& url() const { return request_.url; }

  void Continue(const base::DictValue& overrides) {
    const bool overridden = overrides.Find("url") || overrides.Find("method") ||
                            overrides.Find("headers") || overrides.Find("postData");
    if (const std::string* url = overrides.FindString("url")) {
      GURL changed(*url);
      if (changed.is_valid() && changed != request_.url) {
        // A navigation's response is looked for by the URL it was asked for.
        if (original_url_.empty())
          original_url_ = request_.url.spec();
        request_.url = changed;
      }
    }
    if (const std::string* method = overrides.FindString("method"))
      request_.method = *method;
    if (const base::ListValue* headers = overrides.FindList("headers")) {
      request_.headers.Clear();
      for (const base::Value& header : *headers) {
        const std::string* name = header.is_dict() ? header.GetDict().FindString("name") : nullptr;
        const std::string* value = header.is_dict() ? header.GetDict().FindString("value") : nullptr;
        // Not those of the connection and the cookie store (Host, Trailer,
        // Cookie, ...), which the network sets itself: what DevTools' Fetch
        // gives Chrome's network service keeps its own.
        static constexpr std::string_view kNetworks[] = {
            "host", "trailer", "cookie", "cookie2", "connection", "keep-alive", "te",
            "transfer-encoding", "upgrade", "expect", "content-length"};
        if (name && value &&
            std::find(std::begin(kNetworks), std::end(kNetworks), base::ToLowerASCII(*name)) ==
                std::end(kNetworks)) {
          request_.headers.SetHeader(*name, *value);
        }
      }
    }
    if (const std::string* post = overrides.FindString("postData")) {
      std::string bytes;
      if (!base::Base64Decode(*post, &bytes))
        bytes = *post;
      request_.request_body = network::ResourceRequestBody::CreateFromCopyOfBytes(
          base::as_byte_span(bytes));
    }
    if (overridden)
      ContinuedWith();
    if (!target_) {
      Complete(net::ERR_FAILED);
      return;
    }
    target_->CreateLoaderAndStart(
        target_loader_.BindNewPipeAndPassReceiver(), request_id_, options_,
        request_, client_receiver_.BindNewPipeAndPassRemote(), annotation_);
    client_receiver_.set_disconnect_handler(
        base::BindOnce(&Loader::NetworkGone, base::Unretained(this)));
  }

  // A response of the client's: Route.fulfill.
  // Route.fulfill's Set-Cookie go into the profile's cookies first, as the
  // network stores those of what it fetches (Chromium's interception answers
  // below the cookie store): a document reads them at once.
  void Fulfill(const base::DictValue& response) {
    std::vector<std::string> cookies;
    if (const base::ListValue* headers = response.FindList("headers")) {
      for (const base::Value& header : *headers) {
        const std::string* name = header.is_dict() ? header.GetDict().FindString("name") : nullptr;
        const std::string* value = header.is_dict() ? header.GetDict().FindString("value") : nullptr;
        if (name && value && base::EqualsCaseInsensitiveASCII(*name, "set-cookie")) {
          for (const std::string& line : base::SplitString(*value, "\n", base::TRIM_WHITESPACE,
                                                            base::SPLIT_WANT_NONEMPTY)) {
            cookies.push_back(line);
          }
        }
      }
    }
    if (cookies.empty() || !network_) {
      FulfillNow(response);
      return;
    }
    StoreCookies(std::move(cookies),
                 base::BindOnce([](base::WeakPtr<Loader> loader, base::DictValue response) {
                   if (loader)
                     loader->FulfillNow(response);
                 }, GetWeakPtr(), response.Clone()));
  }
  void StoreCookies(std::vector<std::string> lines, base::OnceClosure then);
  // The cookies the network will send with it, for a route's request, as
  // DevTools' interception has them (InterceptionJob::FetchCookies()).
  void FetchCookies(base::OnceClosure then);
  void EmitRoute(const std::string& route);

  void FulfillNow(const base::DictValue& response) {
    const int status = response.FindInt("status").value_or(200);
    std::string raw = "HTTP/1.1 " + base::NumberToString(status) + " " +
                      FulfillStatusText(status) + "\r\n";
    std::string mime_type = "text/plain";
    if (const base::ListValue* headers = response.FindList("headers")) {
      for (const base::Value& header : *headers) {
        const std::string* name = header.is_dict() ? header.GetDict().FindString("name") : nullptr;
        const std::string* value = header.is_dict() ? header.GetDict().FindString("value") : nullptr;
        if (!name || !value)
          continue;
        raw += *name + ": " + *value + "\r\n";
        if (base::EqualsCaseInsensitiveASCII(*name, "content-type"))
          mime_type = *value;
      }
    }
    // network.ts's _maybeAddCorsHeaders(): a cross-origin request's fulfilled
    // response allows its origin unless it says otherwise. The Origin header
    // is the network service's to add, from the initiator.
    {
      std::string origin = request_.headers.GetHeader("Origin").value_or(std::string());
      if (origin.empty() && request_.request_initiator &&
          network::cors::ShouldCheckCors(request_.url, request_.request_initiator, request_.mode)) {
        origin = request_.request_initiator->Serialize();
      }
      bool has_cors = false;
      if (const base::ListValue* headers = response.FindList("headers")) {
        for (const base::Value& header : *headers) {
          const std::string* name = header.is_dict() ? header.GetDict().FindString("name") : nullptr;
          if (name && base::EqualsCaseInsensitiveASCII(*name, "access-control-allow-origin"))
            has_cors = true;
        }
      }
      if (!origin.empty() && request_.url.SchemeIsHTTPOrHTTPS() && !has_cors &&
          url::Origin::Create(request_.url).Serialize() != base::TrimWhitespaceASCII(origin, base::TRIM_ALL)) {
        raw += "access-control-allow-origin: " + origin + "\r\n";
        raw += "access-control-allow-credentials: true\r\n";
        raw += "vary: Origin\r\n";
      }
    }
    std::string body;
    if (const std::string* given = response.FindString("body")) {
      if (!response.FindBool("isBase64").value_or(false) ||
          !base::Base64Decode(*given, &body)) {
        body = *given;
      }
    }
    auto head = network::mojom::URLResponseHead::New();
    head->headers = base::MakeRefCounted<net::HttpResponseHeaders>(
        net::HttpUtil::AssembleRawHeaders(raw));
    if (std::string location; head->headers->IsRedirect(&location)) {
      const GURL new_url = request_.url.Resolve(location);
      if (new_url.is_valid()) {
        const net::RedirectInfo redirect = net::RedirectInfo::ComputeRedirectInfo(
            request_.method, request_.url, request_.site_for_cookies,
            request_.update_first_party_url_on_redirect
                ? net::RedirectInfo::FirstPartyURLPolicy::UPDATE_URL_ON_REDIRECT
                : net::RedirectInfo::FirstPartyURLPolicy::NEVER_CHANGE_URL,
            request_.referrer_policy, request_.referrer.spec(), request_.request_initiator, status,
            new_url, std::nullopt, false);
        if (redirect.new_method != request_.method)
          request_.request_body = nullptr;
        request_.site_for_cookies = redirect.new_site_for_cookies;
        request_.referrer = GURL(redirect.new_referrer);
        request_.referrer_policy = redirect.new_referrer_policy;
        head->mime_type = mime_type;
        fulfilled_redirect_ = true;
        OnReceiveRedirect(redirect, std::move(head));
        return;
      }
    }
    std::string charset;
    head->headers->GetMimeTypeAndCharset(&head->mime_type, &charset);
    if (head->mime_type.empty())
      head->mime_type = mime_type;
    head->charset = charset;
    head->content_length = static_cast<int64_t>(body.size());
    mojo::ScopedDataPipeProducerHandle producer;
    mojo::ScopedDataPipeConsumerHandle consumer;
    if (mojo::CreateDataPipe(std::max<uint32_t>(1, body.size()), producer,
                             consumer) != MOJO_RESULT_OK) {
      Complete(net::ERR_INSUFFICIENT_RESOURCES);
      return;
    }
    size_t written = 0;
    if (!body.empty()) {
      producer->WriteData(base::as_byte_span(body), MOJO_WRITE_DATA_FLAG_ALL_OR_NONE,
                          written);
    }
    producer.reset();
    // A cross-origin request's CORS check, which the network service makes
    // of what it fetches and a fulfilled response would skip: Chromium's
    // interception answers below it (fetch.spec.whatwg.org's CORS check).
    if (request_.request_initiator &&
        network::cors::ShouldCheckCors(request_.url, request_.request_initiator, request_.mode)) {
      auto checked = network::cors::CheckAccess(
          request_.url, head->headers->GetNormalizedHeader("access-control-allow-origin"),
          head->headers->GetNormalizedHeader("access-control-allow-credentials"),
          request_.credentials_mode, *request_.request_initiator);
      if (!checked.has_value()) {
        OnCompleteInternal(network::URLLoaderCompletionStatus(checked.error()));
        return;
      }
    }
    // Response.body of a fulfilled request is what was given, as Playwright
    // keeps it (network.ts's Route.fulfill).
    KeepFulfilled(body);
    Responded(*head);
    client_->OnReceiveResponse(std::move(head), std::move(consumer),
                               std::nullopt);
    network::URLLoaderCompletionStatus done(net::OK);
    done.decoded_body_length = base::ByteSize(body.size());
    done.encoded_body_length = base::ByteSize(body.size());
    done.encoded_data_length = base::ByteSize(body.size() + response_headers_size_);
    OnCompleteInternal(done);
  }

  void Abort(int error) { Complete(error); }
  const std::string& request_id() const { return id_; }
  void KeepFulfilled(std::string body);
  // Page.requestContinued: what route.continue() changed, for a HAR's
  // recordRequestOverrides.
  void ContinuedWith();

  // network::mojom::URLLoader
  void FollowRedirect(network::HttpRequestHeadersUpdateParams params,
                      const std::optional<GURL>& new_url) override {
    // A redirect the client fulfilled: its next hop goes through routing
    // again, as the redirect DevTools' Fetch.fulfillRequest issues does.
    if (fulfilled_redirect_) {
      fulfilled_redirect_ = false;
      for (const std::string& name : params.removed_headers)
        request_.headers.RemoveHeader(name);
      request_.headers.MergeFrom(params.modified_headers);
      if (new_url)
        request_.url = *new_url;
      target_loader_.reset();
      client_receiver_.reset();
      Reroute();
      return;
    }
    if (target_loader_)
      target_loader_->FollowRedirect(std::move(params), new_url);
  }
  void SetPriority(net::RequestPriority priority, int32_t value) override {
    if (target_loader_)
      target_loader_->SetPriority(priority, value);
  }

  // network::mojom::URLLoaderClient
  void OnReceiveEarlyHints(network::mojom::EarlyHintsPtr hints) override {
    client_->OnReceiveEarlyHints(std::move(hints));
  }
  void OnReceiveResponse(network::mojom::URLResponseHeadPtr head,
                         mojo::ScopedDataPipeConsumerHandle body,
                         std::optional<mojo_base::BigBuffer> metadata) override;
  void OnReceiveRedirect(const net::RedirectInfo& redirect,
                         network::mojom::URLResponseHeadPtr head) override;
  void OnUploadProgress(int64_t position,
                        int64_t total,
                        OnUploadProgressCallback callback) override {
    client_->OnUploadProgress(position, total, std::move(callback));
  }
  void OnTransferSizeUpdated(int32_t diff) override {
    client_->OnTransferSizeUpdated(diff);
  }
  void OnComplete(const network::URLLoaderCompletionStatus& status) override {
    OnCompleteInternal(status);
  }

  base::WeakPtr<Loader> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

 private:
  base::DictValue RequestValue() {
    base::DictValue value;
    value.Set("request", id_);
    // Without its fragment, as network.ts's stripFragmentFromUrl().
    value.Set("url", request_.url.GetWithoutRef().spec());
    value.Set("method", request_.method);
    base::ListValue headers = RequestHeaders(request_.headers);
    // The Origin the network service adds (cors_url_loader.cc), which
    // DevTools' interception sees: a CORS request to another origin, or one
    // that is not GET or HEAD.
    const bool cors = request_.mode == network::mojom::RequestMode::kCors ||
                      request_.mode == network::mojom::RequestMode::kCorsWithForcedPreflight;
    if (request_.request_initiator && !request_.headers.HasHeader("Origin") &&
        request_.mode != network::mojom::RequestMode::kNavigate &&
        ((cors && !request_.request_initiator->IsSameOriginWith(request_.url)) ||
         (request_.method != "GET" && request_.method != "HEAD"))) {
      base::DictValue origin;
      origin.Set("name", "Origin");
      origin.Set("value", request_.request_initiator->Serialize());
      headers.Append(std::move(origin));
    }
    if (!cookie_.empty() && !request_.headers.HasHeader("Cookie")) {
      base::DictValue cookie;
      cookie.Set("name", "Cookie");
      cookie.Set("value", cookie_);
      headers.Append(std::move(cookie));
    }
    // And the Referer it sends (the policy applied already, by Blink).
    if (request_.referrer.is_valid() && !request_.headers.HasHeader("Referer")) {
      base::DictValue referer;
      referer.Set("name", "Referer");
      referer.Set("value", request_.referrer.spec());
      headers.Append(std::move(referer));
    }
    value.Set("headers", std::move(headers));
    // The type DevTools has from Blink (XHR and fetch(), which the network
    // cannot tell apart), else the network's.
    value.Set("resourceType", devtools_type_.empty() ? std::string(ResourceType(request_)) : devtools_type_);
    value.Set("isNavigationRequest", navigation_);
    value.Set("frame", frame_);
    if (!redirected_from_.empty())
      value.Set("redirectedFrom", redirected_from_);
    // The body the page gave, as base64 (the protocol's binary).
    if (request_.request_body) {
      std::string body;
      for (const auto& element : *request_.request_body->elements()) {
        if (element.type() != network::DataElement::Tag::kBytes)
          continue;
        const auto bytes = element.As<network::DataElementBytes>().bytes();
        body.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
      }
      if (!body.empty())
        value.Set("postData", base::Base64Encode(body));
    }
    return value;
  }

  void Responded(const network::mojom::URLResponseHead& head);
  void OnCompleteInternal(const network::URLLoaderCompletionStatus& status);

  void Complete(int error) {
    OnCompleteInternal(network::URLLoaderCompletionStatus(error));
  }

  // The page's end went: the request is cancelled, which Playwright reports
  // as failed (net::ERR_ABORTED).
  void Gone();
  void NetworkGone() {
    if (!completed_)
      Complete(net::ERR_ABORTED);
  }

  base::WeakPtr<NetworkImpl> network_;
  std::string id_;
  // The URL asked for, when route.continue() changed it.
  std::string original_url_;
  // The page's DevTools session's type for it, lower case, if it said one.
  std::string devtools_type_;
  std::string frame_;
  // FetchCookies()'s Cookie header, reported, never sent from here.
  std::string cookie_;
  bool navigation_;
  int32_t request_id_;
  uint32_t options_;
  network::ResourceRequest request_;
  net::MutableNetworkTrafficAnnotationTag annotation_;
  raw_ptr<network::mojom::URLLoaderFactory> target_;
  std::string redirected_from_;
  bool completed_ = false;
  // A redirect of Route.fulfill's, which the client follows (FollowRedirect).
  bool fulfilled_redirect_ = false;
  base::TimeTicks timing_start_ = base::TimeTicks::Now();
  base::Time wall_start_ = base::Time::Now();
  int64_t response_headers_size_ = 0;
  mojo::Receiver<network::mojom::URLLoader> loader_receiver_;
  mojo::Remote<network::mojom::URLLoaderClient> client_;
  mojo::Remote<network::mojom::URLLoader> target_loader_;
  mojo::Remote<network::mojom::URLLoaderFactory> held_target_;
  mojo::Receiver<network::mojom::URLLoaderClient> client_receiver_{this};
  base::WeakPtrFactory<Loader> weak_factory_{this};
};

// The factory in front of the network's for one of the page's factories.
class ProxyFactory : public network::mojom::URLLoaderFactory {
 public:
  ProxyFactory(base::WeakPtr<NetworkImpl> network,
               std::string frame,
               mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
               mojo::PendingRemote<network::mojom::URLLoaderFactory> target)
      : network_(std::move(network)),
        frame_(std::move(frame)),
        target_(std::move(target)) {
    receivers_.Add(this, std::move(receiver));
    receivers_.set_disconnect_handler(base::BindRepeating(
        &ProxyFactory::Disconnected, base::Unretained(this)));
    target_.set_disconnect_handler(
        base::BindOnce(&ProxyFactory::Gone, base::Unretained(this)));
  }

  void CreateLoaderAndStart(
      mojo::PendingReceiver<network::mojom::URLLoader> loader,
      int32_t request_id,
      uint32_t options,
      const network::ResourceRequest& request,
      mojo::PendingRemote<network::mojom::URLLoaderClient> client,
      const net::MutableNetworkTrafficAnnotationTag& annotation) override;

  void Clone(mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver)
      override {
    receivers_.Add(this, std::move(receiver));
  }

 private:
  void Disconnected() {
    if (receivers_.empty())
      Gone();
  }
  void Gone() { delete this; }

  base::WeakPtr<NetworkImpl> network_;
  std::string frame_;
  mojo::Remote<network::mojom::URLLoaderFactory> target_;
  mojo::ReceiverSet<network::mojom::URLLoaderFactory> receivers_;
};

class NetworkImpl : public Network,
                    public content::WebContentsUserData<NetworkImpl> {
 public:
  ~NetworkImpl() override = default;

  void Emit(const std::string& event, base::DictValue params) {
    if (emit_)
      emit_.Run(event, std::move(params));
  }

  std::string NextId(const char* prefix) {
    return prefix + base::NumberToString(++next_id_);
  }

  bool Intercepts(const GURL& url) const {
    for (const auto* set : {&patterns_, &context_patterns_}) {
      for (const auto& pattern : *set) {
        if (RE2::PartialMatch(url.spec(), *pattern))
          return true;
      }
    }
    return false;
  }

  // Whether the page or its context routes anything (crNetworkManager.ts
  // disables the cache then: Network.setCacheDisabled).
  bool Routes() const { return !patterns_.empty() || !context_patterns_.empty(); }

  void Hold(const std::string& route, base::WeakPtr<Loader> loader) {
    routes_[route] = std::move(loader);
  }

  void AddExtraHeaders(network::ResourceRequest& request) const {
    for (const auto& [name, value] : extra_headers_)
      request.headers.SetHeader(name, value);
  }

  void SetEmit(EventFunc emit) { emit_ = std::move(emit); }

  base::WeakPtr<NetworkImpl> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

  // Network
  bool SetPatterns(const base::ListValue& patterns, bool context, std::string* error) override {
    return Compile(patterns, context ? &context_patterns_ : &patterns_, error);
  }

  bool SetWebSocketPatterns(const base::ListValue& patterns,
                            std::string* error) override {
    return Compile(patterns, &web_socket_patterns_, error);
  }

  bool RoutesWebSocket(const std::string& url) const override {
    for (const auto& pattern : web_socket_patterns_) {
      if (RE2::PartialMatch(url, *pattern))
        return true;
    }
    return false;
  }

  static bool Compile(const base::ListValue& patterns,
                      std::vector<std::unique_ptr<RE2>>* out,
                      std::string* error) {
    std::vector<std::unique_ptr<RE2>> compiled;
    for (const base::Value& pattern : patterns) {
      if (!pattern.is_dict())
        continue;
      std::string source;
      RE2::Options options;
      options.set_log_errors(false);
      if (const std::string* glob = pattern.GetDict().FindString("glob")) {
        source = GlobToRegex(*glob);
      } else if (const std::string* regex =
                     pattern.GetDict().FindString("regexSource")) {
        source = *regex;
        const std::string* flags = pattern.GetDict().FindString("regexFlags");
        options.set_case_sensitive(!flags || flags->find('i') == std::string::npos);
      } else if (pattern.GetDict().FindDict("urlPattern")) {
        // urlMatch.ts's deserializeURLPattern() where the server has no
        // URLPattern: every URL, which the client matches again.
        source = "";
      } else {
        continue;
      }
      auto re = std::make_unique<RE2>(source, options);
      if (!re->ok()) {
        *error = "Invalid URL pattern: " + source;
        return false;
      }
      compiled.push_back(std::move(re));
    }
    *out = std::move(compiled);
    return true;
  }

  void SetOffline(bool offline) override { offline_ = offline; }
  void SetBlankDocuments(bool blank) override { blank_documents_ = blank; }
  bool blank_documents() const { return blank_documents_; }
  bool blank_documents_ = false;

  void KeepNavigationResponse(const std::string& url, const base::DictValue& response) {
    navigation_responses_.insert_or_assign(url, response.Clone());
    navigation_order_.push_back(url);
    while (navigation_order_.size() > kBodiesKept) {
      // The oldest record of a URL, unless it has been kept again since.
      if (std::count(navigation_order_.begin(), navigation_order_.end(), navigation_order_.front()) == 1)
        navigation_responses_.erase(navigation_order_.front());
      navigation_order_.pop_front();
    }
  }
  std::optional<base::DictValue> NavigationResponse(const std::string& url) override {
    auto found = navigation_responses_.find(url);
    return found == navigation_responses_.end() ? std::nullopt : std::make_optional(found->second.Clone());
  }

  void KeepBody(const std::string& request, std::string body) {
    bodies_[request] = std::move(body);
    order_.push_back(request);
    while (order_.size() > kBodiesKept) {
      bodies_.erase(order_.front());
      order_.pop_front();
    }
  }

  void BodyStarted(const std::string& request) { bodies_coming_.insert(request); }
  void BodyEnded(const std::string& request, std::optional<std::string> body) {
    bodies_coming_.erase(request);
    if (body)
      KeepBody(request, *body);
    auto waiters = body_waiters_.extract(request);
    if (waiters.empty())
      return;
    for (BodyFunc& done : waiters.mapped())
      std::move(done).Run(body);
  }
  void WhenBody(const std::string& request, BodyFunc done) override {
    if (std::optional<std::string> body = Body(request)) {
      std::move(done).Run(std::move(body));
      return;
    }
    if (!bodies_coming_.contains(request)) {
      std::move(done).Run(std::nullopt);
      return;
    }
    body_waiters_[request].push_back(std::move(done));
  }

  std::optional<std::string> Body(const std::string& request) override {
    auto found = bodies_.find(request);
    if (found == bodies_.end())
      return std::nullopt;
    return found->second;
  }
  bool offline() const { return offline_; }

  void SetExtraHeaders(const base::ListValue& headers) override {
    extra_headers_.clear();
    for (const base::Value& header : headers) {
      const std::string* name = header.is_dict() ? header.GetDict().FindString("name") : nullptr;
      const std::string* value = header.is_dict() ? header.GetDict().FindString("value") : nullptr;
      if (name && value)
        extra_headers_.emplace_back(*name, *value);
    }
  }

  bool Continue(const std::string& route,
                const base::DictValue& overrides) override {
    bool gone = false;
    base::WeakPtr<Loader> loader = Take(route, &gone);
    if (!loader)
      return gone;
    // harTracer.ts's _wasContinued (route.continue(), not a fallback's).
    if (!overrides.FindBool("isFallback").value_or(false)) {
      base::DictValue route_event;
      route_event.Set("request", loader->request_id());
      Emit("Page.requestRouteContinued", std::move(route_event));
    }
    loader->Continue(overrides);
    return true;
  }

  bool Fulfill(const std::string& route,
               const base::DictValue& response) override {
    bool gone = false;
    base::WeakPtr<Loader> loader = Take(route, &gone);
    if (!loader)
      return gone;
    // harTracer.ts's _wasFulfilled.
    {
      base::DictValue route_event;
      route_event.Set("request", loader->request_id());
      Emit("Page.requestRouteFulfilled", std::move(route_event));
    }
    loader->Fulfill(response);
    return true;
  }

  void DevToolsType(const std::string& devtools_id, const std::string& type) override {
    devtools_types_[devtools_id] = type;
    // Told after the request started (DevTools is not always first): its type
    // for those that record it (a HAR's _resourceType).
    if (auto chain = devtools_chains_.find(devtools_id);
        chain != devtools_chains_.end() && !chain->second.hops.empty()) {
      const std::string lower = base::ToLowerASCII(type);
      if (lower != "other" && lower != "document") {
        base::DictValue params;
        params.Set("request", chain->second.hops.back());
        params.Set("resourceType", lower);
        Emit("Page.requestResourceType", std::move(params));
      }
    }
    if (devtools_types_.size() > 2000)
      devtools_types_.erase(devtools_types_.begin());
  }
  std::string DevToolsTypeOf(const std::string& devtools_id) const {
    auto found = devtools_types_.find(devtools_id);
    return found == devtools_types_.end() ? std::string() : found->second;
  }
  // A request's DevTools id known, or the next request of its redirects:
  // DevTools tells each hop's headers in order, under the same id.
  void DevToolsRequestStarted(const std::string& devtools_id, const std::string& request) {
    if (devtools_chains_.size() > 2000 && !devtools_chains_.contains(devtools_id))
      devtools_chains_.erase(devtools_chains_.begin());
    DevToolsChain& chain = devtools_chains_[devtools_id];
    chain.hops.push_back(request);
    FlushExtraInfo(chain);
  }
  void DevToolsExtraInfo(const std::string& kind,
                         const std::string& devtools_id,
                         base::ListValue headers) override {
    if (devtools_chains_.size() > 2000 && !devtools_chains_.contains(devtools_id))
      devtools_chains_.erase(devtools_chains_.begin());
    DevToolsChain& chain = devtools_chains_[devtools_id];
    if (chain.pending.size() < 64)
      chain.pending.emplace_back(kind, std::move(headers));
    FlushExtraInfo(chain);
  }
  bool SawDevToolsRequest(const std::string& devtools_id) const override {
    auto found = devtools_chains_.find(devtools_id);
    return found != devtools_chains_.end() && !found->second.hops.empty();
  }
  void ServedByWorker(const std::string& devtools_id,
                      base::DictValue request,
                      base::DictValue response) override {
    if (SawDevToolsRequest(devtools_id) || worker_served_.contains(devtools_id))
      return;
    const std::string id = NextId("q");
    worker_served_[devtools_id] = id;
    if (worker_served_.size() > 2000)
      worker_served_.erase(worker_served_.begin());
    request.Set("request", id);
    const bool navigation = request.FindBool("isNavigationRequest").value_or(false);
    Emit("Page.request", std::move(request));
    response.Set("request", id);
    // Unless the memory cache served it.
    if (!response.Find("fromServiceWorker"))
      response.Set("fromServiceWorker", true);
    // A navigation's, which goto() answers with.
    if (const std::string* url = response.FindString("url"); navigation && url)
      KeepNavigationResponse(*url, response);
    Emit("Page.response", std::move(response));
    BodyStarted(id);
  }
  void ServedByWorkerEnded(const std::string& devtools_id,
                           std::optional<std::string> body,
                           const std::string& failure) override {
    auto found = worker_served_.find(devtools_id);
    if (found == worker_served_.end())
      return;
    const std::string id = found->second;
    worker_served_.erase(found);
    BodyEnded(id, std::move(body));
    base::DictValue params;
    params.Set("request", id);
    if (!failure.empty()) {
      params.Set("failureText", failure);
      Emit("Page.requestFailed", std::move(params));
    } else {
      params.Set("responseEndTiming", -1);
      Emit("Page.requestFinished", std::move(params));
    }
  }
  void ServedByWorkerFailed(const std::string& devtools_id,
                            base::DictValue request,
                            const std::string& failure) override {
    if (SawDevToolsRequest(devtools_id) || worker_served_.contains(devtools_id))
      return;
    const std::string id = NextId("q");
    request.Set("request", id);
    Emit("Page.request", std::move(request));
    base::DictValue params;
    params.Set("request", id);
    params.Set("failureText", failure);
    Emit("Page.requestFailed", std::move(params));
  }
  std::map<std::string, std::string> worker_served_;
  struct DevToolsChain {
    std::vector<std::string> hops;
    size_t requests = 0;
    size_t responses = 0;
    std::vector<std::pair<std::string, base::ListValue>> pending;
  };
  void FlushExtraInfo(DevToolsChain& chain) {
    for (auto it = chain.pending.begin(); it != chain.pending.end();) {
      size_t& next = it->first == "request" ? chain.requests : chain.responses;
      if (next >= chain.hops.size()) {
        ++it;
        continue;
      }
      EmitExtraInfo(it->first, chain.hops[next++], std::move(it->second));
      it = chain.pending.erase(it);
    }
  }
  void EmitExtraInfo(const std::string& kind, const std::string& request, base::ListValue headers) {
    base::DictValue params;
    params.Set("request", request);
    params.Set("headers", std::move(headers));
    Emit(kind == "request" ? "Page.requestExtraInfo" : "Page.responseExtraInfo", std::move(params));
  }

  std::optional<std::string> RouteUrl(const std::string& route) override {
    auto found = routes_.find(route);
    if (found == routes_.end() || !found->second)
      return std::nullopt;
    return found->second->url().spec();
  }

  bool Abort(const std::string& route, const std::string& error) override {
    bool gone = false;
    base::WeakPtr<Loader> loader = Take(route, &gone);
    if (!loader)
      return gone;
    // harTracer.ts's _wasAborted.
    {
      base::DictValue route_event;
      route_event.Set("request", loader->request_id());
      Emit("Page.requestRouteAborted", std::move(route_event));
    }
    loader->Abort(NetError(error));
    return true;
  }

 private:
  explicit NetworkImpl(content::WebContents* contents)
      : content::WebContentsUserData<NetworkImpl>(*contents) {}
  friend class content::WebContentsUserData<NetworkImpl>;
  WEB_CONTENTS_USER_DATA_KEY_DECL();

  // The held request of @route; @gone when it was held and its page has
  // cancelled it since (the route is handled without error, as Playwright's).
  base::WeakPtr<Loader> Take(const std::string& route, bool* gone) {
    auto found = routes_.find(route);
    *gone = false;
    if (found == routes_.end())
      return nullptr;
    base::WeakPtr<Loader> loader = found->second;
    routes_.erase(found);
    *gone = !loader;
    return loader;
  }

  EventFunc emit_;
  std::vector<std::unique_ptr<RE2>> patterns_;
  std::vector<std::unique_ptr<RE2>> context_patterns_;
  std::vector<std::unique_ptr<RE2>> web_socket_patterns_;
  std::vector<std::pair<std::string, std::string>> extra_headers_;
  std::map<std::string, base::WeakPtr<Loader>> routes_;
  int next_id_ = 0;
  bool offline_ = false;
  std::map<std::string, std::string> bodies_;
  // DevTools' ids of the page's requests: their types, their requests, and
  // the extra info that came before the request.
  std::map<std::string, std::string> devtools_types_;
  std::map<std::string, DevToolsChain> devtools_chains_;
  // Bodies on their way, and Response.body calls waiting for them.
  std::set<std::string> bodies_coming_;
  std::map<std::string, std::vector<BodyFunc>> body_waiters_;
  std::deque<std::string> navigation_order_;
  std::map<std::string, base::DictValue> navigation_responses_;
  std::deque<std::string> order_;
  base::WeakPtrFactory<NetworkImpl> weak_factory_{this};
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(NetworkImpl);

void Loader::StoreCookies(std::vector<std::string> lines, base::OnceClosure then) {
  content::WebContents* contents = network_ ? &network_->GetWebContents() : nullptr;
  network::mojom::CookieManager* manager =
      contents ? contents->GetBrowserContext()->GetDefaultStoragePartition()->GetCookieManagerForBrowserProcess()
               : nullptr;
  if (!manager) {
    std::move(then).Run();
    return;
  }
  auto done = base::BarrierClosure(lines.size(), std::move(then));
  for (const std::string& line : lines) {
    std::unique_ptr<net::CanonicalCookie> cookie = net::CanonicalCookie::Create(
        request_.url, line, base::Time::Now(), std::nullopt, std::nullopt, net::CookieSourceType::kHTTP,
        nullptr);
    if (!cookie) {
      done.Run();
      continue;
    }
    manager->SetCanonicalCookie(*cookie, request_.url, net::CookieOptions::MakeAllInclusive(),
                                base::BindOnce([](base::RepeatingClosure done, net::CookieAccessResult) { done.Run(); },
                                               done));
  }
}

void Loader::Start() {
  // A data: URL is no request, as Playwright reports none for it
  // (crNetworkManager.ts).
  if (request_.url.SchemeIs(url::kDataScheme))
    network_ = nullptr;
  // A load from the cache alone (a font's first try, say), which goes to
  // the network as a load of its own if the cache has nothing: Chrome's
  // DevTools reports the one that loads.
  if (request_.load_flags & net::LOAD_ONLY_FROM_CACHE)
    network_ = nullptr;
  if (!network_) {
    Continue({});
    return;
  }
  network_->AddExtraHeaders(request_);
  // Routed: no disk cache, as crNetworkManager.ts has it
  // (Network.setCacheDisabled).
  if (network_->Routes())
    request_.load_flags |= net::LOAD_BYPASS_CACHE;
  if (request_.devtools_request_id) {
    network_->DevToolsRequestStarted(*request_.devtools_request_id, id_);
    devtools_type_ = base::ToLowerASCII(network_->DevToolsTypeOf(*request_.devtools_request_id));
    // DevTools says "other" where the network knows better (a document).
    if (devtools_type_ == "other" || devtools_type_ == "document")
      devtools_type_.clear();
  }
  network_->Emit("Page.request", RequestValue());
  if (network_->blank_documents()) {
    base::DictValue blank;
    blank.Set("status", 200);
    base::ListValue headers;
    base::DictValue type;
    type.Set("name", "content-type");
    type.Set("value", "text/html");
    headers.Append(std::move(type));
    blank.Set("headers", std::move(headers));
    blank.Set("body", "<html></html>");
    Fulfill(blank);
    return;
  }
  // Offline, the network fails it, routed or not: a route answers first, as
  // Fetch's interception comes before Network.emulateNetworkConditions.
  if (!network_->Intercepts(request_.url)) {
    if (network_->offline()) {
      Complete(net::ERR_INTERNET_DISCONNECTED);
      return;
    }
    Continue({});
    return;
  }
  // Held for the client: Route.continue, fulfill or abort, with a factory
  // of its own, which outlives the page's.
  target_->Clone(held_target_.BindNewPipeAndPassReceiver());
  target_ = held_target_.get();
  const std::string route = network_->NextId("r");
  network_->Hold(route, GetWeakPtr());
  FetchCookies(base::BindOnce(&Loader::EmitRoute, GetWeakPtr(), route));
}


void Loader::EmitRoute(const std::string& route) {
  if (!network_)
    return;
  base::DictValue params;
  params.Set("route", route);
  params.Set("request", RequestValue());
  network_->Emit("BrowserContext.route", std::move(params));
}

void Loader::FetchCookies(base::OnceClosure then) {
  content::WebContents* contents = network_ ? &network_->GetWebContents() : nullptr;
  network::mojom::CookieManager* manager =
      contents ? contents->GetBrowserContext()->GetDefaultStoragePartition()->GetCookieManagerForBrowserProcess()
               : nullptr;
  const bool credentials =
      request_.credentials_mode == network::mojom::CredentialsMode::kInclude ||
      (request_.credentials_mode == network::mojom::CredentialsMode::kSameOrigin &&
       (!request_.request_initiator || request_.request_initiator->IsSameOriginWith(request_.url)));
  if (!manager || !credentials || request_.headers.HasHeader("Cookie")) {
    std::move(then).Run();
    return;
  }
  net::CookieOptions options = net::CookieOptions::MakeAllInclusive();
  options.set_do_not_update_access_time();
  manager->GetCookieList(
      request_.url, options, net::CookiePartitionKeyCollection(),
      base::BindOnce(
          [](base::WeakPtr<Loader> self, base::OnceClosure then,
             const net::CookieAccessResultList& cookies, const net::CookieAccessResultList&) {
            if (!self)
              return;
            net::CookieList list;
            for (const auto& cookie : cookies)
              list.push_back(cookie.cookie);
            self->cookie_ = net::CanonicalCookie::BuildCookieLine(list);
            std::move(then).Run();
          },
          GetWeakPtr(), std::move(then)));
}

void Loader::Reroute() {
  if (!network_) {
    Continue({});
    return;
  }
  network_->AddExtraHeaders(request_);
  if (!network_->Intercepts(request_.url)) {
    if (network_->offline()) {
      Complete(net::ERR_INTERNET_DISCONNECTED);
      return;
    }
    Continue({});
    return;
  }
  const std::string route = network_->NextId("r");
  network_->Hold(route, GetWeakPtr());
  base::DictValue params;
  params.Set("route", route);
  params.Set("request", RequestValue());
  network_->Emit("BrowserContext.route", std::move(params));
}

void Loader::Gone() {
  if (!completed_ && network_) {
    completed_ = true;
    base::DictValue params;
    params.Set("request", id_);
    params.Set("failureText", net::ErrorToString(net::ERR_ABORTED));
    network_->Emit("Page.requestFailed", std::move(params));
  }
  delete this;
}

void Loader::ContinuedWith() {
  if (!network_)
    return;
  base::DictValue params = RequestValue();
  network_->Emit("Page.requestContinued", std::move(params));
}

void Loader::KeepFulfilled(std::string body) {
  if (network_)
    network_->KeepBody(id_, std::move(body));
}

void Loader::OnReceiveResponse(network::mojom::URLResponseHeadPtr head,
                               mojo::ScopedDataPipeConsumerHandle body,
                               std::optional<mojo_base::BigBuffer> metadata) {
  Responded(*head);
  mojo::ScopedDataPipeProducerHandle producer;
  mojo::ScopedDataPipeConsumerHandle consumer;
  if (body && network_ &&
      mojo::CreateDataPipe(nullptr, producer, consumer) == MOJO_RESULT_OK) {
    // Deletes itself when the body has passed.
    network_->BodyStarted(id_);
    new BodyTee(std::move(body), std::move(producer),
                base::BindOnce(
                    [](base::WeakPtr<NetworkImpl> network, std::string id,
                       std::string kept, bool complete) {
                      if (network) {
                        network->BodyEnded(id, complete ? std::make_optional(std::move(kept))
                                                        : std::nullopt);
                      }
                    },
                    network_, id_));
    body = std::move(consumer);
  }
  client_->OnReceiveResponse(std::move(head), std::move(body),
                             std::move(metadata));
}

void Loader::Responded(const network::mojom::URLResponseHead& head) {
  if (!network_)
    return;
  base::DictValue response;
  response.Set("request", id_);
  response.Set("url", request_.url.GetWithoutRef().spec());
  response.Set("status", head.headers ? head.headers->response_code() : 200);
  response.Set("statusText", head.headers ? head.headers->GetStatusText() : "");
  response.Set("headers", ResponseHeaders(head.headers.get()));
  response.Set("mimeType", head.mime_type);
  const auto& load = head.load_timing;
  if (!load.request_start.is_null())
    timing_start_ = load.request_start;
  if (!load.request_start_time.is_null())
    wall_start_ = load.request_start_time;
  auto elapsed = [&](base::TimeTicks time) {
    return time.is_null() ? -1.0 : (time - timing_start_).InMillisecondsF();
  };
  base::DictValue timing;
  timing.Set("startTime", wall_start_.InMillisecondsFSinceUnixEpoch());
  timing.Set("domainLookupStart", elapsed(load.connect_timing.domain_lookup_start));
  timing.Set("domainLookupEnd", elapsed(load.connect_timing.domain_lookup_end));
  timing.Set("connectStart", elapsed(load.connect_timing.connect_start));
  timing.Set("connectEnd", elapsed(load.connect_timing.connect_end));
  timing.Set("secureConnectionStart", elapsed(load.connect_timing.ssl_start));
  timing.Set("requestStart", elapsed(load.send_start));
  timing.Set("responseStart", elapsed(load.receive_headers_start));
  response.Set("timing", std::move(timing));
  response.Set("fromServiceWorker", head.was_fetched_via_service_worker);
  response_headers_size_ = head.headers ? head.headers->raw_headers().size() : 0;
  if (head.headers)
    response.Set("httpVersion", base::NumberToString(head.headers->GetHttpVersion().major_value()) + "." +
        base::NumberToString(head.headers->GetHttpVersion().minor_value()));
  // The network stack writes HTTP/2 and HTTP/3 status lines as HTTP/1.1.
  if (head.alpn_negotiated_protocol == "h2") response.Set("httpVersion", "2.0");
  else if (head.alpn_negotiated_protocol == "h3") response.Set("httpVersion", "3.0");

  if (!head.remote_endpoint.address().empty()) {
    response.Set("serverAddr", head.remote_endpoint.ToStringWithoutPort());
  }
  if (head.remote_endpoint.port())
    response.Set("serverPort", head.remote_endpoint.port());
  if (head.ssl_info && head.ssl_info->cert) {
    // Response.securityDetails, as CDP's securityDetails gives Playwright.
    const net::SSLInfo& ssl = *head.ssl_info;
    base::DictValue security;
    const char* version = nullptr;
    net::SSLVersionToString(&version, net::SSLConnectionStatusToVersion(ssl.connection_status));
    security.Set("protocol", version ? version : "");
    security.Set("subjectName", ssl.cert->subject().common_name);
    security.Set("issuer", ssl.cert->issuer().common_name);
    security.Set("validFrom", ssl.cert->valid_start().InSecondsFSinceUnixEpoch());
    security.Set("validTo", ssl.cert->valid_expiry().InSecondsFSinceUnixEpoch());
    response.Set("securityDetails", std::move(security));
  }
  if (navigation_) {
    network_->KeepNavigationResponse(request_.url.spec(), response);
    if (!original_url_.empty())
      network_->KeepNavigationResponse(original_url_, response);
  }
  network_->Emit("Page.response", std::move(response));
}

void Loader::OnReceiveRedirect(const net::RedirectInfo& redirect,
                               network::mojom::URLResponseHeadPtr head) {
  Responded(*head);
  if (network_) {
    // The redirect is a request of its own, from the one before, as
    // Playwright reports it.
    base::DictValue finished;
    finished.Set("request", id_);
    finished.Set("responseEndTiming", (base::TimeTicks::Now() - timing_start_).InMillisecondsF());
    network_->Emit("Page.requestFinished", std::move(finished));
    redirected_from_ = id_;
    id_ = network_->NextId("q");
    request_.url = redirect.new_url;
    request_.method = redirect.new_method;
    timing_start_ = base::TimeTicks::Now();
    wall_start_ = base::Time::Now();
    response_headers_size_ = 0;
    if (request_.devtools_request_id)
      network_->DevToolsRequestStarted(*request_.devtools_request_id, id_);
    network_->Emit("Page.request", RequestValue());
  }
  client_->OnReceiveRedirect(redirect, std::move(head));
}

void Loader::OnCompleteInternal(
    const network::URLLoaderCompletionStatus& status) {
  if (completed_)
    return;
  completed_ = true;
  if (network_) {
    base::DictValue params;
    params.Set("request", id_);
    if (status.error_code == net::OK) {
      const base::TimeTicks end = status.completion_time.is_null() ? base::TimeTicks::Now() : status.completion_time;
      params.Set("responseEndTiming", (end - timing_start_).InMillisecondsF());
      base::DictValue sizes;
      int64_t upload = 0;
      if (request_.request_body) {
        for (const auto& element : *request_.request_body->elements()) {
          if (element.type() == network::DataElement::Tag::kBytes)
            upload += element.As<network::DataElementBytes>().bytes().size();
          else if (element.type() == network::DataElement::Tag::kFile)
            upload += element.As<network::DataElementFile>().length();
          else { upload = -1; break; }
        }
      }
      sizes.Set("requestBodySize", static_cast<double>(upload));
      // Playwright's header-size convention: HTTP request line plus the
      // observed header names/values and CRLFs, independent of H2 framing.
      sizes.Set("requestHeadersSize", static_cast<double>(request_.method.size() +
          request_.url.PathForRequest().size() + 12 + request_.headers.ToString().size()));
      sizes.Set("responseBodySize", static_cast<double>(status.encoded_body_length.InBytes()));
      const int64_t received = status.encoded_data_length.InBytes();
      const int64_t encoded_body = status.encoded_body_length.InBytes();
      sizes.Set("responseHeadersSize", static_cast<double>(received >= encoded_body && received > 0
          ? received - encoded_body : response_headers_size_));
      params.Set("sizes", std::move(sizes));
      network_->Emit("Page.requestFinished", std::move(params));
    } else {
      params.Set("failureText", net::ErrorToString(status.error_code));
      network_->Emit("Page.requestFailed", std::move(params));
    }
  }
  client_->OnComplete(status);
}

void ProxyFactory::CreateLoaderAndStart(
    mojo::PendingReceiver<network::mojom::URLLoader> loader,
    int32_t request_id,
    uint32_t options,
    const network::ResourceRequest& request,
    mojo::PendingRemote<network::mojom::URLLoaderClient> client,
    const net::MutableNetworkTrafficAnnotationTag& annotation) {
  const std::string id = network_ ? network_->NextId("q") : "q0";
  const bool navigation =
      request.mode == network::mojom::RequestMode::kNavigate;
  // Deletes itself when the page's end goes.
  // The certificate with the response, for Response.securityDetails.
  options |= network::mojom::kURLLoadOptionSendSSLInfoWithResponse;
  auto* proxied = new Loader(network_, id, frame_, navigation, std::move(loader),
                             request_id, options, request, std::move(client),
                             annotation, target_.get());
  proxied->Start();
}

}  // namespace

std::string NetworkGlobToRegex(const std::string& glob) { return GlobToRegex(glob); }
int NetworkErrorCode(const std::string& code) { return NetError(code); }
const char* NetworkResourceType(const network::ResourceRequest& request) { return ResourceType(request); }
bool NetworkPatternsMatch(const base::ListValue& patterns, const std::string& url) {
  std::vector<std::unique_ptr<RE2>> compiled;
  std::string error;
  if (!NetworkImpl::Compile(patterns, &compiled, &error))
    return false;
  for (const auto& pattern : compiled) {
    if (RE2::PartialMatch(url, *pattern))
      return true;
  }
  return false;
}

Network* Network::For(content::WebContents* contents) {
  return NetworkImpl::FromWebContents(contents);
}

void Network::Create(content::WebContents* contents, EventFunc emit) {
  NetworkImpl::CreateForWebContents(contents);
  NetworkImpl::FromWebContents(contents)->SetEmit(std::move(emit));
}

namespace {

// A preflight's answer, held until the network service lets it go.
class AnsweredLoader : public network::mojom::URLLoader {
 public:
  void FollowRedirect(network::HttpRequestHeadersUpdateParams, const std::optional<GURL>&) override {}
  void SetPriority(net::RequestPriority, int32_t) override {}
};

// The network service's own requests of a page's factory, its CORS
// preflights among them (kURLLoadOptionAsCorsPreflight), as Chromium's
// DevTools has them through the factory's override.
class PreflightFactory : public network::mojom::URLLoaderFactory {
 public:
  PreflightFactory(base::WeakPtr<NetworkImpl> network,
                   mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
                   mojo::PendingRemote<network::mojom::URLLoaderFactory> target)
      : network_(std::move(network)), target_(std::move(target)) {
    receivers_.Add(this, std::move(receiver));
    receivers_.set_disconnect_handler(base::BindRepeating(
        &PreflightFactory::Disconnected, base::Unretained(this)));
    target_.set_disconnect_handler(
        base::BindOnce(&PreflightFactory::Gone, base::Unretained(this)));
  }

  void CreateLoaderAndStart(
      mojo::PendingReceiver<network::mojom::URLLoader> loader,
      int32_t request_id,
      uint32_t options,
      const network::ResourceRequest& request,
      mojo::PendingRemote<network::mojom::URLLoaderClient> client,
      const net::MutableNetworkTrafficAnnotationTag& annotation) override {
    if (!(options & network::mojom::kURLLoadOptionAsCorsPreflight) || !network_ ||
        !network_->Routes()) {
      target_->CreateLoaderAndStart(std::move(loader), request_id, options, request,
                                    std::move(client), annotation);
      return;
    }
    // crNetworkManager.ts: "If interception is enabled, we accept all CORS
    // options, assuming that this was intended when setting route."
    auto header = [&](const char* name) {
      return request.headers.GetHeader(name).value_or(std::string());
    };
    std::string origin = header("Origin");
    if (origin.empty())
      origin = request.request_initiator ? request.request_initiator->Serialize() : "*";
    std::string method = header("Access-Control-Request-Method");
    std::string raw = "HTTP/1.1 204 No Content\nAccess-Control-Allow-Origin: " + origin +
                      "\nAccess-Control-Allow-Methods: " +
                      (method.empty() ? std::string("GET, POST, OPTIONS, DELETE") : method) +
                      "\nAccess-Control-Allow-Credentials: true\n";
    const std::string allowed = header("Access-Control-Request-Headers");
    if (!allowed.empty())
      raw += "Access-Control-Allow-Headers: " + allowed + "\n";
    auto head = network::mojom::URLResponseHead::New();
    head->headers = base::MakeRefCounted<net::HttpResponseHeaders>(
        net::HttpUtil::AssembleRawHeaders(raw));
    head->request_time = head->response_time = base::Time::Now();
    mojo::ScopedDataPipeProducerHandle producer;
    mojo::ScopedDataPipeConsumerHandle consumer;
    if (mojo::CreateDataPipe(nullptr, producer, consumer) != MOJO_RESULT_OK) {
      target_->CreateLoaderAndStart(std::move(loader), request_id, options, request,
                                    std::move(client), annotation);
      return;
    }
    producer.reset();
    mojo::Remote<network::mojom::URLLoaderClient> answer(std::move(client));
    answer->OnReceiveResponse(std::move(head), std::move(consumer), std::nullopt);
    answer->OnComplete(network::URLLoaderCompletionStatus(net::OK));
    mojo::MakeSelfOwnedReceiver(std::make_unique<AnsweredLoader>(), std::move(loader));
  }

  void Clone(mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver) override {
    receivers_.Add(this, std::move(receiver));
  }

 private:
  void Disconnected() {
    if (receivers_.empty())
      Gone();
  }
  void Gone() { delete this; }

  base::WeakPtr<NetworkImpl> network_;
  mojo::Remote<network::mojom::URLLoaderFactory> target_;
  mojo::ReceiverSet<network::mojom::URLLoaderFactory> receivers_;
};

}  // namespace

void InterceptCorsPreflights(content::RenderFrameHost* frame,
                             network::mojom::URLLoaderFactoryOverridePtr* factory_override) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!frame || !factory_override)
    return;
  content::WebContents* contents = content::WebContents::FromRenderFrameHost(frame);
  NetworkImpl* network = contents ? NetworkImpl::FromWebContents(contents) : nullptr;
  if (!network)
    return;
  mojo::PendingRemote<network::mojom::URLLoaderFactory> overriding;
  auto receiver = overriding.InitWithNewPipeAndPassReceiver();
  if (*factory_override) {
    // Another's override first, nearer the network.
    new PreflightFactory(network->GetWeakPtr(), std::move(receiver),
                         std::move((*factory_override)->overriding_factory));
    (*factory_override)->overriding_factory = std::move(overriding);
    return;
  }
  mojo::PendingRemote<network::mojom::URLLoaderFactory> target;
  auto overridden = target.InitWithNewPipeAndPassReceiver();
  // Deletes itself when the network service's end and its clones go.
  new PreflightFactory(network->GetWeakPtr(), std::move(receiver), std::move(target));
  *factory_override = network::mojom::URLLoaderFactoryOverride::New(
      std::move(overriding), std::move(overridden), /*skip_cors_enabled_scheme_check=*/false);
}

void InterceptURLLoaderFactory(content::RenderFrameHost* frame,
                               network::URLLoaderFactoryBuilder& builder) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!frame)
    return;
  content::WebContents* contents =
      content::WebContents::FromRenderFrameHost(frame);
  NetworkImpl* network = contents ? NetworkImpl::FromWebContents(contents) : nullptr;
  if (!network)
    return;
  auto [receiver, target] = builder.Append();
  // Deletes itself when the page's factory and its clones go.
  new ProxyFactory(network->GetWeakPtr(),
                   "f" + base::NumberToString(frame->GetFrameTreeNodeId().value()),
                   std::move(receiver), std::move(target));
}

}  // namespace zebkit
