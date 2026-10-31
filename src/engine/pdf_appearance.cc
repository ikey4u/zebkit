// Appearance streams, drawn from an annotation's dictionary as the PDF
// reference describes them, and the dictionary work that needs PDFium's own
// classes. Nothing of base here (pdf_internal.h). Every stream is a form
// XObject whose /BBox is the annotation's /Rect and whose content is in page
// space, as PDFium's own generator writes them; a rotation is a cm in the
// content, and the /Rect grows to what it covers.

#ifdef UNSAFE_BUFFERS_BUILD
// The C API takes and returns C strings and arrays, as PDFium's does.
#pragma allow_unsafe_buffers
#endif

#include "zebkit/engine/pdf_appearance.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

#include "core/fpdfapi/edit/cpdf_npagetooneexporter.h"
#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/page/cpdf_annotcontext.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/parser/cfdf_document.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_boolean.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_null.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/fx_coordinates.h"
#include "core/fxcrt/widestring.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "public/fpdf_edit.h"
#include "public/fpdf_ppo.h"
#include "third_party/libxml/src/include/libxml/parser.h"
#include "third_party/libxml/src/include/libxml/tree.h"
#include "zebkit/engine/canvas_internal.h"

namespace zebkit::pdf {

namespace {

constexpr char kOwnKey[] = "ZebkitOwnAppearance";
constexpr char kBoxKey[] = "ZebkitBox";
constexpr char kStampTextKey[] = "ZebkitStampText";

RetainPtr<CPDF_Dictionary> Dict(FPDF_ANNOTATION annot) {
  CPDF_AnnotContext* context = CPDFAnnotContextFromFPDFAnnotation(annot);
  return context ? context->GetMutableAnnotDict() : nullptr;
}

void Num(std::string& out, double value) {
  if (!std::isfinite(value))
    value = 0;
  value = std::clamp(value, -1e7, 1e7);
  char buffer[32];
  char* end = std::to_chars(buffer, buffer + sizeof(buffer), value,
                            std::chars_format::fixed, 3)
                  .ptr;
  std::string_view text(buffer, static_cast<size_t>(end - buffer));
  while (text.back() == '0')
    text.remove_suffix(1);
  if (text.back() == '.')
    text.remove_suffix(1);
  out.append(text);
  out.push_back(' ');
}

struct Rgb {
  double r = 0, g = 0, b = 0;
};

std::optional<Rgb> ColorOf(const CPDF_Dictionary& dict, const char* key) {
  RetainPtr<const CPDF_Array> c = dict.GetArrayFor(key);
  if (!c)
    return std::nullopt;
  if (c->size() == 1) {
    const double g = c->GetFloatAt(0);
    return Rgb{g, g, g};
  }
  if (c->size() == 3)
    return Rgb{c->GetFloatAt(0), c->GetFloatAt(1), c->GetFloatAt(2)};
  if (c->size() == 4) {
    const double k = c->GetFloatAt(3);
    return Rgb{(1 - c->GetFloatAt(0)) * (1 - k), (1 - c->GetFloatAt(1)) * (1 - k),
               (1 - c->GetFloatAt(2)) * (1 - k)};
  }
  return std::nullopt;
}

struct Box {
  double l = 0, b = 0, r = 0, t = 0;
  double width() const { return r - l; }
  double height() const { return t - b; }
};

Box RectOf(const CPDF_Dictionary& dict, const char* key) {
  CFX_FloatRect rect = dict.GetRectFor(key);
  rect.Normalize();
  return {rect.left, rect.bottom, rect.right, rect.top};
}

// What covers some points.
struct Bounds {
  bool any = false;
  Box box;
  void Add(double x, double y) {
    if (!any) {
      box = {x, y, x, y};
      any = true;
      return;
    }
    box.l = std::min(box.l, x);
    box.b = std::min(box.b, y);
    box.r = std::max(box.r, x);
    box.t = std::max(box.t, y);
  }
  void Inflate(double by) {
    box.l -= by;
    box.b -= by;
    box.r += by;
    box.t += by;
  }
};

double BorderWidth(const CPDF_Dictionary& dict, double fallback) {
  if (RetainPtr<const CPDF_Dictionary> bs = dict.GetDictFor("BS")) {
    if (bs->KeyExist("W"))
      return std::clamp<double>(bs->GetFloatFor("W"), 0, 1000);
    return 1;
  }
  if (RetainPtr<const CPDF_Array> border = dict.GetArrayFor("Border")) {
    if (border->size() >= 3)
      return std::clamp<double>(border->GetFloatAt(2), 0, 1000);
  }
  return fallback;
}

std::string DashOperator(const CPDF_Dictionary& dict) {
  RetainPtr<const CPDF_Array> dash;
  if (RetainPtr<const CPDF_Dictionary> bs = dict.GetDictFor("BS")) {
    if (bs->GetByteStringFor("S") != "D")
      return std::string();
    dash = bs->GetArrayFor("D");
    if (!dash || dash->IsEmpty())
      return "[3] 0 d\n";
  } else if (RetainPtr<const CPDF_Array> border = dict.GetArrayFor("Border")) {
    if (border->size() >= 4)
      dash = border->GetArrayAt(3);
  }
  if (!dash || dash->IsEmpty())
    return std::string();
  std::string out = "[";
  double total = 0;
  for (size_t i = 0; i < dash->size() && i < 32; ++i) {
    const double length = std::max<double>(dash->GetFloatAt(i), 0);
    total += length;
    Num(out, length);
  }
  if (total <= 0)
    return std::string();
  return out + "] 0 d\n";
}

// The stream's resources as it is written, in the annotation's document.
struct Resources {
  RetainPtr<CPDF_Dictionary> states, fonts, xobjects, shadings;
};

void WriteAppearance(CPDF_Document* document,
                     CPDF_Dictionary* annot,
                     const std::string& content,
                     const Box& bbox,
                     const Resources& resources) {
  auto stream_dict = document->New<CPDF_Dictionary>();
  stream_dict->SetNewFor<CPDF_Name>("Type", "XObject");
  stream_dict->SetNewFor<CPDF_Name>("Subtype", "Form");
  stream_dict->SetNewFor<CPDF_Number>("FormType", 1);
  stream_dict->SetMatrixFor("Matrix", CFX_Matrix());
  stream_dict->SetRectFor(
      "BBox", CFX_FloatRect(static_cast<float>(bbox.l), static_cast<float>(bbox.b),
                            static_cast<float>(bbox.r), static_cast<float>(bbox.t)));
  auto res = stream_dict->SetNewFor<CPDF_Dictionary>("Resources");
  if (resources.states)
    res->SetFor("ExtGState", resources.states);
  if (resources.fonts)
    res->SetFor("Font", resources.fonts);
  if (resources.xobjects)
    res->SetFor("XObject", resources.xobjects);
  if (resources.shadings)
    res->SetFor("Shading", resources.shadings);
  auto stream = document->NewIndirect<CPDF_Stream>(std::move(stream_dict));
  stream->SetDataAndRemoveFilter(pdfium::as_byte_span(content));
  auto ap = annot->SetNewFor<CPDF_Dictionary>("AP");
  ap->SetNewFor<CPDF_Reference>("N", document, stream->GetObjNum());
  annot->RemoveFor("PDFIUM_HasGeneratedAP");
}

// The box an annotation was given, before a rotation or a callout made its
// /Rect larger.
Box GivenBox(const CPDF_Dictionary& dict) {
  if (dict.KeyExist(kBoxKey))
    return RectOf(dict, kBoxKey);
  return RectOf(dict, "Rect");
}

double RotationOf(const CPDF_Dictionary& dict) {
  const double degrees = dict.GetFloatFor("Rotate");
  return std::isfinite(degrees) ? std::fmod(degrees, 360.0) : 0;
}

// A rotation by @degrees, counterclockwise as PDF has it, about the box's centre.
struct Rotation {
  double a = 1, b = 0, e = 0, f = 0;
  explicit Rotation(double degrees, const Box& box) {
    if (degrees == 0)
      return;
    const double radians = degrees * std::numbers::pi / 180;
    a = std::cos(radians);
    b = std::sin(radians);
    const double cx = (box.l + box.r) / 2, cy = (box.b + box.t) / 2;
    e = cx - a * cx + b * cy;
    f = cy - b * cx - a * cy;
  }
  void Write(std::string& out) const {
    for (double value : {a, b, -b, a, e, f})
      Num(out, value);
    out += "cm\n";
  }
  void Add(Bounds& bounds, const Box& box) const {
    for (double x : {box.l, box.r}) {
      for (double y : {box.b, box.t})
        bounds.Add(a * x - b * y + e, b * x + a * y + f);
    }
  }
};

// What every generated appearance starts with: the opacity and the blend.
void WriteState(CPDF_Document* document,
                const CPDF_Dictionary& annot,
                std::string& out,
                Resources& resources) {
  const double opacity =
      annot.KeyExist("CA") ? std::clamp<double>(annot.GetFloatFor("CA"), 0, 1) : 1;
  const ByteString blend = annot.GetByteStringFor("BM");
  if (opacity >= 1 && (blend.IsEmpty() || blend == "Normal"))
    return;
  auto state = document->New<CPDF_Dictionary>();
  state->SetNewFor<CPDF_Name>("Type", "ExtGState");
  state->SetNewFor<CPDF_Number>("CA", static_cast<float>(opacity));
  state->SetNewFor<CPDF_Number>("ca", static_cast<float>(opacity));
  state->SetNewFor<CPDF_Name>("BM", blend.IsEmpty() ? ByteString("Normal") : blend);
  if (!resources.states)
    resources.states = document->New<CPDF_Dictionary>();
  resources.states->SetFor("GS", std::move(state));
  out += "/GS gs\n";
}

void FillColor(std::string& out, const Rgb& c) {
  Num(out, c.r);
  Num(out, c.g);
  Num(out, c.b);
  out += "rg\n";
}

void StrokeColor(std::string& out, const Rgb& c) {
  Num(out, c.r);
  Num(out, c.g);
  Num(out, c.b);
  out += "RG\n";
}

void Move(std::string& out, double x, double y) {
  Num(out, x);
  Num(out, y);
  out += "m\n";
}

void Line(std::string& out, double x, double y) {
  Num(out, x);
  Num(out, y);
  out += "l\n";
}

void Curve(std::string& out, double a, double b, double c, double d, double e, double f) {
  for (double value : {a, b, c, d, e, f})
    Num(out, value);
  out += "c\n";
}

void Ellipse(std::string& out, double cx, double cy, double rx, double ry) {
  constexpr double k = 0.5522847498;
  Move(out, cx + rx, cy);
  Curve(out, cx + rx, cy + k * ry, cx + k * rx, cy + ry, cx, cy + ry);
  Curve(out, cx - k * rx, cy + ry, cx - rx, cy + k * ry, cx - rx, cy);
  Curve(out, cx - rx, cy - k * ry, cx - k * rx, cy - ry, cx, cy - ry);
  Curve(out, cx + k * rx, cy - ry, cx + rx, cy - k * ry, cx + rx, cy);
  out += "h\n";
}

void Rectangle(std::string& out, double x, double y, double w, double h) {
  Num(out, x);
  Num(out, y);
  Num(out, w);
  Num(out, h);
  out += "re\n";
}

void RoundedRectangle(std::string& out, const Box& box, double radius) {
  constexpr double k = 0.5522847498;
  const double r = std::min({radius, box.width() / 2, box.height() / 2});
  Move(out, box.l + r, box.b);
  Line(out, box.r - r, box.b);
  Curve(out, box.r - r + k * r, box.b, box.r, box.b + r - k * r, box.r, box.b + r);
  Line(out, box.r, box.t - r);
  Curve(out, box.r, box.t - r + k * r, box.r - r + k * r, box.t, box.r - r, box.t);
  Line(out, box.l + r, box.t);
  Curve(out, box.l + r - k * r, box.t, box.l, box.t - r + k * r, box.l, box.t - r);
  Line(out, box.l, box.b + r);
  Curve(out, box.l, box.b + r - k * r, box.l + r - k * r, box.b, box.l + r, box.b);
  out += "h\n";
}

using Point = std::pair<double, double>;

// A number in text, whatever the locale; what does not start as one is 0.
double ToNumber(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '+'))
    text.remove_prefix(1);
  double value = 0;
  std::from_chars(text.data(), text.data() + text.size(), value);
  return std::isfinite(value) ? value : 0;
}

