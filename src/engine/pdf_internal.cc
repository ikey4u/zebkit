// PDFium's classes for <zebkit/pdf.h>; nothing of base here (pdf_internal.h).

#include "zebkit/engine/pdf_internal.h"

#include "core/fpdfapi/page/cpdf_annotcontext.h"
#include "core/fpdfapi/parser/cpdf_array.h"
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

}  // namespace zebkit::pdf
