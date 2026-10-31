// What a zebkit_pdf is, among pdf.cc, pdf_hooks.cc and pdf_interchange.cc.

#ifndef ZEBKIT_ENGINE_PDF_DOCUMENT_H_
#define ZEBKIT_ENGINE_PDF_DOCUMENT_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "public/fpdf_formfill.h"
#include "public/fpdfview.h"
#include "zebkit/engine/pdf_appearance.h"

struct zebkit_pdf {
  // The file, for a document opened from memory: PDFium reads it as it
  // needs to, for as long as the document is open.
  std::vector<uint8_t> bytes;
  // The file's path, for one opened from a file: signatures are checked
  // against the bytes it had.
  std::string path;
  FPDF_DOCUMENT document = nullptr;
  // The form-fill environment, made when it is first needed: for form
  // fields' names and values, and to draw them as filled in.
  FPDF_FORMFILLINFO form_info = {};
  FPDF_FORMHANDLE form = nullptr;
  // Fonts for generated appearances.
  zebkit::pdf::AppearanceFonts fonts;
  // How many calls with hooks are running; a hook that closes the document
  // only marks it, and the outermost call ends it.
  int depth = 0;
  bool closing = false;
  int hook_depth = 0;
  // What an OCR hook said of a page, by page dictionary's object number: a
  // page's index changes.
  struct OcrLine {
    std::string text;
    std::vector<double> box;
  };
  std::map<int, std::vector<OcrLine>> ocr;
};

namespace zebkit::pdf {

// Ends the document, now.
void DestroyDocument(zebkit_pdf* pdf);

// Around a call that runs hooks: a hook may close the document, which then
// goes when the outermost call is done with it. Declared before any lock.
class ApiScope {
 public:
  explicit ApiScope(zebkit_pdf* pdf) : pdf_(pdf) {
    if (pdf_)
      ++pdf_->depth;
  }
  ~ApiScope() {
    if (pdf_ && --pdf_->depth == 0 && pdf_->closing)
      DestroyDocument(pdf_);
  }
  ApiScope(const ApiScope&) = delete;
  ApiScope& operator=(const ApiScope&) = delete;

 private:
  zebkit_pdf* pdf_;
};

}  // namespace zebkit::pdf

#endif  // ZEBKIT_ENGINE_PDF_DOCUMENT_H_
