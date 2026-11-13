// Connections opened ahead of the requests that use them
// (docs/CONNECTION_REUSE.md, step 2): zebkit_preconnect()
// (<zebkit/services.h>) and the startup setting "preconnect".

#include "zebkit/engine/preconnect.h"

#include <string>
#include <vector>

#include "base/command_line.h"
#include "base/logging.h"
#include "base/strings/string_split.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/storage_partition.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "net/base/network_anonymization_key.h"
#include "net/base/schemeful_site.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/constants.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "url/gurl.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/include/zebkit/services.h"
#include "zebkit/engine/window.h"

namespace zebkit {

void Preconnect(content::BrowserContext* profile, const GURL& url, int count) {
  static constexpr net::NetworkTrafficAnnotationTag kAnnotation =
      net::DefineNetworkTrafficAnnotation("zebkit_preconnect", R"(
        semantics {
          sender: "zebkit embedding application"
          description: "Connections opened before the application's requests or pages use them."
          trigger: "The application calls zebkit_preconnect() or lists the origin in its startup settings."
          data: "None: a connection and its TLS handshake, no request."
          destination: OTHER
        }
        policy {
          cookies_allowed: NO
          setting: "Up to the application."
          policy_exception_justification: "Made by the application."
        })");
  // What a top-level navigation to @url uses (navigation_request.cc): the
  // sockets match it, also with connections partitioned by site.
  profile->GetDefaultStoragePartition()->GetNetworkContext()->PreconnectSockets(
      count, url, network::mojom::CredentialsMode::kInclude,
      net::NetworkAnonymizationKey::CreateSameSite(net::SchemefulSite(url)),
      network::GetNoOpNetworkRestrictionsId(), net::MutableNetworkTrafficAnnotationTag(kAnnotation),
      /*keepalive_config=*/std::nullopt, mojo::NullRemote());
}

void PreconnectAtStartup(content::BrowserContext* profile) {
  // startup_settings.cc's: URLs, separated by spaces.
  const std::string list =
      base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII("zebkit-preconnect");
  for (std::string_view text : base::SplitStringPiece(list, " ", base::TRIM_WHITESPACE,
                                                      base::SPLIT_WANT_NONEMPTY)) {
    const GURL url(text);
    if (url.is_valid() && url.SchemeIsHTTPOrHTTPS() && url.has_host())
      Preconnect(profile, url, 1);
    else
      LOG(ERROR) << "preconnect: not an http or https URL: " << text;
  }
}

}  // namespace zebkit

extern "C" bool zebkit_preconnect(zebkit_context* context, const char* url, int count) {
  if (!zebkit::CurrentlyOnEngineThread() || zebkit::Window::IsShutDown() || !url)
    return false;
  if ((context && !context->profile) || count < 1 || count > 6)
    return false;
  const GURL parsed(url);
  if (!parsed.is_valid() || !parsed.SchemeIsHTTPOrHTTPS() || !parsed.has_host())
    return false;
  content::BrowserContext* profile = zebkit::ProfileOf(context);
  if (!profile)
    return false;
  zebkit::Preconnect(profile, parsed, count);
  return true;
}
