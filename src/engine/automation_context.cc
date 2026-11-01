// zebkit's automation, the page's profile (automation_context.h): cookies
// through its network context's cookie manager, requests through its URL
// loader factory, the media features in its preferences.

#include "zebkit/engine/automation_context.h"

#include <memory>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/base/net_errors.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_constants.h"
#include "net/cookies/cookie_inclusion_status.h"
#include "net/cookies/cookie_options.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "net/url_request/redirect_info.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/cpp/simple_url_loader_stream_consumer.h"
#include "services/network/public/mojom/cookie_manager.mojom.h"
#include "services/network/public/mojom/url_loader_network_service_observer.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "third_party/blink/public/common/web_preferences/web_preferences.h"
#include "third_party/blink/public/mojom/css/preferred_color_scheme.mojom-shared.h"
#include "third_party/blink/public/mojom/css/preferred_contrast.mojom-shared.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace zebkit {

namespace {

constexpr net::NetworkTrafficAnnotationTag kAnnotation =
    net::DefineNetworkTrafficAnnotation("zebkit_automation_fetch", R"(
      semantics {
        sender: "zebkit automation"
        description: "A request the application asks for (Playwright's APIRequestContext)."
        trigger: "The application's automation command."
        data: "What the application sends."
        destination: OTHER
      }
      policy {
        cookies_allowed: YES
        setting: "The application decides."
        policy_exception_justification: "An embedder's own request."
      })");

network::mojom::CookieManager* CookieManager(content::BrowserContext* profile) {
  return profile->GetDefaultStoragePartition()
      ->GetCookieManagerForBrowserProcess();
}

const char* SameSiteName(net::CookieSameSite same_site) {
  switch (same_site) {
    case net::CookieSameSite::STRICT_MODE:
      return "Strict";
    case net::CookieSameSite::LAX_MODE:
      return "Lax";
    default:
      return "None";
  }
}

// A cookie as Playwright's protocol has it (structs.yml's NetworkCookie).
base::DictValue CookieValue(const net::CanonicalCookie& cookie) {
  base::DictValue value;
  value.Set("name", cookie.Name());
  value.Set("value", cookie.Value());
  value.Set("domain", cookie.Domain());
  value.Set("path", cookie.Path());
  value.Set("expires", cookie.IsPersistent()
                           ? cookie.ExpiryDate().InSecondsFSinceUnixEpoch()
                           : -1.0);
  value.Set("httpOnly", cookie.IsHttpOnly());
  value.Set("secure", cookie.SecureAttribute());
  value.Set("sameSite", SameSiteName(cookie.SameSite()));
  return value;
}

// Whether @cookie would go with a request to @url, as Playwright filters
// BrowserContext.cookies.
bool CookieForUrl(const net::CanonicalCookie& cookie, const GURL& url) {
  if (!cookie.IsDomainMatch(url.host()))
    return false;
  if (!cookie.IsOnPath(url.path()))
    return false;
  if (cookie.SecureAttribute() && !url.SchemeIsCryptographic() &&
      url.host() != "localhost") {
    return false;
  }
  return true;
}

void Fail(ContextDone done, const std::string& error) {
  std::move(done).Run(base::Value(), error);
}

}  // namespace

void ContextCookies(content::BrowserContext* profile,
                    const base::DictValue& params,
                    ContextDone done) {
  std::vector<GURL> urls;
  if (const base::ListValue* given = params.FindList("urls")) {
    for (const base::Value& url : *given) {
      if (url.is_string())
        urls.emplace_back(url.GetString());
    }
  }
  CookieManager(profile)->GetAllCookies(base::BindOnce(
      [](std::vector<GURL> urls, ContextDone done,
         const std::vector<net::CanonicalCookie>& cookies) {
        base::ListValue list;
        for (const net::CanonicalCookie& cookie : cookies) {
          bool wanted = urls.empty();
          for (const GURL& url : urls)
            wanted = wanted || CookieForUrl(cookie, url);
          if (wanted)
            list.Append(CookieValue(cookie));
        }
        base::DictValue answer;
        answer.Set("cookies", std::move(list));
        std::move(done).Run(base::Value(std::move(answer)), std::string());
      },
      std::move(urls), std::move(done)));
}

