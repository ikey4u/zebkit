/* The part of a media file between two times, copied packet by packet with the
 * FFmpeg inside the engine: what zebkit_ffmpeg() has no switch for. Starts at the
 * key frame at or before @start of each stream, as `ffmpeg -ss -to -c copy` does.
 * @input and @output have been checked against the server's bounds; @output is a
 * new file of the caller's. Returns 0, or -1 with the reason in @error. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <zebkit/vendor/ffmpeg.h>

static int failed(char *error, int size, const char *what, int code) {
    char reason[128] = "";
    if (code < 0)
        av_strerror(code, reason, sizeof(reason));
    snprintf(error, (size_t)size, "%s%s%s", what, reason[0] ? ": " : "", reason);
    return -1;
}

int zebkit_mcp_media_cut(const char *input, const char *output, double start, double end, char *error, int error_size) {
    AVFormatContext *in = NULL, *out = NULL;
    AVPacket *packet = NULL;
    int64_t *first = NULL;
    int *map = NULL;
    int result = -1, code, opened = 0;

    if ((code = avformat_open_input(&in, input, NULL, NULL)) < 0)
        return failed(error, error_size, "cannot open the input", code);
    if ((code = avformat_find_stream_info(in, NULL)) < 0) {
        failed(error, error_size, "cannot read the input's streams", code);
        goto done;
    }
    if ((code = avformat_alloc_output_context2(&out, NULL, NULL, output)) < 0 || !out) {
        failed(error, error_size, "the output's name is not a container this build writes", code);
        goto done;
    }
    map = calloc(in->nb_streams, sizeof(*map));
    first = calloc(in->nb_streams, sizeof(*first));
    if (!map || !first) {
        failed(error, error_size, "out of memory", 0);
        goto done;
    }
    int copied = 0;
    for (unsigned i = 0; i < in->nb_streams; ++i) {
        enum AVMediaType type = in->streams[i]->codecpar->codec_type;
        first[i] = AV_NOPTS_VALUE;
        map[i] = -1;
        if (type != AVMEDIA_TYPE_AUDIO && type != AVMEDIA_TYPE_VIDEO)
            continue;
        AVStream *stream = avformat_new_stream(out, NULL);
        if (!stream || avcodec_parameters_copy(stream->codecpar, in->streams[i]->codecpar) < 0) {
            failed(error, error_size, "cannot copy a stream's parameters", 0);
            goto done;
        }
        stream->codecpar->codec_tag = 0;
        stream->time_base = in->streams[i]->time_base;
        map[i] = copied++;
    }
    if (!copied) {
        failed(error, error_size, "the input has no audio or video", 0);
        goto done;
    }
    if ((code = avio_open(&out->pb, output, AVIO_FLAG_WRITE)) < 0) {
        failed(error, error_size, "cannot write the output", code);
        goto done;
    }
    opened = 1;
    if ((code = avformat_write_header(out, NULL)) < 0) {
        failed(error, error_size, "the codecs do not go in this container", code);
        goto done;
    }
    if (start > 0 && (code = av_seek_frame(in, -1, (int64_t)(start * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD)) < 0) {
        failed(error, error_size, "cannot seek in the input", code);
        goto done;
    }
    packet = av_packet_alloc();
    if (!packet) {
        failed(error, error_size, "out of memory", 0);
        goto done;
    }
    int64_t written = 0;
    while ((code = av_read_frame(in, packet)) >= 0) {
        int index = packet->stream_index;
        if (index < 0 || (unsigned)index >= in->nb_streams || map[index] < 0) {
            av_packet_unref(packet);
            continue;
        }
        AVStream *from = in->streams[index];
        int64_t when = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
        if (when != AV_NOPTS_VALUE && when * av_q2d(from->time_base) >= end) {
            av_packet_unref(packet);
            continue; /* Other streams may still have packets before the end. */
        }
        if (first[index] == AV_NOPTS_VALUE) {
            if (!(packet->flags & AV_PKT_FLAG_KEY) && from->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                av_packet_unref(packet);
                continue;
            }
            first[index] = packet->dts != AV_NOPTS_VALUE ? packet->dts : when;
        }
        int64_t base = first[index] == AV_NOPTS_VALUE ? 0 : first[index];
        if (packet->pts != AV_NOPTS_VALUE)
            packet->pts -= base;
        if (packet->dts != AV_NOPTS_VALUE)
            packet->dts -= base;
        AVStream *to = out->streams[map[index]];
        av_packet_rescale_ts(packet, from->time_base, to->time_base);
        packet->stream_index = map[index];
        packet->pos = -1;
        if ((code = av_interleaved_write_frame(out, packet)) < 0) {
            failed(error, error_size, "cannot write a packet", code);
            goto done;
        }
        written++;
    }
    if (code != AVERROR_EOF) {
        failed(error, error_size, "cannot read the input", code);
        goto done;
    }
    if (!written) {
        failed(error, error_size, "nothing in the input lies between those times", 0);
        goto done;
    }
    if ((code = av_write_trailer(out)) < 0) {
        failed(error, error_size, "cannot finish the output", code);
        goto done;
    }
    result = 0;
done:
    av_packet_free(&packet);
    free(map);
    free(first);
    if (out) {
        if (opened)
            avio_closep(&out->pb);
        avformat_free_context(out);
    }
    avformat_close_input(&in);
    return result;
}
