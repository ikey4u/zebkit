// zebkit_main(), zebkit_helper_main() and the classes that start the engine.

#ifdef UNSAFE_BUFFERS_BUILD
// The C API takes and returns C strings and arrays.
#pragma allow_unsafe_buffers
#endif

#include "zebkit/engine/engine.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

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
#include "base/version_info/version_info.h"
#include "content/public/app/content_main.h"
#include "content/public/browser/browser_main_runner.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/common/content_switches.h"
#include "content/shell/browser/shell.h"
#include "content/shell/browser/shell_platform_delegate.h"
#include "content/shell/common/shell_switches.h"
#include "net/base/module/net_module.h"
#include "net/grit/net_resources.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/ozone/public/ozone_switches.h"

namespace zebkit {

// Where libzebkit is: the engine's data files and zebkit-helper are beside it.
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

// The arguments Chromium sees: the application's, and what the engine
// needs that the application did not say.
std::vector<std::string> EngineArguments(int argc, char** argv) {
  std::vector<std::string> args(argv, argv + argc);
  if (args.empty())
    args.push_back("zebkit");
  const base::FilePath library = LibraryDirectory();
  if (!HasSwitch(argc, argv, switches::kBrowserSubprocessPath)) {
    args.push_back(std::string("--") + switches::kBrowserSubprocessPath + "=" +
                   library.Append("zebkit-helper").value());
  }
  if (!HasSwitch(argc, argv, switches::kContentShellUserDataDir)) {
    std::string config;
    if (const char* xdg = getenv("XDG_CONFIG_HOME"); xdg && *xdg)
      config = xdg;
    else if (const char* home = getenv("HOME"); home && *home)
      config = std::string(home) + "/.config";
    if (!config.empty()) {
      const std::string program = base::FilePath(args[0]).BaseName().value();
      args.push_back(std::string("--") + switches::kContentShellUserDataDir +
                     "=" + config + "/zebkit/" + program);
    }
  }
  if (!HasSwitch(argc, argv, switches::kOzonePlatform)) {
    const char* wayland = getenv("WAYLAND_DISPLAY");
    const char* x11 = getenv("DISPLAY");
    const bool display = (wayland && *wayland) || (x11 && *x11);
    if (!display || HasSwitch(argc, argv, "headless"))
      args.push_back(std::string("--") + switches::kOzonePlatform + "=headless");
  }
  args.push_back(std::string("--") + switches::kContentShellHideToolbar);
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
  // application is not where the engine is.
  const base::FilePath library = LibraryDirectory();
  if (!library.empty()) {
    base::PathService::OverrideAndCreateIfNeeded(
        base::DIR_MODULE, library, /*is_absolute=*/true, /*create=*/false);
    // The zygote is started from FILE_EXE, not from
    // --browser-subprocess-path: it must be zebkit-helper, whatever the
    // application's executable is (python3, for one).
    base::PathService::OverrideAndCreateIfNeeded(
        base::FILE_EXE, library.Append("zebkit-helper"), /*is_absolute=*/true,
        /*create=*/false);
  }
  return content::ShellMainDelegate::BasicStartupComplete();
}

bool MainDelegate::ShouldLoadV8Snapshot(const std::string& process_type) {
  // The browser process runs the application's own script
  // (zebkit_script_new()), so it needs V8 as a renderer does.
  return process_type.empty() ||
         content::ShellMainDelegate::ShouldLoadV8Snapshot(process_type);
}

content::ContentBrowserClient* MainDelegate::CreateContentBrowserClient() {
  browser_client_ = std::make_unique<BrowserClient>();
  return browser_client_.get();
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

namespace {

// The content shell's windows, but the loop does not end when the last one
// closes: when an application is done is the application's to say
// (zebkit_quit()), or its script's (zebkit_script_on_idle()).
class PlatformDelegate : public content::ShellPlatformDelegate {
 public:
  void DidCloseLastWindow() override {}
};

}  // namespace

std::unique_ptr<content::ShellPlatformDelegate>
BrowserMainParts::CreateShellPlatformDelegate() {
  return std::make_unique<PlatformDelegate>();
}

int BrowserMainParts::PreMainMessageLoopRun() {
  InitializeBrowserContexts();
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
  return base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
             switches::kOzonePlatform) == "headless";
}

}  // extern "C"
