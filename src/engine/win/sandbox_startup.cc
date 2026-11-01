// zebkit_sandbox.lib: linked into the application's executable, so that
// Chromium's sandbox is in that image (<zebkit/win_sandbox.h>).

#include <windows.h>

#include <intrin.h>
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

// base's JSON reader and its log can go through Rust, whose libraries this is not
// linked with. Their C++ names are here so that the objects link. Nothing in this
// library calls them: the engine in zebkit.dll has its own base. Should one ever
// be reached the process ends at once, rather than going on with a wrong answer;
// tests/engine/win_sandbox_stubs.cc shows that it does, and the suite's sandboxed
// tests (win-*-sandbox) that it does not happen in any kind of child.
namespace {
[[noreturn]] void StubReached(const char* name) {
  HANDLE error = ::GetStdHandle(STD_ERROR_HANDLE);
  if (error && error != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    ::WriteFile(error, name, static_cast<DWORD>(::strlen(name)), &written, nullptr);
  }
  __fastfail(FAST_FAIL_FATAL_APP_EXIT);
}
}  // namespace

namespace logging::internal {
void init_rust_logging() noexcept {
  StubReached("zebkit_sandbox.lib: init_rust_logging reached\n");
}
}  // namespace logging::internal
namespace serde_json_lenient {
bool decode_json(::rust::Slice<const ::std::uint8_t>,
                 const JsonOptions&,
                 List&,
                 DecodeError&) noexcept {
  StubReached("zebkit_sandbox.lib: decode_json reached\n");
}
}  // namespace serde_json_lenient

// The C++ side of Rust's cxx that the JSON reader's object names.
namespace rust {
inline namespace cxxbridge1 {
String::String() noexcept : repr{} {
  StubReached("zebkit_sandbox.lib: rust::String reached\n");
}
String::~String() noexcept {
  StubReached("zebkit_sandbox.lib: ~rust::String reached\n");
}
String::operator std::string() const {
  StubReached("zebkit_sandbox.lib: rust::String's string reached\n");
}
const char* Str::data() const noexcept {
  StubReached("zebkit_sandbox.lib: rust::Str::data reached\n");
}
std::size_t Str::size() const noexcept {
  StubReached("zebkit_sandbox.lib: rust::Str::size reached\n");
}
Str::operator std::string() const {
  StubReached("zebkit_sandbox.lib: rust::Str's string reached\n");
}
void sliceInit(void*, const void*, std::size_t) noexcept {
  StubReached("zebkit_sandbox.lib: rust::sliceInit reached\n");
}
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
