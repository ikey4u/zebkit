/* A media file read back with the engine's own FFmpeg: its streams,
 * codecs, size, duration, and every frame decoded. For tests/engine/media.c
 * and tests/engine/media_probe.c. */

#ifndef ZEBKIT_TESTS_MEDIA_PROBE_H
#define ZEBKIT_TESTS_MEDIA_PROBE_H

#include <errno.h>
#include <stdio.h>
#include <zebkit/vendor/ffmpeg.h>

typedef struct {
    int ok;
    int streams;
    int video, audio; /* AVCodecID, or 0 */
    int width, height;
    int min_width, max_width, min_height, max_height;
    unsigned video_tag;
    char creation_time[64];
    char wall_clock[16384];
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
    const AVDictionaryEntry *created = av_dict_get(context->metadata, "creation_time", NULL, 0);
    const AVDictionaryEntry *clock = av_dict_get(context->metadata, "zebkit.wall_clock", NULL, 0);
    if (created && created->value) snprintf(p.creation_time, sizeof(p.creation_time), "%s", created->value);
    if (clock && clock->value) snprintf(p.wall_clock, sizeof(p.wall_clock), "%s", clock->value);
    p.streams = (int)context->nb_streams;
    p.duration = context->duration > 0 ? context->duration / 1e6 : 0;
    AVCodecContext *decoders[8] = {0};
    for (unsigned i = 0; i < context->nb_streams && i < 8; ++i) {
        AVCodecParameters *par = context->streams[i]->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
            p.video = par->codec_id, p.width = par->width, p.height = par->height;
            p.video_tag = par->codec_tag;
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
                    if (decoders[i]->codec_type == AVMEDIA_TYPE_VIDEO) {
                        ++p.video_frames;
                        if (!p.min_width || frame->width < p.min_width) p.min_width = frame->width;
                        if (frame->width > p.max_width) p.max_width = frame->width;
                        if (!p.min_height || frame->height < p.min_height) p.min_height = frame->height;
                        if (frame->height > p.max_height) p.max_height = frame->height;
                    } else
                        ++p.audio_frames;
                    av_frame_unref(frame);
                }
                if (got != AVERROR(EAGAIN) && got != AVERROR_EOF) ++p.errors;
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

#endif
