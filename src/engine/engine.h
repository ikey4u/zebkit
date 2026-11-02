// The engine inside libzebkit: Chromium's content layer started as a library.

#ifndef ZEBKIT_ENGINE_ENGINE_H_
#define ZEBKIT_ENGINE_ENGINE_H_

#include <memory>
#include <string>
#include <variant>

#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/run_loop.h"
#include "build/build_config.h"
#include "components/memory_system/memory_system.h"
#include "content/public/app/content_main_delegate.h"
#include "content/public/browser/browser_main_parts.h"
#include "content/public/browser/content_browser_client.h"
#include "content/public/common/content_client.h"
#include "content/public/renderer/content_renderer_client.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/native_ui_types.h"
#include "zebkit/engine/origin_trial_policy.h"
#include "zebkit/engine/path_utf8.h"
#include "zebkit/engine/profile.h"
#include "zebkit/engine/window.h"
#include "zebkit/engine/include/zebkit/zebkit.h"

namespace base {
class FilePath;
}

namespace content {
struct ContentMainParams;
class BrowserMainRunner;
class BrowserContext;
class RenderFrame;
class WebContents;
}

namespace performance_manager {
class PerformanceManagerLifetime;
}

// A context (<zebkit/zebkit.h>): a profile in memory, of its own.
struct zebkit_context {
  std::unique_ptr<zebkit::Profile> profile;
};

namespace zebkit {

class BrowserMainParts;
class ContentClient;
class BrowserClient;
class RendererClient;

// The delegate of every zebkit process. In the browser process it starts
// the browser with BrowserMainRunner, as Android WebView does, so that the
// application's ready callback runs between initialization and the loop.
class MainDelegate : public content::ContentMainDelegate {
 public:
  MainDelegate(zebkit_task_func ready, void* data, bool external_loop = false);
  ~MainDelegate() override;
  void ShutdownBrowser();

  std::optional<int> BasicStartupComplete() override;
  bool ShouldCreateFeatureList(InvokedIn invoked_in) override;
  bool ShouldInitializeMojo(InvokedIn invoked_in) override;
  void PreSandboxStartup() override;
  std::optional<int> PreBrowserMain() override;
  std::optional<int> PostEarlyInitialization(InvokedIn invoked_in) override;
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
  bool external_loop_;
  std::unique_ptr<content::BrowserMainRunner> browser_runner_;
  std::unique_ptr<ContentClient> content_client_;
  std::unique_ptr<BrowserClient> browser_client_;
  std::unique_ptr<RendererClient> renderer_client_;
  memory_system::MemorySystem memory_system_;
};

// The application's URL schemes (scheme.cc) and the engine's resources.
class ContentClient : public content::ContentClient {
 public:
  std::u16string GetLocalizedString(int message_id) override;
  std::string_view GetDataResource(int resource_id,
                                   ui::ResourceScaleFactor scale_factor) override;
  scoped_refptr<base::RefCountedMemory> GetDataResourceBytes(int resource_id) override;
  std::string GetDataResourceString(int resource_id) override;
  gfx::Image& GetNativeImageNamed(int resource_id) override;
  blink::OriginTrialPolicy* GetOriginTrialPolicy() override;
  void AddAdditionalSchemes(Schemes* schemes) override;
  // Chrome's PDF viewer, as a plugin of Chromium's own (pdf_viewer_renderer.cc).
  void AddPlugins(std::vector<content::WebPluginInfo>* plugins) override;

