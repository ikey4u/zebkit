/* What a page plays, saved as it plays (<zebkit/media.h>), in a headless
 * view, every file read back with the engine's own FFmpeg (its streams,
 * codecs, duration, and every frame decoded):
 *
 * - an MP4 (H.264 with B-frames, AAC) and a WebM (VP9, Opus) the element
 *   loads itself (src=), captured from the start as they play;
 * - the MP4 as DASH through Media Source Extensions: three segments
 *   appended before the capture, the rest while it runs; and again with
 *   what the player holds behind the playhead (includeBuffered);
 * - a seek forward during a capture: two ranges, the gap closed up;
 * - <audio> alone (Opus), to a path without an extension;
 * - the media element under the player's controls (zebkit_view_media_at);
 * - an element that loads other media during a capture;
 * - encrypted media (CENC) refused, and ids that are not media.
 *
 *   ZEBKIT_TEST_BASE   http://127.0.0.1:<port>, serving tests/engine/pages
 *   ZEBKIT_TEST_FILES  a directory for the files written
 *
 * scripts/engine-test.sh runs it with --headless. Exit 0 when every check
 * passes. */

#define _GNU_SOURCE
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <zebkit/media.h>
#include <zebkit/vendor/ffmpeg.h>
#include <zebkit/zebkit.h>

static int failures, checks;

static void check(int ok, const char *what, const char *format, ...) {
    ++checks;
    if (!ok)
        ++failures;
    char detail[1024] = "";
    if (format) {
        va_list args;
        va_start(args, format);
        vsnprintf(detail, sizeof(detail), format, args);
        va_end(args);
    }
    printf("%s %s%s%s\n", ok ? "ok  " : "FAIL", what, *detail ? ": " : "", detail);
    fflush(stdout);
}

static int64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static zebkit_view *view;
static char base[256], files[512];

/* ---- JSON, as far as the checks need it ------------------------------ */

/* The number after "key": in @json (after @from, if given), or @otherwise. */
static double number(const char *json, const char *key, double otherwise) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *at = json ? strstr(json, pattern) : NULL;
    return at ? strtod(at + strlen(pattern), NULL) : otherwise;
}

/* The string after "key": in @json, into @out. */
static int string(const char *json, const char *key, char *out, size_t size) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *at = json ? strstr(json, pattern) : NULL;
    out[0] = 0;
    if (!at)
        return 0;
    at += strlen(pattern);
    size_t n = 0;
    while (at[n] && at[n] != '"' && n + 1 < size)
        ++n;
    memcpy(out, at, n);
    out[n] = 0;
    return 1;
}

/* How many [a,b] pairs the array after "key": has. */
static int pairs(const char *json, const char *key) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":[", key);
    const char *at = json ? strstr(json, pattern) : NULL;
    if (!at)
        return -1;
    at += strlen(pattern);
    int count = 0, depth = 1;
    for (; *at && depth > 0; ++at) {
        if (*at == '[') {
            ++depth;
            if (depth == 2)
                ++count;
        } else if (*at == ']') {
            --depth;
        }
    }
    return count;
}

/* ---- Files read back ------------------------------------------------- */

typedef struct {
    int ok;
    int streams;
    int video, audio; /* AVCodecID, or 0 */
    int width, height;
    double duration;
    int video_frames, audio_frames, errors;
} probe_t;

