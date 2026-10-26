// zebkit_main(), zebkit_helper_main() and the classes that start the engine.

#ifdef UNSAFE_BUFFERS_BUILD
// The C API takes and returns C strings and arrays.
#pragma allow_unsafe_buffers
#endif

#include "zebkit/engine/engine.h"
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
#include "base/memory/ref_counted_memory.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/process/current_process.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/version_info/version_info.h"
#include "build/build_config.h"
#include "components/embedder_support/switches.h"
#include "net/http/http_util.h"
#include "third_party/blink/public/common/renderer_preferences/renderer_preferences.h"
#include "third_party/icu/source/common/unicode/locid.h"
#include "components/embedder_support/user_agent_utils.h"
#include "components/version_info/version_info_with_user_agent.h"
#include "content/public/app/content_main.h"
#include "content/public/browser/browser_main_runner.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_switches.h"
#include "content/shell/browser/shell.h"
#include "content/shell/browser/shell_platform_delegate.h"
#include "content/shell/common/shell_switches.h"
#include "net/base/module/net_module.h"
#include "net/grit/net_resources.h"
#include "services/network/public/mojom/cookie_manager.mojom.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/base/ui_base_paths.h"
#include "ui/gfx/switches.h"
#include "ui/views/widget/widget.h"
#include "zebkit/engine/app.h"
#include "zebkit/engine/call.h"
#include "zebkit/engine/cookies.h"
#include "zebkit/engine/media_capture.h"
#include "zebkit/engine/scheme.h"
#include "zebkit/engine/view.h"

#if BUILDFLAG(IS_OZONE)
#include "ui/ozone/public/ozone_switches.h"
#endif

namespace zebkit {

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
  std::vector<std::string> args(argv, argv + argc);
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
      !HasSwitch(argc, argv, switches::kLogFile)) {
    args.push_back(std::string("--") + switches::kEnableLogging + "=stderr");
  }
  args.push_back(std::string("--") + switches::kContentShellHideToolbar);
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

MainDelegate::MainDelegate(zebkit_task_func ready, void* data)
    : ready_(ready), data_(data) {}

MainDelegate::~MainDelegate() = default;

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
  return content::ShellMainDelegate::BasicStartupComplete();
}

bool MainDelegate::ShouldLoadV8Snapshot(const std::string& process_type) {
  // The browser process runs the application's own script
  // (zebkit_script_new()), so it needs V8 as a renderer does.
  return process_type.empty() ||
         content::ShellMainDelegate::ShouldLoadV8Snapshot(process_type);
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

void RendererClient::RenderThreadStarted() {
  content::ShellContentRendererClient::RenderThreadStarted();
  RegisterSchemesInRenderer();
}

std::variant<int, content::MainFunctionParams> MainDelegate::RunProcess(
    const std::string& process_type,
    content::MainFunctionParams main_function_params) {
  if (!process_type.empty()) {
    return content::ShellMainDelegate::RunProcess(
        process_type, std::move(main_function_params));
  }
  base::CurrentProcess::GetInstance().SetProcessType(
      base::CurrentProcessType::PROCESS_BROWSER);
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
  if (opens_window && !user_gesture &&
      !base::CommandLine::ForCurrentProcess()->HasSwitch(
          embedder_support::kDisablePopupBlocking)) {
    return false;
  }
  return content::ShellContentBrowserClient::CanCreateWindow(
      opener, opener_url, opener_top_level_frame_url, source_origin,
      container_type, target_url, referrer, frame_name, disposition, features,
      user_gesture, opener_suppressed, no_javascript_access);
}

// The product CDP names in /json/version and Browser.getVersion, as
// Chrome names it ("Chrome/155.0.8059.84"): the content shell names none,
// and tools read it to know what they drive.
std::string BrowserClient::GetProduct() {
  return std::string(version_info::GetProductNameAndVersionForUserAgent());
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
  context_params->cookie_manager_params->block_third_party_cookies =
      !base::CommandLine::ForCurrentProcess()->HasSwitch(
          "zebkit-allow-third-party-cookies");
  // Accept-Language as Chrome sends it: each language with its weight
  // ("en-US,en;q=0.9"), not the list as it is.
  context_params->accept_language = net::HttpUtil::GenerateAcceptLanguageHeader(
      net::HttpUtil::ExpandLanguageList(AcceptLanguages()));
}

// A browser starts a renderer ahead of the next navigation; an application
// showing its pages mostly pays for one in memory, as Electron does not by
// default. --zebkit-spare-renderer asks for it.
bool BrowserClient::ShouldUseSpareRenderProcessHost(
    content::BrowserContext* browser_context,
    const GURL& site_url,
    std::optional<SpareProcessRefusedByEmbedderReason>& refused_reason) {
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          "zebkit-spare-renderer")) {
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
  SetUpApplication(zebkit_is_headless());
}
#endif

int BrowserMainParts::PreMainMessageLoopRun() {
  InitializeBrowserContexts();
  InstallClientHints();
  AllowSchemes();
  content::Shell::SetNewWindowHandler(base::BindRepeating(&HandleNewWindow));
  content::Shell::SetPopupHandler(base::BindRepeating(&HandlePopup));
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
  CloseAllViews();
  content::Shell::Shutdown();
  content::ShellBrowserMainParts::PostMainMessageLoopRun();
}

content::BrowserContext* GetBrowserContext() {
  return content::ShellContentBrowserClient::Get()->browser_context();
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

int zebkit_main(int argc, char** argv, zebkit_task_func ready, void* data) {
#if BUILDFLAG(IS_MAC)
  zebkit::SetUpBundle(zebkit::LibraryDirectory());
#endif
  std::vector<std::string> args = zebkit::EngineArguments(argc, argv);
  std::vector<char*> engine_argv;
  for (std::string& arg : args)
    engine_argv.push_back(arg.data());
  engine_argv.push_back(nullptr);
  static base::NoDestructor<zebkit::MainDelegate> delegate(ready, data);
  return zebkit::RunContent(static_cast<int>(args.size()), engine_argv.data(),
                            delegate.get());
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
                                                 content::Shell::Shutdown();
                                               }));
}

void zebkit_post_task(zebkit_task_func func, void* data) {
  content::GetUIThreadTaskRunner({})->PostTask(FROM_HERE,
                                               base::BindOnce(func, data));
}

void zebkit_post_delayed_task(zebkit_task_func func,
                              void* data,
                              uint32_t delay_ms) {
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
