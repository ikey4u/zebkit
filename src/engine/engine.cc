// zebkit_main(), zebkit_helper_main() and the classes that start the engine.

#ifdef UNSAFE_BUFFERS_BUILD
// The C API takes and returns C strings and arrays.
#pragma allow_unsafe_buffers
#endif

#include "content/public/common/webplugininfo.h"
#include "crypto/crypto_buildflags.h"
#include "net/ssl/client_cert_store.h"
#if BUILDFLAG(USE_NSS_CLIENT_CERTS)
#include "net/ssl/client_cert_store_nss.h"
#elif BUILDFLAG(IS_MAC)
#include "net/ssl/client_cert_store_mac.h"
#elif BUILDFLAG(IS_WIN)
#include "net/ssl/client_cert_store_win.h"
#endif
#include "zebkit/engine/ime_ibus_linux.h"
#include "zebkit/engine/device_choosers.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/appearance.h"
#include "zebkit/engine/sign_in_dialog.h"
#include "zebkit/engine/share.h"
#include "third_party/blink/public/platform/web_runtime_features.h"

#if BUILDFLAG(IS_POSIX)
#include <signal.h>
#endif
#include "zebkit/engine/host_loop.h"
#include "zebkit/engine/startup_settings.h"
#include "zebkit/engine/settings.h"
#include "zebkit/engine/permissions.h"
#include "zebkit/engine/navigation_hooks.h"
#include "content/public/browser/client_certificate_delegate.h"
#include "net/ssl/client_cert_identity.h"
#include "net/ssl/ssl_cert_request_info.h"
#include "zebkit/engine/network_hooks.h"
#include "zebkit/engine/page_hooks.h"
#include "zebkit/engine/certificate_hooks.h"
#include "zebkit/engine/http_auth.h"
#include "zebkit/engine/popup_metadata.h"
#include "zebkit/engine/page_internal.h"

#if !BUILDFLAG(IS_WIN)
#include <dlfcn.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "base/base_paths.h"
#include "base/check.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/containers/fixed_flat_set.h"
#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/logging/logging_settings.h"
#include "base/json/json_writer.h"
#include "base/memory/ref_counted_memory.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/process/current_process.h"
#include "base/run_loop.h"
#include "base/threading/platform_thread.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "base/strings/string_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/version_info/version_info.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_LINUX)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <thread>

#include "device/bluetooth/dbus/dbus_bluez_manager_wrapper_linux.h"
#include "sandbox/linux/services/credentials.h"
#include "ui/linux/linux_ui.h"
#include "ui/linux/linux_ui_factory.h"
#include "sandbox/policy/switches.h"
#endif
#if BUILDFLAG(IS_WIN)
#include "base/win/win_util.h"
#include "sandbox/policy/switches.h"
#endif
#include "components/embedder_support/origin_trials/origin_trial_policy_impl.h"
#include "components/embedder_support/switches.h"
#include "net/http/http_util.h"
#include "net/proxy_resolution/proxy_config.h"
#include "net/proxy_resolution/proxy_config_with_annotation.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "third_party/blink/public/common/renderer_preferences/renderer_preferences.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/icu/source/common/unicode/locid.h"
#include "components/embedder_support/user_agent_utils.h"
#include "components/version_info/version_info_with_user_agent.h"
#include "components/crash/core/common/crash_key.h"
#include "components/memory_system/initializer.h"
#include "components/memory_system/parameters.h"
#include "components/performance_manager/embedder/graph_features.h"
#include "components/performance_manager/embedder/performance_manager_lifetime.h"
#include "content/public/app/content_main.h"
#include "content/public/app/initialize_mojo_core.h"
#include "content/public/common/result_codes.h"
#include "content/public/common/url_constants.h"
#include "device/bluetooth/bluetooth_adapter_factory.h"
#include "net/base/features.h"
#include "third_party/blink/public/common/features.h"
#include "third_party/blink/public/common/web_preferences/web_preferences.h"
#include "content/public/common/content_features.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/ui_base_switches.h"
#include "url/url_constants.h"
#include "zebkit/engine/profile.h"
#include "zebkit/engine/window.h"
#include "zebkit/engine/window_platform.h"
#include "content/public/app/content_main_runner.h"
#include "content/public/browser/browser_main_runner.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_switches.h"
#include "net/base/module/net_module.h"
#include "net/grit/net_resources.h"
#include "services/network/public/mojom/cookie_manager.mojom.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/mojom/websocket.mojom.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/base/ui_base_paths.h"
#include "ui/gfx/switches.h"
#include "zebkit/engine/app.h"
#include "zebkit/engine/automation.h"
#include "zebkit/engine/automation_network.h"
#include "zebkit/engine/cookies.h"
#include "zebkit/engine/media_capture.h"
#include "zebkit/engine/scheme.h"
#include "zebkit/engine/view.h"

#include "content/public/browser/authenticator_request_client_delegate.h"
#include "zebkit/engine/webauthn.h"

#if defined(ZEBKIT_COOKIES_USE_DBUS)
#include "dbus/bus.h"
#endif

#if BUILDFLAG(IS_OZONE)
#include "ui/ozone/public/ozone_switches.h"
#endif

namespace zebkit {

// The compatibility entry opts into zebkit's application shell; init does
// not replace an existing application's delegate, menu bar or Dock policy.
bool g_external_loop = false;
bool g_application_integration = false;

#if !BUILDFLAG(IS_WIN)
// Where libzebkit is: the engine's data files and zebkit-helper are beside
// it. On macOS, the framework's version directory, which has them inside.
// (Windows': engine_win.cc.)
base::FilePath LibraryDirectory() {
  Dl_info info;
  if (!dladdr(reinterpret_cast<void*>(&zebkit_main), &info) || !info.dli_fname)
    return base::FilePath();
  char* real = realpath(info.dli_fname, nullptr);
  if (!real)
    return base::FilePath();
  base::FilePath path(real);
  free(real);
  return path.DirName();
}
#endif  // !BUILDFLAG(IS_WIN)

#if !BUILDFLAG(IS_MAC) && !BUILDFLAG(IS_WIN)
base::FilePath HelperPath(const base::FilePath& library) {
  return library.Append("zebkit-helper");
}
#endif

namespace {

bool HasSwitch(int argc, char** argv, const char* name) {
  const size_t length = strlen(name);
  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    if (arg[0] != '-' || arg[1] != '-')
      continue;
    if (strncmp(arg + 2, name, length) == 0 &&
        (arg[2 + length] == '\0' || arg[2 + length] == '=')) {
      return true;
    }
  }
  return false;
}

// The value of --name=value, or "" when it is not given.
std::string SwitchValue(int argc, char** argv, const char* name) {
  const size_t length = strlen(name);
  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    if (arg[0] == '-' && arg[1] == '-' && strncmp(arg + 2, name, length) == 0 &&
        arg[2 + length] == '=') {
      return arg + 3 + length;
    }
  }
  return "";
}

