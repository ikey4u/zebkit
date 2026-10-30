/* Native real-time access to a media element's encoded packets, displayed
 * video and PCM. See docs/MEDIA_TAP.md. Existing file-only capture remains
 * available in <zebkit/media.h>. */
#ifndef ZEBKIT_MEDIA_TAP_H
#define ZEBKIT_MEDIA_TAP_H
#include <zebkit/zebkit.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct zebkit_media_tap zebkit_media_tap;
/* Engine-thread callback. json and bytes are borrowed for this call only.
 * Copy what you need and return promptly. Freeing/stopping the tap here is
 * allowed. No callback follows free(). Events: audioConfig, videoConfig,
 * packet, video (RGBA8), audio (planar native-endian float32), timeline,
 * progress, done. Config payloads contain codec initialization bytes.
 * Timestamps are microseconds, unknown is -9223372036854775807.
 * PCM uses a monotonic captureTimeUs, not an invented media/source timestamp.
 */
typedef void (*zebkit_media_tap_func)(zebkit_media_tap *tap, const char *json,
                                     const uint8_t *bytes, size_t size, void *data);
/* media_id comes from zebkit_view_media_list(). options is a JSON object:
 * packets, video, audio (bool, default true); maxFps (1..60, default 10),
 * maxWidth/maxHeight (1..4096, default 1280/720), maxPendingBytes
 * (65536..67108864, default 16777216), includeBuffered (default false).
 * Optional absolute path records encoded packets using the existing writer;
 * recordingOptions is its options object (format, wallClock, limits, ...).
 * At least one output must be enabled. File/MSE/HLS support encoded outputs;
 * MediaStream supports video and PCM, not original demuxed packets/remuxing.
 * EME/protected media is refused. One tap per element; an active encoded
 * file capture conflicts with an encoded tap. Decoded-only taps can coexist.
 * maxFps samples displayed frames, not every decoded frame. Video is scaled
 * to fit the bounds without enlarging. Backpressure skips decoded samples
 * (reported in droppedSamples); encoded overflow ends with an explicit error.
 * Synchronous validation errors return NULL and allocated *error. Asynchronous
 * failures arrive as done events. Stop and wait for done before engine exit.
 */
ZEBKIT_API zebkit_media_tap *zebkit_media_tap_start(zebkit_view *view,
    const char *media_id, const char *options_json, zebkit_media_tap_func func,
    void *data, char **error);
ZEBKIT_API void zebkit_media_tap_stop(zebkit_media_tap *tap);
/* Stops, suppresses callbacks and releases the handle; any file is finalized
 * asynchronously while the engine runs. NULL is accepted by stop/free. */
ZEBKIT_API void zebkit_media_tap_free(zebkit_media_tap *tap);
#ifdef __cplusplus
}
#endif
#endif
