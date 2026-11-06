// zebkit's automation, screenshots compared as Playwright's expect does
// (automation_compare.h).

#ifdef UNSAFE_BUFFERS_BUILD
// pixelmatch's loops index the pixels as the original does.
#pragma allow_unsafe_buffers
#endif

#include "zebkit/engine/automation_compare.h"

#include <math.h>
#include <string.h>

#include <algorithm>
#include <cstdlib>

#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/geometry/size.h"

namespace zebkit {

namespace {

struct Image {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> data;
};

std::optional<Image> Decode(base::span<const uint8_t> png) {
  std::optional<gfx::PNGCodec::DecodeOutput> decoded =
      gfx::PNGCodec::Decode(png, gfx::PNGCodec::FORMAT_RGBA);
  if (!decoded)
    return std::nullopt;
  Image image;
  image.width = decoded->width;
  image.height = decoded->height;
  image.data = std::move(decoded->output);
  return image;
}

// imageUtils.ts's padImageToSize(): transparent pixels right and below.
Image Pad(const Image& image, int width, int height) {
  if (image.width == width && image.height == height)
    return image;
  Image padded;
  padded.width = width;
  padded.height = height;
  padded.data.assign(static_cast<size_t>(width) * height * 4, 0);
  for (int y = 0; y < image.height && y < height; ++y) {
    memcpy(&padded.data[(static_cast<size_t>(y) * width) * 4],
           &image.data[(static_cast<size_t>(y) * image.width) * 4],
           static_cast<size_t>(std::min(image.width, width)) * 4);
  }
  return padded;
}

// ---- pixelmatch (ISC License, Copyright (c) 2019, Mapbox) ----------------

double Blend(double c, double a) {
  return 255 + (c - 255) * a;
}
double RgbToY(double r, double g, double b) {
  return r * 0.29889531 + g * 0.58662247 + b * 0.11448223;
}
double RgbToI(double r, double g, double b) {
  return r * 0.59597799 - g * 0.27417610 - b * 0.32180189;
}
double RgbToQ(double r, double g, double b) {
  return r * 0.21147017 - g * 0.52261711 + b * 0.31114694;
}

double ColorDelta(const uint8_t* img1, const uint8_t* img2, size_t k, size_t m, bool y_only) {
  double r1 = img1[k + 0], g1 = img1[k + 1], b1 = img1[k + 2], a1 = img1[k + 3];
  double r2 = img2[m + 0], g2 = img2[m + 1], b2 = img2[m + 2], a2 = img2[m + 3];
  if (a1 == a2 && r1 == r2 && g1 == g2 && b1 == b2)
    return 0;
  if (a1 < 255) {
    a1 /= 255;
    r1 = Blend(r1, a1);
    g1 = Blend(g1, a1);
    b1 = Blend(b1, a1);
  }
  if (a2 < 255) {
    a2 /= 255;
    r2 = Blend(r2, a2);
    g2 = Blend(g2, a2);
    b2 = Blend(b2, a2);
  }
  const double y1 = RgbToY(r1, g1, b1);
  const double y2 = RgbToY(r2, g2, b2);
  const double y = y1 - y2;
  if (y_only)
    return y;
  const double i = RgbToI(r1, g1, b1) - RgbToI(r2, g2, b2);
  const double q = RgbToQ(r1, g1, b1) - RgbToQ(r2, g2, b2);
  const double delta = 0.5053 * y * y + 0.299 * i * i + 0.1957 * q * q;
  return y1 > y2 ? -delta : delta;
}

bool HasManySiblings(const uint8_t* img, int x1, int y1, int width, int height) {
  const int x0 = std::max(x1 - 1, 0);
  const int y0 = std::max(y1 - 1, 0);
  const int x2 = std::min(x1 + 1, width - 1);
  const int y2 = std::min(y1 + 1, height - 1);
  const size_t pos = (static_cast<size_t>(y1) * width + x1) * 4;
  int zeroes = x1 == x0 || x1 == x2 || y1 == y0 || y1 == y2 ? 1 : 0;
  for (int x = x0; x <= x2; ++x) {
    for (int y = y0; y <= y2; ++y) {
      if (x == x1 && y == y1)
        continue;
      const size_t pos2 = (static_cast<size_t>(y) * width + x) * 4;
      if (memcmp(img + pos, img + pos2, 4) == 0)
        ++zeroes;
      if (zeroes > 2)
        return true;
    }
  }
  return false;
}

bool Antialiased(const uint8_t* img, int x1, int y1, int width, int height, const uint8_t* img2) {
  const int x0 = std::max(x1 - 1, 0);
  const int y0 = std::max(y1 - 1, 0);
  const int x2 = std::min(x1 + 1, width - 1);
  const int y2 = std::min(y1 + 1, height - 1);
  const size_t pos = (static_cast<size_t>(y1) * width + x1) * 4;
  int zeroes = x1 == x0 || x1 == x2 || y1 == y0 || y1 == y2 ? 1 : 0;
  double min = 0;
  double max = 0;
  int min_x = 0, min_y = 0, max_x = 0, max_y = 0;
  for (int x = x0; x <= x2; ++x) {
    for (int y = y0; y <= y2; ++y) {
      if (x == x1 && y == y1)
        continue;
      const double delta = ColorDelta(img, img, pos, (static_cast<size_t>(y) * width + x) * 4, true);
      if (delta == 0) {
        ++zeroes;
        if (zeroes > 2)
          return false;
      } else if (delta < min) {
        min = delta;
        min_x = x;
        min_y = y;
      } else if (delta > max) {
        max = delta;
        max_x = x;
        max_y = y;
      }
    }
  }
  if (min == 0 || max == 0)
    return false;
  return (HasManySiblings(img, min_x, min_y, width, height) &&
          HasManySiblings(img2, min_x, min_y, width, height)) ||
         (HasManySiblings(img, max_x, max_y, width, height) &&
          HasManySiblings(img2, max_x, max_y, width, height));
}

void DrawPixel(uint8_t* output, size_t pos, double r, double g, double b) {
  output[pos + 0] = static_cast<uint8_t>(std::clamp(r, 0.0, 255.0));
  output[pos + 1] = static_cast<uint8_t>(std::clamp(g, 0.0, 255.0));
  output[pos + 2] = static_cast<uint8_t>(std::clamp(b, 0.0, 255.0));
  output[pos + 3] = 255;
}

void DrawGrayPixel(const uint8_t* img, size_t i, double alpha, uint8_t* output) {
  const double value = Blend(RgbToY(img[i + 0], img[i + 1], img[i + 2]), alpha * img[i + 3] / 255);
  DrawPixel(output, i, value, value, value);
}

// pixelmatch(img1, img2, output, width, height, {threshold}).
int Pixelmatch(const uint8_t* img1, const uint8_t* img2, uint8_t* output, int width, int height,
               double threshold) {
  constexpr double kAlpha = 0.1;
  const size_t length = static_cast<size_t>(width) * height;
  if (memcmp(img1, img2, length * 4) == 0) {
    for (size_t i = 0; i < length; ++i)
      DrawGrayPixel(img1, 4 * i, kAlpha, output);
    return 0;
  }
  const double max_delta = 35215 * threshold * threshold;
  int diff = 0;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const size_t pos = (static_cast<size_t>(y) * width + x) * 4;
      const double delta = ColorDelta(img1, img2, pos, pos, false);
      if (std::abs(delta) > max_delta) {
        if (Antialiased(img1, x, y, width, height, img2) ||
            Antialiased(img2, x, y, width, height, img1)) {
          DrawPixel(output, pos, 255, 255, 0);
        } else {
          DrawPixel(output, pos, 255, 0, 0);
          ++diff;
        }
      } else {
        DrawGrayPixel(img1, pos, kAlpha, output);
      }
    }
  }
  return diff;
}

}  // namespace

