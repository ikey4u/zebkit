// A canvas that writes PDF operators (canvas.cc), for the code that puts the
// outcome in an appearance stream (pdf_appearance.cc): plain values, neither
// Skia's nor base's nor PDFium's, as those files cannot all meet.

#ifndef ZEBKIT_ENGINE_CANVAS_INTERNAL_H_
#define ZEBKIT_ENGINE_CANVAS_INTERNAL_H_

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "zebkit/engine/include/zebkit/canvas.h"

namespace zebkit {

struct PdfCanvasOutput {
  double width = 0, height = 0;
  std::string content;  // the operators, in a space of y up
  struct State {
    std::string name;
    double fill_alpha = 1, stroke_alpha = 1;
    std::string blend;
  };
  std::vector<State> states;
  struct Image {
    std::string name;
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
  };
  std::vector<Image> images;
  struct Shading {
    std::string name;
    bool radial = false;
    std::array<double, 6> coords = {};  // x0 y0 x1 y1, or x y r x y r
    std::vector<double> offsets;
    std::vector<uint32_t> colors;
  };
  std::vector<Shading> shadings;
};

// A canvas @width by @height points that writes PDF operators.
zebkit_canvas* NewPdfCanvas(double width, double height);
// What it has written, with its saved states closed; empty for a canvas of
// pixels. The canvas is still the caller's to close.
PdfCanvasOutput TakePdfCanvas(zebkit_canvas* canvas);

// A canvas of @width by @height pixels over @rgba, whose first space is
// @matrix (a, b, c, d, e, f) rather than the pixels, and y up as page space
// is, so that text and images stand upright in it.
zebkit_canvas* WrapCanvasInPageSpace(uint8_t* rgba,
                                     int width,
                                     int height,
                                     size_t stride,
                                     const double matrix[6]);

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_CANVAS_INTERNAL_H_