// The arguments Chromium sees: the application's, and what the engine
// needs that the application did not say.
std::vector<std::string> EngineArguments(int argc, char** argv) {
  std::vector<std::string> args;
  for (int i = 0; i < argc; ++i)
    args.emplace_back(argv[i]);
  if (args.empty())
    args.push_back("zebkit");
  const base::FilePath library = LibraryDirectory();
  if (!HasSwitch(argc, argv, switches::kBrowserSubprocessPath)) {
    args.push_back(std::string("--") + switches::kBrowserSubprocessPath + "=" +
                   PathToUTF8(HelperPath(library)));
  }
  if (!HasSwitch(argc, argv, kUserDataDirSwitch)) {
    std::string config;
#if BUILDFLAG(IS_MAC)
    if (const char* home = getenv("HOME"); home && *home)
      config = std::string(home) + "/Library/Application Support";
#elif BUILDFLAG(IS_WIN)
    config = DefaultConfigDirectory();
#else
    if (const char* xdg = getenv("XDG_CONFIG_HOME"); xdg && *xdg)
      config = xdg;
    else if (const char* home = getenv("HOME"); home && *home)
      config = std::string(home) + "/.config";
#endif
    if (!config.empty()) {
#if BUILDFLAG(IS_WIN)
      const std::string program = PathToUTF8(
          base::FilePath(base::UTF8ToWide(args[0])).BaseName().RemoveFinalExtension());
#else
      const std::string program = base::FilePath(args[0]).BaseName().value();
#endif
      args.push_back(std::string("--") + kUserDataDirSwitch +
                     "=" + config + "/zebkit/" + program);
    }
  }
#if BUILDFLAG(IS_OZONE)
  if (!HasSwitch(argc, argv, switches::kOzonePlatform)) {
    const char* wayland = getenv("WAYLAND_DISPLAY");
    const char* x11 = getenv("DISPLAY");
    const bool display = (wayland && *wayland) || (x11 && *x11);
    if (!display || HasSwitch(argc, argv, switches::kHeadless))
      args.push_back(std::string("--") + switches::kOzonePlatform + "=headless");
  }
  // Ozone's headless screen is 1 by 1 pixel, which no person's screen is:
  // pages that lay out by screen.width break, and sites take it for a bot.
  // Chrome's headless shell gives its window's size; zebkit gives the size
  // of --window-size, or else the most common desktop screen, 1920 by 1080,
  // since its headless views are shown to people. --ozone-override-screen-size
  // and --screen-info still decide.
  const bool headless_ozone =
      SwitchValue(argc, argv, switches::kOzonePlatform) == "headless" ||
      std::find(args.begin(), args.end(),
                std::string("--") + switches::kOzonePlatform + "=headless") !=
          args.end();
  if (headless_ozone &&
      !HasSwitch(argc, argv, switches::kOzoneOverrideScreenSize) &&
      !HasSwitch(argc, argv, "screen-info")) {
    std::string size = SwitchValue(argc, argv, "window-size");
    if (size.empty())
      size = "1920,1080";
    args.push_back(std::string("--") + switches::kOzoneOverrideScreenSize +
                   "=" + size);
  }
#endif
  // The log goes to stderr, not to a file beside the executable, which for
  // the engine is in its package, perhaps read-only or shared.
  if (!HasSwitch(argc, argv, switches::kEnableLogging) &&
      !HasSwitch(argc, argv, switches::kLogFile) &&
      !HasSwitch(argc, argv, "disable-logging")) {
    args.push_back(std::string("--") + switches::kEnableLogging + "=stderr");
  }
  // Features a browser wants and an application showing its pages does not,
  // as Electron leaves them out (its shell/browser/feature_list.cc): a spare
  // renderer kept warm for the next navigation, a process with its memory.
  // --zebkit-spare-renderer keeps it.
  //
  // And FedCM, which signs in with an identity provider through dialogs of
  // the browser's own: the engine has none and turns every request
  // down as dismissed, so a site's "Sign in with Google" fails, and is held
  // back for a while after. Without FedCM, sites sign in through a popup,
  // as in Firefox, which has no FedCM; zebkit opens it from the person's
  // click with its opener (docs/API.md). --zebkit-fedcm keeps it, for an
  // application that answers its dialogs (docs/ROADMAP_HOOKS.md).
  // Both are added to any --disable-features given.
  std::vector<std::string> disabled;
  if (!HasSwitch(argc, argv, "zebkit-spare-renderer"))
    disabled.push_back("SpareRendererForSitePerProcess");
  if (!HasSwitch(argc, argv, "zebkit-fedcm"))
    disabled.push_back("FedCm");
  for (const std::string& feature : disabled) {
    const std::string prefix = "--disable-features=";
    auto given = std::find_if(args.begin(), args.end(), [&](const std::string& arg) {
      return arg.starts_with(prefix);
    });
    if (given == args.end())
      args.push_back(prefix + feature);
    else
      *given += "," + feature;
  }
  return args;
}

scoped_refptr<base::RefCountedMemory> NetResourceProvider(int key) {
  if (key == IDR_DIR_HEADER_HTML) {
    return ui::ResourceBundle::GetSharedInstance().LoadDataResourceBytes(
        IDR_DIR_HEADER_HTML);
  }
  return nullptr;
}

// To --log-file, or as --enable-logging says; EngineArguments() defaults to
// standard error, not a file beside the executable.
void InitLogging(const base::CommandLine& command_line) {
  logging::LoggingSettings settings;
  if (command_line.HasSwitch("disable-logging")) {
    settings.logging_dest = logging::LOG_NONE;
    logging::InitLogging(settings);
    return;
  }
  const std::string dest = command_line.GetSwitchValueASCII(switches::kEnableLogging);
  if (dest == "stderr") {
    settings.logging_dest = logging::LOG_TO_STDERR;
  } else if (dest == "system") {
    settings.logging_dest = logging::LOG_TO_SYSTEM_DEBUG_LOG;
  } else {
    base::FilePath file = command_line.GetSwitchValuePath(switches::kLogFile);
#if BUILDFLAG(IS_WIN)
    // A child process may be given a handle in --log-file.
    if (dest == "handle") {
      uint32_t value = 0;
      HANDLE duplicate = nullptr;
      if (base::StringToUint(command_line.GetSwitchValueNative(switches::kLogFile), &value) &&
          ::DuplicateHandle(GetCurrentProcess(), base::win::Uint32ToHandle(value),
                            GetCurrentProcess(), &duplicate, 0, FALSE,
                            DUPLICATE_SAME_ACCESS)) {
        settings.log_file = duplicate;
        settings.logging_dest = logging::LOG_TO_ALL;
        file.clear();
      }
    }
#endif
    if (!file.empty()) {
      settings.log_file_path = file.value();
      settings.logging_dest = logging::LOG_TO_FILE;
    } else if (settings.logging_dest == logging::LOG_DEFAULT) {
      settings.logging_dest = logging::LOG_TO_STDERR;
    }
  }
  settings.delete_old = logging::APPEND_TO_OLD_LOG_FILE;
  logging::InitLogging(settings);
  logging::SetLogItems(true /* Process ID */, true /* Thread ID */,
                       true /* Timestamp */, false /* Tick count */);
}

#if BUILDFLAG(IS_LINUX)
// Started cold, the engine's files not in the page cache, the browser and
// its first processes fault in about 100 MiB of the library's 159 in some
// two hundred separate reads; on a disk that seeks that was 4.2 s to the
// page's request, where reading the whole file front to back takes 0.9 s.
// So the browser process reads its files into the cache on a thread of its
// own while startup goes on, as Chrome pre-reads chrome.dll on Windows and
// its library on Android. (base::PreReadFile is posix_fadvise() here, which
// the kernel cuts to one readahead window.) 2026-11-07, cold to first
// paint 5.3 to 2.4 s. Only when the library is not in the cache already: a
// warm start looks at 64 of its pages and reads nothing (mincore() over all
// of it cost the warm start 35 ms).
bool InPageCache(int fd, size_t size) {
  void* map = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED)
    return true;
  const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  const size_t pages = (size + page - 1) / page;
  bool cached = true;
  for (size_t i = 0; cached && i < 64; ++i) {
    unsigned char resident = 0;
    char* at = static_cast<char*>(map) + (pages * i / 64) * page;
    cached = mincore(at, page, &resident) == 0 && (resident & 1);
  }
  munmap(map, size);
  return cached;
}