static probe_t probe(const char *path) {
    probe_t p = {0};
    AVFormatContext *context = NULL;
    if (avformat_open_input(&context, path, NULL, NULL) < 0)
        return p;
    if (avformat_find_stream_info(context, NULL) < 0) {
        avformat_close_input(&context);
        return p;
    }
    p.ok = 1;
    p.streams = (int)context->nb_streams;
    p.duration = context->duration > 0 ? context->duration / 1e6 : 0;
    AVCodecContext *decoders[8] = {0};
    for (unsigned i = 0; i < context->nb_streams && i < 8; ++i) {
        AVCodecParameters *par = context->streams[i]->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
            p.video = par->codec_id, p.width = par->width, p.height = par->height;
        } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
            p.audio = par->codec_id;
        }
        const AVCodec *codec = avcodec_find_decoder(par->codec_id);
        if (!codec)
            continue;
        decoders[i] = avcodec_alloc_context3(codec);
        if (avcodec_parameters_to_context(decoders[i], par) < 0 || avcodec_open2(decoders[i], codec, NULL) < 0)
            avcodec_free_context(&decoders[i]);
    }
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    for (int flushing = 0; flushing < 2; ++flushing) {
        for (;;) {
            int index = -1;
            if (!flushing) {
                if (av_read_frame(context, packet) < 0)
                    break;
                index = packet->stream_index;
            }
            for (unsigned i = 0; i < context->nb_streams && i < 8; ++i) {
                if (!decoders[i] || (!flushing && (int)i != index))
                    continue;
                if (avcodec_send_packet(decoders[i], flushing ? NULL : packet) < 0)
                    ++p.errors;
                int got;
                while ((got = avcodec_receive_frame(decoders[i], frame)) >= 0) {
                    if (decoders[i]->codec_type == AVMEDIA_TYPE_VIDEO)
                        ++p.video_frames;
                    else
                        ++p.audio_frames;
                    av_frame_unref(frame);
                }
            }
            if (flushing)
                break;
            av_packet_unref(packet);
        }
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
    for (int i = 0; i < 8; ++i)
        avcodec_free_context(&decoders[i]);
    avformat_close_input(&context);
    return p;
}

/* ---- Steps ----------------------------------------------------------- */

typedef void (*step_func)(void);
static step_func steps[128];
static int step_count, current;
static char result[65536];

static void run_step(void *data) {
    (void)data;
    if (current >= step_count) {
        printf("%d of %d checks passed\n", checks - failures, checks);
        zebkit_quit();
        return;
    }
    steps[current++]();
}

static void next(uint32_t delay_ms) { zebkit_post_delayed_task(run_step, NULL, delay_ms); }

static void evaluated(zebkit_view *v, const char *json, const char *error, void *data) {
    (void)v, (void)data;
    snprintf(result, sizeof(result), "%s", json ? json : error ? error : "");
    next(0);
}
static void evaluate(const char *script) { zebkit_view_evaluate(view, script, evaluated, NULL); }

/* Evaluates @script until it is @expected, or @timeout_ms passes. */
static const char *poll_script, *poll_expected, *poll_what;
static int64_t poll_until;
static void poll_evaluated(zebkit_view *v, const char *json, const char *error, void *data);
static void poll_again(void *data) {
    (void)data;
    zebkit_view_evaluate(view, poll_script, poll_evaluated, NULL);
}
static void poll_evaluated(zebkit_view *v, const char *json, const char *error, void *data) {
    (void)v, (void)data, (void)error;
    if (json && strcmp(json, poll_expected) == 0) {
        check(1, poll_what, NULL);
        next(0);
    } else if (now_us() > poll_until) {
        check(0, poll_what, "timed out: %s", json ? json : error ? error : "");
        next(0);
    } else {
        zebkit_post_delayed_task(poll_again, NULL, 50);
    }
}
static void wait_script(const char *script, const char *expected, uint32_t timeout_ms, const char *what) {
    poll_script = script, poll_expected = expected, poll_what = what;
    poll_until = now_us() + (int64_t)timeout_ms * 1000;
    poll_again(NULL);
}

static int loads;
static void on_load(zebkit_view *v, zebkit_load_event event, const char *url, void *data) {
    (void)v, (void)data;
    if (event == ZEBKIT_LOAD_FINISHED && strstr(url, "media.html"))
        ++loads;
}
static void wait_loaded(void *data) {
    (void)data;
    if (loads) {
        check(1, "the page loads", NULL);
        next(0);
    } else {
        zebkit_post_delayed_task(wait_loaded, NULL, 50);
    }
}

/* ---- Media and captures ---------------------------------------------- */

static char list[16384], media_id[64];

static void listed(zebkit_view *v, const char *json, const char *error, void *data) {
    (void)v, (void)data, (void)error;
    snprintf(list, sizeof(list), "%s", json ? json : "");
    string(list, "id", media_id, sizeof(media_id));
    next(0);
}
static void list_media(void) { zebkit_view_media_list(view, listed, NULL); }

