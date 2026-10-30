#ifndef ZEBKIT_ENGINE_POPUP_METADATA_H_
#define ZEBKIT_ENGINE_POPUP_METADATA_H_
#include <string_view>
#include "base/values.h"
#include "third_party/blink/public/mojom/window_features/window_features.mojom-forward.h"
#include "ui/base/window_open_disposition.h"
class GURL;
struct zebkit_view;
namespace zebkit {
base::DictValue WindowFeaturesInfo(const blink::mojom::WindowFeatures& features);
const char* WindowDispositionName(WindowOpenDisposition disposition);
zebkit_view* WindowViewForTarget(std::string_view target_id);
void NotifyWindowOpenEvent(zebkit_view* view, base::DictValue event);
void SetWindowOpenInfo(zebkit_view* opener, zebkit_view* popup, const GURL& url,
                       WindowOpenDisposition disposition, const blink::mojom::WindowFeatures& features,
                       bool user_gesture);
}
#endif