void ReadIntoPageCache(const base::FilePath& path) {
  const int fd = open(path.value().c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return;
  std::vector<char> buffer(1 << 20);
  while (read(fd, buffer.data(), buffer.size()) > 0) {
  }
  close(fd);
}

void PrefetchEngineFiles(int argc, char** argv) {
  if (HasSwitch(argc, argv, "type"))
    return;
  const base::FilePath directory = LibraryDirectory();
  if (directory.empty())
    return;
  std::thread([directory] {
    const base::FilePath library = directory.Append("libzebkit.so.1");
    struct stat info;
    const int fd = open(library.value().c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
      return;
    const bool cached = fstat(fd, &info) != 0 || InPageCache(fd, static_cast<size_t>(info.st_size));
    close(fd);
    if (cached)
      return;
    for (const char* name : {"libzebkit.so.1", "icudtl.dat", "v8_context_snapshot.bin",
                             "snapshot_blob.bin", "zebkit.pak"}) {
      ReadIntoPageCache(directory.Append(name));
    }
  }).detach();
}
#endif  // BUILDFLAG(IS_LINUX)

int RunContent(int argc, char** argv, MainDelegate* delegate) {
  content::ContentMainParams params(delegate);
#if BUILDFLAG(IS_WIN)
  SetUpWindowsMainParams(argc, argv, &params);
#else
  params.argc = argc;
  params.argv = const_cast<const char**>(argv);
#endif
  return content::ContentMain(std::move(params));
}

}  // namespace

MainDelegate::MainDelegate(zebkit_task_func ready, void* data, bool external_loop)
    : ready_(ready), data_(data), external_loop_(external_loop) {}

MainDelegate::~MainDelegate() = default;

void MainDelegate::ShutdownBrowser() {
  if (browser_runner_) {
    browser_runner_->Shutdown();
    browser_runner_.reset();
  }
}

#if BUILDFLAG(IS_LINUX)
// docs/SANDBOX.md. Chromium's choice (ZygoteHostImpl::Init): user namespaces, else
// the helper "chrome-sandbox" beside FILE_EXE, which is beside the engine.
bool CheckSandbox(const base::CommandLine& command) {
  if (command.HasSwitch(sandbox::policy::switches::kNoSandbox)) {
    fprintf(stderr, "zebkit: running pages without the sandbox (--no-sandbox)\n");
    return true;
  }
  const bool root = geteuid() == 0;
  const bool namespaces =
      !root && !command.HasSwitch(sandbox::policy::switches::kDisableNamespaceSandbox) &&
      sandbox::Credentials::CanCreateProcessInNewUserNS();
  if (namespaces)
    return true;
  const base::FilePath helper = LibraryDirectory().AppendASCII("chrome-sandbox");
  std::string state = "is not there";
  struct stat info;
  if (!root && !command.HasSwitch(sandbox::policy::switches::kDisableSetuidSandbox) &&
      stat(helper.value().c_str(), &info) == 0) {
    struct statvfs volume;
    if (info.st_uid != 0 || !(info.st_mode & S_ISUID) || !(info.st_mode & S_IXOTH)) {
      state = "is not installed (it must be owned by root, mode 4755)";
    } else if (statvfs(helper.value().c_str(), &volume) == 0 && (volume.f_flag & ST_NOSUID)) {
      state = "is on a file system mounted nosuid";
    } else {
      return true;
    }
  }
  if (root) {
    fprintf(stderr,
            "zebkit: refusing to start as root with the sandbox, which Chromium does not "
            "support. Run as another user, or pass --no-sandbox where the container itself "
            "is the isolation.\n");
    return false;
  }
  fprintf(stderr,
          "zebkit: no sandbox for the page processes, and pages do not run without one "
          "unless told to.\n"
          "  This system does not allow unprivileged user namespaces, and the helper %s %s.\n"
          "  Any one of:\n"
          "  - allow them (on Ubuntu 23.10 and later: an AppArmor profile, or sysctl "
          "kernel.apparmor_restrict_unprivileged_userns=0; elsewhere sysctl "
          "kernel.unprivileged_userns_clone=1 or user.max_user_namespaces);\n"
          "  - install the helper, as root: %s/zebkit-sandbox-install;\n"
          "  - pass --no-sandbox where something else isolates the process, such as a "
          "container of its own, and only for pages you trust that much.\n",
          helper.value().c_str(), state.c_str(), LibraryDirectory().value().c_str());
  return false;
}
#endif  // BUILDFLAG(IS_LINUX)

#if BUILDFLAG(IS_WIN)
// The sandbox on Windows takes a call from the application's executable
// (<zebkit/win_sandbox.h>). Never without one unless told.
bool CheckSandbox(const base::CommandLine& command) {
  if (command.HasSwitch(sandbox::policy::switches::kNoSandbox)) {
    fprintf(stderr, "zebkit: running pages without the sandbox (--no-sandbox)\n");
    LOG(WARNING) << "Running pages without the sandbox (--no-sandbox)";
    return true;
  }
  if (HasExecutableSandbox())
    return true;
  fprintf(stderr,
          "zebkit: no sandbox for the page processes, and pages do not run without one "
          "unless told to.\n"
          "  Link zebkit_sandbox.lib and call zebkit_win_startup() first in main() "
          "(<zebkit/win_sandbox.h>), or pass --no-sandbox where something else isolates "
          "the process, and only for pages you trust that much.\n");
  return false;
}
#endif  // BUILDFLAG(IS_WIN)

std::optional<int> MainDelegate::BasicStartupComplete() {
  // Chromium finds its data files beside the executable, which for an
  // application is not where the engine is. (On macOS, in the framework
  // bundle: SetUpBundle(), before the engine starts.)
  const base::FilePath library = LibraryDirectory();
#if !BUILDFLAG(IS_MAC)
  if (!library.empty()) {
    base::PathService::OverrideAndCreateIfNeeded(
        base::DIR_MODULE, library, /*is_absolute=*/true, /*create=*/false);
    // The zygote is started from FILE_EXE, not from
    // --browser-subprocess-path: it must be zebkit-helper, whatever the
    // application's executable is (python3, for one).
    base::PathService::OverrideAndCreateIfNeeded(
        base::FILE_EXE, HelperPath(library), /*is_absolute=*/true,
        /*create=*/false);
    // Translations would be in locales/, which Chromium creates when it is
    // not there; zebkit has none, and leaves its package as it is.
    base::PathService::OverrideAndCreateIfNeeded(
        ui::DIR_LOCALES, library.AppendASCII("locales"), /*is_absolute=*/true,
        /*create=*/false);
  }
#endif
  std::optional<int> result;
  const auto& command = *base::CommandLine::ForCurrentProcess();
#if BUILDFLAG(IS_WIN)
  base::win::EnableStrictHandleCheckingForCurrentProcess();
#endif
  InitLogging(command);
  RegisterUserDataPath();
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN)
  // In the browser process: never without a sandbox unless told, and a refusal
  // that says what to do, ahead of Chromium's fatal message.
  if (!result && !command.HasSwitch("type") && !CheckSandbox(command))
    return 1;
#endif
  if (command.HasSwitch("disable-logging")) {
    logging::SetMinLogLevel(logging::LOGGING_FATAL);
  } else if (command.HasSwitch("log-level")) {
    int level;
    if (base::StringToInt(command.GetSwitchValueASCII("log-level"), &level))
      logging::SetMinLogLevel(level);
  }
  return result;
}

bool MainDelegate::ShouldLoadV8Snapshot(const std::string& process_type) {
  // The browser process runs the application's own script
  // (zebkit_script_new()), so it needs V8 as a renderer does.
  return process_type.empty() ||
         content::ContentMainDelegate::ShouldLoadV8Snapshot(process_type);
}

bool MainDelegate::ShouldCreateFeatureList(InvokedIn invoked_in) {
  return std::holds_alternative<InvokedInChildProcess>(invoked_in);
}

bool MainDelegate::ShouldInitializeMojo(InvokedIn invoked_in) {
  return ShouldCreateFeatureList(invoked_in);
}

void MainDelegate::PreSandboxStartup() {
  crash_reporter::InitializeCrashKeys();
#if BUILDFLAG(IS_MAC)
  ui::ResourceBundle::InitSharedInstanceWithPakPath(ResourcePackPath());
#else
  base::FilePath pak_file;
  CHECK(base::PathService::Get(base::DIR_ASSETS, &pak_file));
  ui::ResourceBundle::InitSharedInstanceWithPakPath(
      pak_file.Append(FILE_PATH_LITERAL("zebkit.pak")));
#endif
}

std::optional<int> MainDelegate::PreBrowserMain() {
  std::optional<int> exit_code = content::ContentMainDelegate::PreBrowserMain();
#if BUILDFLAG(IS_MAC)
  // An embedded engine uses the application's existing NSApplication and
  // native pump.
  if (!exit_code && !external_loop_)
    RegisterApplication();
#endif
  return exit_code;
}

std::optional<int> MainDelegate::PostEarlyInitialization(InvokedIn invoked_in) {
  if (!ShouldCreateFeatureList(invoked_in))
    browser_client_->CreateFeatureListAndFieldTrials();
  if (!ShouldInitializeMojo(invoked_in))
    content::InitializeMojoCore();
  // The sampler enforced, as the content shell: its users there were not
  // clear, and the engine has run with it.
  memory_system::Initializer()
      .SetDispatcherParameters(memory_system::DispatcherParameters::
                                   PoissonAllocationSamplerInclusion::kEnforce,
                               memory_system::DispatcherParameters::
                                   AllocationTraceRecorderInclusion::kIgnore,
                               base::CommandLine::ForCurrentProcess()
                                   ->GetSwitchValueASCII(switches::kProcessType))
      .Initialize(memory_system_);
  return std::nullopt;
}

content::ContentClient* MainDelegate::CreateContentClient() {
  content_client_ = std::make_unique<ContentClient>();
  return content_client_.get();
}

content::ContentBrowserClient* MainDelegate::CreateContentBrowserClient() {
  browser_client_ = std::make_unique<BrowserClient>();
  return browser_client_.get();
}

content::ContentRendererClient* MainDelegate::CreateContentRendererClient() {
  renderer_client_ = std::make_unique<RendererClient>();
  return renderer_client_.get();
}

std::u16string ContentClient::GetLocalizedString(int message_id) {
  return l10n_util::GetStringUTF16(message_id);
}

std::string_view ContentClient::GetDataResource(
    int resource_id,
    ui::ResourceScaleFactor scale_factor) {
  return ui::ResourceBundle::GetSharedInstance().GetRawDataResourceForScale(
      resource_id, scale_factor);
}

scoped_refptr<base::RefCountedMemory> ContentClient::GetDataResourceBytes(int resource_id) {
  return ui::ResourceBundle::GetSharedInstance().LoadDataResourceBytes(resource_id);
}

std::string ContentClient::GetDataResourceString(int resource_id) {
  return ui::ResourceBundle::GetSharedInstance().LoadDataResourceString(resource_id);
}

gfx::Image& ContentClient::GetNativeImageNamed(int resource_id) {
  return ui::ResourceBundle::GetSharedInstance().GetNativeImageNamed(resource_id);
}

blink::OriginTrialPolicy* ContentClient::GetOriginTrialPolicy() {
  // Chrome's key and rules: tokens are checked as Chrome checks them. Asked for
  // from more than one thread, and not before the feature list exists.
  base::AutoLock lock(origin_trial_policy_lock_);
  if (!origin_trial_policy_)
    origin_trial_policy_ = std::make_unique<embedder_support::OriginTrialPolicyImpl>();
  return origin_trial_policy_.get();
}

