// zebkit's automation, screenshots compared as Playwright's expect does
// (packages/utils/comparators.ts, third_party/pixelmatch.js), for
// Page.expectScreenshot.

#ifndef ZEBKIT_ENGINE_AUTOMATION_COMPARE_H_
#define ZEBKIT_ENGINE_AUTOMATION_COMPARE_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <vector>

#include "base/containers/span.h"

namespace zebkit {

// What Playwright's comparators.ts compareImages() finds of two PNG images.
struct ImageDifference {
  // Empty when the images match.
  std::string message;
  // The diff image (a PNG), as pixelmatch draws it.
  std::vector<uint8_t> diff;
};

struct ImageCompareOptions {
  std::optional<double> threshold;
  std::optional<double> max_diff_pixels;
  std::optional<double> max_diff_pixel_ratio;
  std::string comparator;
};

// The images' difference with pixelmatch (the default comparator): nullopt
// when they match within the options; an error message in @error when
// @expected is not an image or the comparator is unknown.
std::optional<ImageDifference> CompareImages(base::span<const uint8_t> actual,
                                             base::span<const uint8_t> expected,
                                             const ImageCompareOptions& options,
                                             std::string* error);

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_AUTOMATION_COMPARE_H_