static zebkit_media_capture *capture;
static char done_json[16384], progress_json[8192];
static int done_count, progress_count;
static char capture_path[600];

static void on_progress(zebkit_media_capture *c, const char *json, void *data) {
    (void)c, (void)data;
    ++progress_count;
    snprintf(progress_json, sizeof(progress_json), "%s", json);
}
static void on_done(zebkit_media_capture *c, const char *json, void *data) {
    (void)data;
    ++done_count;
    snprintf(done_json, sizeof(done_json), "%s", json);
    zebkit_media_capture_free(c);
    if (c == capture)
        capture = NULL;
}

static void start_capture(const char *name, const char *options) {
    snprintf(capture_path, sizeof(capture_path), "%s/%s", files, name);
    done_count = progress_count = 0;
    done_json[0] = progress_json[0] = 0;
    char *error = NULL;
    capture = zebkit_media_capture_start(view, media_id, capture_path, options, &error);
    check(capture != NULL, "the capture starts", "%s: %s", media_id, error ? error : "");
    zebkit_free(error);
    if (capture) {
        zebkit_media_capture_on_progress(capture, on_progress, NULL);
        zebkit_media_capture_on_done(capture, on_done, NULL);
    }
}

static int64_t done_until;
static void wait_done_poll(void *data) {
    (void)data;
    if (done_count) {
        check(1, "the capture ends", NULL);
        next(0);
    } else if (now_us() > done_until) {
        check(0, "the capture ends", "timed out");
        next(0);
    } else {
        zebkit_post_delayed_task(wait_done_poll, NULL, 50);
    }
}
static void wait_done(uint32_t timeout_ms) {
    done_until = now_us() + (int64_t)timeout_ms * 1000;
    wait_done_poll(NULL);
}

static void stop_and_wait(void) {
    if (capture)
        zebkit_media_capture_stop(capture);
    wait_done(30000);
}

/* The first file of the capture, read back. */
static probe_t first_file(char *path, size_t size) {
    probe_t p = {0};
    const char *files_at = strstr(done_json, "\"files\":[");
    if (!files_at || !string(files_at, "path", path, size))
        return p;
    return probe(path);
}

static int ends_with(const char *text, const char *end) {
    size_t a = strlen(text), b = strlen(end);
    return a >= b && strcmp(text + a - b, end) == 0;
}

/* A whole 8 s clip: both tracks, its codecs, all its frames. */
static void check_clip(const char *what, const char *extension, int video, int audio) {
    char error[256] = "", path[600] = "";
    string(done_json, "error", error, sizeof(error));
    check(!*error, what, "no error: %s", error);
    check(progress_count > 0, what, "progress reported %d times", progress_count);
    probe_t p = first_file(path, sizeof(path));
    check(ends_with(path, extension), what, "written to %s", path);
    check(p.ok && p.streams == 2, what, "%d streams in the file", p.streams);
    check(p.video == video && p.audio == audio, what, "codecs %d and %d", p.video, p.audio);
    check(p.width == 320 && p.height == 180, what, "%dx%d", p.width, p.height);
    check(p.duration > 7.4 && p.duration < 8.6, what, "%.2f s long", p.duration);
    /* 8 s at 24 frames a second; Opus and AAC frames of 20 and 21 ms. */
    check(p.video_frames >= 185 && p.video_frames <= 193, what, "%d video frames decoded", p.video_frames);
    check(p.audio_frames >= 350, what, "%d audio frames decoded", p.audio_frames);
    check(p.errors == 0, what, "%d decoding errors", p.errors);
}

/* ---- The steps -------------------------------------------------------- */

static void s_load(void) {
    char url[512];
    snprintf(url, sizeof(url), "%s/media.html", base);
    zebkit_view_load_url(view, url);
    wait_loaded(NULL);
}

