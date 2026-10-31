// Network requests and responses (network_hooks.cc): docs/ROADMAP_HOOKS.md.

#ifndef ZEBKIT_ENGINE_NETWORK_HOOKS_H_
#define ZEBKIT_ENGINE_NETWORK_HOOKS_H_

namespace content {
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
// From WillCreateURLLoaderFactory(), ahead of automation's and CDP's.
void InterceptNetworkHookFactory(content::RenderFrameHost* frame, bool worker,
                                 network::URLLoaderFactoryBuilder& builder);

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_NETWORK_HOOKS_H_
