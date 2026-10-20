// <zebkit/pdf.h>: documents on Chromium's PDFium, through its public API;
// what that does not reach is in pdf_internal.cc. Compiled with PDFium's
// own configuration (//zebkit/engine:pdf).

#ifdef UNSAFE_BUFFERS_BUILD
// The C API takes and returns C strings and arrays, as PDFium's does.
#pragma allow_unsafe_buffers
#endif

#include "zebkit/engine/include/zebkit/pdf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/strings/utf_string_conversions.h"
#include "base/synchronization/lock.h"
#include "base/time/time.h"
#include "base/values.h"
#include "public/fpdf_annot.h"
#include "public/fpdf_attachment.h"
#include "public/fpdf_ppo.h"
#include "public/fpdf_flatten.h"
#include "public/fpdf_catalog.h"
#include "public/fpdf_doc.h"
#include "public/fpdf_edit.h"
#include "public/fpdf_formfill.h"
#include "public/fpdf_save.h"
#include "public/fpdf_text.h"
#include "public/fpdf_transformpage.h"
#include "public/fpdfview.h"
#include "zebkit/engine/pdf_internal.h"

struct zebkit_pdf {
  // The file, for a document opened from memory: PDFium reads it as it
  // needs to, for as long as the document is open.
  std::vector<uint8_t> bytes;
  FPDF_DOCUMENT document = nullptr;
  // The form-fill environment, made when it is first needed: for form
  // fields' names and values, and to draw them as filled in.
  FPDF_FORMFILLINFO form_info = {};
  FPDF_FORMHANDLE form = nullptr;
};

namespace {

// PDFium is not thread-safe, documents or not: one call at a time.
base::Lock& PdfiumLock() {
  static base::NoDestructor<base::Lock> lock;
  return *lock;
}

void InitializeOnce() {
  static const bool initialized = [] {
    FPDF_LIBRARY_CONFIG config = {};
    config.version = 2;
    FPDF_InitLibraryWithConfig(&config);
    return true;
  }();
  (void)initialized;
}

std::nullptr_t Fail(char** error, const std::string& message) {
  if (error)
    *error = strdup(message.c_str());
  return nullptr;
}

bool FailFalse(char** error, const std::string& message) {
  Fail(error, message);
  return false;
}

char* Give(const std::string& text) {
  return strdup(text.c_str());
}

char* GiveJson(const base::Value& value) {
  return Give(base::WriteJson(value).value_or("null"));
}

uint8_t* GiveBytes(const uint8_t* data, size_t size, size_t* out_size) {
  uint8_t* copy = static_cast<uint8_t*>(malloc(size ? size : 1));
  if (!copy)
    return nullptr;
  if (size)
    memcpy(copy, data, size);
  if (out_size)
    *out_size = size;
  return copy;
}

std::string LastError() {
  switch (FPDF_GetLastError()) {
    case FPDF_ERR_FILE:
      return "the file cannot be read";
    case FPDF_ERR_FORMAT:
      return "not a PDF, or a damaged one";
    case FPDF_ERR_PASSWORD:
      return "password";
    case FPDF_ERR_SECURITY:
      return "the document's encryption is not supported";
    case FPDF_ERR_PAGE:
      return "the page cannot be read";
    default:
      return "the document cannot be opened";
  }
}

// What PDFium writes into a buffer the caller sizes: UTF-16LE, ending in
// a zero, its length in bytes.
template <typename Get>
std::string Utf16String(Get get) {
  const unsigned long bytes = get(nullptr, 0);
  if (bytes <= 2)
    return std::string();
  std::u16string text(bytes / 2, u'\0');
  get(text.data(), bytes);
  text.resize(bytes / 2 - 1);
  return base::UTF16ToUTF8(text);
}

std::u16string Utf16(const char* text) {
  return base::UTF8ToUTF16(text ? text : "");
}

FPDF_WIDESTRING Wide(const std::u16string& text) {
  return reinterpret_cast<FPDF_WIDESTRING>(text.c_str());
}

base::ListValue PdfRect(double left, double bottom, double right, double top) {
  base::ListValue rect;
  rect.Append(left);
  rect.Append(bottom);
  rect.Append(right);
  rect.Append(top);
  return rect;
}

// A page, loaded for one call.
class Page {
 public:
  Page(zebkit_pdf* pdf, int index)
      : page_(pdf && pdf->document && index >= 0 &&
                      index < FPDF_GetPageCount(pdf->document)
                  ? FPDF_LoadPage(pdf->document, index)
                  : nullptr) {}
  ~Page() {
    if (text_)
      FPDFText_ClosePage(text_);
    if (page_)
      FPDF_ClosePage(page_);
  }
  Page(const Page&) = delete;
  Page& operator=(const Page&) = delete;

  FPDF_PAGE get() const { return page_; }
  FPDF_TEXTPAGE text() {
    if (!text_ && page_)
      text_ = FPDFText_LoadPage(page_);
    return text_;
  }

 private:
  FPDF_PAGE page_;
  FPDF_TEXTPAGE text_ = nullptr;
};

FPDF_FORMHANDLE Form(zebkit_pdf* pdf) {
  if (!pdf->form) {
    pdf->form_info.version = 2;
    pdf->form = FPDFDOC_InitFormFillEnvironment(pdf->document, &pdf->form_info);
  }
  return pdf->form;
}

// The document information entries, by their names in "metadata".
constexpr struct {
  const char* key;
  const char* tag;
} kMetadata[] = {
    {"title", "Title"},       {"author", "Author"},
    {"subject", "Subject"},   {"keywords", "Keywords"},
    {"creator", "Creator"},   {"producer", "Producer"},
    {"creationDate", "CreationDate"}, {"modDate", "ModDate"},
};

base::Value Destination(FPDF_DOCUMENT document, FPDF_DEST dest) {
  base::DictValue place;
  if (!dest)
    return base::Value(std::move(place));
  const int page = FPDFDest_GetDestPageIndex(document, dest);
  if (page >= 0)
    place.Set("page", page);
  FPDF_BOOL has_x = false, has_y = false, has_zoom = false;
  FS_FLOAT x = 0, y = 0, zoom = 0;
  if (FPDFDest_GetLocationInPage(dest, &has_x, &has_y, &has_zoom, &x, &y,
                                 &zoom)) {
    if (has_x)
      place.Set("x", x);
    if (has_y)
      place.Set("y", y);
    if (has_zoom)
      place.Set("zoom", zoom);
  }
  return base::Value(std::move(place));
}

void AddAction(FPDF_DOCUMENT document,
               FPDF_ACTION action,
               base::DictValue& into) {
  if (!action)
    return;
  switch (FPDFAction_GetType(action)) {
    case PDFACTION_GOTO:
      if (base::Value place =
              Destination(document, FPDFAction_GetDest(document, action));
          place.is_dict()) {
        into.Merge(std::move(place.GetDict()));
      }
      break;
    case PDFACTION_URI: {
      const unsigned long size =
          FPDFAction_GetURIPath(document, action, nullptr, 0);
      if (size > 1) {
        std::string uri(size, '\0');
        FPDFAction_GetURIPath(document, action, uri.data(), size);
        uri.resize(size - 1);
        into.Set("url", uri);
      }
      break;
    }
    default:
      break;
  }
}

base::ListValue Outline(FPDF_DOCUMENT document,
                        FPDF_BOOKMARK parent,
                        int depth) {
  base::ListValue items;
  // Against outlines that loop back on themselves.
  if (depth > 64)
    return items;
  for (FPDF_BOOKMARK bookmark = FPDFBookmark_GetFirstChild(document, parent);
       bookmark && items.size() < 100000;
       bookmark = FPDFBookmark_GetNextSibling(document, bookmark)) {
    base::DictValue item;
    item.Set("title", Utf16String([&](void* buffer, unsigned long size) {
               return FPDFBookmark_GetTitle(bookmark, buffer, size);
             }));
    if (FPDF_DEST dest = FPDFBookmark_GetDest(document, bookmark)) {
      if (base::Value place = Destination(document, dest); place.is_dict())
        item.Merge(std::move(place.GetDict()));
    } else {
      AddAction(document, FPDFBookmark_GetAction(bookmark), item);
    }
    // A positive count is an open entry, a negative one a closed entry.
    item.Set("open", FPDFBookmark_GetCount(bookmark) > 0);
    base::ListValue children = Outline(document, bookmark, depth + 1);
    if (!children.empty())
      item.Set("children", std::move(children));
    items.Append(std::move(item));
  }
  return items;
}

// FPDF_FILEWRITE that keeps what it is given.
struct Writer : FPDF_FILEWRITE {
  std::vector<uint8_t> bytes;
};

int WriteBlock(FPDF_FILEWRITE* self, const void* data, unsigned long size) {
  auto* writer = static_cast<Writer*>(self);
  const uint8_t* begin = static_cast<const uint8_t*>(data);
  writer->bytes.insert(writer->bytes.end(), begin, begin + size);
  return 1;
}

}  // namespace