// A cloudy border (/BE /S /C): bumps along the closed polygon, outward. The
// reference leaves their shape to the writer: this is what Acrobat's look like.
void CloudPath(std::string& out, const std::vector<Point>& polygon, double intensity) {
  if (polygon.size() < 3)
    return;
  double area = 0;
  for (size_t i = 0; i < polygon.size(); ++i) {
    const Point& p = polygon[i];
    const Point& q = polygon[(i + 1) % polygon.size()];
    area += p.first * q.second - q.first * p.second;
  }
  const double outward = area >= 0 ? -1 : 1;
  const double bump = 6 * std::clamp(intensity, 0.5, 2.0);
  Move(out, polygon[0].first, polygon[0].second);
  int written = 0;
  for (size_t i = 0; i < polygon.size(); ++i) {
    const Point& p = polygon[i];
    const Point& q = polygon[(i + 1) % polygon.size()];
    const double dx = q.first - p.first, dy = q.second - p.second;
    const double length = std::hypot(dx, dy);
    if (length <= 0)
      continue;
    const int count = std::clamp(static_cast<int>(std::round(length / bump)), 1, 256);
    const double nx = -dy / length * outward, ny = dx / length * outward;
    const double height = std::min(length / count, bump) * 0.66;
    for (int k = 0; k < count && written < 8192; ++k, ++written) {
      const double x0 = p.first + dx * k / count, y0 = p.second + dy * k / count;
      const double x1 = p.first + dx * (k + 1) / count, y1 = p.second + dy * (k + 1) / count;
      Curve(out, x0 + nx * height, y0 + ny * height, x1 + nx * height, y1 + ny * height, x1, y1);
    }
  }
  out += "h\n";
}

bool IsCloudy(const CPDF_Dictionary& dict, double* intensity) {
  RetainPtr<const CPDF_Dictionary> effect = dict.GetDictFor("BE");
  if (!effect || effect->GetByteStringFor("S") != "C")
    return false;
  *intensity = effect->KeyExist("I") ? effect->GetFloatFor("I") : 1;
  return true;
}

// ---- Text ----

// A font of a stream's resources: a standard one, or one the application gave.
struct TextFont {
  RetainPtr<CPDF_Font> font;
  std::string resource;
  bool embedded = false;
};

class Fonts {
 public:
  Fonts(CPDF_Document* document, AppearanceFonts& table, Resources& resources)
      : document_(document), table_(table), resources_(resources) {}

  const TextFont* Get(const std::string& family, bool bold, bool italic) {
    const std::string key = family + (bold ? "|b" : "|") + (italic ? "i" : "");
    auto found = used_.find(key);
    if (found != used_.end())
      return &found->second;
    TextFont text_font;
    auto bytes = table_.bytes.find(family);
    if (bytes != table_.bytes.end()) {
      FPDF_FONT& loaded = table_.loaded[family];
      if (!loaded) {
        const auto& data = bytes->second;
        loaded = FPDFText_LoadFont(FPDFDocumentFromCPDFDocument(document_), data.data(),
                                   static_cast<uint32_t>(data.size()), FPDF_FONT_TRUETYPE,
                                   /*cid=*/true);
      }
      if (loaded) {
        text_font.font.Reset(CPDFFontFromFPDFFont(loaded));
        text_font.embedded = true;
      }
    }
    if (!text_font.font)
      text_font.font = CPDF_Font::GetStockFont(document_, StandardName(family, bold, italic));
    if (!text_font.font)
      return nullptr;
    text_font.resource = "F" + std::to_string(used_.size());
    if (!resources_.fonts)
      resources_.fonts = document_->New<CPDF_Dictionary>();
    // A standard font's dictionary is PDFium's own, no object of the file:
    // the stream has a copy, or other readers find no font in it.
    if (const uint32_t number = text_font.font->GetFontDictObjNum()) {
      resources_.fonts->SetNewFor<CPDF_Reference>(text_font.resource.c_str(), document_, number);
    } else {
      resources_.fonts->SetFor(text_font.resource.c_str(), text_font.font->GetMutableFontDict()->Clone());
    }
    return &(used_[key] = std::move(text_font));
  }

  static const char* StandardName(const std::string& family, bool bold, bool italic) {
    std::string lower;
    for (char c : family)
      lower.push_back(static_cast<char>(tolower(static_cast<unsigned char>(c))));
    if (lower.find("bold") != std::string::npos)
      bold = true;
    if (lower.find("italic") != std::string::npos || lower.find("oblique") != std::string::npos)
      italic = true;
    if (lower.find("tim") != std::string::npos ||
        (lower.find("serif") != std::string::npos && lower.find("sans") == std::string::npos)) {
      return bold ? (italic ? "Times-BoldItalic" : "Times-Bold")
                  : (italic ? "Times-Italic" : "Times-Roman");
    }
    if (lower.find("cour") != std::string::npos || lower.find("mono") != std::string::npos)
      return bold ? (italic ? "Courier-BoldOblique" : "Courier-Bold")
                  : (italic ? "Courier-Oblique" : "Courier");
    return bold ? (italic ? "Helvetica-BoldOblique" : "Helvetica-Bold")
                : (italic ? "Helvetica-Oblique" : "Helvetica");
  }

 private:
  CPDF_Document* document_;
  AppearanceFonts& table_;
  Resources& resources_;
  std::map<std::string, TextFont> used_;
};

bool IsStandardFamily(const std::string& family) {
  std::string lower;
  for (char c : family)
    lower.push_back(static_cast<char>(tolower(static_cast<unsigned char>(c))));
  for (const char* name : {"helv", "arial", "sans", "tim", "serif", "cour", "mono", "zadb", "symb"}) {
    if (lower.find(name) != std::string::npos)
      return true;
  }
  return false;
}

// The encoded bytes of @text and its width at size 1. What the font does not
// have is a question mark.
struct Encoded {
  std::string hex;
  double width = 0;
};

Encoded Encode(const TextFont& font, const WideString& text) {
  Encoded out;
  ByteString bytes;
  for (size_t i = 0; i < text.GetLength(); ++i) {
    uint32_t code = font.font->CharCodeFromUnicode(text[i]);
    if (code == 0 || code == static_cast<uint32_t>(-1))
      code = font.font->CharCodeFromUnicode(L'?');
    if (code == static_cast<uint32_t>(-1))
      continue;
    font.font->AppendChar(&bytes, code);
    out.width += font.font->GetCharWidth(code) / 1000.0;
  }
  static const char kHex[] = "0123456789ABCDEF";
  for (size_t i = 0; i < bytes.GetLength(); ++i) {
    const uint8_t byte = static_cast<uint8_t>(bytes[i]);
    out.hex.push_back(kHex[byte >> 4]);
    out.hex.push_back(kHex[byte & 15]);
  }
  return out;
}

void WriteText(std::string& out,
               const TextFont& font,
               const Encoded& text,
               double size,
               const Rgb& color,
               double x,
               double y) {
  out += "BT\n";
  FillColor(out, color);
  out += "/" + font.resource + " ";
  Num(out, size);
  out += "Tf\n";
  Num(out, x);
  Num(out, y);
  out += "Td\n<" + text.hex + "> Tj\nET\n";
}

struct TextStyle {
  std::string family = "Helvetica";
  double size = 12;
  Rgb color;
  bool bold = false, italic = false, underline = false;
};

struct Run {
  TextStyle style;
  WideString text;
};

struct Paragraph {
  int align = 0;  // 0 left, 1 centre, 2 right
  std::vector<Run> runs;
};

// /DA: "/Helv 12 Tf 1 0 0 rg"; the stroke colour is the border's.
void ParseDefaultAppearance(const ByteString& da, TextStyle* style, std::optional<Rgb>* border) {
  std::vector<std::string> tokens;
  std::string token;
  auto flush = [&] {
    if (!token.empty())
      tokens.push_back(token);
    token.clear();
  };
  for (size_t i = 0; i < da.GetLength() && i < 4096; ++i) {
    const char c = da[i];
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
      flush();
    } else {
      token.push_back(c);
    }
  }
  flush();
  auto number = [&](size_t at) { return ToNumber(tokens[at]); };
  for (size_t i = 0; i < tokens.size(); ++i) {
    const std::string& op = tokens[i];
    if (op == "Tf" && i >= 2 && tokens[i - 2][0] == '/') {
      // A name's #xx, as zebkit_pdf_annotation_add() writes spaces.
      style->family.clear();
      const std::string& name = tokens[i - 2];
      for (size_t k = 1; k < name.size(); ++k) {
        if (name[k] == '#' && k + 2 < name.size()) {
          style->family.push_back(static_cast<char>(strtol(name.substr(k + 1, 2).c_str(), nullptr, 16)));
          k += 2;
        } else {
          style->family.push_back(name[k]);
        }
      }
      const double size = number(i - 1);
      if (size > 0 && size <= 4096)
        style->size = size;
    } else if ((op == "rg" || op == "RG") && i >= 3) {
      const Rgb c{number(i - 3), number(i - 2), number(i - 1)};
      if (op == "rg")
        style->color = c;
      else if (border)
        *border = c;
    } else if ((op == "g" || op == "G") && i >= 1) {
      const Rgb c{number(i - 1), number(i - 1), number(i - 1)};
      if (op == "g")
        style->color = c;
      else if (border)
        *border = c;
    } else if ((op == "k" || op == "K") && i >= 4) {
      const double k = number(i - 1);
      const Rgb c{(1 - number(i - 4)) * (1 - k), (1 - number(i - 3)) * (1 - k),
                  (1 - number(i - 2)) * (1 - k)};
      if (op == "k")
        style->color = c;
      else if (border)
        *border = c;
    }
  }
}

std::string Trim(std::string text) {
  const size_t first = text.find_first_not_of(" \t\r\n'\"");
  if (first == std::string::npos)
    return std::string();
  const size_t last = text.find_last_not_of(" \t\r\n'\"");
  return text.substr(first, last - first + 1);
}

// One CSS declaration list, as /DS and the style attributes of /RC have it.
void ParseStyle(const std::string& css, TextStyle* style, int* align) {
  size_t at = 0;
  while (at < css.size()) {
    size_t end = css.find(';', at);
    if (end == std::string::npos)
      end = css.size();
    const std::string declaration = css.substr(at, end - at);
    at = end + 1;
    const size_t colon = declaration.find(':');
    if (colon == std::string::npos)
      continue;
    std::string key = Trim(declaration.substr(0, colon));
    std::string value = Trim(declaration.substr(colon + 1));
    std::string lower;
    for (char c : value)
      lower.push_back(static_cast<char>(tolower(static_cast<unsigned char>(c))));
    if (key == "font-size") {
      const double size = ToNumber(value);
      if (size > 0 && size <= 4096)
        style->size = size;
    } else if (key == "font-family") {
      const size_t comma = value.find(',');
      style->family = Trim(comma == std::string::npos ? value : value.substr(0, comma));
    } else if (key == "font-weight") {
      style->bold = lower == "bold" || ToNumber(lower) >= 600;
    } else if (key == "font-style") {
      style->italic = lower == "italic" || lower == "oblique";
    } else if (key == "text-decoration") {
      style->underline = lower.find("underline") != std::string::npos;
    } else if (key == "color" && lower.size() == 7 && lower[0] == '#') {
      const unsigned long rgb = strtoul(lower.c_str() + 1, nullptr, 16);
      style->color = {((rgb >> 16) & 255) / 255.0, ((rgb >> 8) & 255) / 255.0, (rgb & 255) / 255.0};
    } else if (key == "text-align" && align) {
      *align = lower == "center" ? 1 : lower == "right" ? 2 : 0;
    } else if (key == "font") {
      // "bold italic 12pt Helvetica": the size is the part that starts with a
      // digit; what follows it is the family.
      size_t from = 0;
      while (from < value.size()) {
        size_t to = value.find(' ', from);
        if (to == std::string::npos)
          to = value.size();
        const std::string part = value.substr(from, to - from);
        from = to + 1;
        if (part == "bold")
          style->bold = true;
        else if (part == "italic" || part == "oblique")
          style->italic = true;
        else if (!part.empty() && isdigit(static_cast<unsigned char>(part[0]))) {
          const double size = ToNumber(part);
          if (size > 0 && size <= 4096)
            style->size = size;
          if (from < value.size())
            style->family = Trim(value.substr(from));
          break;
        }
      }
    }
  }
}

