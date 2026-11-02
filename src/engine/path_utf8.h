// Paths as the UTF-8 the C API speaks, and back. Not FilePath::AsUTF8Unsafe(),
// which on Linux goes through the process's locale; on Windows paths are wide.

#ifndef ZEBKIT_ENGINE_PATH_UTF8_H_
#define ZEBKIT_ENGINE_PATH_UTF8_H_

#include <string>

#include "base/files/file_path.h"
#include "build/build_config.h"
#if BUILDFLAG(IS_WIN)
#include "base/strings/string_util.h"
#include "base/strings/sys_string_conversions.h"
#include "base/strings/utf_string_conversions.h"
#endif

namespace zebkit {

inline std::string PathToUTF8(const base::FilePath& path) {
#if BUILDFLAG(IS_WIN)
  return base::WideToUTF8(path.value());
#else
  return path.value();
#endif
}

inline base::FilePath PathFromUTF8(const std::string& path) {
#if BUILDFLAG(IS_WIN)
  return base::FilePath(base::UTF8ToWide(path));
#else
  return base::FilePath(path);
#endif
}

#if BUILDFLAG(IS_WIN)
// An argument from a host's main() is in the system's code page, from a host that
// knows better in UTF-8, which is what the C API speaks everywhere.
inline std::wstring ArgumentToWide(const std::string& text) {
  return base::IsStringUTF8(text) ? base::UTF8ToWide(text)
                                  : base::SysNativeMBToWide(text);
}
#endif

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_PATH_UTF8_H_
