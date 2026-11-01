// What zebkit decides itself, changed while it runs (<zebkit/hooks.h>): JSON,
// validated whole before anything changes, for a view and for the engine. Unset,
// each is what it would be without this file, so that a privacy default stays
// the default until someone changes it on purpose.
#ifdef UNSAFE_BUFFERS_BUILD
#pragma allow_unsafe_buffers
#endif
#include <stdlib.h>
#include <string.h>

#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>

#include "base/command_line.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/time/time.h"
#include "base/values.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/spare_render_process_host_manager.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "services/network/public/mojom/cookie_manager.mojom.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/page_hooks.h"
#include "zebkit/engine/page_internal.h"
#include "zebkit/engine/settings.h"
#include "zebkit/engine/view.h"

namespace {

template <typename T>
struct Name {
  const char* name;
  T value;
};
constexpr Name<bool> kCookies[] = {{"block", true}, {"allow", false}};
constexpr Name<bool> kDownloads[] = {{"deny", true}, {"save", false}};
constexpr Name<zebkit_dialog_policy> kDialogs[] = {
    {"default", ZEBKIT_DIALOG_POLICY_DEFAULT},
    {"hold", ZEBKIT_DIALOG_POLICY_HOLD},
    {"accept", ZEBKIT_DIALOG_POLICY_ACCEPT},
    {"dismiss", ZEBKIT_DIALOG_POLICY_DISMISS},
};
constexpr Name<zebkit_popup_policy> kWindows[] = {
    {"block-without-gesture", ZEBKIT_POPUP_POLICY_BLOCK_WITHOUT_GESTURE},
    {"allow", ZEBKIT_POPUP_POLICY_ALLOW},
    {"block", ZEBKIT_POPUP_POLICY_BLOCK},
};

template <typename T, size_t N>
std::optional<T> ValueOf(const Name<T> (&names)[N], const base::Value& value) {
  if (value.is_string()) {
    for (const auto& name : names) {
      if (value.GetString() == name.name)
        return name.value;
    }
  }
  return std::nullopt;
}
template <typename T, size_t N>
const char* NameOf(const Name<T> (&names)[N], T value) {
  for (const auto& name : names) {
    if (name.value == value)
      return name.name;
  }
  return "";
}
template <typename T, size_t N>
void PutName(base::DictValue& out, const char* key, const Name<T> (&names)[N], const std::optional<T>& value) {
  if (value)
    out.Set(key, NameOf(names, *value));
}

struct EngineSettings {
  std::optional<bool> block_third_party_cookies;
  std::optional<bool> spare_renderer;
  std::optional<zebkit_dialog_policy> dialogs;
  std::optional<bool> deny_downloads;
  std::optional<zebkit_popup_policy> new_windows;
};
EngineSettings& Engine() {
  static EngineSettings settings;
  return settings;
}
// Between StartRuntimeSettings() and StopRuntimeSettings(); the contexts known are
// alive only then.
bool g_running = false;

// Every context that has a network context. Those made later ask
// BlocksThirdPartyCookies() when theirs is made. A context stays until the engine
// stops (zebkit_context_close()).
std::set<content::BrowserContext*>& CookieContexts() {
  static base::NoDestructor<std::set<content::BrowserContext*>> contexts;
  return *contexts;
}
// What a view asked for its context; the latest of that and the engine's holds.
std::map<content::BrowserContext*, bool>& ContextCookies() {
  static base::NoDestructor<std::map<content::BrowserContext*, bool>> contexts;
  return *contexts;
}
void ApplyCookies(content::BrowserContext* context) {
  const bool block = zebkit::BlocksThirdPartyCookies(context);
  context->ForEachLoadedStoragePartition([block](content::StoragePartition* partition) {
    partition->GetCookieManagerForBrowserProcess()->BlockThirdPartyCookies(block);
  });
}

bool IsReady(std::string* error) {
  if (g_running && content::BrowserThread::CurrentlyOn(content::BrowserThread::UI))
    return true;
  *error = "runtime settings need the engine running, on its thread";
  return false;
}

// Content keeps a spare only when the feature was on as the engine started
// (RenderProcessHostImpl::IsSpareProcessKeptAtAllTimes), and that cannot change
// later: with it off, as zebkit starts by default, content makes none and drops any
// at the next commit of a frame (PrepareForFutureRequests). So the spare is made here,
// when a page has finished loading, which is when no commit is near, for the next
// navigation to take. With the feature on, content makes one after every commit
// whatever the embedder says, and it is taken away here again.
void ApplySpare(bool spare, content::BrowserContext* context) {
  auto& manager = content::SpareRenderProcessHostManager::Get();
  if (spare)
    manager.WarmupSpare(context);
  else if (!manager.GetSpares().empty())
    manager.CleanupSparesForTesting();  // Named so; it is the only public way, and graceful.
}

// The checks, for all of a batch before any of it applies.
bool Validate(const base::DictValue& settings, bool engine, std::string* error) {
  for (auto entry : settings) {
    const std::string& key = entry.first;
    const base::Value& value = entry.second;
    bool known = true, valid = value.is_none();
    if (key == "thirdPartyCookies") valid |= ValueOf(kCookies, value).has_value();
    else if (key == "dialogs") valid |= ValueOf(kDialogs, value).has_value();
    else if (key == "downloads") valid |= ValueOf(kDownloads, value).has_value();
    else if (key == "newWindows") valid |= ValueOf(kWindows, value).has_value();
    else if (key == "spareRenderer") {
      if (!engine) {
        *error = "spareRenderer is the whole engine's: zebkit_set_engine_settings";
        return false;
      }
      valid |= value.is_bool();
    } else {
      known = false;
    }
    if (!known || !valid) {
      *error = (known ? "invalid runtime setting: " : "unknown runtime setting: ") + key;
      return false;
    }
  }
  return true;
}

std::optional<base::Value> Parse(const char* json, bool engine, std::string* error) {
  if (!json || strlen(json) > 65536) {
    *error = "runtime settings must be a JSON object of at most 64 KiB";
    return std::nullopt;
  }
  auto parsed = base::JSONReader::Read(json, 0);
  if (!parsed || !parsed->is_dict()) {
    *error = "runtime settings are not a JSON object";
    return std::nullopt;
  }
  if (!Validate(parsed->GetDict(), engine, error))
    return std::nullopt;
  return parsed;
}

char* Owned(const base::DictValue& explicit_settings, const base::DictValue& effective) {
  base::DictValue out;
  out.Set("explicit", explicit_settings.Clone());
  out.Set("effective", effective.Clone());
  return strdup(base::WriteJson(out).value_or("{}").c_str());
}

}  // namespace