const char* AsChars(const xmlChar* text) {
  return reinterpret_cast<const char*>(text);
}

void WalkRichText(xmlNode* node, const TextStyle& parent, std::vector<Paragraph>& out, int depth) {
  if (depth > 32)
    return;
  for (xmlNode* child = node; child; child = child->next) {
    if (child->type == XML_TEXT_NODE || child->type == XML_CDATA_SECTION_NODE) {
      if (!child->content)
        continue;
      WideString text = WideString::FromUTF8(ByteStringView(AsChars(child->content)));
      // Line ends of the source are spaces, as in HTML.
      for (size_t i = 0; i < text.GetLength(); ++i) {
        if (text[i] == L'\n' || text[i] == L'\r' || text[i] == L'\t')
          text.SetAt(i, L' ');
      }
      if (out.empty())
        out.emplace_back();
      out.back().runs.push_back({parent, std::move(text)});
    } else if (child->type == XML_ELEMENT_NODE) {
      TextStyle style = parent;
      const std::string name = AsChars(child->name);
      int align = -1;
      const bool block = name == "p" || name == "div" || name == "body";
      if (name == "b" || name == "strong")
        style.bold = true;
      if (name == "i" || name == "em")
        style.italic = true;
      if (name == "u")
        style.underline = true;
      if (xmlChar* css = xmlGetProp(child, reinterpret_cast<const xmlChar*>("style"))) {
        ParseStyle(AsChars(css), &style, &align);
        xmlFree(css);
      }
      if (name == "br") {
        out.emplace_back();
        if (out.size() >= 2)
          out.back().align = out[out.size() - 2].align;
        continue;
      }
      if (block && name != "body") {
        out.emplace_back();
        if (align >= 0)
          out.back().align = align;
      }
      WalkRichText(child->children, style, out, depth + 1);
    }
  }
}

// /RC, the XHTML of rich text: empty when it cannot be read. No entity is
// expanded and nothing is fetched; a document with a DTD is not read at all.
std::vector<Paragraph> ParseRichText(const WideString& rc, const TextStyle& base) {
  std::vector<Paragraph> out;
  const ByteString utf8 = rc.ToUTF8();
  if (utf8.IsEmpty() || utf8.GetLength() > (4u << 20))
    return out;
  xmlDocPtr doc = xmlReadMemory(utf8.c_str(), static_cast<int>(utf8.GetLength()), nullptr, "UTF-8",
                                XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING |
                                    XML_PARSE_NO_XXE);
  if (!doc)
    return out;
  if (!doc->intSubset && !doc->extSubset)
    WalkRichText(xmlDocGetRootElement(doc), base, out, 0);
  xmlFreeDoc(doc);
  return out;
}

void GatherFamilies(const std::vector<Paragraph>& paragraphs, std::set<std::string>* into) {
  for (const Paragraph& paragraph : paragraphs) {
    for (const Run& run : paragraph.runs)
      into->insert(run.style.family);
  }
}

// ---- The types ----

struct Generator {
  CPDF_Document* document;
  CPDF_Dictionary* annot;
  AppearanceFonts& table;
  Resources resources;
  std::string out;
  Bounds bounds;
  Fonts fonts;

  Generator(CPDF_Document* d, CPDF_Dictionary* a, AppearanceFonts& t)
      : document(d), annot(a), table(t), fonts(d, t, resources) {}

  Rgb StrokeRgb() const {
    return ColorOf(*annot, "C").value_or(Rgb{0, 0, 0});
  }

  void Finish(const Box& given, bool keep_box) {
    Box bbox = bounds.box;
    if (bbox.width() <= 0 || bbox.height() <= 0) {
      bbox.r = std::max(bbox.r, bbox.l + 1);
      bbox.t = std::max(bbox.t, bbox.b + 1);
    }
    annot->SetRectFor("Rect", CFX_FloatRect(static_cast<float>(bbox.l), static_cast<float>(bbox.b),
                                            static_cast<float>(bbox.r), static_cast<float>(bbox.t)));
    if (keep_box) {
      annot->SetRectFor(kBoxKey, CFX_FloatRect(static_cast<float>(given.l), static_cast<float>(given.b),
                                               static_cast<float>(given.r), static_cast<float>(given.t)));
    } else {
      annot->RemoveFor(kBoxKey);
    }
    WriteAppearance(document, annot, out, bbox, resources);
  }
};

// The arrow, circle, ... at the end of a line, in a space whose +x points out
// of the line and whose origin is its end. Filled shapes take /IC.
void WriteEnding(std::string& out,
                 const ByteString& name,
                 double size,
                 bool has_fill,
                 Bounds* local) {
  const double s = size;
  auto closed_op = has_fill ? "B\n" : "S\n";
  if (name == "OpenArrow") {
    Move(out, -s * 1.5, s * 0.75);
    Line(out, 0, 0);
    Line(out, -s * 1.5, -s * 0.75);
    out += "S\n";
  } else if (name == "ClosedArrow") {
    Move(out, -s * 1.5, s * 0.75);
    Line(out, 0, 0);
    Line(out, -s * 1.5, -s * 0.75);
    out += "h\n";
    out += closed_op;
  } else if (name == "ROpenArrow") {
    Move(out, s * 1.5, s * 0.75);
    Line(out, 0, 0);
    Line(out, s * 1.5, -s * 0.75);
    out += "S\n";
  } else if (name == "RClosedArrow") {
    Move(out, s * 1.5, s * 0.75);
    Line(out, 0, 0);
    Line(out, s * 1.5, -s * 0.75);
    out += "h\n";
    out += closed_op;
  } else if (name == "Circle") {
    Ellipse(out, 0, 0, s * 0.6, s * 0.6);
    out += closed_op;
  } else if (name == "Square") {
    Rectangle(out, -s * 0.6, -s * 0.6, s * 1.2, s * 1.2);
    out += closed_op;
  } else if (name == "Diamond") {
    Move(out, s * 0.8, 0);
    Line(out, 0, s * 0.8);
    Line(out, -s * 0.8, 0);
    Line(out, 0, -s * 0.8);
    out += "h\n";
    out += closed_op;
  } else if (name == "Butt") {
    Move(out, 0, s * 0.75);
    Line(out, 0, -s * 0.75);
    out += "S\n";
  } else if (name == "Slash") {
    Move(out, s * 0.4, s * 0.75);
    Line(out, -s * 0.4, -s * 0.75);
    out += "S\n";
  } else {
    return;
  }
  (void)local;
}

constexpr double kEndingReach = 1.6;

double EndingSize(double width) {
  return std::max(4.0, width * 3);
}

// The ending at (x, y), pointing along (dx, dy).
void WriteEndingAt(std::string& out,
                   const ByteString& name,
                   double x,
                   double y,
                   double dx,
                   double dy,
                   double size,
                   bool has_fill,
                   Bounds& bounds) {
  if (name.IsEmpty() || name == "None")
    return;
  const double length = std::hypot(dx, dy);
  if (length <= 0)
    return;
  const double ux = dx / length, uy = dy / length;
  out += "q\n";
  for (double value : {ux, uy, -uy, ux, x, y})
    Num(out, value);
  out += "cm\n";
  WriteEnding(out, name, size, has_fill, nullptr);
  out += "Q\n";
  const double reach = size * kEndingReach;
  bounds.Add(x - reach, y - reach);
  bounds.Add(x + reach, y + reach);
}

ByteString EndingName(const CPDF_Dictionary& dict, size_t which) {
  RetainPtr<const CPDF_Array> endings = dict.GetArrayFor("LE");
  if (endings)
    return endings->GetByteStringAt(which);
  return dict.GetByteStringFor("LE");  // a free text's is one name
}

bool GenerateLine(Generator& g) {
  CPDF_Dictionary& dict = *g.annot;
  RetainPtr<const CPDF_Array> points = dict.GetArrayFor("L");
  if (!points || points->size() < 4)
    return false;
  const double x0 = points->GetFloatAt(0), y0 = points->GetFloatAt(1);
  const double x1 = points->GetFloatAt(2), y1 = points->GetFloatAt(3);
  const double length = std::hypot(x1 - x0, y1 - y0);
  if (!std::isfinite(length))
    return false;
  const double ux = length > 0 ? (x1 - x0) / length : 1;
  const double uy = length > 0 ? (y1 - y0) / length : 0;
  const double width = BorderWidth(dict, 1);
  const double ll = dict.GetFloatFor("LL");
  double lle = dict.GetFloatFor("LLE"), llo = dict.GetFloatFor("LLO");
  if (ll < 0) {
    lle = -lle;
    llo = -llo;
  }
  // Everything below is in the line's own space: (0, 0) its start, +x along it.
  auto place = [&](double x, double y, Bounds& bounds) {
    bounds.Add(x0 + ux * x - uy * y, y0 + uy * x + ux * y);
  };
  const double y = llo + ll;
  std::string local;
  std::optional<Rgb> interior = ColorOf(dict, "IC");
  StrokeColor(local, g.StrokeRgb());
  if (interior)
    FillColor(local, *interior);
  Num(local, width);
  local += "w\n" + DashOperator(dict);
  // The leader lines, from where the line is measured to past the line.
  if (ll != 0) {
    Move(local, 0, llo);
    Line(local, 0, y + lle);
    Move(local, length, llo);
    Line(local, length, y + lle);
    local += "S\n";
    place(0, llo, g.bounds);
    place(0, y + lle, g.bounds);
    place(length, llo, g.bounds);
    place(length, y + lle, g.bounds);
  }
  // The caption, in /Contents.
  const WideString caption = dict.GetUnicodeTextFor("Contents");
  const bool show_caption = dict.GetBooleanFor("Cap", false) && !caption.IsEmpty();
  double gap = 0;
  double caption_x = 0, caption_y = 0;
  const double caption_size = 10;
  Encoded encoded;
  const TextFont* font = nullptr;
  if (show_caption) {
    font = g.fonts.Get("Helvetica", false, false);
    if (font) {
      encoded = Encode(*font, caption);
      const double text_width = encoded.width * caption_size;
      double offset_x = 0, offset_y = 0;
      if (RetainPtr<const CPDF_Array> offset = dict.GetArrayFor("CO"); offset && offset->size() >= 2) {
        offset_x = offset->GetFloatAt(0);
        offset_y = offset->GetFloatAt(1);
      }
      const bool top = dict.GetByteStringFor("CP") == "Top";
      caption_x = length / 2 - text_width / 2 + offset_x;
      caption_y = top ? y + width / 2 + 2 + offset_y : y - caption_size * 0.35 + offset_y;
      if (!top && offset_y == 0)
        gap = std::min(text_width + 4, length);
      place(caption_x, caption_y - 3, g.bounds);
      place(caption_x + text_width, caption_y + caption_size, g.bounds);
    }
  }
  // The line, broken around an inline caption.
  if (gap > 0) {
    Move(local, 0, y);
    Line(local, (length - gap) / 2, y);
    Move(local, (length + gap) / 2, y);
    Line(local, length, y);
  } else {
    Move(local, 0, y);
    Line(local, length, y);
  }
  local += "S\n";
  place(0, y, g.bounds);
  place(length, y, g.bounds);
  // The endings: the start's points back along the line.
  const double size = EndingSize(width);
  Bounds ends;
  WriteEndingAt(local, EndingName(dict, 0), 0, y, -1, 0, size, interior.has_value(), ends);
  WriteEndingAt(local, EndingName(dict, 1), length, y, 1, 0, size, interior.has_value(), ends);
  if (ends.any) {
    place(ends.box.l, ends.box.b, g.bounds);
    place(ends.box.r, ends.box.b, g.bounds);
    place(ends.box.l, ends.box.t, g.bounds);
    place(ends.box.r, ends.box.t, g.bounds);
  }
  if (font && show_caption)
    WriteText(local, *font, encoded, caption_size, g.StrokeRgb(), caption_x, caption_y);
  g.bounds.Inflate(width / 2 + 1);

  g.out += "q\n";
  for (double value : {ux, uy, -uy, ux, x0, y0})
    Num(g.out, value);
  g.out += "cm\n" + local + "Q\n";
  const double rotation = RotationOf(dict);
  if (rotation != 0) {
    const Box box = g.bounds.box;
    Rotation r(rotation, box);
    std::string rotated = "q\n";
    r.Write(rotated);
    g.out = rotated + g.out + "Q\n";
    Bounds turned;
    r.Add(turned, box);
    g.bounds = turned;
  }
  g.Finish(Box(), false);
  return true;
}

