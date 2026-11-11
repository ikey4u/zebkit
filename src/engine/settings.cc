// Runtime view preferences, applied on every Chromium recomputation.
#ifdef UNSAFE_BUFFERS_BUILD
#pragma allow_unsafe_buffers
#endif
#include "zebkit/engine/settings.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/view.h"
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <string>
#include "base/i18n/character_encoding.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/utf_string_conversions.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/common/web_preferences/web_preferences.h"

namespace {
using Preferences = blink::web_pref::WebPreferences;
struct BoolField { const char* name; bool Preferences::* member; };
constexpr BoolField kBools[] = {
    {"javaScript", &Preferences::javascript_enabled},
    {"images", &Preferences::images_enabled},
    {"remoteFonts", &Preferences::remote_fonts_enabled},
    {"plugins", &Preferences::plugins_enabled},
    {"spatialNavigation", &Preferences::spatial_navigation_enabled},
    {"smoothScrolling", &Preferences::enable_scroll_animator},
    {"textAutosizing", &Preferences::text_size_adjust_enabled},
    {"localStorage", &Preferences::local_storage_enabled},
    {"hyperlinkAuditing", &Preferences::hyperlink_auditing_enabled},
    {"reducedMotion", &Preferences::prefers_reduced_motion},
    {"reducedTransparency", &Preferences::prefers_reduced_transparency},
    {"hideScrollbars", &Preferences::hide_scrollbars},
};
struct IntField { const char* name; int Preferences::* member; int minimum; };
constexpr IntField kInts[] = {
    {"defaultFontSize", &Preferences::default_font_size, 1},
    {"defaultFixedFontSize", &Preferences::default_fixed_font_size, 1},
    {"minimumFontSize", &Preferences::minimum_font_size, 0},
    {"minimumLogicalFontSize", &Preferences::minimum_logical_font_size, 0},
};
struct FontField { const char* name; blink::web_pref::ScriptFontFamilyMap Preferences::* member; };
constexpr FontField kFonts[] = {
    {"standard", &Preferences::standard_font_family_map},
    {"fixed", &Preferences::fixed_font_family_map},
    {"serif", &Preferences::serif_font_family_map},
    {"sansSerif", &Preferences::sans_serif_font_family_map},
    {"cursive", &Preferences::cursive_font_family_map},
    {"fantasy", &Preferences::fantasy_font_family_map},
    {"math", &Preferences::math_font_family_map},
};
bool Family(const base::Value& value) {
  return value.is_string() && !value.GetString().empty() && value.GetString().size() <= 1024 &&
         value.GetString().find('\0') == std::string::npos;
}
bool Fonts(const base::Value& value) {
  if (!value.is_dict()) return false;
  for (auto entry : value.GetDict()) {
    if (std::none_of(std::begin(kFonts), std::end(kFonts), [&](auto field) { return entry.first == field.name; }))
      return false;
    if (Family(entry.second)) continue;
    if (!entry.second.is_dict() || entry.second.GetDict().size() > 256) return false;
    for (auto script : entry.second.GetDict()) {
      const auto& key = script.first;
      if (key.size() != 4 || key[0] < 'A' || key[0] > 'Z' ||
          !std::all_of(key.begin() + 1, key.end(), [](char c) { return c >= 'a' && c <= 'z'; }) ||
          !Family(script.second)) return false;
    }
  }
  return true;
}
bool Validate(base::DictValue& settings, std::string* error) {
  for (auto entry : settings) {
    const auto& key = entry.first;
    const auto& value = entry.second;
    bool known = false, valid = value.is_none();
    for (auto field : kBools) if (key == field.name) { known = true; valid |= value.is_bool(); }
    for (auto field : kInts) if (key == field.name) {
      known = true;
      valid |= value.is_int() && value.GetInt() >= field.minimum && value.GetInt() <= 512;
    }
    if (key == "fonts") { known = true; valid |= Fonts(value); }
    if (key == "autoplay") {
      known = true;
      valid |= value.is_string() && (value.GetString() == "allow" || value.GetString() == "gesture" || value.GetString() == "document-activation");
    }
    if (key == "colorScheme") {
      known = true;
      valid |= value.is_string() && (value.GetString() == "light" || value.GetString() == "dark");
    }
    if (key == "contrast") {
      known = true;
      valid |= value.is_string() && (value.GetString() == "no-preference" || value.GetString() == "more" || value.GetString() == "less" || value.GetString() == "custom");
    }
    if (key == "defaultEncoding") {
      known = true;
      valid |= value.is_string() && value.GetString().size() <= 128 && value.GetString().find('\0') == std::string::npos &&
               !base::GetCanonicalEncodingNameByAliasName(value.GetString()).empty();
    }
    if (!known || !valid) {
      *error = (known ? "invalid view setting: " : "unknown view setting: ") + key;
      return false;
    }
  }
  if (auto* encoding = settings.FindString("defaultEncoding"))
    *encoding = base::GetCanonicalEncodingNameByAliasName(*encoding);
  return true;
}
}  // namespace

