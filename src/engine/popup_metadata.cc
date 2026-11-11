#include "zebkit/engine/popup_metadata.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/view.h"
#include "url/origin.h"
#include <stdlib.h>
#include <string.h>
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/task/single_thread_task_runner.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/mojom/window_features/window_features.mojom.h"

namespace zebkit {
base::DictValue WindowFeaturesInfo(const blink::mojom::WindowFeatures& features) {
  base::DictValue result, bounds;
  bounds.Set("x", features.bounds.x()); bounds.Set("y", features.bounds.y());
  bounds.Set("width", features.bounds.width()); bounds.Set("height", features.bounds.height());
  result.Set("bounds", std::move(bounds));
  result.Set("hasX", features.has_x); result.Set("hasY", features.has_y);
  result.Set("hasWidth", features.has_width); result.Set("hasHeight", features.has_height);
  result.Set("popup", features.is_popup); result.Set("alwaysOnTop", features.always_on_top);
  return result;
}
const char* WindowDispositionName(WindowOpenDisposition disposition) {
  switch (disposition) {
    case WindowOpenDisposition::UNKNOWN: return "unknown";
    case WindowOpenDisposition::CURRENT_TAB: return "current-tab";
    case WindowOpenDisposition::SINGLETON_TAB: return "singleton-tab";
    case WindowOpenDisposition::NEW_FOREGROUND_TAB: return "new-foreground-tab";
    case WindowOpenDisposition::NEW_BACKGROUND_TAB: return "new-background-tab";
    case WindowOpenDisposition::NEW_POPUP: return "new-popup";
    case WindowOpenDisposition::NEW_WINDOW: return "new-window";
    case WindowOpenDisposition::SAVE_TO_DISK: return "save-to-disk";
    case WindowOpenDisposition::OFF_THE_RECORD: return "off-the-record";
    case WindowOpenDisposition::IGNORE_ACTION: return "ignore-action";
    case WindowOpenDisposition::SWITCH_TO_TAB: return "switch-to-tab";
    case WindowOpenDisposition::NEW_PICTURE_IN_PICTURE: return "new-picture-in-picture";
    case WindowOpenDisposition::NEW_SPLIT_VIEW: return "new-split-view";
  }
  return "unknown";
}
zebkit_view* WindowViewForTarget(std::string_view target_id) {
  for (zebkit_view* view : zebkit_view::Views()) {
    const std::string* id = view->window_open_info_.FindString("targetId");
    if (id && *id == target_id && view->web_contents()) return view;
  }
  return nullptr;
}
void NotifyWindowOpenEvent(zebkit_view* view, base::DictValue event) {
  if (!view || !view->on_window_open_event_) return;
  auto json = base::WriteJson(event).value_or("{}");
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
      base::BindOnce([](base::WeakPtr<zebkit_view> target, uint64_t generation, std::string json) {
        if (target && target->window_open_event_generation_ == generation &&
            target->on_window_open_event_) {
          auto function = target->on_window_open_event_; void* data = target->on_window_open_event_data_;
          function(target.get(), json.c_str(), data);
        }
      }, view->GetWeakPtr(), view->window_open_event_generation_, std::move(json)));
}
void SetWindowOpenInfo(zebkit_view* opener, zebkit_view* popup, const GURL& url,
                       WindowOpenDisposition disposition, const blink::mojom::WindowFeatures& features,
                       bool user_gesture) {
  auto* contents = popup->web_contents();
  auto& info = popup->window_open_info_;
  info.Set("phase", "created"); info.Set("allowed", true);
  info.Set("url", url.spec());
  info.Set("frameName", contents->GetPrimaryMainFrame()->GetFrameName());
  info.Set("userGesture", user_gesture);
  info.Set("disposition", WindowDispositionName(disposition));
  info.Set("features", WindowFeaturesInfo(features));
  auto* source = contents->GetOpener();
  info.Set("hasOpener", source != nullptr);
  if (source) info.Set("openerOrigin", source->GetLastCommittedOrigin().Serialize());
  else info.Set("openerOrigin", base::Value());
  info.Set("topOrigin", opener->web_contents()->GetPrimaryMainFrame()->GetLastCommittedOrigin().Serialize());
  char* target = zebkit_view_get_target_id(popup);
  if (target) { info.Set("targetId", target); free(target); }
  NotifyWindowOpenEvent(opener, info.Clone());
}
}  // namespace zebkit

extern "C" {
void zebkit_view_on_window_open_event(zebkit_view* view, zebkit_window_open_event_func function, void* data) {
  if (!zebkit::LiveView(view) || !view->web_contents()) return;
  ++view->window_open_event_generation_;
  view->on_window_open_event_ = function; view->on_window_open_event_data_ = data;
}
char* zebkit_view_get_window_open_info(zebkit_view* view) {
  if (!zebkit::LiveView(view) || !view->web_contents()) return nullptr;
  return strdup(base::WriteJson(view->window_open_info_).value_or("{}").c_str());
}
}