// The points of a circle or an ellipse, for a cloudy border around it.
std::vector<Point> EllipsePoints(const Box& box) {
  std::vector<Point> points;
  const int count = 48;
  for (int i = 0; i < count; ++i) {
    const double angle = 2 * std::numbers::pi * i / count;
    points.emplace_back((box.l + box.r) / 2 + box.width() / 2 * std::cos(angle),
                        (box.b + box.t) / 2 + box.height() / 2 * std::sin(angle));
  }
  return points;
}

// What a measurement shows inside a shape: /Contents, at its middle.
void WriteDimension(Generator& g, const Box& area, std::string& out) {
  const ByteString intent = g.annot->GetByteStringFor("IT");
  if (intent.GetLength() < 9 || intent.Last(9) != "Dimension")
    return;
  const WideString caption = g.annot->GetUnicodeTextFor("Contents");
  if (caption.IsEmpty())
    return;
  const TextFont* font = g.fonts.Get("Helvetica", false, false);
  if (!font)
    return;
  const Encoded encoded = Encode(*font, caption);
  const double size = 10;
  WriteText(out, *font, encoded, size, g.StrokeRgb(),
            (area.l + area.r) / 2 - encoded.width * size / 2, (area.b + area.t) / 2 - size * 0.35);
}

bool GenerateShape(Generator& g, FPDF_ANNOTATION_SUBTYPE subtype) {
  CPDF_Dictionary& dict = *g.annot;
  const Box given = GivenBox(dict);
  const double width = BorderWidth(dict, 1);
  std::optional<Rgb> interior = ColorOf(dict, "IC");
  double intensity = 1;
  const bool cloudy = IsCloudy(dict, &intensity);
  if (given.width() <= 0 || given.height() <= 0)
    return false;
  if (width <= 0 && !interior)
    return false;
  // A cloud's bumps reach outward: its shape stands inside by as much.
  const double inset = width / 2 + (cloudy ? 6 * std::clamp(intensity, 0.5, 2.0) * 0.66 : 0);
  Box area = {given.l + inset, given.b + inset, given.r - inset, given.t - inset};
  if (area.width() <= 0 || area.height() <= 0)
    area = given;
  std::string local;
  if (interior)
    FillColor(local, *interior);
  StrokeColor(local, g.StrokeRgb());
  Num(local, width);
  local += "w\n" + DashOperator(dict);
  if (cloudy) {
    std::vector<Point> polygon;
    if (subtype == FPDF_ANNOT_CIRCLE) {
      polygon = EllipsePoints(area);
    } else {
      polygon = {{area.l, area.b}, {area.r, area.b}, {area.r, area.t}, {area.l, area.t}};
    }
    CloudPath(local, polygon, intensity);
    // /RD: how far the rectangle stands from the shape it holds.
    auto differences = dict.SetNewFor<CPDF_Array>("RD");
    for (int i = 0; i < 4; ++i)
      differences->AppendNew<CPDF_Number>(static_cast<float>(inset));
  } else if (subtype == FPDF_ANNOT_CIRCLE) {
    Ellipse(local, (area.l + area.r) / 2, (area.b + area.t) / 2, area.width() / 2, area.height() / 2);
    dict.RemoveFor("RD");
  } else {
    Rectangle(local, area.l, area.b, area.width(), area.height());
    dict.RemoveFor("RD");
  }
  local += interior && width > 0 ? "B\n" : interior ? "f\n" : "S\n";
  WriteDimension(g, area, local);
  const double rotation = RotationOf(dict);
  Rotation r(rotation, given);
  r.Add(g.bounds, given);
  if (rotation != 0) {
    g.out += "q\n";
    r.Write(g.out);
    g.out += local + "Q\n";
    g.bounds.Inflate(1);
  } else {
    g.out += local;
  }
  g.Finish(given, rotation != 0);
  return true;
}

bool GeneratePolygon(Generator& g, bool closed) {
  CPDF_Dictionary& dict = *g.annot;
  RetainPtr<const CPDF_Array> vertices = dict.GetArrayFor("Vertices");
  if (!vertices || vertices->size() < 4)
    return false;
  std::vector<Point> points;
  for (size_t i = 0; i + 1 < vertices->size() && points.size() < 65536; i += 2)
    points.emplace_back(vertices->GetFloatAt(i), vertices->GetFloatAt(i + 1));
  const double width = BorderWidth(dict, 1);
  std::optional<Rgb> interior = ColorOf(dict, "IC");
  double intensity = 1;
  const bool cloudy = closed && IsCloudy(dict, &intensity) && points.size() >= 3;
  std::string local;
  if (interior)
    FillColor(local, *interior);
  StrokeColor(local, g.StrokeRgb());
  Num(local, width);
  local += "w\n" + DashOperator(dict);
  for (const Point& p : points)
    g.bounds.Add(p.first, p.second);
  const Box area = g.bounds.box;
  if (cloudy) {
    CloudPath(local, points, intensity);
    g.bounds.Inflate(6 * std::clamp(intensity, 0.5, 2.0));
  } else {
    Move(local, points[0].first, points[0].second);
    for (size_t i = 1; i < points.size(); ++i)
      Line(local, points[i].first, points[i].second);
    if (closed)
      local += "h\n";
  }
  if (closed)
    local += interior && width > 0 ? "B\n" : interior ? "f\n" : "S\n";
  else
    local += "S\n";
  if (!closed && points.size() >= 2) {
    const double size = EndingSize(width);
    const Point& a = points[0];
    const Point& b = points[1];
    const Point& y = points[points.size() - 2];
    const Point& z = points[points.size() - 1];
    WriteEndingAt(local, EndingName(dict, 0), a.first, a.second, a.first - b.first, a.second - b.second,
                  size, interior.has_value(), g.bounds);
    WriteEndingAt(local, EndingName(dict, 1), z.first, z.second, z.first - y.first, z.second - y.second,
                  size, interior.has_value(), g.bounds);
  }
  WriteDimension(g, area, local);
  g.bounds.Inflate(width / 2 + 1);
  const double rotation = RotationOf(dict);
  if (rotation != 0) {
    const Box box = g.bounds.box;
    Rotation r(rotation, box);
    g.out += "q\n";
    r.Write(g.out);
    g.out += local + "Q\n";
    Bounds turned;
    r.Add(turned, box);
    g.bounds = turned;
  } else {
    g.out += local;
  }
  g.Finish(Box(), false);
  return true;
}

// Whether the text has what WinAnsi, the standard fonts' encoding, does not.
bool NeedsOtherFont(const WideString& text) {
  for (size_t i = 0; i < text.GetLength(); ++i) {
    const wchar_t c = text[i];
    if (c > 0xff && !(c >= 0x2013 && c <= 0x2122))
      return true;
  }
  return false;
}

// Word-wrapped runs inside @area, top down.
void LayOutText(Generator& g, std::vector<Paragraph>& paragraphs, const Box& area, std::string& out) {
  struct Piece {
    const TextFont* font = nullptr;
    const TextStyle* style = nullptr;
    Encoded encoded;
    double width = 0;
    bool space = false;
  };
  double y = area.t;
  for (Paragraph& paragraph : paragraphs) {
    std::vector<Piece> pieces;
    double tallest = 0;
    for (const Run& run : paragraph.runs) {
      // What the standard fonts lack, in the font given for that.
      const bool fallback = NeedsOtherFont(run.text) && !g.table.bytes.count(run.style.family) &&
                            g.table.bytes.count(std::string());
      const TextFont* font =
          g.fonts.Get(fallback ? std::string() : run.style.family, run.style.bold, run.style.italic);
      if (!font)
        continue;
      tallest = std::max(tallest, run.style.size);
      // Words and the spaces between them.
      size_t i = 0;
      const size_t n = std::min<size_t>(run.text.GetLength(), 1u << 20);
      while (i < n) {
        const bool space = run.text[i] == L' ';
        size_t j = i;
        while (j < n && (run.text[j] == L' ') == space)
          ++j;
        Piece piece;
        piece.font = font;
        piece.style = &run.style;
        piece.encoded = Encode(*font, run.text.Substr(i, j - i));
        piece.width = piece.encoded.width * run.style.size;
        piece.space = space;
        pieces.push_back(std::move(piece));
        i = j;
      }
    }
    if (tallest == 0)
      tallest = 12;
    size_t at = 0;
    do {
      // As many pieces as fit: at least one word, however wide.
      size_t end = at;
      double line_width = 0;
      double trailing = 0;
      double tallest_here = 0;
      while (end < pieces.size()) {
        if (pieces[end].space) {
          trailing += pieces[end].width;
          ++end;
          continue;
        }
        if (line_width > 0 && line_width + trailing + pieces[end].width > area.width())
          break;
        line_width += trailing + pieces[end].width;
        trailing = 0;
        tallest_here = std::max(tallest_here, pieces[end].style->size);
        ++end;
      }
      if (tallest_here == 0)
        tallest_here = tallest;
      const double baseline = y - tallest_here;
      if (baseline < area.b - tallest_here)
        return;
      double x = area.l;
      if (paragraph.align == 1)
        x += (area.width() - line_width) / 2;
      else if (paragraph.align == 2)
        x += area.width() - line_width;
      for (size_t i = at; i < end; ++i) {
        const Piece& piece = pieces[i];
        if (!piece.space) {
          WriteText(out, *piece.font, piece.encoded, piece.style->size, piece.style->color, x, baseline);
          if (piece.style->underline) {
            StrokeColor(out, piece.style->color);
            out += "0.5 w\n";
            Move(out, x, baseline - 1.5);
            Line(out, x + piece.width, baseline - 1.5);
            out += "S\n";
          }
        }
        x += piece.width;
      }
      y -= tallest_here * 1.2;
      at = end;
    } while (at < pieces.size());
  }
}

std::vector<Paragraph> FreeTextParagraphs(const CPDF_Dictionary& dict,
                                          TextStyle* base,
                                          std::optional<Rgb>* border_color) {
  ParseDefaultAppearance(dict.GetByteStringFor("DA"), base, border_color);
  if (dict.KeyExist("DS"))
    ParseStyle(dict.GetUnicodeTextFor("DS").ToUTF8().c_str(), base, nullptr);
  std::vector<Paragraph> paragraphs;
  if (dict.KeyExist("RC"))
    paragraphs = ParseRichText(dict.GetUnicodeTextFor("RC"), *base);
  if (paragraphs.empty()) {
    const WideString contents = dict.GetUnicodeTextFor("Contents");
    size_t at = 0;
    while (at <= contents.GetLength()) {
      size_t end = at;
      while (end < contents.GetLength() && contents[end] != L'\n' && contents[end] != L'\r')
        ++end;
      Paragraph paragraph;
      paragraph.runs.push_back({*base, contents.Substr(at, end - at)});
      paragraphs.push_back(std::move(paragraph));
      if (end < contents.GetLength() && contents[end] == L'\r' && end + 1 < contents.GetLength() &&
          contents[end + 1] == L'\n') {
        ++end;
      }
      at = end + 1;
    }
  }
  // /Q holds for what says nothing of its own.
  const int q = dict.KeyExist("Q") ? dict.GetIntegerFor("Q") : 0;
  for (Paragraph& paragraph : paragraphs) {
    if (paragraph.align == 0 && q >= 0 && q <= 2)
      paragraph.align = q;
  }
  return paragraphs;
}

