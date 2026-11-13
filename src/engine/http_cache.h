// What a profile's network keeps between runs: the HTTP cache, the servers'
// properties (HTTP/2, alternative services) and transport security state
// (HSTS), as Chrome keeps them (docs/CONNECTION_REUSE.md, step 1).

#ifndef ZEBKIT_ENGINE_HTTP_CACHE_H_
#define ZEBKIT_ENGINE_HTTP_CACHE_H_

#include "base/files/file_path.h"
#include "services/network/public/mojom/network_context.mojom-forward.h"

namespace content {
class BrowserContext;
}

namespace zebkit {
// For a profile on disk, under <profile>/<partition>: Cache, and Network's
// "Network Persistent State" and "TransportSecurity"; for one in memory, and
// a zebkit_context, in memory. The startup setting httpCache
// (--zebkit-http-cache=disk|memory|off, --zebkit-http-cache-size=BYTES)
// decides the cache.
void ConfigureNetworkPersistence(content::BrowserContext* context,
                                 bool in_memory,
                                 const base::FilePath& partition,
                                 network::mojom::NetworkContextParams* params);
}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_HTTP_CACHE_H_
