// The engine inside libzebkit: Chromium's content layer started as a
// library, on the content shell's browser classes for now.

#ifndef ZEBKIT_ENGINE_ENGINE_H_
#define ZEBKIT_ENGINE_ENGINE_H_

#include <memory>
#include <string>
#include <variant>

#include "content/public/browser/browser_main_parts.h"
#include "content/shell/app/shell_main_delegate.h"
#include "content/shell/browser/shell_browser_main_parts.h"
#include "content/shell/browser/shell_content_browser_client.h"
#include "zebkit/engine/include/zebkit/zebkit.h"

namespace base {
class FilePath;
}

namespace content {
class BrowserContext;
}

namespace zebkit {

// The delegate of every zebkit process. In the browser process it starts
// the browser with BrowserMainRunner, as Android WebView does, so that the
// application's ready callback runs between initialization and the loop.
class MainDelegate : public content::ShellMainDelegate {
 public:
  MainDelegate(zebkit_task_func ready, void* data);
  ~MainDelegate() override;

  std::optional<int> BasicStartupComplete() override;
  bool ShouldLoadV8Snapshot(const std::string& process_type) override;
  content::ContentBrowserClient* CreateContentBrowserClient() override;
  std::variant<int, content::MainFunctionParams> RunProcess(
      const std::string& process_type,
      content::MainFunctionParams main_function_params) override;

 private:
  zebkit_task_func ready_;
  void* data_;
  std::unique_ptr<content::ShellContentBrowserClient> browser_client_;
};

class BrowserClient : public content::ShellContentBrowserClient {
 public:
  std::unique_ptr<content::BrowserMainParts> CreateBrowserMainParts(
      bool is_integration_test) override;
};

// The content shell's, without its first window and without a DevTools
// port nobody asked for.
class BrowserMainParts : public content::ShellBrowserMainParts {
 public:
  int PreMainMessageLoopRun() override;
  void PostMainMessageLoopRun() override;

 protected:
  void InitializeMessageLoopContext() override {}
  std::unique_ptr<content::ShellPlatformDelegate> CreateShellPlatformDelegate()
      override;
};

// The directory libzebkit is in, which has the engine's data files.
base::FilePath LibraryDirectory();

// The browser context views are created in.
content::BrowserContext* GetBrowserContext();

// CDP (cdp.cc).
unsigned CdpListen(const std::string& address, unsigned port, std::string* error);
unsigned CdpPort();
std::string CdpUrl();
void CdpStop();

// Starts CDP if the command line asks for it, as Chrome's switches do.
void StartCdpFromCommandLine();

// Closes the views the application holds, before the shell closes its
// windows (view.cc).
void CloseAllViews();

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_ENGINE_H_
