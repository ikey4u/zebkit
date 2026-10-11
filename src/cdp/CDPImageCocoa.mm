/*
 * Screenshots in the formats CDP offers, on macOS. WebKit's inspector
 * produces PNG only; JPEG is made here by re-encoding it with ImageIO. The
 * system has no encoder for WebP, so that format is not offered.
 */

#import "config.h"
#import "CDPInternal.h"

#import <ImageIO/ImageIO.h>
#import <wtf/RetainPtr.h>
#import <wtf/text/Base64.h>

namespace Zebkit::CDP {

String transcodeScreenshot(const String& base64PNG, const String& format, int quality)
{
    if (format != "jpeg"_s)
        return { };
    auto png = base64Decode(base64PNG);
    if (!png)
        return { };
    auto data = adoptCF(CFDataCreate(nullptr, png->span().data(), png->size()));
    auto source = adoptCF(CGImageSourceCreateWithData(data.get(), nullptr));
    auto image = source ? adoptCF(CGImageSourceCreateImageAtIndex(source.get(), 0, nullptr)) : nullptr;
    if (!image)
        return { };

    // JPEG has no transparency. Put the page on white, as a browser window
    // would, instead of letting clear areas come out black.
    size_t width = CGImageGetWidth(image.get());
    size_t height = CGImageGetHeight(image.get());
    auto colorSpace = adoptCF(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
    auto context = adoptCF(CGBitmapContextCreate(nullptr, width, height, 8, 0, colorSpace.get(), static_cast<uint32_t>(kCGImageAlphaNoneSkipLast)));
    if (!context)
        return { };
    CGContextSetRGBFillColor(context.get(), 1, 1, 1, 1);
    CGContextFillRect(context.get(), CGRectMake(0, 0, width, height));
    CGContextDrawImage(context.get(), CGRectMake(0, 0, width, height), image.get());
    auto opaque = adoptCF(CGBitmapContextCreateImage(context.get()));

    auto output = adoptCF(CFDataCreateMutable(nullptr, 0));
    auto destination = adoptCF(CGImageDestinationCreateWithData(output.get(), CFSTR("public.jpeg"), 1, nullptr));
    if (!opaque || !destination)
        return { };
    NSDictionary *options = @{ (__bridge NSString *)kCGImageDestinationLossyCompressionQuality: @(std::clamp(quality, 0, 100) / 100.0) };
    CGImageDestinationAddImage(destination.get(), opaque.get(), (__bridge CFDictionaryRef)options);
    if (!CGImageDestinationFinalize(destination.get()))
        return { };
    return base64EncodeToString(unsafeMakeSpan(CFDataGetBytePtr(output.get()), static_cast<size_t>(CFDataGetLength(output.get()))));
}

} // namespace Zebkit::CDP
