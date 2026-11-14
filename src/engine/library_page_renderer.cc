// A JavaScript library's page, its renderer's side (library_page.cc has the
// browser's): V8's heap limit in the page's renderer (heapLimitMB, given as
// --max-old-space-size) told to the browser as it is reached, so that the
// library ends with OUT_OF_MEMORY. V8 itself ends a renderer whose heap is
// full by crashing it, which the browser cannot tell from any other crash.
// The page's runner (library_page_runner.h) is the rest of this side.

#include <algorithm>

#include "base/functional/callback_helpers.h"
#include "base/memory/raw_ptr.h"
#include "content/public/common/isolated_world_ids.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_frame_observer.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "third_party/blink/public/platform/web_security_origin.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "zebkit/engine/library_page.h"
#include "zebkit/engine/page_call.mojom.h"

namespace zebkit {

namespace {

class LibraryPageAgent : public content::RenderFrameObserver {
 public:
  explicit LibraryPageAgent(content::RenderFrame* frame) : content::RenderFrameObserver(frame) {}

  // content::RenderFrameObserver
  void DidCreateScriptContext(v8::Local<v8::Context> context, int32_t world_id) override {
    if (isolate_ || world_id != content::ISOLATED_WORLD_ID_GLOBAL)
      return;
    blink::WebLocalFrame* frame = render_frame()->GetWebFrame();
    // Only a library's page, whose renderer is its own (a profile of its own).
    if (!frame->IsOutermostMainFrame() || frame->GetSecurityOrigin().Protocol().Utf8() != kLibraryScheme)
      return;
    render_frame()->GetRemoteAssociatedInterfaces()->GetInterface(&host_);
    isolate_ = v8::Isolate::GetCurrent();
    isolate_->AddNearHeapLimitCallback(&NearHeapLimit, this);
  }

  void OnDestruct() override {
    if (isolate_)
      isolate_->RemoveNearHeapLimitCallback(&NearHeapLimit, 0);
    delete this;
  }

 private:
  static size_t NearHeapLimit(void* data, size_t current, size_t initial) {
    auto* self = static_cast<LibraryPageAgent*>(data);
    if (!self->told_ && self->host_) {
      self->told_ = true;
      self->host_->Call("library", "oom", mojo_base::BigBuffer(), mojo_base::BigBuffer(), base::DoNothing());
    }
    // Room while the browser stops the renderer.
    return current + std::max(current / 2, size_t{64} << 20);
  }

  mojo::AssociatedRemote<mojom::PageCallHost> host_;
  raw_ptr<v8::Isolate> isolate_ = nullptr;
  bool told_ = false;
};

}  // namespace

void InstallLibraryPageAgent(content::RenderFrame* render_frame) {
  new LibraryPageAgent(render_frame);
}

}  // namespace zebkit