void ContentClient::AddAdditionalSchemes(Schemes* schemes) {
  AddSchemes(schemes);
}

void ContentClient::AddPlugins(std::vector<content::WebPluginInfo>* plugins) {
  // A switch to have PDFs as downloads, as the setting "Download PDFs" in
  // Chrome: then navigator.pdfViewerEnabled is false as well.
  if (base::CommandLine::ForCurrentProcess()->HasSwitch("disable-pdf-viewer"))
    return;
  content::WebPluginInfo info;
  info.name = u"Chrome PDF Viewer";
  info.path = base::FilePath(FILE_PATH_LITERAL("internal-pdf-viewer"));
  info.desc = u"Portable Document Format";
  info.type = content::WebPluginInfo::PLUGIN_TYPE_BROWSER_INTERNAL_PLUGIN;
  info.mime_types.emplace_back("application/pdf", "pdf", "Portable Document Format");
  info.mime_types.emplace_back("text/pdf", "pdf", "Portable Document Format");
  plugins->push_back(std::move(info));
}

void RendererClient::RenderThreadStarted() {
  // The page's locale is the system's, from base's first default ICU locale
  // (patches/1083), as Chrome's from l10n_util.
  RegisterSchemesInRenderer();
  // navigator.share(), served by the browser's ShareService (share.h).
  blink::WebRuntimeFeatures::EnableWebShare(true);
}

std::variant<int, content::MainFunctionParams> MainDelegate::RunProcess(
    const std::string& process_type,
    content::MainFunctionParams main_function_params) {
  MarkICUInitialized();
  if (!process_type.empty()) {
    return std::move(main_function_params);
  }
  base::CurrentProcess::GetInstance().SetProcessType(
      base::CurrentProcessType::PROCESS_BROWSER);
  if (external_loop_) {
    // RunContentProcess's autorelease pool lives on its stack. Nothing in
    // the externally driven browser may retain it after Initialize returns.
#if BUILDFLAG(IS_MAC)
    main_function_params.autorelease_pool = nullptr;
#endif
    browser_runner_ = content::BrowserMainRunner::Create();
    const int result = browser_runner_->Initialize(std::move(main_function_params));
    return result < 0 ? 0 : (result ? result : 1);
  }
  std::unique_ptr<content::BrowserMainRunner> runner =
      content::BrowserMainRunner::Create();
  const int exit_code = runner->Initialize(std::move(main_function_params));
  if (exit_code >= 0)
    return exit_code;
  if (ready_) {
    // In the loop, so that the application's code runs as every later
    // callback will.
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(ready_, data_));
  }
  const int result = runner->Run();
  runner->Shutdown();
  return result;
}

std::unique_ptr<content::BrowserMainParts> BrowserClient::CreateBrowserMainParts(
    bool is_integration_test) {
  auto parts = std::make_unique<BrowserMainParts>();
  main_parts_ = parts.get();
  return parts;
}

bool BrowserClient::IsHandledURL(const GURL& url) {
  if (!url.is_valid())
    return false;
  static constexpr auto kSchemes = base::MakeFixedFlatSet<std::string_view>({
      url::kHttpScheme,
      url::kHttpsScheme,
      url::kWsScheme,
      url::kWssScheme,
      url::kBlobScheme,
      url::kFileSystemScheme,
      content::kChromeUIScheme,
      content::kChromeUIUntrustedScheme,
      content::kChromeDevToolsScheme,
      url::kDataScheme,
      url::kFileScheme,
  });
  return IsApplicationScheme(url.scheme()) || kSchemes.contains(url.scheme());
}

std::unique_ptr<content::WebContentsViewDelegate>
BrowserClient::GetWebContentsViewDelegate(content::WebContents* web_contents) {
  return WrapViewDelegate(web_contents, nullptr);
}

void BrowserClient::AppendExtraCommandLineSwitches(
    base::CommandLine* command_line,
    int child_process_id) {
  if (command_line->GetSwitchValueASCII(switches::kProcessType) ==
          switches::kRendererProcess &&
      base::FeatureList::IsEnabled(features::kIsolatedWebApps)) {
    command_line->AppendSwitch(switches::kEnableIsolatedWebAppsInRenderer);
  }
  const auto& process = *base::CommandLine::ForCurrentProcess();
  for (const char* flag : {"enable-logging", "disable-logging", "log-file", "log-level"}) {
#if BUILDFLAG(IS_WIN)
    // Chromium gives a child the open log by handle and refuses to start one
    // that has been given these (child_process_launcher_helper_win.cc).
    if (logging::IsLoggingToFileEnabled() &&
        (!strcmp(flag, "enable-logging") || !strcmp(flag, "log-file"))) {
      continue;
    }
#endif
    if (process.HasSwitch(flag))
      command_line->AppendSwitchNative(flag, process.GetSwitchValueNative(flag));
  }
  AppendSchemeSwitch(command_line);
  AppendMediaSwitch(command_line);
}

std::string AcceptLanguages() {
  static const base::NoDestructor<std::string> languages([] {
    const std::string given =
        base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
            "accept-lang");
    if (!given.empty())
      return given;
    // As Chrome's default for the system's locale (from LANGUAGE, LC_ALL,
    // LANG; ICU's): the locale, then its language, "en-US" without one.
    // (l10n_util's needs Chrome's locale packs, which zebkit has not.)
    const icu::Locale& system = icu::Locale::getDefault();
    std::string language = system.getLanguage();
    const std::string country = system.getCountry();
    if (language.empty() || language == "c" || language == "posix")
      return std::string("en-US,en");
    if (country.empty())
      return language;
    return language + "-" + country + "," + language;
  }());
  return *languages;
}

void ApplyLanguages(content::WebContents* contents) {
  blink::RendererPreferences* prefs = contents->GetMutableRendererPrefs();
  prefs->accept_languages = AcceptLanguages();
  // The focus ring's colour, as Chrome sets it (renderer_preferences_util.cc):
  // Blink's own default, 0xE59700, is a placeholder that draws an orange ring
  // round every focused field.
  prefs->focus_ring_color = BUILDFLAG(IS_MAC) ? SkColorSetRGB(0x00, 0x5F, 0xCC) : SkColorSetRGB(0x10, 0x10, 0x10);
  contents->SyncRendererPrefs();
}

// Chrome's, from the locale.
std::string BrowserClient::GetAcceptLangs(content::BrowserContext* context) {
  return AcceptLanguages();
}

// The user agent is Chrome's with --headless too, not "HeadlessChrome":
// headless views are shown to people in applications, and sites take
// HeadlessChrome for a bot. Client hints never name it. Chrome's headless
// string with --zebkit-headless-user-agent; --user-agent always wins.
std::string BrowserClient::GetUserAgent() {
  std::string agent = embedder_support::GetUserAgent();
  const base::CommandLine& command_line =
      *base::CommandLine::ForCurrentProcess();
  if (!command_line.HasSwitch(embedder_support::kUserAgent) &&
      !command_line.HasSwitch("zebkit-headless-user-agent")) {
    base::ReplaceFirstSubstringAfterOffset(&agent, 0, "HeadlessChrome/",
                                           "Chrome/");
  }
  return agent;
}

// Popups are blocked as Chrome blocks them: a page opens a window or a tab
// only while the person acts (a click, a key), not from a timer or on load
// (blocked_content::ShouldBlockPopup and ConsiderForPopupBlocking, whose
// rule this is). --disable-popup-blocking lets every one open, as in Chrome.
void BrowserClient::CreateThrottlesForNavigation(content::NavigationThrottleRegistry& registry) {
  AddNavigationPolicyThrottle(registry);
}
void BrowserClient::CreateThrottlesForCommitWithoutUrlLoader(content::NavigationThrottleRegistry& registry) {
  AddNavigationPolicyThrottle(registry);
}
std::vector<std::unique_ptr<content::CommitDeferringCondition>>
BrowserClient::CreateCommitDeferringConditionsForNavigation(
    content::NavigationHandle* handle, content::CommitDeferringCondition::NavigationType type) {
  auto conditions = content::ContentBrowserClient::CreateCommitDeferringConditionsForNavigation(handle, type);
  auto policy = CreateNavigationActivationPolicy(handle,
      type == content::CommitDeferringCondition::NavigationType::kPrerenderedPageActivation);
  if (policy) conditions.push_back(std::move(policy));
  return conditions;
}

void BrowserClient::OverrideWebPreferences(content::WebContents* contents,
                                           content::SiteInstance& main_frame_site,
                                           blink::web_pref::WebPreferences* prefs) {
  const auto& command = *base::CommandLine::ForCurrentProcess();
  if (FollowsSystemAppearance() && !command.HasSwitch(switches::kForceDarkMode) &&
      !command.HasSwitch(switches::kForceHighContrast)) {
    // The system's, as Chrome takes them (appearance.cc).
    ApplySystemAppearance(prefs);
  } else {
    prefs->preferred_color_scheme = command.HasSwitch(switches::kForceDarkMode)
                                        ? blink::mojom::PreferredColorScheme::kDark
                                        : blink::mojom::PreferredColorScheme::kLight;
    const bool high_contrast = command.HasSwitch(switches::kForceHighContrast);
    prefs->in_forced_colors = high_contrast;
    prefs->preferred_contrast = high_contrast
                                    ? blink::mojom::PreferredContrast::kMore
                                    : blink::mojom::PreferredContrast::kNoPreference;
  }
  ApplyViewSettings(contents, prefs);
}

