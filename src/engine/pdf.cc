// <zebkit/pdf.h>: documents on Chromium's PDFium, through its public API;
// what that does not reach is in pdf_internal.cc. Compiled with PDFium's
// own configuration (//zebkit/engine:pdf).

#ifdef UNSAFE_BUFFERS_BUILD
// The C API takes and returns C strings and arrays, as PDFium's does.
#pragma allow_unsafe_buffers
#endif

#include "zebkit/engine/include/zebkit/pdf.h"
#include "zebkit/engine/include/zebkit/services.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <map>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/containers/fixed_flat_set.h"
#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/synchronization/lock.h"
#include "base/time/time.h"
#include "base/values.h"
#include "public/fpdf_annot.h"
#include "third_party/boringssl/src/include/openssl/bn.h"
#include "third_party/boringssl/src/include/openssl/bytestring.h"
#include "third_party/boringssl/src/include/openssl/digest.h"
#include "third_party/boringssl/src/include/openssl/evp.h"
#include "third_party/boringssl/src/include/openssl/nid.h"
#include "third_party/boringssl/src/include/openssl/obj.h"
#include "third_party/boringssl/src/include/openssl/rsa.h"
#include "third_party/boringssl/src/include/openssl/mem.h"
#include "third_party/boringssl/src/include/openssl/pkcs7.h"
#include "third_party/boringssl/src/include/openssl/x509.h"
#include "public/fpdf_attachment.h"
#include "public/fpdf_ppo.h"
#include "public/fpdf_flatten.h"
#include "public/fpdf_catalog.h"
#include "public/fpdf_doc.h"
#include "public/fpdf_edit.h"
#include "public/fpdf_formfill.h"
#include "public/fpdf_save.h"
#include "public/fpdf_signature.h"
#include "public/fpdf_text.h"
#include "public/fpdf_transformpage.h"
#include "public/fpdfview.h"
#include "zebkit/engine/canvas_internal.h"
#include "zebkit/engine/pdf_appearance.h"
#include "zebkit/engine/pdf_document.h"
#include "zebkit/engine/pdf_hooks.h"
#include "zebkit/engine/pdf_internal.h"

using zebkit::pdf::Hook;
using zebkit::pdf::HookType;
using zebkit::pdf::Hooks;
using zebkit::pdf::HooksFor;
using zebkit::pdf::HasEventHooks;
using zebkit::pdf::RunAfter;
using zebkit::pdf::RunBefore;

namespace {

// PDFium is not thread-safe, documents or not: one call at a time.
base::Lock& PdfiumLock() {
  static base::NoDestructor<base::Lock> lock;
  return *lock;
}

}  // namespace

namespace zebkit::pdf {

void DestroyDocument(zebkit_pdf* pdf) {
  RemoveHooksOf(pdf);
  base::AutoLock lock(PdfiumLock());
  CloseAppearanceFonts(pdf->fonts);
  if (pdf->form)
    FPDFDOC_ExitFormFillEnvironment(pdf->form);
  if (pdf->document)
    FPDF_CloseDocument(pdf->document);
  delete pdf;
}

}  // namespace zebkit::pdf

