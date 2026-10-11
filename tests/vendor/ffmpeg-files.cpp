// The parts of the engine's FFmpeg that are there for applications: reading
// a file (libavformat), decoding its video, scaling a frame (libswscale),
// and encoding and writing a file. Run by scripts/vendor.sh.
//
//   ffmpeg-files INPUT.mp4 OUTPUT.flac

#include <zebkit/vendor/ffmpeg.h>

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace ff = zebkit::vendor::ffmpeg;

static int check(int result, const char* what)
{
    if (result >= 0)
        return result;
    char text[AV_ERROR_MAX_STRING_SIZE] = "";
    ff::av_strerror(result, text, sizeof(text));
    throw std::runtime_error(std::string(what) + ": " + text);
}

// Opens a file, decodes the first frames of its video and scales one down.
static void readVideo(const char* path)
{
    ff::AVFormatContext* input = nullptr;
    check(ff::avformat_open_input(&input, path, nullptr, nullptr), "cannot open the input");
    check(ff::avformat_find_stream_info(input, nullptr), "cannot read the streams");
    const ff::AVCodec* codec = nullptr;
    int video = check(ff::av_find_best_stream(input, ff::AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0), "no video stream");
    int audio = ff::av_find_best_stream(input, ff::AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    ff::AVCodecContext* decoder = ff::avcodec_alloc_context3(codec);
    check(ff::avcodec_parameters_to_context(decoder, input->streams[video]->codecpar), "cannot set up the decoder");
    check(ff::avcodec_open2(decoder, codec, nullptr), "cannot open the decoder");

    ff::AVPacket* packet = ff::av_packet_alloc();
    ff::AVFrame* frame = ff::av_frame_alloc();
    int frames = 0, scaledSum = 0;
    const int smallWidth = 64, smallHeight = 36;
    std::vector<uint8_t> small(smallWidth * smallHeight * 3);
    while (frames < 5 && ff::av_read_frame(input, packet) >= 0) {
        if (packet->stream_index == video) {
            check(ff::avcodec_send_packet(decoder, packet), "cannot decode");
            while (ff::avcodec_receive_frame(decoder, frame) >= 0) {
                if (!frames) {
                    ff::SwsContext* scaler = ff::sws_getContext(frame->width, frame->height, static_cast<ff::AVPixelFormat>(frame->format),
                        smallWidth, smallHeight, ff::AV_PIX_FMT_RGB24, ff::SWS_BILINEAR, nullptr, nullptr, nullptr);
                    if (!scaler)
                        throw std::runtime_error("cannot set up scaling");
                    uint8_t* planes[4] = { small.data(), nullptr, nullptr, nullptr };
                    int strides[4] = { smallWidth * 3, 0, 0, 0 };
                    check(ff::sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, planes, strides) == smallHeight ? 0 : -1, "scaling stopped short");
                    ff::sws_freeContext(scaler);
                    for (uint8_t value : small)
                        scaledSum += value;
                }
                frames++;
            }
        }
        ff::av_packet_unref(packet);
    }
    std::printf("read: format=%s streams=%u video=%s %dx%d audio=%s frames=%d scaled=%dx%d sum=%d\n", input->iformat->name, input->nb_streams,
        codec->name, decoder->width, decoder->height,
        audio >= 0 ? ff::avcodec_get_name(input->streams[audio]->codecpar->codec_id) : "none", frames, smallWidth, smallHeight, scaledSum);

    ff::av_frame_free(&frame);
    ff::av_packet_free(&packet);
    ff::avcodec_free_context(&decoder);
    ff::avformat_close_input(&input);
}

// Encodes one second of a 440 Hz tone and writes it to a file.
static void writeAudio(const char* path)
{
    const int rate = 44100;
    ff::AVFormatContext* output = nullptr;
    check(ff::avformat_alloc_output_context2(&output, nullptr, nullptr, path), "no muxer for the output");
    const ff::AVCodec* codec = ff::avcodec_find_encoder(output->oformat->audio_codec);
    if (!codec)
        throw std::runtime_error("no encoder for the output");
    ff::AVCodecContext* encoder = ff::avcodec_alloc_context3(codec);
    encoder->sample_rate = rate;
    encoder->sample_fmt = ff::AV_SAMPLE_FMT_S16;
    ff::av_channel_layout_default(&encoder->ch_layout, 1);
    encoder->time_base = ff::AVRational { 1, rate };
    check(ff::avcodec_open2(encoder, codec, nullptr), "cannot open the encoder");

    ff::AVStream* stream = ff::avformat_new_stream(output, nullptr);
    check(ff::avcodec_parameters_from_context(stream->codecpar, encoder), "cannot describe the stream");
    stream->time_base = encoder->time_base;
    check(ff::avio_open(&output->pb, path, AVIO_FLAG_WRITE), "cannot create the output");
    check(ff::avformat_write_header(output, nullptr), "cannot write the header");

    ff::AVFrame* frame = ff::av_frame_alloc();
    ff::AVPacket* packet = ff::av_packet_alloc();
    int frameSize = encoder->frame_size > 0 ? encoder->frame_size : 4096;
    int packets = 0;
    auto drain = [&] {
        while (ff::avcodec_receive_packet(encoder, packet) >= 0) {
            ff::av_packet_rescale_ts(packet, encoder->time_base, stream->time_base);
            packet->stream_index = stream->index;
            check(ff::av_interleaved_write_frame(output, packet), "cannot write a packet");
            packets++;
        }
    };
    for (int position = 0; position < rate; position += frameSize) {
        frame->nb_samples = std::min(frameSize, rate - position);
        frame->format = encoder->sample_fmt;
        frame->sample_rate = rate;
        check(ff::av_channel_layout_copy(&frame->ch_layout, &encoder->ch_layout), "cannot set the layout");
        check(ff::av_frame_get_buffer(frame, 0), "cannot allocate a frame");
        int16_t* samples = reinterpret_cast<int16_t*>(frame->data[0]);
        for (int i = 0; i < frame->nb_samples; i++)
            samples[i] = static_cast<int16_t>(8000 * std::sin(2 * M_PI * 440 * (position + i) / rate));
        frame->pts = position;
        check(ff::avcodec_send_frame(encoder, frame), "cannot encode");
        ff::av_frame_unref(frame);
        drain();
    }
    check(ff::avcodec_send_frame(encoder, nullptr), "cannot finish encoding");
    drain();
    check(ff::av_write_trailer(output), "cannot finish the file");
    std::printf("write: format=%s encoder=%s packets=%d\n", output->oformat->name, codec->name, packets);

    ff::av_packet_free(&packet);
    ff::av_frame_free(&frame);
    ff::avcodec_free_context(&encoder);
    ff::avio_closep(&output->pb);
    ff::avformat_free_context(output);
}

int main(int argc, char** argv)
try {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s INPUT.mp4 OUTPUT.flac\n", argv[0]);
        return 2;
    }
    readVideo(argv[1]);
    writeAudio(argv[2]);
    std::printf("encoders: aac=%d mjpeg=%d h264=%d\n", ff::avcodec_find_encoder(ff::AV_CODEC_ID_AAC) != nullptr,
        ff::avcodec_find_encoder(ff::AV_CODEC_ID_MJPEG) != nullptr, ff::avcodec_find_encoder(ff::AV_CODEC_ID_H264) != nullptr);
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "ffmpeg-files: %s\n", error.what());
    return 1;
}
