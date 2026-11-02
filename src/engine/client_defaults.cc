// What the engine's content clients answer that is not a hook of the
// application's: Chromium's content layer asks of every embedder (engine.h).

#ifdef UNSAFE_BUFFERS_BUILD
#pragma allow_unsafe_buffers
#endif

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/base_switches.h"
#include "base/command_line.h"
#include "base/containers/flat_set.h"
#include "base/feature_list.h"
#include "base/files/file_path.h"
#include "base/logging.h"
#include "base/metrics/field_trial.h"
#include "base/no_destructor.h"
#include "base/task/sequenced_task_runner.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "build/build_config.h"
#include "components/custom_handlers/protocol_handler_registry.h"
#include "components/custom_handlers/protocol_handler_throttle.h"
#include "components/embedder_support/user_agent_utils.h"
#include "components/network_hints/browser/simple_network_hints_handler_impl.h"
#include "components/network_hints/renderer/web_prescient_networking_impl.h"
#include "components/performance_manager/embedder/binders.h"
#include "components/performance_manager/embedder/performance_manager_registry.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/generated_code_cache_settings.h"
#include "content/public/browser/navigation_ui_data.h"
#include "content/public/browser/page_navigator.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/site_instance.h"
#include "content/public/common/content_features.h"
#include "content/public/common/content_switch_dependent_feature_overrides.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/url_constants.h"
#include "content/public/common/web_identity.h"
#include "content/public/renderer/render_thread.h"
#include "mojo/public/cpp/bindings/binder_map.h"
#include "net/base/features.h"
#include "net/base/net_errors.h"
#include "net/dns/public/dns_over_https_config.h"
#include "net/dns/public/secure_dns_mode.h"
#include "services/network/public/mojom/network_service.mojom.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_registry.h"
#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"
#include "third_party/blink/public/common/loader/url_loader_throttle.h"
#include "third_party/blink/public/platform/url_loader_throttle_provider.h"
#include "third_party/blink/public/platform/web_url_error.h"
#include "third_party/blink/public/web/modules/credentialmanagement/throttle_helper.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "v8/include/v8-initialization.h"
#include "v8/include/v8.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/protocol_handlers.h"
#include "zebkit/engine/speech_delegate.h"
#include "zebkit/engine/window.h"

#if (BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)) && \
    (defined(ARCH_CPU_X86_64) || defined(ARCH_CPU_ARM64))
#define ENABLE_WEB_ASSEMBLY_TRAP_HANDLER_LINUX
#include "base/debug/stack_trace.h"
#include "v8/include/v8-wasm-trap-handler-posix.h"
#endif

#if BUILDFLAG(IS_MAC)
#include "services/device/public/cpp/geolocation/geolocation_system_permission_manager.h"
#endif

#if BUILDFLAG(IS_WIN)
#include "content/public/browser/security_principal.h"
#include "media/mojo/mojom/media_foundation_preferences.mojom.h"
#include "media/mojo/services/media_foundation_preferences.h"
#endif

