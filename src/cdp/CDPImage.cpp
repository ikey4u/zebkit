/*
 * Screenshots in the formats CDP offers. WebKit's inspector produces PNG
 * only; JPEG and WebP are made here by re-encoding it with the Skia that
 * the engine already contains.
 */

#include "config.h"
#include "CDPInternal.h"

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/codec/SkCodec.h>
#include <skia/codec/SkPngDecoder.h>
#include <skia/core/SkBitmap.h>
#include <skia/core/SkData.h>
#include <skia/core/SkStream.h>
#include <skia/encode/SkJpegEncoder.h>
#include <skia/encode/SkWebpEncoder.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END
#include <wtf/text/Base64.h>

namespace Zebkit::CDP {

String transcodeScreenshot(const String& base64PNG, const String& format, int quality)
{
    auto png = base64Decode(base64PNG);
    if (!png)
        return { };
    auto codec = SkPngDecoder::Decode(SkData::MakeWithCopy(png->span().data(), png->size()), nullptr);
    if (!codec)
        return { };

    auto info = codec->getInfo().makeColorType(kRGBA_8888_SkColorType).makeAlphaType(kPremul_SkAlphaType);
    SkBitmap bitmap;
    if (!bitmap.tryAllocPixels(info) || codec->getPixels(info, bitmap.getPixels(), bitmap.rowBytes()) != SkCodec::kSuccess)
        return { };

    SkDynamicMemoryWStream stream;
    if (format == "jpeg"_s) {
        // JPEG has no transparency. Put the page on white, as a browser
        // window would, instead of letting clear areas come out black.
        auto pixels = unsafeMakeSpan(static_cast<uint8_t*>(bitmap.getPixels()), bitmap.computeByteSize());
        for (size_t offset = 0; offset + 3 < pixels.size(); offset += 4) {
            unsigned transparency = 255 - pixels[offset + 3];
            if (!transparency)
                continue;
            for (size_t channel = 0; channel < 3; ++channel)
                pixels[offset + channel] = std::min(255u, pixels[offset + channel] + transparency);
            pixels[offset + 3] = 255;
        }
        SkJpegEncoder::Options options;
        options.fQuality = std::clamp(quality, 0, 100);
        if (!SkJpegEncoder::Encode(&stream, bitmap.pixmap(), options))
            return { };
    } else {
        SkWebpEncoder::Options options;
        options.fQuality = std::clamp(quality, 0, 100);
        if (!SkWebpEncoder::Encode(&stream, bitmap.pixmap(), options))
            return { };
    }
    auto encoded = stream.detachAsData();
    return base64EncodeToString(unsafeMakeSpan(static_cast<const uint8_t*>(encoded->data()), encoded->size()));
}

} // namespace Zebkit::CDP