extern "C" {

zebkit_pdf* zebkit_pdf_open(const void* data,
                            size_t size,
                            const char* password,
                            char** error) {
  if (!data || !size)
    return Fail(error, "no data");
  base::AutoLock lock(PdfiumLock());
  InitializeOnce();
  auto pdf = std::make_unique<zebkit_pdf>();
  const uint8_t* begin = static_cast<const uint8_t*>(data);
  pdf->bytes.assign(begin, begin + size);
  pdf->document = FPDF_LoadMemDocument64(pdf->bytes.data(), pdf->bytes.size(),
                                         password);
  if (!pdf->document)
    return Fail(error, LastError());
  return pdf.release();
}

zebkit_pdf* zebkit_pdf_open_file(const char* path,
                                 const char* password,
                                 char** error) {
  if (!path || !*path)
    return Fail(error, "no path");
  base::AutoLock lock(PdfiumLock());
  InitializeOnce();
  auto pdf = std::make_unique<zebkit_pdf>();
  pdf->document = FPDF_LoadDocument(path, password);
  if (!pdf->document)
    return Fail(error, LastError());
  return pdf.release();
}

void zebkit_pdf_close(zebkit_pdf* pdf) {
  if (!pdf)
    return;
  base::AutoLock lock(PdfiumLock());
  if (pdf->form)
    FPDFDOC_ExitFormFillEnvironment(pdf->form);
  if (pdf->document)
    FPDF_CloseDocument(pdf->document);
  delete pdf;
}

uint8_t* zebkit_pdf_save(zebkit_pdf* pdf,
                         unsigned flags,
                         size_t* size,
                         char** error) {
  if (!pdf)
    return Fail(error, "no document");
  base::AutoLock lock(PdfiumLock());
  Writer writer;
  writer.version = 1;
  writer.WriteBlock = &WriteBlock;
  FPDF_DWORD pdfium_flags = 0;
  if (flags & ZEBKIT_PDF_SAVE_INCREMENTAL)
    pdfium_flags |= FPDF_INCREMENTAL;
  if (flags & ZEBKIT_PDF_SAVE_NO_SECURITY)
    pdfium_flags |= FPDF_REMOVE_SECURITY;
  if (!FPDF_SaveAsCopy(pdf->document, &writer, pdfium_flags))
    return Fail(error, "the document cannot be written");
  return GiveBytes(writer.bytes.data(), writer.bytes.size(), size);
}

char* zebkit_pdf_info(zebkit_pdf* pdf) {
  if (!pdf)
    return nullptr;
  base::AutoLock lock(PdfiumLock());
  FPDF_DOCUMENT document = pdf->document;
  base::DictValue info;
  info.Set("pages", FPDF_GetPageCount(document));
  int version = 0;
  if (FPDF_GetFileVersion(document, &version))
    info.Set("version", version);
  const int revision = FPDF_GetSecurityHandlerRevision(document);
  info.Set("encrypted", revision != -1);
  info.Set("securityRevision", revision);
  // The permission bits of the PDF reference, table 22.
  const unsigned long bits = FPDF_GetDocPermissions(document);
  base::DictValue permissions;
  permissions.Set("print", (bits & (1 << 2)) != 0);
  permissions.Set("modify", (bits & (1 << 3)) != 0);
  permissions.Set("copy", (bits & (1 << 4)) != 0);
  permissions.Set("annotate", (bits & (1 << 5)) != 0);
  permissions.Set("fillForms", (bits & (1 << 8)) != 0);
  permissions.Set("extract", (bits & (1 << 9)) != 0);
  permissions.Set("assemble", (bits & (1 << 10)) != 0);
  permissions.Set("printHighQuality", (bits & (1 << 11)) != 0);
  info.Set("permissions", std::move(permissions));
  switch (FPDF_GetFormType(document)) {
    case FORMTYPE_ACRO_FORM:
      info.Set("form", "acro");
      break;
    case FORMTYPE_XFA_FULL:
    case FORMTYPE_XFA_FOREGROUND:
      info.Set("form", "xfa");
      break;
    default:
      info.Set("form", "none");
  }
  info.Set("tagged", static_cast<bool>(FPDFCatalog_IsTagged(document)));
  const std::string language =
      Utf16String([&](void* buffer, unsigned long size) {
        return FPDFCatalog_GetLanguage(
            document, static_cast<FPDF_WCHAR*>(buffer), size);
      });
  if (!language.empty())
    info.Set("language", language);
  base::DictValue metadata;
  for (const auto& entry : kMetadata) {
    const std::string value =
        Utf16String([&](void* buffer, unsigned long size) {
          return FPDF_GetMetaText(document, entry.tag, buffer, size);
        });
    if (!value.empty())
      metadata.Set(entry.key, value);
  }
  info.Set("metadata", std::move(metadata));
  return GiveJson(base::Value(std::move(info)));
}

bool zebkit_pdf_set_metadata(zebkit_pdf* pdf, const char* json, char** error) {
  if (!pdf)
    return FailFalse(error, "no document");
  std::optional<base::Value> parsed = base::JSONReader::Read(
      json ? json : "", base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!parsed || !parsed->is_dict())
    return FailFalse(error, "the metadata is not a JSON object");
  // Checked before anything is changed.
  for (auto [key, value] : parsed->GetDict()) {
    const char* tag = nullptr;
    for (const auto& entry : kMetadata) {
      if (key == entry.key)
        tag = entry.tag;
    }
    if (!tag)
      return FailFalse(error, "unknown metadata entry: " + key);
    if (!value.is_none() && !value.is_string())
      return FailFalse(error, key + " is not a string");
  }
  base::AutoLock lock(PdfiumLock());
  for (auto [key, value] : parsed->GetDict()) {
    for (const auto& entry : kMetadata) {
      if (key != entry.key)
        continue;
      if (!zebkit::pdf::SetInfo(
              pdf->document, entry.tag,
              value.is_string() ? value.GetString().c_str() : nullptr)) {
        return FailFalse(error, "the document information cannot be changed");
      }
    }
  }
  return true;
}

int zebkit_pdf_page_count(zebkit_pdf* pdf) {
  if (!pdf)
    return -1;
  base::AutoLock lock(PdfiumLock());
  return FPDF_GetPageCount(pdf->document);
}

char* zebkit_pdf_page(zebkit_pdf* pdf, int index, char** error) {
  base::AutoLock lock(PdfiumLock());
  Page page(pdf, index);
  if (!page.get())
    return Fail(error, "no page " + std::to_string(index));
  base::DictValue info;
  FS_SIZEF size = {};
  FPDF_GetPageSizeByIndexF(pdf->document, index, &size);
  info.Set("width", size.width);
  info.Set("height", size.height);
  info.Set("rotation", FPDFPage_GetRotation(page.get()) * 90);
  const std::string label = Utf16String([&](void* buffer, unsigned long n) {
    return FPDF_GetPageLabel(pdf->document, index, buffer, n);
  });
  if (!label.empty())
    info.Set("label", label);
  using GetBox = FPDF_BOOL (*)(FPDF_PAGE, float*, float*, float*, float*);
  constexpr struct {
    const char* name;
    GetBox get;
  } kBoxes[] = {
      {"mediaBox", &FPDFPage_GetMediaBox}, {"cropBox", &FPDFPage_GetCropBox},
      {"bleedBox", &FPDFPage_GetBleedBox}, {"trimBox", &FPDFPage_GetTrimBox},
      {"artBox", &FPDFPage_GetArtBox},
  };
  for (const auto& box : kBoxes) {
    float left = 0, bottom = 0, right = 0, top = 0;
    if (box.get(page.get(), &left, &bottom, &right, &top))
      info.Set(box.name, PdfRect(left, bottom, right, top));
  }
  return GiveJson(base::Value(std::move(info)));
}

uint8_t* zebkit_pdf_render(zebkit_pdf* pdf,
                           int index,
                           const zebkit_pdf_render_options* given,
                           int* width,
                           int* height,
                           char** error) {
  zebkit_pdf_render_options options = {};
  options.flags = ZEBKIT_PDF_RENDER_ANNOTATIONS;
  options.background = 0xffffffff;
  if (given)
    options = *given;
  const float scale = options.scale > 0 ? options.scale : 1;
  if (options.rotation % 90)
    return Fail(error, "the rotation is not a multiple of 90");
  base::AutoLock lock(PdfiumLock());
  Page page(pdf, index);
  if (!page.get())
    return Fail(error, "no page " + std::to_string(index));
  const int quarter_turns = ((options.rotation / 90) % 4 + 4) % 4;
  // The page's own rotation is applied by PDFium; the size is the rendered
  // page's, which turns with an odd number of quarter turns.
  double page_width = FPDF_GetPageWidthF(page.get());
  double page_height = FPDF_GetPageHeightF(page.get());
  if (quarter_turns % 2)
    std::swap(page_width, page_height);
  const int full_width = std::max(1, static_cast<int>(page_width * scale + 0.5));
  const int full_height =
      std::max(1, static_cast<int>(page_height * scale + 0.5));
  int tile_x = 0, tile_y = 0, tile_width = full_width, tile_height = full_height;
  if (options.width > 0 && options.height > 0) {
    tile_x = options.x;
    tile_y = options.y;
    tile_width = options.width;
    tile_height = options.height;
  }
  // A limit against asking for gigabytes.
  if (static_cast<int64_t>(tile_width) * tile_height > (1 << 28))
    return Fail(error, "the image would be too large");
  FPDF_BITMAP bitmap = FPDFBitmap_Create(tile_width, tile_height, 1);
  if (!bitmap)
    return Fail(error, "not enough memory for the image");
  // FPDFBitmap_FillRect writes 0xAARRGGBB as B, G, R, A in memory, and the
  // page is rendered as R, G, B, A (FPDF_REVERSE_BYTE_ORDER): red and blue
  // change places here, so that the background is the one asked for.
  const uint32_t rgba = options.background;
  const uint32_t fill = ((rgba & 0xff) << 24) | (((rgba >> 8) & 0xff) << 16) |
                        (((rgba >> 16) & 0xff) << 8) | (rgba >> 24);
  FPDFBitmap_FillRect(bitmap, 0, 0, tile_width, tile_height, fill);
  int flags = FPDF_REVERSE_BYTE_ORDER;
  if (options.flags & ZEBKIT_PDF_RENDER_ANNOTATIONS)
    flags |= FPDF_ANNOT;
  if (options.flags & ZEBKIT_PDF_RENDER_LCD_TEXT)
    flags |= FPDF_LCD_TEXT;
  if (options.flags & ZEBKIT_PDF_RENDER_GRAYSCALE)
    flags |= FPDF_GRAYSCALE;
  if (options.flags & ZEBKIT_PDF_RENDER_PRINTING)
    flags |= FPDF_PRINTING;
  if (options.flags & ZEBKIT_PDF_RENDER_NO_SMOOTHING) {
    flags |= FPDF_RENDER_NO_SMOOTHTEXT | FPDF_RENDER_NO_SMOOTHIMAGE |
             FPDF_RENDER_NO_SMOOTHPATH;
  }
  FPDF_RenderPageBitmap(bitmap, page.get(), -tile_x, -tile_y, full_width,
                        full_height, quarter_turns, flags);
  if (options.flags & ZEBKIT_PDF_RENDER_FORMS) {
    if (FPDF_FORMHANDLE form = Form(pdf)) {
      FORM_OnAfterLoadPage(page.get(), form);
      FPDF_FFLDraw(form, bitmap, page.get(), -tile_x, -tile_y, full_width,
                   full_height, quarter_turns, flags);
      FORM_OnBeforeClosePage(page.get(), form);
    }
  }
  // RGBA in memory, not premultiplied, as PDFium's bitmaps are.
  const uint8_t* pixels =
      static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(bitmap));
  const int stride = FPDFBitmap_GetStride(bitmap);
  const size_t row = static_cast<size_t>(tile_width) * 4;
  uint8_t* out = static_cast<uint8_t*>(malloc(row * tile_height));
  if (out) {
    for (int y = 0; y < tile_height; ++y)
      memcpy(out + row * y, pixels + static_cast<size_t>(stride) * y, row);
  }
  FPDFBitmap_Destroy(bitmap);
  if (!out)
    return Fail(error, "not enough memory for the image");
  if (width)
    *width = tile_width;
  if (height)
    *height = tile_height;
  return out;
}