// The system's theme changed: whether the pages' preferences are now others.
bool BrowserClient::WebPreferencesNeedUpdateForColorRelatedStateChanges(
    content::WebContents& web_contents,
    const content::SiteInstance& main_frame_site) const {
  if (!FollowsSystemAppearance())
    return false;
  const blink::web_pref::WebPreferences& current = web_contents.GetOrCreateWebPreferences();
  blink::web_pref::WebPreferences wanted = current;
  ApplySystemAppearance(&wanted);
  ApplyViewSettings(&web_contents, &wanted);
  return wanted.preferred_color_scheme != current.preferred_color_scheme ||
         wanted.preferred_contrast != current.preferred_contrast ||
         wanted.in_forced_colors != current.in_forced_colors;
}

bool BrowserClient::CanCreateWindow(
    content::RenderFrameHost* opener,
    const GURL& opener_url,
    const GURL& opener_top_level_frame_url,
    const url::Origin& source_origin,
    content::mojom::WindowContainerType container_type,
    const GURL& target_url,
    const content::Referrer& referrer,
    const std::string& frame_name,
    WindowOpenDisposition disposition,
    const blink::mojom::WindowFeatures& features,
    bool user_gesture,
    bool opener_suppressed,
    bool* no_javascript_access) {
  bool opens_window = false;
  switch (disposition) {
    case WindowOpenDisposition::NEW_POPUP:
    case WindowOpenDisposition::NEW_FOREGROUND_TAB:
    case WindowOpenDisposition::NEW_BACKGROUND_TAB:
    case WindowOpenDisposition::NEW_WINDOW:
    case WindowOpenDisposition::NEW_PICTURE_IN_PICTURE:
    case WindowOpenDisposition::NEW_SPLIT_VIEW:
      opens_window = true;
      break;
    default:
      break;
  }
  zebkit_view* view = opener ? ViewOf(content::WebContents::FromRenderFrameHost(opener)) : nullptr;
  const zebkit_popup_policy policy = zebkit_view_get_popup_policy(view);
  const bool blocked_by_policy = opens_window &&
      (policy == ZEBKIT_POPUP_POLICY_BLOCK ||
       (policy == ZEBKIT_POPUP_POLICY_BLOCK_WITHOUT_GESTURE && !user_gesture));
  const bool allowed = !blocked_by_policy &&
      content::ContentBrowserClient::CanCreateWindow(
          opener, opener_url, opener_top_level_frame_url, source_origin,
          container_type, target_url, referrer, frame_name, disposition, features,
          user_gesture, opener_suppressed, no_javascript_access);
  // Automation's popups wait for their context's settings (view.h).
  if (allowed && opener) {
    if (PopupHoldFunc hook = PopupHoldHook()) {
      hook(content::WebContents::FromRenderFrameHost(opener), true);
      // Unless HandlePopup() did: a window that was not made after all.
      content::GetUIThreadTaskRunner({})->PostTask(
          FROM_HERE, base::BindOnce([] {
            if (PopupHoldFunc hook = PopupHoldHook())
              hook(nullptr, false);
          }));
    }
  }
  if (opens_window && view && view->on_window_open_event_) {
    base::DictValue event;
    event.Set("phase", "decision"); event.Set("allowed", allowed);
    event.Set("url", target_url.spec()); event.Set("frameName", frame_name);
    event.Set("userGesture", user_gesture);
    event.Set("disposition", WindowDispositionName(disposition));
    event.Set("containerType", static_cast<int>(container_type));
    event.Set("openerUrl", opener_url.spec());
    event.Set("topUrl", opener_top_level_frame_url.spec());
    event.Set("sourceOrigin", source_origin.Serialize());
    event.Set("referrer", referrer.url.spec());
    event.Set("openerSuppressed", opener_suppressed);
    event.Set("features", WindowFeaturesInfo(features));
    NotifyWindowOpenEvent(view, std::move(event));
  }
  if (!allowed) {
    if (view && view->on_popup_blocked_) {
      base::DictValue event;
      event.Set("url", target_url.spec());
      event.Set("frameName", frame_name);
      event.Set("userGesture", user_gesture);
      content::GetUIThreadTaskRunner({})->PostTask(
          FROM_HERE, base::BindOnce(
              [](base::WeakPtr<zebkit_view> view, std::string json) {
                if (view && view->on_popup_blocked_)
                  view->on_popup_blocked_(view.get(), json.c_str(), view->on_popup_blocked_data_);
              }, view->GetWeakPtr(), base::WriteJson(event).value_or("{}")));
    }
    return false;
  }
  return true;
}

// The product CDP names in /json/version and Browser.getVersion, as
// Chrome names it ("Chrome/157.0.8103.38"): tools read it to know what they
// drive.
std::string BrowserClient::GetProduct() {
  return std::string(version_info::GetProductNameAndVersionForUserAgent());
}

// WebUSB, WebHID, Web Serial: their choosers are the application's (device_choosers.h).
content::UsbDelegate* BrowserClient::GetUsbDelegate() {
  return UsbChooserDelegate();
}
content::HidDelegate* BrowserClient::GetHidDelegate() {
  return HidChooserDelegate();
}
content::SerialDelegate* BrowserClient::GetSerialDelegate() {
  return SerialChooserDelegate();
}

content::WebAuthenticationDelegate* BrowserClient::GetWebAuthenticationDelegate() {
  return GetWebAuthnDelegate();
}
std::unique_ptr<content::AuthenticatorRequestClientDelegate>
BrowserClient::GetWebAuthenticationRequestDelegate(content::RenderFrameHost* frame) {
  return CreateWebAuthnRequestDelegate(frame);
}

// The network settings of the engine's own (below).
void ConfigureNetworkDefaults(content::BrowserContext* context,
                              network::mojom::NetworkContextParams* context_params);

void BrowserClient::ConfigureNetworkContextParams(
    content::BrowserContext* context,
    bool in_memory,
    const base::FilePath& relative_partition_path,
    network::mojom::NetworkContextParams* params,
    cert_verifier::mojom::CertVerifierCreationParams* cert_params) {
  params->user_agent = GetUserAgent();
  params->enable_zstd = true;
  params->device_bound_sessions_enabled =
      base::FeatureList::IsEnabled(net::features::kDeviceBoundSessions);
  ConfigureNetworkDefaults(context, params);
  ConfigurePersistentCookies(context, in_memory, relative_partition_path, params);
}

// Third-party cookies are blocked, as Safari and Firefox block them: an
// application's pages rarely need them, and advertising opens a frame, and
// with site isolation a process, for each network that would sync one
// (53 such frames on one news site, 2 without). The application allows them
// with --zebkit-allow-third-party-cookies.
void ConfigureNetworkDefaults(content::BrowserContext* context,
                              network::mojom::NetworkContextParams* context_params) {
  if (!context_params->cookie_manager_params)
    context_params->cookie_manager_params =
        network::mojom::CookieManagerParams::New();
  // The switch is the default of zebkit_set_engine_settings's thirdPartyCookies.
  context_params->cookie_manager_params->block_third_party_cookies =
      BlocksThirdPartyCookies(context);
  // Accept-Language as Chrome sends it: each language with its weight
  // ("en-US,en;q=0.9"), not the list as it is.
  context_params->accept_language = net::HttpUtil::GenerateAcceptLanguageHeader(
      net::HttpUtil::ExpandLanguageList(AcceptLanguages()));
  static constexpr auto kProxyAnnotation = net::DefineNetworkTrafficAnnotation(
      "zebkit_application_proxy", R"(
        semantics {
          sender: "zebkit embedding application"
          description: "Network traffic uses the proxy selected by the embedding application."
          trigger: "The application configures a proxy through startup settings or switches."
          data: "The requests the application or its pages send."
          destination: OTHER
        }
        policy {
          cookies_allowed: YES
          setting: "The embedding application selects the proxy and cookie policy."
          policy_exception_justification: "An embedding application's own network configuration."
        })");
  // An automation context's own (Browser.newContext's proxy).
  if (std::optional<std::pair<std::string, std::string>> rules = AutomationProxyFor(context)) {
    net::ProxyConfig proxy;
    proxy.proxy_rules().ParseFromString(rules->first);
    proxy.proxy_rules().bypass_rules.ParseFromString(rules->second);
    context_params->initial_proxy_config = net::ProxyConfigWithAnnotation(proxy, kProxyAnnotation);
    context_params->proxy_config_client_receiver.reset();
    return;
  }
  const auto& command = *base::CommandLine::ForCurrentProcess();
  if (command.HasSwitch("no-proxy-server") || command.HasSwitch("proxy-server") ||
      command.HasSwitch("proxy-pac-url") || command.HasSwitch("proxy-auto-detect")) {
    net::ProxyConfig proxy;
    if (!command.HasSwitch("no-proxy-server")) {
      if (command.HasSwitch("proxy-server")) {
        proxy.proxy_rules().ParseFromString(command.GetSwitchValueASCII("proxy-server"));
        if (command.HasSwitch("proxy-bypass-list"))
          proxy.proxy_rules().bypass_rules.ParseFromString(command.GetSwitchValueASCII("proxy-bypass-list"));
      } else if (command.HasSwitch("proxy-pac-url")) {
        proxy = net::ProxyConfig::CreateFromCustomPacURL(GURL(command.GetSwitchValueASCII("proxy-pac-url")));
      } else {
        proxy.set_auto_detect(true);
      }
    }
    context_params->initial_proxy_config = net::ProxyConfigWithAnnotation(proxy, kProxyAnnotation);
    context_params->proxy_config_client_receiver.reset();
  }
}