namespace {

void InitializeOnce() {
  static const bool initialized = [] {
    FPDF_LIBRARY_CONFIG config = {};
    config.version = 2;
    FPDF_InitLibraryWithConfig(&config);
    return true;
  }();
  (void)initialized;
}

using zebkit::pdf::ApiScope;
using zebkit::pdf::DestroyDocument;

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
  // A page whose content is parsed, for rendering, text and objects; or,
  // with @parse false, one for its dictionary and annotations alone, which
  // costs far less to open.
  Page(zebkit_pdf* pdf, int index, bool parse = true)
      : page_(pdf && pdf->document && index >= 0 &&
                      index < FPDF_GetPageCount(pdf->document)
                  ? (parse ? FPDF_LoadPage(pdf->document, index)
                           : zebkit::pdf::LoadPageUnparsed(pdf->document,
                                                           index))
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
  // A dangling destination has no page, and coordinates mean nothing without
  // one: zebkit_pdf_set_outline() refuses them, as it should.
  if (page < 0)
    return base::Value(std::move(place));
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

bool ParseOutline(const base::ListValue& list, int pages, int depth,
                  size_t* count, std::vector<zebkit::pdf::OutlineItem>* items,
                  std::string* error) {
  if (depth > 64 || list.size() > 100000 - *count) {
    *error = "the outline exceeds 64 levels or 100000 items";
    return false;
  }
  for (const auto& value : list) {
    if (++*count > 100000 || !value.is_dict()) {
      *error = "outline items must be objects (at most 100000)"; return false;
    }
    const auto& dict = value.GetDict();
    for (auto entry : dict) {
      const auto& key = entry.first;
      if (key != "title" && key != "page" && key != "x" && key != "y" &&
          key != "zoom" && key != "url" && key != "open" && key != "children") {
        *error = "unknown outline entry: " + key; return false;
      }
    }
    zebkit::pdf::OutlineItem item;
    const std::string* title = dict.FindString("title");
    if (!title || title->size() > 1024 * 1024) {
      *error = "an outline title must be a string of at most 1 MiB"; return false;
    }
    item.title = *title;
    if (dict.contains("page")) {
      auto page = dict.FindInt("page");
      if (!page || *page < 0 || *page >= pages) {
        *error = "an outline page is outside the document"; return false;
      }
      item.page = *page;
    }
    if (dict.contains("url")) {
      const std::string* url = dict.FindString("url");
      if (!url || url->size() > 1024 * 1024 || url->find('\0') != std::string::npos || item.page >= 0) {
        *error = "an outline URL must be a string, without page or NUL"; return false;
      }
      item.url = *url;
    }
    for (auto coordinate : {std::pair{"x", &item.x}, {"y", &item.y}, {"zoom", &item.zoom}}) {
      if (!dict.contains(coordinate.first)) continue;
      auto number = dict.FindDouble(coordinate.first);
      if (item.page < 0 || !number || !std::isfinite(*number) ||
          std::abs(*number) > std::numeric_limits<float>::max() ||
          (std::string_view(coordinate.first) == "zoom" && *number < 0)) {
        *error = "outline coordinates need a page and finite numbers; zoom is nonnegative"; return false;
      }
      *coordinate.second = static_cast<float>(*number);
    }
    if (dict.contains("open")) {
      auto open = dict.FindBool("open");
      if (!open) { *error = "outline open must be a boolean"; return false; }
      item.open = *open;
    }
    if (dict.contains("children")) {
      const auto* children = dict.FindList("children");
      if (!children) { *error = "outline children must be an array"; return false; }
      if (!ParseOutline(*children, pages, depth + 1, count, &item.children, error)) return false;
    }
    items->push_back(std::move(item));
  }
  return true;
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

static zebkit_pdf* OpenBytes(const void* data,
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

static zebkit_pdf* OpenPath(const char* path,
                            const char* password,
                            char** error) {
  if (!path || !*path)
    return Fail(error, "no path");
  base::AutoLock lock(PdfiumLock());
  InitializeOnce();
  auto pdf = std::make_unique<zebkit_pdf>();
  pdf->path = path;
  pdf->document = FPDF_LoadDocument(path, password);
  if (!pdf->document)
    return Fail(error, LastError());
  return pdf.release();
}

// The password a "before" hook left in @operation, or the caller's.
static zebkit_pdf* OpenWithHooks(const void* data,
                                 size_t size,
                                 const char* path,
                                 const char* password,
                                 char** error) {
  std::string changed_password;
  if (HasEventHooks(nullptr, "open", true)) {
    base::DictValue operation;
    if (path)
      operation.Set("path", path);
    else
      operation.Set("size", static_cast<double>(size));
    if (password)
      operation.Set("password", password);
    if (!RunBefore(nullptr, "open", operation, error))
      return nullptr;
    if (const std::string* given = operation.FindString("password")) {
      changed_password = *given;
      password = changed_password.c_str();
    } else {
      password = nullptr;
    }
  }
  zebkit_pdf* pdf =
      path ? OpenPath(path, password, error) : OpenBytes(data, size, password, error);
  if (pdf && HasEventHooks(pdf, "open", false)) {
    bool closed = false;
    {
      ApiScope scope(pdf);
      base::DictValue operation;
      operation.Set("pages", zebkit_pdf_page_count(pdf));
      RunAfter(pdf, "open", std::move(operation));
      closed = pdf->closing;
    }
    // It went with the scope: the caller gets none to use or close.
    if (closed)
      return Fail(error, "a hook closed the document");
  }
  return pdf;
}

zebkit_pdf* zebkit_pdf_open(const void* data,
                            size_t size,
                            const char* password,
                            char** error) {
  return OpenWithHooks(data, size, nullptr, password, error);
}

zebkit_pdf* zebkit_pdf_open_file(const char* path,
                                 const char* password,
                                 char** error) {
  if (!path || !*path)
    return Fail(error, "no path");
  return OpenWithHooks(nullptr, 0, path, password, error);
}

void zebkit_pdf_close(zebkit_pdf* pdf) {
  if (!pdf)
    return;
  // A hook's call: the call that ran the hook still uses it.
  if (pdf->depth > 0) {
    pdf->closing = true;
    return;
  }
  DestroyDocument(pdf);
}

uint8_t* zebkit_pdf_save(zebkit_pdf* pdf,
                         unsigned flags,
                         size_t* size,
                         char** error) {
  if (!pdf)
    return Fail(error, "no document");
  ApiScope scope(pdf);
  if (HasEventHooks(pdf, "save", true)) {
    base::DictValue operation;
    operation.Set("flags", static_cast<int>(flags));
    if (!RunBefore(pdf, "save", operation, error))
      return nullptr;
    flags = static_cast<unsigned>(operation.FindInt("flags").value_or(static_cast<int>(flags)));
  }
  size_t written = 0;
  uint8_t* file = nullptr;
  {
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
  file = GiveBytes(writer.bytes.data(), writer.bytes.size(), &written);
  }
  if (size)
    *size = written;
  if (file && HasEventHooks(pdf, "save", false)) {
    base::DictValue operation;
    operation.Set("flags", static_cast<int>(flags));
    operation.Set("size", static_cast<double>(written));
    RunAfter(pdf, "save", std::move(operation));
  }
  return file;
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
  // The bits as they are, the owner's and the user's (without the owner
  // password).
  info.Set("permissionBits", static_cast<double>(bits));
  info.Set("userPermissionBits",
           static_cast<double>(FPDF_GetDocUserPermissions(document)));
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
  const std::string form_da = zebkit::pdf::FormDefaultAppearance(document);
  if (!form_da.empty())
    info.Set("formDefaultAppearance", form_da);
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
  Page page(pdf, index, /*parse=*/false);
  if (!page.get())
    return Fail(error, "no page " + std::to_string(index));
  base::DictValue info;
  FS_SIZEF size = {};
  FPDF_GetPageSizeByIndexF(pdf->document, index, &size);
  info.Set("width", size.width);
  info.Set("height", size.height);
  info.Set("rotation", FPDFPage_GetRotation(page.get()) * 90);
  info.Set("objectNumber", zebkit::pdf::PageObjectNumber(page.get()));
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

namespace {

// What FPDFBitmap_FillRect() takes for @rgba (0xRRGGBBAA): it writes
// 0xAARRGGBB as B, G, R, A in memory, and pages are rendered as R, G, B, A
// (FPDF_REVERSE_BYTE_ORDER), so red and blue change places here.
uint32_t Argb(uint32_t rgba) {
  return ((rgba & 0xff) << 24) | (((rgba >> 8) & 0xff) << 16) |
         (((rgba >> 16) & 0xff) << 8) | (rgba >> 24);
}

// The bitmap's pixels, R, G, B, A in rows without padding, for the caller;
// the bitmap is destroyed.
uint8_t* TakePixels(FPDF_BITMAP bitmap, int* width, int* height, char** error) {
  const int w = FPDFBitmap_GetWidth(bitmap);
  const int h = FPDFBitmap_GetHeight(bitmap);
  // Not premultiplied, as PDFium's bitmaps are.
  const uint8_t* pixels =
      static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(bitmap));
  const int stride = FPDFBitmap_GetStride(bitmap);
  const size_t row = static_cast<size_t>(w) * 4;
  uint8_t* out = static_cast<uint8_t*>(malloc(row * h));
  if (out) {
    for (int y = 0; y < h; ++y)
      memcpy(out + row * y, pixels + static_cast<size_t>(stride) * y, row);
  }
  FPDFBitmap_Destroy(bitmap);
  if (!out)
    return Fail(error, "not enough memory for the image");
  if (width)
    *width = w;
  if (height)
    *height = h;
  return out;
}

}  // namespace

// @layers: whether the layers hooks draw go on it.
static uint8_t* RenderPage(zebkit_pdf* pdf,
                           int index,
                           const zebkit_pdf_render_options& options,
                           bool layers,
                           int* width,
                           int* height,
                           char** error) {
  const float scale = options.scale > 0 ? options.scale : 1;
  if (options.rotation % 90)
    return Fail(error, "the rotation is not a multiple of 90");
  // Before the lock: a hook that removed itself is destroyed when this goes,
  // and its destroy function may call in.
  std::vector<Hooks> ran;
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
  FPDFBitmap_FillRect(bitmap, 0, 0, tile_width, tile_height,
                      Argb(options.background));
  // Layers draw on the bitmap, outside the lock: their hooks may call in.
  zebkit_pdf_layer_info info = {};
  info.width = tile_width;
  info.height = tile_height;
  info.scale = scale;
  info.rotation = options.rotation;
  zebkit::pdf::PageDisplayMatrix(page.get(), -tile_x, -tile_y, full_width, full_height,
                                 quarter_turns, info.matrix);
  auto draw_layers = [&](const char* position) {
    if (!layers)
      return;
    const Hooks hooks = HooksFor(HookType::kLayer, pdf, position);
    if (hooks.empty())
      return;
    ran.push_back(hooks);
    base::AutoUnlock unlock(PdfiumLock());
    zebkit_canvas* canvas = zebkit::WrapCanvasInPageSpace(
        static_cast<uint8_t*>(FPDFBitmap_GetBuffer(bitmap)), tile_width, tile_height,
        static_cast<size_t>(FPDFBitmap_GetStride(bitmap)), info.matrix);
    if (!canvas)
      return;
    for (const auto& hook : hooks) {
      if (hook->removed.load() || pdf->closing)
        continue;
      zebkit_canvas_save(canvas);
      reinterpret_cast<zebkit_pdf_layer_func>(hook->func)(canvas, pdf, index, &info, hook->data);
      zebkit_canvas_restore(canvas);
    }
    zebkit_canvas_close(canvas);
  };
  draw_layers("below");
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
  draw_layers("above");
  return TakePixels(bitmap, width, height, error);
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
  ApiScope scope(pdf);
  if (HasEventHooks(pdf, "render", true)) {
    base::DictValue operation;
    operation.Set("page", index);
    base::DictValue fields;
    fields.Set("scale", options.scale);
    fields.Set("rotation", options.rotation);
    fields.Set("flags", static_cast<int>(options.flags));
    fields.Set("background", static_cast<double>(options.background));
    fields.Set("x", options.x);
    fields.Set("y", options.y);
    fields.Set("width", options.width);
    fields.Set("height", options.height);
    operation.Set("options", std::move(fields));
    if (!RunBefore(pdf, "render", operation, error))
      return nullptr;
    index = operation.FindInt("page").value_or(index);
    if (const base::DictValue* changed = operation.FindDict("options")) {
      options.scale = static_cast<float>(changed->FindDouble("scale").value_or(options.scale));
      options.rotation = changed->FindInt("rotation").value_or(options.rotation);
      options.flags = static_cast<unsigned>(changed->FindInt("flags").value_or(static_cast<int>(options.flags)));
      options.background = static_cast<uint32_t>(changed->FindDouble("background").value_or(options.background));
      options.x = changed->FindInt("x").value_or(options.x);
      options.y = changed->FindInt("y").value_or(options.y);
      options.width = changed->FindInt("width").value_or(options.width);
      options.height = changed->FindInt("height").value_or(options.height);
    }
  }
  int out_width = 0, out_height = 0;
  uint8_t* pixels = RenderPage(pdf, index, options, true, &out_width, &out_height, error);
  if (pixels) {
    if (width)
      *width = out_width;
    if (height)
      *height = out_height;
    if (HasEventHooks(pdf, "render", false)) {
      base::DictValue operation;
      operation.Set("page", index);
      operation.Set("width", out_width);
      operation.Set("height", out_height);
      RunAfter(pdf, "render", std::move(operation));
    }
  }
  return pixels;
}

uint8_t* zebkit_pdf_render_annotation(zebkit_pdf* pdf,
                                      int index,
                                      int annotation,
                                      int mode,
                                      const zebkit_pdf_render_options* given,
                                      int* width,
                                      int* height,
                                      char** error) {
  zebkit_pdf_render_options options = {};
  if (given)
    options = *given;
  const float scale = options.scale > 0 ? options.scale : 1;
  if (options.rotation % 90)
    return Fail(error, "the rotation is not a multiple of 90");
  if (mode < 0 || mode > 2)
    return Fail(error, "the mode is 0, 1 or 2");
  base::AutoLock lock(PdfiumLock());
  Page page(pdf, index);
  if (!page.get())
    return Fail(error, "no page " + std::to_string(index));
  FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page.get(), annotation);
  if (!annot)
    return Fail(error, "no annotation " + std::to_string(annotation));
  FS_RECTF rect = {};
  if (!FPDFAnnot_GetRect(annot, &rect) ||
      !(zebkit::pdf::AppearanceModes(annot) & (1 << mode))) {
    FPDFPage_CloseAnnot(annot);
    return Fail(error, "no appearance");
  }
  const float l = std::min(rect.left, rect.right);
  const float r = std::max(rect.left, rect.right);
  const float b = std::min(rect.bottom, rect.top);
  const float t = std::max(rect.bottom, rect.top);
  const int upright_width = std::max(1, static_cast<int>((r - l) * scale + 0.5));
  const int upright_height =
      std::max(1, static_cast<int>((t - b) * scale + 0.5));
  const int quarter_turns = ((options.rotation / 90) % 4 + 4) % 4;
  const int image_width = quarter_turns % 2 ? upright_height : upright_width;
  const int image_height = quarter_turns % 2 ? upright_width : upright_height;
  if (static_cast<int64_t>(image_width) * image_height > (1 << 28)) {
    FPDFPage_CloseAnnot(annot);
    return Fail(error, "the image would be too large");
  }
  FPDF_BITMAP bitmap = FPDFBitmap_Create(image_width, image_height, 1);
  if (!bitmap) {
    FPDFPage_CloseAnnot(annot);
    return Fail(error, "not enough memory for the image");
  }
  FPDFBitmap_FillRect(bitmap, 0, 0, image_width, image_height,
                      Argb(options.background));
  // Page space to the image: the rectangle upright, y down, then turned
  // clockwise about it.
  const float W = upright_width, H = upright_height, k = scale;
  const float matrices[4][6] = {
      {k, 0, 0, -k, -l * k, t * k},
      {0, k, k, 0, H - t * k, -l * k},
      {-k, 0, 0, k, W + l * k, H - t * k},
      {0, -k, -k, 0, t * k, W + l * k},
  };
  const bool drawn = zebkit::pdf::DrawAnnotation(page.get(), annot, mode, bitmap,
                                                 matrices[quarter_turns]);
  FPDFPage_CloseAnnot(annot);
  if (!drawn) {
    FPDFBitmap_Destroy(bitmap);
    return Fail(error, "no appearance");
  }
  return TakePixels(bitmap, width, height, error);
}

namespace {

// A character as a reader should have it: PDFium marks a hyphen it found
// where a word is broken across lines with U+0002 (and U+FFFE in its text),
// which is text to no one; a hyphen, the character in its place, keeps
// characters and their indexes one for one.
char16_t ReadableChar(char16_t c) {
  return c == 0x2 || c == 0xfffe ? u'-' : c;
}

std::u16string ReadableText(std::u16string text) {
  for (char16_t& c : text)
    c = ReadableChar(c);
  return text;
}

}  // namespace

static char* PageText(zebkit_pdf* pdf, int index, char** error) {
  base::AutoLock lock(PdfiumLock());
  Page page(pdf, index);
  if (!page.text())
    return Fail(error, "no page " + std::to_string(index));
  const int count = FPDFText_CountChars(page.text());
  std::u16string text(count + 1, u'\0');
  const int written = FPDFText_GetText(
      page.text(), 0, count, reinterpret_cast<unsigned short*>(text.data()));
  text.resize(written > 0 ? written - 1 : 0);
  return Give(base::UTF16ToUTF8(ReadableText(std::move(text))));
}

namespace {

// A text object, for zebkit_pdf_text_layout(): its font, and the colour
// its character @index is filled with.
base::DictValue DescribeTextObject(FPDF_TEXTPAGE text_page,
                                   FPDF_PAGEOBJECT object,
                                   int index) {
  base::DictValue item;
  if (FPDF_FONT font = FPDFTextObj_GetFont(object)) {
    std::string name(256, '\0');
    size_t length = FPDFFont_GetBaseFontName(font, name.data(), name.size());
    if (length > name.size()) {
      name.assign(length, '\0');
      length = FPDFFont_GetBaseFontName(font, name.data(), name.size());
    }
    name.resize(length > 0 ? length - 1 : 0);
    item.Set("font", name);
    std::string family(256, '\0');
    length = FPDFFont_GetFamilyName(font, family.data(), family.size());
    if (length > family.size()) {
      family.assign(length, '\0');
      length = FPDFFont_GetFamilyName(font, family.data(), family.size());
    }
    family.resize(length > 0 ? length - 1 : 0);
    item.Set("family", family);
    item.Set("weight", FPDFFont_GetWeight(font));
    // The font descriptor's flags: 1 fixed pitch, 64 italic.
    const int flags = FPDFFont_GetFlags(font);
    item.Set("italic", flags >= 0 && (flags & 64) != 0);
    item.Set("monospaced", flags >= 0 && (flags & 1) != 0);
    item.Set("embedded", FPDFFont_GetIsEmbedded(font) == 1);
  }
  float size = 0;
  if (FPDFTextObj_GetFontSize(object, &size))
    item.Set("size", size);
  unsigned int r = 0, g = 0, b = 0, a = 0;
  if (FPDFText_GetFillColor(text_page, index, &r, &g, &b, &a)) {
    base::ListValue color;
    for (unsigned int channel : {r, g, b, a})
      color.Append(static_cast<int>(channel));
    item.Set("color", std::move(color));
  }
  return item;
}

}  // namespace

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
  base::ListValue objects;
  std::string font;
  // The text objects the characters are drawn by, numbered as they come.
  std::map<FPDF_PAGEOBJECT, int> numbers;
  for (int i = 0; i < count; ++i) {
    base::DictValue item;
    const unsigned int unicode = FPDFText_GetUnicode(text_page, i);
    std::u16string one(1, ReadableChar(static_cast<char16_t>(unicode)));
    item.Set("c", base::UTF16ToUTF8(one));
    if (FPDFText_IsGenerated(text_page, i) != 1) {
      double left = 0, right = 0, bottom = 0, top = 0;
      if (FPDFText_GetCharBox(text_page, i, &left, &right, &bottom, &top))
        item.Set("box", PdfRect(left, bottom, right, top));
      FS_RECTF loose = {};
      if (FPDFText_GetLooseCharBox(text_page, i, &loose))
        item.Set("loose", PdfRect(loose.left, loose.bottom, loose.right, loose.top));
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
    if (FPDF_PAGEOBJECT object = FPDFText_GetTextObject(text_page, i)) {
      auto [it, added] =
          numbers.emplace(object, static_cast<int>(numbers.size()));
      if (added)
        objects.Append(DescribeTextObject(text_page, object, i));
      item.Set("object", it->second);
    }
    chars.Append(std::move(item));
  }
  base::DictValue layout;
  layout.Set("text", base::UTF16ToUTF8(ReadableText(std::move(text))));
  layout.Set("chars", std::move(chars));
  layout.Set("objects", std::move(objects));
  return GiveJson(base::Value(std::move(layout)));
}

static char* PageTextLines(zebkit_pdf* pdf, int index, char** error) {
  base::AutoLock lock(PdfiumLock());
  Page page(pdf, index);
  FPDF_TEXTPAGE text_page = page.text();
  if (!text_page)
    return Fail(error, "no page " + std::to_string(index));
  base::ListValue lines;
  std::u16string text;
  double left = 0, bottom = 0, right = 0, top = 0;
  bool boxed = false;
  int unmapped = 0;
  auto end_line = [&] {
    if (!text.empty() && boxed) {
      base::DictValue line;
      line.Set("text", base::UTF16ToUTF8(text));
      line.Set("box", PdfRect(left, bottom, right, top));
      line.Set("unmapped", unmapped);
      lines.Append(std::move(line));
    }
    text.clear();
    boxed = false;
    unmapped = 0;
  };
  const int count = FPDFText_CountChars(text_page);
  for (int i = 0; i < count; ++i) {
    const char16_t c =
        ReadableChar(static_cast<char16_t>(FPDFText_GetUnicode(text_page, i)));
    if (c == u'\r' || c == u'\n') {
      end_line();
      continue;
    }
    text.push_back(c);
    if (FPDFText_IsGenerated(text_page, i) == 1)
      continue;
    if (FPDFText_HasUnicodeMapError(text_page, i) == 1)
      ++unmapped;
    double l = 0, r = 0, b = 0, t = 0;
    if (!FPDFText_GetCharBox(text_page, i, &l, &r, &b, &t) || (l == r && b == t))
      continue;
    if (!boxed) {
      left = l, bottom = b, right = r, top = t;
      boxed = true;
    } else {
      left = std::min(left, l), bottom = std::min(bottom, b);
      right = std::max(right, r), top = std::max(top, t);
    }
  }
  end_line();
  return GiveJson(base::Value(std::move(lines)));
}

namespace {

// The image objects among @object and in it, when it is a form, in order.
void CollectImages(FPDF_PAGEOBJECT object, std::vector<FPDF_PAGEOBJECT>& images) {
  const int type = FPDFPageObj_GetType(object);
  if (type == FPDF_PAGEOBJ_FORM) {
    const int count = FPDFFormObj_CountObjects(object);
    for (int i = 0; i < count; ++i)
      CollectImages(FPDFFormObj_GetObject(object, i), images);
  } else if (type == FPDF_PAGEOBJ_IMAGE) {
    images.push_back(object);
  }
}

// An image stored as a JPEG file and nothing else (DCTDecode its only
// filter), which can be handed over as it is.
bool IsJpeg(FPDF_PAGEOBJECT image) {
  if (FPDFImageObj_GetImageFilterCount(image) != 1)
    return false;
  char filter[16] = {};
  FPDFImageObj_GetImageFilter(image, 0, filter, sizeof(filter));
  return std::string_view(filter) == "DCTDecode";
}

std::vector<FPDF_PAGEOBJECT> PageImages(FPDF_PAGE page) {
  std::vector<FPDF_PAGEOBJECT> images;
  const int count = FPDFPage_CountObjects(page);
  for (int i = 0; i < count; ++i)
    CollectImages(FPDFPage_GetObject(page, i), images);
  return images;
}

}  // namespace

char* zebkit_pdf_images(zebkit_pdf* pdf, int index, char** error) {
  base::AutoLock lock(PdfiumLock());
  Page page(pdf, index);
  if (!page.get())
    return Fail(error, "no page " + std::to_string(index));
  base::ListValue images;
  for (FPDF_PAGEOBJECT object : PageImages(page.get())) {
    base::DictValue image;
    unsigned int width = 0, height = 0;
    FPDFImageObj_GetImagePixelSize(object, &width, &height);
    image.Set("width", static_cast<int>(width));
    image.Set("height", static_cast<int>(height));
    float left = 0, bottom = 0, right = 0, top = 0;
    if (FPDFPageObj_GetBounds(object, &left, &bottom, &right, &top))
      image.Set("rect", PdfRect(left, bottom, right, top));
    image.Set("jpeg", IsJpeg(object));
    images.Append(std::move(image));
  }
  return GiveJson(base::Value(std::move(images)));
}

uint8_t* zebkit_pdf_image_data(zebkit_pdf* pdf,
                               int index,
                               int image,
                               size_t* size,
                               bool* jpeg,
                               char** error) {
  base::AutoLock lock(PdfiumLock());
  Page page(pdf, index);
  if (!page.get()) {
    Fail(error, "no page " + std::to_string(index));
    return nullptr;
  }
  const std::vector<FPDF_PAGEOBJECT> images = PageImages(page.get());
  if (image < 0 || image >= static_cast<int>(images.size())) {
    Fail(error, "no image " + std::to_string(image));
    return nullptr;
  }
  FPDF_PAGEOBJECT object = images[image];
  if (IsJpeg(object)) {
    // The JPEG file as the document has it.
    const unsigned long length =
        FPDFImageObj_GetImageDataRaw(object, nullptr, 0);
    auto* bytes = static_cast<uint8_t*>(malloc(std::max(length, 1ul)));
    FPDFImageObj_GetImageDataRaw(object, bytes, length);
    *size = length;
    if (jpeg)
      *jpeg = true;
    return bytes;
  }
  // Its pixels, decoded, as PNG.
  FPDF_BITMAP bitmap = FPDFImageObj_GetBitmap(object);
  if (!bitmap) {
    Fail(error, "the image cannot be decoded");
    return nullptr;
  }
  const int width = FPDFBitmap_GetWidth(bitmap);
  const int height = FPDFBitmap_GetHeight(bitmap);
  const int stride = FPDFBitmap_GetStride(bitmap);
  const int format = FPDFBitmap_GetFormat(bitmap);
  const auto* in = static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(bitmap));
  std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
  for (int y = 0; y < height; ++y) {
    const uint8_t* row = in + static_cast<size_t>(y) * stride;
    uint8_t* out = rgba.data() + static_cast<size_t>(y) * width * 4;
    for (int x = 0; x < width; ++x, out += 4) {
      switch (format) {
        case FPDFBitmap_Gray:
          out[0] = out[1] = out[2] = row[x];
          out[3] = 255;
          break;
        case FPDFBitmap_BGR:
          out[0] = row[x * 3 + 2], out[1] = row[x * 3 + 1], out[2] = row[x * 3];
          out[3] = 255;
          break;
        default:  // BGRx, BGRA
          out[0] = row[x * 4 + 2], out[1] = row[x * 4 + 1], out[2] = row[x * 4];
          out[3] = format == FPDFBitmap_BGRA ? row[x * 4 + 3] : 255;
          break;
      }
    }
  }
  FPDFBitmap_Destroy(bitmap);
  if (jpeg)
    *jpeg = false;
  return zebkit_image_encode(ZEBKIT_IMAGE_PNG, rgba.data(), width, height, 0,
                             size, error);
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

static char* SearchPages(zebkit_pdf* pdf,
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
      match.Set("context", base::UTF16ToUTF8(ReadableText(std::move(context))));
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

namespace {

std::string AsciiString(FPDF_SIGNATURE signature,
                        unsigned long (*get)(FPDF_SIGNATURE, char*, unsigned long)) {
  const unsigned long size = get(signature, nullptr, 0);
  if (size <= 1)
    return std::string();
  std::string text(size, '\0');
  get(signature, text.data(), size);
  text.resize(size - 1);
  return text;
}

std::string CertificateName(const X509_NAME* name) {
  char text[256] = {};
  if (X509_NAME_get_text_by_NID(name, NID_commonName, text, sizeof(text)) > 0)
    return text;
  char* line = X509_NAME_oneline(name, nullptr, 0);
  std::string out = line ? line : "";
  OPENSSL_free(line);
  return out;
}

std::string CertificateTime(const ASN1_TIME* time) {
  int64_t seconds = 0;
  if (!time || !ASN1_TIME_to_posix(time, &seconds))
    return std::string();
  const time_t when = static_cast<time_t>(seconds);
  struct tm utc = {};
  char text[32] = {};
#if defined(_WIN32)
  const bool converted = gmtime_s(&utc, &when) == 0;
#else
  const bool converted = gmtime_r(&when, &utc) != nullptr;
#endif
  if (!converted ||
      !strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc)) {
    return std::string();
  }
  return text;
}

// The signer's certificate among those the PKCS#7 carries: the one that
// issued none of the others.
base::Value Signer(const std::vector<uint8_t>& contents) {
  bssl::UniquePtr<STACK_OF(X509)> certificates(sk_X509_new_null());
  CBS cbs;
  CBS_init(&cbs, contents.data(), contents.size());
  if (!certificates || !PKCS7_get_certificates(certificates.get(), &cbs) ||
      sk_X509_num(certificates.get()) == 0) {
    return base::Value();
  }
  X509* signer = nullptr;
  for (X509* candidate : certificates.get()) {
    bool issuer = false;
    for (X509* other : certificates.get()) {
      if (other != candidate &&
          X509_NAME_cmp(X509_get_issuer_name(other),
                        X509_get_subject_name(candidate)) == 0) {
        issuer = true;
      }
    }
    if (!issuer) {
      signer = candidate;
      break;
    }
  }
  if (!signer)
    signer = sk_X509_value(certificates.get(), 0);
  base::DictValue out;
  out.Set("name", CertificateName(X509_get_subject_name(signer)));
  out.Set("issuer", CertificateName(X509_get_issuer_name(signer)));
  if (bssl::UniquePtr<BIGNUM> serial{
          ASN1_INTEGER_to_BN(X509_get0_serialNumber(signer), nullptr)}) {
    char* hex = BN_bn2hex(serial.get());
    out.Set("serial", hex ? base::ToLowerASCII(hex) : std::string());
    OPENSSL_free(hex);
  }
  out.Set("notBefore", CertificateTime(X509_get0_notBefore(signer)));
  out.Set("notAfter", CertificateTime(X509_get0_notAfter(signer)));
  return base::Value(std::move(out));
}

// The PKCS#7 a signature holds: its DER element alone, without the zeros
// that pad /Contents to the room the signer left for it.
std::vector<uint8_t> SignatureContents(FPDF_SIGNATURE signature) {
  std::vector<uint8_t> contents(
      FPDFSignatureObj_GetContents(signature, nullptr, 0));
  if (!contents.empty()) {
    FPDFSignatureObj_GetContents(signature, contents.data(),
                                 contents.size());
  }
  CBS cbs, element;
  CBS_init(&cbs, contents.data(), contents.size());
  if (CBS_get_any_asn1_element(&cbs, &element, nullptr, nullptr))
    contents.resize(CBS_len(&element));
  return contents;
}

}  // namespace

namespace {

// The document's bytes from @offset, @length of them, as it was opened.
bool ReadOpened(const zebkit_pdf* pdf,
                int64_t offset,
                int64_t length,
                std::vector<uint8_t>& out) {
  if (offset < 0 || length < 0)
    return false;
  if (!pdf->bytes.empty()) {
    if (offset + length > static_cast<int64_t>(pdf->bytes.size()))
      return false;
    out.insert(out.end(), pdf->bytes.begin() + offset,
               pdf->bytes.begin() + offset + length);
    return true;
  }
  base::File file(base::FilePath::FromUTF8Unsafe(pdf->path),
                  base::File::FLAG_OPEN | base::File::FLAG_READ);
  if (!file.IsValid())
    return false;
  const size_t at = out.size();
  out.resize(at + static_cast<size_t>(length));
  return file.Read(offset, base::span(out).subspan(at)) ==
         std::optional<size_t>(static_cast<size_t>(length));
}

int64_t OpenedSize(const zebkit_pdf* pdf) {
  if (!pdf->bytes.empty())
    return static_cast<int64_t>(pdf->bytes.size());
  base::File file(base::FilePath::FromUTF8Unsafe(pdf->path),
                  base::File::FLAG_OPEN | base::File::FLAG_READ);
  return file.IsValid() ? file.GetLength() : -1;
}

// What is learned of a CMS SignedData's first signer: the digest it
// names, its signed attributes (as DER, a SET), the messageDigest among
// them, the signature algorithm and value, and who it is.
struct SignerInfo {
  const EVP_MD* digest = nullptr;
  std::vector<uint8_t> signed_attributes;
  std::vector<uint8_t> message_digest;
  int signature_nid = NID_undef;
  std::vector<uint8_t> signature;
  // issuerAndSerialNumber, or a subjectKeyIdentifier.
  std::vector<uint8_t> issuer;
  std::vector<uint8_t> serial;
  std::vector<uint8_t> key_id;
};

std::vector<uint8_t> Bytes(const CBS& cbs) {
  return std::vector<uint8_t>(CBS_data(&cbs), CBS_data(&cbs) + CBS_len(&cbs));
}

int Nid(CBS oid) {
  return OBJ_cbs2nid(&oid);
}

// ContentInfo { signedData, SignedData { version, digestAlgorithms,
// encapContentInfo, [0] certificates, [1] crls, signerInfos } }, RFC 5652.
std::optional<SignerInfo> ParseSigner(const std::vector<uint8_t>& contents) {
  CBS in, content_info, type, wrapped, signed_data, skip, signers, signer;
  CBS_init(&in, contents.data(), contents.size());
  if (!CBS_get_asn1(&in, &content_info, CBS_ASN1_SEQUENCE) ||
      !CBS_get_asn1(&content_info, &type, CBS_ASN1_OBJECT) ||
      Nid(type) != NID_pkcs7_signed ||
      !CBS_get_asn1(&content_info, &wrapped,
                    CBS_ASN1_CONTEXT_SPECIFIC | CBS_ASN1_CONSTRUCTED | 0) ||
      !CBS_get_asn1(&wrapped, &signed_data, CBS_ASN1_SEQUENCE) ||
      !CBS_get_asn1(&signed_data, &skip, CBS_ASN1_INTEGER) ||
      !CBS_get_asn1(&signed_data, &skip, CBS_ASN1_SET) ||
      !CBS_get_asn1(&signed_data, &skip, CBS_ASN1_SEQUENCE)) {
    return std::nullopt;
  }
  // Certificates and CRLs, which come before the signers when there.
  while (CBS_peek_asn1_tag(&signed_data, CBS_ASN1_CONTEXT_SPECIFIC |
                                             CBS_ASN1_CONSTRUCTED | 0) ||
         CBS_peek_asn1_tag(&signed_data, CBS_ASN1_CONTEXT_SPECIFIC |
                                             CBS_ASN1_CONSTRUCTED | 1)) {
    if (!CBS_get_any_asn1(&signed_data, &skip, nullptr))
      return std::nullopt;
  }
  CBS digest_algorithm, digest_oid, signature_algorithm, signature_oid,
      signature, sid;
  CBS_ASN1_TAG sid_tag = 0;
  if (!CBS_get_asn1(&signed_data, &signers, CBS_ASN1_SET) ||
      !CBS_get_asn1(&signers, &signer, CBS_ASN1_SEQUENCE) ||
      !CBS_get_asn1(&signer, &skip, CBS_ASN1_INTEGER) ||
      !CBS_get_any_asn1(&signer, &sid, &sid_tag) ||
      !CBS_get_asn1(&signer, &digest_algorithm, CBS_ASN1_SEQUENCE) ||
      !CBS_get_asn1(&digest_algorithm, &digest_oid, CBS_ASN1_OBJECT)) {
    return std::nullopt;
  }
  SignerInfo info;
  info.digest = EVP_get_digestbynid(Nid(digest_oid));
  // The signer by issuer and serial, or [0] its key's identifier.
  CBS issuer, serial;
  if (sid_tag == CBS_ASN1_CONTEXT_SPECIFIC) {
    info.key_id = Bytes(sid);
  } else if (sid_tag == CBS_ASN1_SEQUENCE &&
             CBS_get_asn1(&sid, &issuer, CBS_ASN1_SEQUENCE) &&
             CBS_get_asn1(&sid, &serial, CBS_ASN1_INTEGER)) {
    info.issuer = Bytes(issuer);
    info.serial = Bytes(serial);
  }
  // [0] IMPLICIT signedAttrs: signed as a SET (RFC 5652, 5.4).
  if (CBS_peek_asn1_tag(&signer,
                        CBS_ASN1_CONTEXT_SPECIFIC | CBS_ASN1_CONSTRUCTED | 0)) {
    CBS attributes_element, attributes;
    size_t header = 0;
    if (!CBS_get_any_asn1_element(&signer, &attributes_element, nullptr,
                                  &header)) {
      return std::nullopt;
    }
    // Retagged as the SET OF it is signed as: 0x31 (CBS's tag constants
    // are not the DER byte).
    info.signed_attributes = Bytes(attributes_element);
    info.signed_attributes[0] = 0x31;
    attributes = attributes_element;
    CBS_skip(&attributes, header);
    while (CBS_len(&attributes)) {
      CBS attribute, oid, values, value;
      if (!CBS_get_asn1(&attributes, &attribute, CBS_ASN1_SEQUENCE) ||
          !CBS_get_asn1(&attribute, &oid, CBS_ASN1_OBJECT) ||
          !CBS_get_asn1(&attribute, &values, CBS_ASN1_SET)) {
        return std::nullopt;
      }
      if (Nid(oid) == NID_pkcs9_messageDigest &&
          CBS_get_asn1(&values, &value, CBS_ASN1_OCTETSTRING)) {
        info.message_digest = Bytes(value);
      }
    }
  }
  if (!CBS_get_asn1(&signer, &signature_algorithm, CBS_ASN1_SEQUENCE) ||
      !CBS_get_asn1(&signature_algorithm, &signature_oid, CBS_ASN1_OBJECT) ||
      !CBS_get_asn1(&signer, &signature, CBS_ASN1_OCTETSTRING)) {
    return std::nullopt;
  }
  info.signature_nid = Nid(signature_oid);
  info.signature = Bytes(signature);
  return info;
}

// The certificate the signer names.
X509* FindSigner(STACK_OF(X509) * certificates, const SignerInfo& info) {
  for (X509* certificate : certificates) {
    if (!info.serial.empty()) {
      const ASN1_INTEGER* serial = X509_get0_serialNumber(certificate);
      uint8_t* der = nullptr;
      const int length = i2d_ASN1_INTEGER(serial, &der);
      CBS whole, value;
      CBS_init(&whole, der, length > 0 ? length : 0);
      const bool same =
          CBS_get_asn1(&whole, &value, CBS_ASN1_INTEGER) &&
          Bytes(value) == info.serial;
      OPENSSL_free(der);
      if (same)
        return certificate;
    } else if (!info.key_id.empty()) {
      const ASN1_OCTET_STRING* key_id = X509_get0_subject_key_id(certificate);
      if (key_id && static_cast<size_t>(ASN1_STRING_length(key_id)) ==
                        info.key_id.size() &&
          std::equal(info.key_id.begin(), info.key_id.end(),
                     ASN1_STRING_get0_data(key_id))) {
        return certificate;
      }
    }
  }
  return nullptr;
}

// Whether the signature is of the bytes it covers, by the key of the
// certificate it carries: "intact", or the reason not, in @out.
void CheckSignature(const zebkit_pdf* pdf,
                    const std::vector<int>& range,
                    const std::vector<uint8_t>& contents,
                    base::DictValue& out) {
  auto problem = [&](const char* why) {
    out.Set("intact", false);
    out.Set("problem", why);
  };
  const int64_t size = OpenedSize(pdf);
  out.Set("coversDocument",
          range.size() == 4 && range[0] == 0 && size >= 0 &&
              static_cast<int64_t>(range[2]) + range[3] == size);
  std::optional<SignerInfo> info = ParseSigner(contents);
  if (!info) {
    problem("the signature is not a CMS signature zebkit reads");
    return;
  }
  if (!info->digest) {
    problem("the signature's digest algorithm is not known");
    return;
  }
  out.Set("digest", OBJ_nid2sn(EVP_MD_type(info->digest)));
  if (range.size() % 2 || range.empty()) {
    problem("the byte range is not pairs of offsets and lengths");
    return;
  }
  std::vector<uint8_t> covered;
  for (size_t i = 0; i < range.size(); i += 2) {
    if (!ReadOpened(pdf, range[i], range[i + 1], covered)) {
      problem("the byte range reaches outside the file");
      return;
    }
  }
  uint8_t digest[EVP_MAX_MD_SIZE];
  unsigned int digest_size = 0;
  EVP_Digest(covered.data(), covered.size(), digest, &digest_size,
             info->digest, nullptr);
  if (info->signed_attributes.empty() || info->message_digest.empty()) {
    problem("the signature has no signed message digest");
    return;
  }
  if (info->message_digest !=
      std::vector<uint8_t>(digest, digest + digest_size)) {
    problem("the document was changed after it was signed");
    return;
  }
  bssl::UniquePtr<STACK_OF(X509)> certificates(sk_X509_new_null());
  CBS cbs;
  CBS_init(&cbs, contents.data(), contents.size());
  X509* signer = certificates &&
                         PKCS7_get_certificates(certificates.get(), &cbs)
                     ? FindSigner(certificates.get(), *info)
                     : nullptr;
  EVP_PKEY* key = signer ? X509_get0_pubkey(signer) : nullptr;
  if (!key) {
    problem("the signer's certificate is not in the signature");
    return;
  }
  bssl::ScopedEVP_MD_CTX context;
  EVP_PKEY_CTX* key_context = nullptr;
  bool ok = EVP_DigestVerifyInit(context.get(), &key_context, info->digest,
                                 nullptr, key);
  if (ok && info->signature_nid == NID_rsassaPss) {
    ok = EVP_PKEY_CTX_set_rsa_padding(key_context, RSA_PKCS1_PSS_PADDING) &&
         EVP_PKEY_CTX_set_rsa_pss_saltlen(key_context, -1);
  }
  ok = ok && EVP_DigestVerify(context.get(), info->signature.data(),
                              info->signature.size(),
                              info->signed_attributes.data(),
                              info->signed_attributes.size());
  if (!ok) {
    problem("the signature does not match the signer's key");
    return;
  }
  out.Set("intact", true);
}

}  // namespace

static char* ReadSignatures(zebkit_pdf* pdf, char** error) {
  base::AutoLock lock(PdfiumLock());
  if (!pdf || !pdf->document)
    return Fail(error, "no document");
  base::ListValue signatures;
  const int count = FPDF_GetSignatureCount(pdf->document);
  for (int i = 0; i < count; ++i) {
    FPDF_SIGNATURE signature = FPDF_GetSignatureObject(pdf->document, i);
    if (!signature)
      continue;
    base::DictValue item;
    item.Set("field", zebkit::pdf::SignatureFieldName(signature));
    item.Set("subFilter",
             AsciiString(signature, &FPDFSignatureObj_GetSubFilter));
    item.Set("reason", Utf16String([&](void* buffer, unsigned long size) {
               return FPDFSignatureObj_GetReason(signature, buffer, size);
             }));
    item.Set("time", AsciiString(signature, &FPDFSignatureObj_GetTime));
    std::vector<int> range(FPDFSignatureObj_GetByteRange(signature, nullptr, 0));
    if (!range.empty())
      FPDFSignatureObj_GetByteRange(signature, range.data(), range.size());
    base::ListValue byte_range;
    for (int offset : range)
      byte_range.Append(offset);
    item.Set("byteRange", std::move(byte_range));
    item.Set("docMDP",
             static_cast<int>(FPDFSignatureObj_GetDocMDPPermission(signature)));
    const std::vector<uint8_t> contents = SignatureContents(signature);
    item.Set("size", static_cast<int>(contents.size()));
    if (base::Value signer = Signer(contents); !signer.is_none())
      item.Set("signer", std::move(signer));
    CheckSignature(pdf, range, contents, item);
    signatures.Append(std::move(item));
  }
  return GiveJson(base::Value(std::move(signatures)));
}

uint8_t* zebkit_pdf_signature_contents(zebkit_pdf* pdf,
                                       int index,
                                       size_t* size,
                                       char** error) {
  base::AutoLock lock(PdfiumLock());
  FPDF_SIGNATURE signature =
      pdf && pdf->document ? FPDF_GetSignatureObject(pdf->document, index)
                           : nullptr;
  if (!signature) {
    Fail(error, "no signature " + std::to_string(index));
    return nullptr;
  }
  const std::vector<uint8_t> contents = SignatureContents(signature);
  auto* bytes = static_cast<uint8_t*>(malloc(std::max<size_t>(contents.size(), 1)));
  std::copy(contents.begin(), contents.end(), bytes);
  *size = contents.size();
  return bytes;
}

char* zebkit_pdf_outline(zebkit_pdf* pdf, char** error) {
  if (!pdf)
    return Fail(error, "no document");
  base::AutoLock lock(PdfiumLock());
  return GiveJson(base::Value(Outline(pdf->document, nullptr, 0)));
}

bool zebkit_pdf_set_outline(zebkit_pdf* pdf, const char* json, char** error) {
  if (!pdf) return FailFalse(error, "no document");
  if (!json || strlen(json) > 16 * 1024 * 1024)
    return FailFalse(error, "the outline must be a JSON array of at most 16 MiB");
  auto value = base::JSONReader::Read(json, 0, 256);
  if (!value || !value->is_list()) return FailFalse(error, "the outline is not a JSON array");
  base::AutoLock lock(PdfiumLock());
  size_t count = 0;
  std::vector<zebkit::pdf::OutlineItem> items;
  std::string message;
  if (!ParseOutline(value->GetList(), FPDF_GetPageCount(pdf->document), 0, &count, &items, &message))
    return FailFalse(error, message);
  if (!zebkit::pdf::SetOutline(pdf->document, items))
    return FailFalse(error, "the outline target pages cannot be read");
  return true;
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

// The same list, as /Subtype names them.
constexpr const char* kSubtypeNames[] = {
    "",          "Text",     "Link",        "FreeText",  "Line",
    "Square",    "Circle",   "Polygon",     "PolyLine",  "Highlight",
    "Underline", "Squiggly", "StrikeOut",   "Stamp",     "Caret",
    "Ink",       "Popup",    "FileAttachment", "Sound",  "Movie",
    "Widget",    "Screen",   "PrinterMark", "TrapNet",   "Watermark",
    "3D",        "RichMedia", "XFAWidget",  "Redact",
};

constexpr const char* kAppearanceModes[] = {"normal", "rollover", "down"};

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

// A PDF object's plain value as JSON (zebkit_pdf_annotations(), "entries"):
// a name is "/Name", a string that starts with "/" has another before it,
// a reference is {"ref": object number}.
base::Value EntryToJson(const zebkit::pdf::Value& value) {
  using Kind = zebkit::pdf::Value::Kind;
  switch (value.kind) {
    case Kind::kNull:
      return base::Value();
    case Kind::kBool:
      return base::Value(value.boolean);
    case Kind::kNumber:
      if (value.number == static_cast<int>(value.number))
        return base::Value(static_cast<int>(value.number));
      return base::Value(value.number);
    case Kind::kString:
      return base::Value(value.text.starts_with("/") ? "/" + value.text
                                                     : value.text);
    case Kind::kName:
      return base::Value("/" + value.text);
    case Kind::kRef:
      return base::Value(
          base::DictValue().Set("ref", static_cast<int>(value.number)));
    case Kind::kArray: {
      base::ListValue list;
      for (const auto& item : value.items)
        list.Append(EntryToJson(item));
      return base::Value(std::move(list));
    }
    case Kind::kDict: {
      base::DictValue dict;
      for (const auto& [key, item] : value.entries)
        dict.Set(key, EntryToJson(item));
      return base::Value(std::move(dict));
    }
  }
  return base::Value();
}

std::optional<zebkit::pdf::Value> JsonToEntry(const base::Value& json,
                                              int depth = 0) {
  using Kind = zebkit::pdf::Value::Kind;
  zebkit::pdf::Value value;
  if (depth > 8)
    return std::nullopt;
  if (json.is_none()) {
    value.kind = Kind::kNull;
  } else if (json.is_bool()) {
    value.kind = Kind::kBool;
    value.boolean = json.GetBool();
  } else if (json.is_int() || json.is_double()) {
    value.kind = Kind::kNumber;
    value.number = json.GetDouble();
  } else if (json.is_string()) {
    const std::string& text = json.GetString();
    if (text.starts_with("//")) {
      value.kind = Kind::kString;
      value.text = text.substr(1);
    } else if (text.starts_with("/")) {
      value.kind = Kind::kName;
      value.text = text.substr(1);
    } else {
      value.kind = Kind::kString;
      value.text = text;
    }
  } else if (json.is_list()) {
    value.kind = Kind::kArray;
    for (const base::Value& item : json.GetList()) {
      std::optional<zebkit::pdf::Value> entry = JsonToEntry(item, depth + 1);
      if (!entry)
        return std::nullopt;
      value.items.push_back(std::move(*entry));
    }
  } else if (json.is_dict()) {
    const base::DictValue& dict = json.GetDict();
    if (dict.size() == 1 && dict.FindInt("ref")) {
      value.kind = Kind::kRef;
      value.number = *dict.FindInt("ref");
      return value;
    }
    value.kind = Kind::kDict;
    for (const auto [key, item] : dict) {
      std::optional<zebkit::pdf::Value> entry = JsonToEntry(item, depth + 1);
      if (!entry)
        return std::nullopt;
      value.entries.emplace_back(key, std::move(*entry));
    }
  } else {
    return std::nullopt;
  }
  return value;
}

// What "entries" holds, under the names zebkit_pdf_annotation_add() takes.
void DescribeEntries(zebkit_pdf* pdf, const base::DictValue& entries, base::DictValue& item);

base::DictValue DescribeAnnotation(zebkit_pdf* pdf,
                                   FPDF_PAGE page,
                                   FPDF_ANNOTATION annot,
                                   int index,
                                   size_t* budget = nullptr) {
  base::DictValue item;
  item.Set("index", index);
  if (const int object = zebkit::pdf::AnnotationObjectNumber(annot))
    item.Set("object", object);
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
  base::ListValue appearances;
  const int modes = zebkit::pdf::AppearanceModes(annot);
  for (int mode = 0; mode < 3; ++mode) {
    if (modes & (1 << mode))
      appearances.Append(kAppearanceModes[mode]);
  }
  item.Set("appearances", std::move(appearances));
  base::Value entries = EntryToJson(zebkit::pdf::AnnotationEntries(annot, budget));
  if (entries.is_dict()) {
    DescribeEntries(pdf, entries.GetDict(), item);
    item.Set("entries", std::move(entries.GetDict()));
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
    if ((!item.is_int() && !item.is_double()) || !std::isfinite(item.GetDouble()) ||
        std::abs(item.GetDouble()) > 1e7) {
      return std::nullopt;
    }
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

// The keys of zebkit_pdf_annotation_add() beyond the ones every annotation has,
// each to its entry and back.
constexpr std::pair<const char*, const char*> kEndings[] = {
    {"none", "None"},           {"openArrow", "OpenArrow"},
    {"closedArrow", "ClosedArrow"}, {"rOpenArrow", "ROpenArrow"},
    {"rClosedArrow", "RClosedArrow"}, {"circle", "Circle"},
    {"square", "Square"},       {"diamond", "Diamond"},
    {"butt", "Butt"},           {"slash", "Slash"},
};
constexpr std::pair<const char*, const char*> kBorderStyles[] = {
    {"solid", "S"}, {"dashed", "D"}, {"beveled", "B"}, {"inset", "I"}, {"underline", "U"},
};
constexpr const char* kBlends[] = {
    "Normal", "Multiply", "Screen", "Overlay", "Darken", "Lighten", "ColorDodge", "ColorBurn",
    "HardLight", "SoftLight", "Difference", "Exclusion", "Hue", "Saturation", "Color", "Luminosity",
};
constexpr const char* kStampNames[] = {
    "Approved", "Experimental", "NotApproved", "AsIs", "Expired", "NotForPublicRelease",
    "Confidential", "Final", "Sold", "Departmental", "ForComment", "TopSecret", "Draft",
    "ForPublicRelease",
};
constexpr const char* kNoteIcons[] = {"Comment", "Key", "Note", "Help", "NewParagraph", "Paragraph", "Insert"};
constexpr const char* kAttachmentIcons[] = {"Graph", "PushPin", "Paperclip", "Tag", "GraphPushPin", "PaperclipTag"};
constexpr std::pair<const char*, const char*> kStateNames[] = {
    {"marked", "Marked"}, {"unmarked", "Unmarked"}, {"accepted", "Accepted"}, {"rejected", "Rejected"},
    {"cancelled", "Cancelled"}, {"completed", "Completed"}, {"none", "None"},
};
constexpr auto kUndrawnAppearanceKeys = base::MakeFixedFlatSet<std::string_view>(
    {"inReplyTo", "replyType", "state", "stateModel", "popup"});

extern "C++" {
template <size_t N>
const char* FindPair(const std::pair<const char*, const char*> (&table)[N], const std::string& key) {
  for (const auto& entry : table) {
    if (key == entry.first)
      return entry.second;
  }
  return nullptr;
}

template <size_t N>
const char* FindPairBack(const std::pair<const char*, const char*> (&table)[N], const std::string& value) {
  for (const auto& entry : table) {
    if (value == entry.second)
      return entry.first;
  }
  return nullptr;
}

template <size_t N>
bool InList(const char* const (&list)[N], const std::string& name) {
  for (const char* item : list) {
    if (name == item)
      return true;
  }
  return false;
}
}  // extern "C++"

// /DA, which free text keeps its font, size and colours in.
struct DefaultAppearance {
  std::string font = "Helvetica";
  double size = 12;
  std::optional<std::array<double, 3>> fill, stroke;
};

DefaultAppearance ParseDa(const std::string& text) {
  DefaultAppearance da;
  std::vector<std::string> tokens = base::SplitString(
      text, " \t\r\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  auto number = [&](size_t at) {
    double value = 0;
    base::StringToDouble(tokens[at], &value);
    return value;
  };
  for (size_t i = 0; i < tokens.size() && i < 256; ++i) {
    if (tokens[i] == "Tf" && i >= 2 && tokens[i - 2].starts_with("/")) {
      da.font = tokens[i - 2].substr(1);
      da.size = number(i - 1);
    } else if (tokens[i] == "rg" && i >= 3) {
      da.fill = {number(i - 3), number(i - 2), number(i - 1)};
    } else if (tokens[i] == "RG" && i >= 3) {
      da.stroke = {number(i - 3), number(i - 2), number(i - 1)};
    } else if (tokens[i] == "g" && i >= 1) {
      da.fill = {number(i - 1), number(i - 1), number(i - 1)};
    } else if (tokens[i] == "G" && i >= 1) {
      da.stroke = {number(i - 1), number(i - 1), number(i - 1)};
    }
  }
  return da;
}

// A number in text, whatever the locale.
std::string FixedText(double value, int precision) {
  // to_chars() writes nothing that can be used when it does not fit.
  if (!std::isfinite(value))
    value = 0;
  value = std::clamp(value, -1e18, 1e18);
  char buffer[64];
  const char* end = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::fixed, precision).ptr;
  return std::string(buffer, static_cast<size_t>(end - buffer));
}

std::string WriteDa(const DefaultAppearance& da) {
  auto color = [](const std::array<double, 3>& c, const char* op) {
    return FixedText(c[0], 3) + " " + FixedText(c[1], 3) + " " + FixedText(c[2], 3) + " " + op;
  };
  std::string out = "/" + da.font + " " + FixedText(da.size, 2) + " Tf " +
                    color(da.fill.value_or(std::array<double, 3>{0, 0, 0}), "rg");
  if (da.stroke)
    out += " " + color(*da.stroke, "RG");
  return out;
}

std::string FontNameInDa(const std::string& family) {
  std::string out;
  for (char c : family) {
    if (base::IsAsciiAlphaNumeric(c) || c == '-' || c == '_' || c == '+') {
      out.push_back(c);
    } else {
      static const char kHex[] = "0123456789ABCDEF";
      out.push_back('#');
      out.push_back(kHex[static_cast<uint8_t>(c) >> 4]);
      out.push_back(kHex[static_cast<uint8_t>(c) & 15]);
    }
  }
  return out;
}

std::string FontNameFromDa(const std::string& name) {
  std::string out;
  for (size_t i = 0; i < name.size(); ++i) {
    if (name[i] == '#' && i + 2 < name.size() && base::IsHexDigit(name[i + 1]) &&
        base::IsHexDigit(name[i + 2])) {
      out.push_back(static_cast<char>(base::HexDigitToInt(name[i + 1]) * 16 +
                                      base::HexDigitToInt(name[i + 2])));
      i += 2;
    } else {
      out.push_back(name[i]);
    }
  }
  return out;
}

std::optional<std::array<double, 3>> ColorKey(const base::Value& value) {
  std::optional<std::vector<float>> c = Numbers(&value, 3);
  if (!c)
    return std::nullopt;
  std::array<double, 3> out;
  for (int i = 0; i < 3; ++i)
    out[i] = std::clamp((*c)[i], 0.0f, 255.0f) / 255;
  return out;
}

// A measurement's value, from the plain entries: what it says in its unit.
struct Measured {
  double value = 0;
  std::string unit, text;
  int precision = 2;
};

std::optional<Measured> MeasureValue(const base::DictValue& entries) {
  const base::DictValue* measure = entries.FindDict("Measure");
  const std::string* subtype = entries.FindString("Subtype");
  if (!measure || !subtype)
    return std::nullopt;
  auto points = [&](const char* key) {
    std::vector<std::pair<double, double>> out;
    if (const base::ListValue* list = entries.FindList(key)) {
      for (size_t i = 0; i + 1 < list->size(); i += 2) {
        if (((*list)[i].is_int() || (*list)[i].is_double()) &&
            ((*list)[i + 1].is_int() || (*list)[i + 1].is_double())) {
          out.emplace_back((*list)[i].GetDouble(), (*list)[i + 1].GetDouble());
        }
      }
    }
    return out;
  };
  auto length = [](const std::vector<std::pair<double, double>>& p, bool closed) {
    double total = 0;
    for (size_t i = 0; i + 1 < p.size(); ++i)
      total += std::hypot(p[i + 1].first - p[i].first, p[i + 1].second - p[i].second);
    if (closed && p.size() > 2)
      total += std::hypot(p.front().first - p.back().first, p.front().second - p.back().second);
    return total;
  };
  double raw = 0;
  bool area = false;
  const std::string* intent = entries.FindString("IT");
  if (*subtype == "/Line") {
    raw = length(points("L"), false);
  } else if (*subtype == "/PolyLine") {
    raw = length(points("Vertices"), false);
  } else if (*subtype == "/Polygon") {
    const auto p = points("Vertices");
    if (intent && *intent == "/PolyLineDimension") {
      raw = length(p, true);
    } else {
      area = true;
      double twice = 0;
      for (size_t i = 0; i < p.size(); ++i) {
        const auto& a = p[i];
        const auto& b = p[(i + 1) % p.size()];
        twice += a.first * b.second - b.first * a.second;
      }
      raw = std::abs(twice) / 2;
    }
  } else if (*subtype == "/Square" || *subtype == "/Circle") {
    const auto p = points(entries.contains("ZebkitBox") ? "ZebkitBox" : "Rect");
    if (p.size() < 2)
      return std::nullopt;
    area = true;
    raw = std::abs((p[1].first - p[0].first) * (p[1].second - p[0].second));
    if (*subtype == "/Circle")
      raw *= std::numbers::pi / 4;
  } else {
    return std::nullopt;
  }
  const base::ListValue* formats = measure->FindList(area ? "A" : "X");
  if (!formats || formats->empty() || !(*formats)[0].is_dict())
    return std::nullopt;
  const base::DictValue& format = (*formats)[0].GetDict();
  Measured out;
  if (const std::string* unit = format.FindString("U"))
    out.unit = *unit;
  out.value = raw * format.FindDouble("C").value_or(1);
  const double denominator = format.FindDouble("D").value_or(100);
  out.precision = std::clamp(static_cast<int>(std::lround(std::log10(std::max(denominator, 1.0)))), 0, 8);
  out.text = FixedText(out.value, out.precision);
  if (!out.unit.empty())
    out.text += " " + out.unit;
  return out;
}

std::string UpdateMeasurement(zebkit_pdf* pdf,
                              FPDF_ANNOTATION annot,
                              const base::DictValue& json,
                              bool* drawn) {
  static constexpr const char* kGeometry[] = {"measure", "line", "vertices", "rect"};
  bool concerned = false;
  for (const char* key : kGeometry)
    concerned = concerned || json.contains(key);
  if (!concerned)
    return std::string();
  base::Value entries = EntryToJson(zebkit::pdf::AnnotationEntries(annot));
  if (!entries.is_dict())
    return std::string();
  std::optional<Measured> measured = MeasureValue(entries.GetDict());
  if (!measured)
    return json.contains("measure") && !json.Find("measure")->is_none()
               ? "this type of annotation is not measured"
               : std::string();
  const int subtype = FPDFAnnot_GetSubtype(annot);
  if (!entries.GetDict().contains("IT")) {
    const char* intent = subtype == FPDF_ANNOT_LINE       ? "LineDimension"
                         : subtype == FPDF_ANNOT_POLYLINE ? "PolyLineDimension"
                         : subtype == FPDF_ANNOT_POLYGON  ? "PolygonDimension"
                                                          : nullptr;
    if (intent) {
      zebkit::pdf::Value name;
      name.kind = zebkit::pdf::Value::Kind::kName;
      name.text = intent;
      zebkit::pdf::SetAnnotationEntry(pdf->document, annot, "IT", name);
    }
  }
  if (!json.contains("contents")) {
    const std::u16string text = base::UTF8ToUTF16(measured->text);
    FPDFAnnot_SetStringValue(annot, "Contents", Wide(text));
  }
  if (subtype == FPDF_ANNOT_LINE && !json.contains("caption")) {
    zebkit::pdf::Value yes;
    yes.kind = zebkit::pdf::Value::Kind::kBool;
    yes.boolean = true;
    zebkit::pdf::SetAnnotationEntry(pdf->document, annot, "Cap", yes);
  }
  *drawn = true;
  return std::string();
}

// One of the keys above on @annot: "" when done, what was wrong, or nothing
// when @key is not among them.
std::optional<std::string> ApplyAppearanceKey(zebkit_pdf* pdf,
                                              FPDF_PAGE page,
                                              FPDF_ANNOTATION annot,
                                              const Hook* kind,
                                              const std::string& key,
                                              const base::Value& value) {
  const int subtype = FPDFAnnot_GetSubtype(annot);
  auto set = [&](const char* entry, base::Value json) {
    std::optional<zebkit::pdf::Value> plain = JsonToEntry(json);
    return plain && zebkit::pdf::SetAnnotationEntry(pdf->document, annot, entry, *plain);
  };
  auto number_in = [](const base::Value* v, double low, double high) {
    return v && (v->is_int() || v->is_double()) && std::isfinite(v->GetDouble()) &&
           v->GetDouble() >= low && v->GetDouble() <= high;
  };
  auto remove = [&](std::initializer_list<const char*> entries) {
    for (const char* entry : entries)
      zebkit::pdf::RemoveAnnotationEntry(annot, entry);
    return std::string();
  };
  auto set_string = [&](const char* entry, const std::string& text) {
    const std::u16string wide = base::UTF8ToUTF16(text);
    return static_cast<bool>(FPDFAnnot_SetStringValue(annot, entry, Wide(wide)));
  };
  auto name_of = [](const char* name) { return base::Value(std::string("/") + name); };

  if (key == "lineEndings") {
    if (value.is_none())
      return remove({"LE"});
    if (subtype != FPDF_ANNOT_LINE && subtype != FPDF_ANNOT_POLYLINE)
      return "lineEndings are for lines and polylines";
    if (!value.is_list() || value.GetList().size() != 2)
      return "lineEndings is [start, end]";
    base::ListValue names;
    for (const base::Value& item : value.GetList()) {
      const char* name = item.is_string() ? FindPair(kEndings, item.GetString()) : nullptr;
      if (!name)
        return "no such line ending";
      names.Append(name_of(name));
    }
    set("LE", base::Value(std::move(names)));
    return std::string();
  }
  if (key == "leader") {
    if (value.is_none())
      return remove({"LL", "LLE", "LLO"});
    if (subtype != FPDF_ANNOT_LINE || !value.is_dict())
      return "leader is for lines, an object";
    for (const auto [name, item] : value.GetDict()) {
      const char* entry = name == "length" ? "LL" : name == "extension" ? "LLE" : name == "offset" ? "LLO" : nullptr;
      if (!entry || !number_in(&item, -1e6, 1e6))
        return "leader has length, extension and offset, numbers";
      if ((name == "extension" || name == "offset") && item.GetDouble() < 0)
        return "a leader's extension and offset are not negative";
      set(entry, item.Clone());
    }
    return std::string();
  }
  if (key == "caption") {
    if (value.is_none())
      return remove({"Cap", "CP", "CO"});
    if (subtype != FPDF_ANNOT_LINE || !value.is_dict())
      return "caption is for lines, an object";
    for (const auto [name, item] : value.GetDict()) {
      if (name == "show" && item.is_bool()) {
        set("Cap", item.Clone());
      } else if (name == "position" && item.is_string() &&
                 (item.GetString() == "inline" || item.GetString() == "top")) {
        set("CP", name_of(item.GetString() == "top" ? "Top" : "Inline"));
      } else if (name == "offset" && Numbers(&item, 2)) {
        set("CO", item.Clone());
      } else {
        return "caption has show, position (inline or top) and offset [x, y]";
      }
    }
    return std::string();
  }
  if (key == "borderStyle") {
    if (value.is_none())
      return remove({"BS"});
    if (!value.is_dict())
      return "borderStyle is an object";
    base::DictValue style;
    style.Set("Type", "/Border");
    style.Set("S", "/S");
    for (const auto [name, item] : value.GetDict()) {
      if (name == "style") {
        const char* letter = item.is_string() ? FindPair(kBorderStyles, item.GetString()) : nullptr;
        if (!letter)
          return "no such border style";
        style.Set("S", std::string("/") + letter);
      } else if (name == "width") {
        if (!number_in(&item, 0, 1000))
          return "a border's width is from 0 to 1000";
        style.Set("W", item.Clone());
        FPDFAnnot_SetBorder(annot, 0, 0, static_cast<float>(item.GetDouble()));
      } else if (name == "dash") {
        std::optional<std::vector<float>> dash = Numbers(&item, 0);
        if (!dash || dash->empty() || dash->size() > 32)
          return "a dash is 1 to 32 numbers";
        for (float length : *dash) {
          if (length < 0)
            return "a dash's lengths are not negative";
        }
        style.Set("D", item.Clone());
        style.Set("S", "/D");
      } else {
        return "borderStyle has style, width and dash";
      }
    }
    set("BS", base::Value(std::move(style)));
    return std::string();
  }
  if (key == "borderEffect") {
    if (value.is_none())
      return remove({"BE"});
    if (!value.is_dict())
      return "borderEffect is an object";
    const std::string* style = value.GetDict().FindString("style");
    const base::Value* intensity = value.GetDict().Find("intensity");
    if (!style || (*style != "none" && *style != "cloudy") ||
        (intensity && !number_in(intensity, 0, 2))) {
      return "borderEffect has style (none or cloudy) and intensity, 0 to 2";
    }
    base::DictValue effect;
    effect.Set("S", *style == "cloudy" ? "/C" : "/S");
    if (intensity)
      effect.Set("I", intensity->Clone());
    set("BE", base::Value(std::move(effect)));
    return std::string();
  }
  if (key == "blend") {
    if (value.is_none())
      return remove({"BM"});
    if (!value.is_string() || !InList(kBlends, value.GetString()))
      return "no such blend mode";
    set("BM", name_of(value.GetString().c_str()));
    return std::string();
  }
  if (key == "rotation") {
    if (value.is_none() || (number_in(&value, 0, 0)))
      return remove({"Rotate"});
    if (!number_in(&value, -36000, 36000))
      return "rotation is degrees";
    zebkit::pdf::SetAnnotationNumber(annot, "Rotate", std::fmod(value.GetDouble(), 360.0));
    return std::string();
  }
  if (key == "font" || key == "fontSize" || key == "textColor" || key == "borderColor") {
    if (subtype != FPDF_ANNOT_FREETEXT)
      return key + " is for free text";
    DefaultAppearance da = ParseDa(AnnotationString(annot, "DA"));
    if (key == "font") {
      if (!value.is_string() || value.GetString().empty() || value.GetString().size() > 256)
        return "font is a family's name";
      da.font = FontNameInDa(value.GetString());
    } else if (key == "fontSize") {
      if (!number_in(&value, 0.1, 4096))
        return "fontSize is from 0.1 to 4096";
      da.size = value.GetDouble();
    } else {
      std::optional<std::array<double, 3>> color = ColorKey(value);
      if (!color)
        return key + " is [r, g, b], each 0-255";
      (key == "textColor" ? da.fill : da.stroke) = *color;
    }
    set_string("DA", WriteDa(da));
    return std::string();
  }
  if (key == "align") {
    if (value.is_none())
      return remove({"Q"});
    const std::string* align = value.is_string() ? &value.GetString() : nullptr;
    if (!align || (*align != "left" && *align != "center" && *align != "right"))
      return "align is left, center or right";
    zebkit::pdf::SetAnnotationNumber(annot, "Q", *align == "left" ? 0 : *align == "center" ? 1 : 2);
    return std::string();
  }
  if (key == "richText" || key == "defaultStyle") {
    const char* entry = key == "richText" ? "RC" : "DS";
    if (value.is_none())
      return remove({entry});
    if (!value.is_string() || value.GetString().size() > (4u << 20) ||
        !base::IsStringUTF8(value.GetString())) {
      return key + " is a string of at most 4 MiB";
    }
    set_string(entry, value.GetString());
    return std::string();
  }
  if (key == "callout") {
    if (value.is_none())
      return remove({"CL", "RD"});
    if (subtype != FPDF_ANNOT_FREETEXT)
      return "callout is for free text";
    std::optional<std::vector<float>> points = Numbers(&value, 0);
    if (!points || (points->size() != 4 && points->size() != 6))
      return "callout is [x1, y1, x2, y2 (, x3, y3)]";
    set("CL", value.Clone());
    // A callout's text box is what "rect" was, or what is kept of it.
    return std::string();
  }
  if (key == "calloutEnding") {
    if (value.is_none())
      return remove({"LE"});
    const char* name = value.is_string() ? FindPair(kEndings, value.GetString()) : nullptr;
    if (subtype != FPDF_ANNOT_FREETEXT || !name)
      return "calloutEnding is for free text, a line ending";
    set("LE", name_of(name));
    return std::string();
  }
  if (key == "intent") {
    if (value.is_none())
      return remove({"IT"});
    if (!value.is_string() || value.GetString().empty() || value.GetString().size() > 64)
      return "intent is a name";
    for (char c : value.GetString()) {
      if (!base::IsAsciiAlphaNumeric(c))
        return "intent is a name, letters and digits";
    }
    set("IT", name_of(value.GetString().c_str()));
    return std::string();
  }
  if (key == "stamp") {
    if (subtype != FPDF_ANNOT_STAMP || !value.is_string() || !InList(kStampNames, value.GetString()))
      return "no such standard stamp";
    set("Name", name_of(value.GetString().c_str()));
    return std::string();
  }
  if (key == "stampText") {
    if (value.is_none())
      return remove({"ZebkitStampText"});
    if (subtype != FPDF_ANNOT_STAMP || !value.is_string() || value.GetString().empty() ||
        value.GetString().size() > 4096) {
      return "stampText is for stamps, a string of at most 4096 bytes";
    }
    set_string("ZebkitStampText", value.GetString());
    return std::string();
  }
  if (key == "icon") {
    const bool note = subtype == FPDF_ANNOT_TEXT, file = subtype == FPDF_ANNOT_FILEATTACHMENT;
    if ((!note && !file) || !value.is_string() ||
        !(note ? InList(kNoteIcons, value.GetString()) : InList(kAttachmentIcons, value.GetString()))) {
      return "no such icon for this type";
    }
    set("Name", name_of(value.GetString().c_str()));
    return std::string();
  }
  if (key == "caretSymbol") {
    if (subtype != FPDF_ANNOT_CARET || !value.is_string() ||
        (value.GetString() != "none" && value.GetString() != "paragraph")) {
      return "caretSymbol is for carets, none or paragraph";
    }
    set("Sy", name_of(value.GetString() == "none" ? "None" : "P"));
    return std::string();
  }
  if (key == "inReplyTo") {
    if (value.is_none())
      return remove({"IRT", "RT"});
    if (!value.is_int())
      return "inReplyTo is the index of an annotation of this page";
    FPDF_ANNOTATION target = FPDFPage_GetAnnot(page, value.GetInt());
    if (!target)
      return "inReplyTo is the index of an annotation of this page";
    const int target_object = zebkit::pdf::AnnotationObjectNumber(target);
    const int own = zebkit::pdf::AnnotationObjectNumber(annot);
    const bool cycle = !target_object || target_object == own ||
                       zebkit::pdf::ReplyChainReaches(target, own);
    FPDFPage_CloseAnnot(target);
    if (cycle)
      return "a reply cannot lead back to itself";
    set("IRT", base::Value(base::DictValue().Set("ref", target_object)));
    return std::string();
  }
  if (key == "replyType") {
    if (value.is_none())
      return remove({"RT"});
    if (!value.is_string() || (value.GetString() != "reply" && value.GetString() != "group"))
      return "replyType is reply or group";
    set("RT", name_of(value.GetString() == "reply" ? "R" : "Group"));
    return std::string();
  }
  if (key == "state") {
    if (value.is_none())
      return remove({"State"});
    const char* name = value.is_string() ? FindPair(kStateNames, value.GetString()) : nullptr;
    if (!name)
      return "no such state";
    set_string("State", name);
    return std::string();
  }
  if (key == "stateModel") {
    if (value.is_none())
      return remove({"StateModel"});
    if (!value.is_string() || (value.GetString() != "marking" && value.GetString() != "review"))
      return "stateModel is marking or review";
    set_string("StateModel", value.GetString() == "marking" ? "Marked" : "Review");
    return std::string();
  }
  if (key == "popup") {
    if (value.is_int())
      return std::string();  // what reading gave: its index
    if (value.is_none())
      return remove({"Popup"});
    if (!value.is_dict())
      return "popup is {rect, open}";
    std::optional<std::vector<float>> rect = Numbers(value.GetDict().Find("rect"), 4);
    const base::Value* open = value.GetDict().Find("open");
    if (!rect || (open && !open->is_bool()))
      return "popup is {rect, open}";
    const int own = zebkit::pdf::AnnotationObjectNumber(annot);
    FPDF_ANNOTATION popup = zebkit::pdf::CreateAnnotation(page, "Popup");
    if (!popup || !own)
      return "the popup cannot be made";
    FS_RECTF box = {(*rect)[0], (*rect)[3], (*rect)[2], (*rect)[1]};
    FPDFAnnot_SetRect(popup, &box);
    zebkit::pdf::Value parent;
    parent.kind = zebkit::pdf::Value::Kind::kRef;
    parent.number = own;
    zebkit::pdf::SetAnnotationEntry(pdf->document, popup, "Parent", parent);
    zebkit::pdf::Value is_open;
    is_open.kind = zebkit::pdf::Value::Kind::kBool;
    is_open.boolean = open && open->GetBool();
    zebkit::pdf::SetAnnotationEntry(pdf->document, popup, "Open", is_open);
    FPDFAnnot_SetFlags(popup, FPDF_ANNOT_FLAG_PRINT);
    zebkit::pdf::Value link;
    link.kind = zebkit::pdf::Value::Kind::kRef;
    link.number = zebkit::pdf::AnnotationObjectNumber(popup);
    zebkit::pdf::SetAnnotationEntry(pdf->document, annot, "Popup", link);
    FPDFPage_CloseAnnot(popup);
    return std::string();
  }
  if (key == "measure") {
    if (value.is_none())
      return remove({"Measure"});
    if (!value.is_dict())
      return "measure is {unit, scale, precision, label}";
    const base::DictValue& given = value.GetDict();
    for (const auto [name, item] : given) {
      if (name != "unit" && name != "scale" && name != "precision" && name != "label" &&
          name != "value" && name != "text") {
        return "measure has unit, scale, precision and label";
      }
    }
    const std::string* unit = given.FindString("unit");
    const base::Value* scale = given.Find("scale");
    const base::Value* precision = given.Find("precision");
    const std::string* label = given.FindString("label");
    if (!unit || unit->empty() || unit->size() > 32 || unit->starts_with("/") ||
        !number_in(scale, 1e-9, 1e9) || scale->GetDouble() <= 0 ||
        (precision && (!precision->is_int() || !number_in(precision, 0, 8))) ||
        (given.contains("label") && (!label || label->size() > 256 || label->starts_with("/")))) {
      return "measure needs a unit, a scale above 0, and a precision from 0 to 8";
    }
    const int digits = precision ? precision->GetInt() : 2;
    const double factor = scale->GetDouble();
    auto format = [&](const std::string& name, double c) {
      base::DictValue f;
      f.Set("Type", "/NumberFormat");
      f.Set("U", name);
      f.Set("C", c);
      f.Set("D", std::pow(10.0, digits));
      f.Set("F", "/D");
      base::ListValue list;
      list.Append(std::move(f));
      return list;
    };
    base::DictValue measure;
    measure.Set("Type", "/Measure");
    measure.Set("Subtype", "/RL");
    measure.Set("R", label ? *label : "1 pt = " + FixedText(factor, 4) + " " + *unit);
    measure.Set("X", format(*unit, factor));
    measure.Set("D", format(*unit, factor));
    measure.Set("A", format(*unit + "\xc2\xb2", factor * factor));
    set("Measure", base::Value(std::move(measure)));
    return std::string();
  }
  if (key == "data") {
    if (!kind)
      return "only an application's kind has data";
    set((kind->prefix + "Data").c_str(), value.Clone());
    return std::string();
  }
  return std::nullopt;
}

// The friendly names of what "entries" holds, for reading.
void DescribeEntries(zebkit_pdf* pdf, const base::DictValue& entries, base::DictValue& item) {
  const std::string* subtype = entries.FindString("Subtype");
  auto name_at = [](const base::Value* v) -> std::string {
    return v && v->is_string() && v->GetString().starts_with("/") ? v->GetString().substr(1) : std::string();
  };
  // A kind, or a subtype the list does not have.
  if (const std::string* type = item.FindString("type"); type && subtype) {
    if (std::shared_ptr<Hook> kind = zebkit::pdf::KindOf(pdf, subtype->substr(std::min<size_t>(1, subtype->size())), &entries)) {
      item.Set("type", kind->key);
      if (const base::Value* data = entries.Find(kind->prefix + "Data"))
        item.Set("data", data->Clone());
    } else if (*type == "unknown") {
      item.Set("subtype", subtype->substr(std::min<size_t>(1, subtype->size())));
    }
  }
  if (const base::ListValue* endings = entries.FindList("LE")) {
    base::ListValue names;
    for (const base::Value& ending : *endings) {
      const char* name = FindPairBack(kEndings, name_at(&ending));
      names.Append(name ? name : "none");
    }
    item.Set("lineEndings", std::move(names));
  } else if (const char* ending = FindPairBack(kEndings, name_at(entries.Find("LE")))) {
    item.Set("calloutEnding", ending);
  }
  if (entries.contains("LL") || entries.contains("LLE") || entries.contains("LLO")) {
    base::DictValue leader;
    leader.Set("length", entries.FindDouble("LL").value_or(0));
    leader.Set("extension", entries.FindDouble("LLE").value_or(0));
    leader.Set("offset", entries.FindDouble("LLO").value_or(0));
    item.Set("leader", std::move(leader));
  }
  if (entries.contains("Cap") || entries.contains("CP") || entries.contains("CO")) {
    base::DictValue caption;
    caption.Set("show", entries.FindBool("Cap").value_or(false));
    caption.Set("position", name_at(entries.Find("CP")) == "Top" ? "top" : "inline");
    if (const base::ListValue* offset = entries.FindList("CO"))
      caption.Set("offset", offset->Clone());
    item.Set("caption", std::move(caption));
  }
  if (const base::DictValue* style = entries.FindDict("BS")) {
    base::DictValue out;
    const char* name = FindPairBack(kBorderStyles, name_at(style->Find("S")));
    out.Set("style", name ? name : "solid");
    out.Set("width", style->FindDouble("W").value_or(1));
    if (const base::ListValue* dash = style->FindList("D"))
      out.Set("dash", dash->Clone());
    item.Set("borderStyle", std::move(out));
  }
  if (const base::DictValue* effect = entries.FindDict("BE")) {
    base::DictValue out;
    out.Set("style", name_at(effect->Find("S")) == "C" ? "cloudy" : "none");
    if (std::optional<double> intensity = effect->FindDouble("I"))
      out.Set("intensity", *intensity);
    item.Set("borderEffect", std::move(out));
  }
  if (const std::string blend = name_at(entries.Find("BM")); !blend.empty())
    item.Set("blend", blend);
  if (std::optional<double> rotation = entries.FindDouble("Rotate"))
    item.Set("rotation", *rotation);
  if (const std::string* da = entries.FindString("DA")) {
    // As a string that starts with a slash, "entries" has one more before it.
    const DefaultAppearance parts = ParseDa(da->starts_with("//") ? da->substr(1) : *da);
    item.Set("font", FontNameFromDa(parts.font));
    item.Set("fontSize", parts.size);
    auto color = [](const std::array<double, 3>& c) {
      base::ListValue list;
      for (double value : c)
        list.Append(static_cast<int>(std::clamp(value, 0.0, 1.0) * 255 + 0.5));
      return list;
    };
    if (parts.fill)
      item.Set("textColor", color(*parts.fill));
    if (parts.stroke)
      item.Set("borderColor", color(*parts.stroke));
  }
  if (std::optional<int> q = entries.FindInt("Q"))
    item.Set("align", *q == 1 ? "center" : *q == 2 ? "right" : "left");
  if (const std::string* rich = entries.FindString("RC"))
    item.Set("richText", *rich);
  if (const std::string* style = entries.FindString("DS"))
    item.Set("defaultStyle", *style);
  if (const base::ListValue* callout = entries.FindList("CL"))
    item.Set("callout", callout->Clone());
  if (const std::string intent = name_at(entries.Find("IT")); !intent.empty())
    item.Set("intent", intent);
  if (subtype) {
    const std::string name = name_at(entries.Find("Name"));
    if (*subtype == "/Stamp" && !name.empty())
      item.Set("stamp", name);
    if ((*subtype == "/Text" || *subtype == "/FileAttachment") && !name.empty())
      item.Set("icon", name);
    if (*subtype == "/Caret" && entries.contains("Sy"))
      item.Set("caretSymbol", name_at(entries.Find("Sy")) == "P" ? "paragraph" : "none");
  }
  if (const std::string* text = entries.FindString("ZebkitStampText"))
    item.Set("stampText", *text);
  if (const std::string rt = name_at(entries.Find("RT")); !rt.empty())
    item.Set("replyType", rt == "Group" ? "group" : "reply");
  if (const std::string* state = entries.FindString("State")) {
    const char* name = FindPairBack(kStateNames, *state);
    item.Set("state", name ? name : "none");
  }
  if (const std::string* model = entries.FindString("StateModel"))
    item.Set("stateModel", *model == "Review" ? "review" : "marking");
  if (const base::DictValue* measure = entries.FindDict("Measure")) {
    base::DictValue out;
    const base::ListValue* formats = measure->FindList("X");
    if (formats && !formats->empty() && (*formats)[0].is_dict()) {
      const base::DictValue& format = (*formats)[0].GetDict();
      if (const std::string* unit = format.FindString("U"))
        out.Set("unit", *unit);
      out.Set("scale", format.FindDouble("C").value_or(1));
      out.Set("precision",
              std::clamp(static_cast<int>(std::lround(std::log10(std::max(format.FindDouble("D").value_or(100), 1.0)))), 0, 8));
    }
    if (const std::string* label = measure->FindString("R"))
      out.Set("label", *label);
    if (std::optional<Measured> measured = MeasureValue(entries)) {
      out.Set("value", measured->value);
      out.Set("text", measured->text);
    }
    item.Set("measure", std::move(out));
  }
}

// Sets what @json says on @annot; @creating for a new one. Returns "" or
// what was wrong.
// What an annotation has that its appearance does not show.
constexpr auto kUndrawnKeys = base::MakeFixedFlatSet<std::string_view>(
    {"contents", "author", "subject", "name", "modified", "created", "flags"});
constexpr auto kUndrawnEntries = base::MakeFixedFlatSet<std::string_view>(
    {"Contents", "T", "Subj", "NM", "M", "CreationDate", "F", "IRT", "RT",
     "State", "StateModel", "Popup", "Open", "IT"});

std::string ApplyAnnotation(zebkit_pdf* pdf,
                            FPDF_PAGE page,
                            FPDF_ANNOTATION annot,
                            const base::DictValue& json,
                            const Hook* kind,
                            bool creating,
                            bool* redraw) {
  const int subtype = FPDFAnnot_GetSubtype(annot);
  // Whether what changes is drawn: an appearance is only made again then,
  // so that one made elsewhere stays when, say, a note's text changes.
  bool drawn = creating;
  for (const auto [key, value] : json) {
    if (key == "index" || key == "type" || key == "object" || key == "field" ||
        key == "appearances" || key == "regenerate" || key == "replies" ||
        key == "subtype" || key == "textRange" || key == "appearanceObject") {
      continue;
    }
    if (!kUndrawnKeys.contains(key) && !kUndrawnAppearanceKeys.contains(key))
      drawn = true;
    if (key == "contents" && (subtype == FPDF_ANNOT_FREETEXT || subtype == FPDF_ANNOT_LINE ||
                              subtype == FPDF_ANNOT_POLYGON || subtype == FPDF_ANNOT_POLYLINE)) {
      drawn = true;
    }
    if (const auto outcome = ApplyAppearanceKey(pdf, page, annot, kind, key, value)) {
      if (!outcome->empty())
        return *outcome;
      continue;
    }
    if (key == "entries") {
      if (!value.is_dict())
        return "entries is not an object";
      for (const auto [name, item] : value.GetDict()) {
        if (name == "AP" || name == "P" || name == "Subtype")
          return "entry " + name + " is not set this way";
        if (!kUndrawnEntries.contains(name))
          drawn = true;
        std::optional<zebkit::pdf::Value> entry = JsonToEntry(item);
        if (!entry || !zebkit::pdf::SetAnnotationEntry(pdf->document, annot,
                                                       name, *entry)) {
          return "entry " + name + " cannot be set";
        }
      }
    } else if (key == "rect") {
      std::optional<std::vector<float>> r = Numbers(&value, 4);
      if (!r)
        return "rect is not [left, bottom, right, top]";
      FS_RECTF rect = {(*r)[0], (*r)[3], (*r)[2], (*r)[1]};
      if (!FPDFAnnot_SetRect(annot, &rect))
        return "the rectangle cannot be set";
      // A new box: what covered a turned one or a callout goes.
      zebkit::pdf::RemoveAnnotationEntry(annot, "ZebkitBox");
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
      if (!json.contains("borderStyle"))
        zebkit::pdf::RemoveAnnotationEntry(annot, "BS");
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
  const std::string measured = UpdateMeasurement(pdf, annot, json, &drawn);
  if (!measured.empty())
    return measured;
  if (std::optional<bool> regenerate = json.FindBool("regenerate")) {
    drawn = *regenerate;
    if (*regenerate)
      zebkit::pdf::SetOwnAppearance(annot, false);
  }
  // Whatever said so, /IRT does not make a loop.
  const int object = zebkit::pdf::AnnotationObjectNumber(annot);
  if (object && zebkit::pdf::ReplyChainReaches(annot, object))
    return "a reply cannot lead back to itself";
  if (redraw)
    *redraw = drawn;
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
  Page page(pdf, index, /*parse=*/false);
  if (!page.get())
    return Fail(error, "no page " + std::to_string(index));
  base::ListValue list;
  const int count = FPDFPage_GetAnnotCount(page.get());
  // What a page's annotations may reach through references, all together.
  size_t budget = size_t{1} << 22;
  for (int i = 0; i < count; ++i) {
    FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page.get(), i);
    if (!annot)
      continue;
    list.Append(DescribeAnnotation(pdf, page.get(), annot, i, &budget));
    FPDFPage_CloseAnnot(annot);
  }
  // Who replies to whom on this page. A file's cycle of /IRT is only two lists.
  for (const base::Value& item : list) {
    const std::optional<int> target = item.GetDict().FindInt("inReplyTo");
    if (!target)
      continue;
    for (base::Value& other : list) {
      base::DictValue& dict = other.GetDict();
      if (dict.FindInt("index") == target) {
        base::ListValue* replies = dict.EnsureList("replies");
        replies->Append(item.GetDict().FindInt("index").value_or(-1));
      }
    }
  }
  return GiveJson(base::Value(std::move(list)));
}

// Quads and a rectangle for "textRange": [start, count].
static std::string ExpandTextRange(FPDF_PAGE page, base::DictValue& json) {
  const base::Value* range = json.Find("textRange");
  if (!range)
    return std::string();
  if (!range->is_list() || range->GetList().size() != 2 || !range->GetList()[0].is_int() ||
      !range->GetList()[1].is_int()) {
    return "textRange is [start, count]";
  }
  const int start = range->GetList()[0].GetInt();
  const int count = range->GetList()[1].GetInt();
  FPDF_TEXTPAGE text = FPDFText_LoadPage(page);
  if (!text)
    return "the page's text cannot be read";
  const int chars = FPDFText_CountChars(text);
  if (start < 0 || count <= 0 || start >= chars || count > chars - start) {
    FPDFText_ClosePage(text);
    return "textRange is outside the page's text";
  }
  base::ListValue quads;
  double left = 0, bottom = 0, right = 0, top = 0;
  const int rects = FPDFText_CountRects(text, start, count);
  for (int i = 0; i < rects; ++i) {
    double l = 0, t = 0, r = 0, b = 0;
    if (!FPDFText_GetRect(text, i, &l, &t, &r, &b))
      continue;
    // As markup has them: the top edge, then the bottom edge.
    base::ListValue quad;
    for (double value : {l, t, r, t, l, b, r, b})
      quad.Append(value);
    quads.Append(std::move(quad));
    left = quads.size() == 1 ? l : std::min(left, l);
    right = quads.size() == 1 ? r : std::max(right, r);
    bottom = quads.size() == 1 ? b : std::min(bottom, b);
    top = quads.size() == 1 ? t : std::max(top, t);
  }
  FPDFText_ClosePage(text);
  if (quads.empty())
    return "textRange covers nothing";
  json.Set("quads", std::move(quads));
  json.Set("rect", PdfRect(left, bottom, right, top));
  return std::string();
}

// The annotation's appearance, from hooks or the generator: outside the lock
// for what hooks draw. @object finds it again, whatever hooks have done.
static bool RedrawAppearance(zebkit_pdf* pdf,
                             int index,
                             int object,
                             bool kind_required,
                             char** error) {
  std::string json, type;
  double width = 0, height = 0;
  std::vector<std::string> wanted_fonts;
  int annotation = -1;
  std::shared_ptr<Hook> kind;
  Hooks providers;
  {
    base::AutoLock lock(PdfiumLock());
    Page page(pdf, index);
    annotation = page.get() ? zebkit::pdf::FindAnnotation(page.get(), object) : -1;
    FPDF_ANNOTATION annot = annotation >= 0 ? FPDFPage_GetAnnot(page.get(), annotation) : nullptr;
    if (!annot)
      return true;  // a hook took it away
    const int subtype = FPDFAnnot_GetSubtype(annot);
    type = subtype > 0 && subtype < static_cast<int>(std::size(kAnnotationTypes)) ? kAnnotationTypes[subtype] : "unknown";
    wanted_fonts = zebkit::pdf::WantedFonts(annot);
    if (zebkit::pdf::AnyHooks()) {
      base::Value entries = EntryToJson(zebkit::pdf::AnnotationEntries(annot));
      kind = zebkit::pdf::KindOf(pdf, zebkit::pdf::AnnotationSubtype(annot),
                                 entries.is_dict() ? &entries.GetDict() : nullptr);
      if (kind)
        type = kind->key;
      providers = HooksFor(HookType::kAppearance, pdf, type);
      if ((kind && kind->func) || !providers.empty()) {
        json = base::WriteJson(DescribeAnnotation(pdf, page.get(), annot, annotation)).value_or("{}");
        const std::array<float, 4> box = zebkit::pdf::AnnotationBox(annot);
        width = box[2] - box[0];
        height = box[3] - box[1];
      }
    }
    FPDFPage_CloseAnnot(annot);
  }
  // Fonts the application has for text the generator is about to write.
  if (!wanted_fonts.empty()) {
    const Hooks font_hooks = HooksFor(HookType::kFont, pdf, "*");
    for (const std::string& family : wanted_fonts) {
      if (pdf->fonts.bytes.count(family))
        continue;
      for (const auto& hook : font_hooks) {
        if (hook->removed.load())
          continue;
        size_t size = 0;
        uint8_t* bytes = reinterpret_cast<zebkit_pdf_font_func>(hook->func)(pdf, family.c_str(), &size, hook->data);
        if (bytes) {
          if (size && size <= (256u << 20))
            pdf->fonts.bytes[family].assign(bytes, bytes + size);
          free(bytes);
          break;
        }
      }
    }
  }
  auto draw = [&](zebkit_pdf_appearance_func func, void* data, bool* drawn) {
    if (width <= 0 || height <= 0 || width > 1e6 || height > 1e6) {
      *drawn = false;
      return true;
    }
    zebkit_canvas* canvas = zebkit::NewPdfCanvas(width, height);
    *drawn = func(canvas, pdf, index, annotation, json.c_str(), width, height, data);
    if (pdf->closing) {
      zebkit_canvas_close(canvas);
      Fail(error, "a hook closed the document");
      return false;
    }
    if (*drawn) {
      const zebkit::PdfCanvasOutput output = zebkit::TakePdfCanvas(canvas);
      base::AutoLock lock(PdfiumLock());
      Page page(pdf, index);
      const int now = page.get() ? zebkit::pdf::FindAnnotation(page.get(), object) : -1;
      if (FPDF_ANNOTATION annot = now >= 0 ? FPDFPage_GetAnnot(page.get(), now) : nullptr) {
        zebkit::pdf::SetCanvasAppearance(pdf->document, annot, output);
        FPDFPage_CloseAnnot(annot);
      }
    }
    zebkit_canvas_close(canvas);
    return true;
  };
  bool drawn = false;
  if (kind && kind->func && !kind->removed.load()) {
    if (!draw(reinterpret_cast<zebkit_pdf_appearance_func>(kind->func), kind->data, &drawn))
      return false;
    if (drawn)
      return true;
  }
  for (auto it = providers.rbegin(); it != providers.rend(); ++it) {
    if ((*it)->removed.load())
      continue;
    if (!draw(reinterpret_cast<zebkit_pdf_appearance_func>((*it)->func), (*it)->data, &drawn))
      return false;
    if (drawn)
      return true;
  }
  if (kind_required)
    return true;  // a kind draws itself or not at all
  base::AutoLock lock(PdfiumLock());
  Page page(pdf, index);
  const int now = page.get() ? zebkit::pdf::FindAnnotation(page.get(), object) : -1;
  if (FPDF_ANNOTATION annot = now >= 0 ? FPDFPage_GetAnnot(page.get(), now) : nullptr) {
    zebkit::pdf::GenerateAppearance(pdf->document, annot, pdf->fonts);
    FPDFPage_CloseAnnot(annot);
  }
  return true;
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
  ApiScope scope(pdf);
  if (HasEventHooks(pdf, "annotation.add", true)) {
    base::DictValue operation;
    operation.Set("page", index);
    operation.Set("annotation", object->Clone());
    if (!RunBefore(pdf, "annotation.add", operation, error))
      return -1;
    index = operation.FindInt("page").value_or(index);
    base::DictValue* changed = operation.FindDict("annotation");
    if (!changed) {
      Fail(error, "a hook's change has no annotation");
      return -1;
    }
    object = std::move(*changed);
  }
  const std::string* type = object->FindString("type");
  std::string subtype_name;
  for (size_t i = 1; type && i < std::size(kAnnotationTypes); ++i) {
    if (*type == kAnnotationTypes[i] &&
        (FPDFAnnot_IsSupportedSubtype(static_cast<FPDF_ANNOTATION_SUBTYPE>(i)) ||
         i == FPDF_ANNOT_CARET)) {
      subtype_name = kSubtypeNames[i];
    }
  }
  std::shared_ptr<Hook> kind = type && subtype_name.empty() ? zebkit::pdf::FindKind(pdf, *type) : nullptr;
  if (kind)
    subtype_name = kind->subtype.empty() ? kind->key : kind->subtype;
  if (subtype_name.empty()) {
    Fail(error, "an annotation of type " + (type ? *type : "(none)") +
                    " cannot be made");
    return -1;
  }
  const bool needs_no_rect =
      (object->contains("line") && *type == "line") || (object->contains("vertices") && (*type == "polygon" || *type == "polyline")) ||
      object->contains("textRange");
  if (!object->Find("rect") && !needs_no_rect) {
    Fail(error, "an annotation needs a rect");
    return -1;
  }
  if (kind) {
    const base::Value no_data{base::Value::Type::DICT};
    const base::Value* data = object->Find("data");
    const std::string problem = zebkit::pdf::CheckKindData(*kind, data ? *data : no_data);
    if (!problem.empty()) {
      Fail(error, problem);
      return -1;
    }
    if (!data)
      object->Set("data", base::DictValue());
  }
  int added = -1, number = 0;
  bool redraw = true;
  {
    base::AutoLock lock(PdfiumLock());
    Page page(pdf, index);
    if (!page.get()) {
      Fail(error, "no page " + std::to_string(index));
      return -1;
    }
    std::string problem = ExpandTextRange(page.get(), *object);
    if (!problem.empty()) {
      Fail(error, problem);
      return -1;
    }
    FPDF_ANNOTATION annot = zebkit::pdf::CreateAnnotation(page.get(), subtype_name.c_str());
    if (!annot) {
      Fail(error, "the annotation cannot be made");
      return -1;
    }
    number = zebkit::pdf::AnnotationObjectNumber(annot);
    if (kind && !kind->subtype.empty()) {
      zebkit::pdf::Value name;
      name.kind = zebkit::pdf::Value::Kind::kName;
      name.text = kind->key;
      zebkit::pdf::SetAnnotationEntry(pdf->document, annot, kind->prefix + "Kind", name);
    }
    problem = ApplyAnnotation(pdf, page.get(), annot, *object, kind.get(), true, &redraw);
    added = FPDFPage_GetAnnotIndex(page.get(), annot);
    FPDFPage_CloseAnnot(annot);
    if (!problem.empty()) {
      // A popup it made goes with it.
      const int count = FPDFPage_GetAnnotCount(page.get());
      for (int i = count - 1; i > added; --i)
        FPDFPage_RemoveAnnot(page.get(), i);
      FPDFPage_RemoveAnnot(page.get(), added);
      Fail(error, problem);
      return -1;
    }
  }
  if (redraw && !RedrawAppearance(pdf, index, number, kind != nullptr, error))
    return -1;
  if (HasEventHooks(pdf, "annotation.add", false)) {
    base::DictValue operation;
    operation.Set("page", index);
    {
      base::AutoLock lock(PdfiumLock());
      Page page(pdf, index);
      added = zebkit::pdf::FindAnnotation(page.get(), number);
      FPDF_ANNOTATION annot = added >= 0 ? FPDFPage_GetAnnot(page.get(), added) : nullptr;
      if (annot) {
        operation.Set("annotation", DescribeAnnotation(pdf, page.get(), annot, added));
        FPDFPage_CloseAnnot(annot);
      }
    }
    operation.Set("index", added);
    base::DictValue inverse;
    inverse.Set("event", "annotation.remove");
    inverse.Set("page", index);
    inverse.Set("index", added);
    operation.Set("inverse", std::move(inverse));
    RunAfter(pdf, "annotation.add", std::move(operation));
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
  ApiScope scope(pdf);
  if (HasEventHooks(pdf, "annotation.update", true)) {
    base::DictValue operation;
    operation.Set("page", index);
    operation.Set("index", annotation);
    operation.Set("annotation", object->Clone());
    if (!RunBefore(pdf, "annotation.update", operation, error))
      return false;
    index = operation.FindInt("page").value_or(index);
    annotation = operation.FindInt("index").value_or(annotation);
    base::DictValue* changed = operation.FindDict("annotation");
    if (!changed)
      return FailFalse(error, "a hook's change has no annotation");
    object = std::move(*changed);
  }
  // An application's data: what it has with these changes, checked outside
  // the lock, as the kind's own check may call in.
  std::shared_ptr<Hook> kind;
  if (zebkit::pdf::AnyHooks()) {
    base::AutoLock lock(PdfiumLock());
    Page page(pdf, index, /*parse=*/false);
    FPDF_ANNOTATION annot = page.get() ? FPDFPage_GetAnnot(page.get(), annotation) : nullptr;
    if (annot) {
      base::Value entries = EntryToJson(zebkit::pdf::AnnotationEntries(annot));
      if (entries.is_dict()) {
        kind = zebkit::pdf::KindOf(pdf, zebkit::pdf::AnnotationSubtype(annot), &entries.GetDict());
        if (kind && object->contains("data")) {
          base::DictValue merged;
          if (const base::DictValue* existing = entries.GetDict().FindDict(kind->prefix + "Data"))
            merged = existing->Clone();
          const base::Value* changes = object->Find("data");
          if (!changes->is_dict()) {
            FPDFPage_CloseAnnot(annot);
            return FailFalse(error, "data is not an object");
          }
          for (const auto [key, value] : changes->GetDict()) {
            if (value.is_none())
              merged.Remove(key);
            else
              merged.Set(key, value.Clone());
          }
          object->Set("data", std::move(merged));
        }
      }
      FPDFPage_CloseAnnot(annot);
    }
  }
  if (kind && object->contains("data")) {
    const std::string problem = zebkit::pdf::CheckKindData(*kind, *object->Find("data"));
    if (!problem.empty())
      return FailFalse(error, problem);
  }
  const bool told = HasEventHooks(pdf, "annotation.update", false);
  base::DictValue inverse_changes;
  int number = 0;
  bool redraw = false;
  {
    base::AutoLock lock(PdfiumLock());
    Page page(pdf, index);
    if (!page.get())
      return FailFalse(error, "no page " + std::to_string(index));
    FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page.get(), annotation);
    if (!annot)
      return FailFalse(error, "no annotation " + std::to_string(annotation));
    number = zebkit::pdf::MakeAnnotationIndirect(page.get(), annotation);
    const zebkit::pdf::AnnotationBackup backup(annot);
    const int annotations_before = FPDFPage_GetAnnotCount(page.get());
    const zebkit::pdf::Value before = zebkit::pdf::AnnotationEntries(annot);
    const int before_appearance = zebkit::pdf::AppearanceObject(annot);
    std::string problem = ExpandTextRange(page.get(), *object);
    if (problem.empty())
      problem = ApplyAnnotation(pdf, page.get(), annot, *object, kind.get(), false, &redraw);
    if (const std::optional<int> appearance = object->FindInt("appearanceObject");
        appearance && problem.empty()) {
      if (!zebkit::pdf::SetAppearanceObject(pdf->document, annot, *appearance))
        problem = "appearanceObject is not an appearance stream of this document";
    }
    const zebkit::pdf::Value after = zebkit::pdf::AnnotationEntries(annot);
    if (!problem.empty()) {
      // All or nothing: what it had, back, and a popup it made gone.
      backup.Restore(annot);
      FPDFPage_CloseAnnot(annot);
      for (int i = FPDFPage_GetAnnotCount(page.get()) - 1; i >= annotations_before; --i)
        FPDFPage_RemoveAnnot(page.get(), i);
      return FailFalse(error, problem);
    }
    if (told) {
      base::Value old_entries = EntryToJson(before);
      base::Value new_entries = EntryToJson(after);
      if (old_entries.is_dict() && new_entries.is_dict()) {
        base::DictValue& dict = old_entries.GetDict();
        dict.Remove("Subtype");
        for (const auto& [key, value] : after.entries) {
          if (key != "Subtype" && !dict.contains(key))
            dict.Set(key, base::Value());
        }
        const base::Value* old_date = dict.Find("M");
        inverse_changes.Set("modified", old_date ? old_date->Clone() : base::Value());
        // Only what changed: "entries" cannot say a stream, and what it cannot
        // say (a file attachment's file) would go with the undoing.
        for (const auto [key, value] : new_entries.GetDict()) {
          if (const base::Value* old_value = dict.Find(key); old_value && *old_value == value)
            dict.Remove(key);
        }
        inverse_changes.Set("entries", std::move(dict));
      }
      inverse_changes.Set("regenerate", false);
      if (before_appearance)
        inverse_changes.Set("appearanceObject", before_appearance);
    }
    FPDFPage_CloseAnnot(annot);
  }
  if (redraw && !RedrawAppearance(pdf, index, number, kind != nullptr, error))
    return false;
  if (told) {
    base::DictValue operation;
    operation.Set("page", index);
    operation.Set("index", annotation);
    operation.Set("annotation", std::move(*object));
    base::DictValue inverse;
    inverse.Set("event", "annotation.update");
    inverse.Set("page", index);
    inverse.Set("index", annotation);
    inverse.Set("annotation", std::move(inverse_changes));
    operation.Set("inverse", std::move(inverse));
    RunAfter(pdf, "annotation.update", std::move(operation));
  }
  return true;
}

bool zebkit_pdf_annotation_remove(zebkit_pdf* pdf,
                                  int index,
                                  int annotation,
                                  char** error) {
  ApiScope scope(pdf);
  if (HasEventHooks(pdf, "annotation.remove", true)) {
    base::DictValue operation;
    operation.Set("page", index);
    operation.Set("index", annotation);
    if (!RunBefore(pdf, "annotation.remove", operation, error))
      return false;
    index = operation.FindInt("page").value_or(index);
    annotation = operation.FindInt("index").value_or(annotation);
  }
  base::ListValue removed;
  {
    base::AutoLock lock(PdfiumLock());
    Page page(pdf, index, /*parse=*/false);
    if (!page.get())
      return FailFalse(error, "no page " + std::to_string(index));
    const int count = FPDFPage_GetAnnotCount(page.get());
    if (annotation < 0 || annotation >= count)
      return FailFalse(error, "no annotation " + std::to_string(annotation));
    // It, its popup, what replies to any of those, and their popups.
    std::vector<bool> going(static_cast<size_t>(count), false);
    going[annotation] = true;
    // Read once: a file's long chain of replies would have them read each round.
    struct Links {
      int popup = -1, answered = -1, parent = -1;
    };
    std::vector<Links> links(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
      FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page.get(), i);
      if (!annot)
        continue;
      auto linked = [&](const char* key) {
        FPDF_ANNOTATION other = FPDFAnnot_GetLinkedAnnot(annot, key);
        if (!other)
          return -1;
        const int at = FPDFPage_GetAnnotIndex(page.get(), other);
        FPDFPage_CloseAnnot(other);
        return at;
      };
      links[i] = {linked("Popup"), linked("IRT"), linked("Parent")};
      FPDFPage_CloseAnnot(annot);
    }
    for (bool grew = true; grew;) {
      grew = false;
      for (int i = 0; i < count; ++i) {
        const auto [popup, answered, parent] = links[i];
        const bool popup_of_going = parent >= 0 && going[parent];
        if (!going[i] && ((answered >= 0 && going[answered]) || popup_of_going)) {
          going[i] = true;
          grew = true;
        }
        if (going[i] && popup >= 0 && !going[popup]) {
          going[popup] = true;
          grew = true;
        }
      }
    }
    std::vector<std::pair<int, int>> items;
    for (int i = 0; i < count; ++i) {
      if (going[i])
        items.emplace_back(i, zebkit::pdf::MakeAnnotationIndirect(page.get(), i));
    }
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
      if (!FPDFPage_RemoveAnnot(page.get(), it->first))
        return FailFalse(error, "no annotation " + std::to_string(it->first));
    }
    for (const auto& [at, object] : items)
      removed.Append(base::DictValue().Set("index", at).Set("object", object));
  }
  if (HasEventHooks(pdf, "annotation.remove", false)) {
    base::DictValue operation;
    operation.Set("page", index);
    operation.Set("index", annotation);
    operation.Set("removed", removed.Clone());
    base::DictValue inverse;
    inverse.Set("event", "annotation.restore");
    inverse.Set("page", index);
    inverse.Set("items", std::move(removed));
    operation.Set("inverse", std::move(inverse));
    RunAfter(pdf, "annotation.remove", std::move(operation));
  }
  return true;
}

int zebkit_pdf_annotation_at(zebkit_pdf* pdf,
                             int index,
                             double x,
                             double y,
                             double tolerance,
                             char** error) {
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(tolerance) || tolerance < 0) {
    Fail(error, "a point is finite, a tolerance not negative");
    return -1;
  }
  struct Candidate {
    int index;
    std::shared_ptr<Hook> kind;
    std::string json;
  };
  std::vector<Candidate> candidates;
  ApiScope scope(pdf);
  {
    base::AutoLock lock(PdfiumLock());
    Page page(pdf, index, /*parse=*/false);
    if (!page.get()) {
      Fail(error, "no page " + std::to_string(index));
      return -1;
    }
    for (int i = FPDFPage_GetAnnotCount(page.get()) - 1; i >= 0; --i) {
      FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page.get(), i);
      if (!annot)
        continue;
      FS_RECTF rect = {};
      const int flags = FPDFAnnot_GetFlags(annot);
      const int subtype = FPDFAnnot_GetSubtype(annot);
      bool hit = !(flags & (FPDF_ANNOT_FLAG_HIDDEN | FPDF_ANNOT_FLAG_INVISIBLE | FPDF_ANNOT_FLAG_NOVIEW)) &&
                 subtype != FPDF_ANNOT_POPUP && FPDFAnnot_GetRect(annot, &rect) &&
                 x >= std::min(rect.left, rect.right) - tolerance &&
                 x <= std::max(rect.left, rect.right) + tolerance &&
                 y >= std::min(rect.bottom, rect.top) - tolerance &&
                 y <= std::max(rect.bottom, rect.top) + tolerance;
      if (hit && subtype == FPDF_ANNOT_LINE) {
        FS_POINTF start = {}, end = {};
        if (FPDFAnnot_GetLine(annot, &start, &end)) {
          // Within the line's width, not its rectangle's diagonal.
          float h = 0, v = 0, width = 1;
          FPDFAnnot_GetBorder(annot, &h, &v, &width);
          const double dx = end.x - start.x, dy = end.y - start.y;
          const double length2 = dx * dx + dy * dy;
          const double t = length2 > 0 ? std::clamp(((x - start.x) * dx + (y - start.y) * dy) / length2, 0.0, 1.0) : 0;
          hit = std::hypot(x - (start.x + t * dx), y - (start.y + t * dy)) <= tolerance + std::max<double>(width, 1) / 2 + 2;
        }
      }
      if (hit) {
        Candidate candidate{i, nullptr, std::string()};
        if (zebkit::pdf::AnyHooks()) {
          base::Value entries = EntryToJson(zebkit::pdf::AnnotationEntries(annot));
          candidate.kind = zebkit::pdf::KindOf(pdf, zebkit::pdf::AnnotationSubtype(annot),
                                               entries.is_dict() ? &entries.GetDict() : nullptr);
          if (candidate.kind && candidate.kind->hit_test)
            candidate.json = base::WriteJson(DescribeAnnotation(pdf, page.get(), annot, i)).value_or("{}");
        }
        candidates.push_back(std::move(candidate));
      }
      FPDFPage_CloseAnnot(annot);
    }
  }
  for (const Candidate& candidate : candidates) {
    if (candidate.kind && candidate.kind->hit_test &&
        !candidate.kind->hit_test(pdf, index, candidate.index, candidate.json.c_str(), x, y, candidate.kind->data)) {
      continue;
    }
    return candidate.index;
  }
  return -1;
}

bool zebkit_pdf_annotation_move(zebkit_pdf* pdf, int index, int annotation, int to, char** error) {
  ApiScope scope(pdf);
  if (HasEventHooks(pdf, "annotation.move", true)) {
    base::DictValue operation;
    operation.Set("page", index);
    operation.Set("index", annotation);
    operation.Set("to", to);
    if (!RunBefore(pdf, "annotation.move", operation, error))
      return false;
    to = operation.FindInt("to").value_or(to);
    annotation = operation.FindInt("index").value_or(annotation);
    index = operation.FindInt("page").value_or(index);
  }
  {
    base::AutoLock lock(PdfiumLock());
    Page page(pdf, index, /*parse=*/false);
    if (!page.get())
      return FailFalse(error, "no page " + std::to_string(index));
    if (!zebkit::pdf::MoveAnnotation(page.get(), annotation, to))
      return FailFalse(error, "no annotation there");
  }
  if (HasEventHooks(pdf, "annotation.move", false)) {
    base::DictValue operation;
    operation.Set("page", index);
    operation.Set("index", annotation);
    operation.Set("to", to);
    base::DictValue inverse;
    inverse.Set("event", "annotation.move");
    inverse.Set("page", index);
    inverse.Set("index", to);
    inverse.Set("to", annotation);
    operation.Set("inverse", std::move(inverse));
    RunAfter(pdf, "annotation.move", std::move(operation));
  }
  return true;
}

int zebkit_pdf_annotation_copy(zebkit_pdf* pdf,
                               int index,
                               int annotation,
                               zebkit_pdf* target,
                               int target_page,
                               char** error) {
  if (!pdf || !target) {
    Fail(error, "no document");
    return -1;
  }
  base::AutoLock lock(PdfiumLock());
  Page from(pdf, index, /*parse=*/false);
  Page to(target, target_page, /*parse=*/false);
  if (!from.get() || !to.get()) {
    Fail(error, "no such page");
    return -1;
  }
  std::string problem;
  const int copied =
      zebkit::pdf::CopyAnnotation(from.get(), annotation, target->document, to.get(), &problem);
  if (copied < 0)
    Fail(error, problem);
  return copied;
}

bool zebkit_pdf_annotation_flatten(zebkit_pdf* pdf, int index, int annotation, char** error) {
  ApiScope scope(pdf);
  if (HasEventHooks(pdf, "annotation.flatten", true)) {
    base::DictValue operation;
    operation.Set("page", index);
    operation.Set("index", annotation);
    if (!RunBefore(pdf, "annotation.flatten", operation, error))
      return false;
    index = operation.FindInt("page").value_or(index);
    annotation = operation.FindInt("index").value_or(annotation);
  }
  {
    base::AutoLock lock(PdfiumLock());
    Page page(pdf, index);
    if (!page.get())
      return FailFalse(error, "no page " + std::to_string(index));
    FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page.get(), annotation);
    if (!annot)
      return FailFalse(error, "no annotation " + std::to_string(annotation));
    for (const std::string& prefix : zebkit::pdf::KindPrefixes(pdf))
      zebkit::pdf::KeepApplicationData(page.get(), annot, prefix);
    FPDFPage_CloseAnnot(annot);
    if (!zebkit::pdf::FlattenAnnotation(pdf->document, page.get(), annotation))
      return FailFalse(error, "the annotation has no appearance");
  }
  if (HasEventHooks(pdf, "annotation.flatten", false)) {
    base::DictValue operation;
    operation.Set("page", index);
    operation.Set("index", annotation);
    RunAfter(pdf, "annotation.flatten", std::move(operation));
  }
  return true;
}

bool zebkit_pdf_annotation_stamp_page(zebkit_pdf* pdf,
                                      int index,
                                      int annotation,
                                      zebkit_pdf* source,
                                      int source_page,
                                      char** error) {
  if (!pdf || !source)
    return FailFalse(error, "no document");
  base::AutoLock lock(PdfiumLock());
  if (source_page < 0 || source_page >= FPDF_GetPageCount(source->document))
    return FailFalse(error, "no page " + std::to_string(source_page) + " there");
  Page page(pdf, index);
  if (!page.get())
    return FailFalse(error, "no page " + std::to_string(index));
  FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page.get(), annotation);
  if (!annot)
    return FailFalse(error, "no annotation " + std::to_string(annotation));
  std::string problem;
  if (FPDFAnnot_GetSubtype(annot) != FPDF_ANNOT_STAMP)
    problem = "only a stamp takes a page";
  else if (!zebkit::pdf::SetPageAppearance(pdf->document, annot, source->document, source_page))
    problem = "the page cannot be made a stamp";
  FPDFPage_CloseAnnot(annot);
  if (!problem.empty())
    return FailFalse(error, problem);
  return true;
}

