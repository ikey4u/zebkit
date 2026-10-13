// ffmpeg: decode an MP3 file to a WAV file with the FFmpeg inside zebkit.
//
//   ffmpeg INPUT.mp3 OUTPUT.wav
//
// The application has no FFmpeg of its own. It includes one header, uses the
// library's ordinary API in the namespace zebkit::vendor::ffmpeg, and links
// against the engine and nothing else (docs/VENDOR.md). On Linux:
//
//   c++ -std=c++17 ffmpeg.cpp -o ffmpeg -I<package>/include -L<package> -l:libzebkit.so.1
//
// On macOS, where the engine is Zebkit.framework:
//
//   c++ -std=c++17 ffmpeg.cpp -o ffmpeg -I<package>/include -F<package> -framework Zebkit
//
// This one works on the codec level: an MP3 file is a bare audio stream,
// which the codec's parser splits into frames. The engine's FFmpeg can also
// open container files (MP4, WebM, Ogg, WAV) with libavformat, scale video
// and encode; tests/vendor/ffmpeg-files.cpp does all three.

#include <zebkit/vendor/ffmpeg.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace ff = zebkit::vendor::ffmpeg;

// Throws with FFmpeg's description of an error code.
static int check(int result, const char* what)
{
    if (result >= 0)
        return result;
    char text[AV_ERROR_MAX_STRING_SIZE] = "";
    ff::av_strerror(result, text, sizeof(text));
    throw std::runtime_error(std::string(what) + ": " + text);
}

// 16-bit PCM in a WAV file: a 44-byte header, then the samples.
static void writeWAV(const char* path, const std::vector<int16_t>& samples, int rate, int channels)
{
    auto put = [](std::ofstream& out, uint32_t value, int bytes) {
        for (int i = 0; i < bytes; i++)
            out.put(static_cast<char>(value >> (8 * i)));
    };
    uint32_t dataBytes = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
    std::ofstream out(path, std::ios::binary);
    out << "RIFF"; put(out, 36 + dataBytes, 4);
    out << "WAVEfmt "; put(out, 16, 4); put(out, 1, 2); put(out, channels, 2);
    put(out, rate, 4); put(out, rate * channels * 2, 4); put(out, channels * 2, 2); put(out, 16, 2);
    out << "data"; put(out, dataBytes, 4);
    out.write(reinterpret_cast<const char*>(samples.data()), dataBytes);
    if (!out)
        throw std::runtime_error(std::string("cannot write ") + path);
}

int main(int argc, char** argv)
try {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s INPUT.mp3 OUTPUT.wav\n", argv[0]);
        return 2;
    }
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<uint8_t> input((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (input.empty())
        throw std::runtime_error(std::string("cannot read ") + argv[1]);
    // Decoders may read a little past the end of what they are given.
    size_t remaining = input.size();
    input.resize(remaining + AV_INPUT_BUFFER_PADDING_SIZE);
    const uint8_t* cursor = input.data();

    // An ID3v2 tag in front of the audio is not audio.
    if (remaining > 10 && cursor[0] == 'I' && cursor[1] == 'D' && cursor[2] == '3') {
        size_t tag = 10 + ((cursor[6] & 0x7f) << 21 | (cursor[7] & 0x7f) << 14 | (cursor[8] & 0x7f) << 7 | (cursor[9] & 0x7f));
        if (tag < remaining) {
            cursor += tag;
            remaining -= tag;
        }
    }

    const ff::AVCodec* codec = ff::avcodec_find_decoder(ff::AV_CODEC_ID_MP3);
    if (!codec)
        throw std::runtime_error("this zebkit has no MP3 decoder");
    ff::AVCodecParserContext* parser = ff::av_parser_init(codec->id);
    ff::AVCodecContext* decoder = ff::avcodec_alloc_context3(codec);
    ff::AVPacket* packet = ff::av_packet_alloc();
    ff::AVFrame* frame = ff::av_frame_alloc();
    check(ff::avcodec_open2(decoder, codec, nullptr), "cannot open the decoder");

    ff::SwrContext* converter = nullptr;
    std::vector<int16_t> output;
    int rate = 0, channels = 0;

    // Takes every frame the decoder has ready and appends it, as interleaved
    // 16-bit samples, to the output.
    auto receive = [&] {
        for (;;) {
            int result = ff::avcodec_receive_frame(decoder, frame);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
                return;
            check(result, "cannot decode");
            if (!converter) {
                rate = frame->sample_rate;
                channels = frame->ch_layout.nb_channels;
                check(ff::swr_alloc_set_opts2(&converter, &frame->ch_layout, ff::AV_SAMPLE_FMT_S16, rate,
                    &frame->ch_layout, static_cast<ff::AVSampleFormat>(frame->format), rate, 0, nullptr), "cannot set up conversion");
                check(ff::swr_init(converter), "cannot start conversion");
            }
            size_t start = output.size();
            output.resize(start + static_cast<size_t>(frame->nb_samples) * channels);
            uint8_t* destination = reinterpret_cast<uint8_t*>(output.data() + start);
            int converted = check(ff::swr_convert(converter, &destination, frame->nb_samples,
                const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples), "cannot convert");
            output.resize(start + static_cast<size_t>(converted) * channels);
        }
    };

    while (remaining > 0) {
        // The parser finds where one frame ends and the next begins.
        int used = check(ff::av_parser_parse2(parser, decoder, &packet->data, &packet->size, cursor, static_cast<int>(remaining),
            AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0), "cannot parse");
        cursor += used;
        remaining -= used;
        if (!packet->size)
            continue;
        int result = ff::avcodec_send_packet(decoder, packet);
        // Bytes that are not a frame (padding, a trailing tag) are skipped.
        if (result == AVERROR_INVALIDDATA)
            continue;
        check(result, "cannot decode");
        receive();
    }
    ff::avcodec_send_packet(decoder, nullptr);
    receive();
    if (output.empty())
        throw std::runtime_error("no audio was found in the file");

    writeWAV(argv[2], output, rate, channels);
    std::printf("FFmpeg %s inside zebkit, decoder %s\n", ff::av_version_info(), codec->long_name);
    std::printf("%s: %d Hz, %d channel%s, %.2f seconds -> %s\n", argv[1], rate, channels, channels == 1 ? "" : "s",
        static_cast<double>(output.size() / channels) / rate, argv[2]);

    ff::swr_free(&converter);
    ff::av_frame_free(&frame);
    ff::av_packet_free(&packet);
    ff::avcodec_free_context(&decoder);
    ff::av_parser_close(parser);
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "ffmpeg example: %s\n", error.what());
    return 1;
}