char* zebkit_pdf_text(zebkit_pdf* pdf, int index, char** error) {
  base::AutoLock lock(PdfiumLock());
  Page page(pdf, index);
  if (!page.text())
    return Fail(error, "no page " + std::to_string(index));
  const int count = FPDFText_CountChars(page.text());
  std::u16string text(count + 1, u'\0');
  const int written = FPDFText_GetText(
      page.text(), 0, count, reinterpret_cast<unsigned short*>(text.data()));
  text.resize(written > 0 ? written - 1 : 0);
  return Give(base::UTF16ToUTF8(text));
}

char* zebkit_pdf_text_layout(zebkit_pdf* pdf, int index, char** error) {
  base::AutoLock lock(PdfiumLock());
  Page page(pdf, index);
  FPDF_TEXTPAGE text_page = page.text();
  if (!text_page)
    return Fail(error, "no page " + std::to_string(index));
  const int count = FPDFText_CountChars(text_page);
  std::u16string text(count + 1, u'\0');
  const int written = FPDFText_GetText(
      text_page, 0, count, reinterpret_cast<unsigned short*>(text.data()));
  text.resize(written > 0 ? written - 1 : 0);
  base::ListValue chars;
  std::string font;
  for (int i = 0; i < count; ++i) {
    base::DictValue item;
    const unsigned int unicode = FPDFText_GetUnicode(text_page, i);
    std::u16string one(1, static_cast<char16_t>(unicode));
    item.Set("c", base::UTF16ToUTF8(one));
    if (FPDFText_IsGenerated(text_page, i) != 1) {
      double left = 0, right = 0, bottom = 0, top = 0;
      if (FPDFText_GetCharBox(text_page, i, &left, &right, &bottom, &top))
        item.Set("box", PdfRect(left, bottom, right, top));
      item.Set("size", FPDFText_GetFontSize(text_page, i));
      const unsigned long size =
          FPDFText_GetFontInfo(text_page, i, nullptr, 0, nullptr);
      if (size > 1) {
        font.assign(size, '\0');
        FPDFText_GetFontInfo(text_page, i, font.data(), size, nullptr);
        font.resize(size - 1);
        item.Set("font", font);
      }
      item.Set("angle", FPDFText_GetCharAngle(text_page, i));
    }
    chars.Append(std::move(item));
  }
  base::DictValue layout;
  layout.Set("text", base::UTF16ToUTF8(text));
  layout.Set("chars", std::move(chars));
  return GiveJson(base::Value(std::move(layout)));
}

namespace {

base::ListValue TextRects(FPDF_TEXTPAGE text_page, int start, int count) {
  base::ListValue rects;
  const int n = FPDFText_CountRects(text_page, start, count);
  for (int i = 0; i < n; ++i) {
    double left = 0, top = 0, right = 0, bottom = 0;
    if (FPDFText_GetRect(text_page, i, &left, &top, &right, &bottom))
      rects.Append(PdfRect(left, bottom, right, top));
  }
  return rects;
}

}  // namespace

char* zebkit_pdf_text_rects(zebkit_pdf* pdf,
                            int index,
                            int start,
                            int count,
                            char** error) {
  base::AutoLock lock(PdfiumLock());
  Page page(pdf, index);
  if (!page.text())
    return Fail(error, "no page " + std::to_string(index));
  return GiveJson(base::Value(TextRects(page.text(), start, count)));
}

