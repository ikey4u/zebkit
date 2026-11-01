// zebkit_sandbox.lib: linked into the application's executable, so that
// Chromium's sandbox is in that image (<zebkit/win_sandbox.h>).

#include <windows.h>

#include <shellapi.h>

#include "base/logging/rust_logger/lib.rs.h"
#include "sandbox/win/src/sandbox.h"
#include "sandbox/win/src/sandbox_factory.h"
#include "sandbox/win/src/sandbox_types.h"
#include "third_party/rust/serde_json_lenient/v0_2/wrapper/lib.rs.h"
#include "zebkit/engine/include/zebkit/win_sandbox.h"

// What this library's objects use of the system, so that the application need
// not name it.
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "delayimp.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "mincore.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "wintrust.lib")

// base's JSON reader and its log can go through Rust, whose libraries this is
// not linked with. Neither is used here: the engine in zebkit.dll has its own.
namespace logging::internal {
void init_rust_logging() noexcept {}
}  // namespace logging::internal
namespace serde_json_lenient {
bool decode_json(::rust::Slice<const ::std::uint8_t>,
                 const JsonOptions&,
                 List&,
                 DecodeError&) noexcept {
  return false;
}
}  // namespace serde_json_lenient

// The C++ side of Rust's cxx that the JSON reader's object names; it never runs.
namespace rust {
inline namespace cxxbridge1 {
String::String() noexcept : repr{} {}
String::~String() noexcept = default;
String::operator std::string() const {
  return std::string();
}
const char* Str::data() const noexcept {
  return nullptr;
}
std::size_t Str::size() const noexcept {
  return 0;
}
Str::operator std::string() const {
  return std::string();
}
void sliceInit(void*, const void*, std::size_t) noexcept {}
}  // namespace cxxbridge1
}  // namespace rust

namespace {

// Whether the process was started with --type=, as the engine's children are.
bool IsChildProcess() {
  int count = 0;
  wchar_t** arguments = ::CommandLineToArgvW(::GetCommandLineW(), &count);
  if (!arguments)
    return false;
  bool child = false;
  for (int i = 1; i < count && !child; ++i)
    child = ::wcsncmp(arguments[i], L"--type=", 7) == 0;
  ::LocalFree(arguments);
  return child;
}

}  // namespace

extern "C" bool zebkit_win_startup(int* exit_code) {
  // As content::InitializeSandboxInfo(), and with no destructor: the engine
  // keeps a pointer to it for the life of the process.
  static sandbox::SandboxInterfaceInfo info = {};
  info.broker_services = sandbox::SandboxFactory::GetBrokerServices();
  if (info.broker_services) {
    info.broker_services->SetStartingMitigations(0);
    info.broker_services->RatchetDownSecurityMitigations(
        sandbox::MITIGATION_DEP | sandbox::MITIGATION_DEP_NO_ATL_THUNK |
        sandbox::MITIGATION_HARDEN_TOKEN_IL_POLICY);
  } else {
    info.target_services = sandbox::SandboxFactory::GetTargetServices();
  }
  zebkit_win_set_sandbox(&info);
  if (!IsChildProcess())
    return false;
  const int code = zebkit_helper_main(0, nullptr);
  if (exit_code)
    *exit_code = code;
  return true;
}