namespace zebkit {

namespace {

using performance_manager::PerformanceManagerRegistry;

BrowserClient* g_browser_client = nullptr;

// --isolated-context-origins, as content's isolation level reads it.
base::flat_set<url::Origin> IsolatedContextOrigins() {
  base::flat_set<url::Origin> origins;
  for (std::string_view text : base::SplitStringPiece(
           base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
               "isolated-context-origins"),
           ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    url::Origin origin = url::Origin::Create(GURL(text));
    if (!origin.opaque())
      origins.insert(origin);
    else
      LOG(ERROR) << "Error parsing IsolatedContext origin: " << text;
  }
  return origins;
}

void BindNetworkHintsHandler(
    content::RenderFrameHost* frame,
    mojo::PendingReceiver<network_hints::mojom::NetworkHintsHandler> receiver) {
  network_hints::SimpleNetworkHintsHandlerImpl::Create(frame, std::move(receiver));
}

#if BUILDFLAG(IS_WIN)
void BindMediaFoundationPreferences(
    content::RenderFrameHost* frame,
    mojo::PendingReceiver<media::mojom::MediaFoundationPreferences> receiver) {
  MediaFoundationPreferencesImpl::Create(
      frame->GetSiteInstance()->GetSecurityPrincipal().GetDeprecatedSiteURL(),
      base::NullCallback(), std::move(receiver));
}
#endif

}  // namespace

BrowserClient* BrowserClient::Get() {
  return g_browser_client;
}

BrowserClient::BrowserClient() {
  g_browser_client = this;
}

BrowserClient::~BrowserClient() {
  g_browser_client = nullptr;
}

Profile* BrowserClient::browser_context() {
  return main_parts_->browser_context();
}

Profile* BrowserClient::off_the_record_browser_context() {
  return main_parts_->off_the_record_browser_context();
}

// The features of the command line (--enable-features, --disable-features, and
// those its other switches imply) and the field trials it forces: the engine
// runs with Chromium's default features, as a browser does.
void BrowserClient::CreateFeatureListAndFieldTrials() {
  const base::CommandLine& command_line = *base::CommandLine::ForCurrentProcess();
  // Child processes are sent the trials in this list.
  static base::NoDestructor<base::FieldTrialList> field_trial_list;
  base::FieldTrialList::CreateTrialsFromString(
      command_line.GetSwitchValueASCII(switches::kForceFieldTrials));
  auto feature_list = std::make_unique<base::FeatureList>();
  feature_list->InitFromCommandLine(
      command_line.GetSwitchValueASCII(switches::kEnableFeatures),
      command_line.GetSwitchValueASCII(switches::kDisableFeatures));
  feature_list->RegisterExtraFeatureOverrides(
      content::GetSwitchDependentFeatureOverrides(command_line));
  base::FeatureList::SetInstance(std::move(feature_list));
}

bool BrowserClient::HasCustomSchemeHandler(content::BrowserContext* browser_context,
                                           const std::string& scheme) {
  auto* registry = ProtocolHandlers::GetForBrowserContext(browser_context);
  return registry && registry->IsHandledProtocol(scheme);
}

std::vector<std::unique_ptr<blink::URLLoaderThrottle>>
BrowserClient::CreateURLLoaderThrottles(
    const network::ResourceRequest& request,
    content::BrowserContext* browser_context,
    const base::RepeatingCallback<content::WebContents*()>& wc_getter,
    content::NavigationUIData* navigation_ui_data,
    content::FrameTreeNodeId frame_tree_node_id,
    std::optional<int64_t> navigation_id) {
  std::vector<std::unique_ptr<blink::URLLoaderThrottle>> result;
  if (auto* registry = ProtocolHandlers::GetForBrowserContext(browser_context)) {
    result.push_back(
        std::make_unique<custom_handlers::ProtocolHandlerThrottle>(*registry));
  }
  return result;
}

bool BrowserClient::AreIsolatedWebAppsEnabled(content::BrowserContext* browser_context) {
  return base::FeatureList::IsEnabled(features::kIsolatedWebApps);
}

bool BrowserClient::ShouldUrlUseApplicationIsolationLevel(
    content::BrowserContext* browser_context,
    const GURL& url) {
  static const base::NoDestructor<base::flat_set<url::Origin>> origins(
      IsolatedContextOrigins());
  GURL::Replacements replacements;
  replacements.ClearPort();
  return origins->contains(url::Origin::Create(url.ReplaceComponents(replacements)));
}

device::GeolocationSystemPermissionManager*
BrowserClient::GetGeolocationSystemPermissionManager() {
#if BUILDFLAG(IS_MAC)
  // The global one, which the embedder sets as Chrome does (the system's).
  return device::GeolocationSystemPermissionManager::GetInstance();
#else
  return nullptr;
#endif
}

std::string BrowserClient::GetDefaultDownloadName() {
  return "download";
}

content::GeneratedCodeCacheSettings BrowserClient::GetGeneratedCodeCacheSettings(
    content::BrowserContext* context) {
  // Size 0 has disk_cache pick one from the disk's size.
  return content::GeneratedCodeCacheSettings(true, 0, context->GetPath());
}

content::SpeechRecognitionManagerDelegate*
BrowserClient::CreateSpeechRecognitionManagerDelegate() {
  return new SpeechDelegate();
}

void BrowserClient::ExposeInterfacesToRenderer(
    service_manager::BinderRegistry* registry,
    blink::AssociatedInterfaceRegistry* associated_registry,
    content::RenderProcessHost* render_process_host) {
  PerformanceManagerRegistry::GetInstance()->CreateProcessNode(render_process_host);
  PerformanceManagerRegistry::GetInstance()
      ->GetBinders()
      .ExposeInterfacesToRendererProcess(registry, render_process_host);
}

void BrowserClient::ExposeInterfacesToChild(
    mojo::BinderMapWithContext<content::BrowserChildProcessHost*>* map) {
  PerformanceManagerRegistry::GetInstance()
      ->GetBinders()
      .ExposeInterfacesToBrowserChildProcess(map);
}

void BrowserClient::RegisterBrowserInterfaceBindersForFrame(
    content::RenderFrameHost* render_frame_host,
    mojo::BinderMapWithContext<content::RenderFrameHost*>* map) {
  PerformanceManagerRegistry::GetInstance()
      ->GetBinders()
      .ExposeInterfacesToRenderFrame(map);
  map->Add<network_hints::mojom::NetworkHintsHandler>(&BindNetworkHintsHandler);
#if BUILDFLAG(IS_WIN)
  map->Add<media::mojom::MediaFoundationPreferences>(&BindMediaFoundationPreferences);
#endif
}

void BrowserClient::OpenURL(content::SiteInstance* site_instance,
                            const content::OpenURLParams& params,
                            base::OnceCallback<void(content::WebContents*)> callback) {
  std::move(callback).Run(Window::Create(site_instance->GetBrowserContext(), params.url,
                                         nullptr, gfx::Size())
                              ->web_contents());
}

base::DictValue BrowserClient::GetNetLogConstants() {
  base::DictValue client;
  client.Set("name", "zebkit");
  base::DictValue constants;
  constants.Set("clientInfo", std::move(client));
  return constants;
}

base::FilePath BrowserClient::GetSandboxedStorageServiceDataDirectory() {
  return browser_context()->GetPath();
}

base::FilePath BrowserClient::GetFirstPartySetsDirectory() {
  return browser_context()->GetPath();
}

std::optional<base::FilePath> BrowserClient::GetLocalTracesDirectory() {
  return browser_context()->GetPath();
}

blink::UserAgentMetadata BrowserClient::GetUserAgentMetadata() {
  return embedder_support::GetUserAgentMetadata();
}

void BrowserClient::OnNetworkServiceCreated(network::mojom::NetworkService* network_service) {
  if (base::FeatureList::IsEnabled(net::features::kAsyncDns)) {
    network_service->ConfigureStubHostResolver(
        net::InsecureDnsMode::kEnabledBuiltIn,
        base::FeatureList::IsEnabled(net::features::kHappyEyeballsV3),
        net::SecureDnsMode::kAutomatic, net::DnsOverHttpsConfig(),
        /*additional_dns_types_enabled=*/true,
        /*fallback_doh_nameservers=*/{});
  }
}

std::vector<base::FilePath> BrowserClient::GetNetworkContextsParentDirectory() {
  return {browser_context()->GetPath()};
}

bool BrowserClient::HasErrorPage(int http_status_code) {
  return http_status_code >= 400 && http_status_code < 600;
}

void BrowserClient::OnWebContentsCreated(content::WebContents* web_contents) {
  PerformanceManagerRegistry::GetInstance()->MaybeCreatePageNodeForWebContents(
      web_contents);
}

// ---- The renderer --------------------------------------------------------

namespace {

// A site's signing in, which FedCM reads from its responses' headers: for
// --zebkit-fedcm (engine.cc).
class ThrottleProvider : public blink::URLLoaderThrottleProvider {
 public:
  ThrottleProvider()
      : main_thread_task_runner_(content::RenderThread::IsMainThread()
                                     ? base::SequencedTaskRunner::GetCurrentDefault()
                                     : nullptr) {}
  explicit ThrottleProvider(scoped_refptr<base::SequencedTaskRunner> task_runner)
      : main_thread_task_runner_(std::move(task_runner)) {}