int zebkit_pdf_char_at(zebkit_pdf* pdf,
                       int index,
                       double x,
                       double y,
                       double tolerance) {
  base::AutoLock lock(PdfiumLock());
  Page page(pdf, index);
  if (!page.text())
    return -1;
  const int found =
      FPDFText_GetCharIndexAtPos(page.text(), x, y, tolerance, tolerance);
  return found >= 0 ? found : -1;
}

char* zebkit_pdf_search(zebkit_pdf* pdf,
                        const char* query,
                        unsigned flags,
                        int first,
                        int limit,
                        char** error) {
  if (!pdf)
    return Fail(error, "no document");
  const std::u16string what = Utf16(query);
  if (what.empty())
    return Fail(error, "nothing to search for");
  base::AutoLock lock(PdfiumLock());
  unsigned long pdfium_flags = 0;
  if (flags & ZEBKIT_PDF_SEARCH_CASE)
    pdfium_flags |= FPDF_MATCHCASE;
  if (flags & ZEBKIT_PDF_SEARCH_WHOLE_WORD)
    pdfium_flags |= FPDF_MATCHWHOLEWORD;
  base::ListValue matches;
  const int pages = FPDF_GetPageCount(pdf->document);
  for (int index = std::max(0, first); index < pages; ++index) {
    Page page(pdf, index);
    FPDF_TEXTPAGE text_page = page.text();
    if (!text_page)
      continue;
    FPDF_SCHHANDLE search =
        FPDFText_FindStart(text_page, Wide(what), pdfium_flags, 0);
    if (!search)
      continue;
    while (FPDFText_FindNext(search)) {
      const int start = FPDFText_GetSchResultIndex(search);
      const int count = FPDFText_GetSchCount(search);
      base::DictValue match;
      match.Set("page", index);
      match.Set("start", start);
      match.Set("count", count);
      match.Set("rects", TextRects(text_page, start, count));
      // Some words on either side, for a list of results.
      const int total = FPDFText_CountChars(text_page);
      const int from = std::max(0, start - 30);
      const int to = std::min(total, start + count + 30);
      std::u16string context(to - from + 1, u'\0');
      const int written =
          FPDFText_GetText(text_page, from, to - from,
                           reinterpret_cast<unsigned short*>(context.data()));
      context.resize(written > 0 ? written - 1 : 0);
      match.Set("context", base::UTF16ToUTF8(context));
      matches.Append(std::move(match));
      if (limit > 0 && static_cast<int>(matches.size()) >= limit)
        break;
    }
    FPDFText_FindClose(search);
    if (limit > 0 && static_cast<int>(matches.size()) >= limit)
      break;
  }
  return GiveJson(base::Value(std::move(matches)));
}

char* zebkit_pdf_outline(zebkit_pdf* pdf, char** error) {
  if (!pdf)
    return Fail(error, "no document");
  base::AutoLock lock(PdfiumLock());
  return GiveJson(base::Value(Outline(pdf->document, nullptr, 0)));
}

char* zebkit_pdf_links(zebkit_pdf* pdf, int index, char** error) {
  base::AutoLock lock(PdfiumLock());
  Page page(pdf, index);
  if (!page.get())
    return Fail(error, "no page " + std::to_string(index));
  base::ListValue links;
  int position = 0;
  FPDF_LINK link = nullptr;
  while (FPDFLink_Enumerate(page.get(), &position, &link)) {
    base::DictValue item;
    FS_RECTF rect = {};
    if (FPDFLink_GetAnnotRect(link, &rect))
      item.Set("rect", PdfRect(rect.left, rect.bottom, rect.right, rect.top));
    if (FPDF_DEST dest = FPDFLink_GetDest(pdf->document, link)) {
      if (base::Value place = Destination(pdf->document, dest);
          place.is_dict()) {
        item.Merge(std::move(place.GetDict()));
      }
    } else {
      AddAction(pdf->document, FPDFLink_GetAction(link), item);
    }
    links.Append(std::move(item));
  }
  if (FPDF_TEXTPAGE text_page = page.text()) {
    if (FPDF_PAGELINK web = FPDFLink_LoadWebLinks(text_page)) {
      const int count = FPDFLink_CountWebLinks(web);
      for (int i = 0; i < count; ++i) {
        const int length = FPDFLink_GetURL(web, i, nullptr, 0);
        std::u16string url(std::max(length, 1), u'\0');
        FPDFLink_GetURL(web, i, reinterpret_cast<unsigned short*>(url.data()),
                        length);
        url.resize(length > 0 ? length - 1 : 0);
        const int rects = FPDFLink_CountRects(web, i);
        for (int r = 0; r < rects; ++r) {
          double left = 0, top = 0, right = 0, bottom = 0;
          if (!FPDFLink_GetRect(web, i, r, &left, &top, &right, &bottom))
            continue;
          base::DictValue item;
          item.Set("rect", PdfRect(left, bottom, right, top));
          item.Set("url", base::UTF16ToUTF8(url));
          item.Set("detected", true);
          links.Append(std::move(item));
        }
      }
      FPDFLink_CloseWebLinks(web);
    }
  }
  return GiveJson(base::Value(std::move(links)));
}

