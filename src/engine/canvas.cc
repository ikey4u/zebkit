// <zebkit/canvas.h>: one set of calls over two backends, Skia over pixels and
// PDF operators for appearance streams. The canvas keeps the state and the
// current path in its first space, as the web's canvas keeps points already
// through the matrix; the backends draw those.

#ifdef UNSAFE_BUFFERS_BUILD
// The C API takes C strings and arrays.
#pragma allow_unsafe_buffers
#endif

#include "zebkit/engine/include/zebkit/canvas.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/json/json_reader.h"
#include "base/values.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkBlendMode.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkData.h"
#include "third_party/skia/include/core/SkFont.h"
#include "third_party/skia/include/core/SkFontArguments.h"
#include "third_party/skia/include/core/SkFontTypes.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "third_party/skia/include/core/SkM44.h"
#include "third_party/skia/include/core/SkMatrix.h"
#include "third_party/skia/include/core/SkPaint.h"
#include "third_party/skia/include/core/SkPath.h"
#include "third_party/skia/include/core/SkPathBuilder.h"
#include "third_party/skia/include/core/SkPixmap.h"
#include "third_party/skia/include/core/SkShader.h"
#include "third_party/skia/include/core/SkTypeface.h"
#include "third_party/skia/include/effects/SkDashPathEffect.h"
#include "third_party/skia/include/effects/SkGradient.h"
#include "third_party/skia/include/ports/SkTypeface_fontations.h"
#include "zebkit/engine/canvas_internal.h"
#include "zebkit/engine/include/zebkit/services.h"
#include "zebkit/engine/text_internal.h"

using zebkit::PdfCanvasOutput;

namespace {

struct Matrix {
  double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
  // This matrix applied after @m, as PDF's cm and the web's transform().
  Matrix Then(const Matrix& m) const {
    return {m.a * a + m.b * c,       m.a * b + m.b * d,
            m.c * a + m.d * c,       m.c * b + m.d * d,
            m.e * a + m.f * c + e,   m.e * b + m.f * d + f};
  }
  void Apply(double x, double y, double* out_x, double* out_y) const {
    *out_x = a * x + c * y + e;
    *out_y = b * x + d * y + f;
  }
  std::optional<Matrix> Inverse() const {
    const double det = a * d - b * c;
    if (!std::isfinite(det) || det == 0)
      return std::nullopt;
    return Matrix{d / det,  -b / det, -c / det, a / det,
                  (c * f - d * e) / det, (b * e - a * f) / det};
  }
  SkMatrix ToSkia() const {
    return SkMatrix::MakeAll(a, c, e, b, d, f, 0, 0, 1);
  }
};

struct Segment {
  enum Verb { kMove, kLine, kQuad, kCubic, kClose } verb;
  double p[6];
};
using Path = std::vector<Segment>;
constexpr size_t kMaxSegments = 1u << 24;

Path Transformed(const Path& path, const Matrix& m) {
  Path out = path;
  for (Segment& segment : out) {
    const int points = segment.verb == Segment::kCubic  ? 3
                       : segment.verb == Segment::kQuad ? 2
                       : segment.verb == Segment::kClose ? 0
                                                         : 1;
    for (int i = 0; i < points; ++i) {
      m.Apply(segment.p[i * 2], segment.p[i * 2 + 1], &segment.p[i * 2],
              &segment.p[i * 2 + 1]);
    }
  }
  return out;
}

struct Gradient {
  bool radial = false;
  std::array<double, 6> coords = {};  // in the first space
  std::vector<double> offsets;
  std::vector<uint32_t> colors;
};

struct Font {
  explicit Font(const void* data, size_t size) : face(data, size) {
    if (!face.valid())
      return;
    typeface = SkTypeface_Make_Fontations(
        SkData::MakeWithCopy(face.bytes().data(), face.bytes().size()),
        SkFontArguments());
  }
  zebkit::TextFace face;
  sk_sp<SkTypeface> typeface;
};

struct State {
  Matrix matrix;
  uint32_t fill = 0x000000ff, stroke = 0x000000ff;
  std::optional<Gradient> gradient;
  double alpha = 1;
  std::string blend = "Normal";
  double line_width = 1, miter = 10;
  int cap = 0, join = 0;
  std::vector<double> dash;
  double dash_phase = 0;
  std::shared_ptr<Font> font;
  double font_size = 12;
};

class Backend {
 public:
  virtual ~Backend() = default;
  virtual void Save() = 0;
  virtual void Restore() = 0;
  // Paths are in the first space.
  virtual void Fill(const Path& path, bool even_odd, const State& state) = 0;
  virtual void Stroke(const Path& path, const State& state) = 0;
  virtual void Clip(const Path& path, bool even_odd) = 0;
  virtual void Clear(uint32_t rgba, double width, double height) = 0;
  // @matrix maps the image's unit square, its top row at y 1, as PDF has it.
  virtual void Image(const uint8_t* rgba,
                     int width,
                     int height,
                     const Matrix& matrix,
                     const State& state) = 0;
};

SkColor4f ToColor(uint32_t rgba, double alpha = 1) {
  return {((rgba >> 24) & 0xff) / 255.0f, ((rgba >> 16) & 0xff) / 255.0f,
          ((rgba >> 8) & 0xff) / 255.0f,
          static_cast<float>((rgba & 0xff) / 255.0 * alpha)};
}

constexpr std::pair<const char*, SkBlendMode> kBlends[] = {
    {"Normal", SkBlendMode::kSrcOver},
    {"Multiply", SkBlendMode::kMultiply},
    {"Screen", SkBlendMode::kScreen},
    {"Overlay", SkBlendMode::kOverlay},
    {"Darken", SkBlendMode::kDarken},
    {"Lighten", SkBlendMode::kLighten},
    {"ColorDodge", SkBlendMode::kColorDodge},
    {"ColorBurn", SkBlendMode::kColorBurn},
    {"HardLight", SkBlendMode::kHardLight},
    {"SoftLight", SkBlendMode::kSoftLight},
    {"Difference", SkBlendMode::kDifference},
    {"Exclusion", SkBlendMode::kExclusion},
    {"Hue", SkBlendMode::kHue},
    {"Saturation", SkBlendMode::kSaturation},
    {"Color", SkBlendMode::kColor},
    {"Luminosity", SkBlendMode::kLuminosity},
};

SkPath ToSkia(const Path& path, bool even_odd) {
  SkPathBuilder builder;
  builder.setFillType(even_odd ? SkPathFillType::kEvenOdd
                               : SkPathFillType::kWinding);
  for (const Segment& s : path) {
    switch (s.verb) {
      case Segment::kMove:
        builder.moveTo(s.p[0], s.p[1]);
        break;
      case Segment::kLine:
        builder.lineTo(s.p[0], s.p[1]);
        break;
      case Segment::kQuad:
        builder.quadTo(s.p[0], s.p[1], s.p[2], s.p[3]);
        break;
      case Segment::kCubic:
        builder.cubicTo(s.p[0], s.p[1], s.p[2], s.p[3], s.p[4], s.p[5]);
        break;
      case Segment::kClose:
        builder.close();
        break;
    }
  }
  return builder.detach();
}

class SkiaBackend : public Backend {
 public:
  SkiaBackend(const SkBitmap& bitmap) : canvas_(bitmap) {}

