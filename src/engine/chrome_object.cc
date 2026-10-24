// window.chrome in pages, as Chrome's renderer makes it: chrome.loadTimes()
// and chrome.csi(), the timing of the page's load, by Chrome's own code
// (chrome/renderer/loadtimes_bindings.cc, built here as it is). Sites read
// window.chrome to know that they run in Chrome; the content shell has none.
// chrome.app is the extension system's, which zebkit does not have.

#include "chrome/renderer/loadtimes_bindings.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_frame_observer.h"
#include "v8/include/v8-context.h"
#include "zebkit/engine/engine.h"

namespace zebkit {

namespace {

// In every script context of the frame, as ChromeRenderFrameObserver
// installs them. Deletes itself with the frame.
class ChromeObject : public content::RenderFrameObserver {
 public:
  explicit ChromeObject(content::RenderFrame* render_frame)
      : content::RenderFrameObserver(render_frame) {}

  void DidCreateScriptContext(v8::Local<v8::Context> context,
                              int32_t world_id) override {
    LoadTimesBindings::Install(context);
  }

  void OnDestruct() override { delete this; }
};

}  // namespace

void InstallChromeObject(content::RenderFrame* render_frame) {
  new ChromeObject(render_frame);
}

}  // namespace zebkit