/* src= MP4 */
static void s_mp4(void) { evaluate("file('video', 'media/clip.mp4')"); }
static void s_mp4_ready(void) { wait_script("ready()", "true", 10000, "the MP4 loads"); }
static void s_mp4_list(void) {
    check(strstr(list, "\"kind\":\"file\"") && strstr(list, "\"video\":\"h264\"") &&
              strstr(list, "\"audio\":\"aac\"") && strstr(list, "\"capturable\":true") && *media_id,
          "the MP4 is listed, as a file, with its codecs", "%s", list);
    check(strstr(list, "\"path\":\"div#player > video:nth-of-type(1)\"") != NULL, "with its place in the page",
          NULL);
    next(0);
}
static char at_json[4096];
static void at_done(zebkit_view *v, const char *json, const char *error, void *data) {
    (void)v, (void)data, (void)error;
    snprintf(at_json, sizeof(at_json), "%s", json ? json : "");
    next(0);
}
static void s_mp4_at(void) { zebkit_view_media_at(view, 160, 160, at_done, NULL); }
static void s_mp4_at_check(void) {
    char id[64];
    string(at_json, "id", id, sizeof(id));
    check(strcmp(id, media_id) == 0, "the element under the player's controls is the video", "%s", at_json);
    next(0);
}
static void s_mp4_at_none(void) { zebkit_view_media_at(view, 600, 500, at_done, NULL); }
static void s_mp4_at_none_check(void) {
    check(strcmp(at_json, "null") == 0, "and nothing where there is none", "%s", at_json);
    next(0);
}
static void s_mp4_start(void) {
    start_capture("file.mp4", NULL);
    evaluate("play(2)");
}
static void s_mp4_play(void) { wait_script("state.ended", "true", 30000, "the MP4 plays to its end"); }
static void s_mp4_check(void) {
    check_clip("src= MP4", ".mp4", AV_CODEC_ID_H264, AV_CODEC_ID_AAC);
    check(number(done_json, "duration", 0) > 7.4, "its ranges cover the clip", "%s", done_json);
    next(0);
}

/* src= WebM */
static void s_webm(void) { evaluate("file('video', 'media/clip.webm')"); }
static void s_webm_ready(void) { wait_script("ready()", "true", 10000, "the WebM loads"); }
static void s_webm_list(void) {
    check(strstr(list, "\"video\":\"vp9\"") && strstr(list, "\"audio\":\"opus\""), "the WebM's codecs", "%s", list);
    start_capture("file-webm", NULL);
    evaluate("play(2)");
}
static void s_webm_play(void) { wait_script("state.ended", "true", 30000, "the WebM plays to its end"); }
static void s_webm_check(void) {
    check_clip("src= WebM", ".webm", AV_CODEC_ID_VP9, AV_CODEC_ID_OPUS);
    next(0);
}

/* MSE: three segments before the capture, the rest during it. */
static void s_mse(void) { evaluate("mse(3)"); }
static void s_mse_ready(void) { wait_script("ready()", "true", 10000, "the DASH stream's first segments play"); }
static void s_mse_list(void) {
    check(strstr(list, "\"kind\":\"mse\"") && strstr(list, "\"video\":\"h264\"") && strstr(list, "blob:"),
          "listed as Media Source Extensions", "%s", list);
    start_capture("mse.mp4", NULL);
    evaluate("more()");
}
static void s_mse_play(void) {
    evaluate("play(2)");
}
static void s_mse_wait(void) { wait_script("state.ended", "true", 30000, "the stream plays to its end"); }
static void s_mse_check(void) {
    check_clip("MSE", ".mp4", AV_CODEC_ID_H264, AV_CODEC_ID_AAC);
    next(0);
}

/* MSE again: started at 3 s, with what is held behind the playhead. */
static void s_buffered(void) { evaluate("mse(3).then(more)"); }
static void s_buffered_ready(void) { wait_script("ready()", "true", 10000, "the stream loads again"); }
static void s_buffered_play(void) { evaluate("play(1)"); }
static void s_buffered_wait(void) { wait_script("time() > 3", "true", 10000, "it plays past 3 s"); }
static void s_buffered_start(void) {
    start_capture("buffered.mp4", "{\"includeBuffered\": true}");
    evaluate("play(4)");
}
static void s_buffered_end(void) { wait_script("state.ended", "true", 30000, "and to its end"); }
static void s_buffered_check(void) {
    check_clip("MSE with what was buffered", ".mp4", AV_CODEC_ID_H264, AV_CODEC_ID_AAC);
    const char *files_at = strstr(done_json, "\"files\":[");
    check(files_at && number(files_at, "start", 9) < 0.2, "from the start of the stream", "%s", done_json);
    next(0);
}