 private:
  OriginTrialPolicyImpl origin_trial_policy_;
};

// The client hints delegate of the browser contexts (client_hints.cc).
void InstallClientHints();
// Release observers before the browser/network service shuts down.
void StopNetworkQuality();
// Client hints for a context made later (zebkit_context_new()).
void InstallClientHintsFor(content::BrowserContext* profile);

// The languages pages and servers are told, as Chrome has them: the
// system's locale, then its language ("en-US,en"), or --accept-lang.
std::string AcceptLanguages();
// Content initialized ICU before the engine or its helpers start running.
void MarkICUInitialized();
// Them in a page's renderer preferences (navigator.languages).
void ApplyLanguages(content::WebContents* contents);

// window.chrome in the frame's pages (chrome_object.cc).
void InstallChromeObject(content::RenderFrame* render_frame);

// The frame's media elements for captures (media_capture_renderer.cc).
void InstallMediaCaptureAgent(content::RenderFrame* render_frame);

class RendererClient : public content::ContentRendererClient {
 public:
  void RenderThreadStarted() override;
  void SetUpWebAssemblyTrapHandler() override;
  void PrepareErrorPage(content::RenderFrame* render_frame,
                        const blink::WebURLError& error,
                        const std::string& http_method,
                        content::mojom::AlternativeErrorPageOverrideInfoPtr
                            alternative_error_page_info,
                        std::string* error_html) override;
  void PrepareErrorPageForHttpStatusError(
      content::RenderFrame* render_frame,
      const blink::WebURLError& error,
      const std::string& http_method,
      int http_status,
      content::mojom::AlternativeErrorPageOverrideInfoPtr
          alternative_error_page_info,
      std::string* error_html) override;
  std::unique_ptr<blink::URLLoaderThrottleProvider>
  CreateURLLoaderThrottleProvider(
      blink::URLLoaderThrottleProviderType provider_type) override;
  std::unique_ptr<blink::WebPrescientNetworking> CreatePrescientNetworking(
      content::RenderFrame* render_frame) override;
  // Printing in the page (print.cc).
  void RenderFrameCreated(content::RenderFrame* render_frame) override;
  // PDF in pages (pdf_viewer_renderer.cc).
  bool OverrideCreatePlugin(content::RenderFrame* render_frame,
                            const blink::WebPluginParams& params,
                            blink::WebPlugin** plugin) override;
  // A PDF renderer has no data of any origin, as Chrome's: script in a PDF's
  // document sees no storage and starts no worker (pdf_viewer_renderer.cc).
  bool IsDomStorageDisabled() const override;
  bool AreDedicatedWorkersDisabled() const override;
  // Workers for automation (automation_renderer.cc).
  void DidInitializeWorkerContextOnWorkerThread(
      v8::Local<v8::Context> context) override;
};

class BrowserClient : public content::ContentBrowserClient {
 public:
  // The one of the process; its contexts are on its main parts.
  static BrowserClient* Get();
  BrowserClient();
  ~BrowserClient() override;
  Profile* browser_context();
  Profile* off_the_record_browser_context();

