#ifndef ZEBKIT_ENGINE_COOKIES_H_
#define ZEBKIT_ENGINE_COOKIES_H_

#include <memory>
#include <string>
#include <vector>

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
// The startup's cookie switches; false leaves the settings as they were.
bool PrepareCookieStorage(const std::vector<std::string>& args);
// As the engine starts: zebkit_set_cookie_storage() and the key are refused.
void LockCookieStorage();
void ConfigurePersistentCookies(content::BrowserContext* context,
                                bool in_memory,
                                const base::FilePath& partition,
                                network::mojom::NetworkContextParams* params);
}  // namespace zebkit
#endif  // ZEBKIT_ENGINE_COOKIES_H_
