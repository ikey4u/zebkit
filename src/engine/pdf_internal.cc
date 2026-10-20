// PDFium's classes for <zebkit/pdf.h>; nothing of base here (pdf_internal.h).

#include "zebkit/engine/pdf_internal.h"

#include <algorithm>

#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/page/cpdf_annotcontext.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_pageobject.h"
#include "core/fpdfapi/page/cpdf_textobject.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_boolean.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_null.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfdoc/cpdf_annot.h"
#include "core/fpdfdoc/cpdf_formcontrol.h"
#include "core/fpdfdoc/cpdf_formfield.h"
#include "core/fpdfdoc/cpdf_interactiveform.h"
#include "fpdfsdk/cpdfsdk_formfillenvironment.h"
#include "fpdfsdk/cpdfsdk_interactiveform.h"
#include "core/fxcrt/widestring.h"
#include "core/fxge/cfx_renderdevice.h"
#include "core/fxge/dib/cfx_dibitmap.h"
#include "fpdfsdk/cpdfsdk_helpers.h"

namespace zebkit::pdf {

bool SetInfo(FPDF_DOCUMENT document, const char* tag, const char* value) {
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(document);
  if (!pdf)
    return false;
  // patches/1014-pdf-document-information.patch
  RetainPtr<CPDF_Dictionary> info = pdf->GetOrCreateInfo();
  if (!info)
    return false;
  if (!value) {
    info->RemoveFor(tag);
    return true;
  }
  info->SetNewFor<CPDF_String>(tag, WideString::FromUTF8(value).AsStringView());
  return true;
}

namespace {

RetainPtr<CPDF_Dictionary> AnnotationDictionary(FPDF_ANNOTATION annot) {
  CPDF_AnnotContext* context = CPDFAnnotContextFromFPDFAnnotation(annot);
  return context ? context->GetMutableAnnotDict() : nullptr;
}

}  // namespace

bool SetAnnotationNumber(FPDF_ANNOTATION annot, const char* key, double value) {
  RetainPtr<CPDF_Dictionary> dict = AnnotationDictionary(annot);
  if (!dict)
    return false;
  dict->SetNewFor<CPDF_Number>(key, static_cast<float>(value));
  return true;
}

bool GetAnnotationColor(FPDF_ANNOTATION annot, const char* key, float rgb[3]) {
  RetainPtr<CPDF_Dictionary> dict = AnnotationDictionary(annot);
  RetainPtr<const CPDF_Array> color = dict ? dict->GetArrayFor(key) : nullptr;
  if (!color)
    return false;
  switch (color->size()) {
    case 1:
      rgb[0] = rgb[1] = rgb[2] = color->GetFloatAt(0);
      return true;
    case 3:
      for (size_t i = 0; i < 3; ++i)
        rgb[i] = color->GetFloatAt(i);
      return true;
    case 4: {
      const float k = color->GetFloatAt(3);
      for (size_t i = 0; i < 3; ++i)
        rgb[i] = (1 - color->GetFloatAt(i)) * (1 - k);
      return true;
    }
    default:
      return false;
  }
}

bool SetAnnotationColor(FPDF_ANNOTATION annot,
                        const char* key,
                        float r,
                        float g,
                        float b) {
  RetainPtr<CPDF_Dictionary> dict = AnnotationDictionary(annot);
  if (!dict)
    return false;
  auto color = dict->SetNewFor<CPDF_Array>(key);
  color->AppendNew<CPDF_Number>(r);
  color->AppendNew<CPDF_Number>(g);
  color->AppendNew<CPDF_Number>(b);
  return true;
}

bool RemoveAnnotationEntry(FPDF_ANNOTATION annot, const char* key) {
  RetainPtr<CPDF_Dictionary> dict = AnnotationDictionary(annot);
  if (!dict)
    return false;
  dict->RemoveFor(key);
  return true;
}

bool SetLinkPage(FPDF_DOCUMENT document, FPDF_ANNOTATION annot, int page) {
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(document);
  RetainPtr<CPDF_Dictionary> dict = AnnotationDictionary(annot);
  if (!pdf || !dict)
    return false;
  RetainPtr<CPDF_Dictionary> page_dict = pdf->GetMutablePageDictionary(page);
  if (!page_dict)
    return false;
  // [page /XYZ null null null]: the page, where it was, at the zoom it was.
  auto dest = dict->SetNewFor<CPDF_Array>("Dest");
  dest->AppendNew<CPDF_Reference>(pdf, page_dict->GetObjNum());
  dest->AppendNew<CPDF_Name>("XYZ");
  dest->AppendNew<CPDF_Null>();
  dest->AppendNew<CPDF_Null>();
  dest->AppendNew<CPDF_Null>();
  dict->RemoveFor("A");
  return true;
}

namespace {

CPDFSDK_InteractiveForm* SdkForm(FPDF_FORMHANDLE form) {
  CPDFSDK_FormFillEnvironment* environment =
      CPDFSDKFormFillEnvironmentFromFPDFFormHandle(form);
  return environment ? environment->GetInteractiveForm() : nullptr;
}

}  // namespace

std::string SetFieldValue(FPDF_FORMHANDLE form,
                          const char* name,
                          const char* value) {
  CPDFSDK_InteractiveForm* sdk = SdkForm(form);
  if (!sdk)
    return "the document has no form";
  CPDF_InteractiveForm* interactive = sdk->GetInteractiveForm();
  const WideString field_name = WideString::FromUTF8(name);
  const WideString wide_value = WideString::FromUTF8(value);
  CPDF_FormField* field = nullptr;
  for (size_t i = 0; i < interactive->CountFields(field_name); ++i) {
    CPDF_FormField* candidate = interactive->GetField(i, field_name);
    if (candidate && candidate->GetFullName() == field_name) {
      field = candidate;
      break;
    }
  }
  if (!field)
    return "no field of that name";
  // The read-only flag of the PDF reference, table 226.
  if (field->GetFieldFlags() & 1)
    return "the field is read-only";
  constexpr auto kNotify = NotificationOption::kNotify;
  switch (field->GetType()) {
    case CPDF_FormField::kText:
    case CPDF_FormField::kRichText:
    case CPDF_FormField::kComboBox:
      if (!field->SetValue(wide_value, kNotify))
        return "the value cannot be set";
      break;
    case CPDF_FormField::kListBox: {
      const int option = field->FindOption(wide_value);
      if (option < 0)
        return "the list has no such option";
      field->ClearSelection(kNotify);
      field->SetItemSelection(option, kNotify);
      break;
    }
    case CPDF_FormField::kCheckBox:
    case CPDF_FormField::kRadioButton: {
      bool found = wide_value == L"Off";
      for (int i = 0; i < field->CountControls(); ++i) {
        CPDF_FormControl* control = field->GetControl(i);
        const bool match = control->GetExportValue() == wide_value;
        if (match || wide_value == L"Off") {
          field->CheckControl(i, match, kNotify);
          found = found || match;
        }
      }
      if (!found)
        return "no button has that export value";
      break;
    }
    default:
      return "fields of this type have no value to set";
  }
  sdk->ResetFieldAppearance(field, std::nullopt);
  sdk->UpdateField(field);
  return std::string();
}

bool ResetForm(FPDF_FORMHANDLE form) {
  CPDFSDK_InteractiveForm* sdk = SdkForm(form);
  if (!sdk)
    return false;
  CPDF_InteractiveForm* interactive = sdk->GetInteractiveForm();
  interactive->ResetForm();
  for (size_t i = 0; i < interactive->CountFields(WideString()); ++i) {
    if (CPDF_FormField* field = interactive->GetField(i, WideString())) {
      sdk->ResetFieldAppearance(field, std::nullopt);
      sdk->UpdateField(field);
    }
  }
  return true;
}

int RedactObjects(FPDF_PAGE fpdf_page,
                  const std::vector<std::array<float, 4>>& areas) {
  CPDF_Page* page = CPDFPageFromFPDFPage(fpdf_page);
  if (!page)
    return -1;
  auto covered = [&](const CFX_FloatRect& rect) {
    for (const auto& a : areas) {
      CFX_FloatRect area(a[0], a[1], a[2], a[3]);
      area.Normalize();
      CFX_FloatRect overlap = area;
      overlap.Intersect(rect);
      if (!overlap.IsEmpty())
        return true;
    }
    return false;
  };
  int changed = 0;
  std::vector<CPDF_PageObject*> removed;
  for (size_t i = 0; i < page->GetPageObjectCount(); ++i) {
    CPDF_PageObject* object = page->GetPageObjectByIndex(i);
    if (!object || object->AsImage() || !covered(object->GetRect()))
      continue;
    CPDF_TextObject* text = object->AsText();
    RetainPtr<CPDF_Font> font = text ? text->GetFont() : nullptr;
    if (!text || !font || font->IsVertWriting()) {
      removed.push_back(object);
      continue;
    }
    // Each character's box: its advance, from the font's descent to its
    // ascent, through the text matrix.
    const CFX_Matrix matrix = text->GetTextMatrix();
    const float size = text->GetFontSize();
    const float ascent = font->GetTypeAscent() * size / 1000;
    const float descent = font->GetTypeDescent() * size / 1000;
    const std::vector<uint32_t> codes = text->GetCharCodes();
    const std::vector<float> positions = text->GetCharPositions();
    std::vector<size_t> kept;
    for (size_t c = 0; c < codes.size() && c < positions.size(); ++c) {
      const float left = positions[c];
      const float right = left + text->GetCharWidth(codes[c]);
      const CFX_FloatRect box = matrix.TransformRect(
          CFX_FloatRect(left, std::min(descent, 0.0f), right,
                        std::max(ascent, size)));
      if (!covered(box))
        kept.push_back(c);
    }
    if (kept.size() == codes.size())
      continue;
    if (kept.empty()) {
      removed.push_back(object);
      continue;
    }
    // The characters left, each where it was.
    ByteString bytes;
    for (size_t c : kept)
      font->AppendChar(&bytes, codes[c]);
    const float first = positions[kept[0]];
    std::vector<float> offsets;
    for (size_t k = 1; k < kept.size(); ++k)
      offsets.push_back(positions[kept[k]] - first);
    const CFX_PointF origin = matrix.Transform(CFX_PointF(first, 0));
    text->SetSegments(pdfium::span_from_ref(bytes), {});
    if (!offsets.empty())
      text->SetAbsolutePositions(offsets);
    text->SetPosition(origin);
    text->SetDirty(true);
    ++changed;
  }
  for (CPDF_PageObject* object : removed) {
    page->RemovePageObject(object);
    ++changed;
  }
  return changed;
}

void RegenerateAppearance(FPDF_DOCUMENT document, FPDF_ANNOTATION annot) {
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(document);
  RetainPtr<CPDF_Dictionary> dict = AnnotationDictionary(annot);
  if (!pdf || !dict)
    return;
  switch (FPDFAnnot_GetSubtype(annot)) {
    case FPDF_ANNOT_CIRCLE:
    case FPDF_ANNOT_FREETEXT:
    case FPDF_ANNOT_HIGHLIGHT:
    case FPDF_ANNOT_INK:
    case FPDF_ANNOT_LINE:
    case FPDF_ANNOT_POLYGON:
    case FPDF_ANNOT_POLYLINE:
    case FPDF_ANNOT_POPUP:
    case FPDF_ANNOT_SQUARE:
    case FPDF_ANNOT_SQUIGGLY:
    case FPDF_ANNOT_STRIKEOUT:
    case FPDF_ANNOT_TEXT:
    case FPDF_ANNOT_UNDERLINE:
      break;
    default:
      return;
  }
  dict->RemoveFor("AP");
  dict->RemoveFor("PDFIUM_HasGeneratedAP");
  // Making the annotation generates its appearance, which stays in the
  // dictionary.
  CPDF_Annot generated(dict, pdf);
}

int AppearanceModes(FPDF_ANNOTATION annot) {
  RetainPtr<CPDF_Dictionary> dict = AnnotationDictionary(annot);
  RetainPtr<const CPDF_Dictionary> ap = dict ? dict->GetDictFor("AP") : nullptr;
  if (!ap)
    return 0;
  int modes = 0;
  if (ap->KeyExist("N"))
    modes |= 1;
  if (ap->KeyExist("R"))
    modes |= 2;
  if (ap->KeyExist("D"))
    modes |= 4;
  return modes;
}

bool DrawAnnotation(FPDF_PAGE page,
                    FPDF_ANNOTATION annot,
                    int mode,
                    FPDF_BITMAP bitmap,
                    const float matrix[6]) {
  CPDF_Page* pdf_page = CPDFPageFromFPDFPage(page);
  RetainPtr<CPDF_Dictionary> dict = AnnotationDictionary(annot);
  RetainPtr<CFX_DIBitmap> pixels(CFXDIBitmapFromFPDFBitmap(bitmap));
  if (!pdf_page || !dict || !pixels || mode < 0 || mode > 2)
    return false;
  if (!(AppearanceModes(annot) & (1 << mode)))
    return false;
  constexpr CPDF_Annot::AppearanceMode kModes[] = {
      CPDF_Annot::AppearanceMode::kNormal, CPDF_Annot::AppearanceMode::kRollover,
      CPDF_Annot::AppearanceMode::kDown};
  CPDF_Annot annotation(dict, pdf_page->GetDocument());
#if defined(PDF_USE_SKIA)
  CFX_DIBitmap::ScopedPremultiplier premultiplier(pixels);
#endif
  std::unique_ptr<CFX_RenderDevice> device =
      CFX_RenderDevice::CreateForBitmap(pixels, /*rgb_byte_order=*/true);
  if (!device)
    return false;
  const CFX_Matrix to_device(matrix[0], matrix[1], matrix[2], matrix[3],
                             matrix[4], matrix[5]);
  return annotation.DrawAppearance(pdf_page, device.get(), to_device,
                                   kModes[mode]);
}

int PageObjectNumber(FPDF_PAGE page) {
  CPDF_Page* pdf_page = CPDFPageFromFPDFPage(page);
  RetainPtr<const CPDF_Dictionary> dict =
      pdf_page ? pdf_page->GetDict() : nullptr;
  return dict ? static_cast<int>(dict->GetObjNum()) : 0;
}

namespace {

// Deep enough for an annotation's entries (/BS, /BE, /MK and the like).
constexpr int kMaxDepth = 8;

// Whether @dict is an annotation's or a page's, which entries refer to
// rather than contain.
bool IsAnnotationOrPage(const CPDF_Dictionary* dict) {
  if (!dict)
    return false;
  const ByteString type = dict->GetNameFor("Type");
  if (type == "Page" || type == "Pages" || type == "Annot")
    return true;
  return dict->KeyExist("Subtype") && dict->KeyExist("Rect");
}

// @followed holds the object numbers being followed, against cycles.
Value ToValue(const CPDF_Object* object,
              int depth,
              std::vector<uint32_t>& followed) {
  Value value;
  if (!object || depth > kMaxDepth)
    return value;
  if (const CPDF_Reference* reference = object->AsReference()) {
    const uint32_t number = reference->GetRefObjNum();
    RetainPtr<const CPDF_Object> target = reference->GetDirect();
    const bool cycle =
        std::find(followed.begin(), followed.end(), number) != followed.end();
    if (!target || cycle || IsAnnotationOrPage(target->AsDictionary())) {
      value.kind = Value::Kind::kRef;
      value.number = number;
      return value;
    }
    followed.push_back(number);
    value = ToValue(target.Get(), depth, followed);
    followed.pop_back();
    return value;
  }
  if (const CPDF_Boolean* boolean = object->AsBoolean()) {
    value.kind = Value::Kind::kBool;
    value.boolean = boolean->GetInteger() != 0;
  } else if (const CPDF_Number* number = object->AsNumber()) {
    value.kind = Value::Kind::kNumber;
    value.number = number->GetNumber();
  } else if (const CPDF_String* string = object->AsString()) {
    value.kind = Value::Kind::kString;
    value.text = string->GetUnicodeText().ToUTF8().c_str();
  } else if (const CPDF_Name* name = object->AsName()) {
    value.kind = Value::Kind::kName;
    value.text = name->GetString().c_str();
  } else if (const CPDF_Array* array = object->AsArray()) {
    value.kind = Value::Kind::kArray;
    CPDF_ArrayLocker locker(array);
    for (const auto& item : locker)
      value.items.push_back(ToValue(item.Get(), depth + 1, followed));
  } else if (const CPDF_Dictionary* dict = object->AsDictionary()) {
    value.kind = Value::Kind::kDict;
    CPDF_DictionaryLocker locker(dict);
    for (const auto& [key, item] : locker) {
      value.entries.emplace_back(key.c_str(),
                                 ToValue(item.Get(), depth + 1, followed));
    }
  }
  return value;
}

RetainPtr<CPDF_Object> ToObject(CPDF_Document* document,
                                const Value& value,
                                int depth) {
  if (depth > kMaxDepth)
    return nullptr;
  switch (value.kind) {
    case Value::Kind::kNull:
      return pdfium::MakeRetain<CPDF_Null>();
    case Value::Kind::kBool:
      return pdfium::MakeRetain<CPDF_Boolean>(value.boolean);
    case Value::Kind::kNumber:
      if (value.number == static_cast<int>(value.number))
        return pdfium::MakeRetain<CPDF_Number>(static_cast<int>(value.number));
      return pdfium::MakeRetain<CPDF_Number>(static_cast<float>(value.number));
    case Value::Kind::kString:
      return pdfium::MakeRetain<CPDF_String>(
          nullptr, WideString::FromUTF8(value.text.c_str()).AsStringView());
    case Value::Kind::kName:
      return pdfium::MakeRetain<CPDF_Name>(nullptr, value.text.c_str());
    case Value::Kind::kRef: {
      const uint32_t number = static_cast<uint32_t>(value.number);
      if (!document || value.number < 1 ||
          !document->GetIndirectObject(number)) {
        return nullptr;
      }
      return pdfium::MakeRetain<CPDF_Reference>(document, number);
    }
    case Value::Kind::kArray: {
      auto array = pdfium::MakeRetain<CPDF_Array>();
      for (const Value& item : value.items) {
        RetainPtr<CPDF_Object> object = ToObject(document, item, depth + 1);
        if (!object)
          return nullptr;
        array->Append(std::move(object));
      }
      return array;
    }
    case Value::Kind::kDict: {
      auto dict = pdfium::MakeRetain<CPDF_Dictionary>();
      for (const auto& [key, item] : value.entries) {
        RetainPtr<CPDF_Object> object = ToObject(document, item, depth + 1);
        if (!object)
          return nullptr;
        dict->SetFor(key.c_str(), std::move(object));
      }
      return dict;
    }
  }
  return nullptr;
}

}  // namespace

Value AnnotationEntries(FPDF_ANNOTATION annot) {
  Value value;
  value.kind = Value::Kind::kDict;
  RetainPtr<CPDF_Dictionary> dict = AnnotationDictionary(annot);
  if (!dict)
    return value;
  CPDF_DictionaryLocker locker(dict);
  for (const auto& [key, item] : locker) {
    if (key == "AP" || key == "P")
      continue;
    std::vector<uint32_t> followed = {dict->GetObjNum()};
    value.entries.emplace_back(key.c_str(), ToValue(item.Get(), 1, followed));
  }
  return value;
}

int AnnotationObjectNumber(FPDF_ANNOTATION annot) {
  RetainPtr<CPDF_Dictionary> dict = AnnotationDictionary(annot);
  return dict ? static_cast<int>(dict->GetObjNum()) : 0;
}

std::string FormDefaultAppearance(FPDF_DOCUMENT document) {
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(document);
  const CPDF_Dictionary* root = pdf ? pdf->GetRoot() : nullptr;
  RetainPtr<const CPDF_Dictionary> form =
      root ? root->GetDictFor("AcroForm") : nullptr;
  return form ? std::string(form->GetByteStringFor("DA").c_str())
              : std::string();
}

bool SetAnnotationEntry(FPDF_DOCUMENT document,
                        FPDF_ANNOTATION annot,
                        const std::string& key,
                        const Value& value) {
  RetainPtr<CPDF_Dictionary> dict = AnnotationDictionary(annot);
  if (!dict || key.empty())
    return false;
  if (value.kind == Value::Kind::kNull) {
    dict->RemoveFor(key.c_str());
    return true;
  }
  RetainPtr<CPDF_Object> object =
      ToObject(CPDFDocumentFromFPDFDocument(document), value, 1);
  if (!object)
    return false;
  dict->SetFor(key.c_str(), std::move(object));
  return true;
}

}  // namespace zebkit::pdf
