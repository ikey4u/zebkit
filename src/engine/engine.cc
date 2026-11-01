// zebkit_main(), zebkit_helper_main() and the classes that start the engine.

#ifdef UNSAFE_BUFFERS_BUILD
// The C API takes and returns C strings and arrays.
#pragma allow_unsafe_buffers
#endif

#include "content/public/common/webplugininfo.h"
#include "zebkit/engine/engine.h"

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

#include <dlfcn.h>
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
#include "base/functional/bind.h"
#include "base/logging.h"
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
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "sandbox/linux/services/credentials.h"
#include "sandbox/policy/switches.h"
#endif
#include "components/embedder_support/switches.h"
#include "net/http/http_util.h"
#include "net/proxy_resolution/proxy_config.h"
#include "net/proxy_resolution/proxy_config_with_annotation.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "third_party/blink/public/common/renderer_preferences/renderer_preferences.h"
#include "third_party/icu/source/common/unicode/locid.h"
#include "components/embedder_support/user_agent_utils.h"
#include "components/version_info/version_info_with_user_agent.h"
#include "content/public/app/content_main.h"
#include "content/public/app/content_main_runner.h"
#include "content/public/browser/browser_main_runner.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_switches.h"
#include "content/shell/browser/shell.h"
#include "content/shell/browser/shell_platform_delegate.h"
#include "content/shell/common/shell_switches.h"
#include "net/base/module/net_module.h"
#include "net/grit/net_resources.h"
#include "services/network/public/mojom/cookie_manager.mojom.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/mojom/websocket.mojom.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/base/ui_base_paths.h"
#include "ui/gfx/switches.h"
#include "ui/views/widget/widget.h"
#include "zebkit/engine/app.h"
#include "zebkit/engine/automation.h"
#include "zebkit/engine/automation_network.h"
#include "zebkit/engine/call.h"
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

// Where libzebkit is: the engine's data files and zebkit-helper are beside
// it. On macOS, the framework's version directory, which has them inside.
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

