/* Native, synchronous media tools using the FFmpeg built into zebkit.
 * No engine initialization, shell, subprocess or system FFmpeg required.
 * Calls may run concurrently on worker threads with distinct output paths.
 * Strings returned through json/error belong to the caller: zebkit_free().
 */
#ifndef ZEBKIT_MEDIA_TOOLS_H
#define ZEBKIT_MEDIA_TOOLS_H
#include "zebkit.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Called on the calling thread, initially and while processing packets.
 * Return nonzero to continue, zero to cancel. time_us can be INT64_MIN when
 * unknown. Do not throw/unwind through the callback. */
typedef int (*zebkit_media_progress_func)(int64_t packets, int64_t time_us, void *data);
/* argv excludes the executable name. All paths are local regular files.
 * ffprobe: [-show_format] [-show_streams] [-of json|-print_format json]
 *          [-i] INPUT
 * With neither show switch, both format and streams are returned.
 * ffmpeg: [-y|-n] -i INPUT [-map 0:INDEX]... [-vn] [-an]
 *   (-c copy | -c:a pcm_s16le | -c:v rawvideo) [-f FORMAT]
 *   [-ar RATE] [-ac CHANNELS] [-s WIDTHxHEIGHT] [-frames:v COUNT]
 *   [-pix_fmt rgba] OUTPUT
 * copy supports MP4/MOV/Matroska/WebM; PCM supports WAV/s16le; video RGBA.
 * Unsupported switches/codecs fail explicitly. See docs/MEDIA_TOOLS.md.
 * Existing outputs are preserved unless -y; output is staged in a sibling
 * temporary file and published only on success. No partial output on cancel.
 * Returns 0 on success, 1 on cancellation, -1 on failure. error may be NULL.
 */
ZEBKIT_API int zebkit_ffprobe(int argc, const char *const *argv,
                            char **json, char **error);
ZEBKIT_API int zebkit_ffmpeg(int argc, const char *const *argv,
                           zebkit_media_progress_func progress, void *data,
                           char **error);
/* Convenience probe, equivalent to ffprobe INPUT, returns both sections. */
ZEBKIT_API int zebkit_media_probe(const char *path, char **json, char **error);
#ifdef __cplusplus
}
#endif
#endif
