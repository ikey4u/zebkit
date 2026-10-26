// A host-scoped cookie key, protected by the user's macOS Keychain.
#include "zebkit/engine/cookies.h"

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/rand_util.h"
#include "base/strings/sys_string_conversions.h"
#include "base/task/thread_pool.h"
#include "components/os_crypt/async/browser/key_provider.h"
#include "components/os_crypt/async/common/algorithm.mojom.h"
#include "crypto/apple/keychain_v2.h"

namespace zebkit {
namespace {
using Key = os_crypt_async::Encryptor::Key;
using KeyError = os_crypt_async::KeyProvider::KeyError;

base::expected<Key, KeyError> ReadKey(const std::string& identity) {
  auto& keychain = crypto::apple::KeychainV2::GetInstance();
  const std::string service = "Zebkit Cookies: " + identity;
  auto bytes = keychain.FindGenericPassword(service, identity);
  if (!bytes.has_value() && bytes.error() == errSecItemNotFound) {
    auto generated = base::RandBytesAsVector(Key::kAES256GCMKeySize);
    const OSStatus status =
        keychain.AddGenericPassword(service, identity, generated);
    if (status == errSecSuccess)
      bytes = std::move(generated);
    else if (status == errSecDuplicateItem)
      bytes = keychain.FindGenericPassword(service, identity);
  }
  if (!bytes.has_value() || bytes->size() != Key::kAES256GCMKeySize)
    return base::unexpected(KeyError::kTemporarilyUnavailable);
  return Key(*bytes, os_crypt_async::mojom::Algorithm::kAES256GCM);
}

class CookieKeyProvider : public os_crypt_async::KeyProvider {
 public:
  CookieKeyProvider() {
    @autoreleasepool {
      identity_ = base::SysNSStringToUTF8([[NSBundle mainBundle] bundleIdentifier]);
    }
    if (identity_.empty())
      identity_ = base::CommandLine::ForCurrentProcess()
                      ->GetProgram().BaseName().value();
  }
  void GetKey(KeyCallback callback) override {
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
        base::BindOnce(&ReadKey, identity_),
        base::BindOnce(std::move(callback), "zb1"));
  }
  bool UseForEncryption() override { return true; }

 private:
  std::string identity_;
};
}  // namespace

std::unique_ptr<os_crypt_async::KeyProvider> CreateCookieKeyProvider() {
  return std::make_unique<CookieKeyProvider>();
}
}  // namespace zebkit
