// Network requests and responses (network_hooks.cc): docs/HOOKS.md.

#ifndef ZEBKIT_ENGINE_NETWORK_HOOKS_H_
#define ZEBKIT_ENGINE_NETWORK_HOOKS_H_

#include <optional>
#include <string>

#include "content/public/browser/content_browser_client.h"

class GURL;
namespace content {
class BrowserContext;
class RenderFrameHost;
}
namespace network {
class URLLoaderFactoryBuilder;
}
struct zebkit_view;

namespace zebkit {

void InstallNetworkHooks();
void StopNetworkHooks();
void CancelNetworkHookRequests(zebkit_view* view);
// What a factory is for: a frame's, a dedicated or shared worker's, a service worker's.
enum class NetworkHookFactory { kFrame, kWorker, kServiceWorker };
// From WillCreateURLLoaderFactory(), ahead of automation's and CDP's. @frame is null
// for shared and service workers, which a profile's policy decides.
void InterceptNetworkHookFactory(content::BrowserContext* profile, content::RenderFrameHost* frame,
                                 NetworkHookFactory kind, network::URLLoaderFactoryBuilder& builder,
                                 mojo::PendingRemote<network::mojom::TrustedURLLoaderHeaderClient>* header_client);
// From WillInterceptWebSocket() and CreateWebSocket(); @frame as above.
bool WillInterceptNetworkHookWebSocket(content::RenderFrameHost* frame);
void CreateNetworkHookWebSocket(content::RenderFrameHost* frame,
                                content::ContentBrowserClient::WebSocketFactory factory, const GURL& url,
                                const std::optional<std::string>& user_agent,
                                mojo::PendingRemote<network::mojom::WebSocketHandshakeClient> client,
                                mojo::PendingRemote<network::mojom::TrustedHeaderClient> header_client);

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_NETWORK_HOOKS_H_
