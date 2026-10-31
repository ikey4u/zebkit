/*
 * Shaping a line of text on the engine's HarfBuzz: from a font's bytes, a
 * size and UTF-8 to the glyphs to draw and where. Breaking lines, choosing
 * fonts and bidirectional reordering are the application's.
 *
 * The rules of <zebkit/services.h> hold: what is returned is the caller's,
 * to free with zebkit_free(); a function that fails returns NULL and, when
 * @error is not NULL, sets it to a message to free with zebkit_free().
 * None of it needs zebkit_main(); any thread may call it.
 */

#ifndef ZEBKIT_TEXT_H
#define ZEBKIT_TEXT_H

#include <stddef.h>

#include <zebkit/zebkit.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Shapes @text, UTF-8, in the font of @font_size bytes at @font (TrueType or
 * OpenType; the first face of a collection), at @size units per em (points,
 * pixels: the answer's units are these). @language is a BCP 47 tag ("ar",
 * "zh-Hans") or NULL; @direction is "ltr", "rtl", "ttb", "btt" or NULL to
 * tell from the text. The answer, as JSON:
 *   {"direction": "ltr", "script": "Latn", "unitsPerEm": 1000,
 *    "advance": [x, y],
 *    "glyphs": [{"glyph": 36, "cluster": 0, "advance": [x, y],
 *                "offset": [x, y]}, ...]}
 * "cluster" is the byte of @text the glyph came from; glyphs are in visual
 * order, so that in right-to-left text clusters go down. y is up.
 * Fails on what is not a font, a size that is not finite and above 0 or above
 * 100000, a font over 256 MiB, a text over 16 MiB or not UTF-8, a direction
 * of no such name.
 */
ZEBKIT_API char *zebkit_text_shape(const void *font, size_t font_size, double size, const char *text,
                                   const char *language, const char *direction, char **error);

#ifdef __cplusplus
}
#endif

#endif /* ZEBKIT_TEXT_H */