namespace {

constexpr const char* kAnnotationTypes[] = {
    "unknown",   "text",         "link",        "freeText",  "line",
    "square",    "circle",       "polygon",     "polyline",  "highlight",
    "underline", "squiggly",     "strikeOut",   "stamp",     "caret",
    "ink",       "popup",        "fileAttachment", "sound",  "movie",
    "widget",    "screen",       "printerMark", "trapNet",   "watermark",
    "threeD",    "richMedia",    "xfaWidget",   "redact",
};

// The string entries of an annotation, by their names in the JSON.
constexpr struct {
  const char* key;
  const char* entry;
} kAnnotationStrings[] = {
    {"contents", "Contents"}, {"author", "T"},         {"subject", "Subj"},
    {"name", "NM"},           {"modified", "M"},       {"created", "CreationDate"},
};

base::ListValue Color(const float rgb[3]) {
  base::ListValue color;
  for (int i = 0; i < 3; ++i)
    color.Append(static_cast<int>(std::clamp(rgb[i], 0.0f, 1.0f) * 255 + 0.5f));
  return color;
}

base::ListValue PdfPoint(float x, float y) {
  base::ListValue point;
  point.Append(x);
  point.Append(y);
  return point;
}

std::string AnnotationString(FPDF_ANNOTATION annot, const char* key) {
  return Utf16String([&](void* buffer, unsigned long size) {
    return FPDFAnnot_GetStringValue(annot, key,
                                    static_cast<FPDF_WCHAR*>(buffer), size);
  });
}

base::DictValue DescribeAnnotation(zebkit_pdf* pdf,
                                   FPDF_PAGE page,
                                   FPDF_ANNOTATION annot,
                                   int index) {
  base::DictValue item;
  item.Set("index", index);
  const int subtype = FPDFAnnot_GetSubtype(annot);
  item.Set("type", subtype >= 0 && subtype < static_cast<int>(std::size(
                                                 kAnnotationTypes))
                       ? kAnnotationTypes[subtype]
                       : "unknown");
  FS_RECTF rect = {};
  if (FPDFAnnot_GetRect(annot, &rect))
    item.Set("rect", PdfRect(rect.left, rect.bottom, rect.right, rect.top));
  float rgb[3] = {};
  if (zebkit::pdf::GetAnnotationColor(annot, "C", rgb))
    item.Set("color", Color(rgb));
  if (zebkit::pdf::GetAnnotationColor(annot, "IC", rgb))
    item.Set("interiorColor", Color(rgb));
  float number = 0;
  if (FPDFAnnot_GetNumberValue(annot, "CA", &number))
    item.Set("opacity", number);
  float horizontal = 0, vertical = 0, width = 0;
  if (FPDFAnnot_GetBorder(annot, &horizontal, &vertical, &width))
    item.Set("border", width);
  item.Set("flags", FPDFAnnot_GetFlags(annot));
  for (const auto& entry : kAnnotationStrings) {
    if (FPDFAnnot_HasKey(annot, entry.entry))
      item.Set(entry.key, AnnotationString(annot, entry.entry));
  }
  if (const size_t quads = FPDFAnnot_CountAttachmentPoints(annot)) {
    base::ListValue list;
    for (size_t i = 0; i < quads; ++i) {
      FS_QUADPOINTSF quad = {};
      if (!FPDFAnnot_GetAttachmentPoints(annot, i, &quad))
        continue;
      base::ListValue points;
      for (float value : {quad.x1, quad.y1, quad.x2, quad.y2, quad.x3,
                          quad.y3, quad.x4, quad.y4}) {
        points.Append(value);
      }
      list.Append(std::move(points));
    }
    item.Set("quads", std::move(list));
  }
  if (subtype == FPDF_ANNOT_INK) {
    base::ListValue strokes;
    const int count = FPDFAnnot_GetInkListCount(annot);
    for (int i = 0; i < count; ++i) {
      const unsigned long n = FPDFAnnot_GetInkListPath(annot, i, nullptr, 0);
      std::vector<FS_POINTF> points(n);
      FPDFAnnot_GetInkListPath(annot, i, points.data(), n);
      base::ListValue stroke;
      for (const FS_POINTF& point : points)
        stroke.Append(PdfPoint(point.x, point.y));
      strokes.Append(std::move(stroke));
    }
    item.Set("ink", std::move(strokes));
  }
  if (subtype == FPDF_ANNOT_POLYGON || subtype == FPDF_ANNOT_POLYLINE) {
    const unsigned long n = FPDFAnnot_GetVertices(annot, nullptr, 0);
    std::vector<FS_POINTF> points(n);
    FPDFAnnot_GetVertices(annot, points.data(), n);
    base::ListValue vertices;
    for (const FS_POINTF& point : points)
      vertices.Append(PdfPoint(point.x, point.y));
    item.Set("vertices", std::move(vertices));
  }
  FS_POINTF start = {}, end = {};
  if (subtype == FPDF_ANNOT_LINE && FPDFAnnot_GetLine(annot, &start, &end)) {
    base::ListValue line;
    for (float value : {start.x, start.y, end.x, end.y})
      line.Append(value);
    item.Set("line", std::move(line));
  }
  if (subtype == FPDF_ANNOT_LINK) {
    if (FPDF_LINK link = FPDFAnnot_GetLink(annot)) {
      if (FPDF_DEST dest = FPDFLink_GetDest(pdf->document, link)) {
        if (base::Value place = Destination(pdf->document, dest);
            place.is_dict()) {
          item.Merge(std::move(place.GetDict()));
        }
      } else {
        AddAction(pdf->document, FPDFLink_GetAction(link), item);
      }
    }
  }
  for (const auto& [key, entry] :
       {std::pair{"popup", "Popup"}, std::pair{"inReplyTo", "IRT"}}) {
    if (FPDF_ANNOTATION linked = FPDFAnnot_GetLinkedAnnot(annot, entry)) {
      const int linked_index = FPDFPage_GetAnnotIndex(page, linked);
      if (linked_index >= 0)
        item.Set(key, linked_index);
      FPDFPage_CloseAnnot(linked);
    }
  }
  if (subtype == FPDF_ANNOT_WIDGET) {
    if (FPDF_FORMHANDLE form = Form(pdf)) {
      item.Set("field", Utf16String([&](void* buffer, unsigned long size) {
                 return FPDFAnnot_GetFormFieldName(
                     form, annot, static_cast<FPDF_WCHAR*>(buffer), size);
               }));
    }
  }
  return item;
}

// A PDF date for now, as the PDF reference writes one.
std::string PdfDateNow() {
  base::Time::Exploded now;
  base::Time::Now().UTCExplode(&now);
  char date[32];
  snprintf(date, sizeof(date), "D:%04d%02d%02d%02d%02d%02dZ", now.year,
           now.month, now.day_of_month, now.hour, now.minute, now.second);
  return date;
}

std::optional<std::vector<float>> Numbers(const base::Value* value,
                                          size_t count) {
  if (!value || !value->is_list())
    return std::nullopt;
  const base::ListValue& list = value->GetList();
  if (count && list.size() != count)
    return std::nullopt;
  std::vector<float> numbers;
  for (const base::Value& item : list) {
    if (!item.is_int() && !item.is_double())
      return std::nullopt;
    numbers.push_back(static_cast<float>(item.GetDouble()));
  }
  return numbers;
}

std::optional<std::vector<FS_POINTF>> Points(const base::Value* value) {
  if (!value || !value->is_list())
    return std::nullopt;
  std::vector<FS_POINTF> points;
  for (const base::Value& item : value->GetList()) {
    std::optional<std::vector<float>> xy = Numbers(&item, 2);
    if (!xy)
      return std::nullopt;
    points.push_back({(*xy)[0], (*xy)[1]});
  }
  return points;
}

// Sets what @json says on @annot; @creating for a new one. Returns "" or
// what was wrong.
std::string ApplyAnnotation(zebkit_pdf* pdf,
                            FPDF_ANNOTATION annot,
                            const base::DictValue& json,
                            bool creating) {
  const int subtype = FPDFAnnot_GetSubtype(annot);
  for (const auto [key, value] : json) {
    if (key == "index" || key == "type" || key == "popup" ||
        key == "inReplyTo" || key == "field") {
      continue;
    }
    if (key == "rect") {
      std::optional<std::vector<float>> r = Numbers(&value, 4);
      if (!r)
        return "rect is not [left, bottom, right, top]";
      FS_RECTF rect = {(*r)[0], (*r)[3], (*r)[2], (*r)[1]};
      if (!FPDFAnnot_SetRect(annot, &rect))
        return "the rectangle cannot be set";
    } else if (key == "color" || key == "interiorColor") {
      const char* entry = key == "color" ? "C" : "IC";
      if (value.is_none()) {
        zebkit::pdf::RemoveAnnotationEntry(annot, entry);
        continue;
      }
      std::optional<std::vector<float>> c = Numbers(&value, 3);
      if (!c)
        return key + " is not [r, g, b], each 0-255";
      auto channel = [&](size_t i) {
        return std::clamp((*c)[i], 0.0f, 255.0f) / 255;
      };
      zebkit::pdf::SetAnnotationColor(annot, entry, channel(0), channel(1),
                                      channel(2));
    } else if (key == "opacity") {
      if (!value.is_int() && !value.is_double())
        return "opacity is not a number";
      zebkit::pdf::SetAnnotationNumber(annot, "CA",
                                       std::clamp(value.GetDouble(), 0.0, 1.0));
    } else if (key == "border") {
      if (!value.is_int() && !value.is_double())
        return "border is not a number";
      if (!FPDFAnnot_SetBorder(annot, 0, 0,
                               static_cast<float>(value.GetDouble()))) {
        return "the border cannot be set";
      }
    } else if (key == "flags") {
      if (!value.is_int())
        return "flags is not a number";
      FPDFAnnot_SetFlags(annot, value.GetInt());
    } else if (key == "quads") {
      if (!FPDFAnnot_HasAttachmentPoints(annot))
        return "a " + std::string(kAnnotationTypes[subtype]) +
               " annotation has no quads";
      if (!value.is_list())
        return "quads is not a list";
      zebkit::pdf::RemoveAnnotationEntry(annot, "QuadPoints");
      for (const base::Value& item : value.GetList()) {
        std::optional<std::vector<float>> q = Numbers(&item, 8);
        if (!q)
          return "a quad is not 8 numbers";
        FS_QUADPOINTSF quad = {(*q)[0], (*q)[1], (*q)[2], (*q)[3],
                               (*q)[4], (*q)[5], (*q)[6], (*q)[7]};
        FPDFAnnot_AppendAttachmentPoints(annot, &quad);
      }
    } else if (key == "ink") {
      if (subtype != FPDF_ANNOT_INK)
        return "only ink annotations have ink";
      if (!value.is_list())
        return "ink is not a list of strokes";
      FPDFAnnot_RemoveInkList(annot);
      for (const base::Value& stroke : value.GetList()) {
        std::optional<std::vector<FS_POINTF>> points = Points(&stroke);
        if (!points || points->empty())
          return "a stroke is not a list of [x, y]";
        FPDFAnnot_AddInkStroke(annot, points->data(), points->size());
      }
    } else if (key == "vertices") {
      std::optional<std::vector<FS_POINTF>> points = Points(&value);
      if (!points || !FPDFAnnot_SetVertices(annot, points->data(),
                                            points->size())) {
        return "vertices are for polygons and polylines, two points or more";
      }
    } else if (key == "line") {
      std::optional<std::vector<float>> l = Numbers(&value, 4);
      FS_POINTF start = {}, end = {};
      if (l) {
        start = {(*l)[0], (*l)[1]};
        end = {(*l)[2], (*l)[3]};
      }
      if (!l || !FPDFAnnot_SetLine(annot, &start, &end))
        return "line is for line annotations: [x1, y1, x2, y2]";
    } else if (key == "url") {
      if (!value.is_string() ||
          !FPDFAnnot_SetURI(annot, value.GetString().c_str())) {
        return "url is for links, as a string";
      }
    } else if (key == "page") {
      if (subtype != FPDF_ANNOT_LINK || !value.is_int() ||
          !zebkit::pdf::SetLinkPage(pdf->document, annot, value.GetInt())) {
        return "page is for links, as the index of a page";
      }
    } else {
      const char* entry = nullptr;
      for (const auto& string : kAnnotationStrings) {
        if (key == string.key)
          entry = string.entry;
      }
      if (!entry)
        return "unknown key: " + key;
      if (value.is_none()) {
        zebkit::pdf::RemoveAnnotationEntry(annot, entry);
        continue;
      }
      if (!value.is_string())
        return key + " is not a string";
      const std::u16string text = base::UTF8ToUTF16(value.GetString());
      FPDFAnnot_SetStringValue(annot, entry, Wide(text));
    }
  }
  if (!json.contains("modified")) {
    const std::u16string now = base::UTF8ToUTF16(PdfDateNow());
    FPDFAnnot_SetStringValue(annot, "M", Wide(now));
    if (creating && !json.contains("created"))
      FPDFAnnot_SetStringValue(annot, "CreationDate", Wide(now));
  }
  if (creating && !json.contains("flags"))
    FPDFAnnot_SetFlags(annot, FPDF_ANNOT_FLAG_PRINT);
  zebkit::pdf::RegenerateAppearance(pdf->document, annot);
  return std::string();
}

std::optional<base::DictValue> ParseObject(const char* json) {
  std::optional<base::Value> parsed = base::JSONReader::Read(
      json ? json : "", base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!parsed || !parsed->is_dict())
    return std::nullopt;
  return std::move(parsed->GetDict());
}

}  // namespace

