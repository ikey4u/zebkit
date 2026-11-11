#include "zebkit/engine/appearance.h"

#include <string.h>

#include <memory>
#include <optional>
#include <string>

#include "base/command_line.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/observer_list.h"
#include "base/values.h"
#include "build/build_config.h"
#include "content/public/browser/browser_accessibility_state.h"
#include "content/public/browser/browser_thread.h"
#include "ui/accessibility/ax_mode.h"
#include "third_party/icu/source/common/unicode/locid.h"
#include "ui/native_theme/native_theme.h"
#include "ui/native_theme/native_theme_observer.h"
#include "zebkit/engine/include/zebkit/zebkit.h"

#if BUILDFLAG(IS_LINUX) && defined(ZEBKIT_USE_DBUS)
#include "base/memory/weak_ptr.h"
#include "components/dbus/thread_linux/dbus_thread_linux.h"
#include "components/dbus/utils/call_method.h"
#include "components/dbus/utils/connect_to_signal.h"
#include "components/dbus/utils/variant.h"
#include "components/dbus/xdg/portal.h"
#include "dbus/bus.h"
#include "dbus/object_proxy.h"
#define ZEBKIT_APPEARANCE_PORTAL 1
#endif

namespace zebkit {

namespace {

constexpr char kFollowSwitch[] = "zebkit-follow-system-appearance";

#if defined(ZEBKIT_APPEARANCE_PORTAL)
void TellEnvironment();
std::optional<bool>& SystemDark();
#endif

#if defined(ZEBKIT_APPEARANCE_PORTAL)
// org.freedesktop.appearance's color-scheme from the XDG portal, as
// ui/linux/portal_settings_linux.cc reads it for Chrome's toolkits, into the
// NativeTheme the pages and the engine's own UI take theirs from while they
// follow the system; otherwise only what zebkit_get_environment() says. zebkit
// has no GTK or Qt behind it; a desktop without the portal leaves the theme
// light. The portal's "no preference" is light, as Chrome and Firefox read it.
class PortalAppearance {
 public:
  PortalAppearance()
      : bus_(dbus_thread_linux::GetSharedSessionBus()),
        proxy_(bus_->GetObjectProxy("org.freedesktop.portal.Desktop",
                                    dbus::ObjectPath("/org/freedesktop/portal/desktop"))) {
    dbus_xdg::RequestXdgDesktopPortal(bus_.get(), kInterface,
                                      base::BindOnce(&PortalAppearance::Ready, weak_factory_.GetWeakPtr()));
  }

 private:
  static constexpr char kInterface[] = "org.freedesktop.portal.Settings";
  static constexpr char kNamespace[] = "org.freedesktop.appearance";

  void Ready(uint32_t version) {
    if (!version)
      return;
    dbus_utils::ConnectToSignal<"ssv">(
        proxy_, kInterface, "SettingChanged",
        base::BindRepeating(&PortalAppearance::Changed, weak_factory_.GetWeakPtr()),
        base::BindOnce([](const std::string&, const std::string&, bool) {}));
    dbus_utils::CallMethod<"ss", "v">(proxy_, kInterface, "Read",
                                      base::BindOnce(&PortalAppearance::Read, weak_factory_.GetWeakPtr()),
                                      kNamespace, "color-scheme");
  }

  void Read(dbus_utils::CallMethodResultSig<"v"> result) {
    if (!result.has_value())
      return;
    auto& [outer] = result.value();
    auto inner = std::move(outer).Take<dbus_utils::Variant>();
    if (!inner.has_value())
      return;
    if (auto scheme = std::move(*inner).Take<uint32_t>())
      Set(*scheme);
  }

  void Changed(dbus_utils::ConnectToSignalResultSig<"ssv"> result) {
    if (!result.has_value())
      return;
    auto& [space, key, value] = result.value();
    if (space != kNamespace || key != "color-scheme")
      return;
    if (auto scheme = std::move(value).Take<uint32_t>())
      Set(*scheme);
  }

  // 0 no preference, 1 dark, 2 light (the portal's specification).
  static void Set(uint32_t scheme) {
    const bool dark = scheme == 1;
    if (!FollowsSystemAppearance()) {
      if (SystemDark() != dark) {
        SystemDark() = dark;
        TellEnvironment();
      }
      return;
    }
    SystemDark() = dark;
    const auto preferred = dark ? ui::NativeTheme::PreferredColorScheme::kDark
                                : ui::NativeTheme::PreferredColorScheme::kLight;
    for (ui::NativeTheme* theme : {ui::NativeTheme::GetInstanceForWeb(), ui::NativeTheme::GetInstanceForNativeUi()}) {
      if (theme->preferred_color_scheme() == preferred)
        continue;
      theme->set_preferred_color_scheme(preferred);
      theme->NotifyOnNativeThemeUpdated();
    }
  }

