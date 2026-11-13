// What the C API's entry points share: strings for the application and the
// errors they give back (freed with zebkit_free()).

#ifndef ZEBKIT_ENGINE_C_API_UTIL_H_
#define ZEBKIT_ENGINE_C_API_UTIL_H_

#include <stdlib.h>
#include <string.h>

#include <string>
#include <string_view>

#include "base/compiler_specific.h"

namespace zebkit {

// A copy of @text for the application, which frees it with zebkit_free().
inline char* CopyString(std::string_view text) {
  char* copy = static_cast<char*>(malloc(text.size() + 1));
  if (!copy)
    return nullptr;
  // The application's C string: as many bytes as @text has, and a NUL.
  if (!text.empty())
    UNSAFE_BUFFERS(memcpy(copy, text.data(), text.size()));
  UNSAFE_BUFFERS(copy[text.size()] = '\0');
  return copy;
}

// @message in @error when the application asked for it (a NULL @error does
// not); false, for `return SetError(error, "...")`.
inline bool SetError(char** error, std::string_view message) {
  if (error)
    *error = CopyString(message);
  return false;
}

// The same for the engine's own functions, which give it as a std::string.
inline bool SetError(std::string* error, std::string_view message) {
  if (error)
    *error = std::string(message);
  return false;
}

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_C_API_UTIL_H_