  std::unique_ptr<blink::URLLoaderThrottleProvider> Clone() override {
    return std::make_unique<ThrottleProvider>(main_thread_task_runner_);
  }

  std::vector<std::unique_ptr<blink::URLLoaderThrottle>> CreateThrottles(
      base::optional_ref<const blink::LocalFrameToken> local_frame_token,
      const network::ResourceRequest& request) override {
    std::vector<std::unique_ptr<blink::URLLoaderThrottle>> throttles;
    if (local_frame_token.has_value()) {
      auto throttle = content::MaybeCreateIdentityUrlLoaderThrottle(
          base::BindRepeating(
              [](const blink::LocalFrameToken& token,
                 const scoped_refptr<base::SequencedTaskRunner> task_runner,
                 const std::optional<url::Origin>& initiator,
                 const url::Origin& idp_origin,
                 blink::mojom::IdpSigninStatus status) {
                if (content::RenderThread::IsMainThread()) {
                  blink::SetIdpSigninStatus(token, idp_origin, status);
                } else if (task_runner) {
                  task_runner->PostTask(FROM_HERE,
                                        base::BindOnce(&blink::SetIdpSigninStatus,
                                                       token, idp_origin, status));
                }
              },
              local_frame_token.value(), main_thread_task_runner_),
          content::GetSetLoginHeaderInProcessParser());
      if (throttle)
        throttles.push_back(std::move(throttle));
    }
    return throttles;
  }

