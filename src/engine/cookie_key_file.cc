// Built-in private-file cookie key storage; shared by every Zebkit binding.
#include "cookie_key_file.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <system_error>
#include <vector>

#if defined(_WIN32)
// windows.h must precede sddl.h.
// clang-format off
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
// clang-format on
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace zebkit::cookie_keys {
namespace {
namespace fs = std::filesystem;
constexpr std::string_view kHeader = "Zebkit Cookie Key v1\n";
constexpr size_t kSize = kHeader.size() + Key{}.size();
enum class ReadResult { kFound, kMissing, kError };
bool Fail(std::string& error, const char* message) {
  error = message;
  return false;
}

#if defined(_WIN32)
class Descriptor {
 public:
  Descriptor() {
    ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:P(A;;FA;;;OW)", SDDL_REVISION_1, &value_, nullptr);
  }
  explicit Descriptor(PSECURITY_DESCRIPTOR value) : value_(value) {}
  Descriptor(const Descriptor&) = delete;
  Descriptor& operator=(const Descriptor&) = delete;
  ~Descriptor() {
    if (value_)
      LocalFree(value_);
  }
  PSECURITY_DESCRIPTOR get() const { return value_; }

 private:
  PSECURITY_DESCRIPTOR value_ = nullptr;
};
class File {
 public:
  explicit File(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
  ~File() {
    if (valid())
      CloseHandle(value_);
  }
  File(const File&) = delete;
  File& operator=(const File&) = delete;
  bool valid() const { return value_ != INVALID_HANDLE_VALUE; }
  HANDLE get() const { return value_; }

 private:
  HANDLE value_;
};
bool PrivatePermissions(HANDLE handle) {
  PSID owner = nullptr;
  PACL acl = nullptr;
  PSECURITY_DESCRIPTOR raw = nullptr;
  if (GetSecurityInfo(handle, SE_FILE_OBJECT,
                      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                      &owner, nullptr, &acl, nullptr, &raw) != ERROR_SUCCESS)
    return false;
  Descriptor descriptor(raw);
  SECURITY_DESCRIPTOR_CONTROL control;
  DWORD revision;
  if (!acl || !owner ||
      !GetSecurityDescriptorControl(raw, &control, &revision) ||
      !(control & SE_DACL_PROTECTED))
    return false;
  HANDLE token_handle;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token_handle))
    return false;
  File token(token_handle);
  DWORD size = 0;
  GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
  if (!size)
    return false;
  std::vector<uint8_t> user(size);
  if (!GetTokenInformation(token.get(), TokenUser, user.data(), size, &size) ||
      !EqualSid(owner, reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid))
    return false;
  bool owner_allowed = false;
  for (DWORD i = 0; i < acl->AceCount; ++i) {
    void* ace = nullptr;
    if (!GetAce(acl, i, &ace))
      return false;
    auto* header = static_cast<ACE_HEADER*>(ace);
    if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
      auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
      PSID sid = &allowed->SidStart;
      if (!IsValidSid(sid) || (!EqualSid(sid, owner) &&
                               !IsWellKnownSid(sid, WinCreatorOwnerRightsSid)))
        return false;
      owner_allowed = true;
    } else if (header->AceType != ACCESS_DENIED_ACE_TYPE) {
      return false;
    }
  }
  return owner_allowed;
}
bool PrivateDirectory(const fs::path& path, std::string& error) {
  Descriptor descriptor;
  if (!descriptor.get())
    return Fail(error, "Cannot create private key permissions");
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor.get(), FALSE};
  if (!CreateDirectoryW(path.c_str(), &attributes) &&
      GetLastError() != ERROR_ALREADY_EXISTS)
    return Fail(error, "Cannot create private key directory");
  File directory(CreateFileW(
      path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  BY_HANDLE_FILE_INFORMATION info;
  if (!directory.valid() ||
      !GetFileInformationByHandle(directory.get(), &info) ||
      !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
      (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
      !PrivatePermissions(directory.get()))
    return Fail(error,
                "Key directory must be owned and private; existing ACLs are "
                "not modified");
  return true;
}
HANDLE OpenPrivate(const fs::path& path, bool create) {
  Descriptor descriptor;
  if (!descriptor.get())
    return INVALID_HANDLE_VALUE;
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor.get(), FALSE};
  HANDLE handle = CreateFileW(
      path.c_str(), GENERIC_READ | (create ? GENERIC_WRITE : 0),
      FILE_SHARE_READ, &attributes, create ? CREATE_NEW : OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE)
    return handle;
  BY_HANDLE_FILE_INFORMATION info;
  if (!GetFileInformationByHandle(handle, &info) ||
      GetFileType(handle) != FILE_TYPE_DISK ||
      (info.dwFileAttributes &
       (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
      info.nNumberOfLinks != 1 || !PrivatePermissions(handle)) {
    CloseHandle(handle);
    SetLastError(ERROR_ACCESS_DENIED);
    return INVALID_HANDLE_VALUE;
  }
  return handle;
}
ReadResult Read(const fs::path& path, Key& key, std::string& error) {
  File file(OpenPrivate(path, false));
  if (!file.valid()) {
    DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
      return ReadResult::kMissing;
    Fail(error,
         "Cannot open private cookie key; permissions or file type invalid");
    return ReadResult::kError;
  }
  std::array<uint8_t, kSize + 1> bytes{};
  DWORD count = 0;
  if (!ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()),
                &count, nullptr) ||
      count != kSize ||
      std::memcmp(bytes.data(), kHeader.data(), kHeader.size())) {
    Fail(error, "Invalid cookie key file; it will not be replaced");
    return ReadResult::kError;
  }
  std::copy_n(bytes.begin() + kHeader.size(), key.size(), key.begin());
  return ReadResult::kFound;
}
bool WritePrivate(const fs::path& path,
                  const Key& key,
                  std::string& error,
                  bool& created) {
  created = false;
  File file(OpenPrivate(path, true));
  if (!file.valid())
    return Fail(error, "Cannot create temporary cookie key");
  created = true;
  DWORD written = 0;
  if (!WriteFile(file.get(), kHeader.data(), static_cast<DWORD>(kHeader.size()),
                 &written, nullptr) ||
      written != kHeader.size() ||
      !WriteFile(file.get(), key.data(), static_cast<DWORD>(key.size()),
                 &written, nullptr) ||
      written != key.size() || !FlushFileBuffers(file.get())) {
    return Fail(error, "Cannot flush temporary cookie key");
  }
  return true;
}
#else
class File {
 public:
  explicit File(int value = -1) : value_(value) {}
  ~File() {
    if (valid())
      close(value_);
  }
  File(const File&) = delete;
  File& operator=(const File&) = delete;
  bool valid() const { return value_ >= 0; }
  int get() const { return value_; }

 private:
  int value_;
};
bool PrivateDirectory(const fs::path& path, std::string& error) {
  if (mkdir(path.c_str(), 0700) && errno != EEXIST)
    return Fail(error, "Cannot create private key directory");
  struct stat info;
  if (lstat(path.c_str(), &info) || !S_ISDIR(info.st_mode) ||
      info.st_uid != geteuid() || (info.st_mode & 0077))
    return Fail(
        error,
        "Key directory must be owned, private (0700), and not a symbolic link");
  return true;
}
int OpenPrivate(const fs::path& path, bool create) {
  int fd = open(path.c_str(),
                (create ? O_RDWR | O_CREAT | O_EXCL : O_RDONLY) | O_NOFOLLOW |
                    O_NONBLOCK | O_CLOEXEC,
                0600);
  if (fd < 0)
    return fd;
  struct stat info;
  if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_uid != geteuid() ||
      (info.st_mode & 0077) || info.st_nlink != 1) {
    close(fd);
    errno = EACCES;
    return -1;
  }
  return fd;
}
ReadResult Read(const fs::path& path, Key& key, std::string& error) {
  File file(OpenPrivate(path, false));
  if (!file.valid()) {
    if (errno == ENOENT)
      return ReadResult::kMissing;
    Fail(error,
         "Cannot open private cookie key; permissions or file type invalid");
    return ReadResult::kError;
  }
  std::array<uint8_t, kSize + 1> bytes{};
  size_t count = 0;
  while (count < bytes.size()) {
    ssize_t n = read(file.get(), bytes.data() + count, bytes.size() - count);
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0) {
      Fail(error, "Cannot read private cookie key");
      return ReadResult::kError;
    }
    if (!n)
      break;
    count += static_cast<size_t>(n);
  }
  if (count != kSize ||
      std::memcmp(bytes.data(), kHeader.data(), kHeader.size())) {
    Fail(error, "Invalid cookie key file; it will not be replaced");
    return ReadResult::kError;
  }
  std::copy_n(bytes.begin() + kHeader.size(), key.size(), key.begin());
  return ReadResult::kFound;
}
bool WritePrivate(const fs::path& path,
                  const Key& key,
                  std::string& error,
                  bool& created) {
  created = false;
  File file(OpenPrivate(path, true));
  if (!file.valid())
    return Fail(error, "Cannot create temporary cookie key");
  created = true;
  std::array<uint8_t, kSize> bytes;
  std::copy(kHeader.begin(), kHeader.end(), bytes.begin());
  std::copy(key.begin(), key.end(), bytes.begin() + kHeader.size());
  size_t count = 0;
  while (count < bytes.size()) {
    ssize_t n = write(file.get(), bytes.data() + count, bytes.size() - count);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return Fail(error, "Cannot write temporary cookie key");
    count += static_cast<size_t>(n);
  }
  if (fsync(file.get()))
    return Fail(error, "Cannot flush temporary cookie key");
  return true;
}
#endif

