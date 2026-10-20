// What <zebkit/pdf.h> does with PDFium's own classes, where its public API
// does not reach (pdf_internal.cc). PDFium keeps copies of base's headers
// under base's include guards, so its classes and base cannot meet in one
// file: this header has neither.

#ifndef ZEBKIT_ENGINE_PDF_INTERNAL_H_
#define ZEBKIT_ENGINE_PDF_INTERNAL_H_

#include <array>
#include <string>
#include <utility>
#include <vector>

#include "public/fpdf_annot.h"
#include "public/fpdf_formfill.h"
#include "public/fpdfview.h"

namespace zebkit::pdf {

// Sets entry @tag of the document information to the UTF-8 @value, or
// removes it when @value is null; the dictionary is made if there is none.
bool SetInfo(FPDF_DOCUMENT document, const char* tag, const char* value);

// An annotation's entries that PDFium's API has no setter for: a number,
// and the removal of any entry.
bool SetAnnotationNumber(FPDF_ANNOTATION annot, const char* key, double value);
bool RemoveAnnotationEntry(FPDF_ANNOTATION annot, const char* key);

// An annotation's colour entry (/C or /IC) as RGB from 0 to 1, from gray,
// RGB or CMYK; and set as RGB. PDFium's own functions refuse annotations
// that have an appearance stream, which most have.
bool GetAnnotationColor(FPDF_ANNOTATION annot, const char* key, float rgb[3]);
bool SetAnnotationColor(FPDF_ANNOTATION annot, const char* key, float r, float g, float b);

// Makes a link annotation go to page @page of @document.
bool SetLinkPage(FPDF_DOCUMENT document, FPDF_ANNOTATION annot, int page);

// Sets form field @name to @value (zebkit_pdf_form_set() says how each type
// takes it) and updates its appearance. Returns "" or what was wrong.
std::string SetFieldValue(FPDF_FORMHANDLE form,
                          const char* name,
                          const char* value);

// Every field back to its default value, with its appearance.
bool ResetForm(FPDF_FORMHANDLE form);

// Takes what is under @areas (left, bottom, right, top) out of @page's
// objects: characters of text one by one, the rest of a text object kept
// where it was; paths, shadings and forms whole. Images are left for the
// caller, which blacks out their pixels. Returns how many objects were
// changed or removed, or -1.
int RedactObjects(FPDF_PAGE page, const std::vector<std::array<float, 4>>& areas);

// Generates the annotation's appearance again from its entries, for the
// types PDFium makes appearances for; others keep theirs.
void RegenerateAppearance(FPDF_DOCUMENT document, FPDF_ANNOTATION annot);

// The appearances an annotation has: 1 normal, 2 rollover, 4 down.
int AppearanceModes(FPDF_ANNOTATION annot);

// Draws appearance @mode (0 normal, 1 rollover, 2 down) of the annotation
// into @bitmap, R, G, B, A in memory, with @matrix (a, b, c, d, e, f) from
// page space to its pixels. False when it has no such appearance, or is
// hidden.
bool DrawAnnotation(FPDF_PAGE page,
                    FPDF_ANNOTATION annot,
                    int mode,
                    FPDF_BITMAP bitmap,
                    const float matrix[6]);

// The object number of the page's dictionary.
int PageObjectNumber(FPDF_PAGE page);

// A PDF object as plain values, between PDFium's objects and JSON
// (pdf.cc): an indirect reference is its object number, not followed.
struct Value {
  enum class Kind { kNull, kBool, kNumber, kString, kName, kArray, kDict, kRef };
  Kind kind = Kind::kNull;
  bool boolean = false;
  double number = 0;  // a number; the object number of a reference
  std::string text;   // a string, UTF-8; a name, without its slash
  std::vector<Value> items;
  std::vector<std::pair<std::string, Value>> entries;
};

// The annotation dictionary's entries but its appearance (/AP) and its
// page (/P).
Value AnnotationEntries(FPDF_ANNOTATION annot);

// Sets entry @key of the annotation dictionary to @value; null removes
// it. A reference must be to an object of @document.
bool SetAnnotationEntry(FPDF_DOCUMENT document,
                        FPDF_ANNOTATION annot,
                        const std::string& key,
                        const Value& value);

}  // namespace zebkit::pdf

#endif  // ZEBKIT_ENGINE_PDF_INTERNAL_H_