#if !BUILDFLAG(IS_MAC)
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
                   HelperPath(library).value());
  }
  if (!HasSwitch(argc, argv, switches::kContentShellUserDataDir)) {
    std::string config;
#if BUILDFLAG(IS_MAC)
    if (const char* home = getenv("HOME"); home && *home)
      config = std::string(home) + "/Library/Application Support";
#else
    if (const char* xdg = getenv("XDG_CONFIG_HOME"); xdg && *xdg)
      config = xdg;
    else if (const char* home = getenv("HOME"); home && *home)
      config = std::string(home) + "/.config";
#endif
    if (!config.empty()) {
      const std::string program = base::FilePath(args[0]).BaseName().value();
      args.push_back(std::string("--") + switches::kContentShellUserDataDir +
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
  args.push_back(std::string("--") + switches::kContentShellHideToolbar);
  // --window-size is the window's size, as in Chrome; the shell's own
  // switch for it still decides.
  {
    std::string size = SwitchValue(argc, argv, "window-size");
    const size_t comma = size.find(',');
    if (comma != std::string::npos &&
        !HasSwitch(argc, argv, switches::kContentShellHostWindowSize)) {
      args.push_back(std::string("--") + switches::kContentShellHostWindowSize +
                     "=" + size.substr(0, comma) + "x" + size.substr(comma + 1));
    }
  }
  // Features a browser wants and an application showing its pages does not,
  // as Electron leaves them out (its shell/browser/feature_list.cc): a spare
  // renderer kept warm for the next navigation, a process with its memory.
  // --zebkit-spare-renderer keeps it.
  //
  // And FedCM, which signs in with an identity provider through dialogs of
  // the browser's own: the content shell has none and turns every request
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

int RunContent(int argc, char** argv, MainDelegate* delegate) {
  content::ContentMainParams params(delegate);
  params.argc = argc;
  params.argv = const_cast<const char**>(argv);
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
        ui::DIR_LOCALES, library.Append("locales"), /*is_absolute=*/true,
        /*create=*/false);
  }
#endif
  auto result = content::ShellMainDelegate::BasicStartupComplete();
  const auto& command = *base::CommandLine::ForCurrentProcess();
#if BUILDFLAG(IS_LINUX)
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
         content::ShellMainDelegate::ShouldLoadV8Snapshot(process_type);
}

std::optional<int> MainDelegate::PreBrowserMain() {
  // The shell registers its own NSApplication subclass on macOS. An embedded
  // engine uses the application's existing NSApplication and native pump.
  return external_loop_ ? content::ContentMainDelegate::PreBrowserMain()
                        : content::ShellMainDelegate::PreBrowserMain();
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

void ContentClient::AddAdditionalSchemes(Schemes* schemes) {
  content::ShellContentClient::AddAdditionalSchemes(schemes);
  AddSchemes(schemes);
}

void ContentClient::AddPlugins(std::vector<content::WebPluginInfo>* plugins) {
  content::ShellContentClient::AddPlugins(plugins);
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
  content::ShellContentRendererClient::RenderThreadStarted();
  RegisterSchemesInRenderer();
}

std::variant<int, content::MainFunctionParams> MainDelegate::RunProcess(
    const std::string& process_type,
    content::MainFunctionParams main_function_params) {
  MarkICUInitialized();
  if (!process_type.empty()) {
    return content::ShellMainDelegate::RunProcess(
        process_type, std::move(main_function_params));
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
  set_browser_main_parts(parts.get());
  return parts;
}

bool BrowserClient::IsHandledURL(const GURL& url) {
  return (url.is_valid() && IsApplicationScheme(url.scheme())) ||
         content::ShellContentBrowserClient::IsHandledURL(url);
}

std::unique_ptr<content::WebContentsViewDelegate>
BrowserClient::GetWebContentsViewDelegate(content::WebContents* web_contents) {
  return WrapViewDelegate(
      web_contents,
      content::ShellContentBrowserClient::GetWebContentsViewDelegate(
          web_contents));
}

void BrowserClient::AppendExtraCommandLineSwitches(
    base::CommandLine* command_line,
    int child_process_id) {
  content::ShellContentBrowserClient::AppendExtraCommandLineSwitches(
      command_line, child_process_id);
  const auto& process = *base::CommandLine::ForCurrentProcess();
  for (const char* flag : {"enable-logging", "disable-logging", "log-file", "log-level"}) {
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
  contents->GetMutableRendererPrefs()->accept_languages = AcceptLanguages();
  contents->SyncRendererPrefs();
}

// The content shell's are "en-us,en", whatever the system's, and pages were
// told "en-US" alone (navigator.languages). Chrome's, from the locale.
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
  content::ShellContentBrowserClient::CreateThrottlesForNavigation(registry);
  AddNavigationPolicyThrottle(registry);
}
void BrowserClient::CreateThrottlesForCommitWithoutUrlLoader(content::NavigationThrottleRegistry& registry) {
  content::ShellContentBrowserClient::CreateThrottlesForCommitWithoutUrlLoader(registry);
  AddNavigationPolicyThrottle(registry);
}
std::vector<std::unique_ptr<content::CommitDeferringCondition>>
BrowserClient::CreateCommitDeferringConditionsForNavigation(
    content::NavigationHandle* handle, content::CommitDeferringCondition::NavigationType type) {
  auto conditions = content::ShellContentBrowserClient::CreateCommitDeferringConditionsForNavigation(handle, type);
  auto policy = CreateNavigationActivationPolicy(handle,
      type == content::CommitDeferringCondition::NavigationType::kPrerenderedPageActivation);
  if (policy) conditions.push_back(std::move(policy));
  return conditions;
}

void BrowserClient::OverrideWebPreferences(content::WebContents* contents,
                                           content::SiteInstance& main_frame_site,
                                           blink::web_pref::WebPreferences* prefs) {
  content::ShellContentBrowserClient::OverrideWebPreferences(contents, main_frame_site, prefs);
  ApplyViewSettings(contents, prefs);
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
      content::ShellContentBrowserClient::CanCreateWindow(
          opener, opener_url, opener_top_level_frame_url, source_origin,
          container_type, target_url, referrer, frame_name, disposition, features,
          user_gesture, opener_suppressed, no_javascript_access);
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
// Chrome names it ("Chrome/155.0.8059.84"): the content shell names none,
// and tools read it to know what they drive.
std::string BrowserClient::GetProduct() {
  return std::string(version_info::GetProductNameAndVersionForUserAgent());
}

content::WebAuthenticationDelegate* BrowserClient::GetWebAuthenticationDelegate() {
  return GetWebAuthnDelegate();
}
std::unique_ptr<content::AuthenticatorRequestClientDelegate>
BrowserClient::GetWebAuthenticationRequestDelegate(content::RenderFrameHost* frame) {
  return CreateWebAuthnRequestDelegate(frame);
}

void BrowserClient::ConfigureNetworkContextParams(
    content::BrowserContext* context,
    bool in_memory,
    const base::FilePath& relative_partition_path,
    network::mojom::NetworkContextParams* params,
    cert_verifier::mojom::CertVerifierCreationParams* cert_params) {
  content::ShellContentBrowserClient::ConfigureNetworkContextParams(
      context, in_memory, relative_partition_path, params, cert_params);
  ConfigurePersistentCookies(context, in_memory, relative_partition_path, params);
}

// Third-party cookies are blocked, as Safari and Firefox block them: an
// application's pages rarely need them, and advertising opens a frame, and
// with site isolation a process, for each network that would sync one
// (53 such frames on one news site, 2 without). The application allows them
// with --zebkit-allow-third-party-cookies.
void BrowserClient::ConfigureNetworkContextParamsForShell(
    content::BrowserContext* context,
    network::mojom::NetworkContextParams* context_params,
    cert_verifier::mojom::CertVerifierCreationParams*
        cert_verifier_creation_params) {
  content::ShellContentBrowserClient::ConfigureNetworkContextParamsForShell(
      context, context_params, cert_verifier_creation_params);
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
  return content::ShellContentBrowserClient::
      CreateNonNetworkNavigationURLLoaderFactory(scheme, frame_tree_node_id);
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

base::OnceClosure BrowserClient::SelectClientCertificate(
    content::BrowserContext* browser_context,
    int process_id,
    content::WebContents* web_contents,
    net::SSLCertRequestInfo* cert_request_info,
    net::ClientCertIdentityList client_certs,
    std::unique_ptr<content::ClientCertificateDelegate> delegate) {
  return HandleClientCertificate(web_contents, *cert_request_info, std::move(delegate));
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
    content::GuestPageHolder* guest_page_holder,
    content::LoginDelegate::LoginAuthRequiredCallback auth_required_callback) {
  if (auto delegate = CreateHttpAuthDelegate(auth_info, web_contents, url,
          is_request_for_primary_main_frame_navigation, is_request_for_navigation,
          first_auth_attempt, auth_required_callback)) return delegate;
  if (browser_context) {
    // Only taken when automation answers; otherwise the shell's own.
    auto callback = std::make_shared<
        content::LoginDelegate::LoginAuthRequiredCallback>(
        std::move(auth_required_callback));
    std::unique_ptr<content::LoginDelegate> delegate =
        CreateAutomationLoginDelegate(
            auth_info, browser_context, first_auth_attempt,
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
  return content::ShellContentBrowserClient::CreateLoginDelegate(
      auth_info, web_contents, browser_context, request_id,
      is_request_for_primary_main_frame_navigation, is_request_for_navigation,
      url, std::move(response_headers), first_auth_attempt, guest_page_holder,
      std::move(auth_required_callback));
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
    InterceptNetworkHookFactory(browser_context, frame, NetworkHookFactory::kFrame, factory_builder);
  } else if (type == URLLoaderFactoryType::kWorkerMainResource ||
             type == URLLoaderFactoryType::kWorkerSubResource) {
    InterceptNetworkHookFactory(browser_context, frame, NetworkHookFactory::kWorker, factory_builder);
  } else if (type == URLLoaderFactoryType::kServiceWorkerScript ||
             type == URLLoaderFactoryType::kServiceWorkerSubResource) {
    InterceptNetworkHookFactory(browser_context, frame, NetworkHookFactory::kServiceWorker, factory_builder);
  }
  if (type == URLLoaderFactoryType::kNavigation ||
      type == URLLoaderFactoryType::kDocumentSubResource) {
    InterceptURLLoaderFactory(frame, factory_builder);
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
  content::ShellContentBrowserClient::
      RegisterNonNetworkSubresourceURLLoaderFactories(
          render_process_id, render_frame_id, request_initiator_origin,
          factories);
  AddSchemeLoaderFactories(factories);
  // zebkit.* calls: from frames, which these factories are for.
  factories->emplace(kCallScheme, CreateCallLoaderFactory(render_process_id,
                                                          render_frame_id));
}

void BrowserClient::RegisterNonNetworkWorkerMainResourceURLLoaderFactories(
    content::BrowserContext* browser_context,
    const std::optional<url::Origin>& request_initiator,
    network::mojom::RequestDestination request_destination,
    NonNetworkURLLoaderFactoryMap* factories) {
  content::ShellContentBrowserClient::
      RegisterNonNetworkWorkerMainResourceURLLoaderFactories(
          browser_context, request_initiator, request_destination, factories);
  AddSchemeLoaderFactories(factories);
}

namespace {

// The content shell's windows, but the loop does not end when the last one
// closes: when an application is done is the application's to say
// (zebkit_quit()), or its script's (zebkit_script_on_idle()).
// The window's title: the application's, when it gave one.
std::u16string WindowTitle(content::Shell* shell, const std::u16string& page) {
  zebkit_view* view = ViewOf(shell->web_contents());
  return view && view->window_title_ ? base::UTF8ToUTF16(*view->window_title_)
                                     : page;
}

class PlatformDelegate : public content::ShellPlatformDelegate {
 public:
  void DidCloseLastWindow() override {}
  void DidCreateOrAttachWebContents(content::Shell* shell,
                                    content::WebContents* contents) override {
    content::ShellPlatformDelegate::DidCreateOrAttachWebContents(shell,
                                                                 contents);
    ApplyLanguages(contents);
  }
  void SetTitle(content::Shell* shell, const std::u16string& title) override {
    content::ShellPlatformDelegate::SetTitle(shell, WindowTitle(shell, title));
  }
  // Dialogs held for the application or a CDP client (dialog.cc).
  std::unique_ptr<content::JavaScriptDialogManager>
  CreateJavaScriptDialogManager(content::Shell* shell) override {
    return CreateDialogManager(shell);
  }
};

// A key the page did not take: the application menu's, where windows have
// no menu bar (app.h).
bool HandleKey(content::Shell* shell,
               const input::NativeWebKeyboardEvent& event) {
  const std::string id = MenuItemForKey(event);
  if (id.empty())
    return false;
  RunMenuItem(id);
  return true;
}

// The shell's platform delegate, for the windows' titles.
content::ShellPlatformDelegate* g_platform = nullptr;

}  // namespace

void UpdateWindowTitle(content::Shell* shell) {
  if (g_platform && shell->web_contents())
    g_platform->SetTitle(shell, shell->web_contents()->GetTitle());
}

void FocusWindow(content::Shell* shell) {
#if BUILDFLAG(IS_MAC)
  FocusNativeWindow(shell->window());
#else
  if (views::Widget* widget =
          views::Widget::GetWidgetForNativeWindow(shell->window())) {
    widget->Activate();
  }
#endif
  if (shell->web_contents())
    shell->web_contents()->Focus();
}

void SetWindowSizeLimits(content::Shell* shell,
                         const gfx::Size& minimum,
                         bool resizable) {
#if BUILDFLAG(IS_MAC)
  SetNativeWindowSizeLimits(shell->window(), minimum, resizable);
#else
  if (g_platform)
    g_platform->SetWindowSizeLimits(shell, minimum, resizable);
#endif
}

std::unique_ptr<content::ShellPlatformDelegate>
BrowserMainParts::CreateShellPlatformDelegate() {
  std::unique_ptr<content::ShellPlatformDelegate> platform;
#if BUILDFLAG(IS_MAC)
  if (zebkit_is_headless())
    platform = CreateHeadlessPlatformDelegate();
#endif
  if (!platform)
    platform = std::make_unique<PlatformDelegate>();
  g_platform = platform.get();
  return platform;
}

#if BUILDFLAG(IS_MAC)
void BrowserMainParts::PreCreateMainMessageLoop() {
  if (!g_external_loop || g_application_integration)
    SetUpApplication(zebkit_is_headless());
}
#endif

int BrowserMainParts::PreMainMessageLoopRun() {
  InitializeBrowserContexts();
  InstallClientHints();
  InstallPermissionHooks();
  InstallCertificateHooks();
  InstallHttpAuthHooks();
  InstallNavigationHooks();
  InstallPageHooks();
  InstallDownloadHooks();
  StartRuntimeSettings();
  InstallClientCertificateHooks();
  InstallNetworkHooks();
  AllowSchemes();
  content::Shell::SetNewWindowHandler(base::BindRepeating(&HandleNewWindow));
  content::Shell::SetPopupHandler(base::BindRepeating(&HandlePopup));
  // Playwright's dialogs and file choosers, and file choosers without a
  // display cancelled (automation.cc).
  InstallAutomationHooks();
  content::Shell::SetKeyboardEventHandler(base::BindRepeating(&HandleKey));
#if BUILDFLAG(IS_MAC)
  SetUpViews();
#endif
  content::Shell::Initialize(CreateShellPlatformDelegate());
  net::NetModule::SetResourceProvider(&NetResourceProvider);
  StartCdpFromCommandLine();
  return 0;
}

void BrowserMainParts::PostMainMessageLoopRun() {
  CdpStop();
  StopNetworkQuality();
  StopNavigationHooks();
  StopPermissionHooks();
  StopCertificateHooks();
  StopHttpAuthHooks();
  StopPageHooks();
  StopRuntimeSettings();
  StopDownloadHooks();
  StopClientCertificateHooks();
  StopNetworkHooks();
  CloseAllViews();
  content::Shell::Shutdown();
  content::ShellBrowserMainParts::PostMainMessageLoopRun();
}

content::BrowserContext* GetBrowserContext() {
  return content::ShellContentBrowserClient::Get()->browser_context();
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
  g_engine_started = true;
#if BUILDFLAG(IS_MAC)
  zebkit::SetUpBundle(zebkit::LibraryDirectory());
#endif
  for (int i = 0; i < argc; ++i) {
    if (!argv[i])
      return 1;
  }
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
  params.argc = static_cast<int>(engine->args.size());
  params.argv = engine->argv.data();
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
                                                   content::Shell::Shutdown();
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