bool GenerateFreeText(Generator& g) {
  CPDF_Dictionary& dict = *g.annot;
  const Box given = GivenBox(dict);
  if (given.width() <= 0 || given.height() <= 0)
    return false;
  TextStyle base;
  std::optional<Rgb> border_color;
  std::vector<Paragraph> paragraphs = FreeTextParagraphs(dict, &base, &border_color);
  const double width = BorderWidth(dict, 0);
  std::optional<Rgb> background = ColorOf(dict, "C");
  const Rgb stroke = border_color.value_or(base.color);

  // The box, turned about its middle: its background, border and text.
  std::string local;
  if (background)
    FillColor(local, *background);
  if (width > 0) {
    StrokeColor(local, stroke);
    Num(local, width);
    local += "w\n" + DashOperator(dict);
  }
  double intensity = 1;
  const bool cloudy = IsCloudy(dict, &intensity);
  const double inset = width / 2 + (cloudy ? 6 * std::clamp(intensity, 0.5, 2.0) * 0.66 : 0);
  const Box shape = {given.l + inset, given.b + inset, given.r - inset, given.t - inset};
  if ((background || width > 0) && shape.width() > 0 && shape.height() > 0) {
    if (cloudy) {
      CloudPath(local, {{shape.l, shape.b}, {shape.r, shape.b}, {shape.r, shape.t}, {shape.l, shape.t}},
                intensity);
    } else {
      Rectangle(local, shape.l, shape.b, shape.width(), shape.height());
    }
    local += background && width > 0 ? "B\n" : background ? "f\n" : "S\n";
  }
  // The text, clipped to the box.
  const double padding = inset + 2;
  const Box text_area = {given.l + padding, given.b + padding, given.r - padding, given.t - padding};
  local += "q\n";
  Rectangle(local, shape.l, shape.b, std::max(shape.width(), 0.0), std::max(shape.height(), 0.0));
  local += "W n\n";
  if (text_area.width() > 0 && text_area.height() > 0)
    LayOutText(g, paragraphs, text_area, local);
  local += "Q\n";

  const double rotation = RotationOf(dict);
  Rotation r(rotation, given);
  r.Add(g.bounds, given);
  if (rotation != 0) {
    g.out += "q\n";
    r.Write(g.out);
    g.out += local + "Q\n";
  } else {
    g.out += local;
  }

  // The callout: /CL from its far end to the box, in page space. /LE at the far end.
  bool has_callout = false;
  if (RetainPtr<const CPDF_Array> callout = dict.GetArrayFor("CL");
      callout && (callout->size() == 4 || callout->size() == 6)) {
    has_callout = true;
    std::vector<Point> points;
    for (size_t i = 0; i + 1 < callout->size(); i += 2)
      points.emplace_back(callout->GetFloatAt(i), callout->GetFloatAt(i + 1));
    const double line_width = std::max(width, 1.0);
    StrokeColor(g.out, stroke);
    if (background)
      FillColor(g.out, *background);
    Num(g.out, line_width);
    g.out += "w\n";
    Move(g.out, points[0].first, points[0].second);
    for (size_t i = 1; i < points.size(); ++i)
      Line(g.out, points[i].first, points[i].second);
    g.out += "S\n";
    for (const Point& p : points)
      g.bounds.Add(p.first, p.second);
    WriteEndingAt(g.out, EndingName(dict, 0), points[0].first, points[0].second,
                  points[0].first - points[1].first, points[0].second - points[1].second,
                  EndingSize(line_width), background.has_value(), g.bounds);
    g.bounds.Inflate(line_width);
    const Box whole = g.bounds.box;
    auto differences = dict.SetNewFor<CPDF_Array>("RD");
    differences->AppendNew<CPDF_Number>(static_cast<float>(given.l - whole.l));
    differences->AppendNew<CPDF_Number>(static_cast<float>(given.b - whole.b));
    differences->AppendNew<CPDF_Number>(static_cast<float>(whole.r - given.r));
    differences->AppendNew<CPDF_Number>(static_cast<float>(whole.t - given.t));
  } else {
    dict.RemoveFor("RD");
    if (rotation != 0)
      g.bounds.Inflate(1);
  }
  g.Finish(given, rotation != 0 || has_callout);
  return true;
}

struct StandardStamp {
  const char* name;
  const char* label;
  Rgb color;
};
constexpr Rgb kStampRed = {0.8, 0.1, 0.1};
constexpr Rgb kStampGreen = {0.1, 0.55, 0.2};
constexpr Rgb kStampBlue = {0.1, 0.25, 0.7};
const StandardStamp kStandardStamps[] = {
    {"Approved", "APPROVED", kStampGreen},
    {"Experimental", "EXPERIMENTAL", kStampBlue},
    {"NotApproved", "NOT APPROVED", kStampRed},
    {"AsIs", "AS IS", kStampBlue},
    {"Expired", "EXPIRED", kStampRed},
    {"NotForPublicRelease", "NOT FOR PUBLIC RELEASE", kStampRed},
    {"Confidential", "CONFIDENTIAL", kStampRed},
    {"Final", "FINAL", kStampGreen},
    {"Sold", "SOLD", kStampBlue},
    {"Departmental", "DEPARTMENTAL", kStampBlue},
    {"ForComment", "FOR COMMENT", kStampBlue},
    {"TopSecret", "TOP SECRET", kStampRed},
    {"Draft", "DRAFT", kStampRed},
    {"ForPublicRelease", "FOR PUBLIC RELEASE", kStampGreen},
};

bool GenerateStamp(Generator& g) {
  CPDF_Dictionary& dict = *g.annot;
  const Box given = GivenBox(dict);
  if (given.width() <= 0 || given.height() <= 0)
    return false;
  WideString label = dict.GetUnicodeTextFor(kStampTextKey);
  Rgb color = kStampBlue;
  if (label.IsEmpty()) {
    const ByteString name = dict.GetByteStringFor("Name");
    if (name.IsEmpty())
      return false;  // an image, or made elsewhere
    bool known = false;
    for (const StandardStamp& stamp : kStandardStamps) {
      if (name == stamp.name) {
        label = WideString::FromUTF8(ByteStringView(stamp.label));
        color = stamp.color;
        known = true;
      }
    }
    if (!known)
      return false;
  }
  color = ColorOf(dict, "C").value_or(color);
  const TextFont* font = g.fonts.Get("Helvetica", true, false);
  if (!font)
    return false;
  std::string local;
  const double line = std::clamp(std::min(given.width(), given.height()) * 0.05, 0.5, 4.0);
  StrokeColor(local, color);
  Num(local, line);
  local += "w\n";
  const Box border = {given.l + line / 2, given.b + line / 2, given.r - line / 2, given.t - line / 2};
  RoundedRectangle(local, border, given.height() * 0.15);
  local += "S\n";
  const Encoded encoded = Encode(*font, label);
  if (encoded.width > 0) {
    const double size = std::min((given.width() - 4 * line) / encoded.width, given.height() * 0.6);
    if (size > 0) {
      WriteText(local, *font, encoded, size, color,
                (given.l + given.r) / 2 - encoded.width * size / 2,
                (given.b + given.t) / 2 - size * 0.35);
    }
  }
  const double rotation = RotationOf(dict);
  Rotation r(rotation, given);
  r.Add(g.bounds, given);
  if (rotation != 0) {
    g.out += "q\n";
    r.Write(g.out);
    g.out += local + "Q\n";
    g.bounds.Inflate(1);
  } else {
    g.out += local;
  }
  g.Finish(given, rotation != 0);
  return true;
}

bool GenerateCaret(Generator& g) {
  CPDF_Dictionary& dict = *g.annot;
  const Box given = GivenBox(dict);
  if (given.width() <= 0 || given.height() <= 0)
    return false;
  std::string local;
  FillColor(local, ColorOf(dict, "C").value_or(Rgb{0, 0, 1}));
  // A caret: two curves meeting at the top.
  const double mid = (given.l + given.r) / 2;
  Move(local, given.l, given.b);
  Curve(local, mid - given.width() * 0.1, given.b + given.height() * 0.25, mid - given.width() * 0.05,
        given.t - given.height() * 0.2, mid, given.t);
  Curve(local, mid + given.width() * 0.05, given.t - given.height() * 0.2, mid + given.width() * 0.1,
        given.b + given.height() * 0.25, given.r, given.b);
  local += "h\nf\n";
  // /Sy /P: a paragraph mark above.
  if (dict.GetByteStringFor("Sy") == "P") {
    if (const TextFont* font = g.fonts.Get("Helvetica", false, false)) {
      const Encoded encoded = Encode(*font, WideString::FromUTF8(ByteStringView("\xc2\xb6")));
      const double size = given.height() * 0.5;
      WriteText(local, *font, encoded, size, ColorOf(dict, "C").value_or(Rgb{0, 0, 1}),
                mid - encoded.width * size / 2, given.b + given.height() * 0.1);
    }
  }
  const double rotation = RotationOf(dict);
  Rotation r(rotation, given);
  r.Add(g.bounds, given);
  if (rotation != 0) {
    g.out += "q\n";
    r.Write(g.out);
    g.out += local + "Q\n";
  } else {
    g.out += local;
  }
  g.Finish(given, rotation != 0);
  return true;
}

// A note's or an attachment's icon: 20 by 20 units, scaled into the box.
void WriteIcon(std::string& out, const ByteString& name, const Rgb& color) {
  FillColor(out, color);
  StrokeColor(out, {0, 0, 0});
  out += "1 w 1 j\n";
  auto bars = [&](double x0, double x1, std::initializer_list<double> ys) {
    for (double y : ys) {
      Move(out, x0, y);
      Line(out, x1, y);
    }
    out += "S\n";
  };
  if (name == "Comment") {
    RoundedRectangle(out, {1, 6, 19, 19}, 3);
    Move(out, 5, 6);
    Line(out, 5, 1);
    Line(out, 10, 6);
    out += "B\n";
    bars(4, 16, {15, 12.5, 10});
  } else if (name == "Key") {
    Ellipse(out, 6, 13, 5, 5);
    out += "B\n";
    Move(out, 9.5, 9.5);
    Line(out, 18, 1);
    Line(out, 18, 4);
    Move(out, 15, 4);
    Line(out, 15, 7);
    out += "S\n";
  } else if (name == "Help") {
    Ellipse(out, 10, 10, 9, 9);
    out += "B\n";
    Move(out, 7, 13);
    Curve(out, 7, 17, 13, 17, 13, 13);
    Curve(out, 13, 10, 10, 10.5, 10, 7);
    Move(out, 10, 5);
    Line(out, 10, 3.5);
    out += "S\n";
  } else if (name == "NewParagraph") {
    Move(out, 10, 19);
    Line(out, 2, 8);
    Line(out, 18, 8);
    out += "h\nB\n";
    bars(4, 16, {5, 2});
  } else if (name == "Paragraph") {
    Rectangle(out, 1, 1, 18, 18);
    out += "B\n";
    Move(out, 10, 16);
    Line(out, 10, 4);
    Move(out, 14, 16);
    Line(out, 14, 4);
    Move(out, 14, 16);
    Line(out, 8, 16);
    Curve(out, 4, 16, 4, 10, 8, 10);
    Line(out, 10, 10);
    out += "S\n";
  } else if (name == "Insert") {
    Move(out, 10, 18);
    Line(out, 1, 3);
    Line(out, 19, 3);
    out += "h\nB\n";
  } else if (name == "Graph") {
    Rectangle(out, 1, 1, 18, 18);
    out += "B\n";
    Rectangle(out, 4, 3, 3, 8);
    Rectangle(out, 9, 3, 3, 12);
    Rectangle(out, 14, 3, 3, 5);
    out += "S\n";
  } else if (name == "PushPin" || name == "GraphPushPin") {
    Ellipse(out, 10, 14, 5, 5);
    out += "B\n";
    Move(out, 10, 9);
    Line(out, 10, 1);
    out += "S\n";
  } else if (name == "Paperclip" || name == "PaperclipTag") {
    Move(out, 7, 5);
    Line(out, 7, 15);
    Curve(out, 7, 19, 13, 19, 13, 15);
    Line(out, 13, 4);
    Curve(out, 13, 1, 16, 1, 16, 4);
    Line(out, 16, 12);
    out += "S\n";
  } else if (name == "Tag") {
    Move(out, 2, 10);
    Line(out, 8, 16);
    Line(out, 18, 16);
    Line(out, 18, 4);
    Line(out, 8, 4);
    out += "h\nB\n";
    Ellipse(out, 8, 10, 1.5, 1.5);
    out += "S\n";
  } else {
    // Note, and what has no drawing of its own.
    Move(out, 2, 19);
    Line(out, 14, 19);
    Line(out, 18, 15);
    Line(out, 18, 1);
    Line(out, 2, 1);
    out += "h\nB\n";
    bars(5, 15, {14, 11, 8, 5});
  }
}