  void SetOnline(bool is_online) override {}

 private:
  scoped_refptr<base::SequencedTaskRunner> main_thread_task_runner_;
};

}  // namespace

void RendererClient::SetUpWebAssemblyTrapHandler() {
#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
  content::ContentRendererClient::SetUpWebAssemblyTrapHandler();
#elif defined(ENABLE_WEB_ASSEMBLY_TRAP_HANDLER_LINUX)
  // No crash reporter: V8's own handler, unless in-process stack dumps have
  // the signal first, and then through them.
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kDisableInProcessStackTraces)) {
    v8::V8::EnableWebAssemblyTrapHandler(/*use_v8_signal_handler=*/true);
  } else if (base::debug::SetStackDumpFirstChanceCallback(
                 v8::TryHandleWebAssemblyTrapPosix)) {
    v8::V8::EnableWebAssemblyTrapHandler(/*use_v8_signal_handler=*/false);
  }
#else
  v8::V8::EnableWebAssemblyTrapHandler(/*use_v8_signal_handler=*/true);
#endif
}

void RendererClient::PrepareErrorPage(
    content::RenderFrame* render_frame,
    const blink::WebURLError& error,
    const std::string& http_method,
    content::mojom::AlternativeErrorPageOverrideInfoPtr alternative_error_page_info,
    std::string* error_html) {
  if (error_html && error_html->empty()) {
    *error_html =
        "<head><title>Error</title></head><body>Could not load the requested "
        "resource.<br/>Error code: " +
        base::NumberToString(error.reason()) +
        (error.reason() < 0 ? " (" + net::ErrorToString(error.reason()) + ")"
                            : "") +
        "</body>";
  }
}

void RendererClient::PrepareErrorPageForHttpStatusError(
    content::RenderFrame* render_frame,
    const blink::WebURLError& error,
    const std::string& http_method,
    int http_status,
    content::mojom::AlternativeErrorPageOverrideInfoPtr alternative_error_page_info,
    std::string* error_html) {
  if (error_html) {
    *error_html =
        "<head><title>Error</title></head><body>Server returned HTTP status " +
        base::NumberToString(http_status) + "</body>";
  }
}

std::unique_ptr<blink::URLLoaderThrottleProvider>
RendererClient::CreateURLLoaderThrottleProvider(
    blink::URLLoaderThrottleProviderType provider_type) {
  return std::make_unique<ThrottleProvider>();
}

std::unique_ptr<blink::WebPrescientNetworking> RendererClient::CreatePrescientNetworking(
    content::RenderFrame* render_frame) {
  return std::make_unique<network_hints::WebPrescientNetworkingImpl>(render_frame);
}

}  // namespace zebkit