  void CreateFeatureListAndFieldTrials();
  std::vector<std::unique_ptr<blink::URLLoaderThrottle>> CreateURLLoaderThrottles(
      const network::ResourceRequest& request,
      content::BrowserContext* browser_context,
      const base::RepeatingCallback<content::WebContents*()>& wc_getter,
      content::NavigationUIData* navigation_ui_data,
      content::FrameTreeNodeId frame_tree_node_id,
      std::optional<int64_t> navigation_id) override;
  bool HasCustomSchemeHandler(content::BrowserContext* browser_context,
                              const std::string& scheme) override;
  bool AreIsolatedWebAppsEnabled(content::BrowserContext* browser_context) override;
  bool ShouldUrlUseApplicationIsolationLevel(content::BrowserContext* browser_context,
                                             const GURL& url) override;
  device::GeolocationSystemPermissionManager*
  GetGeolocationSystemPermissionManager() override;
  std::string GetDefaultDownloadName() override;
  content::GeneratedCodeCacheSettings GetGeneratedCodeCacheSettings(
      content::BrowserContext* context) override;
  content::SpeechRecognitionManagerDelegate*
  CreateSpeechRecognitionManagerDelegate() override;
  void ExposeInterfacesToRenderer(
      service_manager::BinderRegistry* registry,
      blink::AssociatedInterfaceRegistry* associated_registry,
      content::RenderProcessHost* render_process_host) override;
  void ExposeInterfacesToChild(
      mojo::BinderMapWithContext<content::BrowserChildProcessHost*>* map) override;
  void RegisterBrowserInterfaceBindersForFrame(
      content::RenderFrameHost* render_frame_host,
      mojo::BinderMapWithContext<content::RenderFrameHost*>* map) override;
  void OpenURL(content::SiteInstance* site_instance,
               const content::OpenURLParams& params,
               base::OnceCallback<void(content::WebContents*)> callback) override;
  base::DictValue GetNetLogConstants() override;
  base::FilePath GetSandboxedStorageServiceDataDirectory() override;
  base::FilePath GetFirstPartySetsDirectory() override;
  std::optional<base::FilePath> GetLocalTracesDirectory() override;
  blink::UserAgentMetadata GetUserAgentMetadata() override;
  void OnNetworkServiceCreated(network::mojom::NetworkService* network_service) override;
  std::vector<base::FilePath> GetNetworkContextsParentDirectory() override;
  bool HasErrorPage(int http_status_code) override;
  void OnWebContentsCreated(content::WebContents* web_contents) override;
  content::WebAuthenticationDelegate* GetWebAuthenticationDelegate() override;
  std::unique_ptr<content::AuthenticatorRequestClientDelegate>
  GetWebAuthenticationRequestDelegate(content::RenderFrameHost* frame) override;
  std::unique_ptr<content::BrowserMainParts> CreateBrowserMainParts(
      bool is_integration_test) override;
  bool IsHandledURL(const GURL& url) override;
  void CreateThrottlesForNavigation(content::NavigationThrottleRegistry& registry) override;
  void CreateThrottlesForCommitWithoutUrlLoader(content::NavigationThrottleRegistry& registry) override;
  std::vector<std::unique_ptr<content::CommitDeferringCondition>>
  CreateCommitDeferringConditionsForNavigation(content::NavigationHandle* handle,
      content::CommitDeferringCondition::NavigationType type) override;
  void AllowCertificateError(content::WebContents* contents, int error,
      const net::SSLInfo& info, const GURL& url, bool main_frame, bool strict,
      base::OnceCallback<void(content::CertificateRequestResultType)> reply) override;
  bool HandleExternalProtocol(
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
      mojo::PendingRemote<network::mojom::URLLoaderFactory>* out_factory) override;
  base::OnceClosure SelectClientCertificate(
      content::BrowserContext* browser_context,
      int process_id,
      content::WebContents* web_contents,
      net::SSLCertRequestInfo* cert_request_info,
      net::ClientCertIdentityList client_certs,
      std::unique_ptr<content::ClientCertificateDelegate> delegate) override;
  void OverrideWebPreferences(content::WebContents* contents,
                              content::SiteInstance& main_frame_site,
                              blink::web_pref::WebPreferences* prefs) override;
  std::unique_ptr<content::WebContentsViewDelegate> GetWebContentsViewDelegate(
      content::WebContents* web_contents) override;
  void AppendExtraCommandLineSwitches(base::CommandLine* command_line,
                                      int child_process_id) override;
  mojo::PendingRemote<network::mojom::URLLoaderFactory>
  CreateNonNetworkNavigationURLLoaderFactory(
      const std::string& scheme,
      content::FrameTreeNodeId frame_tree_node_id) override;
  // HTTP authentication, answered with automation's credentials
  // (automation.cc).
  std::unique_ptr<content::LoginDelegate> CreateLoginDelegate(
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
      content::LoginDelegate::LoginAuthRequiredCallback auth_required_callback)
      override;
  // The pages with automation's requests, for Playwright's network events
  // and routes (automation_network.cc).
  void WillCreateURLLoaderFactory(
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
      bool is_for_network_service) override;
  // A WebSocket's handshake through the view's network filter (network_hooks.cc).
  bool WillInterceptWebSocket(content::RenderFrameHost* frame) override;
  void CreateWebSocket(content::RenderFrameHost* frame, WebSocketFactory factory, const GURL& url,
                       const net::SiteForCookies& site_for_cookies,
                       const std::optional<std::string>& user_agent,
                       mojo::PendingRemote<network::mojom::WebSocketHandshakeClient> handshake_client,
                       WebSocketOptions options) override;
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
  void ConfigureNetworkContextParams(
      content::BrowserContext* context,
      bool in_memory,
      const base::FilePath& relative_partition_path,
      network::mojom::NetworkContextParams* params,
      cert_verifier::mojom::CertVerifierCreationParams* cert_params) override;
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

 private:
  raw_ptr<BrowserMainParts> main_parts_ = nullptr;
};

// The browser's start and end around its loop.
class BrowserMainParts : public content::BrowserMainParts {
 public:
  BrowserMainParts();
  ~BrowserMainParts() override;

  // content::BrowserMainParts
  void PostCreateMainMessageLoop() override;
  void ToolkitInitialized() override;
  int PostCreateThreads() override;
  int PreMainMessageLoopRun() override;
  void WillRunMainMessageLoop(std::unique_ptr<base::RunLoop>& run_loop) override;
  void PostMainMessageLoopRun() override;
  void PostDestroyThreads() override;
#if BUILDFLAG(IS_MAC)
  void PreCreateMainMessageLoop() override;
#endif

