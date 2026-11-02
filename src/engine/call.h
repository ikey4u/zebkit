// zebkit.* calls from a page: the browser's side of the frame's channel
// (call.cc; page_call_renderer.cc has the renderer's).

#ifndef ZEBKIT_ENGINE_CALL_H_
#define ZEBKIT_ENGINE_CALL_H_

#include <string_view>

#include "mojo/public/cpp/bindings/pending_associated_receiver.h"
#include "zebkit/engine/page_call.mojom-forward.h"

namespace content {
class RenderFrame;
class RenderFrameHost;
}  // namespace content

namespace zebkit {

// The calls of @frame's pages.
void BindPageCallHost(
    content::RenderFrameHost* frame,
    mojo::PendingAssociatedReceiver<mojom::PageCallHost> receiver);

// The renderer's side, in each frame (page_call_renderer.cc).
void InstallPageCallAgent(content::RenderFrame* render_frame);

// Whether @text is one JSON value (an array when @array) that can be
// handed on as it is, without being parsed and written again: UTF-8, as
// deep as JSONReader takes, no \u escape of a lone surrogate. @bytes_key,
// when given, is told whether an object in it has a key "$bytes", or one
// with an escape.
bool IsPlainJson(std::string_view text, bool array, bool* bytes_key = nullptr);

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_CALL_H_
