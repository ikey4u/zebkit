// zebkit's services (services.h) that need the engine: HTTP through its
// network service, and registrable domains (net's list of public suffixes).
// The others are in services_core.cc.

#ifdef UNSAFE_BUFFERS_BUILD
// The C API takes and returns C strings and arrays.
#pragma allow_unsafe_buffers
#endif

#include <stdlib.h>
#include <string.h>

#include <memory>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/memory/ref_counted.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "build/build_config.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/storage_partition.h"
#include "net/base/net_errors.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "net/cookies/site_for_cookies.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "net/url_request/redirect_info.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/cpp/simple_url_loader_stream_consumer.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/include/zebkit/services.h"
#include "zebkit/engine/services_internal.h"

namespace {

using zebkit::services::Fail;
using zebkit::services::Give;
using zebkit::services::GiveString;
using zebkit::services::kDefaultMaxSize;

// ---- HTTP ----

class Fetch : public network::SimpleURLLoaderStreamConsumer {
 public:
  static void Start(std::unique_ptr<network::ResourceRequest> request,
                    std::string body,
                    std::string content_type,
                    zebkit_response_func func,
                    void* data) {
    auto* fetch = new Fetch(func, data);
    constexpr net::NetworkTrafficAnnotationTag kAnnotation =
        net::DefineNetworkTrafficAnnotation("zebkit_fetch", R"(
          semantics {
            sender: "zebkit"
            description: "A request the embedding application makes."
            trigger: "The application calls zebkit_fetch()."
            data: "What the application sends."
            destination: OTHER
          }
          policy {
            cookies_allowed: YES
            setting: "Up to the application."
            policy_exception_justification: "Made by the application."
          })");
    fetch->loader_ =
        network::SimpleURLLoader::Create(std::move(request), kAnnotation);
    fetch->loader_->SetOnResponseStartedCallback(
        base::BindOnce(&Fetch::ResponseStarted, base::Unretained(fetch)));
    // What the application gave for one origin is not for another's, as the
    // fetch standard does for Authorization. The stack's own cookies are kept
    // to their own origins by the stack.
    fetch->loader_->SetOnRedirectCallback(base::BindRepeating(
        [](const GURL& from, const net::RedirectInfo& info,
           const network::mojom::URLResponseHead&,
           std::vector<std::string>* removed) {
          if (!url::Origin::Create(from).IsSameOriginWith(info.new_url)) {
            removed->push_back("Authorization");
            removed->push_back("Proxy-Authorization");
            removed->push_back("Cookie");
          }
        }));
    if (!body.empty() || !content_type.empty())
      fetch->loader_->AttachStringForUpload(std::move(body), content_type);
    fetch->loader_->SetAllowHttpErrorResults(true);
    scoped_refptr<network::SharedURLLoaderFactory> factory =
        zebkit::GetBrowserContext()
            ->GetDefaultStoragePartition()
            ->GetURLLoaderFactoryForBrowserProcess();
    // DownloadToString only accepts limits up to 5 MiB. The public C API
    // permits 256 MiB, so consume the stream while enforcing that bound.
    fetch->loader_->DownloadAsStream(factory.get(), fetch);
  }

 private:
  Fetch(zebkit_response_func func, void* data) : func_(func), data_(data) {}
  ~Fetch() override = default;

  void ResponseStarted(const GURL& url,
                       const network::mojom::URLResponseHead& head) {
    response_url_ = url.spec();
    response_headers_ = head.headers;
  }

  void OnDataReceived(std::string_view chunk, base::OnceClosure resume) override {
    if (chunk.size() > kDefaultMaxSize - body_.size()) {
      // The stream API permits deleting the loader from this callback.
      Done(std::nullopt, net::ERR_INSUFFICIENT_RESOURCES);
      return;
    }
    body_.append(chunk);
    std::move(resume).Run();
  }

  void OnComplete(bool success) override {
    if (success)
      Done(std::move(body_));
    else
      Done(std::nullopt);
  }

