#ifndef ZEBKIT_ENGINE_SETTINGS_H_
#define ZEBKIT_ENGINE_SETTINGS_H_
namespace content { class WebContents; }
namespace blink::web_pref { struct WebPreferences; }
namespace zebkit {
void ApplyViewSettings(content::WebContents* contents, blink::web_pref::WebPreferences* prefs);
}
#endif
