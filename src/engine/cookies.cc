// Persistent network cookies for a profile, using Chromium's encrypted store.
#include "zebkit/engine/cookies.h"

#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "build/build_config.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "content/public/browser/browser_context.h"
#include "services/network/public/cpp/cookie_encryption_provider_impl.h"
#include "services/network/public/cpp/transferable_directory.h"
#include "services/network/public/mojom/network_context.mojom.h"

#if defined(ZEBKIT_COOKIES_USE_DBUS)
#include "components/dbus/thread_linux/dbus_thread_linux.h"
#include "components/os_crypt/async/browser/freedesktop_secret_key_provider.h"
#endif

namespace zebkit {
namespace {
using KeyProvider = os_crypt_async::KeyProvider;
using KeyProviders = std::vector<std::pair<size_t, std::unique_ptr<KeyProvider>>>;

class CookieCrypt {
 public:
  explicit CookieCrypt(std::unique_ptr<os_crypt_async::KeyProvider> key)
      : crypt_(Providers(std::move(key))), provider_(&crypt_) {}
  auto Bind() { return provider_.BindNewRemote(); }

 private:
  static KeyProviders Providers(std::unique_ptr<KeyProvider> key) {
    KeyProviders providers;
    providers.emplace_back(0, std::move(key));
    return providers;
  }
  os_crypt_async::OSCryptAsync crypt_;
  CookieEncryptionProviderImpl provider_;
};

#if defined(ZEBKIT_COOKIES_USE_DBUS)
class DesktopKeyProvider : public os_crypt_async::KeyProvider {
 public:
  DesktopKeyProvider()
      : backend_(
            base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
                "password-store"),
            "Zebkit " + base::CommandLine::ForCurrentProcess()
                            ->GetProgram().BaseName().value(),
            dbus_thread_linux::GetSharedSessionBus()) {}
  void GetKey(KeyCallback callback) override {
    backend_.GetKey(base::BindOnce(
        [](KeyCallback callback, const std::string& tag,
           base::expected<os_crypt_async::Encryptor::Key, KeyError> key) {
          // Do not persist authentication cookies under the well-known
          // "peanuts" fallback key when the desktop keyring is unavailable.
          if (tag == "v10")
            key = base::unexpected(KeyError::kTemporarilyUnavailable);
          std::move(callback).Run(tag, std::move(key));
        },
        std::move(callback)));
  }
  bool UseForEncryption() override { return true; }

 private:
  os_crypt_async::FreedesktopSecretKeyProvider backend_;
};
#endif
}  // namespace

#if !BUILDFLAG(IS_MAC)
std::unique_ptr<os_crypt_async::KeyProvider> CreateCookieKeyProvider() {
#if defined(ZEBKIT_COOKIES_USE_DBUS)
  return std::make_unique<DesktopKeyProvider>();
#else
  return nullptr;
#endif
}
#endif

void ConfigurePersistentCookies(content::BrowserContext* context,
                                bool in_memory,
                                const base::FilePath& partition,
                                network::mojom::NetworkContextParams* params) {
  if (in_memory || context->IsOffTheRecord() || context->GetPath().empty())
    return;
  static base::NoDestructor<std::unique_ptr<CookieCrypt>> crypt([] {
    auto key = CreateCookieKeyProvider();
    return key ? std::make_unique<CookieCrypt>(std::move(key)) : nullptr;
  }());
  if (!*crypt) {
    LOG(ERROR) << "No OS cookie key provider; cookies remain in memory";
    return;
  }
  if (!params->file_paths)
    params->file_paths = network::mojom::NetworkContextFilePaths::New();
  params->file_paths->data_directory = network::TransferableDirectory(
      context->GetPath().Append(partition).AppendASCII("Network"));
  params->file_paths->cookie_database_name =
      base::FilePath(FILE_PATH_LITERAL("Cookies"));
  params->enable_encrypted_cookies = true;
  params->cookie_encryption_provider = (*crypt)->Bind();
  // Keep Chromium's session-cookie lifetime semantics. Persistent cookies
  // survive restart; cookies without an expiry remain scoped to this run.
}
}  // namespace zebkit