bool GenerateIcon(Generator& g, const char* fallback, Rgb color) {
  CPDF_Dictionary& dict = *g.annot;
  const Box given = GivenBox(dict);
  if (given.width() <= 0 || given.height() <= 0)
    return false;
  ByteString name = dict.GetByteStringFor("Name");
  if (name.IsEmpty())
    name = fallback;
  const double scale = std::min(given.width(), given.height()) / 20;
  std::string local = "q\n";
  for (double value : {scale, 0.0, 0.0, scale, given.l, given.t - 20 * scale})
    Num(local, value);
  local += "cm\n";
  WriteIcon(local, name, ColorOf(dict, "C").value_or(color));
  local += "Q\n";
  const double rotation = RotationOf(dict);
  Rotation r(rotation, given);
  r.Add(g.bounds, given);
  if (rotation != 0) {
    g.out += "q\n";
    r.Write(g.out);
    g.out += local + "Q\n";
  } else {
    g.out += local;
  }
  g.Finish(given, rotation != 0);
  return true;
}

// After PDFium's own generator: the annotation's /BM into its state.
void ApplyBlend(CPDF_Dictionary& dict) {
  const ByteString blend = dict.GetByteStringFor("BM");
  if (blend.IsEmpty())
    return;
  RetainPtr<CPDF_Dictionary> ap = dict.GetMutableDictFor("AP");
  RetainPtr<CPDF_Stream> stream = ap ? ap->GetMutableStreamFor("N") : nullptr;
  RetainPtr<CPDF_Dictionary> stream_dict = stream ? stream->GetMutableDict() : nullptr;
  RetainPtr<CPDF_Dictionary> resources =
      stream_dict ? stream_dict->GetMutableDictFor("Resources") : nullptr;
  RetainPtr<CPDF_Dictionary> states =
      resources ? resources->GetMutableDictFor("ExtGState") : nullptr;
  if (!states)
    return;
  for (const ByteString& key : states->GetKeys()) {
    if (RetainPtr<CPDF_Dictionary> state = states->GetMutableDictFor(key.AsStringView()))
      state->SetNewFor<CPDF_Name>("BM", blend);
  }
}

// What RegenerateAppearance() does for types PDFium draws.
void RegenerateByPdfium(FPDF_DOCUMENT document, FPDF_ANNOTATION annot, CPDF_Dictionary& dict) {
  RegenerateAppearance(document, annot);
  ApplyBlend(dict);
}

}  // namespace

void CloseAppearanceFonts(AppearanceFonts& fonts) {
  for (auto& [family, font] : fonts.loaded) {
    if (font)
      FPDFFont_Close(font);
  }
  fonts.loaded.clear();
}

FPDF_ANNOTATION CreateAnnotation(FPDF_PAGE page, const char* subtype) {
  CPDF_Page* pdf_page = CPDFPageFromFPDFPage(page);
  if (!pdf_page || !subtype || !*subtype)
    return nullptr;
  CPDF_Document* document = pdf_page->GetDocument();
  auto dict = document->NewIndirect<CPDF_Dictionary>();
  dict->SetNewFor<CPDF_Name>("Type", "Annot");
  dict->SetNewFor<CPDF_Name>("Subtype", subtype);
  RetainPtr<CPDF_Array> annots = pdf_page->GetOrCreateAnnotsArray();
  annots->AppendNew<CPDF_Reference>(document, dict->GetObjNum());
  auto context = std::make_unique<CPDF_AnnotContext>(dict, IPDFPageFromFPDFPage(page));
  return FPDFAnnotationFromCPDFAnnotContext(context.release());
}

std::string AnnotationSubtype(FPDF_ANNOTATION annot) {
  RetainPtr<CPDF_Dictionary> dict = Dict(annot);
  return dict ? std::string(dict->GetByteStringFor("Subtype").c_str()) : std::string();
}

bool GenerateAppearance(FPDF_DOCUMENT document, FPDF_ANNOTATION annot, AppearanceFonts& fonts) {
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(document);
  RetainPtr<CPDF_Dictionary> dict = Dict(annot);
  if (!pdf || !dict)
    return false;
  if (dict->KeyExist(kOwnKey))
    return false;
  Generator g(pdf, dict.Get(), fonts);
  WriteState(pdf, *dict, g.out, g.resources);
  const FPDF_ANNOTATION_SUBTYPE subtype = FPDFAnnot_GetSubtype(annot);
  switch (subtype) {
    case FPDF_ANNOT_LINE:
      return GenerateLine(g);
    case FPDF_ANNOT_SQUARE:
    case FPDF_ANNOT_CIRCLE:
      return GenerateShape(g, subtype);
    case FPDF_ANNOT_POLYGON:
      return GeneratePolygon(g, true);
    case FPDF_ANNOT_POLYLINE:
      return GeneratePolygon(g, false);
    case FPDF_ANNOT_FREETEXT:
      return GenerateFreeText(g);
    case FPDF_ANNOT_STAMP:
      return GenerateStamp(g);
    case FPDF_ANNOT_CARET:
      return GenerateCaret(g);
    case FPDF_ANNOT_TEXT:
      return GenerateIcon(g, "Note", {1, 0.9, 0.3});
    case FPDF_ANNOT_FILEATTACHMENT:
      return GenerateIcon(g, "PushPin", {0.7, 0.8, 1});
    default:
      RegenerateByPdfium(document, annot, *dict);
      return true;
  }
}

std::vector<std::string> WantedFonts(FPDF_ANNOTATION annot) {
  std::vector<std::string> wanted;
  RetainPtr<CPDF_Dictionary> dict = Dict(annot);
  if (!dict || FPDFAnnot_GetSubtype(annot) != FPDF_ANNOT_FREETEXT)
    return wanted;
  TextStyle base;
  std::vector<Paragraph> paragraphs = FreeTextParagraphs(*dict, &base, nullptr);
  std::set<std::string> families;
  GatherFamilies(paragraphs, &families);
  families.insert(base.family);
  // A standard font is enough but for what it does not have: WinAnsi's.
  for (const Paragraph& paragraph : paragraphs) {
    for (const Run& run : paragraph.runs) {
      if (NeedsOtherFont(run.text) &&
          std::find(wanted.begin(), wanted.end(), std::string()) == wanted.end()) {
        wanted.push_back(std::string());
      }
    }
  }
  for (const std::string& family : families) {
    if (!IsStandardFamily(family))
      wanted.push_back(family);
  }
  return wanted;
}

void SetOwnAppearance(FPDF_ANNOTATION annot, bool own) {
  RetainPtr<CPDF_Dictionary> dict = Dict(annot);
  if (!dict)
    return;
  if (own)
    dict->SetNewFor<CPDF_Boolean>(kOwnKey, true);
  else
    dict->RemoveFor(kOwnKey);
}

bool HasOwnAppearance(FPDF_ANNOTATION annot) {
  RetainPtr<CPDF_Dictionary> dict = Dict(annot);
  return dict && dict->KeyExist(kOwnKey);
}

std::array<float, 4> AnnotationBox(FPDF_ANNOTATION annot) {
  RetainPtr<CPDF_Dictionary> dict = Dict(annot);
  if (!dict)
    return {};
  const Box box = GivenBox(*dict);
  return {static_cast<float>(box.l), static_cast<float>(box.b), static_cast<float>(box.r),
          static_cast<float>(box.t)};
}

namespace {

RetainPtr<CPDF_Stream> MakeImage(CPDF_Document* document, const PdfCanvasOutput::Image& image) {
  const size_t pixels = static_cast<size_t>(image.width) * image.height;
  DataVector<uint8_t> rgb(pixels * 3);
  DataVector<uint8_t> alpha(pixels);
  bool opaque = true;
  for (size_t i = 0; i < pixels; ++i) {
    rgb[i * 3] = image.rgba[i * 4];
    rgb[i * 3 + 1] = image.rgba[i * 4 + 1];
    rgb[i * 3 + 2] = image.rgba[i * 4 + 2];
    alpha[i] = image.rgba[i * 4 + 3];
    opaque = opaque && alpha[i] == 255;
  }
  auto make = [&](const char* space, DataVector<uint8_t> data) {
    auto dict = document->New<CPDF_Dictionary>();
    dict->SetNewFor<CPDF_Name>("Type", "XObject");
    dict->SetNewFor<CPDF_Name>("Subtype", "Image");
    dict->SetNewFor<CPDF_Number>("Width", image.width);
    dict->SetNewFor<CPDF_Number>("Height", image.height);
    dict->SetNewFor<CPDF_Name>("ColorSpace", space);
    dict->SetNewFor<CPDF_Number>("BitsPerComponent", 8);
    return document->NewIndirect<CPDF_Stream>(std::move(data), std::move(dict));
  };
  RetainPtr<CPDF_Stream> stream = make("DeviceRGB", std::move(rgb));
  if (!opaque) {
    RetainPtr<CPDF_Stream> mask = make("DeviceGray", std::move(alpha));
    stream->GetMutableDict()->SetNewFor<CPDF_Reference>("SMask", document, mask->GetObjNum());
  }
  return stream;
}

RetainPtr<CPDF_Dictionary> MakeShading(CPDF_Document* document, const PdfCanvasOutput::Shading& s) {
  auto rgb = [](uint32_t c, double out[3]) {
    out[0] = ((c >> 24) & 255) / 255.0;
    out[1] = ((c >> 16) & 255) / 255.0;
    out[2] = ((c >> 8) & 255) / 255.0;
  };
  auto dict = document->New<CPDF_Dictionary>();
  dict->SetNewFor<CPDF_Number>("ShadingType", s.radial ? 3 : 2);
  dict->SetNewFor<CPDF_Name>("ColorSpace", "DeviceRGB");
  auto coords = dict->SetNewFor<CPDF_Array>("Coords");
  if (s.radial) {
    for (double value : {s.coords[0], s.coords[1], 0.0, s.coords[0], s.coords[1], s.coords[2]})
      coords->AppendNew<CPDF_Number>(static_cast<float>(value));
  } else {
    for (int i = 0; i < 4; ++i)
      coords->AppendNew<CPDF_Number>(static_cast<float>(s.coords[i]));
  }
  auto extend = dict->SetNewFor<CPDF_Array>("Extend");
  extend->AppendNew<CPDF_Boolean>(true);
  extend->AppendNew<CPDF_Boolean>(true);
  // A stitching of exponential functions, one between each two stops.
  auto function = dict->SetNewFor<CPDF_Dictionary>("Function");
  function->SetNewFor<CPDF_Number>("FunctionType", 3);
  auto domain = function->SetNewFor<CPDF_Array>("Domain");
  domain->AppendNew<CPDF_Number>(0);
  domain->AppendNew<CPDF_Number>(1);
  auto functions = function->SetNewFor<CPDF_Array>("Functions");
  auto bounds = function->SetNewFor<CPDF_Array>("Bounds");
  auto encode = function->SetNewFor<CPDF_Array>("Encode");
  // Before the first stop and after the last, their colours.
  std::vector<double> offsets = s.offsets;
  std::vector<uint32_t> colors = s.colors;
  if (offsets.front() > 0) {
    offsets.insert(offsets.begin(), 0);
    colors.insert(colors.begin(), colors.front());
  }
  if (offsets.back() < 1) {
    offsets.push_back(1);
    colors.push_back(colors.back());
  }
  for (size_t i = 0; i + 1 < offsets.size(); ++i) {
    auto part = functions->AppendNew<CPDF_Dictionary>();
    part->SetNewFor<CPDF_Number>("FunctionType", 2);
    part->SetNewFor<CPDF_Number>("N", 1);
    auto part_domain = part->SetNewFor<CPDF_Array>("Domain");
    part_domain->AppendNew<CPDF_Number>(0);
    part_domain->AppendNew<CPDF_Number>(1);
    double from[3], to[3];
    rgb(colors[i], from);
    rgb(colors[i + 1], to);
    auto c0 = part->SetNewFor<CPDF_Array>("C0");
    auto c1 = part->SetNewFor<CPDF_Array>("C1");
    for (int k = 0; k < 3; ++k) {
      c0->AppendNew<CPDF_Number>(static_cast<float>(from[k]));
      c1->AppendNew<CPDF_Number>(static_cast<float>(to[k]));
    }
    encode->AppendNew<CPDF_Number>(0);
    encode->AppendNew<CPDF_Number>(1);
    if (i + 2 < offsets.size())
      bounds->AppendNew<CPDF_Number>(static_cast<float>(offsets[i + 1]));
  }
  return dict;
}

}  // namespace