bool HasCookies(const fs::path& profile, bool& found, std::string& error) {
  std::error_code ec;
  fs::recursive_directory_iterator it(profile, ec), end;
  while (!ec && it != end) {
    if (it.depth() > 64)
      return Fail(error, "Profile directory nesting exceeds limit");
    if (it->path().filename() == "Cookies.host") {
      found = true;
      return true;
    }
    it.increment(ec);
  }
  return ec ? Fail(error, "Cannot check profile for existing host cookies")
            : true;
}
}  // namespace

bool LoadOrCreate(const fs::path& profile,
                  const fs::path& directory,
                  Random random,
                  Key& key,
                  std::string& error) {
  error.clear();
  if (profile.empty() || !profile.is_absolute() ||
      (!directory.empty() && !directory.is_absolute()))
    return Fail(error,
                "Cookie storage needs an absolute profile and key directory");
  std::error_code ec;
  fs::create_directories(profile, ec);
  if (ec)
    return Fail(error, "Cannot create cookie profile directory");
  fs::path folder = directory.empty() ? profile / "Secrets" : directory;
  fs::create_directories(folder.parent_path(), ec);
  if (ec || !PrivateDirectory(folder, error))
    return false;
  fs::path path = folder / "cookie.key";
  ReadResult read = Read(path, key, error);
  if (read != ReadResult::kMissing)
    return read == ReadResult::kFound;
  bool found = false;
  if (!HasCookies(profile, found, error))
    return false;
  if (found)
    return Fail(
        error,
        "Cookie key missing; restore cookie.key from the same profile backup");
  if (!random || !random(key))
    return Fail(error, "Cannot generate cookie key");
  std::array<uint8_t, 16> nonce;
  if (!random(nonce))
    return Fail(error, "Cannot generate temporary key filename");
  std::string name = ".cookie-key-";
  for (uint8_t byte : nonce) {
    constexpr char hex[] = "0123456789abcdef";
    name += hex[byte >> 4];
    name += hex[byte & 15];
  }
  fs::path temporary = folder / name;
  // CREATE_NEW / O_EXCL reserves the temporary without replacing any file.
  bool created = false;
  if (!WritePrivate(temporary, key, error, created)) {
    if (created) {
      std::error_code ignored;
      fs::remove(temporary, ignored);
    }
    return false;
  }
  fs::create_hard_link(temporary, path, ec);
  bool published = !ec || ec == std::errc::file_exists;
  std::error_code ignored;
  fs::remove(temporary, ignored);
  if (!published)
    return Fail(error, "Cannot atomically publish cookie key");
#if !defined(_WIN32)
  File folder_fd(
      open(folder.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (!folder_fd.valid() || fsync(folder_fd.get()))
    return Fail(error, "Cannot flush cookie key directory");
#endif
  return Read(path, key, error) == ReadResult::kFound;
}
}  // namespace zebkit::cookie_keys
