// Shaping for <zebkit/text.h> and <zebkit/canvas.h> (text.cc).

#ifndef ZEBKIT_ENGINE_TEXT_INTERNAL_H_
#define ZEBKIT_ENGINE_TEXT_INTERNAL_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct hb_face_t;

namespace zebkit {

// A font's bytes, which it keeps, and HarfBuzz's face over them.
class TextFace {
 public:
  TextFace(const void* data, size_t size);
  ~TextFace();
  TextFace(const TextFace&) = delete;
  TextFace& operator=(const TextFace&) = delete;

  bool valid() const;
  unsigned units_per_em() const;
  hb_face_t* face() const { return face_; }
  const std::vector<char>& bytes() const { return bytes_; }

 private:
  std::vector<char> bytes_;
  hb_face_t* face_;
};

struct ShapedGlyph {
  uint32_t glyph = 0;
  uint32_t cluster = 0;
  double advance_x = 0, advance_y = 0, offset_x = 0, offset_y = 0;
};

struct ShapedText {
  std::vector<ShapedGlyph> glyphs;
  double advance_x = 0, advance_y = 0;
  std::string direction, script;
};

// @text at @size units per em; "" in *@problem or what was wrong.
bool Shape(const TextFace& face,
           double size,
           const std::string& text,
           const char* language,
           const char* direction,
           ShapedText* out,
           std::string* problem);

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_TEXT_INTERNAL_H_
