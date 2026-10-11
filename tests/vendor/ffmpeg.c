/* The same library from C, where its API has its own names. Run by
 * scripts/vendor.sh. */

#include <zebkit/vendor/ffmpeg.h>

int main(void)
{
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_VP9);
    AVDictionary* options = NULL;
    av_dict_set(&options, "threads", "1", 0);
    printf("c: version=%s vp9=%s options=%d log2(1024)=%d\n", av_version_info(), codec ? codec->name : "missing",
        av_dict_count(options), av_log2(1024));
    av_dict_free(&options);
    return codec ? 0 : 1;
}