char* zebkit_pdf_annotations(zebkit_pdf* pdf, int index, char** error) {
  base::AutoLock lock(PdfiumLock());
  Page page(pdf, index);
  if (!page.get())
    return Fail(error, "no page " + std::to_string(index));
  base::ListValue list;
  const int count = FPDFPage_GetAnnotCount(page.get());
  for (int i = 0; i < count; ++i) {
    FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page.get(), i);
    if (!annot)
      continue;
    list.Append(DescribeAnnotation(pdf, page.get(), annot, i));
    FPDFPage_CloseAnnot(annot);
  }
  return GiveJson(base::Value(std::move(list)));
}

int zebkit_pdf_annotation_add(zebkit_pdf* pdf,
                              int index,
                              const char* json,
                              char** error) {
  std::optional<base::DictValue> object = ParseObject(json);
  if (!object) {
    Fail(error, "the annotation is not a JSON object");
    return -1;
  }
  const std::string* type = object->FindString("type");
  int subtype = -1;
  for (size_t i = 0; type && i < std::size(kAnnotationTypes); ++i) {
    if (*type == kAnnotationTypes[i])
      subtype = static_cast<int>(i);
  }
  if (subtype < 0 || !FPDFAnnot_IsSupportedSubtype(subtype)) {
    Fail(error, "an annotation of type " + (type ? *type : "(none)") +
                    " cannot be made");
    return -1;
  }
  if (!object->Find("rect")) {
    Fail(error, "an annotation needs a rect");
    return -1;
  }
  base::AutoLock lock(PdfiumLock());
  Page page(pdf, index);
  if (!page.get()) {
    Fail(error, "no page " + std::to_string(index));
    return -1;
  }
  FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page.get(), subtype);
  if (!annot) {
    Fail(error, "the annotation cannot be made");
    return -1;
  }
  const std::string problem = ApplyAnnotation(pdf, annot, *object, true);
  const int added = FPDFPage_GetAnnotIndex(page.get(), annot);
  FPDFPage_CloseAnnot(annot);
  if (!problem.empty()) {
    FPDFPage_RemoveAnnot(page.get(), added);
    Fail(error, problem);
    return -1;
  }
  return added;
}

bool zebkit_pdf_annotation_update(zebkit_pdf* pdf,
                                  int index,
                                  int annotation,
                                  const char* json,
                                  char** error) {
  std::optional<base::DictValue> object = ParseObject(json);
  if (!object)
    return FailFalse(error, "the changes are not a JSON object");
  base::AutoLock lock(PdfiumLock());
  Page page(pdf, index);
  if (!page.get())
    return FailFalse(error, "no page " + std::to_string(index));
  FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page.get(), annotation);
  if (!annot)
    return FailFalse(error, "no annotation " + std::to_string(annotation));
  const std::string problem = ApplyAnnotation(pdf, annot, *object, false);
  FPDFPage_CloseAnnot(annot);
  if (!problem.empty())
    return FailFalse(error, problem);
  return true;
}

bool zebkit_pdf_annotation_remove(zebkit_pdf* pdf,
                                  int index,
                                  int annotation,
                                  char** error) {
  base::AutoLock lock(PdfiumLock());
  Page page(pdf, index);
  if (!page.get())
    return FailFalse(error, "no page " + std::to_string(index));
  if (!FPDFPage_RemoveAnnot(page.get(), annotation))
    return FailFalse(error, "no annotation " + std::to_string(annotation));
  return true;
}

bool zebkit_pdf_flatten(zebkit_pdf* pdf,
                        int index,
                        bool for_print,
                        char** error) {
  base::AutoLock lock(PdfiumLock());
  Page page(pdf, index);
  if (!page.get())
    return FailFalse(error, "no page " + std::to_string(index));
  if (FPDFPage_Flatten(page.get(), for_print ? FLAT_PRINT
                                             : FLAT_NORMALDISPLAY) ==
      FLATTEN_FAIL) {
    return FailFalse(error, "the page cannot be flattened");
  }
  return true;
}

