// Persistent network cookies for a profile, using Chromium's encrypted store.
#include "zebkit/engine/cookies.h"

#include <algorithm>
#include <array>
#include <optional>
#include <utility>
#include <vector>

#include <cstdio>
#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/rand_util.h"
#include "base/strings/string_util.h"
#include "build/build_config.h"
#include "components/os_crypt/async/browser/key_provider.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/common/algorithm.mojom.h"
#include "content/public/browser/browser_context.h"
#include "services/network/public/cpp/cookie_encryption_provider_impl.h"
#include "services/network/public/cpp/transferable_directory.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "zebkit/engine/cookie_key_file.h"
#include "zebkit/engine/include/zebkit/zebkit.h"

#if defined(ZEBKIT_COOKIES_USE_DBUS)
#include "components/dbus/thread_linux/dbus_thread_linux.h"
#include "components/os_crypt/async/browser/freedesktop_secret_key_provider.h"
#endif

namespace zebkit {
namespace {
using KeyProvider = os_crypt_async::KeyProvider;
using KeyProviders = std::vector<std::pair<size_t, std::unique_ptr<KeyProvider>>>;

struct HostCookieKey {
  std::optional<std::array<uint8_t, 32>> key;
  bool locked = false;
  int mode = ZEBKIT_COOKIE_STORAGE_OS;
  std::string directory;
};
HostCookieKey& HostKey() {
  static base::NoDestructor<HostCookieKey> state;
  return *state;
}
class HostKeyProvider : public KeyProvider {
 public:
  explicit HostKeyProvider(const std::array<uint8_t, 32>& key) : key_(key) {}
  void GetKey(KeyCallback callback) override {
    std::move(callback).Run(
        "zbh", os_crypt_async::Encryptor::Key(
                   key_, os_crypt_async::mojom::Algorithm::kAES256GCM));
  }
  bool UseForEncryption() override { return true; }

 private:
  std::array<uint8_t, 32> key_;
};

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

bool PrepareCookieStorage(const std::vector<std::string>& args) {
  auto& state = HostKey();
  if (state.locked)
    return false;
  base::CommandLine command(args);
  if (command.HasSwitch("zebkit-cookie-storage")) {
    std::string mode = command.GetSwitchValueASCII("zebkit-cookie-storage");
    if (mode != "os" && mode != "file") {
      fprintf(stderr, "zebkit: unknown cookie storage mode\n");
      state.locked = true;
      return false;
    }
    std::string directory =
        command.HasSwitch("zebkit-cookie-key-directory")
            ? command.GetSwitchValuePath("zebkit-cookie-key-directory")
                  .AsUTF8Unsafe()
            : (mode == "file" ? state.directory : std::string());
    if (!zebkit_set_cookie_storage(
            mode == "file" ? ZEBKIT_COOKIE_STORAGE_FILE
                           : ZEBKIT_COOKIE_STORAGE_OS,
            directory.empty() ? nullptr : directory.c_str())) {
      fprintf(stderr, "zebkit: invalid cookie storage configuration\n");
      state.locked = true;
      return false;
    }
  } else if (command.HasSwitch("zebkit-cookie-key-directory")) {
    std::string directory =
        command.GetSwitchValuePath("zebkit-cookie-key-directory")
            .AsUTF8Unsafe();
    if (state.mode != ZEBKIT_COOKIE_STORAGE_FILE ||
        !zebkit_set_cookie_storage(ZEBKIT_COOKIE_STORAGE_FILE,
                                   directory.c_str())) {
      fprintf(stderr, "zebkit: key directory requires file cookie storage\n");
      state.locked = true;
      return false;
    }
  }
  state.locked = true;
  if (state.mode != ZEBKIT_COOKIE_STORAGE_FILE)
    return true;
  auto profile = command.GetSwitchValuePath("user-data-dir");
  if (profile.empty()) {
    fprintf(stderr,
            "zebkit: file cookie storage requires a profile directory\n");
    return false;
  }
  std::error_code ec;
  auto path = std::filesystem::absolute(
      std::filesystem::u8path(profile.AsUTF8Unsafe()), ec);
  cookie_keys::Key key;
  std::string error;
  bool loaded =
      !ec && cookie_keys::LoadOrCreate(
                 path, std::filesystem::u8path(state.directory),
                 [](std::span<uint8_t> bytes) {
                   base::RandBytes(base::span(bytes.data(), bytes.size()));
                   return true;
                 },
                 key, error);
  if (!loaded) {
    fprintf(stderr, "zebkit: cookie file storage unavailable: %s\n",
            error.c_str());
    return false;
  }
  state.key = key;
  return true;
}

void ConfigurePersistentCookies(content::BrowserContext* context,
                                bool in_memory,
                                const base::FilePath& partition,
                                network::mojom::NetworkContextParams* params) {
  if (in_memory || context->IsOffTheRecord() || context->GetPath().empty())
    return;
  static base::NoDestructor<std::unique_ptr<CookieCrypt>> crypt([] {
    std::unique_ptr<KeyProvider> key;
    if (HostKey().key)
      key = std::make_unique<HostKeyProvider>(*HostKey().key);
    else
      key = CreateCookieKeyProvider();
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
      base::FilePath(HostKey().key ? FILE_PATH_LITERAL("Cookies.host")
                                   : FILE_PATH_LITERAL("Cookies"));
  params->enable_encrypted_cookies = true;
  params->cookie_encryption_provider = (*crypt)->Bind();
  // Keep Chromium's session-cookie lifetime semantics. Persistent cookies
  // survive restart; cookies without an expiry remain scoped to this run.
}
}  // namespace zebkit

extern "C" bool zebkit_set_cookie_encryption_key(const uint8_t* key,
                                                 size_t size) {
  auto& state = zebkit::HostKey();
  if (state.locked || (key ? size != 32 : size != 0))
    return false;
  if (key) {
    std::array<uint8_t, 32> copy;
    std::copy_n(key, copy.size(), copy.begin());
    state.key = copy;
    state.mode = 2;
    state.directory.clear();
  } else {
    state.key.reset();
    state.mode = ZEBKIT_COOKIE_STORAGE_OS;
    state.directory.clear();
  }
  return true;
}

extern "C" bool zebkit_set_cookie_storage(zebkit_cookie_storage storage,
                                          const char* key_directory) {
  auto& state = zebkit::HostKey();
  if (state.locked || (storage != ZEBKIT_COOKIE_STORAGE_OS &&
                       storage != ZEBKIT_COOKIE_STORAGE_FILE))
    return false;
  std::string directory = key_directory ? key_directory : "";
  if ((!directory.empty() && storage == ZEBKIT_COOKIE_STORAGE_OS) ||
      !base::IsStringUTF8(directory))
    return false;
  if (!directory.empty() && !std::filesystem::u8path(directory).is_absolute())
    return false;
  state.mode = storage;
  state.directory = std::move(directory);
  state.key.reset();
  return true;
}
