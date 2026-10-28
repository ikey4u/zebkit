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
#include "base/time/time.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_constants.h"
#include "net/cookies/cookie_inclusion_status.h"
#include "net/cookies/cookie_options.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/cookie_manager.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "third_party/blink/public/common/web_preferences/web_preferences.h"
#include "third_party/blink/public/mojom/css/preferred_color_scheme.mojom-shared.h"
#include "third_party/blink/public/mojom/css/preferred_contrast.mojom-shared.h"
#include "url/gurl.h"

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
  if (max_redirects == 0)
    request->redirect_mode = network::mojom::RedirectMode::kManual;
  request->credentials_mode = network::mojom::CredentialsMode::kInclude;
  request->site_for_cookies = net::SiteForCookies::FromUrl(target);
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
  network::SimpleURLLoader* raw = loader.get();
  raw->DownloadToStringOfUnboundedSizeUntilCrashAndDie(
      profile->GetDefaultStoragePartition()
          ->GetURLLoaderFactoryForBrowserProcess()
          .get(),
      base::BindOnce(
          [](std::unique_ptr<network::SimpleURLLoader> loader, ContextDone done,
             std::optional<std::string> body) {
            const network::mojom::URLResponseHead* head = loader->ResponseInfo();
            if (!head || !head->headers) {
              Fail(std::move(done), net::ErrorToString(loader->NetError()));
              return;
            }
            base::DictValue response;
            response.Set("url", loader->GetFinalURL().spec());
            response.Set("status", head->headers->response_code());
            response.Set("statusText", head->headers->GetStatusText());
            base::ListValue headers;
            size_t iterator = 0;
            std::string name, value;
            while (head->headers->EnumerateHeaderLines(&iterator, &name, &value)) {
              base::DictValue header;
              header.Set("name", name);
              header.Set("value", value);
              headers.Append(std::move(header));
            }
            response.Set("headers", std::move(headers));
            response.Set("body", base::Base64Encode(body.value_or("")));
            base::DictValue answer;
            answer.Set("response", std::move(response));
            std::move(done).Run(base::Value(std::move(answer)), std::string());
          },
          std::move(loader), std::move(done)));
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
