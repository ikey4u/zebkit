// What <zebkit/pdf.h> does with PDFium's own classes, where its public API
// does not reach (pdf_internal.cc). PDFium keeps copies of base's headers
// under base's include guards, so its classes and base cannot meet in one
// file: this header has neither.

#ifndef ZEBKIT_ENGINE_PDF_INTERNAL_H_
#define ZEBKIT_ENGINE_PDF_INTERNAL_H_

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "public/fpdf_annot.h"
#include "public/fpdf_formfill.h"
#include "public/fpdf_signature.h"
#include "public/fpdfview.h"

namespace zebkit::pdf {

// Sets entry @tag of the document information to the UTF-8 @value, or
// removes it when @value is null; the dictionary is made if there is none.
bool SetInfo(FPDF_DOCUMENT document, const char* tag, const char* value);

struct OutlineItem {
  std::string title;
  int page = -1;
  std::optional<float> x, y, zoom;
  std::optional<std::string> url;
  bool open = false;
  std::vector<OutlineItem> children;
};
// Replaces the catalog outline only after the new tree has been built.
bool SetOutline(FPDF_DOCUMENT document, const std::vector<OutlineItem>& items);

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

// A signature's field, by its full name ("Signatures.Approval"), UTF-8.
std::string SignatureFieldName(FPDF_SIGNATURE signature);

// The object number of the page's dictionary.
int PageObjectNumber(FPDF_PAGE page);

// Page @index as FPDF_LoadPage() has it, but without its content parsed:
// for what its dictionary says (size, boxes, rotation, annotations), which
// is all most pages are asked. FPDF_ClosePage() closes it. Not for
// rendering, text or objects.
FPDF_PAGE LoadPageUnparsed(FPDF_DOCUMENT document, int index);

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
// page (/P). Indirect objects are followed, but for annotations and pages,
// which stay references, and for one already being followed. What is reached
// through references takes from *@budget, or from a million of its own; past
// that they stay references: a small file's arrays of references to arrays of
// references would otherwise never end.
Value AnnotationEntries(FPDF_ANNOTATION annot, size_t* budget = nullptr);

// The object number of the annotation's dictionary; 0 for a direct one.
int AnnotationObjectNumber(FPDF_ANNOTATION annot);

// The document's interactive form's default appearance (/AcroForm /DA);
// empty without one.
std::string FormDefaultAppearance(FPDF_DOCUMENT document);

// Sets entry @key of the annotation dictionary to @value; null removes
// it. A reference must be to an object of @document.
bool SetAnnotationEntry(FPDF_DOCUMENT document,
                        FPDF_ANNOTATION annot,
                        const std::string& key,
                        const Value& value);

}  // namespace zebkit::pdf

#endif  // ZEBKIT_ENGINE_PDF_INTERNAL_H_
