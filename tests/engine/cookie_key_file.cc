// Standalone backend contract tests; also compile/link for Linux and Windows.
#include "../../src/engine/cookie_key_file.h"
#include <cassert>
#include <fstream>
#include <iostream>
#if !defined(_WIN32)
#include <sys/stat.h>
#endif
namespace fs = std::filesystem;
using namespace zebkit::cookie_keys;
static bool RandomBytes(std::span<uint8_t> out) {
  for (size_t i = 0; i < out.size(); ++i) out[i] = static_cast<uint8_t>(i + 1);
  return true;
}
static bool NoRandom(std::span<uint8_t>) { assert(false && "existing key must be reused"); return false; }
static void Write(const fs::path& path, const std::string& data) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc); file << data;
}
static std::string Read(const fs::path& path) {
  std::ifstream file(path, std::ios::binary); return std::string(std::istreambuf_iterator<char>(file), {});
}
int main(int argc, char** argv) {
  assert(argc == 2);
  fs::path root = fs::absolute(fs::path(argv[1]));
  assert(!fs::exists(root)); fs::create_directories(root);
  Key key, previous; std::string error;
  auto profile = root / "profile";
  assert(LoadOrCreate(profile, {}, RandomBytes, key, error));
  previous = key;
  auto file = profile / "Secrets/cookie.key";
  assert(Read(file).size() == 53);
  assert(LoadOrCreate(profile, {}, NoRandom, key, error)); assert(key == previous);
  // An already deployed host's file uses the exact same format.
  auto legacy = root / "legacy";
  assert(LoadOrCreate(legacy, {}, RandomBytes, key, error));
  std::string existing = "Zebkit Cookie Key v1\n" + std::string(32, 'q');
  Write(legacy / "Secrets/cookie.key", existing);
  assert(LoadOrCreate(legacy, {}, NoRandom, key, error));
  assert(key[0] == 'q' && Read(legacy / "Secrets/cookie.key") == existing);
  fs::create_directories(profile / "partition/Network");
  Write(profile / "partition/Network/Cookies.host", "owned synthetic database");
  fs::remove(file);
  assert(!LoadOrCreate(profile, {}, RandomBytes, key, error)); assert(!fs::exists(file));
  // Invalid/truncated files are not replaced.
  Write(file, "broken");
#if !defined(_WIN32)
  chmod(file.c_str(), 0600);
#endif
  assert(!LoadOrCreate(profile, {}, RandomBytes, key, error)); assert(Read(file) == "broken");
  auto custom_profile = root / "custom-profile", custom = root / "custom-key-directory";
  assert(LoadOrCreate(custom_profile, custom, RandomBytes, key, error));
  assert(fs::exists(custom / "cookie.key") && !fs::exists(custom_profile / "Secrets"));
  assert(!LoadOrCreate(custom_profile, "relative", RandomBytes, key, error));
#if !defined(_WIN32)
  auto safe = custom / "cookie.key";
  chmod(safe.c_str(), 0644);
  assert(!LoadOrCreate(custom_profile, custom, RandomBytes, key, error));
  chmod(safe.c_str(), 0600);
  fs::rename(safe, custom / "original");
  fs::create_symlink(custom / "original", safe);
  assert(!LoadOrCreate(custom_profile, custom, RandomBytes, key, error));
  fs::remove(safe); fs::create_hard_link(custom / "original", safe);
  assert(!LoadOrCreate(custom_profile, custom, RandomBytes, key, error));
  fs::remove(safe); fs::remove(custom / "original");
  chmod(custom.c_str(), 0755);
  assert(!LoadOrCreate(custom_profile, custom, RandomBytes, key, error));
#endif
  assert(!LoadOrCreate("relative", {}, RandomBytes, key, error));
  fs::remove_all(root);
  std::cout << "ok built-in file key: restart, legacy compatibility, missing/corrupt protection, directory selection and permissions\n";
}