  void Save() override { canvas_.save(); }
  void Restore() override { canvas_.restore(); }

  void Fill(const Path& path, bool even_odd, const State& state) override {
    SkPaint paint = BasePaint(state);
    if (state.gradient) {
      std::vector<SkColor4f> colors;
      std::vector<float> offsets;
      for (size_t i = 0; i < state.gradient->colors.size(); ++i) {
        colors.push_back(ToColor(state.gradient->colors[i]));
        offsets.push_back(static_cast<float>(state.gradient->offsets[i]));
      }
      const SkGradient gradient(
          SkGradient::Colors(colors, offsets, SkTileMode::kClamp),
          SkGradient::Interpolation());
      const auto& c = state.gradient->coords;
      if (state.gradient->radial) {
        paint.setShader(SkShaders::RadialGradient(
            {static_cast<float>(c[0]), static_cast<float>(c[1])},
            static_cast<float>(c[2]), gradient));
      } else {
        const SkPoint points[2] = {
            {static_cast<float>(c[0]), static_cast<float>(c[1])},
            {static_cast<float>(c[2]), static_cast<float>(c[3])}};
        paint.setShader(SkShaders::LinearGradient(points, gradient));
      }
      paint.setAlphaf(static_cast<float>(state.alpha));
    } else {
      paint.setColor4f(ToColor(state.fill, state.alpha));
    }
    canvas_.setMatrix(SkMatrix::I());
    canvas_.drawPath(ToSkia(path, even_odd), paint);
  }

  void Stroke(const Path& path, const State& state) override {
    // The stroke's shape is in the matrix's space: a scale widens it.
    const std::optional<Matrix> inverse = state.matrix.Inverse();
    if (!inverse)
      return;
    SkPaint paint = BasePaint(state);
    paint.setColor4f(ToColor(state.stroke, state.alpha));
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(static_cast<float>(state.line_width));
    paint.setStrokeMiter(static_cast<float>(state.miter));
    constexpr SkPaint::Cap kCaps[] = {SkPaint::kButt_Cap, SkPaint::kRound_Cap,
                                      SkPaint::kSquare_Cap};
    constexpr SkPaint::Join kJoins[] = {
        SkPaint::kMiter_Join, SkPaint::kRound_Join, SkPaint::kBevel_Join};
    paint.setStrokeCap(kCaps[state.cap]);
    paint.setStrokeJoin(kJoins[state.join]);
    if (!state.dash.empty()) {
      // Skia wants an even number of lengths, as PDF does not.
      std::vector<SkScalar> lengths;
      for (double length : state.dash)
        lengths.push_back(static_cast<SkScalar>(length));
      if (lengths.size() % 2) {
        const size_t n = lengths.size();
        for (size_t i = 0; i < n; ++i)
          lengths.push_back(lengths[i]);
      }
      paint.setPathEffect(SkDashPathEffect::Make(
          lengths, static_cast<SkScalar>(state.dash_phase)));
    }
    canvas_.setMatrix(state.matrix.ToSkia());
    canvas_.drawPath(ToSkia(Transformed(path, *inverse), false), paint);
    canvas_.setMatrix(SkMatrix::I());
  }

  void Clip(const Path& path, bool even_odd) override {
    canvas_.setMatrix(SkMatrix::I());
    canvas_.clipPath(ToSkia(path, even_odd), SkClipOp::kIntersect, true);
  }

  void Clear(uint32_t rgba, double, double) override {
    canvas_.setMatrix(SkMatrix::I());
    SkPaint paint;
    paint.setColor4f(ToColor(rgba));
    paint.setBlendMode(SkBlendMode::kSrc);
    canvas_.drawPaint(paint);
  }

  void Image(const uint8_t* rgba,
             int width,
             int height,
             const Matrix& matrix,
             const State& state) override {
    const SkImageInfo info = SkImageInfo::Make(
        width, height, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    sk_sp<SkImage> image = SkImages::RasterFromPixmapCopy(
        SkPixmap(info, rgba, static_cast<size_t>(width) * 4));
    if (!image)
      return;
    SkPaint paint = BasePaint(state);
    paint.setAlphaf(static_cast<float>(state.alpha));
    // The image's pixels to its unit square, top row at y 1.
    const Matrix to_unit = {1.0 / width, 0, 0, -1.0 / height, 0, 1};
    canvas_.setMatrix(matrix.Then(to_unit).ToSkia());
    canvas_.drawImage(image, 0, 0,
                      SkSamplingOptions(SkFilterMode::kLinear), &paint);
    canvas_.setMatrix(SkMatrix::I());
  }

 private:
  SkPaint BasePaint(const State& state) {
    SkPaint paint;
    paint.setAntiAlias(true);
    for (const auto& [name, mode] : kBlends) {
      if (state.blend == name)
        paint.setBlendMode(mode);
    }
    return paint;
  }

  SkCanvas canvas_;
};

// A number as PDF writes one, whatever the locale.
void Number(std::string& out, double value) {
  if (!std::isfinite(value))
    value = 0;
  value = std::clamp(value, -1e7, 1e7);
  char buffer[32];
  const char* end = std::to_chars(buffer, buffer + sizeof(buffer), value,
                           std::chars_format::fixed, 4)
                 .ptr;
  std::string_view text(buffer, static_cast<size_t>(end - buffer));
  while (text.back() == '0')
    text.remove_suffix(1);
  if (text.back() == '.')
    text.remove_suffix(1);
  out.append(text);
  out.push_back(' ');
}

class PdfBackend : public Backend {
 public:
  void Save() override { output_.content += "q\n"; ++depth_; }
  void Restore() override {
    if (depth_ > 0) {
      output_.content += "Q\n";
      --depth_;
    }
  }

  void Fill(const Path& path, bool even_odd, const State& state) override {
    std::string& out = output_.content;
    out += "q\n";
    if (state.gradient) {
      UseState(state.alpha, 1, state.blend);
      Write(path);
      out += even_odd ? "W* n\n" : "W n\n";
      PdfCanvasOutput::Shading shading;
      shading.name = "Sh" + std::to_string(output_.shadings.size());
      shading.radial = state.gradient->radial;
      shading.coords = state.gradient->coords;
      shading.offsets = state.gradient->offsets;
      shading.colors = state.gradient->colors;
      out += "/" + shading.name + " sh\n";
      output_.shadings.push_back(std::move(shading));
    } else {
      UseState((state.fill & 0xff) / 255.0 * state.alpha, 1, state.blend);
      Color(state.fill, "rg\n");
      Write(path);
      out += even_odd ? "f*\n" : "f\n";
    }
    out += "Q\n";
  }