/* A seek forward during a capture. */
static void s_seek(void) { evaluate("file('video', 'media/clip.mp4')"); }
static void s_seek_ready(void) { wait_script("ready()", "true", 10000, "the MP4 loads again"); }
static void s_seek_start(void) {
    start_capture("seek.mp4", NULL);
    evaluate("play(1)");
}
static void s_seek_wait(void) { wait_script("time() >= 0.8", "true", 10000, "it plays"); }
static void s_seek_jump(void) { evaluate("seek(6.5)"); }
static void s_seek_end(void) { wait_script("state.ended", "true", 30000, "it plays from 6.5 s to its end"); }
static void s_seek_check(void) {
    char error[256] = "", path[600] = "";
    string(done_json, "error", error, sizeof(error));
    check(!*error, "a capture across a seek", "no error: %s", error);
    check(pairs(done_json, "ranges") >= 2, "has the two stretches played", "%s", done_json);
    const char *files_at = strstr(done_json, "\"files\":[");
    check(files_at && pairs(files_at, "gaps") >= 1, "and reports the gap closed between them", NULL);
    probe_t p = first_file(path, sizeof(path));
    check(p.ok && p.streams == 2 && p.errors == 0, "the file plays", "%d streams, %d errors", p.streams, p.errors);
    check(p.duration > 2.0 && p.duration < 7.0, "it is shorter than the clip, without the gap", "%.2f s, %d frames",
          p.duration, p.video_frames);
    next(0);
}

/* <audio> alone. */
static void s_audio(void) { evaluate("file('audio', 'media/tone.webm')"); }
static void s_audio_ready(void) { wait_script("ready()", "true", 10000, "the audio loads"); }
static void s_audio_list(void) {
    check(strstr(list, "\"tag\":\"audio\"") && strstr(list, "\"hasVideo\":false") &&
              strstr(list, "\"audio\":\"opus\""),
          "audio alone is listed", "%s", list);
    start_capture("tone", NULL);
    evaluate("play(2)");
}
static void s_audio_play(void) { wait_script("state.ended", "true", 30000, "it plays to its end"); }
static void s_audio_check(void) {
    char path[600] = "";
    probe_t p = first_file(path, sizeof(path));
    check(ends_with(path, "/tone.webm"), "audio alone is written as WebM", "%s", path);
    check(p.ok && p.streams == 1 && p.audio == AV_CODEC_ID_OPUS && !p.video, "with its one Opus track", "%d streams",
          p.streams);
    check(p.duration > 7.4 && p.duration < 8.6 && p.errors == 0, "all of it", "%.2f s, %d frames, %d errors",
          p.duration, p.audio_frames, p.errors);
    next(0);
}

/* The element loads other media during a capture. */
static void s_change(void) { evaluate("file('video', 'media/clip.mp4')"); }
static void s_change_ready(void) { wait_script("ready()", "true", 10000, "the MP4 loads once more"); }
static void s_change_start(void) {
    start_capture("changed.mp4", NULL);
    evaluate("play(1)");
}
static void s_change_wait(void) { wait_script("time() > 1", "true", 10000, "it plays for a second"); }
static void s_change_load(void) { evaluate("media.src = 'media/clip.webm'; true"); }
static void s_change_done(void) { wait_done(15000); }
static void s_change_check(void) {
    char reason[128] = "", path[600] = "";
    string(done_json, "reason", reason, sizeof(reason));
    check(strcmp(reason, "the element loaded other media") == 0 || strcmp(reason, "the player stopped") == 0,
          "a capture ends when its element loads other media", "%s", reason);
    probe_t p = first_file(path, sizeof(path));
    check(p.ok && p.video == AV_CODEC_ID_H264 && p.duration > 0.5, "with what it had", "%.2f s", p.duration);
    next(0);
}

