// The engine inside libzebkit: Chromium's content layer started as a
// library, on the content shell's browser classes for now.

#ifndef ZEBKIT_ENGINE_ENGINE_H_
#define ZEBKIT_ENGINE_ENGINE_H_

#include <memory>
#include <string>
#include <variant>

#include "build/build_config.h"
#include "content/public/browser/browser_main_parts.h"
#include "content/shell/app/shell_main_delegate.h"
#include "content/shell/browser/shell_browser_main_parts.h"
#include "content/shell/browser/shell_content_browser_client.h"
#include "content/shell/common/shell_content_client.h"
#include "content/shell/renderer/shell_content_renderer_client.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/native_ui_types.h"
#include "zebkit/engine/include/zebkit/zebkit.h"

namespace base {
class FilePath;
}

namespace content {
class BrowserContext;
class RenderFrame;
class WebContents;
class Shell;
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
  content::ContentClient* CreateContentClient() override;
  content::ContentBrowserClient* CreateContentBrowserClient() override;
  content::ContentRendererClient* CreateContentRendererClient() override;
  std::variant<int, content::MainFunctionParams> RunProcess(
      const std::string& process_type,
      content::MainFunctionParams main_function_params) override;

 private:
  zebkit_task_func ready_;
  void* data_;
  std::unique_ptr<content::ShellContentClient> content_client_;
  std::unique_ptr<content::ShellContentBrowserClient> browser_client_;
  std::unique_ptr<content::ShellContentRendererClient> renderer_client_;
};

// The content shell's, with the application's URL schemes (scheme.cc).
class ContentClient : public content::ShellContentClient {
 public:
  void AddAdditionalSchemes(Schemes* schemes) override;
};

// The client hints delegate of the browser contexts (client_hints.cc).
void InstallClientHints();

// The languages pages and servers are told, as Chrome has them: the
// system's locale, then its language ("en-US,en"), or --accept-lang.
std::string AcceptLanguages();
// Them in a page's renderer preferences (navigator.languages).
void ApplyLanguages(content::WebContents* contents);

// window.chrome in the frame's pages (chrome_object.cc).
void InstallChromeObject(content::RenderFrame* render_frame);

class RendererClient : public content::ShellContentRendererClient {
 public:
  RendererClient() : content::ShellContentRendererClient(false) {}
  void RenderThreadStarted() override;
  // Printing in the page (print.cc).
  void RenderFrameCreated(content::RenderFrame* render_frame) override;
};

class BrowserClient : public content::ShellContentBrowserClient {
 public:
  std::unique_ptr<content::BrowserMainParts> CreateBrowserMainParts(
      bool is_integration_test) override;
  bool IsHandledURL(const GURL& url) override;
  std::unique_ptr<content::WebContentsViewDelegate> GetWebContentsViewDelegate(
      content::WebContents* web_contents) override;
  void AppendExtraCommandLineSwitches(base::CommandLine* command_line,
                                      int child_process_id) override;
  mojo::PendingRemote<network::mojom::URLLoaderFactory>
  CreateNonNetworkNavigationURLLoaderFactory(
      const std::string& scheme,
      content::FrameTreeNodeId frame_tree_node_id) override;
  void RegisterNonNetworkSubresourceURLLoaderFactories(
      int render_process_id,
      int render_frame_id,
      const std::optional<url::Origin>& request_initiator_origin,
      NonNetworkURLLoaderFactoryMap* factories) override;
  void RegisterNonNetworkWorkerMainResourceURLLoaderFactories(
      content::BrowserContext* browser_context,
      const std::optional<url::Origin>& request_initiator,
      network::mojom::RequestDestination request_destination,
      NonNetworkURLLoaderFactoryMap* factories) override;
  void ConfigureNetworkContextParamsForShell(
      content::BrowserContext* context,
      network::mojom::NetworkContextParams* context_params,
      cert_verifier::mojom::CertVerifierCreationParams*
          cert_verifier_creation_params) override;
  std::string GetProduct() override;
  std::string GetUserAgent() override;
  std::string GetAcceptLangs(content::BrowserContext* context) override;
  bool CanCreateWindow(content::RenderFrameHost* opener,
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
                       bool* no_javascript_access) override;
  // Page.printToPDF and the clients of each page (devtools.cc).
  std::unique_ptr<content::DevToolsManagerDelegate>
  CreateDevToolsManagerDelegate() override;
  // The page's printing reaches the browser (print.cc).
  void RegisterAssociatedInterfaceBindersForRenderFrameHost(
      content::RenderFrameHost& render_frame_host,
      blink::AssociatedInterfaceRegistry& associated_registry) override;
  bool ShouldUseSpareRenderProcessHost(
      content::BrowserContext* browser_context,
      const GURL& site_url,
      std::optional<SpareProcessRefusedByEmbedderReason>& refused_reason)
      override;
};

// The content shell's, without its first window and without a DevTools
// port nobody asked for.
class BrowserMainParts : public content::ShellBrowserMainParts {
 public:
  int PreMainMessageLoopRun() override;
  void PostMainMessageLoopRun() override;

#if BUILDFLAG(IS_MAC)
  void PreCreateMainMessageLoop() override;
#endif

 protected:
  void InitializeMessageLoopContext() override {}
  std::unique_ptr<content::ShellPlatformDelegate> CreateShellPlatformDelegate()
      override;
};

// The directory libzebkit is in, which has the engine's data files.
base::FilePath LibraryDirectory();

// The program the engine's child processes run.
base::FilePath HelperPath(const base::FilePath& library);

#if BUILDFLAG(IS_MAC)
// engine_mac.mm: the framework as the bundle Chromium finds its files in;
// the helper's sandbox; the application's menu and its place in the Dock;
// the views delegate; views without windows.
void SetUpBundle(const base::FilePath& library);
bool EnterHelperSandbox(int argc, char** argv);
void SetUpApplication(bool headless);
void SetUpViews();
void FocusNativeWindow(gfx::NativeWindow window);
// The smallest content size the window may have, and whether it resizes.
void SetNativeWindowSizeLimits(gfx::NativeWindow window,
                               const gfx::Size& minimum,
                               bool resizable);
std::unique_ptr<content::ShellPlatformDelegate> CreateHeadlessPlatformDelegate();
#endif

// The browser context views are created in.
content::BrowserContext* GetBrowserContext();

// CDP (cdp.cc).
unsigned CdpListen(const std::string& address, unsigned port, std::string* error);
unsigned CdpPort();
std::string CdpUrl();
void CdpStop();

// Starts CDP if the command line asks for it, as Chrome's switches do.
void StartCdpFromCommandLine();

// The window of a view: its title again, from the application's or the
// page's; brought to the front.
void UpdateWindowTitle(content::Shell* shell);
void FocusWindow(content::Shell* shell);
// zebkit_view_set_size_limits().
void SetWindowSizeLimits(content::Shell* shell,
                         const gfx::Size& minimum,
                         bool resizable);

// A view's page in another view's window (attach.cc; engine_mac.mm).
bool AttachView(zebkit_view* view, zebkit_view* parent, const gfx::Rect& bounds);
void SetAttachedBounds(zebkit_view* view, const gfx::Rect& bounds);
void SetAttachedVisible(zebkit_view* view, bool visible);
void DetachView(zebkit_view* view);

// Closes the views the application holds, before the shell closes its
// windows (view.cc).
void CloseAllViews();

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_ENGINE_H_
