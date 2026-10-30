/* A media file read back with the engine's own FFmpeg, for the tests that
 * are not in C (tests/engine/cdp_page.py): its streams, codecs, size,
 * duration and every frame decoded, as JSON on one line.
 *
 *   media_probe <file>
 *
 *   {"ok": true, "streams": 2, "video": "h264", "audio": "aac",
 *    "width": 320, "height": 180, "duration": 8.0,
 *    "videoFrames": 192, "audioFrames": 376, "errors": 0}
 *
 * Exit 0 when the file opens. */

#include <stdio.h>

#include <zebkit/vendor/ffmpeg.h>

#include "media_probe.h"

static void json_string(const char *value) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*p == '"' || *p == '\\') { putchar('\\'); putchar(*p); }
        else if (*p < 32) printf("\\u%04x", *p);
        else putchar(*p);
    }
    putchar('"');
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <file>\n", argv[0]);
        return 2;
    }
    probe_t p = probe(argv[1]);
    printf("{\"ok\": %s, \"streams\": %d, \"video\": \"%s\", \"audio\": \"%s\", \"width\": %d, \"height\": %d, "
           "\"duration\": %.3f, \"videoFrames\": %d, \"audioFrames\": %d, \"errors\": %d, \"minWidth\": %d, \"maxWidth\": %d, \"minHeight\": %d, \"maxHeight\": %d, \"videoTag\": %u",
           p.ok ? "true" : "false", p.streams, p.video ? avcodec_get_name(p.video) : "",
           p.audio ? avcodec_get_name(p.audio) : "", p.width, p.height, p.duration, p.video_frames, p.audio_frames,
           p.errors, p.min_width, p.max_width, p.min_height, p.max_height, p.video_tag);
    fputs(",\"creationTime\":", stdout); json_string(p.creation_time);
    fputs(",\"wallClockMetadata\":", stdout); json_string(p.wall_clock);
    puts("}");
    return p.ok ? 0 : 1;
}