/* Encrypted media. */
static void s_cenc(void) { evaluate("encrypted()"); }
static void s_cenc_wait(void) { wait_script("state.appended >= 1", "true", 10000, "the encrypted video is appended"); }
static void s_cenc_list(void) {
    check(strstr(list, "\"encrypted\":true") && strstr(list, "\"capturable\":false"), "listed as encrypted", "%s",
          list);
    start_capture("cenc.mp4", NULL);
    wait_done(10000);
}
static void s_cenc_check(void) {
    char error[256] = "";
    string(done_json, "error", error, sizeof(error));
    check(strstr(error, "encrypted") != NULL, "a capture of it is refused", "%s", done_json);
    check(pairs(done_json, "files") == 0 || strstr(done_json, "\"files\":[]"), "and writes nothing", NULL);
    next(0);
}

static void s_bad_ids(void) {
    char *error = NULL;
    zebkit_media_capture *c = zebkit_media_capture_start(view, "not an id", "/tmp/x.mp4", NULL, &error);
    check(!c && error, "an id that is not one is refused", "%s", error ? error : "");
    zebkit_free(error);
    error = NULL;
    c = zebkit_media_capture_start(view, "99999.1.1", "/tmp/x.mp4", NULL, &error);
    check(!c && error, "and one of no frame", "%s", error ? error : "");
    zebkit_free(error);
    error = NULL;
    c = zebkit_media_capture_start(view, media_id, "relative.mp4", NULL, &error);
    check(!c && error, "and a relative path", "%s", error ? error : "");
    zebkit_free(error);
    error = NULL;
    c = zebkit_media_capture_start(view, media_id, "/tmp/x.mp4", "{\"format\": \"avi\"}", &error);
    check(!c && error, "and a format there is no muxer for", "%s", error ? error : "");
    zebkit_free(error);
    next(0);
}

static void ready(void *data) {
    (void)data;
    const char *env = getenv("ZEBKIT_TEST_BASE");
    snprintf(base, sizeof(base), "%s", env ? env : "http://127.0.0.1:8796");
    env = getenv("ZEBKIT_TEST_FILES");
    snprintf(files, sizeof(files), "%s", env ? env : "/tmp");
    view = zebkit_view_new(NULL, 640, 480);
    zebkit_view_on_load(view, on_load, NULL);

    step_func all[] = {
        s_load,
        s_mp4, s_mp4_ready, list_media, s_mp4_list, s_mp4_at, s_mp4_at_check, s_mp4_at_none, s_mp4_at_none_check,
        s_mp4_start, s_mp4_play, stop_and_wait, s_mp4_check,
        s_webm, s_webm_ready, list_media, s_webm_list, s_webm_play, stop_and_wait, s_webm_check,
        s_mse, s_mse_ready, list_media, s_mse_list, s_mse_play, s_mse_wait, stop_and_wait, s_mse_check,
        s_buffered, s_buffered_ready, list_media, s_buffered_play, s_buffered_wait, s_buffered_start,
        s_buffered_end, stop_and_wait, s_buffered_check,
        s_seek, s_seek_ready, list_media, s_seek_start, s_seek_wait, s_seek_jump, s_seek_end, stop_and_wait,
        s_seek_check,
        s_audio, s_audio_ready, list_media, s_audio_list, s_audio_play, stop_and_wait, s_audio_check,
        s_change, s_change_ready, list_media, s_change_start, s_change_wait, s_change_load, s_change_done,
        s_change_check,
        s_cenc, s_cenc_wait, list_media, s_cenc_list, s_cenc_check,
        s_bad_ids,
    };
    step_count = (int)(sizeof(all) / sizeof(all[0]));
    memcpy(steps, all, sizeof(all));
    next(0);
}

int main(int argc, char **argv) {
    int code = zebkit_main(argc, argv, ready, NULL);
    if (code)
        return code;
    return failures || checks < 70 ? 1 : 0;
}
