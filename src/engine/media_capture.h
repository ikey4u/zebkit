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

namespace content {
class DevToolsAgentHostClientChannel;
class WebContents;
}  // namespace content

namespace zebkit {

using MediaJsonCallback = base::OnceCallback<void(std::string json)>;

// zebkit_view_media_list(), zebkit_view_media_at() and
// zebkit_media_capture_start() for the page of @contents.
void ListMedia(content::WebContents* contents, MediaJsonCallback done);
void MediaAt(content::WebContents* contents,
             float x,
             float y,
             MediaJsonCallback done);
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
// to the session that last spoke of the capture. Stop and status work
// from any session, the browser target's too.
bool IsMediaCommand(std::string_view method);
void HandleMediaCommand(content::DevToolsAgentHostClientChannel* channel,
                        int id,
                        std::string_view method,
                        const std::string& params_json);

// A message to a CDP client, if it is still attached (devtools.cc).
bool SendToDevToolsClient(content::DevToolsAgentHostClientChannel* channel,
                          base::DictValue message);

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_MEDIA_CAPTURE_H_