// A browser starts a renderer ahead of the next navigation; an application
// showing its pages mostly pays for one in memory, as Electron does not by
// default. --zebkit-spare-renderer asks for it.
bool BrowserClient::ShouldUseSpareRenderProcessHost(
    content::BrowserContext* browser_context,
    const GURL& site_url,
    std::optional<SpareProcessRefusedByEmbedderReason>& refused_reason) {
  // "spareRenderer" of zebkit_set_engine_settings; the switch is its default.
  if (UsesSpareRenderer()) {
    refused_reason = std::nullopt;
    return true;
  }
  refused_reason = SpareProcessRefusedByEmbedderReason::DefaultDisabled;
  return false;
}

mojo::PendingRemote<network::mojom::URLLoaderFactory>
BrowserClient::CreateNonNetworkNavigationURLLoaderFactory(
    const std::string& scheme,
    content::FrameTreeNodeId frame_tree_node_id) {
  if (IsApplicationScheme(scheme))
    return CreateSchemeLoaderFactory(scheme);
  return content::ContentBrowserClient::CreateNonNetworkNavigationURLLoaderFactory(
      scheme, frame_tree_node_id);
}

void BrowserClient::AllowCertificateError(content::WebContents* contents, int error,
    const net::SSLInfo& info, const GURL& url, bool main_frame, bool strict,
    base::OnceCallback<void(content::CertificateRequestResultType)> reply) {
  HandleCertificateError(contents, error, info, url, main_frame, strict, std::move(reply));
}

bool BrowserClient::HandleExternalProtocol(
    const GURL& url,
    base::RepeatingCallback<content::WebContents*()> web_contents_getter,
    content::FrameTreeNodeId frame_tree_node_id,
    content::NavigationUIData* navigation_data,
    bool is_primary_main_frame,
    bool is_in_fenced_frame_tree,
    network::mojom::WebSandboxFlags sandbox_flags,
    ui::PageTransition page_transition,
    bool has_user_gesture,
    const std::optional<url::Origin>& initiating_origin,
    content::RenderFrameHost* initiator_document,
    const net::IsolationInfo& isolation_info,
    mojo::PendingRemote<network::mojom::URLLoaderFactory>* out_factory) {
  TellExternalProtocol(url, web_contents_getter.Run(), frame_tree_node_id,
                       is_primary_main_frame, is_in_fenced_frame_tree, sandbox_flags,
                       has_user_gesture, initiating_origin, initiator_document);
  // Nothing is opened here. The navigation ends without an error page, as
  // Chrome's does when it has handed a link to another program.
  return true;
}

// The operating system's identities (NSS's database, Keychain, the user's
// Windows store), only when the application allows them
// (zebkit_set_engine_settings's clientCertificateStores): their keys stay in
// the store, which signs.
std::unique_ptr<net::ClientCertStore> BrowserClient::CreateClientCertStore(
    content::BrowserContext* browser_context) {
  if (!UsesClientCertificateStores())
    return nullptr;
#if BUILDFLAG(USE_NSS_CLIENT_CERTS)
  // No password delegate: identities whose key needs the database's password
  // are not offered (hooks.h).
  return std::make_unique<net::ClientCertStoreNSS>(net::ClientCertStoreNSS::PasswordDelegateFactory());
#elif BUILDFLAG(IS_MAC)
  return std::make_unique<net::ClientCertStoreMac>();
#elif BUILDFLAG(IS_WIN)
  return std::make_unique<net::ClientCertStoreWin>();
#else
  return nullptr;
#endif
}

base::OnceClosure BrowserClient::SelectClientCertificate(
    content::BrowserContext* browser_context,
    int process_id,
    content::WebContents* web_contents,
    net::SSLCertRequestInfo* cert_request_info,
    net::ClientCertIdentityList client_certs,
    std::unique_ptr<content::ClientCertificateDelegate> delegate) {
  // Browser.newContext's clientCertificates first.
  if (AutomationSelectClientCertificate(browser_context, *cert_request_info, &delegate))
    return base::OnceClosure();
  return HandleClientCertificate(web_contents, *cert_request_info, std::move(client_certs),
                                 std::move(delegate));
}

std::unique_ptr<content::LoginDelegate> BrowserClient::CreateLoginDelegate(
    const net::AuthChallengeInfo& auth_info,
    content::WebContents* web_contents,
    content::BrowserContext* browser_context,
    const content::GlobalRequestID& request_id,
    bool is_request_for_primary_main_frame_navigation,
    bool is_request_for_navigation,
    const GURL& url,
    scoped_refptr<net::HttpResponseHeaders> response_headers,
    bool first_auth_attempt,
    bool do_not_prompt_for_login,
    content::GuestPageHolder* guest_page_holder,
    content::LoginDelegate::LoginAuthRequiredCallback auth_required_callback) {
  // A proxy's challenge that must not ask anyone (Chromium 157 brings these
  // here; before, the network service cancelled them): no one is asked.
  if (do_not_prompt_for_login)
    return nullptr;
  if (auto delegate = CreateHttpAuthDelegate(auth_info, web_contents, url,
          is_request_for_primary_main_frame_navigation, is_request_for_navigation,
          first_auth_attempt, auth_required_callback)) return delegate;
  if (browser_context) {
    // Only taken when automation answers.
    auto callback = std::make_shared<
        content::LoginDelegate::LoginAuthRequiredCallback>(
        std::move(auth_required_callback));
    std::unique_ptr<content::LoginDelegate> delegate =
        CreateAutomationLoginDelegate(
            auth_info, browser_context, web_contents,
            is_request_for_primary_main_frame_navigation, url, first_auth_attempt,
            base::BindOnce(
                [](std::shared_ptr<content::LoginDelegate::LoginAuthRequiredCallback>
                       callback,
                   const std::optional<net::AuthCredentials>& credentials) {
                  if (*callback)
                    std::move(*callback).Run(credentials);
                },
                callback));
    if (delegate)
      return delegate;
    auth_required_callback = std::move(*callback);
  }
  // Neither the application nor automation answers: the engine's sign-in
  // dialog where the page has a window (sign_in_dialog.h).
  if (auto dialog = CreateSignInDialogDelegate(auth_info, web_contents, url, auth_required_callback))
    return dialog;
  return nullptr;
}

void BrowserClient::WillCreateURLLoaderFactory(
    content::BrowserContext* browser_context,
    content::RenderFrameHost* frame,
    int render_process_id,
    URLLoaderFactoryType type,
    const url::Origin& request_initiator,
    const net::IsolationInfo& isolation_info,
    std::optional<int64_t> navigation_id,
    ukm::SourceIdObj ukm_source_id,
    network::URLLoaderFactoryBuilder& factory_builder,
    mojo::PendingRemote<network::mojom::TrustedURLLoaderHeaderClient>*
        header_client,
    bool* bypass_redirect_checks,
    bool* disable_secure_dns,
    network::mojom::URLLoaderFactoryOverridePtr* factory_override,
    scoped_refptr<base::SequencedTaskRunner> navigation_response_task_runner,
    bool is_for_network_service) {
  // Dedicated workers have their ancestor frame; shared and service workers
  // have no view. Ahead of automation's, as ahead of CDP's.
  if (type == URLLoaderFactoryType::kNavigation ||
      type == URLLoaderFactoryType::kDocumentSubResource) {
    InterceptNetworkHookFactory(browser_context, frame, NetworkHookFactory::kFrame, factory_builder, header_client);
  } else if (type == URLLoaderFactoryType::kWorkerMainResource ||
             type == URLLoaderFactoryType::kWorkerSubResource) {
    InterceptNetworkHookFactory(browser_context, frame, NetworkHookFactory::kWorker, factory_builder, header_client);
  } else if (type == URLLoaderFactoryType::kServiceWorkerScript ||
             type == URLLoaderFactoryType::kServiceWorkerSubResource) {
    InterceptNetworkHookFactory(browser_context, frame, NetworkHookFactory::kServiceWorker, factory_builder, header_client);
  }
  // A dedicated worker's requests too, as its page's (Playwright reports and
  // routes them on the page).
  if (type == URLLoaderFactoryType::kNavigation ||
      type == URLLoaderFactoryType::kDocumentSubResource ||
      type == URLLoaderFactoryType::kWorkerMainResource ||
      type == URLLoaderFactoryType::kWorkerSubResource) {
    InterceptURLLoaderFactory(frame, factory_builder);
  }
  // Below the network's CORS, as DevTools' Fetch: the preflights of a page
  // that routes.
  if (type == URLLoaderFactoryType::kDocumentSubResource ||
      type == URLLoaderFactoryType::kWorkerSubResource) {
    InterceptCorsPreflights(frame, factory_override);
  }
}

