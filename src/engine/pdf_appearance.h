// Appearance streams and the other things <zebkit/pdf.h> does to annotation
// dictionaries with PDFium's own classes (pdf_appearance.cc). As with
// pdf_internal.h, nothing of base here: PDFium's classes and base cannot meet
// in one file.

#ifndef ZEBKIT_ENGINE_PDF_APPEARANCE_H_
#define ZEBKIT_ENGINE_PDF_APPEARANCE_H_

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "public/fpdf_annot.h"
#include "public/fpdfview.h"
#include "zebkit/engine/canvas_internal.h"
#include "zebkit/engine/pdf_internal.h"

namespace zebkit::pdf {

// Fonts a document's generated appearances may use, by family name: the bytes
// the application gave, and PDFium's font once embedded.
struct AppearanceFonts {
  std::map<std::string, std::vector<uint8_t>> bytes;
  std::map<std::string, FPDF_FONT> loaded;
};
void CloseAppearanceFonts(AppearanceFonts& fonts);

// A copy of an annotation's dictionary as it is, streams and references
// included, to put back when a change of it fails.
class AnnotationBackup {
 public:
  explicit AnnotationBackup(FPDF_ANNOTATION annot);
  ~AnnotationBackup();
  void Restore(FPDF_ANNOTATION annot) const;

 private:
  struct Entries;
  std::unique_ptr<Entries> entries_;
};

// Makes an annotation of /Subtype @subtype on @page, always an indirect object,
// and returns it to FPDFPage_CloseAnnot(). Unlike FPDFPage_CreateAnnot() it
// takes any subtype: carets, and an application's own kinds.
FPDF_ANNOTATION CreateAnnotation(FPDF_PAGE page, const char* subtype);

// The /Subtype's name.
std::string AnnotationSubtype(FPDF_ANNOTATION annot);

// Generates the normal appearance of the types this file draws (lines,
// squares, circles, polygons, polylines, free text, stamps, carets, notes and
// file attachments), or sets the /BM of those PDFium draws. False when the
// annotation keeps what it has: a stamp of an image, or "own" appearance.
bool GenerateAppearance(FPDF_DOCUMENT document,
                        FPDF_ANNOTATION annot,
                        AppearanceFonts& fonts);

// The font families the annotation's free text wants that are not the
// standard fonts, or "" for any, when its text has what they do not cover.
std::vector<std::string> WantedFonts(FPDF_ANNOTATION annot);

// Marks an appearance as made by the application (a canvas, an image, a page),
// which generation does not replace; or clears the mark.
void SetOwnAppearance(FPDF_ANNOTATION annot, bool own);
bool HasOwnAppearance(FPDF_ANNOTATION annot);

// The annotation's box without its rotation: (left, bottom, right, top), and
// its width and height for a canvas.
std::array<float, 4> AnnotationBox(FPDF_ANNOTATION annot);

// Makes what a PDF canvas wrote the annotation's normal appearance, over its
// box, turned by its rotation.
bool SetCanvasAppearance(FPDF_DOCUMENT document,
                         FPDF_ANNOTATION annot,
                         const PdfCanvasOutput& canvas);

// Makes page @page of @source, as vectors, the appearance of the annotation.
bool SetPageAppearance(FPDF_DOCUMENT document,
                       FPDF_ANNOTATION annot,
                       FPDF_DOCUMENT source,
                       int page);

// The object number of the normal appearance stream, 0 for none or a direct
// one; and that appearance put back.
int AppearanceObject(FPDF_ANNOTATION annot);
bool SetAppearanceObject(FPDF_DOCUMENT document, FPDF_ANNOTATION annot, int object);

// The index of the annotation of object number @object on the page, or -1.
int FindAnnotation(FPDF_PAGE page, int object);
// Makes annotation @index an indirect object and returns its number, or 0.
int MakeAnnotationIndirect(FPDF_PAGE page, int index);
// Puts annotation dictionary @object at @index of the page's annotations.
bool RestoreAnnotation(FPDF_PAGE page, int object, int index);
// Moves annotation @from to index @to of the page's annotations.
bool MoveAnnotation(FPDF_PAGE page, int from, int to);

// A copy of annotation @index of @page on @target_page (of the same or another
// document), without its links to a popup, a parent or its page. Returns its
// index there, or -1 with *@problem.
int CopyAnnotation(FPDF_PAGE page,
                   int index,
                   FPDF_DOCUMENT target_document,
                   FPDF_PAGE target_page,
                   std::string* problem);

// Annotation @index's normal appearance as part of the page's content, and the
// annotation gone. False without an appearance.
bool FlattenAnnotation(FPDF_DOCUMENT document, FPDF_PAGE page, int index);

// Page space to the pixels of an image of the page at (@x, @y), @width by
// @height, turned @quarter_turns clockwise, as FPDF_RenderPageBitmap() does.
void PageDisplayMatrix(FPDF_PAGE page,
                       int x,
                       int y,
                       int width,
                       int height,
                       int quarter_turns,
                       double matrix[6]);

// Keeps what annotation @annot holds under @prefix in the page's /PieceInfo
// /@prefix /Private, an array of {Kind, Rect, entries}, for a page about to
// lose the annotation. True if it held something.
bool KeepApplicationData(FPDF_PAGE page, FPDF_ANNOTATION annot, const std::string& prefix);
// That array, as plain values.
Value PageApplicationData(FPDF_PAGE page, const std::string& prefix);

// The annotations of an FDF file: their entries (/Page among them) and object
// numbers. False when it is not one.
struct FdfAnnotation {
  int object = 0;
  Value entries;
};
bool ParseFdf(const uint8_t* data, size_t size, std::vector<FdfAnnotation>* out);

// Whether following /IRT from the annotation reaches object @object, or goes
// round without an end.
bool ReplyChainReaches(FPDF_ANNOTATION annot, int object);

}  // namespace zebkit::pdf

#endif  // ZEBKIT_ENGINE_PDF_APPEARANCE_H_