namespace zebkit {
void ApplyViewSettings(content::WebContents* contents, Preferences* prefs) {
  auto* view = ViewOf(contents);
  if (!view) return;
  const auto& settings = view->settings_;
  for (auto field : kBools) if (auto value = settings.FindBool(field.name)) prefs->*field.member = *value;
  for (auto field : kInts) if (auto value = settings.FindInt(field.name)) prefs->*field.member = *value;
  if (auto images = settings.FindBool("images")) prefs->loads_images_automatically = *images;
  if (const auto* encoding = settings.FindString("defaultEncoding")) prefs->default_encoding = *encoding;
  if (const auto* fonts = settings.FindDict("fonts")) {
    for (auto field : kFonts) {
      const auto* value = fonts->Find(field.name);
      if (!value) continue;
      auto& map = prefs->*field.member;
      if (value->is_string()) map[blink::web_pref::kCommonScript] = base::UTF8ToUTF16(value->GetString());
      else for (auto script : value->GetDict()) map[script.first] = base::UTF8ToUTF16(script.second.GetString());
    }
  }
  if (const auto* autoplay = settings.FindString("autoplay")) {
    using Policy = blink::mojom::AutoplayPolicy;
    prefs->autoplay_policy = *autoplay == "allow" ? Policy::kNoUserGestureRequired :
                            *autoplay == "gesture" ? Policy::kUserGestureRequired : Policy::kDocumentUserActivationRequired;
  }
  if (const auto* scheme = settings.FindString("colorScheme")) {
    prefs->preferred_color_scheme = *scheme == "dark" ? blink::mojom::PreferredColorScheme::kDark : blink::mojom::PreferredColorScheme::kLight;
    prefs->preferred_root_scrollbar_color_scheme = prefs->preferred_color_scheme;
  }
  if (const auto* contrast = settings.FindString("contrast")) {
    using Contrast = blink::mojom::PreferredContrast;
    prefs->preferred_contrast = *contrast == "more" ? Contrast::kMore : *contrast == "less" ? Contrast::kLess :
                                *contrast == "custom" ? Contrast::kCustom : Contrast::kNoPreference;
  }
}
}  // namespace zebkit

extern "C" {
bool zebkit_view_set_settings(zebkit_view* view, const char* json, char** error) {
  auto fail = [&](const std::string& message) { if (error) *error = strdup(message.c_str()); return false; };
  if (!zebkit::LiveView(view) || !view->web_contents()) return fail("the view is closed");
  if (!json || strlen(json) > 65536) return fail("view settings must be a JSON object of at most 64 KiB");
  auto parsed = base::JSONReader::Read(json, 0);
  if (!parsed || !parsed->is_dict()) return fail("view settings are not a JSON object");
  std::string message;
  if (!Validate(parsed->GetDict(), &message)) return fail(message);
  if (parsed->GetDict().empty()) return true;
  for (auto entry : parsed->GetDict()) {
    if (entry.second.is_none()) view->settings_.Remove(entry.first);
    else view->settings_.Set(entry.first, entry.second.Clone());
  }
  view->web_contents()->NotifyPreferencesChanged();
  return true;
}
char* zebkit_view_get_settings(zebkit_view* view) {
  return zebkit::LiveView(view) && view->web_contents() ? strdup(base::WriteJson(view->settings_).value_or("{}").c_str()) : nullptr;
}
}