bool zebkit_pdf_annotation_attach_file(zebkit_pdf* pdf,
                                       int index,
                                       int annotation,
                                       const char* name,
                                       const void* data,
                                       size_t size,
                                       char** error) {
  if (!name || !*name || (!data && size) || size > (1u << 31))
    return FailFalse(error, "a file has a name, and at most 2 GiB of bytes");
  base::AutoLock lock(PdfiumLock());
  Page page(pdf, index, /*parse=*/false);
  if (!page.get())
    return FailFalse(error, "no page " + std::to_string(index));
  FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page.get(), annotation);
  if (!annot)
    return FailFalse(error, "no annotation " + std::to_string(annotation));
  std::string problem;
  if (FPDFAnnot_GetSubtype(annot) != FPDF_ANNOT_FILEATTACHMENT) {
    problem = "only a file attachment takes a file";
  } else {
    const std::u16string wide = Utf16(name);
    FPDF_ATTACHMENT attachment = FPDFAnnot_AddFileAttachment(annot, Wide(wide));
    if (!attachment ||
        !FPDFAttachment_SetFile(attachment, pdf->document, data, static_cast<unsigned long>(size))) {
      problem = "the file cannot be embedded";
    }
  }
  FPDFPage_CloseAnnot(annot);
  if (!problem.empty())
    return FailFalse(error, problem);
  return true;
}