  void OnRetry(base::OnceClosure start_retry) override {
    body_.clear();
    std::move(start_retry).Run();
  }

  void Done(std::optional<std::string> body, int error_override = net::OK) {
    zebkit_response response = {};
    std::string headers;
    // The loader's status accessors require completed requests. A size
    // rejection happens during streaming, so use the captured response.
    const bool early = error_override != net::OK;
    std::string url = early ? response_url_ : loader_->GetFinalURL().spec();
    std::string error;
    const network::mojom::URLResponseHead* head =
        early ? nullptr : loader_->ResponseInfo();
    const net::HttpResponseHeaders* received_headers =
        early ? response_headers_.get() : (head ? head->headers.get() : nullptr);
    if (received_headers) {
      response.status = received_headers->response_code();
      size_t iter = 0;
      std::string name, value;
      while (received_headers->EnumerateHeaderLines(&iter, &name, &value)) {
        if (!headers.empty())
          headers += "\r\n";
        headers += name + ": " + value;
      }
    }
    if (!body) {
      error = net::ErrorToString(error_override == net::OK
                                    ? loader_->NetError()
                                    : error_override);
      response.error = error.c_str();
    } else {
      response.body = reinterpret_cast<const uint8_t*>(body->data());
      response.body_size = body->size();
    }
    response.url = url.c_str();
    response.headers = headers.c_str();
    if (func_)
      func_(&response, data_);
    delete this;
  }

  zebkit_response_func func_;
  void* data_;
  std::unique_ptr<network::SimpleURLLoader> loader_;
  std::string body_;
  std::string response_url_;
  scoped_refptr<net::HttpResponseHeaders> response_headers_;
};

}  // namespace

extern "C" {

char* zebkit_url_registrable_domain(const char* host_or_url) {
  if (!host_or_url || !*host_or_url)
    return nullptr;
  zebkit_icu_init();
  GURL parsed(host_or_url);
  if (!parsed.is_valid() || !parsed.has_host())
    parsed = GURL(std::string("https://") + host_or_url + "/");
  if (!parsed.is_valid() || !parsed.has_host())
    return nullptr;
  return GiveString(net::registry_controlled_domains::GetDomainAndRegistry(
      parsed, net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES));
}

void zebkit_fetch(const char* method,
                  const char* url,
                  const char* headers,
                  const void* body,
                  size_t body_size,
                  zebkit_response_func func,
                  void* data) {
  if (!zebkit::CurrentlyOnEngineThread())
    return;
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL(url ? url : "");
  request->method = method && *method ? method : "GET";
  request->credentials_mode = network::mojom::CredentialsMode::kInclude;
  request->site_for_cookies = net::SiteForCookies::FromUrl(request->url);
  std::string content_type;
  if (headers) {
    for (std::string_view line : base::SplitStringPieceUsingSubstr(
             headers, "\r\n", base::TRIM_WHITESPACE,
             base::SPLIT_WANT_NONEMPTY)) {
      const size_t colon = line.find(':');
      if (colon == std::string_view::npos)
        continue;
      std::string name(base::TrimWhitespaceASCII(line.substr(0, colon),
                                                 base::TRIM_ALL));
      std::string value(base::TrimWhitespaceASCII(line.substr(colon + 1),
                                                  base::TRIM_ALL));
      if (base::EqualsCaseInsensitiveASCII(name, "content-type"))
        content_type = value;
      else
        request->headers.SetHeader(name, value);
    }
  }
  if (!request->url.is_valid()) {
    zebkit_response response = {};
    response.url = "";
    response.headers = "";
    response.error = "not a valid URL";
    if (func)
      func(&response, data);
    return;
  }
  std::string upload;
  if (body && body_size)
    upload.assign(static_cast<const char*>(body), body_size);
  if (!upload.empty() && content_type.empty())
    content_type = "application/octet-stream";
  Fetch::Start(std::move(request), std::move(upload), std::move(content_type),
               func, data);
}

}  // extern "C"