int zebkit_pdf_redact(zebkit_pdf* pdf,
                      int index,
                      const char* areas_json,
                      uint32_t fill,
                      char** error) {
  std::vector<std::array<float, 4>> areas;
  if (areas_json) {
    std::optional<base::Value> parsed = base::JSONReader::Read(
        areas_json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
    if (!parsed || !parsed->is_list()) {
      Fail(error, "the areas are not a JSON array of [l, b, r, t]");
      return -1;
    }
    for (const base::Value& item : parsed->GetList()) {
      std::optional<std::vector<float>> r = Numbers(&item, 4);
      if (!r) {
        Fail(error, "an area is not [l, b, r, t]");
        return -1;
      }
      areas.push_back({(*r)[0], (*r)[1], (*r)[2], (*r)[3]});
    }
  }
  base::AutoLock lock(PdfiumLock());
  Page page(pdf, index);
  if (!page.get()) {
    Fail(error, "no page " + std::to_string(index));
    return -1;
  }
  // The page's redaction marks: their quadrilaterals, or their rectangles.
  std::vector<int> marks;
  for (int i = FPDFPage_GetAnnotCount(page.get()) - 1; i >= 0; --i) {
    FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page.get(), i);
    if (!annot)
      continue;
    if (FPDFAnnot_GetSubtype(annot) == FPDF_ANNOT_REDACT) {
      marks.push_back(i);
      if (!areas_json) {
        const size_t quads = FPDFAnnot_CountAttachmentPoints(annot);
        for (size_t q = 0; q < quads; ++q) {
          FS_QUADPOINTSF quad = {};
          if (!FPDFAnnot_GetAttachmentPoints(annot, q, &quad))
            continue;
          areas.push_back(
              {std::min({quad.x1, quad.x2, quad.x3, quad.x4}),
               std::min({quad.y1, quad.y2, quad.y3, quad.y4}),
               std::max({quad.x1, quad.x2, quad.x3, quad.x4}),
               std::max({quad.y1, quad.y2, quad.y3, quad.y4})});
        }
        FS_RECTF rect = {};
        if (!quads && FPDFAnnot_GetRect(annot, &rect))
          areas.push_back({rect.left, rect.bottom, rect.right, rect.top});
      }
    }
    FPDFPage_CloseAnnot(annot);
  }
  if (areas.empty())
    return 0;
  auto overlaps = [&](float left, float bottom, float right, float top) {
    for (const auto& a : areas) {
      if (left < a[2] && right > a[0] && bottom < a[3] && top > a[1])
        return true;
    }
    return false;
  };
  // Text, paths, shadings and forms.
  if (zebkit::pdf::RedactObjects(page.get(), areas) < 0) {
    Fail(error, "the page cannot be changed");
    return -1;
  }
  // Images: their pixels under the areas, black.
  const int objects = FPDFPage_CountObjects(page.get());
  for (int i = 0; i < objects; ++i) {
    FPDF_PAGEOBJECT object = FPDFPage_GetObject(page.get(), i);
    float left = 0, bottom = 0, right = 0, top = 0;
    if (!object || FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_IMAGE ||
        !FPDFPageObj_GetBounds(object, &left, &bottom, &right, &top) ||
        !overlaps(left, bottom, right, top)) {
      continue;
    }
    FS_MATRIX matrix = {};
    FPDF_BITMAP bitmap = FPDFImageObj_GetBitmap(object);
    if (!bitmap || !FPDFPageObj_GetMatrix(object, &matrix)) {
      if (bitmap)
        FPDFBitmap_Destroy(bitmap);
      FPDFPage_RemoveObject(page.get(), object);
      FPDFPageObj_Destroy(object);
      --i;
      continue;
    }
    const int width = FPDFBitmap_GetWidth(bitmap);
    const int height = FPDFBitmap_GetHeight(bitmap);
    // Page space to the image's unit square: the inverse of its matrix.
    const double det = matrix.a * matrix.d - matrix.b * matrix.c;
    for (const auto& a : areas) {
      if (det == 0)
        break;
      double min_u = 1, min_v = 1, max_u = 0, max_v = 0;
      for (float x : {a[0], a[2]}) {
        for (float y : {a[1], a[3]}) {
          const double dx = x - matrix.e, dy = y - matrix.f;
          const double u = (matrix.d * dx - matrix.c * dy) / det;
          const double v = (-matrix.b * dx + matrix.a * dy) / det;
          min_u = std::min(min_u, u);
          max_u = std::max(max_u, u);
          min_v = std::min(min_v, v);
          max_v = std::max(max_v, v);
        }
      }
      // Rows run from the top of the image, v from its bottom.
      const int x0 = std::clamp(static_cast<int>(std::floor(min_u * width)), 0, width);
      const int x1 = std::clamp(static_cast<int>(std::ceil(max_u * width)), 0, width);
      const int y0 = std::clamp(static_cast<int>(std::floor((1 - max_v) * height)), 0, height);
      const int y1 = std::clamp(static_cast<int>(std::ceil((1 - min_v) * height)), 0, height);
      if (x1 > x0 && y1 > y0)
        FPDFBitmap_FillRect(bitmap, x0, y0, x1 - x0, y1 - y0, 0xff000000);
    }
    FPDF_PAGE pages[] = {page.get()};
    FPDFImageObj_SetBitmap(pages, 1, object, bitmap);
    FPDFBitmap_Destroy(bitmap);
  }
  // Boxes over the areas.
  if (fill & 0xff) {
    for (const auto& a : areas) {
      FPDF_PAGEOBJECT box =
          FPDFPageObj_CreateNewRect(a[0], a[1], a[2] - a[0], a[3] - a[1]);
      FPDFPageObj_SetFillColor(box, fill >> 24, (fill >> 16) & 0xff,
                               (fill >> 8) & 0xff, fill & 0xff);
      FPDFPath_SetDrawMode(box, FPDF_FILLMODE_WINDING, false);
      FPDFPage_InsertObject(page.get(), box);
    }
  }
  if (!FPDFPage_GenerateContent(page.get())) {
    Fail(error, "the page's content cannot be written");
    return -1;
  }
  // The marks, and annotations that reach into an area.
  for (int i = FPDFPage_GetAnnotCount(page.get()) - 1; i >= 0; --i) {
    FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page.get(), i);
    if (!annot)
      continue;
    FS_RECTF rect = {};
    const bool mark = FPDFAnnot_GetSubtype(annot) == FPDF_ANNOT_REDACT;
    const bool reaches = FPDFAnnot_GetRect(annot, &rect) &&
                         overlaps(rect.left, std::min(rect.bottom, rect.top),
                                  rect.right, std::max(rect.bottom, rect.top));
    FPDFPage_CloseAnnot(annot);
    if (mark || reaches)
      FPDFPage_RemoveAnnot(page.get(), i);
  }
  return static_cast<int>(areas.size());
}

char* zebkit_pdf_form_fields(zebkit_pdf* pdf, char** error) {
  if (!pdf)
    return Fail(error, "no document");
  base::AutoLock lock(PdfiumLock());
  FPDF_FORMHANDLE form = Form(pdf);
  if (!form)
    return Fail(error, "the document's form cannot be read");
  static constexpr const char* kTypes[] = {
      "unknown", "pushbutton", "checkbox", "radio",
      "combobox", "listbox",   "text",     "signature",
  };
  // One entry per field, in the order first seen.
  std::vector<std::string> order;
  std::map<std::string, base::DictValue> fields;
  const int pages = FPDF_GetPageCount(pdf->document);
  for (int index = 0; index < pages; ++index) {
    Page page(pdf, index);
    if (!page.get())
      continue;
    FORM_OnAfterLoadPage(page.get(), form);
    const int count = FPDFPage_GetAnnotCount(page.get());
    for (int i = 0; i < count; ++i) {
      FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page.get(), i);
      if (!annot)
        continue;
      if (FPDFAnnot_GetSubtype(annot) != FPDF_ANNOT_WIDGET) {
        FPDFPage_CloseAnnot(annot);
        continue;
      }
      auto text = [&](auto get) {
        return Utf16String([&](void* buffer, unsigned long size) {
          return get(form, annot, static_cast<FPDF_WCHAR*>(buffer), size);
        });
      };
      const std::string name = text(&FPDFAnnot_GetFormFieldName);
      auto [field, added] = fields.try_emplace(name);
      const int type = FPDFAnnot_GetFormFieldType(form, annot);
      if (added) {
        order.push_back(name);
        base::DictValue& f = field->second;
        f.Set("name", name);
        f.Set("type", type >= 0 && type < static_cast<int>(std::size(kTypes))
                          ? kTypes[type]
                          : "unknown");
        const int flags = FPDFAnnot_GetFormFieldFlags(form, annot);
        f.Set("flags", flags);
        f.Set("readOnly", (flags & FPDF_FORMFLAG_READONLY) != 0);
        f.Set("required", (flags & FPDF_FORMFLAG_REQUIRED) != 0);
        f.Set("value", text(&FPDFAnnot_GetFormFieldValue));
        const int options = FPDFAnnot_GetOptionCount(form, annot);
        if (options > 0) {
          base::ListValue list;
          for (int o = 0; o < options; ++o) {
            base::DictValue option;
            option.Set("label", Utf16String([&](void* buffer,
                                                 unsigned long size) {
                         return FPDFAnnot_GetOptionLabel(
                             form, annot, o, static_cast<FPDF_WCHAR*>(buffer),
                             size);
                       }));
            option.Set("selected",
                       static_cast<bool>(
                           FPDFAnnot_IsOptionSelected(form, annot, o)));
            list.Append(std::move(option));
          }
          f.Set("options", std::move(list));
        }
        f.Set("widgets", base::ListValue());
      }
      base::DictValue widget;
      widget.Set("page", index);
      widget.Set("annotation", i);
      FS_RECTF rect = {};
      if (FPDFAnnot_GetRect(annot, &rect))
        widget.Set("rect", PdfRect(rect.left, rect.bottom, rect.right, rect.top));
      if (type == FPDF_FORMFIELD_CHECKBOX ||
          type == FPDF_FORMFIELD_RADIOBUTTON) {
        widget.Set("checked", static_cast<bool>(FPDFAnnot_IsChecked(form, annot)));
        widget.Set("exportValue", text(&FPDFAnnot_GetFormFieldExportValue));
      }
      field->second.FindList("widgets")->Append(std::move(widget));
      FPDFPage_CloseAnnot(annot);
    }
    FORM_OnBeforeClosePage(page.get(), form);
  }
  base::ListValue list;
  for (const std::string& name : order)
    list.Append(std::move(fields[name]));
  return GiveJson(base::Value(std::move(list)));
}