void ContextAddCookies(content::BrowserContext* profile,
                       const base::DictValue& params,
                       ContextDone done) {
  const base::ListValue* cookies = params.FindList("cookies");
  if (!cookies) {
    Fail(std::move(done), "cookies: expected an array");
    return;
  }
  std::vector<std::unique_ptr<net::CanonicalCookie>> made;
  std::vector<GURL> sources;
  for (const base::Value& entry : *cookies) {
    if (!entry.is_dict()) {
      Fail(std::move(done), "cookies: expected objects");
      return;
    }
    const base::DictValue& cookie = entry.GetDict();
    const std::string* name = cookie.FindString("name");
    const std::string* value = cookie.FindString("value");
    if (!name || !value) {
      Fail(std::move(done), "Cookie should have a name and a value");
      return;
    }
    std::string domain, path = "/";
    GURL url;
    if (const std::string* given = cookie.FindString("url")) {
      url = GURL(*given);
      if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS()) {
        Fail(std::move(done), "Invalid URL " + *given);
        return;
      }
    } else if (const std::string* given_domain = cookie.FindString("domain")) {
      domain = *given_domain;
      if (const std::string* given_path = cookie.FindString("path"))
        path = *given_path;
      const bool secure = cookie.FindBool("secure").value_or(false);
      const std::string host =
          domain.starts_with(".") ? domain.substr(1) : domain;
      url = GURL(std::string(secure ? "https://" : "http://") + host + path);
    } else {
      Fail(std::move(done), "Cookie should have a url or a domain/path pair");
      return;
    }
    const double expires = cookie.FindDouble("expires").value_or(-1);
    std::string same_site = "Lax";
    if (const std::string* given = cookie.FindString("sameSite"))
      same_site = *given;
    const net::CookieSameSite site =
        same_site == "Strict" ? net::CookieSameSite::STRICT_MODE
        : same_site == "None" ? net::CookieSameSite::NO_RESTRICTION
                              : net::CookieSameSite::LAX_MODE;
    const base::Time now = base::Time::Now();
    net::CookieInclusionStatus status;
    std::unique_ptr<net::CanonicalCookie> canonical =
        net::CanonicalCookie::CreateSanitizedCookie(
            url, *name, *value, domain.empty() ? std::string() : domain,
            cookie.FindString("url") ? std::string(url.path()) : path, now,
            expires > 0 ? base::Time::FromSecondsSinceUnixEpoch(expires)
                        : base::Time(),
            now, cookie.FindBool("secure").value_or(url.SchemeIsCryptographic()),
            cookie.FindBool("httpOnly").value_or(false), site,
            net::COOKIE_PRIORITY_DEFAULT, std::nullopt, &status);
    if (!canonical) {
      Fail(std::move(done), "Invalid cookie fields: " + *name);
      return;
    }
    made.push_back(std::move(canonical));
    sources.push_back(url);
  }
  // Set one after the other, then answered.
  struct Pending {
    std::vector<std::unique_ptr<net::CanonicalCookie>> cookies;
    std::vector<GURL> sources;
    ContextDone done;
    std::string error;
    size_t left;
  };
  auto pending = std::make_shared<Pending>();
  pending->cookies = std::move(made);
  pending->sources = std::move(sources);
  pending->done = std::move(done);
  pending->left = pending->cookies.size();
  if (!pending->left) {
    std::move(pending->done).Run(base::Value(base::DictValue()), std::string());
    return;
  }
  network::mojom::CookieManager* manager = CookieManager(profile);
  net::CookieOptions options = net::CookieOptions::MakeAllInclusive();
  for (size_t i = 0; i < pending->cookies.size(); ++i) {
    manager->SetCanonicalCookie(
        *pending->cookies[i], pending->sources[i], options,
        base::BindOnce(
            [](std::shared_ptr<Pending> pending, std::string name,
               net::CookieAccessResult result) {
              if (!result.status.IsInclude() && pending->error.empty())
                pending->error = "Cookie was not set: " + name;
              if (--pending->left == 0) {
                std::move(pending->done)
                    .Run(base::Value(base::DictValue()), pending->error);
              }
            },
            pending, pending->cookies[i]->Name()));
  }
}

