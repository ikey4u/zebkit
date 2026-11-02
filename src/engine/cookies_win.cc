// A host-scoped cookie key, protected by the user's DPAPI: the key is a file
// beside the profile that only this Windows user can decrypt. cookies_mac.mm
// is macOS's.
#include <windows.h>

#include <dpapi.h>

#include <string>
#include <vector>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/rand_util.h"
#include "base/task/thread_pool.h"
#include "components/os_crypt/async/browser/key_provider.h"
#include "components/os_crypt/async/common/algorithm.mojom.h"
#include "zebkit/engine/profile.h"
#include "zebkit/engine/cookies.h"
#include "zebkit/engine/path_utf8.h"

namespace zebkit {
namespace {
using Key = os_crypt_async::Encryptor::Key;
using KeyError = os_crypt_async::KeyProvider::KeyError;

std::optional<std::vector<uint8_t>> Unprotect(const std::string& sealed) {
  DATA_BLOB in = {static_cast<DWORD>(sealed.size()),
                  reinterpret_cast<BYTE*>(const_cast<char*>(sealed.data()))};
  DATA_BLOB out = {};
  if (!::CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out)) {
    return std::nullopt;
  }
  std::vector<uint8_t> key(out.pbData, out.pbData + out.cbData);
  ::SecureZeroMemory(out.pbData, out.cbData);
  ::LocalFree(out.pbData);
  return key;
}

std::optional<std::string> Protect(const std::vector<uint8_t>& key) {
  DATA_BLOB in = {static_cast<DWORD>(key.size()),
                  const_cast<BYTE*>(key.data())};
  DATA_BLOB out = {};
  if (!::CryptProtectData(&in, L"Zebkit Cookies", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) {
    return std::nullopt;
  }
  std::string sealed(reinterpret_cast<char*>(out.pbData), out.cbData);
  ::LocalFree(out.pbData);
  return sealed;
}

base::expected<Key, KeyError> ReadKey(const base::FilePath& path) {
  std::string sealed;
  if (base::ReadFileToString(path, &sealed)) {
    auto key = Unprotect(sealed);
    // Another user's, or a file from another machine: not to be overwritten,
    // that would lose what it protects without saying so.
    if (!key || key->size() != Key::kAES256GCMKeySize)
      return base::unexpected(KeyError::kPermanentlyUnavailable);
    return Key(*key, os_crypt_async::mojom::Algorithm::kAES256GCM);
  }
  auto generated = base::RandBytesAsVector(Key::kAES256GCMKeySize);
  auto protected_key = Protect(generated);
  if (!protected_key || !base::CreateDirectory(path.DirName())) {
    LOG(ERROR) << "Cannot protect the cookie key for " << path;
    return base::unexpected(KeyError::kTemporarilyUnavailable);
  }
  // Not every file system replaces a file atomically (a VM's shared folder does not).
  if (!base::ImportantFileWriter::WriteFileAtomically(path, *protected_key) &&
      !base::WriteFile(path, *protected_key)) {
    LOG(ERROR) << "Cannot write the cookie key to " << path;
    return base::unexpected(KeyError::kTemporarilyUnavailable);
  }
  return Key(generated, os_crypt_async::mojom::Algorithm::kAES256GCM);
}

class CookieKeyProvider : public os_crypt_async::KeyProvider {
 public:
  void GetKey(KeyCallback callback) override {
    // The profile the application named, else the engine's default one.
    const base::CommandLine& command = *base::CommandLine::ForCurrentProcess();
    base::FilePath directory = command.GetSwitchValuePath("user-data-dir");
    if (directory.empty())
      directory = command.GetSwitchValuePath(kUserDataDirSwitch);
    directory = base::MakeAbsoluteFilePath(directory.empty() ? base::FilePath(FILE_PATH_LITERAL(".")) : directory);
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
        base::BindOnce(&ReadKey,
                       directory.Append(FILE_PATH_LITERAL("Cookie Key"))),
        base::BindOnce(std::move(callback), "zb1"));
  }
  bool UseForEncryption() override { return true; }
};
}  // namespace

std::unique_ptr<os_crypt_async::KeyProvider> CreateCookieKeyProvider() {
  return std::make_unique<CookieKeyProvider>();
}
}  // namespace zebkit