bool BrowserClient::WillInterceptWebSocket(content::RenderFrameHost* frame) {
  return WillInterceptNetworkHookWebSocket(frame);
}

void BrowserClient::CreateWebSocket(
    content::RenderFrameHost* frame,
    WebSocketFactory factory,
    const GURL& url,
    const net::SiteForCookies& site_for_cookies,
    const std::optional<std::string>& user_agent,
    mojo::PendingRemote<network::mojom::WebSocketHandshakeClient> handshake_client,
    WebSocketOptions options) {
  CreateNetworkHookWebSocket(frame, std::move(factory), url, user_agent, std::move(handshake_client),
                             std::move(options.header_client));
}

void BrowserClient::RegisterNonNetworkSubresourceURLLoaderFactories(
    int render_process_id,
    int render_frame_id,
    const std::optional<url::Origin>& request_initiator_origin,
    NonNetworkURLLoaderFactoryMap* factories) {
  AddSchemeLoaderFactories(factories);
}

void BrowserClient::RegisterNonNetworkWorkerMainResourceURLLoaderFactories(
    content::BrowserContext* browser_context,
    const std::optional<url::Origin>& request_initiator,
    network::mojom::RequestDestination request_destination,
    NonNetworkURLLoaderFactoryMap* factories) {
  AddSchemeLoaderFactories(factories);
}

namespace {

// A key the page did not take: the application menu's, where windows have
// no menu bar (app.h).
bool HandleKey(Window* window,
               const input::NativeWebKeyboardEvent& event) {
  const std::string id = MenuItemForKey(event);
  if (id.empty())
    return false;
  RunMenuItem(id);
  return true;
}

}  // namespace

void FocusWindow(Window* window) {
  PlatformFor(window->kind())->BringToFront(window);
}

void SetWindowSizeLimits(Window* window,
                         const gfx::Size& minimum,
                         bool resizable) {
  PlatformFor(window->kind())->SetSizeLimits(window, minimum, resizable);
}

BrowserMainParts::BrowserMainParts() = default;
BrowserMainParts::~BrowserMainParts() = default;

void BrowserMainParts::PostCreateMainMessageLoop() {
#if BUILDFLAG(IS_LINUX)
  bluez::DBusBluezManagerWrapperLinux::Initialize();
#endif
}

void BrowserMainParts::ToolkitInitialized() {
#if BUILDFLAG(IS_LINUX)
  ui::LinuxUi::SetInstance(ui::GetDefaultLinuxUi());
#if defined(ZEBKIT_COOKIES_USE_DBUS)
  InstallIBusInputMethod();
#endif
#endif
}

int BrowserMainParts::PostCreateThreads() {
  performance_manager_lifetime_ =
      std::make_unique<performance_manager::PerformanceManagerLifetime>(
          performance_manager::GraphFeatures::WithMinimal(), base::DoNothing());
  return content::RESULT_CODE_NORMAL_EXIT;
}

void BrowserMainParts::WillRunMainMessageLoop(
    std::unique_ptr<base::RunLoop>& run_loop) {
  Window::SetMainMessageLoopQuitClosure(run_loop->QuitClosure());
}

void BrowserMainParts::PostDestroyThreads() {
#if BUILDFLAG(IS_LINUX)
  device::BluetoothAdapterFactory::Shutdown();
  bluez::DBusBluezManagerWrapperLinux::Shutdown();
#endif
}

#if BUILDFLAG(IS_MAC)
void BrowserMainParts::PreCreateMainMessageLoop() {
  if (!g_external_loop || g_application_integration)
    SetUpApplication(zebkit_is_headless());
}
#endif

int BrowserMainParts::PreMainMessageLoopRun() {
  browser_context_ = std::make_unique<Profile>(false);
  off_the_record_browser_context_ = std::make_unique<Profile>(true);
  // Persistent origin trials are needed before the first request.
  browser_context_->GetOriginTrialsControllerDelegate();
  off_the_record_browser_context_->GetOriginTrialsControllerDelegate();
  InstallClientHints();
  InstallPermissionHooks();
#if BUILDFLAG(IS_MAC)
  HookHostAccessibility(true);
#endif
  InstallCertificateHooks();
  InstallHttpAuthHooks();
  InstallNavigationHooks();
  InstallPageHooks();
  InstallShareHooks();
  InstallDownloadHooks();
  StartRuntimeSettings();
  InstallClientCertificateHooks();
  InstallNetworkHooks();
  AllowSchemes();
  Window::SetNewWindowHandler(base::BindRepeating(&HandleNewWindow));
  Window::SetPopupHandler(base::BindRepeating(&HandlePopup));
  // Playwright's dialogs and file choosers, and file choosers without a
  // display cancelled (automation.cc).
  InstallAutomationHooks();
  Window::SetKeyboardEventHandler(base::BindRepeating(&HandleKey));
#if BUILDFLAG(IS_MAC)
  SetUpViews();
#endif
  InitializePlatforms();
  if (FollowsSystemAppearance())
    StartSystemAppearance();
  net::NetModule::SetResourceProvider(&NetResourceProvider);
  StartCdpFromCommandLine();
  return 0;
}

void BrowserMainParts::PostMainMessageLoopRun() {
#if BUILDFLAG(IS_MAC)
  HookHostAccessibility(false);
#endif
  StopSystemAppearance();
  CdpStop();
  StopNetworkQuality();
  StopNavigationHooks();
  StopPermissionHooks();
  StopCertificateHooks();
  StopHttpAuthHooks();
  StopPageHooks();
  StopShareHooks();
  StopRuntimeSettings();
  StopDownloadHooks();
  StopClientCertificateHooks();
  StopNetworkHooks();
  CloseAllViews();
  Window::Shutdown();
  DCHECK(Window::windows().empty());
  browser_context_.reset();
  off_the_record_browser_context_.reset();
#if BUILDFLAG(IS_LINUX)
  ui::LinuxUi::SetInstance(nullptr);
#endif
  performance_manager_lifetime_.reset();
}

content::BrowserContext* GetBrowserContext() {
  return BrowserClient::Get()->browser_context();
}

content::BrowserContext* ProfileOf(zebkit_context* context) {
  return context && context->profile ? context->profile.get()
                                      : GetBrowserContext();
}

void ConfigureContext(content::BrowserContext* profile) {
  InstallClientHintsFor(profile);
}

}  // namespace zebkit

namespace {

// Content's normal entry point performs process setup before calling Run(),
// then immediately shuts down. Keep its state alive for the host's loop;
// browser teardown must precede the content runner's teardown.
class EmbeddedRunner : public content::ContentMainRunner {
 public:
  int Initialize(content::ContentMainParams params) override {
    return runner_->Initialize(std::move(params));
  }
  void ReInitializeParams(content::ContentMainParams params) override {
    runner_->ReInitializeParams(std::move(params));
  }
  int Run() override { return runner_->Run(); }
  void Shutdown() override {}
  void Finish() { runner_.reset(); }

 private:
  std::unique_ptr<content::ContentMainRunner> runner_ =
      content::ContentMainRunner::Create();
};

struct EmbeddedEngine {
  std::vector<std::string> args;
  std::vector<const char*> argv;
  zebkit::MainDelegate delegate{nullptr, nullptr, true};
  EmbeddedRunner runner;
  base::PlatformThreadRef thread = base::PlatformThread::CurrentRef();
  bool pumping = false;
  bool ready = false;
};

// Chromium cannot be initialized twice in one process, even after shutdown.
bool g_engine_started = false;
EmbeddedEngine* g_embedded = nullptr;
thread_local bool g_embedded_thread = false;

}  // namespace

extern "C" {

bool zebkit_view_set_popup_policy(zebkit_view* view, zebkit_popup_policy policy) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!view || !view->web_contents() ||
      (policy != ZEBKIT_POPUP_POLICY_BLOCK_WITHOUT_GESTURE &&
       policy != ZEBKIT_POPUP_POLICY_ALLOW && policy != ZEBKIT_POPUP_POLICY_BLOCK))
    return false;
  view->popup_policy_ = policy;
  return true;
}

zebkit_popup_policy zebkit_view_get_popup_policy(zebkit_view* view) {
  if (view && view->popup_policy_)
    return *view->popup_policy_;
  if (auto engine = zebkit::EnginePopupPolicy())
    return *engine;
  return base::CommandLine::ForCurrentProcess()->HasSwitch(embedder_support::kDisablePopupBlocking)
      ? ZEBKIT_POPUP_POLICY_ALLOW : ZEBKIT_POPUP_POLICY_BLOCK_WITHOUT_GESTURE;
}

