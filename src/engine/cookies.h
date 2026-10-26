#ifndef ZEBKIT_ENGINE_COOKIES_H_
#define ZEBKIT_ENGINE_COOKIES_H_

#include <memory>

#include "base/files/file_path.h"
#include "services/network/public/mojom/network_context.mojom-forward.h"

namespace content {
class BrowserContext;
}
namespace os_crypt_async {
class KeyProvider;
}

namespace zebkit {
std::unique_ptr<os_crypt_async::KeyProvider> CreateCookieKeyProvider();
void ConfigurePersistentCookies(content::BrowserContext* context,
                                bool in_memory,
                                const base::FilePath& partition,
                                network::mojom::NetworkContextParams* params);
}  // namespace zebkit
#endif  // ZEBKIT_ENGINE_COOKIES_H_