uint8_t* zebkit_pdf_annotation_file(zebkit_pdf* pdf,
                                    int index,
                                    int annotation,
                                    char** name,
                                    size_t* size,
                                    char** error) {
  base::AutoLock lock(PdfiumLock());
  Page page(pdf, index, /*parse=*/false);
  if (!page.get())
    return Fail(error, "no page " + std::to_string(index));
  FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page.get(), annotation);
  if (!annot)
    return Fail(error, "no annotation " + std::to_string(annotation));
  FPDF_ATTACHMENT attachment = FPDFAnnot_GetFileAttachment(annot);
  uint8_t* out = nullptr;
  unsigned long length = 0;
  if (attachment && FPDFAttachment_GetFile(attachment, nullptr, 0, &length)) {
    std::vector<uint8_t> bytes(length);
    unsigned long got = 0;
    if (FPDFAttachment_GetFile(attachment, bytes.data(), length, &got)) {
      out = GiveBytes(bytes.data(), got, size);
      if (name) {
        *name = Give(Utf16String([&](void* buffer, unsigned long buffer_size) {
          return FPDFAttachment_GetName(attachment, static_cast<FPDF_WCHAR*>(buffer), buffer_size);
        }));
      }
    }
  }
  FPDFPage_CloseAnnot(annot);
  if (!out)
    return Fail(error, "the annotation has no file");
  return out;
}

