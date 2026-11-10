package zebkit

/*
#include "bridge_b.h"
*/
import "C"

// ShapeText shapes text with HarfBuzz, in font (a TrueType or OpenType
// file; the first face of a collection) at size units per em. language is a
// BCP 47 tag or "", direction "ltr", "rtl", "ttb", "btt" or "" to tell from
// the text. The answer is JSON (<zebkit/text.h>): {"direction", "script",
// "unitsPerEm", "advance": [x, y], "glyphs": [{"glyph", "cluster", "advance",
// "offset"}]}, glyphs in visual order, y up.
func ShapeText(font []byte, size float64, text, language, direction string) (string, error) {
	ctext := cString(text)
	defer free(ctext)
	clanguage := optionalString(language)
	defer free(clanguage)
	cdirection := optionalString(direction)
	defer free(cdirection)
	var message *C.char
	out := C.zebkit_text_shape(bytesArg(font), C.size_t(len(font)), C.double(size), ctext, clanguage, cdirection, &message)
	if out == nil {
		return "", takeError(message, "not shaped")
	}
	return takeString(out), nil
}
