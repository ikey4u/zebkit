// Helpers of the services' C API (services_core.cc, services.cc): buffers
// and strings the caller frees with zebkit_free(), and errors.

#ifndef ZEBKIT_ENGINE_SERVICES_INTERNAL_H_
#define ZEBKIT_ENGINE_SERVICES_INTERNAL_H_

#ifdef UNSAFE_BUFFERS_BUILD
// The C API takes and returns C strings and arrays.
#pragma allow_unsafe_buffers
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cstddef>
#include <string>
#include <vector>

#include "base/files/file_path.h"

namespace zebkit::services {

// The most a response or a decompression gives, unless the caller says.
inline constexpr size_t kDefaultMaxSize = 256u << 20;

// A buffer the caller frees with zebkit_free().
inline uint8_t* Give(const uint8_t* data, size_t size, size_t* out_size) {
  uint8_t* copy = static_cast<uint8_t*>(malloc(size ? size : 1));
  if (!copy)
    return nullptr;
  if (size)
    memcpy(copy, data, size);
  if (out_size)
    *out_size = size;
  return copy;
}

inline uint8_t* Give(const std::vector<uint8_t>& data, size_t* out_size) {
  return Give(data.data(), data.size(), out_size);
}

inline char* GiveString(const std::string& text) {
  return strdup(text.c_str());
}

inline std::nullptr_t Fail(char** error, const std::string& message) {
  if (error)
    *error = strdup(message.c_str());
  return nullptr;
}

}  // namespace zebkit::services

namespace zebkit {

// ICU's data, found and loaded once: beside libzebkit for the engine
// (engine.cc), where the application said or beside the library for the
// library without the engine (engine_free.cc). zebkit_icu_init() calls it.
bool LoadICUData();

namespace services {
// ICU's data from the file at `path` (an icudtl.dat).
bool InitializeICUFromFile(const base::FilePath& path);
}  // namespace services

// Content initialized ICU itself (engine.cc, before RunProcess()).
void MarkICUInitialized();

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_SERVICES_INTERNAL_H_