bool zebkit_pdf_annotation_draw(zebkit_pdf* pdf, int index, int annotation, const char* ops, char** error) {
  std::array<float, 4> box = {};
  {
    base::AutoLock lock(PdfiumLock());
    Page page(pdf, index, /*parse=*/false);
    if (!page.get())
      return FailFalse(error, "no page " + std::to_string(index));
    FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page.get(), annotation);
    if (!annot)
      return FailFalse(error, "no annotation " + std::to_string(annotation));
    box = zebkit::pdf::AnnotationBox(annot);
    FPDFPage_CloseAnnot(annot);
  }
  const double width = box[2] - box[0], height = box[3] - box[1];
  if (width <= 0 || height <= 0)
    return FailFalse(error, "the annotation has no area");
  zebkit_canvas* canvas = zebkit::NewPdfCanvas(width, height);
  if (!zebkit_canvas_run(canvas, ops, error)) {
    zebkit_canvas_close(canvas);
    return false;
  }
  const zebkit::PdfCanvasOutput output = zebkit::TakePdfCanvas(canvas);
  zebkit_canvas_close(canvas);
  base::AutoLock lock(PdfiumLock());
  Page page(pdf, index, /*parse=*/false);
  FPDF_ANNOTATION annot = page.get() ? FPDFPage_GetAnnot(page.get(), annotation) : nullptr;
  if (!annot)
    return FailFalse(error, "no annotation " + std::to_string(annotation));
  const bool ok = zebkit::pdf::SetCanvasAppearance(pdf->document, annot, output);
  FPDFPage_CloseAnnot(annot);
  if (!ok)
    return FailFalse(error, "the appearance cannot be written");
  return true;
}

