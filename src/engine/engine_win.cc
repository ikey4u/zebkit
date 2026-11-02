// What the engine needs of Windows: where it is, its arguments, its child
// processes. engine_mac.mm is macOS's.

#include "zebkit/engine/engine.h"

#include <windows.h>

#include <knownfolders.h>
#include <shellapi.h>
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
  // As XDG_CONFIG_HOME on Linux: where a person, or a test, says it is.
  std::wstring given(MAX_PATH, L'\0');
  const DWORD length = ::GetEnvironmentVariableW(L"LOCALAPPDATA", given.data(), MAX_PATH);
  if (length && length < MAX_PATH) {
    given.resize(length);
    return base::WideToUTF8(given);
  }
  base::win::ScopedCoMem<wchar_t> path;
  if (FAILED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path)))
    return std::string();
  return base::WideToUTF8(path.get());
}

void WindowsArgumentsAsUTF8(int argc,
                            char** argv,
                            std::vector<std::string>* store,
                            std::vector<char*>* pointers) {
  int count = 0;
  wchar_t** wide = ::CommandLineToArgvW(::GetCommandLineW(), &count);
  store->reserve(argc);
  for (int i = 0; i < argc; ++i) {
    if (!argv[i]) {
      pointers->push_back(nullptr);
      continue;
    }
    std::string argument(argv[i]);
    // main()'s argv is the process's own, lossily in the code page: a name of another
    // script comes out as question marks. The argument Windows holds, when it is that.
    if (wide && i < count && argument == base::SysWideToNativeMB(wide[i])) {
      argument = base::WideToUTF8(wide[i]);
    } else if (!base::IsStringUTF8(argument)) {
      argument = base::WideToUTF8(ArgumentToWide(argument));
    }
    store->push_back(std::move(argument));
    pointers->push_back(store->back().data());
  }
  pointers->push_back(nullptr);
  if (wide)
    ::LocalFree(wide);
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