  void Stroke(const Path& path, const State& state) override {
    const std::optional<Matrix> inverse = state.matrix.Inverse();
    if (!inverse)
      return;
    std::string& out = output_.content;
    out += "q\n";
    UseState(1, (state.stroke & 0xff) / 255.0 * state.alpha, state.blend);
    Color(state.stroke, "RG\n");
    Concat(state.matrix);
    Number(out, state.line_width);
    out += "w ";
    out += std::to_string(state.cap) + " J " + std::to_string(state.join) + " j ";
    Number(out, state.miter);
    out += "M\n";
    if (!state.dash.empty()) {
      out += "[";
      for (double length : state.dash)
        Number(out, length);
      out += "] ";
      Number(out, state.dash_phase);
      out += "d\n";
    }
    Write(Transformed(path, *inverse));
    out += "S\nQ\n";
  }

  void Clip(const Path& path, bool even_odd) override {
    Write(path);
    output_.content += even_odd ? "W* n\n" : "W n\n";
  }

  void Clear(uint32_t rgba, double width, double height) override {
    if (!(rgba & 0xff))
      return;
    std::string& out = output_.content;
    out += "q\n";
    UseState((rgba & 0xff) / 255.0, 1, "Normal");
    Color(rgba, "rg\n");
    out += "0 0 ";
    Number(out, width);
    Number(out, height);
    out += "re f\nQ\n";
  }

  void Image(const uint8_t* rgba,
             int width,
             int height,
             const Matrix& matrix,
             const State& state) override {
    PdfCanvasOutput::Image image;
    image.name = "Im" + std::to_string(output_.images.size());
    image.width = width;
    image.height = height;
    image.rgba.assign(rgba, rgba + static_cast<size_t>(width) * height * 4);
    std::string& out = output_.content;
    out += "q\n";
    UseState(state.alpha, 1, state.blend);
    Concat(matrix);
    out += "/" + image.name + " Do\nQ\n";
    output_.images.push_back(std::move(image));
  }

  PdfCanvasOutput Take(double width, double height) {
    PdfCanvasOutput output = output_;
    for (int i = 0; i < depth_; ++i)
      output.content += "Q\n";
    output.width = width;
    output.height = height;
    return output;
  }

 private:
  void Concat(const Matrix& m) {
    std::string& out = output_.content;
    for (double value : {m.a, m.b, m.c, m.d, m.e, m.f})
      Number(out, value);
    out += "cm\n";
  }

  void Color(uint32_t rgba, const char* op) {
    std::string& out = output_.content;
    Number(out, ((rgba >> 24) & 0xff) / 255.0);
    Number(out, ((rgba >> 16) & 0xff) / 255.0);
    Number(out, ((rgba >> 8) & 0xff) / 255.0);
    out += op;
  }

  void UseState(double fill_alpha, double stroke_alpha, const std::string& blend) {
    if (fill_alpha >= 1 && stroke_alpha >= 1 && blend == "Normal")
      return;
    std::string name;
    for (const auto& state : output_.states) {
      if (state.fill_alpha == fill_alpha && state.stroke_alpha == stroke_alpha &&
          state.blend == blend) {
        name = state.name;
      }
    }
    if (name.empty()) {
      name = "G" + std::to_string(output_.states.size());
      output_.states.push_back({name, fill_alpha, stroke_alpha, blend});
    }
    output_.content += "/" + name + " gs\n";
  }

  void Write(const Path& path) {
    std::string& out = output_.content;
    for (const Segment& s : path) {
      switch (s.verb) {
        case Segment::kMove:
          Number(out, s.p[0]);
          Number(out, s.p[1]);
          out += "m\n";
          break;
        case Segment::kLine:
          Number(out, s.p[0]);
          Number(out, s.p[1]);
          out += "l\n";
          break;
        case Segment::kQuad:
          // PDF has cubics; the current point is the one before.
          Number(out, s.p[0]);
          Number(out, s.p[1]);
          Number(out, s.p[0]);
          Number(out, s.p[1]);
          Number(out, s.p[2]);
          Number(out, s.p[3]);
          out += "c\n";
          break;
        case Segment::kCubic:
          for (double value : s.p)
            Number(out, value);
          out += "c\n";
          break;
        case Segment::kClose:
          out += "h\n";
          break;
      }
    }
  }

  PdfCanvasOutput output_;
  int depth_ = 0;
};

bool Finite(double a, double b = 0, double c = 0, double d = 0, double e = 0,
            double f = 0) {
  return std::isfinite(a) && std::isfinite(b) && std::isfinite(c) &&
         std::isfinite(d) && std::isfinite(e) && std::isfinite(f);
}

char* Copy(const std::string& text) {
  return strdup(text.c_str());
}

void Failed(char** error, const std::string& message) {
  if (error)
    *error = Copy(message);
}

}  // namespace

struct zebkit_canvas_font {
  std::shared_ptr<Font> font;
};

struct zebkit_canvas {
  int width = 0, height = 0;
  double pdf_width = 0, pdf_height = 0;
  // The pixels: its own, or the caller's.
  std::vector<uint8_t> owned;
  uint8_t* pixels = nullptr;
  size_t stride = 0;
  bool unpremultiply_at_close = false;

