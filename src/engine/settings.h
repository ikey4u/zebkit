#ifndef ZEBKIT_ENGINE_SETTINGS_H_
#define ZEBKIT_ENGINE_SETTINGS_H_
#include <optional>
#include <string>
#include "zebkit/engine/include/zebkit/hooks.h"
#include "zebkit/engine/include/zebkit/page.h"
namespace content { class BrowserContext; class WebContents; }
namespace blink::web_pref { struct WebPreferences; }
namespace zebkit {
void ApplyViewSettings(content::WebContents* contents, blink::web_pref::WebPreferences* prefs);

// Runtime settings (runtime_settings.cc), for what they govern to ask.
void StartRuntimeSettings();
void StopRuntimeSettings();
// For a network context being made as well: this is how a later context, or one
// the network service made again, gets what was set.
bool BlocksThirdPartyCookies(content::BrowserContext* context);
bool UsesSpareRenderer();
void SpareAfterLoad(content::BrowserContext* context);
std::optional<zebkit_dialog_policy> EngineDialogPolicy();
std::optional<zebkit_popup_policy> EnginePopupPolicy();
bool EngineDeniesDownloads();
// "webrtcIpHandlingPolicy": @view's, else the engine's, else the startup switch
// (--force-webrtc-ip-handling-policy), else Chromium's "default". @view may be
// nullptr, for a page that has no view yet.
std::string EffectiveWebRtcIpPolicy(const zebkit_view* view);
// That policy into @contents's renderer preferences, sent to its renderers
// when it changed: pages already open take it without a reload.
void ApplyWebRtcIpPolicy(content::WebContents* contents, const zebkit_view* view);
// Whether a TLS server's request for a client certificate is offered the
// operating system's identities (NSS, Keychain, the Windows store): off unless
// the application sets "clientCertificateStores".
bool UsesClientCertificateStores();
// dialog.cc: what the view itself chose, apart from the engine's.
std::optional<zebkit_dialog_policy> ExplicitDialogPolicy(zebkit_view* view);
void ClearDialogPolicy(zebkit_view* view);
// download_hooks.cc: true denies; "ask" when a function decides.
void SetDownloadPolicy(zebkit_view* view, std::optional<bool> deny);
const char* EffectiveDownloads(zebkit_view* view);
}
#endif