  Profile* browser_context() { return browser_context_.get(); }
  Profile* off_the_record_browser_context() {
    return off_the_record_browser_context_.get();
  }

 private:
  std::unique_ptr<Profile> browser_context_;
  std::unique_ptr<Profile> off_the_record_browser_context_;
  std::unique_ptr<performance_manager::PerformanceManagerLifetime>
      performance_manager_lifetime_;
};

// The directory libzebkit is in, which has the engine's data files.
base::FilePath LibraryDirectory();

// The program the engine's child processes run.
base::FilePath HelperPath(const base::FilePath& library);

#if BUILDFLAG(IS_WIN)
// engine_win.cc: the user's per-machine application data directory, in UTF-8.
std::string DefaultConfigDirectory();
// Whether the executable called zebkit_win_startup(), with its sandbox.
bool HasExecutableSandbox();
// Windows' ContentMain takes no arguments and reads the process's command
// line: the engine's arguments become that, and params get the module and the
// sandbox.
void SetUpWindowsMainParams(int argc, char** argv, content::ContentMainParams* params);
#endif

#if BUILDFLAG(IS_MAC)
// engine_mac.mm: the framework as the bundle Chromium finds its files in;
// the helper's sandbox; the application's menu and its place in the Dock;
// the views delegate; views without windows.
void SetUpBundle(const base::FilePath& library);
bool EnterHelperSandbox(int argc, char** argv);
void SetUpApplication(bool headless);
void SetUpViews();
// The NSApplication that Chromium's event handling expects, when the engine
// owns the loop.
void RegisterApplication();
// The resource pack in the framework.
base::FilePath ResourcePackPath();
#endif

// The browser context views are created in.
content::BrowserContext* GetBrowserContext();

// A context's profile (zebkit_context_new()); the engine's own for NULL.
content::BrowserContext* ProfileOf(zebkit_context* context);
// What a new profile has of the engine's own: client hints, cookies.
void ConfigureContext(content::BrowserContext* profile);
// A closed context's profile, deleted when nothing uses it (view.cc), and what the
// engine keeps of it: automation's, the cookie setting's, the worker policy's.
void DestroyProfileWhenUnused(Profile* profile, int tries);
void ForgetAutomationContext(content::BrowserContext* profile);
void ForgetContextCookies(content::BrowserContext* profile);
void ForgetWorkerPolicy(content::BrowserContext* profile);

// CDP (cdp.cc).
unsigned CdpListen(const std::string& address, unsigned port, std::string* error);
unsigned CdpPort();
std::string CdpUrl();
void CdpStop();

// Starts CDP if the command line asks for it, as Chrome's switches do.
void StartCdpFromCommandLine();

// zebkit_view_new_in(), of a kind (window.h).
zebkit_view* NewView(zebkit_context* context,
                     const char* url,
                     int width,
                     int height,
                     Window::Kind kind);
// Nobody sees the page's window: an off-screen view, or all under --headless.
// What a window would show is then answered without one.
bool HasNoDisplay(content::WebContents* contents);

// The window of a view brought to the front.
void FocusWindow(Window* window);
// zebkit_view_set_size_limits().
void SetWindowSizeLimits(Window* window,
                         const gfx::Size& minimum,
                         bool resizable);

// A view's page in another view's window (attach.cc; engine_mac.mm).
bool AttachView(zebkit_view* view, zebkit_view* parent, const gfx::Rect& bounds);
void SetAttachedBounds(zebkit_view* view, const gfx::Rect& bounds);
void SetAttachedVisible(zebkit_view* view, bool visible);
void DetachView(zebkit_view* view);

// The same in a window of the application's (attach.cc; engine_mac.mm). The
// others are false when the view is not in one, and leave it to their callers.
bool AttachViewNative(zebkit_view* view,
                      uintptr_t native_parent,
                      const gfx::Rect& bounds,
                      std::string* error);
bool SetAttachedNativeBounds(zebkit_view* view, const gfx::Rect& bounds);
bool SetAttachedNativeSize(zebkit_view* view, const gfx::Size& size);
bool SetAttachedNativeVisible(zebkit_view* view, bool visible);
bool FocusAttachedNative(zebkit_view* view);
// To its own window again.
bool DetachViewNative(zebkit_view* view);
// The view is closing, and its window with it.
void ForgetAttachedNative(zebkit_view* view);

// Closes the views the application holds, before the rest of the windows
// close (view.cc).
void CloseAllViews();

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_ENGINE_H_
