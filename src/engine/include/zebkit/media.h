/*
 * zebkit: the media a page plays, saved as it plays.
 *
 * A page's <audio> and <video> elements are listed with what they play,
 * and what one of them plays is copied to a file while it plays: the
 * encoded audio and video its demuxer gives its decoders, put in an MP4,
 * WebM or Matroska file by the engine's FFmpeg without decoding or
 * encoding anything. That is the same for a file the element loads (src=),
 * for what a player script appends through Media Source Extensions (DASH
 * and HLS players) and for HLS the engine plays itself: whatever the page
 * delivers, the copy is what the browser plays.
 *
 * A capture has what the player has from the playhead on: what it holds
 * ahead of the playhead already, then what it loads while the capture
 * runs. It does not load anything itself: parts the player never loads
 * (a seek past them) are not in the file, which reports the ranges it has.
 * Encrypted media (Encrypted Media Extensions) cannot be captured.
 *
 * Everything here is for the engine's thread, as in <zebkit/zebkit.h>,
 * for windowed and off-screen views alike.
 */

#ifndef ZEBKIT_MEDIA_H
#define ZEBKIT_MEDIA_H

#include <zebkit/zebkit.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The <audio> and <video> elements in every frame of the view's page, as
 * a JSON array to @func (its @error is NULL):
 *
 *     [{"id": "4.12.3",            the media's id for a capture
 *       "element": 3, "tag": "video",
 *       "path": "body > div:nth-of-type(2) > video:nth-of-type(1)",
 *       "frame": {"url": "https://...", "main": true, "name": ""},
 *       "src": "https://.../movie.mp4",   a blob: URL for MSE
 *       "kind": "file",    "file" (src=), "mse" (Media Source Extensions),
 *                          "hls" (the engine's own HLS), "stream" (a
 *                          camera or WebRTC: not capturable), "none"
 *                          (nothing loaded)
 *       "duration": 60.0,  null when not known; "live": true for streams
 *                          without an end
 *       "currentTime": 12.5,
 *       "buffered": [[0, 30.2]],
 *       "codecs": {"video": "h264", "audio": "aac"},
 *       "hasVideo": true, "hasAudio": true, "width": 1280, "height": 720,
 *       "playing": true, "encrypted": false,
 *       "capturable": true, "capturing": false}]
 *
 * "path" is a CSS selector of the element in its frame's document.
 */
ZEBKIT_API void zebkit_view_media_list(zebkit_view *view, zebkit_result_func func, void *data);

/*
 * The media element at (@x, @y) of the view, in the view's coordinates
 * (device-independent pixels, as its input events have them: CSS pixels
 * times the page's zoom): the element the point hits or is inside of, or
 * else the smallest one whose box holds the point (players draw their
 * controls over the video). Frames of other sites (in other processes)
 * are searched too. Its entry as zebkit_view_media_list() has it, or
 * "null", to @func.
 *
 * The first zebkit_view_media_list() or zebkit_view_media_at() in a page's
 * process readies its players for captures: from then on they keep the
 * video since their last keyframe (up to 16 MiB), and a capture of a file
 * an element loads starts at the keyframe of what is shown. A player that
 * had shown frames before starts at its next keyframe. Players of a
 * process never asked keep nothing.
 */
ZEBKIT_API void zebkit_view_media_at(zebkit_view *view, float x, float y, zebkit_result_func func, void *data);

/*
 * Players keep what a capture starts from from the engine's start: the
 * video since their last keyframe, up to about 16 MiB for each video
 * stream they play, so that a capture of a file an element loads starts at
 * the frame on screen even when nothing has listed media yet. Off by
 * default: then players keep it only once zebkit_view_media_list() or
 * zebkit_view_media_at() ran in their page's process (see above). Before
 * zebkit_main(); every renderer learns of it. The same from the command
 * line: --zebkit-media-history.
 */
ZEBKIT_API void zebkit_media_keep_history(bool keep);

/* A capture, from zebkit_media_capture_start() until zebkit_media_capture_free(). */
typedef struct zebkit_media_capture zebkit_media_capture;

/* The capture's state as JSON (see zebkit_media_capture_status()). */
typedef void (*zebkit_media_capture_func)(zebkit_media_capture *capture, const char *json, void *data);