void ContextClearCookies(content::BrowserContext* profile,
                         const base::DictValue& params,
                         ContextDone done) {
  auto filter = network::mojom::CookieDeletionFilter::New();
  if (const std::string* name = params.FindString("name"))
    filter->cookie_name = *name;
  if (const std::string* domain = params.FindString("domain"))
    filter->host_name = domain->starts_with(".") ? domain->substr(1) : *domain;
  const std::string* path = params.FindString("path");
  const std::string wanted_path = path ? *path : std::string();
  if (wanted_path.empty()) {
    CookieManager(profile)->DeleteCookies(
        std::move(filter),
        base::BindOnce(
            [](ContextDone done, uint32_t) {
              std::move(done).Run(base::Value(base::DictValue()), std::string());
            },
            std::move(done)));
    return;
  }
  // A path: the cookies of the filter on that path, one by one.
  network::mojom::CookieManager* manager = CookieManager(profile);
  manager->GetAllCookies(base::BindOnce(
      [](network::mojom::CookieManager* manager, std::string name,
         std::string domain, std::string path, ContextDone done,
         const std::vector<net::CanonicalCookie>& cookies) {
        for (const net::CanonicalCookie& cookie : cookies) {
          if ((!name.empty() && cookie.Name() != name) ||
              (!domain.empty() && cookie.Domain() != domain &&
               cookie.Domain() != "." + domain) ||
              cookie.Path() != path) {
            continue;
          }
          manager->DeleteCanonicalCookie(cookie, base::DoNothing());
        }
        std::move(done).Run(base::Value(base::DictValue()), std::string());
      },
      manager, filter->cookie_name.value_or(""),
      filter->host_name.value_or(""), wanted_path, std::move(done)));
}