bool SetCanvasAppearance(FPDF_DOCUMENT document,
                         FPDF_ANNOTATION annot,
                         const PdfCanvasOutput& canvas) {
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(document);
  RetainPtr<CPDF_Dictionary> dict = Dict(annot);
  if (!pdf || !dict)
    return false;
  const Box given = GivenBox(*dict);
  Resources resources;
  std::string out;
  WriteState(pdf, *dict, out, resources);
  const double rotation = RotationOf(*dict);
  Rotation r(rotation, given);
  Bounds bounds;
  r.Add(bounds, given);
  out += "q\n";
  r.Write(out);
  // The canvas's space is its box's, with its origin at the box's corner.
  out += "1 0 0 1 ";
  Num(out, given.l);
  Num(out, given.b);
  out += "cm\n";
  out += canvas.content;
  out += "Q\n";
  if (!canvas.states.empty() && !resources.states)
    resources.states = pdf->New<CPDF_Dictionary>();
  for (const auto& state : canvas.states) {
    auto gs = resources.states->SetNewFor<CPDF_Dictionary>(state.name.c_str());
    gs->SetNewFor<CPDF_Name>("Type", "ExtGState");
    gs->SetNewFor<CPDF_Number>("ca", static_cast<float>(state.fill_alpha));
    gs->SetNewFor<CPDF_Number>("CA", static_cast<float>(state.stroke_alpha));
    gs->SetNewFor<CPDF_Name>("BM", state.blend.c_str());
  }
  if (!canvas.images.empty()) {
    resources.xobjects = pdf->New<CPDF_Dictionary>();
    for (const auto& image : canvas.images) {
      resources.xobjects->SetNewFor<CPDF_Reference>(image.name.c_str(), pdf,
                                                    MakeImage(pdf, image)->GetObjNum());
    }
  }
  if (!canvas.shadings.empty()) {
    resources.shadings = pdf->New<CPDF_Dictionary>();
    for (const auto& shading : canvas.shadings)
      resources.shadings->SetFor(shading.name.c_str(), MakeShading(pdf, shading));
  }
  if (rotation != 0) {
    bounds.Inflate(1);
    dict->SetRectFor(kBoxKey, CFX_FloatRect(static_cast<float>(given.l), static_cast<float>(given.b),
                                            static_cast<float>(given.r), static_cast<float>(given.t)));
    dict->SetRectFor("Rect", CFX_FloatRect(static_cast<float>(bounds.box.l), static_cast<float>(bounds.box.b),
                                           static_cast<float>(bounds.box.r), static_cast<float>(bounds.box.t)));
  }
  WriteAppearance(pdf, dict.Get(), out, bounds.box, resources);
  dict->SetNewFor<CPDF_Boolean>(kOwnKey, true);
  return true;
}

bool SetPageAppearance(FPDF_DOCUMENT document,
                       FPDF_ANNOTATION annot,
                       FPDF_DOCUMENT source,
                       int page) {
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(document);
  RetainPtr<CPDF_Dictionary> dict = Dict(annot);
  if (!pdf || !dict)
    return false;
  FPDF_XOBJECT xobject = FPDF_NewXObjectFromPage(document, source, page);
  if (!xobject)
    return false;
  XObjectContext* context = XObjectContextFromFPDFXObject(xobject);
  RetainPtr<CPDF_Stream> stream = context->xobject;
  bool ok = false;
  if (stream) {
    const Box given = GivenBox(*dict);
    const Box source_box = RectOf(*stream->GetDict(), "BBox");
    if (given.width() > 0 && given.height() > 0 && source_box.width() > 0 &&
        source_box.height() > 0) {
      Resources resources;
      std::string out;
      WriteState(pdf, *dict, out, resources);
      const double rotation = RotationOf(*dict);
      Rotation r(rotation, given);
      Bounds bounds;
      r.Add(bounds, given);
      // The page, scaled to the box.
      const double sx = given.width() / source_box.width();
      const double sy = given.height() / source_box.height();
      out += "q\n";
      r.Write(out);
      for (double value : {sx, 0.0, 0.0, sy, given.l - source_box.l * sx, given.b - source_box.b * sy})
        Num(out, value);
      out += "cm\n/Page Do\nQ\n";
      resources.xobjects = pdf->New<CPDF_Dictionary>();
      resources.xobjects->SetNewFor<CPDF_Reference>("Page", pdf, stream->GetObjNum());
      if (rotation != 0) {
        bounds.Inflate(1);
        dict->SetRectFor(kBoxKey, CFX_FloatRect(static_cast<float>(given.l), static_cast<float>(given.b),
                                                static_cast<float>(given.r), static_cast<float>(given.t)));
        dict->SetRectFor("Rect", CFX_FloatRect(static_cast<float>(bounds.box.l), static_cast<float>(bounds.box.b),
                                               static_cast<float>(bounds.box.r), static_cast<float>(bounds.box.t)));
      }
      WriteAppearance(pdf, dict.Get(), out, bounds.box, resources);
      dict->SetNewFor<CPDF_Boolean>(kOwnKey, true);
      ok = true;
    }
  }
  FPDF_CloseXObject(xobject);
  return ok;
}

int AppearanceObject(FPDF_ANNOTATION annot) {
  RetainPtr<CPDF_Dictionary> dict = Dict(annot);
  RetainPtr<CPDF_Dictionary> ap = dict ? dict->GetMutableDictFor("AP") : nullptr;
  RetainPtr<const CPDF_Stream> stream = ap ? ap->GetStreamFor("N") : nullptr;
  return stream ? static_cast<int>(stream->GetObjNum()) : 0;
}

bool SetAppearanceObject(FPDF_DOCUMENT document, FPDF_ANNOTATION annot, int object) {
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(document);
  RetainPtr<CPDF_Dictionary> dict = Dict(annot);
  if (!pdf || !dict || object <= 0)
    return false;
  RetainPtr<const CPDF_Object> target = pdf->GetIndirectObject(static_cast<uint32_t>(object));
  if (!target || !target->IsStream())
    return false;
  auto ap = dict->SetNewFor<CPDF_Dictionary>("AP");
  ap->SetNewFor<CPDF_Reference>("N", pdf, static_cast<uint32_t>(object));
  return true;
}

int FindAnnotation(FPDF_PAGE page, int object) {
  CPDF_Page* pdf_page = CPDFPageFromFPDFPage(page);
  RetainPtr<const CPDF_Array> annots = pdf_page ? pdf_page->GetAnnotsArray() : nullptr;
  if (!annots || object <= 0)
    return -1;
  for (size_t i = 0; i < annots->size(); ++i) {
    RetainPtr<const CPDF_Object> item = annots->GetDirectObjectAt(i);
    if (item && static_cast<int>(item->GetObjNum()) == object)
      return static_cast<int>(i);
  }
  return -1;
}

int MakeAnnotationIndirect(FPDF_PAGE page, int index) {
  CPDF_Page* pdf_page = CPDFPageFromFPDFPage(page);
  RetainPtr<CPDF_Array> annots = pdf_page ? pdf_page->GetMutableAnnotsArray() : nullptr;
  if (!annots || index < 0 || static_cast<size_t>(index) >= annots->size())
    return 0;
  RetainPtr<const CPDF_Object> item = annots->GetDirectObjectAt(index);
  if (!item || !item->IsDictionary())
    return 0;
  if (!item->GetObjNum())
    annots->ConvertToIndirectObjectAt(index, pdf_page->GetDocument());
  item = annots->GetDirectObjectAt(index);
  return item ? static_cast<int>(item->GetObjNum()) : 0;
}

bool RestoreAnnotation(FPDF_PAGE page, int object, int index) {
  CPDF_Page* pdf_page = CPDFPageFromFPDFPage(page);
  if (!pdf_page || object <= 0)
    return false;
  CPDF_Document* document = pdf_page->GetDocument();
  RetainPtr<const CPDF_Object> target = document->GetIndirectObject(static_cast<uint32_t>(object));
  if (!target || !target->IsDictionary() || FindAnnotation(page, object) >= 0)
    return false;
  RetainPtr<CPDF_Array> annots = pdf_page->GetOrCreateAnnotsArray();
  const size_t at = std::min<size_t>(index < 0 ? annots->size() : static_cast<size_t>(index), annots->size());
  annots->InsertNewAt<CPDF_Reference>(at, document, static_cast<uint32_t>(object));
  return true;
}

bool MoveAnnotation(FPDF_PAGE page, int from, int to) {
  CPDF_Page* pdf_page = CPDFPageFromFPDFPage(page);
  RetainPtr<CPDF_Array> annots = pdf_page ? pdf_page->GetMutableAnnotsArray() : nullptr;
  if (!annots || from < 0 || to < 0 || static_cast<size_t>(from) >= annots->size() ||
      static_cast<size_t>(to) >= annots->size()) {
    return false;
  }
  if (from == to)
    return true;
  RetainPtr<CPDF_Object> item = annots->GetMutableObjectAt(from);
  annots->RemoveAt(from);
  annots->InsertAt(to, std::move(item));
  return true;
}

namespace {

// Copies from one document to another: what an indirect object reaches comes
// along, but pages and other annotations, which become null. Against cycles
// each object is copied once; against malice the depth and count are limited.
class Copier {
 public:
  explicit Copier(CPDF_Document* to) : to_(to) {}

  bool failed() const { return failed_; }

  void Rewrite(CPDF_Object* object, int depth) {
    if (depth > 64) {
      failed_ = true;
      return;
    }
    if (CPDF_Dictionary* dict = object->AsMutableDictionary()) {
      for (const ByteString& key : dict->GetKeys()) {
        RetainPtr<CPDF_Object> value = dict->GetMutableObjectFor(key.AsStringView());
        RetainPtr<CPDF_Object> replaced = Convert(value, depth);
        if (replaced)
          dict->SetFor(key, std::move(replaced));
      }
    } else if (CPDF_Array* array = object->AsMutableArray()) {
      for (size_t i = 0; i < array->size(); ++i) {
        RetainPtr<CPDF_Object> value = array->GetMutableObjectAt(i);
        RetainPtr<CPDF_Object> replaced = Convert(value, depth);
        if (replaced) {
          array->RemoveAt(i);
          array->InsertAt(i, std::move(replaced));
        }
      }
    } else if (CPDF_Stream* stream = object->AsMutableStream()) {
      Rewrite(stream->GetMutableDict().Get(), depth + 1);
    }
  }

 private:
  RetainPtr<CPDF_Object> Convert(const RetainPtr<CPDF_Object>& value, int depth) {
    if (!value)
      return nullptr;
    const CPDF_Reference* reference = value->AsReference();
    if (!reference) {
      Rewrite(value.Get(), depth + 1);
      return nullptr;
    }
    const uint32_t number = reference->GetRefObjNum();
    auto known = map_.find(number);
    if (known != map_.end())
      return pdfium::MakeRetain<CPDF_Reference>(to_, known->second);
    RetainPtr<const CPDF_Object> target = reference->GetDirect();
    if (!target)
      return pdfium::MakeRetain<CPDF_Null>();
    if (const CPDF_Dictionary* dict = target->AsDictionary()) {
      const ByteString type = dict->GetByteStringFor("Type");
      if (type == "Page" || type == "Pages" || type == "Annot" ||
          (dict->KeyExist("Subtype") && dict->KeyExist("Rect"))) {
        return pdfium::MakeRetain<CPDF_Null>();
      }
    }
    if (map_.size() > 100000) {
      failed_ = true;
      return pdfium::MakeRetain<CPDF_Null>();
    }
    RetainPtr<CPDF_Object> copy = target->Clone();
    if (!copy) {
      failed_ = true;
      return pdfium::MakeRetain<CPDF_Null>();
    }
    const uint32_t made = to_->AddIndirectObject(copy);
    map_[number] = made;
    Rewrite(copy.Get(), depth + 1);
    return pdfium::MakeRetain<CPDF_Reference>(to_, made);
  }

  CPDF_Document* to_;
  std::map<uint32_t, uint32_t> map_;
  bool failed_ = false;
};

}  // namespace