void zebkit_view_on_popup_blocked(zebkit_view* view, zebkit_popup_blocked_func func, void* data) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!view)
    return;
  view->on_popup_blocked_ = func;
  view->on_popup_blocked_data_ = data;
}

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

int zebkit_main(int argc, char** argv, zebkit_task_func ready, void* data) {
  if (g_engine_started || argc < 0 || (argc && !argv))
    return 1;
#if BUILDFLAG(IS_WIN)
  std::vector<std::string> utf8_store;
  std::vector<char*> utf8_argv;
  if (argc > 0) {
    zebkit::WindowsArgumentsAsUTF8(argc, argv, &utf8_store, &utf8_argv);
    argv = utf8_argv.data();
  }
#endif
  g_engine_started = true;
#if BUILDFLAG(IS_MAC)
  zebkit::SetUpBundle(zebkit::LibraryDirectory());
#endif
  for (int i = 0; i < argc; ++i) {
    if (!argv[i])
      return 1;
  }
#if BUILDFLAG(IS_LINUX)
  zebkit::PrefetchEngineFiles(argc, argv);
#endif
  std::vector<std::string> args = zebkit::EngineArguments(argc, argv);
  if (!zebkit::PrepareCookieStorage(args))
    return 1;
  std::vector<char*> engine_argv;
  for (std::string& arg : args)
    engine_argv.push_back(arg.data());
  engine_argv.push_back(nullptr);
  static base::NoDestructor<zebkit::MainDelegate> delegate(ready, data);
  return zebkit::RunContent(static_cast<int>(args.size()), engine_argv.data(),
                            delegate.get());
}

int zebkit_init(int argc, char** argv) {
  return zebkit_init_with_settings(argc, argv, nullptr, nullptr);
}

int zebkit_init_with_settings(int argc, char** argv, const char* settings_json, char** error) {
  if (error)
    *error = nullptr;
  auto fail = [error](const std::string& message) {
    if (error)
      *error = strdup(message.c_str());
    return 1;
  };
  if (g_engine_started || argc < 0 || (argc && !argv))
    return fail("Engine startup was already attempted, or argv is invalid");
#if BUILDFLAG(IS_WIN)
  std::vector<std::string> utf8_store;
  std::vector<char*> utf8_argv;
  if (argc > 0) {
    zebkit::WindowsArgumentsAsUTF8(argc, argv, &utf8_store, &utf8_argv);
    argv = utf8_argv.data();
  }
#endif
  std::vector<std::string> input;
  std::vector<std::string> positional;
  for (int i = 0; i < argc; ++i) {
    if (!argv[i])
      return fail("argv contains a null argument");
    // After "--" nothing is a switch: what is added below has to come before.
    if (!positional.empty() || !strcmp(argv[i], "--"))
      positional.emplace_back(argv[i]);
    else
      input.emplace_back(argv[i]);
  }
  if (input.empty())
    input.emplace_back("zebkit");
#if BUILDFLAG(IS_LINUX)
  zebkit::PrefetchEngineFiles(argc, argv);
#endif
  bool application_integration = false;
  std::string problem;
  if (!zebkit::ApplyStartupSettings(settings_json, &input, &application_integration, &problem))
    return fail(problem);
  // Chromium already exposes switches for these two process-wide behaviors.
  // The rest is controlled by ContentMainParams::preserve_process_state.
  input.emplace_back("--disable-main-thread-name-override");
  input.emplace_back("--disable-in-process-stack-traces");
#if BUILDFLAG(IS_POSIX)
  // Chromium ignores SIGPIPE in its own signal setup, which an embedded engine
  // skips; its raw writes (--remote-debugging-pipe to a client that has gone)
  // would kill the application. A handler the application set is left alone.
  struct sigaction current = {};
  if (!sigaction(SIGPIPE, nullptr, &current) && current.sa_handler == SIG_DFL)
    signal(SIGPIPE, SIG_IGN);
#endif
  std::vector<char*> input_argv;
  for (auto& arg : input)
    input_argv.push_back(arg.data());
  input_argv.push_back(nullptr);
#if BUILDFLAG(IS_MAC)
  zebkit::SetUpBundle(zebkit::LibraryDirectory());
#endif
  auto engine = std::make_unique<EmbeddedEngine>();
  engine->args = zebkit::EngineArguments(static_cast<int>(input.size()), input_argv.data());
  engine->args.insert(engine->args.end(), positional.begin(), positional.end());
  if (!zebkit::PrepareCookieStorage(engine->args))
    return fail("Cookie storage could not be initialized");
  if (!zebkit::InstallHostLoop())
    return fail("The UI message pump factory was already configured");
  for (const auto& arg : engine->args)
    engine->argv.push_back(arg.c_str());
  engine->argv.push_back(nullptr);
  content::ContentMainParams params(&engine->delegate);
  params.preserve_process_state = true;
#if BUILDFLAG(IS_WIN)
  zebkit::SetUpWindowsMainParams(static_cast<int>(engine->args.size()),
                                 const_cast<char**>(engine->argv.data()), &params);
#else
  params.argc = static_cast<int>(engine->args.size());
  params.argv = engine->argv.data();
#endif
  g_engine_started = true;
  zebkit::g_external_loop = true;
  zebkit::g_application_integration = application_integration;
#if defined(ZEBKIT_COOKIES_USE_DBUS)
  dbus::Bus::SetRequireExplicitSessionBusAddress(true);
#endif
  const int result = content::RunContentProcess(std::move(params), &engine->runner);
  if (result != 0) {
    zebkit::StopHostLoop();
    engine->delegate.ShutdownBrowser();
    engine->runner.Finish();
    if (error)
      *error = strdup(("Engine initialization failed with exit code " + std::to_string(result)).c_str());
    return result;
  }
  engine->ready = true;
  g_embedded = engine.release();
  g_embedded_thread = true;
  return 0;
}

bool zebkit_set_wakeup(zebkit_wakeup_func func, void* data) {
  if (g_engine_started)
    return false;
  zebkit::ConfigureHostWakeup(func, data);
  return true;
}

bool zebkit_pump(void) {
  if (!g_embedded_thread || !g_embedded || !g_embedded->ready || g_embedded->pumping ||
      g_embedded->thread != base::PlatformThread::CurrentRef())
    return false;
  g_embedded->pumping = true;
  base::RunLoop().RunUntilIdle();
  g_embedded->pumping = false;
  return true;
}

bool zebkit_shutdown(void) {
  if (!g_embedded_thread || !g_embedded || !g_embedded->ready || g_embedded->pumping ||
      g_embedded->thread != base::PlatformThread::CurrentRef())
    return false;
  std::unique_ptr<EmbeddedEngine> engine(g_embedded);
  g_embedded = nullptr;
  g_embedded_thread = false;
  engine->ready = false;
  zebkit::StopHostLoop();
  engine->delegate.ShutdownBrowser();
  engine->runner.Finish();
  return true;
}

int zebkit_helper_main(int argc, char** argv) {
#if BUILDFLAG(IS_MAC)
  if (!zebkit::EnterHelperSandbox(argc, argv)) {
    fprintf(stderr, "zebkit: the sandbox could not be entered\n");
    abort();
  }
  zebkit::SetUpBundle(zebkit::LibraryDirectory());
#endif
  static base::NoDestructor<zebkit::MainDelegate> delegate(nullptr, nullptr);
  return zebkit::RunContent(argc, argv, delegate.get());
}

void zebkit_quit(void) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  // As a task: the caller may be a callback of a view being destroyed,
  // such as on_close, which the shutdown must not pull out from under it.
  content::GetUIThreadTaskRunner({})->PostTask(FROM_HERE, base::BindOnce([] {
                                                 zebkit::CloseAllViews();
                                                 // Embedded, the application ends
                                                 // the engine: zebkit_shutdown().
                                                 if (!g_embedded)
                                                   zebkit::Window::Shutdown();
                                               }));
}

void zebkit_post_task(zebkit_task_func func, void* data) {
  CHECK(g_engine_started) << "zebkit_post_task() before the engine started";
  content::GetUIThreadTaskRunner({})->PostTask(FROM_HERE,
                                               base::BindOnce(func, data));
}

void zebkit_post_delayed_task(zebkit_task_func func,
                              void* data,
                              uint32_t delay_ms) {
  CHECK(g_engine_started) << "zebkit_post_delayed_task() before the engine started";
  content::GetUIThreadTaskRunner({})->PostDelayedTask(
      FROM_HERE, base::BindOnce(func, data), base::Milliseconds(delay_ms));
}

bool zebkit_is_headless(void) {
#if BUILDFLAG(IS_OZONE)
  return base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
             switches::kOzonePlatform) == "headless";
#else
  return base::CommandLine::ForCurrentProcess()->HasSwitch(switches::kHeadless);
#endif
}

}  // extern "C"
