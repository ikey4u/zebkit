// What the engine needs of Windows: where it is, its arguments, its child
// processes. engine_mac.mm is macOS's.

#include "zebkit/engine/engine.h"

#include <windows.h>

#include <knownfolders.h>
#include <shlobj.h>

#include <string>
#include <vector>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/strings/string_util.h"
#include "base/strings/sys_string_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/win/scoped_co_mem.h"
#include "base/no_destructor.h"
#include "content/public/app/content_main.h"
#include "content/public/app/sandbox_helper_win.h"
#include "sandbox/win/src/sandbox_types.h"

namespace zebkit {

namespace {

// The executable's own sandbox (zebkit_win_startup()), or none.
sandbox::SandboxInterfaceInfo* g_executable_sandbox = nullptr;

// argv from the host's main() is in the system's code page, from a host that
// knows better in UTF-8, which is what the C API speaks everywhere.
std::wstring ArgumentToWide(const char* argument) {
  const std::string text(argument);
  return base::IsStringUTF8(text) ? base::UTF8ToWide(text)
                                  : base::SysNativeMBToWide(text);
}

}  // namespace

base::FilePath LibraryDirectory() {
  HMODULE module = nullptr;
  if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<const wchar_t*>(&zebkit_main),
                            &module)) {
    return base::FilePath();
  }
  std::wstring path(32768, L'\0');
  const DWORD length = ::GetModuleFileNameW(module, path.data(), path.size());
  if (!length || length == path.size())
    return base::FilePath();
  path.resize(length);
  return base::FilePath(path).DirName();
}

bool HasExecutableSandbox() {
  return g_executable_sandbox != nullptr;
}

base::FilePath HelperPath(const base::FilePath& library) {
  // The sandbox needs its children to be this same executable.
  if (g_executable_sandbox) {
    std::wstring path(32768, L'\0');
    const DWORD length = ::GetModuleFileNameW(nullptr, path.data(), path.size());
    if (length && length != path.size()) {
      path.resize(length);
      return base::FilePath(path);
    }
  }
  return library.Append(FILE_PATH_LITERAL("zebkit-helper.exe"));
}

std::string DefaultConfigDirectory() {
  base::win::ScopedCoMem<wchar_t> path;
  if (FAILED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path)))
    return std::string();
  return base::WideToUTF8(path.get());
}

void SetUpWindowsMainParams(int argc, char** argv, content::ContentMainParams* params) {
  // ContentMain's own CommandLine::Init does nothing after this one.
  base::CommandLine::Init(0, nullptr);
  if (argc > 0) {
    base::CommandLine::StringVector wide;
    for (int i = 0; i < argc; ++i)
      wide.push_back(ArgumentToWide(argv[i]));
    base::CommandLine::ForCurrentProcess()->InitFromArgv(wide);
  }
  params->instance = ::GetModuleHandleW(nullptr);
  if (g_executable_sandbox) {
    params->sandbox_info = g_executable_sandbox;
    return;
  }
  // The library's own copy of the sandbox's services, which works only with
  // --no-sandbox: the broker and its targets have to be one image
  // (MainDelegate::BasicStartupComplete() refuses otherwise).
  static base::NoDestructor<sandbox::SandboxInterfaceInfo> sandbox_info;
  content::InitializeSandboxInfo(sandbox_info.get());
  params->sandbox_info = sandbox_info.get();
}

}  // namespace zebkit

extern "C" ZEBKIT_API void zebkit_win_set_sandbox(void* sandbox_interface_info) {
  zebkit::g_executable_sandbox =
      static_cast<sandbox::SandboxInterfaceInfo*>(sandbox_interface_info);
}