int CopyAnnotation(FPDF_PAGE page,
                   int index,
                   FPDF_DOCUMENT target_document,
                   FPDF_PAGE target_page,
                   std::string* problem) {
  CPDF_Page* from_page = CPDFPageFromFPDFPage(page);
  CPDF_Page* to_page = CPDFPageFromFPDFPage(target_page);
  CPDF_Document* to = CPDFDocumentFromFPDFDocument(target_document);
  RetainPtr<CPDF_Array> annots = from_page ? from_page->GetMutableAnnotsArray() : nullptr;
  if (!annots || !to_page || !to || index < 0 || static_cast<size_t>(index) >= annots->size()) {
    *problem = "no such annotation";
    return -1;
  }
  RetainPtr<const CPDF_Dictionary> source = ToDictionary(annots->GetDirectObjectAt(index));
  if (!source) {
    *problem = "no such annotation";
    return -1;
  }
  CPDF_Document* from = from_page->GetDocument();
  RetainPtr<CPDF_Dictionary> copy = ToDictionary(source->Clone());
  if (!copy) {
    *problem = "the annotation cannot be copied";
    return -1;
  }
  for (const char* key : {"P", "Popup", "Parent", "IRT"})
    copy->RemoveFor(key);
  if (from != to) {
    Copier copier(to);
    copier.Rewrite(copy.Get(), 0);
    if (copier.failed()) {
      *problem = "the annotation reaches too much to be copied";
      return -1;
    }
  }
  const uint32_t number = to->AddIndirectObject(copy);
  RetainPtr<CPDF_Array> target = to_page->GetOrCreateAnnotsArray();
  target->AppendNew<CPDF_Reference>(to, number);
  return static_cast<int>(target->size()) - 1;
}

bool FlattenAnnotation(FPDF_DOCUMENT document, FPDF_PAGE page, int index) {
  CPDF_Page* pdf_page = CPDFPageFromFPDFPage(page);
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(document);
  RetainPtr<CPDF_Array> annots = pdf_page ? pdf_page->GetMutableAnnotsArray() : nullptr;
  if (!annots || !pdf || index < 0 || static_cast<size_t>(index) >= annots->size())
    return false;
  RetainPtr<CPDF_Dictionary> annot = ToDictionary(annots->GetMutableDirectObjectAt(index));
  RetainPtr<CPDF_Dictionary> ap = annot ? annot->GetMutableDictFor("AP") : nullptr;
  RetainPtr<CPDF_Stream> stream = ap ? ap->GetMutableStreamFor("N") : nullptr;
  if (!stream) {
    // A state's appearance, for what has states.
    RetainPtr<CPDF_Dictionary> states = ap ? ap->GetMutableDictFor("N") : nullptr;
    const ByteString state = annot ? annot->GetByteStringFor("AS") : ByteString();
    if (states && !state.IsEmpty())
      stream = states->GetMutableStreamFor(state.AsStringView());
  }
  if (!stream)
    return false;
  if (stream->IsInline()) {
    stream = ToStream(stream->Clone());
    pdf->AddIndirectObject(stream);
  }
  // The stream's box, through its matrix, onto the rectangle.
  CFX_FloatRect rect = annot->GetRectFor("Rect");
  rect.Normalize();
  RetainPtr<const CPDF_Dictionary> stream_dict = stream->GetDict();
  CFX_FloatRect bbox = stream_dict->GetRectFor("BBox");
  bbox.Normalize();
  const CFX_Matrix form_matrix = stream_dict->GetMatrixFor("Matrix");
  const CFX_FloatRect turned = form_matrix.TransformRect(bbox);
  if (turned.Width() <= 0 || turned.Height() <= 0)
    return false;
  CFX_Matrix fit;
  fit.a = rect.Width() / turned.Width();
  fit.d = rect.Height() / turned.Height();
  fit.e = rect.left - turned.left * fit.a;
  fit.f = rect.bottom - turned.bottom * fit.d;

  RetainPtr<CPDF_Dictionary> page_dict = pdf_page->GetMutableDict();
  // Resources may be inherited: the page gets its own before it changes them.
  if (!page_dict->GetDictFor("Resources")) {
    if (RetainPtr<CPDF_Dictionary> inherited = pdf_page->GetMutablePageResources())
      page_dict->SetFor("Resources", inherited->Clone());
  }
  RetainPtr<CPDF_Dictionary> resources = page_dict->GetOrCreateDictFor("Resources");
  RetainPtr<CPDF_Dictionary> xobjects = resources->GetOrCreateDictFor("XObject");
  std::string name;
  for (int i = 0;; ++i) {
    name = "ZkFlat" + std::to_string(i);
    if (!xobjects->KeyExist(name.c_str()))
      break;
  }
  xobjects->SetNewFor<CPDF_Reference>(name.c_str(), pdf, stream->GetObjNum());

  std::string content = "q\n";
  for (double value : {double{fit.a}, 0.0, 0.0, double{fit.d}, double{fit.e}, double{fit.f}})
    Num(content, value);
  content += "cm\n/" + name + " Do\nQ\n";
  auto added = pdf->NewIndirect<CPDF_Stream>(pdf->New<CPDF_Dictionary>());
  added->SetDataAndRemoveFilter(pdfium::as_byte_span(content));
  // What was there in q and Q, so that its state does not reach what is added.
  auto opening = pdf->NewIndirect<CPDF_Stream>(pdf->New<CPDF_Dictionary>());
  opening->SetDataAndRemoveFilter(pdfium::as_byte_span(std::string_view("q\n")));
  auto closing = pdf->NewIndirect<CPDF_Stream>(pdf->New<CPDF_Dictionary>());
  closing->SetDataAndRemoveFilter(pdfium::as_byte_span(std::string_view("\nQ\n")));
  auto contents = pdf->NewIndirect<CPDF_Array>();
  contents->AppendNew<CPDF_Reference>(pdf, opening->GetObjNum());
  RetainPtr<CPDF_Object> existing = page_dict->GetMutableDirectObjectFor("Contents");
  if (existing && existing->IsArray()) {
    const CPDF_Array* array = existing->AsArray();
    for (size_t i = 0; i < array->size(); ++i) {
      if (RetainPtr<const CPDF_Object> item = array->GetDirectObjectAt(i); item && item->GetObjNum())
        contents->AppendNew<CPDF_Reference>(pdf, item->GetObjNum());
    }
  } else if (existing && existing->GetObjNum()) {
    contents->AppendNew<CPDF_Reference>(pdf, existing->GetObjNum());
  }
  contents->AppendNew<CPDF_Reference>(pdf, closing->GetObjNum());
  contents->AppendNew<CPDF_Reference>(pdf, added->GetObjNum());
  page_dict->SetNewFor<CPDF_Reference>("Contents", pdf, contents->GetObjNum());
  annots->RemoveAt(index);
  return true;
}

void PageDisplayMatrix(FPDF_PAGE page,
                       int x,
                       int y,
                       int width,
                       int height,
                       int quarter_turns,
                       double matrix[6]) {
  IPDF_Page* ipdf = IPDFPageFromFPDFPage(page);
  const CFX_Matrix m = ipdf->GetDisplayMatrixForRect(FX_RECT(x, y, x + width, y + height), quarter_turns);
  matrix[0] = m.a;
  matrix[1] = m.b;
  matrix[2] = m.c;
  matrix[3] = m.d;
  matrix[4] = m.e;
  matrix[5] = m.f;
}

bool KeepApplicationData(FPDF_PAGE page, FPDF_ANNOTATION annot, const std::string& prefix) {
  CPDF_Page* pdf_page = CPDFPageFromFPDFPage(page);
  RetainPtr<CPDF_Dictionary> dict = Dict(annot);
  if (!pdf_page || !dict || prefix.empty())
    return false;
  CPDF_Document* document = pdf_page->GetDocument();
  RetainPtr<CPDF_Dictionary> kept;
  for (const ByteString& key : dict->GetKeys()) {
    if (!std::string_view(key.c_str()).starts_with(prefix))
      continue;
    RetainPtr<const CPDF_Object> value = dict->GetDirectObjectFor(key.AsStringView());
    if (!value)
      continue;
    if (!kept)
      kept = document->New<CPDF_Dictionary>();
    kept->SetFor(key, value->Clone());
  }
  if (!kept)
    return false;
  kept->SetRectFor("Rect", dict->GetRectFor("Rect"));
  kept->SetNewFor<CPDF_Name>("Subtype", dict->GetByteStringFor("Subtype"));
  RetainPtr<CPDF_Dictionary> page_dict = pdf_page->GetMutableDict();
  RetainPtr<CPDF_Dictionary> pieces = page_dict->GetOrCreateDictFor("PieceInfo");
  RetainPtr<CPDF_Dictionary> piece = pieces->GetOrCreateDictFor(prefix.c_str());
  piece->SetNewFor<CPDF_String>("LastModified", ByteString(dict->GetByteStringFor("M")));
  RetainPtr<CPDF_Array> list = piece->GetOrCreateArrayFor("Private");
  list->Append(std::move(kept));
  return true;
}

namespace {

Value PlainValue(const CPDF_Object* object, int depth) {
  Value value;
  if (!object || depth > 8)
    return value;
  if (const CPDF_Reference* reference = object->AsReference()) {
    value.kind = Value::Kind::kRef;
    value.number = reference->GetRefObjNum();
  } else if (object->AsBoolean()) {
    value.kind = Value::Kind::kBool;
    value.boolean = object->GetInteger() != 0;
  } else if (object->AsNumber()) {
    value.kind = Value::Kind::kNumber;
    value.number = object->GetNumber();
  } else if (object->AsString()) {
    value.kind = Value::Kind::kString;
    value.text = object->GetUnicodeText().ToUTF8().c_str();
  } else if (object->AsName()) {
    value.kind = Value::Kind::kName;
    value.text = object->GetString().c_str();
  } else if (const CPDF_Array* array = object->AsArray()) {
    value.kind = Value::Kind::kArray;
    CPDF_ArrayLocker locker(array);
    for (const auto& item : locker)
      value.items.push_back(PlainValue(item.Get(), depth + 1));
  } else if (const CPDF_Dictionary* dict = object->AsDictionary()) {
    value.kind = Value::Kind::kDict;
    CPDF_DictionaryLocker locker(dict);
    for (const auto& [key, item] : locker)
      value.entries.emplace_back(key.c_str(), PlainValue(item.Get(), depth + 1));
  }
  return value;
}

}  // namespace

Value PageApplicationData(FPDF_PAGE page, const std::string& prefix) {
  CPDF_Page* pdf_page = CPDFPageFromFPDFPage(page);
  Value empty;
  empty.kind = Value::Kind::kArray;
  RetainPtr<const CPDF_Dictionary> page_dict = pdf_page ? pdf_page->GetDict() : nullptr;
  RetainPtr<const CPDF_Dictionary> pieces = page_dict ? page_dict->GetDictFor("PieceInfo") : nullptr;
  RetainPtr<const CPDF_Dictionary> piece = pieces ? pieces->GetDictFor(prefix.c_str()) : nullptr;
  RetainPtr<const CPDF_Array> list = piece ? piece->GetArrayFor("Private") : nullptr;
  if (!list)
    return empty;
  return PlainValue(list.Get(), 0);
}

bool ParseFdf(const uint8_t* data, size_t size, std::vector<FdfAnnotation>* out) {
  std::unique_ptr<CFDF_Document> document =
      CFDF_Document::ParseMemory(pdfium::span(data, size));
  if (!document || !document->GetRoot())
    return false;
  RetainPtr<const CPDF_Dictionary> fdf = document->GetRoot()->GetDictFor("FDF");
  if (!fdf)
    return false;
  // None is a file of no annotations; anything else there is not one at all.
  if (!fdf->KeyExist("Annots"))
    return true;
  RetainPtr<const CPDF_Array> annots = fdf->GetArrayFor("Annots");
  if (!annots)
    return false;
  if (annots->size() > 100000)
    return false;
  for (size_t i = 0; i < annots->size(); ++i) {
    RetainPtr<const CPDF_Dictionary> annot = ToDictionary(annots->GetDirectObjectAt(i));
    if (!annot)
      return false;
    FdfAnnotation item;
    item.object = static_cast<int>(annot->GetObjNum());
    item.entries = PlainValue(annot.Get(), 0);
    out->push_back(std::move(item));
  }
  return true;
}

bool ReplyChainReaches(FPDF_ANNOTATION annot, int object) {
  RetainPtr<CPDF_Dictionary> dict = Dict(annot);
  std::set<const CPDF_Dictionary*> seen;
  RetainPtr<const CPDF_Dictionary> at = dict ? dict->GetDictFor("IRT") : nullptr;
  while (at) {
    if (static_cast<int>(at->GetObjNum()) == object || !seen.insert(at.Get()).second)
      return true;
    at = at->GetDictFor("IRT");
  }
  return false;
}

}  // namespace zebkit::pdf