char* zebkit_pdf_annotation_action(zebkit_pdf* pdf, int index, int annotation, char** error) {
  ApiScope scope(pdf);
  base::DictValue action;
  {
    base::AutoLock lock(PdfiumLock());
    Page page(pdf, index, /*parse=*/false);
    if (!page.get())
      return Fail(error, "no page " + std::to_string(index));
    FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page.get(), annotation);
    if (!annot)
      return Fail(error, "no annotation " + std::to_string(annotation));
    base::DictValue described = DescribeAnnotation(pdf, page.get(), annot, annotation);
    FPDFPage_CloseAnnot(annot);
    const base::DictValue* entries = described.FindDict("entries");
    const base::DictValue* a = entries ? entries->FindDict("A") : nullptr;
    const std::string* s = a ? a->FindString("S") : nullptr;
    auto text_of = [](const base::Value* value) -> std::string {
      if (!value)
        return std::string();
      if (value->is_string()) {
        const std::string& t = value->GetString();
        return t.starts_with("/") ? t.substr(1) : t;
      }
      if (value->is_dict()) {
        if (const std::string* f = value->GetDict().FindString("F"))
          return *f;
      }
      return std::string();
    };
    if (const std::string* url = described.FindString("url")) {
      action.Set("kind", "uri");
      action.Set("url", *url);
    } else if (std::optional<int> target = described.FindInt("page")) {
      action.Set("kind", "goto");
      action.Set("page", *target);
    } else if (!s) {
      action.Set("kind", "none");
    } else if (*s == "/Named") {
      action.Set("kind", "named");
      action.Set("name", text_of(a->Find("N")));
    } else if (*s == "/Launch") {
      action.Set("kind", "launch");
      action.Set("file", text_of(a->Find("F")));
    } else if (*s == "/JavaScript") {
      action.Set("kind", "javascript");
      action.Set("script", text_of(a->Find("JS")));
    } else {
      action.Set("kind", "other");
      action.Set("action", text_of(a->Find("S")));
    }
  }
  bool handled = false;
  const std::string json = base::WriteJson(action).value_or("{}");
  for (const auto& hook : HooksFor(HookType::kAction, pdf, "*")) {
    if (hook->removed.load() || pdf->closing)
      continue;
    if (reinterpret_cast<zebkit_pdf_action_func>(hook->func)(pdf, json.c_str(), hook->data)) {
      handled = true;
      break;
    }
  }
  action.Set("handled", handled);
  return GiveJson(base::Value(std::move(action)));
}

