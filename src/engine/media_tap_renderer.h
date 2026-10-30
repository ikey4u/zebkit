#ifndef ZEBKIT_ENGINE_MEDIA_TAP_RENDERER_H_
#define ZEBKIT_ENGINE_MEDIA_TAP_RENDERER_H_
#include <memory>
#include <string>
#include "base/functional/callback.h"
#include "third_party/blink/public/web/web_element.h"
#include "zebkit/engine/media_capture.mojom.h"
namespace zebkit {
class NativeMediaTap {
 public:
  virtual ~NativeMediaTap() = default;
  virtual bool encoded() const = 0;
};
std::unique_ptr<NativeMediaTap> StartNativeMediaTap(
    blink::WebElement element, bool packets, bool video, bool audio,
    uint32_t fps, uint32_t width, uint32_t height, uint32_t pending,
    bool buffered, mojo::PendingRemote<mojom::MediaTapSink> sink,
    base::OnceClosure disconnected, std::string* error);
}
#endif
