// <zebkit/text.h> on the engine's HarfBuzz.

#ifdef UNSAFE_BUFFERS_BUILD
// The C API takes C strings and sizes.
#pragma allow_unsafe_buffers
#endif

#include "zebkit/engine/include/zebkit/text.h"

#include <stdlib.h>
#include <string.h>

#include <cmath>
#include <string>
#include <vector>

#include "base/json/json_writer.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "third_party/harfbuzz/src/src/hb.h"
#include "zebkit/engine/include/zebkit/services.h"
#include "zebkit/engine/text_internal.h"

namespace zebkit {

TextFace::TextFace(const void* data, size_t size) {
  const char* bytes = static_cast<const char*>(data);
  bytes_.assign(bytes, bytes + size);
  hb_blob_t* blob =
      hb_blob_create(bytes_.data(), static_cast<unsigned>(bytes_.size()),
                     HB_MEMORY_MODE_READONLY, nullptr, nullptr);
  face_ = hb_face_create(blob, 0);
  hb_blob_destroy(blob);
}

TextFace::~TextFace() {
  hb_face_destroy(face_);
}

bool TextFace::valid() const {
  return hb_face_get_glyph_count(face_) > 0;
}

unsigned TextFace::units_per_em() const {
  return hb_face_get_upem(face_);
}

bool Shape(const TextFace& face,
           double size,
           const std::string& text,
           const char* language,
           const char* direction,
           ShapedText* out,
           std::string* problem) {
  if (!base::IsStringUTF8(text) || text.size() > (16u << 20)) {
    *problem = "the text is not UTF-8, or over 16 MiB";
    return false;
  }
  hb_direction_t wanted = HB_DIRECTION_INVALID;
  if (direction) {
    wanted = hb_direction_from_string(direction, -1);
    if (wanted == HB_DIRECTION_INVALID || strlen(direction) < 3) {
      *problem = "the direction is ltr, rtl, ttb or btt";
      return false;
    }
  }
  // HarfBuzz here takes Unicode's properties from ICU.
  zebkit_icu_init();
  hb_font_t* font = hb_font_create(face.face());
  const unsigned upem = face.units_per_em();
  hb_font_set_scale(font, static_cast<int>(upem), static_cast<int>(upem));
  hb_buffer_t* buffer = hb_buffer_create();
  hb_buffer_add_utf8(buffer, text.data(), static_cast<int>(text.size()), 0, -1);
  if (wanted != HB_DIRECTION_INVALID)
    hb_buffer_set_direction(buffer, wanted);
  if (language)
    hb_buffer_set_language(buffer, hb_language_from_string(language, -1));
  hb_buffer_guess_segment_properties(buffer);
  hb_shape(font, buffer, nullptr, 0);
  if (!hb_buffer_allocation_successful(buffer)) {
    hb_buffer_destroy(buffer);
    hb_font_destroy(font);
    *problem = "not enough memory to shape the text";
    return false;
  }
  unsigned count = 0;
  const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer, &count);
  const hb_glyph_position_t* positions =
      hb_buffer_get_glyph_positions(buffer, &count);
  const double unit = size / upem;
  out->glyphs.clear();
  out->glyphs.reserve(count);
  out->advance_x = out->advance_y = 0;
  for (unsigned i = 0; i < count; ++i) {
    ShapedGlyph glyph;
    glyph.glyph = infos[i].codepoint;
    glyph.cluster = infos[i].cluster;
    glyph.advance_x = positions[i].x_advance * unit;
    glyph.advance_y = positions[i].y_advance * unit;
    glyph.offset_x = positions[i].x_offset * unit;
    glyph.offset_y = positions[i].y_offset * unit;
    out->advance_x += glyph.advance_x;
    out->advance_y += glyph.advance_y;
    out->glyphs.push_back(glyph);
  }
  out->direction = hb_direction_to_string(hb_buffer_get_direction(buffer));
  char tag[5] = {};
  hb_tag_to_string(
      hb_script_to_iso15924_tag(hb_buffer_get_script(buffer)), tag);
  out->script = tag;
  hb_buffer_destroy(buffer);
  hb_font_destroy(font);
  return true;
}

}  // namespace zebkit

namespace {

char* Failed(char** error, const std::string& message) {
  if (error)
    *error = strdup(message.c_str());
  return nullptr;
}

base::ListValue Pair(double x, double y) {
  base::ListValue pair;
  pair.Append(x);
  pair.Append(y);
  return pair;
}

}  // namespace

extern "C" char* zebkit_text_shape(const void* font,
                                   size_t font_size,
                                   double size,
                                   const char* text,
                                   const char* language,
                                   const char* direction,
                                   char** error) {
  if (!font || !font_size || font_size > (256u << 20))
    return Failed(error, "no font, or one over 256 MiB");
  if (!std::isfinite(size) || size <= 0 || size > 100000)
    return Failed(error, "the size is not above 0 and at most 100000");
  if (!text)
    return Failed(error, "no text");
  zebkit::TextFace face(font, font_size);
  if (!face.valid())
    return Failed(error, "not a font");
  zebkit::ShapedText shaped;
  std::string problem;
  if (!zebkit::Shape(face, size, text, language, direction, &shaped, &problem))
    return Failed(error, problem);
  base::ListValue glyphs;
  for (const zebkit::ShapedGlyph& glyph : shaped.glyphs) {
    base::DictValue item;
    item.Set("glyph", static_cast<int>(glyph.glyph));
    item.Set("cluster", static_cast<int>(glyph.cluster));
    item.Set("advance", Pair(glyph.advance_x, glyph.advance_y));
    item.Set("offset", Pair(glyph.offset_x, glyph.offset_y));
    glyphs.Append(std::move(item));
  }
  base::DictValue answer;
  answer.Set("direction", shaped.direction);
  answer.Set("script", shaped.script);
  answer.Set("unitsPerEm", static_cast<int>(face.units_per_em()));
  answer.Set("advance", Pair(shaped.advance_x, shaped.advance_y));
  answer.Set("glyphs", std::move(glyphs));
  return strdup(base::WriteJson(base::Value(std::move(answer)))
                    .value_or("null")
                    .c_str());
}
