// Client hints as Chromium sends them: Sec-CH-UA, Sec-CH-UA-Mobile and
// Sec-CH-UA-Platform with every request, and the hints a site asks for
// (Accept-CH) from then on. The content shell has no delegate for them
// outside its web tests, and without one Chromium sends none, which no
// browser does: sites take a Chromium without them for an altered one.
// What Chrome's headless shell has (headless_client_hints_controller_delegate.cc),
// with zebkit's user agent metadata.

#include <map>
#include <vector>

#include "base/no_destructor.h"
#include "components/embedder_support/user_agent_utils.h"
#include "content/public/browser/client_hints_controller_delegate.h"
#include "content/shell/browser/shell_browser_context.h"
#include "content/shell/browser/shell_content_browser_client.h"
#include "services/network/public/cpp/is_potentially_trustworthy.h"
#include "services/network/public/mojom/web_client_hints_types.mojom.h"
#include "third_party/blink/public/common/client_hints/enabled_client_hints.h"
#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"
#include "ui/gfx/geometry/size.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "zebkit/engine/engine.h"

namespace zebkit {

namespace {

// Network quality hints, which Chrome no longer sends either.
constexpr network::mojom::WebClientHintsType kNotSent[] = {
    network::mojom::WebClientHintsType::kRtt_DEPRECATED,
    network::mojom::WebClientHintsType::kDownlink_DEPRECATED,
    network::mojom::WebClientHintsType::kEct_DEPRECATED,
};

class ClientHints : public content::ClientHintsControllerDelegate {
 public:
  network::NetworkQualityTracker* GetNetworkQualityTracker() override {
    return nullptr;
  }

  void GetAllowedClientHintsFromSource(
      const url::Origin& origin,
      blink::EnabledClientHints* client_hints) override {
    if (auto it = persisted_.find(origin); it != persisted_.end())
      *client_hints = it->second;
    for (auto hint : additional_)
      client_hints->SetIsEnabled(hint, true);
    for (auto hint : kNotSent)
      client_hints->SetIsEnabled(hint, false);
  }

  bool IsJavaScriptAllowed(const GURL& url,
                           content::RenderFrameHost* parent) override {
    return true;
  }

  blink::UserAgentMetadata GetUserAgentMetadata() override {
    return embedder_support::GetUserAgentMetadata();
  }

  // A site's Accept-CH, kept for its origin while the engine runs, on secure
  // origins only, as Chrome keeps it.
  void PersistClientHints(
      const url::Origin& origin,
      content::RenderFrameHost* parent,
      const std::vector<network::mojom::WebClientHintsType>& hints) override {
    const GURL url = origin.GetURL();
    if (!url.is_valid() || !network::IsUrlPotentiallyTrustworthy(url))
      return;
    blink::EnabledClientHints enabled;
    for (auto hint : hints)
      enabled.SetIsEnabled(hint, true);
    persisted_[origin] = enabled;
  }

  void SetAdditionalClientHints(
      const std::vector<network::mojom::WebClientHintsType>& hints) override {
    additional_ = hints;
  }

  void ClearAdditionalClientHints() override { additional_.clear(); }

  void SetMostRecentMainFrameViewportSize(const gfx::Size& size) override {
    viewport_ = size;
  }

  gfx::Size GetMostRecentMainFrameViewportSize() override { return viewport_; }

 private:
  std::map<url::Origin, blink::EnabledClientHints> persisted_;
  std::vector<network::mojom::WebClientHintsType> additional_;
  gfx::Size viewport_;
};

}  // namespace

void InstallClientHintsFor(content::BrowserContext* profile) {
  static base::NoDestructor<ClientHints> contexts;
  static_cast<content::ShellBrowserContext*>(profile)
      ->set_client_hints_controller_delegate(contexts.get());
}

void InstallClientHints() {
  static base::NoDestructor<ClientHints> regular;
  static base::NoDestructor<ClientHints> off_the_record;
  auto* client = content::ShellContentBrowserClient::Get();
  if (client->browser_context())
    client->browser_context()->set_client_hints_controller_delegate(
        regular.get());
  if (client->off_the_record_browser_context())
    client->off_the_record_browser_context()
        ->set_client_hints_controller_delegate(off_the_record.get());
}

}  // namespace zebkit
