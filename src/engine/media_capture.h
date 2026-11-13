// Media capture inside the engine (media_capture.cc): the media list and
// captures of a page, for <zebkit/media.h> and for CDP clients
// (Zebkit.media* commands, devtools.cc).

#ifndef ZEBKIT_ENGINE_MEDIA_CAPTURE_H_
#define ZEBKIT_ENGINE_MEDIA_CAPTURE_H_

#include <string>
#include <string_view>

#include "base/functional/callback.h"
#include "base/values.h"
#include "zebkit/engine/include/zebkit/media.h"

namespace base {
class CommandLine;
}  // namespace base

namespace content {
class RenderFrameHost;
class DevToolsAgentHostClientChannel;
class WebContents;
}  // namespace content

namespace zebkit {

// zebkit_media_keep_history(), for every renderer: they keep what a capture
// starts from from their start (media_capture_renderer.cc).
inline constexpr char kMediaHistorySwitch[] = "zebkit-media-history";
void AppendMediaSwitch(base::CommandLine* command_line);

using MediaJsonCallback = base::OnceCallback<void(std::string json)>;

// zebkit_view_media_list(), zebkit_view_media_at() and
// zebkit_media_capture_start() for the page of @contents.
void ListMedia(content::WebContents* contents, MediaJsonCallback done);
void MediaAt(content::WebContents* contents,
             float x,
             float y,
             MediaJsonCallback done);
// The frame of a media id of the media list ("<process>.<frame>.<element>"),
// live and of @contents, and the element's number in it; null, with why in
// @error, otherwise (zebkit_media_capture_start(), zebkit_media_tap_start()).
content::RenderFrameHost* FrameOfMediaId(content::WebContents* contents,
                                         const std::string& media_id,
                                         int* element,
                                         std::string* error);
zebkit_media_capture* StartMediaCapture(content::WebContents* contents,
                                        const std::string& media_id,
                                        const std::string& path,
                                        const std::string& options_json,
                                        std::string* error);

// The same over CDP, for any page target, the engine's views or not:
//
//   Zebkit.mediaList {}                    -> {media: [...]}
//   Zebkit.mediaAt {x, y}                  -> {media: {...} | null}
//   Zebkit.mediaCaptureStart {mediaId, path, options?}  -> {captureId}
//   Zebkit.mediaCaptureStop {captureId}    -> {}
//   Zebkit.mediaCaptureStatus {captureId}  -> {status: {...}}, the result
//                                            once done (the last 64 kept)
//   events Zebkit.mediaCaptureProgress {captureId, state},
//          Zebkit.mediaCaptureDone {captureId, result}
//
// A capture is the session's that started it: its events go there, only
// it stops it and reads its status, and its captures are stopped (their
// files written) when it detaches.
bool IsMediaCommand(std::string_view method);
void HandleMediaCommand(content::DevToolsAgentHostClientChannel* channel,
                        int id,
                        std::string_view method,
                        const std::string& params_json);
void OnMediaClientDetached(content::DevToolsAgentHostClientChannel* channel);

// A message to a CDP client, if it is still attached (devtools.cc).
bool SendToDevToolsClient(content::DevToolsAgentHostClientChannel* channel,
                          base::DictValue message);

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_MEDIA_CAPTURE_H_