/*
 * Starts copying what media @media_id (an "id" of zebkit_view_media_list())
 * plays to @path, an absolute path whose directory is made if needed. The
 * options are a JSON object, NULL or "{}" for the defaults:
 *
 *   "format": "mp4", "webm", "mkv" or "auto" (the default): the container.
 *       With "auto" the path's extension chooses it if that container
 *       holds the codecs; otherwise WebM for VP8, VP9, AV1, Opus and
 *       Vorbis, MP4 for what MP4 holds (H.264, HEVC, AAC, Opus, ...),
 *       Matroska for the rest. The extension is made to match
 *       (".m4a" and ".mka" for audio alone): the files the capture
 *       reports are where it wrote.
 *   "includeBuffered": true takes everything the player holds, before
 *       the playhead too (what a Media Source Extensions player keeps
 *       behind it), not only from the playhead on.
 *   "closeGaps": seconds (1 by default), or false: a gap in what was
 *       captured longer than that (a seek forward) is closed up, so that
 *       the file plays through; the file reports where its gaps were.
 *   "maxBytes": what the capture holds at most (16 GiB by default), and
 *   "maxDuration": seconds of the media at most (no limit by default).
 *       A capture that reaches one ends with an "error" saying so, and
 *       its files have what it captured until then.
 *
 * The capture's packets are kept while it runs in a new file of its own
 * in @path's directory, never one that is there: without a name outside
 * Windows, so that nothing is left whatever ends the capture; on Windows
 * it goes when the capture ends. @path is replaced if it is there; the other names a
 * capture makes ("movie-2.mp4", or another extension for the container)
 * never replace a file: "movie-2 (1).mp4". A capture also ends with an
 * error when the page gives more than media has (more than 8 tracks, a
 * track that changes its configuration more than 32 times).
 *
 * Returns NULL with @error (the caller frees it) when the media or the
 * path cannot be used. Anything found later (the element went, the media
 * is encrypted) ends the capture with an error, reported to its done
 * function. The file is written when the capture stops: by
 * zebkit_media_capture_stop(), or when the page goes or the element loads
 * other media.
 */
ZEBKIT_API zebkit_media_capture *zebkit_media_capture_start(zebkit_view *view, const char *media_id, const char *path,
                                                            const char *options_json, char **error);

/*
 * @func gets the capture's state about twice a second while it captures:
 *
 *     {"state": "capturing", "bytes": 1048576, "packets": 900,
 *      "duration": 12.0, "ranges": [[10.0, 22.0]],
 *      "tracks": [{"track": 0, "type": "video", "codec": "h264",
 *                  "width": 1280, "height": 720, "ranges": [[10.0, 22.0]]},
 *                 {"track": 1, "type": "audio", "codec": "aac",
 *                  "sampleRate": 44100, "channels": 2, "ranges": [...]}]}
 *
 * "ranges" are the times of the media captured, in its own timeline;
 * "bytes" is what the capture holds. NULL stops.
 */
ZEBKIT_API void zebkit_media_capture_on_progress(zebkit_media_capture *capture, zebkit_media_capture_func func,
                                                 void *data);

/*
 * @func gets the capture's end, once, when its files are written:
 *
 *     {"state": "done", "reason": "stopped",
 *      "error": "...",            only when it failed, or found nothing
 *      "bytes": 1300000, "duration": 12.0, "ranges": [[10.0, 22.0]],
 *      "tracks": [...],
 *      "files": [{"path": "/.../movie.mp4", "format": "mp4",
 *                 "start": 10.0, "duration": 12.0, "bytes": 1300000,
 *                 "gaps": [[15.0, 40.0]],
 *                 "tracks": [{"type": "video", "codec": "h264",
 *                             "width": 1280, "height": 720,
 *                             "packets": 360}, ...]}]}
 *
 * "reason" is "stopped", "the page went away", "the element loaded other
 * media", "the player stopped" or "error". A track whose configuration
 * changes while it is captured (another resolution in an adaptive stream,
 * another codec) goes on in another file, "movie-2.mp4", since a track of
 * a file has one: "files" lists them in order. "start" is where the
 * file's first frame is in the media; "gaps" are the stretches of the
 * media closed up in it.
 */
ZEBKIT_API void zebkit_media_capture_on_done(zebkit_media_capture *capture, zebkit_media_capture_func func,
                                             void *data);

/* Stops the capture and writes its files; its done function follows. */
ZEBKIT_API void zebkit_media_capture_stop(zebkit_media_capture *capture);

/*
 * The capture's state now, as its progress or done function has it, with
 * "state" "starting", "capturing", "finishing" or "done", and "media",
 * the element's entry when it started. The caller frees it.
 */
ZEBKIT_API char *zebkit_media_capture_status(zebkit_media_capture *capture);

/*
 * Lets the capture go: its functions are not called any more. A capture
 * still running is stopped, and its files are written as for
 * zebkit_media_capture_stop().
 *
 * A capture still running when the engine quits does not hold it up, and
 * is not written: stop it, and wait for its done function, first.
 */
ZEBKIT_API void zebkit_media_capture_free(zebkit_media_capture *capture);

#ifdef __cplusplus
}
#endif

#endif /* ZEBKIT_MEDIA_H */