std::optional<ImageDifference> CompareImages(base::span<const uint8_t> actual_png,
                                             base::span<const uint8_t> expected_png,
                                             const ImageCompareOptions& options,
                                             std::string* error) {
  static constexpr uint8_t kPngMagic[] = {137, 80, 78, 71, 13, 10, 26, 10};
  if (expected_png.size() < sizeof(kPngMagic) ||
      memcmp(expected_png.data(), kPngMagic, sizeof(kPngMagic)) != 0) {
    *error = "Could not decode expected image as PNG.";
    return std::nullopt;
  }
  std::optional<Image> actual = Decode(actual_png);
  std::optional<Image> expected = Decode(expected_png);
  if (!actual || !expected) {
    *error = "Could not decode the image as PNG.";
    return std::nullopt;
  }
  const std::string comparator = options.comparator.empty() ? "pixelmatch" : options.comparator;
  if (comparator != "pixelmatch") {
    *error = "Configuration specifies unknown comparator \"" + comparator + "\"";
    return std::nullopt;
  }
  const int width = std::max(expected->width, actual->width);
  const int height = std::max(expected->height, actual->height);
  std::string sizes;
  if (expected->width != actual->width || expected->height != actual->height) {
    sizes = base::StringPrintf("Expected an image %dpx by %dpx, received %dpx by %dpx. ",
                               expected->width, expected->height, actual->width, actual->height);
  }
  const Image a = Pad(*actual, width, height);
  const Image e = Pad(*expected, width, height);
  std::vector<uint8_t> diff(static_cast<size_t>(width) * height * 4, 0);
  const int count = width && height
                        ? Pixelmatch(e.data.data(), a.data.data(), diff.data(), width, height,
                                     options.threshold.value_or(0.2))
                        : 0;
  const double pixels = static_cast<double>(expected->width) * expected->height;
  std::optional<double> max_by_ratio;
  if (options.max_diff_pixel_ratio)
    max_by_ratio = pixels * *options.max_diff_pixel_ratio;
  double max_diff = 0;
  if (options.max_diff_pixels && max_by_ratio)
    max_diff = std::min(*options.max_diff_pixels, *max_by_ratio);
  else
    max_diff = options.max_diff_pixels.value_or(max_by_ratio.value_or(0));
  const double ratio = pixels ? ceil(count / pixels * 100) / 100 : INFINITY;
  std::string mismatch;
  if (count > max_diff) {
    mismatch = base::NumberToString(count) + " pixels (ratio " + base::StringPrintf("%.2f", ratio) +
               " of all image pixels) are different.";
  }
  if (mismatch.empty() && sizes.empty())
    return std::nullopt;
  ImageDifference difference;
  difference.message = sizes + mismatch;
  if (width && height) {
    difference.diff = gfx::PNGCodec::Encode(diff.data(), gfx::PNGCodec::FORMAT_RGBA,
                                            gfx::Size(width, height), width * 4,
                                            /*discard_transparency=*/false, {})
                          .value_or(std::vector<uint8_t>());
  }
  return difference;
}

}  // namespace zebkit