namespace zebkit {

void StartRuntimeSettings() { g_running = true; }
void StopRuntimeSettings() {
  g_running = false;
  CookieContexts().clear();
  ContextCookies().clear();
}

bool BlocksThirdPartyCookies(content::BrowserContext* context) {
  CookieContexts().insert(context);
  auto& contexts = ContextCookies();
  if (auto it = contexts.find(context); it != contexts.end())
    return it->second;
  return Engine().block_third_party_cookies.value_or(
      !base::CommandLine::ForCurrentProcess()->HasSwitch("zebkit-allow-third-party-cookies"));
}
bool UsesSpareRenderer() {
  return Engine().spare_renderer.value_or(
      base::CommandLine::ForCurrentProcess()->HasSwitch("zebkit-spare-renderer"));
}
void SpareAfterLoad(content::BrowserContext* context) {
  if (g_running && Engine().spare_renderer)
    ApplySpare(*Engine().spare_renderer, context);
}
std::optional<zebkit_dialog_policy> EngineDialogPolicy() { return Engine().dialogs; }
std::optional<zebkit_popup_policy> EnginePopupPolicy() { return Engine().new_windows; }
bool EngineDeniesDownloads() { return Engine().deny_downloads.value_or(false); }

}  // namespace zebkit

extern "C" {

bool zebkit_set_engine_settings(const char* json, char** error) {
  std::string message;
  auto fail = [&]() { if (error) *error = strdup(message.c_str()); return false; };
  if (!IsReady(&message)) return fail();
  auto parsed = Parse(json, /*engine=*/true, &message);
  if (!parsed) return fail();
  const base::DictValue& settings = parsed->GetDict();
  auto& engine = Engine();
  if (const base::Value* value = settings.Find("dialogs")) engine.dialogs = ValueOf(kDialogs, *value);
  if (const base::Value* value = settings.Find("downloads")) engine.deny_downloads = ValueOf(kDownloads, *value);
  if (const base::Value* value = settings.Find("newWindows")) engine.new_windows = ValueOf(kWindows, *value);
  if (const base::Value* value = settings.Find("thirdPartyCookies")) {
    engine.block_third_party_cookies = ValueOf(kCookies, *value);
    // All current contexts: over what a view asked before this.
    ContextCookies().clear();
    for (content::BrowserContext* context : CookieContexts())
      ApplyCookies(context);
  }
  if (const base::Value* value = settings.Find("spareRenderer")) {
    engine.spare_renderer = value->is_bool() ? std::optional<bool>(value->GetBool()) : std::nullopt;
    ApplySpare(zebkit::UsesSpareRenderer(), zebkit::GetBrowserContext());
  }
  return true;
}

char* zebkit_get_engine_settings(void) {
  std::string message;
  if (!IsReady(&message)) return nullptr;
  const auto& engine = Engine();
  base::DictValue chosen, effective;
  PutName(chosen, "thirdPartyCookies", kCookies, engine.block_third_party_cookies);
  if (engine.spare_renderer) chosen.Set("spareRenderer", *engine.spare_renderer);
  PutName(chosen, "dialogs", kDialogs, engine.dialogs);
  PutName(chosen, "downloads", kDownloads, engine.deny_downloads);
  PutName(chosen, "newWindows", kWindows, engine.new_windows);
  effective.Set("thirdPartyCookies", NameOf(kCookies, engine.block_third_party_cookies.value_or(
      !base::CommandLine::ForCurrentProcess()->HasSwitch("zebkit-allow-third-party-cookies"))));
  effective.Set("spareRenderer", zebkit::UsesSpareRenderer());
  effective.Set("spareRendererRunning", !content::SpareRenderProcessHostManager::Get().GetSpares().empty());
  effective.Set("dialogs", NameOf(kDialogs, engine.dialogs.value_or(ZEBKIT_DIALOG_POLICY_DEFAULT)));
  effective.Set("downloads", NameOf(kDownloads, engine.deny_downloads.value_or(false)));
  effective.Set("newWindows", NameOf(kWindows, zebkit_view_get_popup_policy(nullptr)));
  return Owned(chosen, effective);
}

bool zebkit_view_set_runtime_settings(zebkit_view* view, const char* json, char** error) {
  std::string message;
  auto fail = [&]() { if (error) *error = strdup(message.c_str()); return false; };
  if (!IsReady(&message)) return fail();
  if (!view || !view->web_contents()) {
    message = "the view is closed";
    return fail();
  }
  auto parsed = Parse(json, /*engine=*/false, &message);
  if (!parsed) return fail();
  const base::DictValue& settings = parsed->GetDict();
  if (const base::Value* value = settings.Find("dialogs")) {
    if (auto policy = ValueOf(kDialogs, *value)) zebkit_view_set_dialog_policy(view, *policy);
    else zebkit::ClearDialogPolicy(view);
  }
  if (const base::Value* value = settings.Find("downloads")) zebkit::SetDownloadPolicy(view, ValueOf(kDownloads, *value));
  if (const base::Value* value = settings.Find("newWindows")) view->popup_policy_ = ValueOf(kWindows, *value);
  if (const base::Value* value = settings.Find("thirdPartyCookies")) {
    content::BrowserContext* context = view->web_contents()->GetBrowserContext();
    if (auto block = ValueOf(kCookies, *value)) ContextCookies()[context] = *block;
    else ContextCookies().erase(context);
    ApplyCookies(context);
  }
  return true;
}

char* zebkit_view_get_runtime_settings(zebkit_view* view) {
  std::string message;
  if (!IsReady(&message) || !view || !view->web_contents()) return nullptr;
  content::BrowserContext* context = view->web_contents()->GetBrowserContext();
  base::DictValue chosen, effective;
  auto& contexts = ContextCookies();
  if (auto it = contexts.find(context); it != contexts.end())
    chosen.Set("thirdPartyCookies", NameOf(kCookies, it->second));
  PutName(chosen, "dialogs", kDialogs, zebkit::ExplicitDialogPolicy(view));
  PutName(chosen, "downloads", kDownloads, view->deny_downloads_);
  PutName(chosen, "newWindows", kWindows, view->popup_policy_);
  effective.Set("thirdPartyCookies", NameOf(kCookies, zebkit::BlocksThirdPartyCookies(context)));
  effective.Set("dialogs", NameOf(kDialogs, zebkit_view_get_dialog_policy(view)));
  effective.Set("downloads", zebkit::EffectiveDownloads(view));
  effective.Set("newWindows", NameOf(kWindows, zebkit_view_get_popup_policy(view)));
  return Owned(chosen, effective);
}

}  // extern "C"