// Playwright's APIRequestContext.fetch: redirects as its client follows them
// (maxRedirects, 0 for the redirect itself as the answer; over it an error), the
// credentials a caller gave left behind at another origin, and a bound on the
// body, streamed rather than held twice.
class ContextFetcher : public network::SimpleURLLoaderStreamConsumer,
                       public network::mojom::URLLoaderNetworkServiceObserver {
 public:
  static constexpr int64_t kDefaultMaxBody = 256 * 1024 * 1024;

  ContextFetcher(ContextDone done, int max_redirects, int64_t max_body)
      : done_(std::move(done)),
        max_redirects_(max_redirects),
        max_body_(max_body) {}

  // What fetch.ts's ignoreHTTPSErrors does in its own client: the network service
  // asks the observer of a request about a certificate.
  mojo::PendingRemote<network::mojom::URLLoaderNetworkServiceObserver> IgnoreCertificateErrors() {
    ignore_certificate_errors_ = true;
    mojo::PendingRemote<network::mojom::URLLoaderNetworkServiceObserver> remote;
    observers_.Add(this, remote.InitWithNewPipeAndPassReceiver());
    return remote;
  }

  void Start(std::unique_ptr<network::SimpleURLLoader> loader,
             network::mojom::URLLoaderFactory* factory) {
    loader_ = std::move(loader);
    loader_->SetOnRedirectCallback(base::BindRepeating(
        &ContextFetcher::Redirected, base::Unretained(this)));
    loader_->SetOnResponseStartedCallback(base::BindOnce(
        &ContextFetcher::Started, base::Unretained(this)));
    loader_->DownloadAsStream(factory, this);
  }

 private:
  // network::mojom::URLLoaderNetworkServiceObserver
  void OnSSLCertificateError(const GURL& url,
                             int net_error,
                             const net::SSLInfo& ssl_info,
                             bool fatal,
                             OnSSLCertificateErrorCallback response) override {
    std::move(response).Run(ignore_certificate_errors_ ? net::OK : net_error);
  }
  void OnCertificateRequested(
      const std::optional<base::UnguessableToken>& window_id,
      const scoped_refptr<net::SSLCertRequestInfo>& cert_info,
      mojo::PendingRemote<network::mojom::ClientCertificateResponder> responder) override {
    mojo::Remote<network::mojom::ClientCertificateResponder>(std::move(responder))
        ->ContinueWithoutCertificate();
  }
  void OnAuthRequired(
      const std::optional<base::UnguessableToken>& window_id,
      int32_t request_id,
      const GURL& url,
      bool first_auth_attempt,
      const net::AuthChallengeInfo& auth_info,
      const scoped_refptr<net::HttpResponseHeaders>& head_headers,
      mojo::PendingRemote<network::mojom::AuthChallengeResponder> responder) override {
    // A 401 is the caller's to see (ContextFetch answers fetch.ts's way).
    mojo::Remote<network::mojom::AuthChallengeResponder>(std::move(responder))
        ->OnAuthCredentials(std::nullopt);
  }
  void OnLocalNetworkAccessPermissionRequired(
      network::mojom::TransportType type,
      network::mojom::IPAddressSpace ip_address_space,
      OnLocalNetworkAccessPermissionRequiredCallback callback) override {
    std::move(callback).Run(network::mojom::LocalNetworkAccessResult::kDenied);
  }
  void OnPlatformLocalNetworkPermissionRequired(
      OnPlatformLocalNetworkPermissionRequiredCallback callback) override {
    std::move(callback).Run(false);
  }
  void OnClearSiteData(const GURL& url,
                       const std::string& header_value,
                       int32_t load_flags,
                       const std::optional<net::CookiePartitionKey>& cookie_partition_key,
                       bool partitioned_state_allowed_only,
                       OnClearSiteDataCallback callback) override {
    std::move(callback).Run();
  }
  void OnLoadingStateUpdate(network::mojom::LoadInfoPtr info,
                            OnLoadingStateUpdateCallback callback) override {
    std::move(callback).Run();
  }
  void OnDataUseUpdate(int32_t network_traffic_annotation_id_hash,
                       base::ByteSize recv_bytes,
                       base::ByteSize sent_bytes) override {}
  void Clone(mojo::PendingReceiver<network::mojom::URLLoaderNetworkServiceObserver>
                 observer) override {
    observers_.Add(this, std::move(observer));
  }
  void OnWebSocketConnectedToLocalNetwork(
      const GURL& request_url,
      network::mojom::IPAddressSpace ip_address_space) override {}
  void OnUrlLoaderConnectedToLocalNetwork(
      const GURL& request_url,
      network::mojom::IPAddressSpace response_address_space,
      network::mojom::IPAddressSpace client_address_space,
      network::mojom::IPAddressSpace target_address_space) override {}

  void Redirected(const GURL& from,
                  const net::RedirectInfo& info,
                  const network::mojom::URLResponseHead& head,
                  std::vector<std::string>* removed) {
    // The loader may be deleted in this callback, which Answer() does: nothing
    // is sent to the redirect's target.
    if (max_redirects_ == 0) {
      head_ = head.headers;
      url_ = from.spec();
      Answer();
      return;
    }
    if (++redirects_ > max_redirects_) {
      error_ = "Max redirect count exceeded";
      Answer();
      return;
    }
    if (!url::Origin::Create(from).IsSameOriginWith(info.new_url)) {
      removed->push_back("Authorization");
      removed->push_back("Proxy-Authorization");
      removed->push_back("Cookie");
    }
  }

  void Started(const GURL& url, const network::mojom::URLResponseHead& head) {
    url_ = url.spec();
    head_ = head.headers;
  }

  void OnDataReceived(std::string_view chunk, base::OnceClosure resume) override {
    if (static_cast<int64_t>(body_.size() + chunk.size()) > max_body_) {
      error_ = "Response body is larger than the limit of " +
               base::NumberToString(max_body_) + " bytes";
      Answer();
      return;
    }
    body_.append(chunk);
    std::move(resume).Run();
  }

  void OnComplete(bool success) override {
    if (!success || !head_)
      error_ = net::ErrorToString(loader_->NetError());
    Answer();
  }

  void OnRetry(base::OnceClosure start_retry) override {
    body_.clear();
    std::move(start_retry).Run();
  }

  // Deletes this.
  void Answer() {
    ContextDone done = std::move(done_);
    if (!error_.empty()) {
      std::string error = std::move(error_);
      delete this;
      Fail(std::move(done), error);
      return;
    }
    base::DictValue response;
    response.Set("url", url_);
    response.Set("status", head_->response_code());
    response.Set("statusText", head_->GetStatusText());
    base::ListValue headers;
    size_t iterator = 0;
    std::string name, value;
    while (head_->EnumerateHeaderLines(&iterator, &name, &value)) {
      base::DictValue header;
      header.Set("name", name);
      header.Set("value", value);
      headers.Append(std::move(header));
    }
    response.Set("headers", std::move(headers));
    response.Set("body", base::Base64Encode(body_));
    base::DictValue answer;
    answer.Set("response", std::move(response));
    delete this;
    std::move(done).Run(base::Value(std::move(answer)), std::string());
  }

  ContextDone done_;
  const int max_redirects_;
  const int64_t max_body_;
  int redirects_ = 0;
  std::string url_, body_, error_;
  scoped_refptr<net::HttpResponseHeaders> head_;
  bool ignore_certificate_errors_ = false;
  mojo::ReceiverSet<network::mojom::URLLoaderNetworkServiceObserver> observers_;
  std::unique_ptr<network::SimpleURLLoader> loader_;
};