  scoped_refptr<dbus::Bus> bus_;
  raw_ptr<dbus::ObjectProxy> proxy_;
  base::WeakPtrFactory<PortalAppearance> weak_factory_{this};
};

// The portal's scheme, once it has said: the system's, which the theme has
// only while the pages follow it.
std::optional<bool>& SystemDark() {
  static std::optional<bool> dark;
  return dark;
}

std::unique_ptr<PortalAppearance>& Portal() {
  static base::NoDestructor<std::unique_ptr<PortalAppearance>> portal;
  return *portal;
}
#endif

base::DictValue Environment() {
  const ui::NativeTheme* theme = ui::NativeTheme::GetInstanceForWeb();
  base::DictValue value;
  bool dark = theme->preferred_color_scheme() == ui::NativeTheme::PreferredColorScheme::kDark;
#if defined(ZEBKIT_APPEARANCE_PORTAL)
  if (SystemDark())
    dark = *SystemDark();
#endif
  value.Set("colorScheme", dark ? "dark" : "light");
  const char* contrast = "no-preference";
  switch (theme->preferred_contrast()) {
    case ui::NativeTheme::PreferredContrast::kMore: contrast = "more"; break;
    case ui::NativeTheme::PreferredContrast::kLess: contrast = "less"; break;
    case ui::NativeTheme::PreferredContrast::kCustom: contrast = "custom"; break;
    case ui::NativeTheme::PreferredContrast::kNoPreference: break;
  }
  value.Set("contrast", contrast);
  value.Set("forcedColors", theme->forced_colors() != ui::ColorProviderKey::ForcedColors::kNone);
  value.Set("reducedTransparency", theme->prefers_reduced_transparency());
  value.Set("invertedColors", theme->inverted_colors());
  value.Set("locale", icu::Locale::getDefault().getName());
  value.Set("following", FollowsSystemAppearance());
  // Whether an assistive technology has the pages' trees (on macOS when
  // VoiceOver or AXManualAccessibility asks, application_mac.mm; elsewhere
  // content's detection, or --force-renderer-accessibility).
  const ui::AXMode mode = content::BrowserAccessibilityState::GetInstance()->GetAccessibilityMode();
  value.Set("accessibility", !mode.has_mode(ui::AXMode::kWebContents) ? (mode.has_mode(ui::AXMode::kNativeAPIs) ? "native" : "off")
                             : mode.has_mode(ui::AXMode::kScreenReader) ? "complete" : "basic");
  return value;
}

// The application's function for the system's changes.
class EnvironmentObserver : public ui::NativeThemeObserver {
 public:
  void Set(zebkit_environment_func func, void* data) {
    func_ = func;
    data_ = data;
    if (func && !observing_) {
      ui::NativeTheme::GetInstanceForWeb()->AddObserver(this);
      observing_ = true;
    }
  }
  void Stop() {
    if (observing_)
      ui::NativeTheme::GetInstanceForWeb()->RemoveObserver(this);
    observing_ = false;
    func_ = nullptr;
  }
  void OnNativeThemeUpdated(ui::NativeTheme* theme) override {
    if (!func_)
      return;
    const std::string json = base::WriteJson(Environment()).value_or("{}");
    func_(json.c_str(), data_);
  }

 private:
  zebkit_environment_func func_ = nullptr;
  void* data_ = nullptr;
  bool observing_ = false;
};

EnvironmentObserver& Observer() {
  static base::NoDestructor<EnvironmentObserver> observer;
  return *observer;
}

#if defined(ZEBKIT_APPEARANCE_PORTAL)
void TellEnvironment() {
  Observer().OnNativeThemeUpdated(ui::NativeTheme::GetInstanceForWeb());
}
#endif

}  // namespace

bool FollowsSystemAppearance() {
  return base::CommandLine::ForCurrentProcess()->HasSwitch(kFollowSwitch);
}

void StartSystemAppearance() {
#if defined(ZEBKIT_APPEARANCE_PORTAL)
  if (!Portal())
    Portal() = std::make_unique<PortalAppearance>();
#endif
}

void StopSystemAppearance() {
  Observer().Stop();
#if defined(ZEBKIT_APPEARANCE_PORTAL)
  Portal().reset();
#endif
}

void ApplySystemAppearance(blink::web_pref::WebPreferences* prefs) {
  const ui::NativeTheme* theme = ui::NativeTheme::GetInstanceForWeb();
  // Web content takes "no preference" for light, as Chrome's does.
  prefs->preferred_color_scheme = theme->preferred_color_scheme() == ui::NativeTheme::PreferredColorScheme::kDark
                                      ? blink::mojom::PreferredColorScheme::kDark
                                      : blink::mojom::PreferredColorScheme::kLight;
  prefs->preferred_root_scrollbar_color_scheme = prefs->preferred_color_scheme;
  switch (theme->preferred_contrast()) {
    case ui::NativeTheme::PreferredContrast::kMore:
      prefs->preferred_contrast = blink::mojom::PreferredContrast::kMore;
      break;
    case ui::NativeTheme::PreferredContrast::kLess:
      prefs->preferred_contrast = blink::mojom::PreferredContrast::kLess;
      break;
    case ui::NativeTheme::PreferredContrast::kCustom:
      prefs->preferred_contrast = blink::mojom::PreferredContrast::kCustom;
      break;
    case ui::NativeTheme::PreferredContrast::kNoPreference:
      prefs->preferred_contrast = blink::mojom::PreferredContrast::kNoPreference;
      break;
  }
  prefs->in_forced_colors = theme->forced_colors() != ui::ColorProviderKey::ForcedColors::kNone;
}

}  // namespace zebkit

extern "C" {

char* zebkit_get_environment(void) {
  if (!content::BrowserThread::CurrentlyOn(content::BrowserThread::UI))
    return nullptr;
  return strdup(base::WriteJson(zebkit::Environment()).value_or("{}").c_str());
}

void zebkit_on_environment_changed(zebkit_environment_func func, void* data) {
  if (!content::BrowserThread::CurrentlyOn(content::BrowserThread::UI))
    return;
  if (func)
    zebkit::Observer().Set(func, data);
  else
    zebkit::Observer().Stop();
}

}  // extern "C"