  std::unique_ptr<Backend> backend;
  PdfBackend* pdf = nullptr;
  // The first space, which zebkit_canvas_set_matrix() starts from.
  Matrix base;
  // Page space: glyphs and images, which are y down, stand upright in it.
  bool y_up = false;
  State state;
  std::vector<State> saved;
  Path path;
  bool has_point = false;
  double point_x = 0, point_y = 0;     // the current point, in the first space
  double start_x = 0, start_y = 0;     // where the subpath began
  std::map<int, std::shared_ptr<Font>> run_fonts;  // zebkit_canvas_run()
};

namespace {

void Add(zebkit_canvas* c, Segment::Verb verb, double x0, double y0, double x1 = 0,
         double y1 = 0, double x2 = 0, double y2 = 0) {
  if (c->path.size() >= kMaxSegments)
    return;
  Segment s{verb, {x0, y0, x1, y1, x2, y2}};
  c->path.push_back(s);
}

// Premultiplies or, with @back, undoes it, in place.
void ConvertAlpha(uint8_t* pixels, int width, int height, size_t stride, bool back) {
  for (int y = 0; y < height; ++y) {
    uint8_t* row = pixels + stride * static_cast<size_t>(y);
    for (int x = 0; x < width; ++x, row += 4) {
      const unsigned alpha = row[3];
      if (alpha == 255)
        continue;
      for (int i = 0; i < 3; ++i) {
        if (back) {
          row[i] = alpha ? static_cast<uint8_t>(std::min(255u, (row[i] * 255u + alpha / 2) / alpha)) : 0;
        } else {
          row[i] = static_cast<uint8_t>((row[i] * alpha + 127) / 255);
        }
      }
    }
  }
}

zebkit_canvas* MakeSkia(uint8_t* pixels, int width, int height, size_t stride) {
  const SkImageInfo info = SkImageInfo::Make(
      width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  SkBitmap bitmap;
  if (!bitmap.installPixels(info, pixels, stride))
    return nullptr;
  auto canvas = std::make_unique<zebkit_canvas>();
  canvas->width = width;
  canvas->height = height;
  canvas->pixels = pixels;
  canvas->stride = stride;
  canvas->backend = std::make_unique<SkiaBackend>(bitmap);
  return canvas.release();
}

bool ValidSize(int width, int height) {
  return width > 0 && height > 0 && width <= 32768 && height <= 32768 &&
         static_cast<int64_t>(width) * height <= (int64_t{1} << 28);
}

// Adds the outlines of @text in the canvas's font to @into, in the first
// space.
bool TextOutlines(zebkit_canvas* c, const char* text, double x, double y,
                  Path* into, char** error) {
  const State& state = c->state;
  if (!state.font || !state.font->typeface) {
    Failed(error, "no font is set");
    return false;
  }
  if (!text || !Finite(x, y)) {
    Failed(error, "no text, or a position that is not finite");
    return false;
  }
  zebkit::ShapedText shaped;
  std::string problem;
  if (!zebkit::Shape(state.font->face, state.font_size, text, nullptr, nullptr,
                     &shaped, &problem)) {
    Failed(error, problem);
    return false;
  }
  SkFont font(state.font->typeface, static_cast<SkScalar>(state.font_size));
  font.setHinting(SkFontHinting::kNone);
  font.setSubpixel(true);
  font.setLinearMetrics(true);
  const double flip = c->y_up ? -1 : 1;
  double pen_x = x, pen_y = y;
  for (const zebkit::ShapedGlyph& glyph : shaped.glyphs) {
    const double origin_x = pen_x + glyph.offset_x;
    const double origin_y = pen_y - flip * glyph.offset_y;
    if (std::optional<SkPath> outline =
            font.getPath(static_cast<SkGlyphID>(glyph.glyph))) {
      auto place = [&](const SkPoint& p, double* out_x, double* out_y) {
        state.matrix.Apply(origin_x + p.x(), origin_y + flip * p.y(), out_x,
                           out_y);
      };
      SkPathIter iter = outline->iter();
      while (std::optional<SkPathIter::Rec> rec = iter.next()) {
        Segment s{};
        const auto points = rec->fPoints;
        switch (rec->fVerb) {
          case SkPathVerb::kMove:
            s.verb = Segment::kMove;
            place(points[0], &s.p[0], &s.p[1]);
            break;
          case SkPathVerb::kLine:
            s.verb = Segment::kLine;
            place(points[1], &s.p[0], &s.p[1]);
            break;
          case SkPathVerb::kQuad:
          case SkPathVerb::kConic:
            s.verb = Segment::kQuad;
            place(points[1], &s.p[0], &s.p[1]);
            place(points[2], &s.p[2], &s.p[3]);
            break;
          case SkPathVerb::kCubic:
            s.verb = Segment::kCubic;
            place(points[1], &s.p[0], &s.p[1]);
            place(points[2], &s.p[2], &s.p[3]);
            place(points[3], &s.p[4], &s.p[5]);
            break;
          case SkPathVerb::kClose:
            s.verb = Segment::kClose;
            break;
        }
        if (into->size() < kMaxSegments)
          into->push_back(s);
      }
    }
    pen_x += glyph.advance_x;
    pen_y -= flip * glyph.advance_y;
  }
  return true;
}

std::optional<Gradient> MakeGradient(zebkit_canvas* c,
                                     const double* offsets,
                                     const uint32_t* colors,
                                     int count) {
  if (!offsets || !colors || count < 2 || count > 256)
    return std::nullopt;
  Gradient gradient;
  double previous = -1;
  for (int i = 0; i < count; ++i) {
    if (!std::isfinite(offsets[i]) || offsets[i] < 0 || offsets[i] > 1 ||
        offsets[i] <= previous) {
      return std::nullopt;
    }
    previous = offsets[i];
    gradient.offsets.push_back(offsets[i]);
    gradient.colors.push_back(colors[i]);
  }
  return gradient;
}

}  // namespace

namespace zebkit {

zebkit_canvas* NewPdfCanvas(double width, double height) {
  auto canvas = std::make_unique<zebkit_canvas>();
  auto backend = std::make_unique<PdfBackend>();
  canvas->pdf = backend.get();
  canvas->backend = std::move(backend);
  canvas->pdf_width = width;
  canvas->pdf_height = height;
  canvas->width = static_cast<int>(std::ceil(width));
  canvas->height = static_cast<int>(std::ceil(height));
  // The top left, y down, as a canvas of pixels is; PDF is y up.
  canvas->base = {1, 0, 0, -1, 0, height};
  canvas->state.matrix = canvas->base;
  return canvas.release();
}

PdfCanvasOutput TakePdfCanvas(zebkit_canvas* canvas) {
  if (!canvas || !canvas->pdf)
    return PdfCanvasOutput();
  return canvas->pdf->Take(canvas->pdf_width, canvas->pdf_height);
}

zebkit_canvas* WrapCanvasInPageSpace(uint8_t* rgba,
                                     int width,
                                     int height,
                                     size_t stride,
                                     const double m[6]) {
  zebkit_canvas* canvas = zebkit_canvas_wrap(rgba, width, height, stride, 0, nullptr);
  if (!canvas)
    return nullptr;
  canvas->base = {m[0], m[1], m[2], m[3], m[4], m[5]};
  canvas->state.matrix = canvas->base;
  canvas->y_up = true;
  return canvas;
}

}  // namespace zebkit

extern "C" {

zebkit_canvas* zebkit_canvas_new(int width, int height, char** error) {
  if (!ValidSize(width, height)) {
    Failed(error, "the size is not positive, or over 32768 or 1 << 28 pixels");
    return nullptr;
  }
  std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
  uint8_t* data = pixels.data();
  zebkit_canvas* canvas = MakeSkia(data, width, height, static_cast<size_t>(width) * 4);
  if (!canvas) {
    Failed(error, "the canvas cannot be made");
    return nullptr;
  }
  canvas->owned = std::move(pixels);  // a move keeps the buffer where it is
  return canvas;
}

zebkit_canvas* zebkit_canvas_wrap(uint8_t* rgba,
                                  int width,
                                  int height,
                                  size_t stride,
                                  unsigned flags,
                                  char** error) {
  const size_t row = static_cast<size_t>(width > 0 ? width : 0) * 4;
  if (!stride)
    stride = row;
  if (!rgba || !ValidSize(width, height) || stride < row) {
    Failed(error, "no pixels, a size that is not positive, or too small a stride");
    return nullptr;
  }
  zebkit_canvas* canvas = MakeSkia(rgba, width, height, stride);
  if (!canvas) {
    Failed(error, "the canvas cannot be made");
    return nullptr;
  }
  // Only over rows Skia has taken: a stride it refuses says no such rows.
  if (flags & ZEBKIT_CANVAS_UNPREMULTIPLIED)
    ConvertAlpha(rgba, width, height, stride, false);
  canvas->unpremultiply_at_close = flags & ZEBKIT_CANVAS_UNPREMULTIPLIED;
  return canvas;
}

void zebkit_canvas_close(zebkit_canvas* canvas) {
  if (!canvas)
    return;
  // Skia has written already; it keeps no pixels of its own.
  canvas->backend.reset();
  if (canvas->unpremultiply_at_close) {
    ConvertAlpha(canvas->pixels, canvas->width, canvas->height, canvas->stride,
                 true);
  }
  delete canvas;
}

int zebkit_canvas_width(zebkit_canvas* canvas) {
  return canvas ? canvas->width : 0;
}

int zebkit_canvas_height(zebkit_canvas* canvas) {
  return canvas ? canvas->height : 0;
}

uint8_t* zebkit_canvas_pixels(zebkit_canvas* canvas, char** error) {
  if (!canvas || !canvas->pixels) {
    Failed(error, "this canvas has no pixels");
    return nullptr;
  }
  const size_t row = static_cast<size_t>(canvas->width) * 4;
  uint8_t* out = static_cast<uint8_t*>(malloc(row * canvas->height));
  if (!out) {
    Failed(error, "not enough memory");
    return nullptr;
  }
  for (int y = 0; y < canvas->height; ++y)
    memcpy(out + row * y, canvas->pixels + canvas->stride * y, row);
  ConvertAlpha(out, canvas->width, canvas->height, row, true);
  return out;
}

uint8_t* zebkit_canvas_png(zebkit_canvas* canvas, size_t* size, char** error) {
  uint8_t* pixels = zebkit_canvas_pixels(canvas, error);
  if (!pixels)
    return nullptr;
  uint8_t* png = zebkit_image_encode(ZEBKIT_IMAGE_PNG, pixels, canvas->width,
                                     canvas->height, -1, size, error);
  free(pixels);
  return png;
}

void zebkit_canvas_save(zebkit_canvas* c) {
  if (!c || c->saved.size() > 4096)
    return;
  c->saved.push_back(c->state);
  c->backend->Save();
}

void zebkit_canvas_restore(zebkit_canvas* c) {
  if (!c || c->saved.empty())
    return;
  c->state = std::move(c->saved.back());
  c->saved.pop_back();
  c->backend->Restore();
}

void zebkit_canvas_transform(zebkit_canvas* c, double a, double b, double cc,
                             double d, double e, double f) {
  if (c && Finite(a, b, cc, d, e, f))
    c->state.matrix = c->state.matrix.Then({a, b, cc, d, e, f});
}

void zebkit_canvas_translate(zebkit_canvas* c, double x, double y) {
  zebkit_canvas_transform(c, 1, 0, 0, 1, x, y);
}

void zebkit_canvas_scale(zebkit_canvas* c, double x, double y) {
  zebkit_canvas_transform(c, x, 0, 0, y, 0, 0);
}

void zebkit_canvas_rotate(zebkit_canvas* c, double degrees) {
  const double radians = degrees * std::numbers::pi / 180;
  const double cosine = std::cos(radians), sine = std::sin(radians);
  zebkit_canvas_transform(c, cosine, sine, -sine, cosine, 0, 0);
}

void zebkit_canvas_set_matrix(zebkit_canvas* c, double a, double b, double cc,
                              double d, double e, double f) {
  if (c && Finite(a, b, cc, d, e, f))
    c->state.matrix = c->base.Then({a, b, cc, d, e, f});
}

void zebkit_canvas_set_fill_color(zebkit_canvas* c, uint32_t rgba) {
  if (!c)
    return;
  c->state.fill = rgba;
  c->state.gradient.reset();
}

void zebkit_canvas_set_stroke_color(zebkit_canvas* c, uint32_t rgba) {
  if (c)
    c->state.stroke = rgba;
}

void zebkit_canvas_set_alpha(zebkit_canvas* c, double alpha) {
  if (c && std::isfinite(alpha))
    c->state.alpha = std::clamp(alpha, 0.0, 1.0);
}

bool zebkit_canvas_set_blend(zebkit_canvas* c, const char* mode) {
  if (!c || !mode)
    return false;
  for (const auto& blend : kBlends) {
    if (std::string_view(mode) == blend.first) {
      c->state.blend = mode;
      return true;
    }
  }
  return false;
}

void zebkit_canvas_set_line_width(zebkit_canvas* c, double width) {
  if (c && std::isfinite(width) && width >= 0)
    c->state.line_width = width;
}

void zebkit_canvas_set_line_cap(zebkit_canvas* c, zebkit_canvas_cap cap) {
  if (c && cap >= ZEBKIT_CANVAS_CAP_BUTT && cap <= ZEBKIT_CANVAS_CAP_SQUARE)
    c->state.cap = cap;
}

void zebkit_canvas_set_line_join(zebkit_canvas* c, zebkit_canvas_join join) {
  if (c && join >= ZEBKIT_CANVAS_JOIN_MITER && join <= ZEBKIT_CANVAS_JOIN_BEVEL)
    c->state.join = join;
}

void zebkit_canvas_set_miter_limit(zebkit_canvas* c, double limit) {
  if (c && std::isfinite(limit) && limit >= 1)
    c->state.miter = limit;
}

void zebkit_canvas_set_dash(zebkit_canvas* c, const double* lengths, int count,
                            double phase) {
  if (!c)
    return;
  c->state.dash.clear();
  c->state.dash_phase = std::isfinite(phase) ? phase : 0;
  if (!lengths || count <= 0 || count > 256)
    return;
  double total = 0;
  for (int i = 0; i < count; ++i) {
    if (!std::isfinite(lengths[i]) || lengths[i] < 0)
      return;
    total += lengths[i];
  }
  // All zero would be a line of nothing, which neither Skia nor PDF has.
  if (total <= 0)
    return;
  c->state.dash.assign(lengths, lengths + count);
}

bool zebkit_canvas_set_linear_gradient(zebkit_canvas* c, double x0, double y0,
                                       double x1, double y1,
                                       const double* offsets,
                                       const uint32_t* colors, int count) {
  if (!c || !Finite(x0, y0, x1, y1))
    return false;
  std::optional<Gradient> gradient = MakeGradient(c, offsets, colors, count);
  if (!gradient)
    return false;
  c->state.matrix.Apply(x0, y0, &gradient->coords[0], &gradient->coords[1]);
  c->state.matrix.Apply(x1, y1, &gradient->coords[2], &gradient->coords[3]);
  c->state.gradient = std::move(gradient);
  return true;
}

bool zebkit_canvas_set_radial_gradient(zebkit_canvas* c, double x, double y,
                                       double radius, const double* offsets,
                                       const uint32_t* colors, int count) {
  if (!c || !Finite(x, y, radius) || radius <= 0)
    return false;
  std::optional<Gradient> gradient = MakeGradient(c, offsets, colors, count);
  if (!gradient)
    return false;
  gradient->radial = true;
  c->state.matrix.Apply(x, y, &gradient->coords[0], &gradient->coords[1]);
  const Matrix& m = c->state.matrix;
  gradient->coords[2] = radius * std::sqrt(std::abs(m.a * m.d - m.b * m.c));
  c->state.gradient = std::move(gradient);
  return true;
}

void zebkit_canvas_begin_path(zebkit_canvas* c) {
  if (!c)
    return;
  c->path.clear();
  c->has_point = false;
}

void zebkit_canvas_move_to(zebkit_canvas* c, double x, double y) {
  if (!c || !Finite(x, y))
    return;
  c->state.matrix.Apply(x, y, &x, &y);
  Add(c, Segment::kMove, x, y);
  c->has_point = true;
  c->point_x = c->start_x = x;
  c->point_y = c->start_y = y;
}

void zebkit_canvas_line_to(zebkit_canvas* c, double x, double y) {
  if (!c || !Finite(x, y))
    return;
  if (!c->has_point) {
    zebkit_canvas_move_to(c, x, y);
    return;
  }
  c->state.matrix.Apply(x, y, &x, &y);
  Add(c, Segment::kLine, x, y);
  c->point_x = x;
  c->point_y = y;
}

void zebkit_canvas_quad_to(zebkit_canvas* c, double cx, double cy, double x, double y) {
  if (!c || !Finite(cx, cy, x, y))
    return;
  if (!c->has_point)
    zebkit_canvas_move_to(c, cx, cy);
  c->state.matrix.Apply(cx, cy, &cx, &cy);
  c->state.matrix.Apply(x, y, &x, &y);
  // A quadratic as a cubic: both backends draw it the same, PDF having no other.
  const double x0 = c->point_x, y0 = c->point_y;
  Add(c, Segment::kCubic, x0 + 2.0 / 3 * (cx - x0), y0 + 2.0 / 3 * (cy - y0),
      x + 2.0 / 3 * (cx - x), y + 2.0 / 3 * (cy - y), x, y);
  c->point_x = x;
  c->point_y = y;
}

void zebkit_canvas_curve_to(zebkit_canvas* c, double c1x, double c1y, double c2x,
                            double c2y, double x, double y) {
  if (!c || !Finite(c1x, c1y, c2x, c2y, x, y))
    return;
  if (!c->has_point)
    zebkit_canvas_move_to(c, c1x, c1y);
  c->state.matrix.Apply(c1x, c1y, &c1x, &c1y);
  c->state.matrix.Apply(c2x, c2y, &c2x, &c2y);
  c->state.matrix.Apply(x, y, &x, &y);
  Add(c, Segment::kCubic, c1x, c1y, c2x, c2y, x, y);
  c->point_x = x;
  c->point_y = y;
}

void zebkit_canvas_arc(zebkit_canvas* c, double x, double y, double radius,
                       double start, double end, bool counterclockwise) {
  if (!c || !Finite(x, y, radius, start, end) || radius < 0)
    return;
  const double kDegree = std::numbers::pi / 180;
  double sweep = end - start;
  if (counterclockwise) {
    if (sweep > 0)
      sweep = std::fmod(sweep, 360.0) - 360;
    sweep = std::max(sweep, -360.0);
  } else {
    if (sweep < 0)
      sweep = std::fmod(sweep, 360.0) + 360;
    sweep = std::min(sweep, 360.0);
  }
  const int parts = std::max(1, static_cast<int>(std::ceil(std::abs(sweep) / 90)));
  const double step = sweep / parts * kDegree;
  // A quarter of a circle or less as a cubic.
  const double k = 4.0 / 3 * std::tan(step / 4) * radius;
  double angle = start * kDegree;
  const double first_x = x + radius * std::cos(angle);
  const double first_y = y + radius * std::sin(angle);
  if (c->has_point)
    zebkit_canvas_line_to(c, first_x, first_y);
  else
    zebkit_canvas_move_to(c, first_x, first_y);
  for (int i = 0; i < parts; ++i) {
    const double next = angle + step;
    const double x0 = x + radius * std::cos(angle), y0 = y + radius * std::sin(angle);
    const double x1 = x + radius * std::cos(next), y1 = y + radius * std::sin(next);
    zebkit_canvas_curve_to(c, x0 - k * std::sin(angle), y0 + k * std::cos(angle),
                           x1 + k * std::sin(next), y1 - k * std::cos(next), x1, y1);
    angle = next;
  }
}

void zebkit_canvas_rect(zebkit_canvas* c, double x, double y, double width, double height) {
  if (!c || !Finite(x, y, width, height))
    return;
  zebkit_canvas_move_to(c, x, y);
  zebkit_canvas_line_to(c, x + width, y);
  zebkit_canvas_line_to(c, x + width, y + height);
  zebkit_canvas_line_to(c, x, y + height);
  zebkit_canvas_close_path(c);
}

void zebkit_canvas_close_path(zebkit_canvas* c) {
  if (!c || !c->has_point)
    return;
  Add(c, Segment::kClose, 0, 0);
  c->point_x = c->start_x;
  c->point_y = c->start_y;
}

void zebkit_canvas_fill(zebkit_canvas* c, bool even_odd) {
  if (c && !c->path.empty())
    c->backend->Fill(c->path, even_odd, c->state);
}

void zebkit_canvas_stroke(zebkit_canvas* c) {
  if (c && !c->path.empty())
    c->backend->Stroke(c->path, c->state);
}

void zebkit_canvas_clip(zebkit_canvas* c, bool even_odd) {
  if (c && !c->path.empty())
    c->backend->Clip(c->path, even_odd);
}

void zebkit_canvas_clear(zebkit_canvas* c, uint32_t rgba) {
  if (c)
    c->backend->Clear(rgba, c->pdf_width, c->pdf_height);
}

bool zebkit_canvas_draw_image(zebkit_canvas* c, const uint8_t* rgba, int width,
                              int height, double x, double y, double dest_width,
                              double dest_height) {
  if (!c || !rgba || width <= 0 || height <= 0 || width > (1 << 14) ||
      height > (1 << 14) || !Finite(x, y, dest_width, dest_height)) {
    return false;
  }
  // The unit square, its y 1 at the top of the destination.
  Matrix place = c->y_up ? Matrix{dest_width, 0, 0, dest_height, x, y}
                         : Matrix{dest_width, 0, 0, -dest_height, x, y + dest_height};
  c->backend->Image(rgba, width, height, c->state.matrix.Then(place), c->state);
  return true;
}

zebkit_canvas_font* zebkit_canvas_font_new(const void* data, size_t size, char** error) {
  if (!data || !size || size > (256u << 20)) {
    Failed(error, "no font, or one over 256 MiB");
    return nullptr;
  }
  auto font = std::make_shared<Font>(data, size);
  if (!font->typeface) {
    Failed(error, "not a font");
    return nullptr;
  }
  return new zebkit_canvas_font{std::move(font)};
}

void zebkit_canvas_font_close(zebkit_canvas_font* font) {
  delete font;
}

void zebkit_canvas_set_font(zebkit_canvas* c, zebkit_canvas_font* font, double size) {
  if (!c || !std::isfinite(size) || size <= 0 || size > 100000)
    return;
  c->state.font = font ? font->font : nullptr;
  c->state.font_size = size;
}

bool zebkit_canvas_fill_text(zebkit_canvas* c, const char* text, double x, double y, char** error) {
  if (!c) {
    Failed(error, "no canvas");
    return false;
  }
  Path outlines;
  if (!TextOutlines(c, text, x, y, &outlines, error))
    return false;
  if (!outlines.empty())
    c->backend->Fill(outlines, false, c->state);
  return true;
}

bool zebkit_canvas_text_path(zebkit_canvas* c, const char* text, double x, double y, char** error) {
  if (!c) {
    Failed(error, "no canvas");
    return false;
  }
  return TextOutlines(c, text, x, y, &c->path, error);
}

double zebkit_canvas_measure_text(zebkit_canvas* c, const char* text) {
  if (!c || !text || !c->state.font)
    return -1;
  zebkit::ShapedText shaped;
  std::string problem;
  if (!zebkit::Shape(c->state.font->face, c->state.font_size, text, nullptr,
                     nullptr, &shaped, &problem)) {
    return -1;
  }
  return shaped.advance_x;
}

bool zebkit_canvas_run(zebkit_canvas* c, const char* ops, char** error) {
  if (!c || !ops) {
    Failed(error, "no canvas or no operations");
    return false;
  }
  if (strlen(ops) > (256u << 20)) {
    Failed(error, "the operations are over 256 MiB");
    return false;
  }
  std::optional<base::Value> parsed =
      base::JSONReader::Read(ops, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!parsed || !parsed->is_list()) {
    Failed(error, "the operations are not a JSON array");
    return false;
  }
  const base::ListValue& list = parsed->GetList();
  if (list.size() > 1000000) {
    Failed(error, "over 1000000 operations");
    return false;
  }
  size_t index = 0;
  auto wrong = [&](const std::string& why) {
    Failed(error, "operation " + std::to_string(index) + ": " + why);
    return false;
  };
  for (; index < list.size(); ++index) {
    if (!list[index].is_list() || list[index].GetList().empty() ||
        !list[index].GetList()[0].is_string()) {
      return wrong("not an array with its name first");
    }
    const base::ListValue& op = list[index].GetList();
    const std::string& name = op[0].GetString();
    // The numbers at 1..n.
    double n[8] = {};
    auto numbers = [&](size_t count, size_t from = 1) {
      if (op.size() < from + count)
        return false;
      for (size_t i = 0; i < count; ++i) {
        const base::Value& value = op[from + i];
        if (!value.is_int() && !value.is_double())
          return false;
        n[i] = value.GetDouble();
      }
      return true;
    };
    auto color = [&](size_t at, uint32_t* out) {
      if (op.size() <= at || (!op[at].is_int() && !op[at].is_double()))
        return false;
      const double value = op[at].GetDouble();
      if (value < 0 || value > 4294967295.0)
        return false;
      *out = static_cast<uint32_t>(value);
      return true;
    };
    auto stops = [&](size_t at, std::vector<double>* offsets,
                     std::vector<uint32_t>* colors) {
      if (op.size() < at + 2 || !op[at].is_list() || !op[at + 1].is_list() ||
          op[at].GetList().size() != op[at + 1].GetList().size()) {
        return false;
      }
      for (const base::Value& value : op[at].GetList()) {
        if (!value.is_int() && !value.is_double())
          return false;
        offsets->push_back(value.GetDouble());
      }
      for (const base::Value& value : op[at + 1].GetList()) {
        if ((!value.is_int() && !value.is_double()) || value.GetDouble() < 0 ||
            value.GetDouble() > 4294967295.0) {
          return false;
        }
        colors->push_back(static_cast<uint32_t>(value.GetDouble()));
      }
      return true;
    };
    uint32_t rgba = 0;
    if (name == "save") {
      zebkit_canvas_save(c);
    } else if (name == "restore") {
      zebkit_canvas_restore(c);
    } else if (name == "translate") {
      if (!numbers(2)) return wrong("translate takes x, y");
      zebkit_canvas_translate(c, n[0], n[1]);
    } else if (name == "scale") {
      if (!numbers(2)) return wrong("scale takes x, y");
      zebkit_canvas_scale(c, n[0], n[1]);
    } else if (name == "rotate") {
      if (!numbers(1)) return wrong("rotate takes degrees");
      zebkit_canvas_rotate(c, n[0]);
    } else if (name == "transform" || name == "setMatrix") {
      if (!numbers(6)) return wrong(name + " takes six numbers");
      if (name == "transform")
        zebkit_canvas_transform(c, n[0], n[1], n[2], n[3], n[4], n[5]);
      else
        zebkit_canvas_set_matrix(c, n[0], n[1], n[2], n[3], n[4], n[5]);
    } else if (name == "fillColor") {
      if (!color(1, &rgba)) return wrong("a colour is 0xRRGGBBAA");
      zebkit_canvas_set_fill_color(c, rgba);
    } else if (name == "strokeColor") {
      if (!color(1, &rgba)) return wrong("a colour is 0xRRGGBBAA");
      zebkit_canvas_set_stroke_color(c, rgba);
    } else if (name == "alpha") {
      if (!numbers(1)) return wrong("alpha takes a number");
      zebkit_canvas_set_alpha(c, n[0]);
    } else if (name == "blend") {
      if (op.size() < 2 || !op[1].is_string() ||
          !zebkit_canvas_set_blend(c, op[1].GetString().c_str())) {
        return wrong("no such blend mode");
      }
    } else if (name == "lineWidth") {
      if (!numbers(1)) return wrong("lineWidth takes a number");
      zebkit_canvas_set_line_width(c, n[0]);
    } else if (name == "lineCap") {
      if (!numbers(1)) return wrong("lineCap takes 0-2");
      zebkit_canvas_set_line_cap(c, static_cast<zebkit_canvas_cap>(n[0]));
    } else if (name == "lineJoin") {
      if (!numbers(1)) return wrong("lineJoin takes 0-2");
      zebkit_canvas_set_line_join(c, static_cast<zebkit_canvas_join>(n[0]));
    } else if (name == "miterLimit") {
      if (!numbers(1)) return wrong("miterLimit takes a number");
      zebkit_canvas_set_miter_limit(c, n[0]);
    } else if (name == "dash") {
      std::vector<double> lengths;
      if (op.size() < 2 || !op[1].is_list()) return wrong("dash takes [lengths], phase");
      for (const base::Value& value : op[1].GetList()) {
        if (!value.is_int() && !value.is_double()) return wrong("a dash length is a number");
        lengths.push_back(value.GetDouble());
      }
      zebkit_canvas_set_dash(c, lengths.data(), static_cast<int>(lengths.size()),
                             numbers(1, 2) ? n[0] : 0);
    } else if (name == "linearGradient") {
      std::vector<double> offsets;
      std::vector<uint32_t> colors;
      if (!numbers(4) || !stops(5, &offsets, &colors) ||
          !zebkit_canvas_set_linear_gradient(c, n[0], n[1], n[2], n[3], offsets.data(),
                                             colors.data(), static_cast<int>(offsets.size()))) {
        return wrong("a gradient's stops go up from 0 to 1, 2 to 256 of them");
      }
    } else if (name == "radialGradient") {
      std::vector<double> offsets;
      std::vector<uint32_t> colors;
      if (!numbers(3) || !stops(4, &offsets, &colors) ||
          !zebkit_canvas_set_radial_gradient(c, n[0], n[1], n[2], offsets.data(), colors.data(),
                                             static_cast<int>(offsets.size()))) {
        return wrong("a gradient's stops go up from 0 to 1, 2 to 256 of them");
      }
    } else if (name == "beginPath") {
      zebkit_canvas_begin_path(c);
    } else if (name == "moveTo") {
      if (!numbers(2)) return wrong("moveTo takes x, y");
      zebkit_canvas_move_to(c, n[0], n[1]);
    } else if (name == "lineTo") {
      if (!numbers(2)) return wrong("lineTo takes x, y");
      zebkit_canvas_line_to(c, n[0], n[1]);
    } else if (name == "quadTo") {
      if (!numbers(4)) return wrong("quadTo takes four numbers");
      zebkit_canvas_quad_to(c, n[0], n[1], n[2], n[3]);
    } else if (name == "curveTo") {
      if (!numbers(6)) return wrong("curveTo takes six numbers");
      zebkit_canvas_curve_to(c, n[0], n[1], n[2], n[3], n[4], n[5]);
    } else if (name == "arc") {
      if (!numbers(5)) return wrong("arc takes x, y, radius, start, end");
      zebkit_canvas_arc(c, n[0], n[1], n[2], n[3], n[4],
                        op.size() > 6 && op[6].is_bool() && op[6].GetBool());
    } else if (name == "rect") {
      if (!numbers(4)) return wrong("rect takes x, y, width, height");
      zebkit_canvas_rect(c, n[0], n[1], n[2], n[3]);
    } else if (name == "closePath") {
      zebkit_canvas_close_path(c);
    } else if (name == "fill") {
      zebkit_canvas_fill(c, op.size() > 1 && op[1].is_bool() && op[1].GetBool());
    } else if (name == "stroke") {
      zebkit_canvas_stroke(c);
    } else if (name == "clip") {
      zebkit_canvas_clip(c, op.size() > 1 && op[1].is_bool() && op[1].GetBool());
    } else if (name == "clear") {
      if (!color(1, &rgba)) return wrong("a colour is 0xRRGGBBAA");
      zebkit_canvas_clear(c, rgba);
    } else if (name == "image") {
      std::string pixels;
      if (op.size() < 8 || !op[1].is_string() || !numbers(6, 2) ||
          !base::Base64Decode(op[1].GetString(), &pixels)) {
        return wrong("image takes base64, width, height, x, y, w, h");
      }
      const int width = static_cast<int>(n[0]), height = static_cast<int>(n[1]);
      if (width <= 0 || height <= 0 || width > (1 << 14) || height > (1 << 14) ||
          pixels.size() != static_cast<size_t>(width) * height * 4 ||
          !zebkit_canvas_draw_image(c, reinterpret_cast<const uint8_t*>(pixels.data()),
                                    width, height, n[2], n[3], n[4], n[5])) {
        return wrong("the image is not width * height * 4 bytes");
      }
    } else if (name == "font") {
      std::string bytes;
      if (!numbers(1) || op.size() < 3 || !op[2].is_string() ||
          !base::Base64Decode(op[2].GetString(), &bytes) || bytes.empty()) {
        return wrong("font takes an id and base64");
      }
      auto font = std::make_shared<Font>(bytes.data(), bytes.size());
      if (!font->typeface)
        return wrong("not a font");
      c->run_fonts[static_cast<int>(n[0])] = std::move(font);
    } else if (name == "setFont") {
      if (!numbers(2)) return wrong("setFont takes an id and a size");
      auto font = c->run_fonts.find(static_cast<int>(n[0]));
      if (font == c->run_fonts.end() || !std::isfinite(n[1]) || n[1] <= 0 || n[1] > 100000)
        return wrong("no such font, or a size that is not above 0");
      c->state.font = font->second;
      c->state.font_size = n[1];
    } else if (name == "fillText" || name == "textPath") {
      if (op.size() < 4 || !op[1].is_string() || !numbers(2, 2))
        return wrong(name + " takes text, x, y");
      char* failure = nullptr;
      const bool ok = name == "fillText"
          ? zebkit_canvas_fill_text(c, op[1].GetString().c_str(), n[0], n[1], &failure)
          : zebkit_canvas_text_path(c, op[1].GetString().c_str(), n[0], n[1], &failure);
      if (!ok) {
        const std::string why = failure ? failure : "the text cannot be drawn";
        free(failure);
        return wrong(why);
      }
    } else {
      return wrong("no operation " + name);
    }
  }
  return true;
}

}  // extern "C"