bool zebkit_pdf_form_set(zebkit_pdf* pdf,
                         const char* name,
                         const char* value,
                         char** error) {
  if (!pdf)
    return FailFalse(error, "no document");
  if (!name || !value)
    return FailFalse(error, "a field's name and a value");
  base::AutoLock lock(PdfiumLock());
  FPDF_FORMHANDLE form = Form(pdf);
  if (!form)
    return FailFalse(error, "the document's form cannot be read");
  const std::string problem = zebkit::pdf::SetFieldValue(form, name, value);
  if (!problem.empty())
    return FailFalse(error, problem);
  return true;
}

bool zebkit_pdf_form_reset(zebkit_pdf* pdf, char** error) {
  if (!pdf)
    return FailFalse(error, "no document");
  base::AutoLock lock(PdfiumLock());
  FPDF_FORMHANDLE form = Form(pdf);
  if (!form || !zebkit::pdf::ResetForm(form))
    return FailFalse(error, "the document has no form");
  return true;
}

bool zebkit_pdf_page_rotate(zebkit_pdf* pdf,
                            int index,
                            int rotation,
                            char** error) {
  if (rotation % 90)
    return FailFalse(error, "the rotation is not a multiple of 90");
  base::AutoLock lock(PdfiumLock());
  Page page(pdf, index);
  if (!page.get())
    return FailFalse(error, "no page " + std::to_string(index));
  FPDFPage_SetRotation(page.get(), ((rotation / 90) % 4 + 4) % 4);
  return true;
}

namespace {

// @count page indexes, each a page of @pages pages, none twice.
bool ValidPages(const int* indexes, int count, int pages) {
  if (!indexes || count <= 0)
    return false;
  std::vector<bool> seen(pages, false);
  for (int i = 0; i < count; ++i) {
    if (indexes[i] < 0 || indexes[i] >= pages || seen[indexes[i]])
      return false;
    seen[indexes[i]] = true;
  }
  return true;
}

}  // namespace

bool zebkit_pdf_pages_delete(zebkit_pdf* pdf,
                             const int* pages,
                             int count,
                             char** error) {
  if (!pdf)
    return FailFalse(error, "no document");
  base::AutoLock lock(PdfiumLock());
  const int total = FPDF_GetPageCount(pdf->document);
  if (!ValidPages(pages, count, total))
    return FailFalse(error, "not a list of pages of the document");
  std::vector<int> sorted(pages, pages + count);
  std::sort(sorted.rbegin(), sorted.rend());
  for (int index : sorted)
    FPDFPage_Delete(pdf->document, index);
  return true;
}

bool zebkit_pdf_pages_move(zebkit_pdf* pdf,
                           const int* pages,
                           int count,
                           int to,
                           char** error) {
  if (!pdf)
    return FailFalse(error, "no document");
  base::AutoLock lock(PdfiumLock());
  const int total = FPDF_GetPageCount(pdf->document);
  if (!ValidPages(pages, count, total) || to < 0 || to > total - count)
    return FailFalse(error, "not a list of pages, or nowhere to move them");
  if (!FPDF_MovePages(pdf->document, pages, static_cast<unsigned long>(count),
                      to)) {
    return FailFalse(error, "the pages cannot be moved");
  }
  return true;
}

bool zebkit_pdf_page_insert(zebkit_pdf* pdf,
                            int at,
                            double width,
                            double height,
                            char** error) {
  if (!pdf)
    return FailFalse(error, "no document");
  if (!(width > 0 && height > 0 && width <= 14400 && height <= 14400))
    return FailFalse(error, "a page is 1 to 14400 points each way");
  base::AutoLock lock(PdfiumLock());
  const int total = FPDF_GetPageCount(pdf->document);
  if (at < 0 || at > total)
    return FailFalse(error, "no place " + std::to_string(at));
  FPDF_PAGE page = FPDFPage_New(pdf->document, at, width, height);
  if (!page)
    return FailFalse(error, "the page cannot be made");
  FPDF_ClosePage(page);
  return true;
}

bool zebkit_pdf_pages_import(zebkit_pdf* pdf,
                             zebkit_pdf* from,
                             const int* pages,
                             int count,
                             int at,
                             char** error) {
  if (!pdf || !from)
    return FailFalse(error, "no document");
  if (pdf == from)
    return FailFalse(error, "a document's pages are copied from another");
  base::AutoLock lock(PdfiumLock());
  const int total = FPDF_GetPageCount(pdf->document);
  if (at < 0 || at > total)
    return FailFalse(error, "no place " + std::to_string(at));
  if (pages &&
      !ValidPages(pages, count, FPDF_GetPageCount(from->document))) {
    return FailFalse(error, "not a list of pages of the other document");
  }
  if (!FPDF_ImportPagesByIndex(pdf->document, from->document, pages,
                               pages ? static_cast<unsigned long>(count) : 0,
                               at)) {
    return FailFalse(error, "the pages cannot be copied");
  }
  return true;
}

zebkit_pdf* zebkit_pdf_new(char** error) {
  base::AutoLock lock(PdfiumLock());
  InitializeOnce();
  auto pdf = std::make_unique<zebkit_pdf>();
  pdf->document = FPDF_CreateNewDocument();
  if (!pdf->document)
    return Fail(error, "the document cannot be made");
  return pdf.release();
}

char* zebkit_pdf_attachments(zebkit_pdf* pdf, char** error) {
  if (!pdf)
    return Fail(error, "no document");
  base::AutoLock lock(PdfiumLock());
  base::ListValue list;
  const int count = FPDFDoc_GetAttachmentCount(pdf->document);
  for (int i = 0; i < count; ++i) {
    FPDF_ATTACHMENT attachment = FPDFDoc_GetAttachment(pdf->document, i);
    base::DictValue item;
    item.Set("name", Utf16String([&](void* buffer, unsigned long size) {
               return FPDFAttachment_GetName(
                   attachment, static_cast<FPDF_WCHAR*>(buffer), size);
             }));
    unsigned long size = 0;
    if (FPDFAttachment_GetFile(attachment, nullptr, 0, &size))
      item.Set("size", static_cast<double>(size));
    list.Append(std::move(item));
  }
  return GiveJson(base::Value(std::move(list)));
}

uint8_t* zebkit_pdf_attachment(zebkit_pdf* pdf,
                               int index,
                               size_t* size,
                               char** error) {
  if (!pdf)
    return Fail(error, "no document");
  base::AutoLock lock(PdfiumLock());
  FPDF_ATTACHMENT attachment = FPDFDoc_GetAttachment(pdf->document, index);
  unsigned long length = 0;
  if (!attachment || !FPDFAttachment_GetFile(attachment, nullptr, 0, &length))
    return Fail(error, "no attachment " + std::to_string(index));
  std::vector<uint8_t> bytes(length);
  FPDFAttachment_GetFile(attachment, bytes.data(), length, &length);
  return GiveBytes(bytes.data(), length, size);
}

bool zebkit_pdf_attach(zebkit_pdf* pdf,
                       const char* name,
                       const void* data,
                       size_t size,
                       char** error) {
  if (!pdf)
    return FailFalse(error, "no document");
  if (!name || !*name)
    return FailFalse(error, "no name");
  if (size > 0xffffffffu)
    return FailFalse(error, "the file is too large");
  base::AutoLock lock(PdfiumLock());
  const std::u16string wide = Utf16(name);
  FPDF_ATTACHMENT attachment = FPDFDoc_AddAttachment(pdf->document, Wide(wide));
  if (!attachment)
    return FailFalse(error, "an attachment of that name exists");
  if (!FPDFAttachment_SetFile(attachment, pdf->document, data,
                              static_cast<unsigned long>(size))) {
    return FailFalse(error, "the file cannot be attached");
  }
  return true;
}

bool zebkit_pdf_detach(zebkit_pdf* pdf, int index, char** error) {
  if (!pdf)
    return FailFalse(error, "no document");
  base::AutoLock lock(PdfiumLock());
  if (!FPDFDoc_DeleteAttachment(pdf->document, index))
    return FailFalse(error, "no attachment " + std::to_string(index));
  return true;
}

}  // extern "C"