char* zebkit_pdf_page_data(zebkit_pdf* pdf, int index, const char* prefix, char** error) {
  if (!prefix || !*prefix)
    return Fail(error, "no prefix");
  base::AutoLock lock(PdfiumLock());
  Page page(pdf, index, /*parse=*/false);
  if (!page.get())
    return Fail(error, "no page " + std::to_string(index));
  return GiveJson(EntryToJson(zebkit::pdf::PageApplicationData(page.get(), prefix)));
}

bool zebkit_pdf_font_add(zebkit_pdf* pdf, const char* family, const void* font, size_t size, char** error) {
  if (!pdf || !family || !font || !size || size > (256u << 20))
    return FailFalse(error, "a font has a family, and at most 256 MiB of bytes");
  base::AutoLock lock(PdfiumLock());
  // Whether it is one: PDFium is what will read it.
  FPDF_FONT loaded = FPDFText_LoadFont(pdf->document, static_cast<const uint8_t*>(font),
                                       static_cast<uint32_t>(size), FPDF_FONT_TRUETYPE, /*cid=*/true);
  if (!loaded)
    return FailFalse(error, "not a font");
  auto existing = pdf->fonts.loaded.find(family);
  if (existing != pdf->fonts.loaded.end() && existing->second)
    FPDFFont_Close(existing->second);
  pdf->fonts.loaded[family] = loaded;
  const uint8_t* begin = static_cast<const uint8_t*>(font);
  pdf->fonts.bytes[family].assign(begin, begin + size);
  return true;
}

// ---- What runs OCR hooks ----

// The page's OCR lines, when it has no text of its own and a hook says what
// it reads; null otherwise. Outside the lock on entry.
static const std::vector<zebkit_pdf::OcrLine>* OcrLines(zebkit_pdf* pdf, int index) {
  if (!pdf)
    return nullptr;
  const Hooks hooks = HooksFor(HookType::kOcr, pdf, "*");
  if (hooks.empty())
    return nullptr;
  int object = 0;
  {
    base::AutoLock lock(PdfiumLock());
    Page page(pdf, index);
    if (!page.get() || !page.text() || FPDFText_CountChars(page.text()) > 0)
      return nullptr;
    object = zebkit::pdf::PageObjectNumber(page.get());
  }
  auto known = pdf->ocr.find(object);
  if (known != pdf->ocr.end())
    return &known->second;
  // Empty from here on: a hook that reads the page's text finds it so.
  pdf->ocr[object] = {};
  constexpr double kScale = 2;
  zebkit_pdf_render_options options = {};
  options.scale = kScale;
  options.background = 0xffffffff;
  int width = 0, height = 0;
  uint8_t* pixels = RenderPage(pdf, index, options, false, &width, &height, nullptr);
  if (!pixels)
    return &pdf->ocr[object];
  char* answer = nullptr;
  for (const auto& hook : hooks) {
    if (hook->removed.load() || pdf->closing)
      continue;
    answer = reinterpret_cast<zebkit_pdf_ocr_func>(hook->func)(pdf, index, pixels, width, height, kScale, hook->data);
    if (answer)
      break;
  }
  free(pixels);
  if (answer) {
    std::optional<base::Value> parsed =
        base::JSONReader::Read(answer, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
    free(answer);
    if (parsed && parsed->is_list()) {
      std::vector<zebkit_pdf::OcrLine> lines;
      for (const base::Value& item : parsed->GetList()) {
        if (!item.is_dict())
          continue;
        const std::string* text = item.GetDict().FindString("text");
        std::optional<std::vector<float>> box = Numbers(item.GetDict().Find("box"), 4);
        if (!text || !box || !base::IsStringUTF8(*text) || text->size() > (1u << 24))
          continue;
        lines.push_back({*text, std::vector<double>(box->begin(), box->end())});
      }
      pdf->ocr[object] = std::move(lines);
    }
  }
  return &pdf->ocr[object];
}

char* zebkit_pdf_text(zebkit_pdf* pdf, int index, char** error) {
  ApiScope scope(pdf);
  char* text = PageText(pdf, index, error);
  if (text && !*text) {
    if (const auto* lines = OcrLines(pdf, index)) {
      std::string joined;
      for (const auto& line : *lines) {
        if (!joined.empty())
          joined += "\n";
        joined += line.text;
      }
      if (!joined.empty()) {
        free(text);
        return Give(joined);
      }
    }
  }
  return text;
}

char* zebkit_pdf_text_lines(zebkit_pdf* pdf, int index, char** error) {
  ApiScope scope(pdf);
  char* answer = PageTextLines(pdf, index, error);
  if (answer && !strcmp(answer, "[]")) {
    if (const auto* lines = OcrLines(pdf, index); lines && !lines->empty()) {
      base::ListValue list;
      for (const auto& line : *lines) {
        base::DictValue item;
        item.Set("text", line.text);
        item.Set("box", PdfRect(line.box[0], line.box[1], line.box[2], line.box[3]));
        item.Set("unmapped", 0);
        item.Set("ocr", true);
        list.Append(std::move(item));
      }
      free(answer);
      return GiveJson(base::Value(std::move(list)));
    }
  }
  return answer;
}

char* zebkit_pdf_search(zebkit_pdf* pdf,
                        const char* query,
                        unsigned flags,
                        int first,
                        int limit,
                        char** error) {
  ApiScope scope(pdf);
  char* answer = SearchPages(pdf, query, flags, first, limit, error);
  if (!answer || !query || !*query || HooksFor(HookType::kOcr, pdf, "*").empty())
    return answer;
  std::optional<base::Value> parsed = base::JSONReader::Read(answer, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!parsed || !parsed->is_list())
    return answer;
  const bool match_case = flags & ZEBKIT_PDF_SEARCH_CASE;
  const std::string needle = match_case ? query : base::ToLowerASCII(query);
  bool added = false;
  const int pages = zebkit_pdf_page_count(pdf);
  for (int page = std::max(first, 0); page < pages; ++page) {
    const auto* lines = OcrLines(pdf, page);
    if (!lines)
      continue;
    for (const auto& line : *lines) {
      const std::string haystack = match_case ? line.text : base::ToLowerASCII(line.text);
      for (size_t at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + 1)) {
        base::DictValue match;
        match.Set("page", page);
        match.Set("start", static_cast<int>(at));
        match.Set("count", static_cast<int>(needle.size()));
        base::ListValue rects;
        rects.Append(PdfRect(line.box[0], line.box[1], line.box[2], line.box[3]));
        match.Set("rects", std::move(rects));
        match.Set("context", line.text);
        match.Set("ocr", true);
        parsed->GetList().Append(std::move(match));
        added = true;
      }
    }
  }
  if (!added)
    return answer;
  free(answer);
  // By page, then those found first; at most what was asked.
  std::stable_sort(parsed->GetList().begin(), parsed->GetList().end(),
                   [](const base::Value& a, const base::Value& b) {
                     return a.GetDict().FindInt("page").value_or(0) < b.GetDict().FindInt("page").value_or(0);
                   });
  if (limit > 0) {
    while (parsed->GetList().size() > static_cast<size_t>(limit))
      parsed->GetList().erase(parsed->GetList().end() - 1);
  }
  return GiveJson(*parsed);
}

char* zebkit_pdf_signatures(zebkit_pdf* pdf, char** error) {
  ApiScope scope(pdf);
  char* answer = ReadSignatures(pdf, error);
  const Hooks hooks = HooksFor(HookType::kTrust, pdf, "*");
  if (!answer || hooks.empty())
    return answer;
  std::optional<base::Value> parsed = base::JSONReader::Read(answer, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!parsed || !parsed->is_list())
    return answer;
  free(answer);
  int index = 0;
  for (base::Value& item : parsed->GetList()) {
    size_t size = 0;
    uint8_t* contents = zebkit_pdf_signature_contents(pdf, index++, &size, nullptr);
    const std::string json = base::WriteJson(item).value_or("{}");
    for (const auto& hook : hooks) {
      if (hook->removed.load() || pdf->closing)
        continue;
      const zebkit_pdf_trust trust = reinterpret_cast<zebkit_pdf_trust_func>(hook->func)(
          pdf, json.c_str(), contents, size, hook->data);
      if (trust != ZEBKIT_PDF_TRUST_UNKNOWN) {
        item.GetDict().Set("trusted", trust == ZEBKIT_PDF_TRUSTED);
        break;
      }
    }
    free(contents);
  }
  return GiveJson(*parsed);
}

// ---- Filters of a drawing tool ----

namespace {

using InputPoints = std::vector<std::pair<double, double>>;

std::optional<InputPoints> ParsePoints(const char* json) {
  std::optional<base::Value> parsed =
      base::JSONReader::Read(json ? json : "", base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!parsed || !parsed->is_list() || parsed->GetList().size() > 1000000)
    return std::nullopt;
  InputPoints points;
  for (const base::Value& item : parsed->GetList()) {
    std::optional<std::vector<float>> xy = Numbers(&item, 2);
    if (!xy)
      return std::nullopt;
    points.emplace_back((*xy)[0], (*xy)[1]);
  }
  return points;
}

std::string WritePoints(const InputPoints& points) {
  base::ListValue list;
  for (const auto& p : points) {
    base::ListValue xy;
    xy.Append(p.first);
    xy.Append(p.second);
    list.Append(std::move(xy));
  }
  return base::WriteJson(list).value_or("[]");
}

// Ramer-Douglas-Peucker, without recursion: a stroke can be long.
InputPoints Simplify(const InputPoints& points, double tolerance) {
  if (points.size() < 3)
    return points;
  std::vector<bool> keep(points.size(), false);
  keep.front() = keep.back() = true;
  std::vector<std::pair<size_t, size_t>> spans = {{0, points.size() - 1}};
  while (!spans.empty()) {
    const auto [from, to] = spans.back();
    spans.pop_back();
    double farthest = 0;
    size_t at = 0;
    const double dx = points[to].first - points[from].first;
    const double dy = points[to].second - points[from].second;
    const double length = std::hypot(dx, dy);
    for (size_t i = from + 1; i < to; ++i) {
      const double px = points[i].first - points[from].first;
      const double py = points[i].second - points[from].second;
      const double distance = length > 0 ? std::abs(px * dy - py * dx) / length : std::hypot(px, py);
      if (distance > farthest) {
        farthest = distance;
        at = i;
      }
    }
    if (farthest > tolerance) {
      keep[at] = true;
      spans.emplace_back(from, at);
      spans.emplace_back(at, to);
    }
  }
  InputPoints out;
  for (size_t i = 0; i < points.size(); ++i) {
    if (keep[i])
      out.push_back(points[i]);
  }
  return out;
}

}  // namespace

char* zebkit_pdf_filter_input(zebkit_pdf* pdf,
                              const char* tool,
                              const char* points_json,
                              const char* options_json,
                              char** error) {
  std::optional<InputPoints> points = ParsePoints(points_json);
  if (!points)
    return Fail(error, "the points are not a JSON array of at most 1000000 [x, y]");
  std::optional<base::DictValue> options;
  if (options_json) {
    options = ParseObject(options_json);
    if (!options)
      return Fail(error, "the options are not a JSON object");
    for (const auto [key, value] : *options) {
      const bool number = value.is_int() || value.is_double();
      if (key == "smooth") {
        if (!value.is_int() || value.GetInt() < 1 || value.GetInt() > 8)
          return Fail(error, "smooth is from 1 to 8");
        if (points->size() > 1000000u >> value.GetInt())
          return Fail(error, "too many points to smooth that much");
      } else if (key == "simplify" || key == "snapGrid" || key == "snapAngle") {
        if (!number || !std::isfinite(value.GetDouble()) || value.GetDouble() <= 0 || value.GetDouble() > 1e6)
          return Fail(error, key + " is a number above 0");
      } else if (key == "constrain") {
        if (!value.is_string() || (value.GetString() != "square" && value.GetString() != "circle"))
          return Fail(error, "constrain is square or circle");
      } else {
        return Fail(error, "no such filter: " + key);
      }
    }
    if (std::optional<int> passes = options->FindInt("smooth")) {
      // Chaikin's corners, the ends staying where they are.
      for (int pass = 0; pass < *passes && points->size() >= 3; ++pass) {
        InputPoints next;
        next.reserve(points->size() * 2);
        next.push_back(points->front());
        for (size_t i = 0; i + 1 < points->size(); ++i) {
          const auto& a = (*points)[i];
          const auto& b = (*points)[i + 1];
          next.emplace_back(a.first * 0.75 + b.first * 0.25, a.second * 0.75 + b.second * 0.25);
          next.emplace_back(a.first * 0.25 + b.first * 0.75, a.second * 0.25 + b.second * 0.75);
        }
        next.push_back(points->back());
        *points = std::move(next);
      }
    }
    if (std::optional<double> tolerance = options->FindDouble("simplify"))
      *points = Simplify(*points, *tolerance);
    if (std::optional<double> grid = options->FindDouble("snapGrid")) {
      for (auto& p : *points) {
        p.first = std::round(p.first / *grid) * *grid;
        p.second = std::round(p.second / *grid) * *grid;
      }
    }
    if (std::optional<double> degrees = options->FindDouble("snapAngle");
        degrees && points->size() >= 2) {
      const auto first = points->front();
      auto& last = points->back();
      const double dx = last.first - first.first, dy = last.second - first.second;
      const double step = *degrees * std::numbers::pi / 180;
      const double angle = std::round(std::atan2(dy, dx) / step) * step;
      const double length = std::hypot(dx, dy);
      last = {first.first + length * std::cos(angle), first.second + length * std::sin(angle)};
    }
    if (options->FindString("constrain") && points->size() >= 2) {
      const auto first = points->front();
      auto& last = points->back();
      const double dx = last.first - first.first, dy = last.second - first.second;
      const double side = std::max(std::abs(dx), std::abs(dy));
      last = {first.first + (dx < 0 ? -side : side), first.second + (dy < 0 ? -side : side)};
    }
  }
  std::string out = WritePoints(*points);
  ApiScope scope(pdf);
  // HooksFor() takes "" for every key: no tool is not all of them.
  if (tool && *tool) {
    for (const auto& hook : HooksFor(HookType::kInput, pdf, tool)) {
      if (hook->removed.load() || (pdf && pdf->closing))
        continue;
      char* changed = reinterpret_cast<zebkit_pdf_input_func>(hook->func)(pdf, tool, out.c_str(), hook->data);
      if (changed) {
        std::optional<InputPoints> checked = ParsePoints(changed);
        free(changed);
        if (!checked)
          return Fail(error, "a filter's points are not a JSON array of [x, y]");
        out = WritePoints(*checked);
      }
    }
  }
  return Give(out);
}

