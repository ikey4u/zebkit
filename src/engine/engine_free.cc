// libzebkit without the engine (zebkit_build_engine = false: Android, iOS;
// docs/MOBILE.md): what engine.cc gives the rest otherwise (the version,
// zebkit_free) and where ICU's data is, without the content layer.

#include <stdlib.h>

#include <string>

#include "base/files/file_path.h"
#include "base/i18n/icu_util.h"
#include "base/no_destructor.h"
#include "base/version_info/version_info.h"
#include "build/build_config.h"
#include "zebkit/engine/include/zebkit/zebkit.h"
#include "zebkit/engine/services_internal.h"

#if !BUILDFLAG(IS_WIN)
#include <dlfcn.h>
#endif

namespace zebkit {

namespace {

#if !BUILDFLAG(IS_WIN) && !BUILDFLAG(IS_APPLE)
// The directory this library is in: an APK's native library directory on
// Android (where only an extracted library is a real file), beside the
// program on Linux.
base::FilePath LibraryDirectoryOfThis() {
  Dl_info info;
  if (!dladdr(reinterpret_cast<void*>(&zebkit_version), &info) ||
      !info.dli_fname) {
    return base::FilePath();
  }
  char* real = realpath(info.dli_fname, nullptr);
  if (!real)
    return base::FilePath();
  base::FilePath path(real);
  free(real);
  return path.DirName();
}
#endif

}  // namespace

// Without a file from the application (zebkit_icu_init_from_file()), ICU's
// data where a library's own files are: in the bundle's resources on Apple's
// systems (an iOS app's, or the framework's), as base looks for it; beside
// the library elsewhere.
bool LoadICUData() {
#if BUILDFLAG(IS_APPLE) || BUILDFLAG(IS_WIN)
  return base::i18n::InitializeICU();
#else
  const base::FilePath directory = LibraryDirectoryOfThis();
  if (directory.empty())
    return false;
  return services::InitializeICUFromFile(directory.AppendASCII("icudtl.dat"));
#endif
}

}  // namespace zebkit

extern "C" {

const char* zebkit_version(void) {
  return ZEBKIT_VERSION;
}

const char* zebkit_engine_version(void) {
  static const base::NoDestructor<std::string> version(
      "Chromium " + std::string(version_info::GetVersionNumber()));
  return version->c_str();
}

void zebkit_free(void* pointer) {
  free(pointer);
}

}  // extern "C"
