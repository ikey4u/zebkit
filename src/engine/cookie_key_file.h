#ifndef ZEBKIT_ENGINE_COOKIE_KEY_FILE_H_
#define ZEBKIT_ENGINE_COOKIE_KEY_FILE_H_

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace zebkit::cookie_keys {
using Key = std::array<uint8_t, 32>;
using Random = bool (*)(std::span<uint8_t>);
// File format and defaults are compatible with the former Host backend.
// Empty directory selects <profile>/Secrets. Errors never replace a key.
bool LoadOrCreate(const std::filesystem::path& profile,
                  const std::filesystem::path& directory,
                  Random random,
                  Key& key,
                  std::string& error);
}  // namespace zebkit::cookie_keys
#endif