base::ListValue HttpCredentialsOf(const base::DictValue& params) {
  base::ListValue list;
  if (const base::ListValue* given = params.FindList("httpCredentials"))
    list = given->Clone();
  else if (const base::DictValue* one = params.FindDict("httpCredentials"))
    list.Append(one->Clone());
  return list;
}

const base::DictValue* MatchingHttpCredentials(const base::ListValue& credentials,
                                               const GURL& url) {
  const std::string origin = url::Origin::Create(url).Serialize();
  for (const base::Value& entry : credentials) {
    if (!entry.is_dict())
      continue;
    const std::string* wanted = entry.GetDict().FindString("origin");
    if (!wanted || wanted->empty() || base::EqualsCaseInsensitiveASCII(*wanted, origin))
      return &entry.GetDict();
  }
  return nullptr;
}

void ContextFetch(content::BrowserContext* profile,
                  const base::DictValue& params,
                  ContextDone done) {
  const std::string* url = params.FindString("url");
  GURL target(url ? *url : std::string());
  if (!target.is_valid()) {
    Fail(std::move(done), "Invalid URL");
    return;
  }
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = target;
  if (const std::string* method = params.FindString("method"))
    request->method = base::ToUpperASCII(*method);
  if (const base::ListValue* headers = params.FindList("headers")) {
    for (const base::Value& header : *headers) {
      const std::string* name =
          header.is_dict() ? header.GetDict().FindString("name") : nullptr;
      const std::string* value =
          header.is_dict() ? header.GetDict().FindString("value") : nullptr;
      if (name && value)
        request->headers.SetHeader(*name, *value);
    }
  }
  const int max_redirects = params.FindInt("maxRedirects").value_or(20);
  request->credentials_mode = network::mojom::CredentialsMode::kInclude;
  request->site_for_cookies = net::SiteForCookies::FromUrl(target);
  double limit = params.FindDouble("maxBodySize")
                     .value_or(ContextFetcher::kDefaultMaxBody);
  if (limit <= 0 || limit > ContextFetcher::kDefaultMaxBody)
    limit = ContextFetcher::kDefaultMaxBody;
  // fetch.ts's httpCredentials, for @target's origin and with no Authorization of
  // the caller's: at once with send "always", else again after a Basic 401.
  const base::ListValue credentials = HttpCredentialsOf(params);
  if (const base::DictValue* given = MatchingHttpCredentials(credentials, target)) {
    const std::string* username = given->FindString("username");
    const std::string* password = given->FindString("password");
    const std::string* send = given->FindString("send");
    if (username && !request->headers.HasHeader("Authorization")) {
      const std::string basic =
          "Basic " + base::Base64Encode(*username + ":" + (password ? *password : std::string()));
      if (send && *send == "always") {
        request->headers.SetHeader("Authorization", basic);
      } else {
        base::DictValue again = params.Clone();
        again.Remove("httpCredentials");
        base::DictValue header;
        header.Set("name", "Authorization");
        header.Set("value", basic);
        if (base::ListValue* headers = again.FindList("headers")) {
          headers->Append(std::move(header));
        } else {
          base::ListValue list;
          list.Append(std::move(header));
          again.Set("headers", std::move(list));
        }
        done = base::BindOnce(
            [](base::WeakPtr<content::BrowserContext> profile, base::DictValue again,
               ContextDone done, base::Value result, const std::string& error) {
              const base::DictValue* response =
                  result.is_dict() ? result.GetDict().FindDict("response") : nullptr;
              bool challenged = false;
              if (response && response->FindInt("status").value_or(0) == 401) {
                if (const base::ListValue* headers = response->FindList("headers")) {
                  for (const base::Value& header : *headers) {
                    const std::string* name = header.GetDict().FindString("name");
                    const std::string* value = header.GetDict().FindString("value");
                    challenged = challenged ||
                                 (name && value &&
                                  base::EqualsCaseInsensitiveASCII(*name, "www-authenticate") &&
                                  base::StartsWith(base::TrimWhitespaceASCII(*value, base::TRIM_ALL), "Basic"));
                  }
                }
              }
              if (challenged && profile) {
                ContextFetch(profile.get(), again, std::move(done));
                return;
              }
              std::move(done).Run(std::move(result), error);
            },
            profile->GetWeakPtr(), std::move(again), std::move(done));
      }
    }
  }
  // Deletes itself when it answers.
  auto* fetcher = new ContextFetcher(std::move(done), max_redirects,
                                     static_cast<int64_t>(limit));
  if (params.FindBool("ignoreHTTPSErrors").value_or(false)) {
    request->trusted_params = network::ResourceRequest::TrustedParams();
    request->trusted_params->url_loader_network_observer =
        fetcher->IgnoreCertificateErrors();
  }
  std::unique_ptr<network::SimpleURLLoader> loader =
      network::SimpleURLLoader::Create(std::move(request), kAnnotation);
  if (const std::string* post = params.FindString("postData")) {
    std::string bytes;
    if (!base::Base64Decode(*post, &bytes))
      bytes = *post;
    std::string type = "application/octet-stream";
    if (const base::ListValue* headers = params.FindList("headers")) {
      for (const base::Value& header : *headers) {
        const std::string* name =
            header.is_dict() ? header.GetDict().FindString("name") : nullptr;
        const std::string* value =
            header.is_dict() ? header.GetDict().FindString("value") : nullptr;
        if (name && value && base::EqualsCaseInsensitiveASCII(*name, "content-type"))
          type = *value;
      }
    }
    loader->AttachStringForUpload(bytes, type);
  }
  loader->SetAllowHttpErrorResults(true);
  const double timeout = params.FindDouble("timeout").value_or(30000);
  if (timeout > 0)
    loader->SetTimeoutDuration(base::Milliseconds(timeout));
  fetcher->Start(std::move(loader),
                 profile->GetDefaultStoragePartition()
                     ->GetURLLoaderFactoryForBrowserProcess()
                     .get());
}

void ContextEmulateMedia(content::WebContents* contents,
                         const base::DictValue& params,
                         ContextDone done) {
  blink::web_pref::WebPreferences prefs = contents->GetOrCreateWebPreferences();
  if (const std::string* scheme = params.FindString("colorScheme")) {
    prefs.preferred_color_scheme =
        *scheme == "dark" ? blink::mojom::PreferredColorScheme::kDark
                          : blink::mojom::PreferredColorScheme::kLight;
  }
  if (const std::string* motion = params.FindString("reducedMotion"))
    prefs.prefers_reduced_motion = *motion == "reduce";
  if (const std::string* contrast = params.FindString("contrast")) {
    prefs.preferred_contrast =
        *contrast == "more" ? blink::mojom::PreferredContrast::kMore
                            : blink::mojom::PreferredContrast::kNoPreference;
  }
  if (const std::string* forced = params.FindString("forcedColors"))
    prefs.in_forced_colors = *forced == "active";
  contents->SetWebPreferences(prefs);
  std::move(done).Run(base::Value(base::DictValue()), std::string());
}

}  // namespace zebkit