bool zebkit_pdf_annotation_set_image(zebkit_pdf* pdf,
                                     int index,
                                     int annotation,
                                     const uint8_t* rgba,
                                     int width,
                                     int height,
                                     char** error) {
  if (!rgba || width <= 0 || height <= 0 || width > (1 << 14) ||
      height > (1 << 14)) {
    return FailFalse(error, "the image has no pixels");
  }
  base::AutoLock lock(PdfiumLock());
  Page page(pdf, index);
  if (!page.get())
    return FailFalse(error, "no page " + std::to_string(index));
  FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page.get(), annotation);
  if (!annot)
    return FailFalse(error, "no annotation " + std::to_string(annotation));
  std::string failure;
  FS_RECTF rect = {};
  FPDF_PAGEOBJECT image = nullptr;
  FPDF_BITMAP bitmap = nullptr;
  if (FPDFAnnot_GetSubtype(annot) != FPDF_ANNOT_STAMP) {
    failure = "only a stamp takes an image";
  } else if (!FPDFAnnot_GetRect(annot, &rect)) {
    failure = "the annotation has no rectangle";
  } else {
    // PDFium's bitmaps are B, G, R, A.
    bitmap = FPDFBitmap_Create(width, height, /*alpha=*/1);
    auto* out = bitmap ? static_cast<uint8_t*>(FPDFBitmap_GetBuffer(bitmap)) : nullptr;
    const int stride = bitmap ? FPDFBitmap_GetStride(bitmap) : 0;
    for (int y = 0; out && y < height; ++y) {
      const uint8_t* from = rgba + static_cast<size_t>(y) * width * 4;
      uint8_t* to = out + static_cast<size_t>(y) * stride;
      for (int x = 0; x < width; ++x) {
        to[x * 4] = from[x * 4 + 2];
        to[x * 4 + 1] = from[x * 4 + 1];
        to[x * 4 + 2] = from[x * 4];
        to[x * 4 + 3] = from[x * 4 + 3];
      }
    }
    image = out ? FPDFPageObj_NewImageObj(pdf->document) : nullptr;
    FPDF_PAGE pages[] = {page.get()};
    if (!image || !FPDFImageObj_SetBitmap(pages, 1, image, bitmap) ||
        !FPDFImageObj_SetMatrix(image, rect.right - rect.left, 0, 0,
                                rect.top - rect.bottom, rect.left,
                                rect.bottom)) {
      failure = "the image cannot be made";
    } else {
      // The stamp's appearance is made of its objects from here on.
      while (FPDFAnnot_GetObjectCount(annot) > 0)
        FPDFAnnot_RemoveObject(annot, 0);
      if (FPDFAnnot_AppendObject(annot, image)) {
        image = nullptr;  // the annotation's now
        zebkit::pdf::SetOwnAppearance(annot, true);
      }
      else
        failure = "the image cannot be drawn in the stamp";
    }
  }
  if (image)
    FPDFPageObj_Destroy(image);
  if (bitmap)
    FPDFBitmap_Destroy(bitmap);
  FPDFPage_CloseAnnot(annot);
  if (!failure.empty())
    return FailFalse(error, failure);
  return true;
}

bool zebkit_pdf_flatten(zebkit_pdf* pdf,
                        int index,
                        bool for_print,
                        char** error) {
  ApiScope scope(pdf);
  if (HasEventHooks(pdf, "flatten", true)) {
    base::DictValue operation;
    operation.Set("page", index);
    operation.Set("print", for_print);
    if (!RunBefore(pdf, "flatten", operation, error))
      return false;
    index = operation.FindInt("page").value_or(index);
    for_print = operation.FindBool("print").value_or(for_print);
  }
  {
  base::AutoLock lock(PdfiumLock());
  Page page(pdf, index);
  if (!page.get())
    return FailFalse(error, "no page " + std::to_string(index));
  // What applications keep on annotations stays with the page.
  const std::vector<std::string> prefixes = zebkit::pdf::KindPrefixes(pdf);
  if (!prefixes.empty()) {
    const int count = FPDFPage_GetAnnotCount(page.get());
    for (int i = 0; i < count; ++i) {
      FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page.get(), i);
      if (!annot)
        continue;
      for (const std::string& prefix : prefixes)
        zebkit::pdf::KeepApplicationData(page.get(), annot, prefix);
      FPDFPage_CloseAnnot(annot);
    }
  }
  if (FPDFPage_Flatten(page.get(), for_print ? FLAT_PRINT
                                             : FLAT_NORMALDISPLAY) ==
      FLATTEN_FAIL) {
    return FailFalse(error, "the page cannot be flattened");
  }
  }
  if (HasEventHooks(pdf, "flatten", false)) {
    base::DictValue operation;
    operation.Set("page", index);
    operation.Set("print", for_print);
    RunAfter(pdf, "flatten", std::move(operation));
  }
  return true;
}

static int Redact(zebkit_pdf* pdf,
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

int zebkit_pdf_redact(zebkit_pdf* pdf,
                      int index,
                      const char* areas_json,
                      uint32_t fill,
                      char** error) {
  ApiScope scope(pdf);
  std::string areas = areas_json ? areas_json : "";
  bool has_areas = areas_json != nullptr;
  if (HasEventHooks(pdf, "redact", true)) {
    base::DictValue operation;
    operation.Set("page", index);
    std::optional<base::Value> parsed =
        has_areas ? base::JSONReader::Read(areas, base::JSON_PARSE_CHROMIUM_EXTENSIONS) : std::nullopt;
    // Not "the page's marks", which null says: a redaction not done as asked.
    if (has_areas && !parsed) {
      Fail(error, "the areas are not a JSON array of [l, b, r, t]");
      return -1;
    }
    operation.Set("areas", parsed ? std::move(*parsed) : base::Value());
    operation.Set("fill", static_cast<double>(fill));
    if (!RunBefore(pdf, "redact", operation, error))
      return -1;
    index = operation.FindInt("page").value_or(index);
    fill = static_cast<uint32_t>(operation.FindDouble("fill").value_or(fill));
    const base::Value* changed = operation.Find("areas");
    has_areas = changed && !changed->is_none();
    if (has_areas)
      areas = base::WriteJson(*changed).value_or("null");
  }
  const int count = Redact(pdf, index, has_areas ? areas.c_str() : nullptr, fill, error);
  if (count >= 0 && HasEventHooks(pdf, "redact", false)) {
    base::DictValue operation;
    operation.Set("page", index);
    operation.Set("fill", static_cast<double>(fill));
    operation.Set("count", count);
    RunAfter(pdf, "redact", std::move(operation));
  }
  return count;
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

static bool SetField(zebkit_pdf* pdf,
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

static bool ResetFields(zebkit_pdf* pdf, char** error) {
  if (!pdf)
    return FailFalse(error, "no document");
  base::AutoLock lock(PdfiumLock());
  FPDF_FORMHANDLE form = Form(pdf);
  if (!form || !zebkit::pdf::ResetForm(form))
    return FailFalse(error, "the document has no form");
  return true;
}

static bool RotatePage(zebkit_pdf* pdf,
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

static bool DeletePages(zebkit_pdf* pdf,
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

static bool MovePages(zebkit_pdf* pdf,
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

static bool InsertPage(zebkit_pdf* pdf,
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

static bool ImportPages(zebkit_pdf* pdf,
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

// The value a field has, for an undo: "" when it is not known.
static std::string FieldValue(zebkit_pdf* pdf, const char* name) {
  std::string value;
  char* json = zebkit_pdf_form_fields(pdf, nullptr);
  if (!json)
    return value;
  std::optional<base::Value> parsed = base::JSONReader::Read(json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  free(json);
  if (parsed && parsed->is_list()) {
    for (const base::Value& field : parsed->GetList()) {
      const std::string* field_name = field.GetDict().FindString("name");
      const std::string* field_value = field.GetDict().FindString("value");
      if (field_name && *field_name == name && field_value)
        value = *field_value;
    }
  }
  return value;
}

bool zebkit_pdf_form_set(zebkit_pdf* pdf,
                         const char* name,
                         const char* value,
                         char** error) {
  ApiScope scope(pdf);
  std::string changed_name = name ? name : "", changed_value = value ? value : "";
  if (name && value && HasEventHooks(pdf, "form.set", true)) {
    base::DictValue operation;
    operation.Set("name", changed_name);
    operation.Set("value", changed_value);
    if (!RunBefore(pdf, "form.set", operation, error))
      return false;
    if (const std::string* n = operation.FindString("name"))
      changed_name = *n;
    if (const std::string* v = operation.FindString("value"))
      changed_value = *v;
  }
  const bool told = HasEventHooks(pdf, "form.set", false);
  const std::string previous = told && name ? FieldValue(pdf, changed_name.c_str()) : std::string();
  if (!SetField(pdf, name ? changed_name.c_str() : nullptr, value ? changed_value.c_str() : nullptr, error))
    return false;
  if (told) {
    base::DictValue operation;
    operation.Set("name", changed_name);
    operation.Set("value", changed_value);
    operation.Set("previous", previous);
    base::DictValue inverse;
    inverse.Set("event", "form.set");
    inverse.Set("name", changed_name);
    inverse.Set("value", previous);
    operation.Set("inverse", std::move(inverse));
    RunAfter(pdf, "form.set", std::move(operation));
  }
  return true;
}

bool zebkit_pdf_form_reset(zebkit_pdf* pdf, char** error) {
  ApiScope scope(pdf);
  base::DictValue operation;
  if (!RunBefore(pdf, "form.reset", operation, error))
    return false;
  if (!ResetFields(pdf, error))
    return false;
  RunAfter(pdf, "form.reset", base::DictValue());
  return true;
}

bool zebkit_pdf_page_rotate(zebkit_pdf* pdf, int index, int rotation, char** error) {
  ApiScope scope(pdf);
  if (HasEventHooks(pdf, "page.rotate", true)) {
    base::DictValue operation;
    operation.Set("page", index);
    operation.Set("rotation", rotation);
    if (!RunBefore(pdf, "page.rotate", operation, error))
      return false;
    index = operation.FindInt("page").value_or(index);
    rotation = operation.FindInt("rotation").value_or(rotation);
  }
  const bool told = HasEventHooks(pdf, "page.rotate", false);
  int previous = 0;
  if (told) {
    base::AutoLock lock(PdfiumLock());
    Page page(pdf, index, /*parse=*/false);
    if (page.get())
      previous = FPDFPage_GetRotation(page.get()) * 90;
  }
  if (!RotatePage(pdf, index, rotation, error))
    return false;
  if (told) {
    base::DictValue operation;
    operation.Set("page", index);
    operation.Set("rotation", rotation);
    base::DictValue inverse;
    inverse.Set("event", "page.rotate");
    inverse.Set("page", index);
    inverse.Set("rotation", previous);
    operation.Set("inverse", std::move(inverse));
    RunAfter(pdf, "page.rotate", std::move(operation));
  }
  return true;
}

static base::ListValue IntsToList(const int* values, int count) {
  base::ListValue list;
  for (int i = 0; values && i < count; ++i)
    list.Append(values[i]);
  return list;
}

// A list of page indexes a hook gave, or the caller's. False when it is not one.
static bool PagesFrom(const base::DictValue& operation, std::vector<int>& pages) {
  const base::ListValue* list = operation.FindList("pages");
  if (!list)
    return false;
  pages.clear();
  for (const base::Value& item : *list) {
    if (!item.is_int())
      return false;
    pages.push_back(item.GetInt());
  }
  return true;
}

bool zebkit_pdf_pages_delete(zebkit_pdf* pdf, const int* pages, int count, char** error) {
  ApiScope scope(pdf);
  std::vector<int> list(pages, pages && count > 0 ? pages + count : pages);
  if (HasEventHooks(pdf, "page.delete", true)) {
    base::DictValue operation;
    operation.Set("pages", IntsToList(pages, count));
    if (!RunBefore(pdf, "page.delete", operation, error))
      return false;
    if (!PagesFrom(operation, list))
      return FailFalse(error, "a hook's pages are not a list of indexes");
  }
  if (!DeletePages(pdf, list.data(), static_cast<int>(list.size()), error))
    return false;
  if (HasEventHooks(pdf, "page.delete", false)) {
    base::DictValue operation;
    operation.Set("pages", IntsToList(list.data(), static_cast<int>(list.size())));
    RunAfter(pdf, "page.delete", std::move(operation));
  }
  return true;
}

bool zebkit_pdf_pages_move(zebkit_pdf* pdf, const int* pages, int count, int to, char** error) {
  ApiScope scope(pdf);
  std::vector<int> list(pages, pages && count > 0 ? pages + count : pages);
  if (HasEventHooks(pdf, "page.move", true)) {
    base::DictValue operation;
    operation.Set("pages", IntsToList(pages, count));
    operation.Set("to", to);
    if (!RunBefore(pdf, "page.move", operation, error))
      return false;
    if (!PagesFrom(operation, list))
      return FailFalse(error, "a hook's pages are not a list of indexes");
    to = operation.FindInt("to").value_or(to);
  }
  if (!MovePages(pdf, list.data(), static_cast<int>(list.size()), to, error))
    return false;
  if (HasEventHooks(pdf, "page.move", false)) {
    base::DictValue operation;
    operation.Set("pages", IntsToList(list.data(), static_cast<int>(list.size())));
    operation.Set("to", to);
    // One page goes back in one move; several may not.
    if (list.size() == 1) {
      base::DictValue inverse;
      inverse.Set("event", "page.move");
      inverse.Set("pages", IntsToList(&to, 1));
      inverse.Set("to", list[0]);
      operation.Set("inverse", std::move(inverse));
    }
    RunAfter(pdf, "page.move", std::move(operation));
  }
  return true;
}

bool zebkit_pdf_page_insert(zebkit_pdf* pdf, int at, double width, double height, char** error) {
  ApiScope scope(pdf);
  if (HasEventHooks(pdf, "page.insert", true)) {
    base::DictValue operation;
    operation.Set("at", at);
    operation.Set("width", width);
    operation.Set("height", height);
    if (!RunBefore(pdf, "page.insert", operation, error))
      return false;
    at = operation.FindInt("at").value_or(at);
    width = operation.FindDouble("width").value_or(width);
    height = operation.FindDouble("height").value_or(height);
  }
  if (!InsertPage(pdf, at, width, height, error))
    return false;
  if (HasEventHooks(pdf, "page.insert", false)) {
    base::DictValue operation;
    operation.Set("at", at);
    operation.Set("width", width);
    operation.Set("height", height);
    base::DictValue inverse;
    inverse.Set("event", "page.delete");
    inverse.Set("pages", IntsToList(&at, 1));
    operation.Set("inverse", std::move(inverse));
    RunAfter(pdf, "page.insert", std::move(operation));
  }
  return true;
}

bool zebkit_pdf_pages_import(zebkit_pdf* pdf, zebkit_pdf* from, const int* pages, int count, int at, char** error) {
  ApiScope scope(pdf);
  ApiScope from_scope(from);
  std::vector<int> list(pages, pages && count > 0 ? pages + count : pages);
  bool all = !pages;
  if (HasEventHooks(pdf, "page.import", true)) {
    base::DictValue operation;
    if (pages)
      operation.Set("pages", IntsToList(pages, count));
    else
      operation.Set("pages", base::Value());
    operation.Set("at", at);
    if (!RunBefore(pdf, "page.import", operation, error))
      return false;
    at = operation.FindInt("at").value_or(at);
    if (operation.FindList("pages")) {
      if (!PagesFrom(operation, list))
        return FailFalse(error, "a hook's pages are not a list of indexes");
      all = false;
    }
  }
  if (!ImportPages(pdf, from, all ? nullptr : list.data(), static_cast<int>(list.size()), at, error))
    return false;
  if (HasEventHooks(pdf, "page.import", false)) {
    base::DictValue operation;
    operation.Set("at", at);
    if (all)
      operation.Set("pages", base::Value());
    else
      operation.Set("pages", IntsToList(list.data(), static_cast<int>(list.size())));
    RunAfter(pdf, "page.import", std::move(operation));
  }
  return true;
}

bool zebkit_pdf_apply(zebkit_pdf* pdf, const char* json, char** error) {
  std::optional<base::DictValue> operation = ParseObject(json);
  if (!operation)
    return FailFalse(error, "the operation is not a JSON object");
  const std::string* event = operation->FindString("event");
  if (!event)
    return FailFalse(error, "the operation has no event");
  ApiScope scope(pdf);
  const std::optional<int> page = operation->FindInt("page");
  const std::optional<int> index = operation->FindInt("index");
  auto write = [](const base::DictValue& value) { return base::WriteJson(value).value_or("{}"); };
  if (*event == "annotation.add") {
    const base::DictValue* annotation = operation->FindDict("annotation");
    if (!page || !annotation)
      return FailFalse(error, "annotation.add has page and annotation");
    // As a read annotation has them: what is not set this way goes.
    base::DictValue clean = annotation->Clone();
    if (base::DictValue* entries = clean.FindDict("entries")) {
      entries->Remove("Subtype");
      entries->Remove("Popup");
      entries->Remove("IRT");
    }
    return zebkit_pdf_annotation_add(pdf, *page, write(clean).c_str(), error) >= 0;
  }
  if (*event == "annotation.update") {
    const base::DictValue* annotation = operation->FindDict("annotation");
    if (!page || !index || !annotation)
      return FailFalse(error, "annotation.update has page, index and annotation");
    return zebkit_pdf_annotation_update(pdf, *page, *index, write(*annotation).c_str(), error);
  }
  if (*event == "annotation.remove") {
    if (!page || !index)
      return FailFalse(error, "annotation.remove has page and index");
    return zebkit_pdf_annotation_remove(pdf, *page, *index, error);
  }
  if (*event == "annotation.move") {
    const std::optional<int> to = operation->FindInt("to");
    if (!page || !index || !to)
      return FailFalse(error, "annotation.move has page, index and to");
    return zebkit_pdf_annotation_move(pdf, *page, *index, *to, error);
  }
  if (*event == "annotation.restore") {
    const base::ListValue* items = operation->FindList("items");
    if (!page || !items || items->empty())
      return FailFalse(error, "annotation.restore has page and items");
    int first = -1;
    {
      base::AutoLock lock(PdfiumLock());
      Page loaded(pdf, *page, /*parse=*/false);
      if (!loaded.get())
        return FailFalse(error, "no page " + std::to_string(*page));
      for (const base::Value& item : *items) {
        const std::optional<int> at = item.is_dict() ? item.GetDict().FindInt("index") : std::nullopt;
        const std::optional<int> object = item.is_dict() ? item.GetDict().FindInt("object") : std::nullopt;
        if (!at || !object || !zebkit::pdf::RestoreAnnotation(loaded.get(), *object, *at))
          return FailFalse(error, "an annotation of that number cannot be put back");
        if (first < 0)
          first = *at;
      }
    }
    if (HasEventHooks(pdf, "annotation.restore", false)) {
      base::DictValue told = operation->Clone();
      base::DictValue inverse;
      inverse.Set("event", "annotation.remove");
      inverse.Set("page", *page);
      inverse.Set("index", first);
      told.Set("inverse", std::move(inverse));
      RunAfter(pdf, "annotation.restore", std::move(told));
    }
    return true;
  }
  if (*event == "form.set") {
    const std::string* name = operation->FindString("name");
    const std::string* value = operation->FindString("value");
    if (!name || !value)
      return FailFalse(error, "form.set has name and value");
    return zebkit_pdf_form_set(pdf, name->c_str(), value->c_str(), error);
  }
  std::vector<int> pages;
  const bool has_pages = PagesFrom(*operation, pages);
  if (*event == "page.insert") {
    const std::optional<int> at = operation->FindInt("at");
    const std::optional<double> width = operation->FindDouble("width");
    const std::optional<double> height = operation->FindDouble("height");
    if (!at || !width || !height)
      return FailFalse(error, "page.insert has at, width and height");
    return zebkit_pdf_page_insert(pdf, *at, *width, *height, error);
  }
  if (*event == "page.delete") {
    if (!has_pages)
      return FailFalse(error, "page.delete has pages");
    return zebkit_pdf_pages_delete(pdf, pages.data(), static_cast<int>(pages.size()), error);
  }
  if (*event == "page.move") {
    const std::optional<int> to = operation->FindInt("to");
    if (!has_pages || !to)
      return FailFalse(error, "page.move has pages and to");
    return zebkit_pdf_pages_move(pdf, pages.data(), static_cast<int>(pages.size()), *to, error);
  }
  if (*event == "page.rotate") {
    const std::optional<int> rotation = operation->FindInt("rotation");
    if (!page || !rotation)
      return FailFalse(error, "page.rotate has page and rotation");
    return zebkit_pdf_page_rotate(pdf, *page, *rotation, error);
  }
  return FailFalse(error, "an operation " + *event + " cannot be run");
}

}  // extern "C"

namespace zebkit::pdf {

base::Value EntriesToJson(const Value& value) {
  return EntryToJson(value);
}

}  // namespace zebkit::pdf
