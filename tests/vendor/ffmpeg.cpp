// Uses the FFmpeg inside zebkit, through zebkit::vendor::ffmpeg, the way an
// application would: decodes an MP3 file to 16-bit samples and says what
// came out. Linked against libzebkit only. Run by scripts/vendor.sh.
//
//   ffmpeg FILE.mp3

#include <zebkit/vendor/ffmpeg.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

namespace ff = zebkit::vendor::ffmpeg;

static int fail(const char* what, int code = 0)
{
    char text[AV_ERROR_MAX_STRING_SIZE] = "";
    if (code)
        ff::av_strerror(code, text, sizeof(text));
    std::fprintf(stderr, "ffmpeg test: %s%s%s\n", what, code ? ": " : "", text);
    return 1;
}

int main(int argc, char** argv)
{
    if (argc != 2)
        return fail("usage: ffmpeg FILE.mp3");
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (data.empty())
        return fail("cannot read the file");
    // Decoders may read a little past the end of what they are given.
    size_t size = data.size();
    data.resize(size + AV_INPUT_BUFFER_PADDING_SIZE);

    const ff::AVCodec* codec = ff::avcodec_find_decoder(ff::AV_CODEC_ID_MP3);
    if (!codec)
        return fail("no MP3 decoder");
    ff::AVCodecParserContext* parser = ff::av_parser_init(codec->id);
    ff::AVCodecContext* context = ff::avcodec_alloc_context3(codec);
    ff::AVPacket* packet = ff::av_packet_alloc();
    ff::AVFrame* frame = ff::av_frame_alloc();
    if (!parser || !context || !packet || !frame)
        return fail("out of memory");
    if (int error = ff::avcodec_open2(context, codec, nullptr); error < 0)
        return fail("cannot open the decoder", error);

    ff::SwrContext* resampler = nullptr;
    std::vector<int16_t> converted;
    long long samples = 0, convertedSamples = 0, frames = 0;
    int rate = 0, channels = 0;

    auto drain = [&]() -> int {
        for (;;) {
            int error = ff::avcodec_receive_frame(context, frame);
            if (error == AVERROR(EAGAIN) || error == AVERROR_EOF)
                return 0;
            if (error < 0)
                return error;
            if (!resampler) {
                // Whatever the decoder produces, to interleaved 16-bit.
                error = ff::swr_alloc_set_opts2(&resampler, &frame->ch_layout, ff::AV_SAMPLE_FMT_S16, frame->sample_rate,
                    &frame->ch_layout, static_cast<ff::AVSampleFormat>(frame->format), frame->sample_rate, 0, nullptr);
                if (error < 0 || (error = ff::swr_init(resampler)) < 0)
                    return error;
                rate = frame->sample_rate;
                channels = frame->ch_layout.nb_channels;
            }
            converted.resize(static_cast<size_t>(frame->nb_samples) * channels);
            uint8_t* output = reinterpret_cast<uint8_t*>(converted.data());
            int count = ff::swr_convert(resampler, &output, frame->nb_samples, const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
            if (count < 0)
                return count;
            samples += frame->nb_samples;
            convertedSamples += count;
            frames++;
        }
    };

    const uint8_t* cursor = data.data();
    // A tag in front of the audio is not the decoder's business.
    if (size > 10 && cursor[0] == 'I' && cursor[1] == 'D' && cursor[2] == '3') {
        size_t tag = 10 + ((cursor[6] & 0x7f) << 21 | (cursor[7] & 0x7f) << 14 | (cursor[8] & 0x7f) << 7 | (cursor[9] & 0x7f));
        if (tag < size) {
            cursor += tag;
            size -= tag;
        }
    }
    while (size > 0) {
        int used = ff::av_parser_parse2(parser, context, &packet->data, &packet->size, cursor, static_cast<int>(size), AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (used < 0)
            return fail("cannot parse the stream", used);
        cursor += used;
        size -= used;
        if (!packet->size)
            continue;
        int error = ff::avcodec_send_packet(context, packet);
        // Bytes that are not a frame (padding, a trailing tag) are skipped.
        if (error == AVERROR_INVALIDDATA)
            continue;
        if (error < 0 || (error = drain()) < 0)
            return fail("cannot decode", error);
    }
    ff::avcodec_send_packet(context, nullptr);
    if (int error = drain(); error < 0)
        return fail("cannot decode the end of the stream", error);

    // An inline function and a macro of the library's own, for good measure.
    double seconds = rate ? samples * ff::av_q2d(ff::AVRational { 1, rate }) : 0;
    std::printf("version=%s avcodec=%u.%u decoder=%s frames=%lld samples=%lld converted=%lld rate=%d channels=%d seconds=%.2f h264=%d aac_encoder=%d\n",
        ff::av_version_info(), AV_VERSION_MAJOR(ff::avcodec_version()), AV_VERSION_MINOR(ff::avcodec_version()), codec->name,
        frames, samples, convertedSamples, rate, channels, seconds,
        ff::avcodec_find_decoder(ff::AV_CODEC_ID_H264) != nullptr, ff::avcodec_find_encoder(ff::AV_CODEC_ID_AAC) != nullptr);

    ff::swr_free(&resampler);
    ff::av_frame_free(&frame);
    ff::av_packet_free(&packet);
    ff::avcodec_free_context(&context);
    ff::av_parser_close(parser);
    return 0;
}
