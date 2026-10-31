// XFDF and FDF, in and out, over <zebkit/pdf.h>'s own calls: annotations as it
// describes and takes them. The XML is read by the engine's libxml without the
// network, without entities and refusing a DTD; FDF by PDFium's parser.

#ifdef UNSAFE_BUFFERS_BUILD
// The C API takes and returns C strings and arrays.
#pragma allow_unsafe_buffers
#endif

#include <stdlib.h>
#include <string.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/escape.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "third_party/libxml/src/include/libxml/parser.h"
#include "third_party/libxml/src/include/libxml/tree.h"
#include "zebkit/engine/include/zebkit/pdf.h"
#include "zebkit/engine/include/zebkit/services.h"
#include "zebkit/engine/pdf_appearance.h"
#include "zebkit/engine/pdf_document.h"
#include "zebkit/engine/pdf_hooks.h"

namespace {

constexpr size_t kMaxFile = 64u << 20;
constexpr size_t kMaxAnnotations = 100000;

// XFDF's element for each type this library makes.
constexpr std::pair<const char*, const char*> kElements[] = {
    {"text", "text"},         {"freeText", "freetext"}, {"line", "line"},
    {"square", "square"},     {"circle", "circle"},     {"polygon", "polygon"},
    {"polyline", "polyline"}, {"highlight", "highlight"}, {"underline", "underline"},
    {"squiggly", "squiggly"}, {"strikeOut", "strikeout"}, {"stamp", "stamp"},
    {"caret", "caret"},       {"ink", "ink"},           {"fileAttachment", "fileattachment"},
    {"redact", "redact"},
};

constexpr std::pair<const char*, int> kFlags[] = {
    {"invisible", 1},  {"hidden", 2},      {"print", 4},        {"nozoom", 8},          {"norotate", 16},
    {"noview", 32},    {"readonly", 64},   {"locked", 128},     {"togglenoview", 256},  {"lockedcontents", 512},
};

std::string Xml(const std::string& text) {
  std::string out;
  for (unsigned char c : text) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\n': out += "&#10;"; break;
      case '\r': out += "&#13;"; break;
      case '\t': out += "&#9;"; break;
      default:
        // XML 1.0 has no other control characters at all.
        if (c >= 0x20)
          out.push_back(static_cast<char>(c));
    }
  }
  return out;
}

std::string NumberText(double value) {
  return base::NumberToString(value);
}

std::string Numbers(const base::ListValue& list, const char* separator = ",") {
  std::string out;
  for (const base::Value& item : list) {
    if (!out.empty())
      out += separator;
    if (item.is_int() || item.is_double())
      out += NumberText(item.GetDouble());
  }
  return out;
}

std::string ColorText(const base::ListValue& color) {
  if (color.size() != 3)
    return std::string();
  return base::StringPrintf("#%02X%02X%02X", color[0].GetInt(), color[1].GetInt(), color[2].GetInt());
}

std::string PointsText(const base::ListValue& points) {
  std::string out;
  for (const base::Value& point : points) {
    if (!point.is_list())
      continue;
    if (!out.empty())
      out += ";";
    out += Numbers(point.GetList());
  }
  return out;
}

// The element of @text, which is XML, to write inside another document: not
// its "<?xml ?>", as /RC usually starts, which is an error anywhere else. ""
// when it is not XML, or has a DTD.
std::string RootElementOf(const std::string& text) {
  xmlDocPtr doc = xmlReadMemory(text.data(), static_cast<int>(text.size()), nullptr, "UTF-8",
                                XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_NO_XXE);
  if (!doc)
    return std::string();
  std::string out;
  xmlNode* root = xmlDocGetRootElement(doc);
  if (root && !doc->intSubset && !doc->extSubset) {
    if (xmlBufferPtr buffer = xmlBufferCreate()) {
      xmlNodeDump(buffer, doc, root, 0, 0);
      out = reinterpret_cast<const char*>(xmlBufferContent(buffer));
      xmlBufferFree(buffer);
    }
  }
  xmlFreeDoc(doc);
  return out;
}

// A file's numbers mean nothing in this document: what one refers to is not here.
void DropReferences(base::Value& value) {
  if (value.is_list()) {
    for (base::Value& item : value.GetList())
      DropReferences(item);
  } else if (value.is_dict()) {
    base::DictValue& dict = value.GetDict();
    if (dict.size() == 1 && dict.FindInt("ref")) {
      value = base::Value();
      return;
    }
    for (auto [key, item] : dict)
      DropReferences(item);
  }
}

// What travels in entries of its own, standard or not: what the attributes do not say.
bool TravelsAsEntry(const std::string& key, const std::vector<std::string>& prefixes) {
  if (key == "Measure" || key == "IT" || key == "ZebkitStampText" || key == "BM" || key == "ZebkitBox")
    return true;
  for (const std::string& prefix : prefixes) {
    if (base::StartsWith(key, prefix))
      return true;
  }
  return false;
}

std::optional<base::Value> ReadAnnotations(zebkit_pdf* pdf, int page, char** error) {
  char* json = zebkit_pdf_annotations(pdf, page, error);
  if (!json)
    return std::nullopt;
  std::optional<base::Value> parsed = base::JSONReader::Read(json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  free(json);
  if (!parsed || !parsed->is_list()) {
    if (error)
      *error = strdup("the annotations cannot be read");
    return std::nullopt;
  }
  return parsed;
}

// The annotations to write: by page, with names for those replied to.
struct Written {
  int page;
  base::DictValue annotation;
  std::string name;
};

const char* ElementOf(const std::string& type) {
  for (const auto& entry : kElements) {
    if (type == entry.first)
      return entry.second;
  }
  return nullptr;
}

bool Gather(zebkit_pdf* pdf, int page, int index, std::vector<Written>* out, char** error) {
  const int pages = zebkit_pdf_page_count(pdf);
  if (pages < 0 || page < -1 || page >= pages) {
    if (error)
      *error = strdup("no such page");
    return false;
  }
  if (index != -1 && page < 0) {
    if (error)
      *error = strdup("an annotation is of a page");
    return false;
  }
  for (int p = page < 0 ? 0 : page; p < (page < 0 ? pages : page + 1); ++p) {
    std::optional<base::Value> annotations = ReadAnnotations(pdf, p, error);
    if (!annotations)
      return false;
    int position = 0;
    for (base::Value& item : annotations->GetList()) {
      const int at = position++;
      if (index != -1 && at != index)
        continue;
      Written written;
      written.page = p;
      written.annotation = std::move(item.GetDict());
      const std::string* name = written.annotation.FindString("name");
      written.name = name && !name->empty() ? *name : base::StringPrintf("zk-%d-%d", p, at);
      out->push_back(std::move(written));
    }
  }
  if (index != -1 && out->empty()) {
    if (error)
      *error = strdup("no such annotation");
    return false;
  }
  return true;
}

std::string TypeOfWritten(const base::DictValue& annotation, std::string* subtype) {
  const std::string* type = annotation.FindString("type");
  std::string name = type ? *type : "unknown";
  if (const base::DictValue* entries = annotation.FindDict("entries")) {
    if (const std::string* s = entries->FindString("Subtype"))
      *subtype = s->substr(s->empty() ? 0 : 1);
  }
  return name;
}

std::string WriteXfdf(zebkit_pdf* pdf, const std::vector<Written>& all) {
  const std::vector<std::string> prefixes = zebkit::pdf::KindPrefixes(pdf);
  std::map<int, std::string> names;
  for (const Written& written : all) {
    if (std::optional<int> object = written.annotation.FindInt("object"))
      names[*object] = written.name;
  }
  std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                    "<xfdf xmlns=\"http://ns.adobe.com/xfdf/\" xml:space=\"preserve\">\n<annots>\n";
  for (const Written& written : all) {
    const base::DictValue& a = written.annotation;
    std::string subtype;
    const std::string type = TypeOfWritten(a, &subtype);
    const char* element = ElementOf(type);
    // Popups go inside what they belong to; links and widgets are not of XFDF.
    const bool custom = !element && a.contains("data");
    const bool foreign = !element && !custom;
    if (foreign)
      continue;
    const std::string tag = element ? element : "zebkit-annot";
    // An attribute said twice would not be XML: the later one stands.
    std::vector<std::pair<std::string, std::string>> attributes;
    auto attr = [&](const char* name, const std::string& value) {
      for (auto& attribute : attributes) {
        if (attribute.first == name) {
          attribute.second = value;
          return;
        }
      }
      attributes.emplace_back(name, value);
    };
    if (custom) {
      attr("kind", type);
      attr("subtype", subtype);
    }
    attr("page", base::NumberToString(written.page));
    if (const base::ListValue* rect = a.FindList("rect"))
      attr("rect", Numbers(*rect));
    attr("name", written.name);
    if (const std::string* author = a.FindString("author"))
      attr("title", *author);
    if (const std::string* subject = a.FindString("subject"))
      attr("subject", *subject);
    if (const std::string* date = a.FindString("modified"))
      attr("date", *date);
    if (const std::string* date = a.FindString("created"))
      attr("creationdate", *date);
    if (const std::optional<int> flags = a.FindInt("flags")) {
      std::string list;
      for (const auto& flag : kFlags) {
        if (*flags & flag.second)
          list += (list.empty() ? "" : ",") + std::string(flag.first);
      }
      attr("flags", list);
    }
    if (const base::ListValue* color = a.FindList("color"))
      attr("color", ColorText(*color));
    if (const base::ListValue* color = a.FindList("interiorColor"))
      attr("interior-color", ColorText(*color));
    if (const std::optional<double> opacity = a.FindDouble("opacity"))
      attr("opacity", NumberText(*opacity));
    if (const std::optional<double> width = a.FindDouble("border"))
      attr("width", NumberText(*width));
    if (const base::DictValue* style = a.FindDict("borderStyle")) {
      if (const std::string* s = style->FindString("style"))
        attr("style", *s == "dashed" ? "dash" : *s);
      if (const std::optional<double> width = style->FindDouble("width"))
        attr("width", NumberText(*width));
      if (const base::ListValue* dash = style->FindList("dash"))
        attr("dashes", Numbers(*dash));
    }
    if (const base::DictValue* effect = a.FindDict("borderEffect")) {
      if (const std::string* s = effect->FindString("style"); s && *s == "cloudy") {
        attr("style", "cloudy");
        attr("intensity", NumberText(effect->FindDouble("intensity").value_or(1)));
      }
    }
    if (const std::string* icon = a.FindString("icon"))
      attr("icon", *icon);
    if (const std::string* stamp = a.FindString("stamp"))
      attr("icon", *stamp);
    if (const std::string* symbol = a.FindString("caretSymbol"))
      attr("symbol", *symbol == "paragraph" ? "paragraph" : "none");
    if (const std::optional<double> rotation = a.FindDouble("rotation"))
      attr("rotation", NumberText(*rotation));
    if (const base::ListValue* quads = a.FindList("quads")) {
      std::string coords;
      for (const base::Value& quad : *quads) {
        if (quad.is_list())
          coords += (coords.empty() ? "" : ",") + Numbers(quad.GetList());
      }
      attr("coords", coords);
    }
    if (const base::ListValue* vertices = a.FindList("vertices"))
      attr("vertices", PointsText(*vertices));
    if (const base::ListValue* line = a.FindList("line"); line && line->size() == 4) {
      attr("start", NumberText((*line)[0].GetDouble()) + "," + NumberText((*line)[1].GetDouble()));
      attr("end", NumberText((*line)[2].GetDouble()) + "," + NumberText((*line)[3].GetDouble()));
    }
    if (const base::ListValue* endings = a.FindList("lineEndings"); endings && endings->size() == 2) {
      attr("head", (*endings)[0].GetString());
      attr("tail", (*endings)[1].GetString());
    }
    if (const base::DictValue* leader = a.FindDict("leader")) {
      attr("leaderLength", NumberText(leader->FindDouble("length").value_or(0)));
      attr("leaderExtend", NumberText(leader->FindDouble("extension").value_or(0)));
      attr("leaderOffset", NumberText(leader->FindDouble("offset").value_or(0)));
    }
    if (const base::DictValue* caption = a.FindDict("caption")) {
      attr("caption", caption->FindBool("show").value_or(false) ? "yes" : "no");
      if (const std::string* position = caption->FindString("position"))
        attr("caption-style", *position == "top" ? "Top" : "Inline");
      // What the file has in /CO, numbers or not.
      if (const base::ListValue* offset = caption->FindList("offset");
          offset && offset->size() == 2 && ((*offset)[0].is_int() || (*offset)[0].is_double()) &&
          ((*offset)[1].is_int() || (*offset)[1].is_double())) {
        attr("caption-offset-h", NumberText((*offset)[0].GetDouble()));
        attr("caption-offset-v", NumberText((*offset)[1].GetDouble()));
      }
    }
    if (const std::string* align = a.FindString("align"))
      attr("justification", *align == "center" ? "centered" : *align);
    if (const base::ListValue* callout = a.FindList("callout"))
      attr("callout", Numbers(*callout));
    if (const std::string* ending = a.FindString("calloutEnding"))
      attr("head", *ending);
    if (const base::DictValue* entries = a.FindDict("entries")) {
      if (const base::DictValue* irt = entries->FindDict("IRT")) {
        if (const std::optional<int> object = irt->FindInt("ref")) {
          auto name = names.find(*object);
          if (name != names.end())
            attr("inreplyto", name->second);
        }
      }
    }
    if (const std::string* reply = a.FindString("replyType"))
      attr("replyType", *reply);
    if (const std::string* state = a.FindString("state"))
      attr("state", *state);
    if (const std::string* model = a.FindString("stateModel"))
      attr("statemodel", *model == "review" ? "Review" : "Marked");

    std::string children;
    if (const std::string* contents = a.FindString("contents"))
      children += "<contents>" + Xml(*contents) + "</contents>\n";
    if (const std::string* rich = a.FindString("richText")) {
      if (const std::string element = RootElementOf(*rich); !element.empty())
        children += "<contents-richtext>" + element + "</contents-richtext>\n";
    }
    if (const base::DictValue* entries = a.FindDict("entries")) {
      // In "entries" a string that starts with a slash has another before it.
      if (const std::string* da = entries->FindString("DA"))
        children += "<defaultappearance>" + Xml(base::StartsWith(*da, "//") ? da->substr(1) : *da) + "</defaultappearance>\n";
      if (const std::string* ds = entries->FindString("DS"))
        children += "<defaultstyle>" + Xml(*ds) + "</defaultstyle>\n";
      base::DictValue carried;
      for (const auto [key, value] : *entries) {
        if (TravelsAsEntry(key, prefixes))
          carried.Set(key, value.Clone());
      }
      if (!carried.empty())
        children += "<zebkit-entries>" + Xml(base::WriteJson(carried).value_or("{}")) + "</zebkit-entries>\n";
    }
    if (const base::ListValue* ink = a.FindList("ink")) {
      children += "<inklist>\n";
      for (const base::Value& stroke : *ink) {
        if (stroke.is_list())
          children += "<gesture>" + PointsText(stroke.GetList()) + "</gesture>\n";
      }
      children += "</inklist>\n";
    }
    // Its popup, which is read as a part of it.
    if (const std::optional<int> popup = a.FindInt("popup")) {
      for (const Written& other : all) {
        if (other.page != written.page || other.annotation.FindInt("index") != popup)
          continue;
        if (const base::ListValue* rect = other.annotation.FindList("rect")) {
          const base::DictValue* entries = other.annotation.FindDict("entries");
          const bool open = entries && entries->FindBool("Open").value_or(false);
          children += "<popup rect=\"" + Xml(Numbers(*rect)) + "\" open=\"" + (open ? "yes" : "no") + "\"/>\n";
        }
      }
    }
    std::string attrs;
    for (const auto& [name, value] : attributes)
      attrs += " " + name + "=\"" + Xml(value) + "\"";
    out += "<" + tag + attrs + ">\n" + children + "</" + tag + ">\n";
  }
  out += "</annots>\n</xfdf>\n";
  return out;
}

// PDF's syntax for a value of "entries".
void WriteFdfValue(const base::Value& value, const std::map<int, int>& objects, std::string& out, int depth) {
  if (depth > 16) {
    out += "null";
    return;
  }
  if (value.is_none()) {
    out += "null";
  } else if (value.is_bool()) {
    out += value.GetBool() ? "true" : "false";
  } else if (value.is_int() || value.is_double()) {
    out += NumberText(value.GetDouble());
  } else if (value.is_string()) {
    const std::string& text = value.GetString();
    if (base::StartsWith(text, "/") && !base::StartsWith(text, "//")) {
      out += "/";
      for (unsigned char c : text.substr(1)) {
        if (c >= 33 && c <= 126 && !strchr("#/()<>[]{}%", c))
          out.push_back(static_cast<char>(c));
        else
          out += base::StringPrintf("#%02X", c);
      }
    } else {
      const std::u16string wide =
          base::UTF8ToUTF16(base::StartsWith(text, "//") ? text.substr(1) : text);
      out += "<FEFF";
      for (char16_t c : wide)
        out += base::StringPrintf("%04X", static_cast<unsigned>(c));
      out += ">";
    }
  } else if (value.is_list()) {
    out += "[";
    for (const base::Value& item : value.GetList()) {
      WriteFdfValue(item, objects, out, depth + 1);
      out += " ";
    }
    out += "]";
  } else if (value.is_dict()) {
    const base::DictValue& dict = value.GetDict();
    if (std::optional<int> ref = dict.FindInt("ref"); ref && dict.size() == 1) {
      auto number = objects.find(*ref);
      out += number != objects.end() ? base::StringPrintf("%d 0 R", number->second) : "null";
      return;
    }
    out += "<<";
    for (const auto [key, item] : dict) {
      out += "/";
      for (unsigned char c : key) {
        if (c >= 33 && c <= 126 && !strchr("#/()<>[]{}%", c))
          out.push_back(static_cast<char>(c));
        else
          out += base::StringPrintf("#%02X", c);
      }
      out += " ";
      WriteFdfValue(item, objects, out, depth + 1);
      out += " ";
    }
    out += ">>";
  }
}

std::string WriteFdf(const std::vector<Written>& all) {
  std::map<int, int> objects;
  int next = 2;
  std::vector<const Written*> kept;
  for (const Written& written : all) {
    std::string subtype;
    const std::string type = TypeOfWritten(written.annotation, &subtype);
    if (!ElementOf(type) && !written.annotation.contains("data"))
      continue;
    kept.push_back(&written);
    if (std::optional<int> object = written.annotation.FindInt("object"))
      objects[*object] = next;
    ++next;
  }
  std::string out = "%FDF-1.2\n1 0 obj\n<</FDF <</Annots [";
  for (size_t i = 0; i < kept.size(); ++i)
    out += base::StringPrintf("%d 0 R ", static_cast<int>(i) + 2);
  out += "]>>>>\nendobj\n";
  int number = 2;
  for (const Written* written : kept) {
    base::DictValue entries;
    if (const base::DictValue* given = written->annotation.FindDict("entries"))
      entries = given->Clone();
    for (const char* key : {"Popup", "Parent", "P", "AP", "A", "AA", "ZebkitOwnAppearance", "PDFIUM_HasGeneratedAP", "FS"})
      entries.Remove(key);
    entries.Set("NM", written->name);
    entries.Set("Page", written->page);
    out += base::StringPrintf("%d 0 obj\n", number++);
    WriteFdfValue(base::Value(std::move(entries)), objects, out, 0);
    out += "\nendobj\n";
  }
  out += "trailer\n<</Root 1 0 R>>\n%%EOF\n";
  return out;
}

// ---- In ----

struct Pending {
  int page = 0;
  base::DictValue annotation;
  std::string name, reply_to;
  int object = 0, reply_object = 0;  // an FDF file's numbers
};

std::string Prop(xmlNode* node, const char* name) {
  xmlChar* value = xmlGetProp(node, reinterpret_cast<const xmlChar*>(name));
  if (!value)
    return std::string();
  std::string out(reinterpret_cast<const char*>(value));
  xmlFree(value);
  return out;
}

bool HasProp(xmlNode* node, const char* name) {
  return xmlHasProp(node, reinterpret_cast<const xmlChar*>(name)) != nullptr;
}

std::string TextOf(xmlNode* node) {
  xmlChar* value = xmlNodeGetContent(node);
  if (!value)
    return std::string();
  std::string out(reinterpret_cast<const char*>(value));
  xmlFree(value);
  return out;
}

std::optional<base::ListValue> NumberList(const std::string& text, size_t limit = 8u << 20) {
  base::ListValue list;
  for (const std::string_view part :
       base::SplitStringPiece(text, ",; \t\r\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    double value = 0;
    if (!base::StringToDouble(part, &value) || list.size() >= limit)
      return std::nullopt;
    list.Append(value);
  }
  return list;
}

std::optional<base::ListValue> PointList(const std::string& text) {
  std::optional<base::ListValue> flat = NumberList(text);
  if (!flat || flat->size() % 2)
    return std::nullopt;
  base::ListValue points;
  for (size_t i = 0; i + 1 < flat->size(); i += 2) {
    base::ListValue point;
    point.Append((*flat)[i].GetDouble());
    point.Append((*flat)[i + 1].GetDouble());
    points.Append(std::move(point));
  }
  return points;
}

std::optional<base::ListValue> ColorList(const std::string& text) {
  if (text.size() != 7 || text[0] != '#')
    return std::nullopt;
  uint32_t rgb = 0;
  if (!base::HexStringToUInt(text.substr(1), &rgb))
    return std::nullopt;
  base::ListValue color;
  color.Append(static_cast<int>((rgb >> 16) & 255));
  color.Append(static_cast<int>((rgb >> 8) & 255));
  color.Append(static_cast<int>(rgb & 255));
  return color;
}

// Attributes as "attr": what a number needs. "" or what is wrong.
std::string ReadXfdfAnnotation(zebkit_pdf* pdf, xmlNode* node, int pages, Pending* out) {
  const std::string tag = reinterpret_cast<const char*>(node->name);
  std::string type;
  for (const auto& entry : kElements) {
    if (tag == entry.second)
      type = entry.first;
  }
  if (tag == "zebkit-annot") {
    type = Prop(node, "kind");
    if (!zebkit::pdf::FindKind(pdf, type))
      return "no kind " + type + " is registered";
  }
  if (type.empty())
    return "an element that is no annotation: " + tag;
  base::DictValue& a = out->annotation;
  a.Set("type", type);
  int page = -1;
  if (!base::StringToInt(Prop(node, "page"), &page) || page < 0 || page >= pages)
    return "the page of an annotation is outside the document";
  out->page = page;
  std::optional<base::ListValue> rect = NumberList(Prop(node, "rect"));
  if (!rect || rect->size() != 4)
    return "the rect of an annotation is not l,b,r,t";
  a.Set("rect", std::move(*rect));
  out->name = Prop(node, "name");
  if (!out->name.empty())
    a.Set("name", out->name);
  out->reply_to = Prop(node, "inreplyto");
  if (HasProp(node, "title"))
    a.Set("author", Prop(node, "title"));
  if (HasProp(node, "subject"))
    a.Set("subject", Prop(node, "subject"));
  if (HasProp(node, "date"))
    a.Set("modified", Prop(node, "date"));
  if (HasProp(node, "creationdate"))
    a.Set("created", Prop(node, "creationdate"));
  if (HasProp(node, "flags")) {
    int flags = 0;
    for (const std::string& name :
         base::SplitString(Prop(node, "flags"), ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
      bool known = false;
      for (const auto& flag : kFlags) {
        if (name == flag.first) {
          flags |= flag.second;
          known = true;
        }
      }
      if (!known)
        return "no flag " + name;
    }
    a.Set("flags", flags);
  }
  for (const auto& [attr, key] : {std::pair{"color", "color"}, std::pair{"interior-color", "interiorColor"}}) {
    if (HasProp(node, attr)) {
      std::optional<base::ListValue> color = ColorList(Prop(node, attr));
      if (!color)
        return std::string(attr) + " is not #RRGGBB";
      a.Set(key, std::move(*color));
    }
  }
  auto number = [&](const char* attr, double* value) {
    return base::StringToDouble(Prop(node, attr), value);
  };
  double value = 0;
  if (HasProp(node, "opacity")) {
    if (!number("opacity", &value))
      return "opacity is not a number";
    a.Set("opacity", value);
  }
  const std::string style = Prop(node, "style");
  if (HasProp(node, "width") || HasProp(node, "dashes") || style == "dash" || style == "solid") {
    base::DictValue border;
    if (HasProp(node, "width")) {
      if (!number("width", &value))
        return "width is not a number";
      border.Set("width", value);
    }
    if (style == "dash")
      border.Set("style", "dashed");
    else if (style == "solid")
      border.Set("style", "solid");
    if (HasProp(node, "dashes")) {
      std::optional<base::ListValue> dash = NumberList(Prop(node, "dashes"), 32);
      if (!dash)
        return "dashes are not numbers";
      border.Set("dash", std::move(*dash));
    }
    a.Set("borderStyle", std::move(border));
  }
  if (style == "cloudy") {
    base::DictValue effect;
    effect.Set("style", "cloudy");
    if (HasProp(node, "intensity")) {
      if (!number("intensity", &value))
        return "intensity is not a number";
      effect.Set("intensity", value);
    }
    a.Set("borderEffect", std::move(effect));
  }
  if (HasProp(node, "icon"))
    a.Set(type == "stamp" ? "stamp" : "icon", Prop(node, "icon"));
  if (HasProp(node, "symbol"))
    a.Set("caretSymbol", Prop(node, "symbol") == "paragraph" ? "paragraph" : "none");
  if (HasProp(node, "rotation")) {
    if (!number("rotation", &value))
      return "rotation is not a number";
    a.Set("rotation", value);
  }
  if (HasProp(node, "coords")) {
    std::optional<base::ListValue> flat = NumberList(Prop(node, "coords"));
    if (!flat || flat->empty() || flat->size() % 8)
      return "coords are not eight numbers each";
    base::ListValue quads;
    for (size_t i = 0; i < flat->size(); i += 8) {
      base::ListValue quad;
      for (size_t k = 0; k < 8; ++k)
        quad.Append((*flat)[i + k].GetDouble());
      quads.Append(std::move(quad));
    }
    a.Set("quads", std::move(quads));
  }
  if (HasProp(node, "vertices")) {
    std::optional<base::ListValue> points = PointList(Prop(node, "vertices"));
    if (!points)
      return "vertices are not x,y;x,y";
    a.Set("vertices", std::move(*points));
  }
  if (HasProp(node, "start") || HasProp(node, "end")) {
    std::optional<base::ListValue> start = NumberList(Prop(node, "start"));
    std::optional<base::ListValue> end = NumberList(Prop(node, "end"));
    if (!start || !end || start->size() != 2 || end->size() != 2)
      return "start and end are x,y";
    start->Append((*end)[0].GetDouble());
    start->Append((*end)[1].GetDouble());
    a.Set("line", std::move(*start));
  }
  if (type == "line" || type == "polyline") {
    if (HasProp(node, "head") || HasProp(node, "tail")) {
      base::ListValue endings;
      endings.Append(HasProp(node, "head") ? Prop(node, "head") : "none");
      endings.Append(HasProp(node, "tail") ? Prop(node, "tail") : "none");
      a.Set("lineEndings", std::move(endings));
    }
  } else if (type == "freeText" && HasProp(node, "head")) {
    a.Set("calloutEnding", Prop(node, "head"));
  }
  if (HasProp(node, "leaderLength") || HasProp(node, "leaderExtend") || HasProp(node, "leaderOffset")) {
    base::DictValue leader;
    for (const auto& [attr, key] : {std::pair{"leaderLength", "length"}, std::pair{"leaderExtend", "extension"},
                                    std::pair{"leaderOffset", "offset"}}) {
      if (HasProp(node, attr)) {
        if (!number(attr, &value))
          return std::string(attr) + " is not a number";
        leader.Set(key, value);
      }
    }
    a.Set("leader", std::move(leader));
  }
  if (HasProp(node, "caption")) {
    base::DictValue caption;
    caption.Set("show", Prop(node, "caption") == "yes");
    if (HasProp(node, "caption-style"))
      caption.Set("position", Prop(node, "caption-style") == "Top" ? "top" : "inline");
    double h = 0, v = 0;
    if (number("caption-offset-h", &h) && number("caption-offset-v", &v)) {
      base::ListValue offset;
      offset.Append(h);
      offset.Append(v);
      caption.Set("offset", std::move(offset));
    }
    a.Set("caption", std::move(caption));
  }
  if (HasProp(node, "justification")) {
    const std::string align = Prop(node, "justification");
    a.Set("align", align == "centered" ? "center" : align);
  }
  if (HasProp(node, "callout")) {
    std::optional<base::ListValue> callout = NumberList(Prop(node, "callout"));
    if (!callout)
      return "callout is not numbers";
    a.Set("callout", std::move(*callout));
  }
  if (HasProp(node, "replyType"))
    a.Set("replyType", Prop(node, "replyType"));
  if (HasProp(node, "state"))
    a.Set("state", base::ToLowerASCII(Prop(node, "state")));
  if (HasProp(node, "statemodel"))
    a.Set("stateModel", Prop(node, "statemodel") == "Review" ? "review" : "marking");

  base::DictValue entries;
  for (xmlNode* child = node->children; child; child = child->next) {
    if (child->type != XML_ELEMENT_NODE)
      continue;
    const std::string name = reinterpret_cast<const char*>(child->name);
    if (name == "contents") {
      a.Set("contents", TextOf(child));
    } else if (name == "contents-richtext") {
      // The XHTML inside it, as text again.
      xmlBufferPtr buffer = xmlBufferCreate();
      std::string rich;
      for (xmlNode* part = child->children; part && buffer; part = part->next) {
        xmlNodeDump(buffer, child->doc, part, 0, 0);
      }
      if (buffer) {
        rich = reinterpret_cast<const char*>(xmlBufferContent(buffer));
        xmlBufferFree(buffer);
      }
      if (!rich.empty())
        a.Set("richText", rich);
    } else if (name == "defaultappearance") {
      const std::string da = TextOf(child);
      entries.Set("DA", base::StartsWith(da, "/") ? "/" + da : da);
    } else if (name == "defaultstyle") {
      a.Set("defaultStyle", TextOf(child));
    } else if (name == "zebkit-entries") {
      std::optional<base::Value> parsed =
          base::JSONReader::Read(TextOf(child), base::JSON_PARSE_CHROMIUM_EXTENSIONS);
      if (!parsed || !parsed->is_dict())
        return "zebkit-entries is not a JSON object";
      // As much as WriteXfdf() puts there, and no links to what is not here:
      // the same a file of FDF is held to.
      const std::vector<std::string> prefixes = zebkit::pdf::KindPrefixes(pdf);
      for (auto [key, item] : parsed->GetDict()) {
        if (!TravelsAsEntry(key, prefixes))
          return "zebkit-entries may not have " + key;
        DropReferences(item);
        entries.Set(key, item.Clone());
      }
    } else if (name == "inklist") {
      base::ListValue strokes;
      for (xmlNode* gesture = child->children; gesture; gesture = gesture->next) {
        if (gesture->type != XML_ELEMENT_NODE)
          continue;
        std::optional<base::ListValue> points = PointList(TextOf(gesture));
        if (!points || points->empty())
          return "a gesture is not x,y;x,y";
        strokes.Append(std::move(*points));
      }
      a.Set("ink", std::move(strokes));
    } else if (name == "popup") {
      std::optional<base::ListValue> popup_rect = NumberList(Prop(child, "rect"));
      if (!popup_rect || popup_rect->size() != 4)
        return "a popup's rect is not l,b,r,t";
      base::DictValue popup;
      popup.Set("rect", std::move(*popup_rect));
      popup.Set("open", Prop(child, "open") == "yes");
      a.Set("popup", std::move(popup));
    }
  }
  if (!entries.empty())
    a.Set("entries", std::move(entries));
  // A kind's data, from what its prefix keeps.
  if (tag == "zebkit-annot") {
    std::shared_ptr<zebkit::pdf::Hook> kind = zebkit::pdf::FindKind(pdf, type);
    if (const base::DictValue* given = a.FindDict("entries")) {
      if (const base::Value* data = given->Find(kind->prefix + "Data")) {
        base::Value copy = data->Clone();
        a.FindDict("entries")->Remove(kind->prefix + "Data");
        a.Set("data", std::move(copy));
      }
    }
  }
  // A turned box: the rectangle of what was unturned.
  if (const base::DictValue* given = a.FindDict("entries")) {
    if (const base::ListValue* box = given->FindList("ZebkitBox")) {
      base::Value copy(box->Clone());
      a.FindDict("entries")->Remove("ZebkitBox");
      a.Set("rect", std::move(copy));
    }
  }
  return std::string();
}

std::string ReadXfdf(zebkit_pdf* pdf, const void* data, size_t size, std::vector<Pending>* out) {
  xmlDocPtr doc = xmlReadMemory(static_cast<const char*>(data), static_cast<int>(size), nullptr, "UTF-8",
                                XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_NO_XXE);
  if (!doc)
    return "not XML";
  std::string problem;
  xmlNode* root = xmlDocGetRootElement(doc);
  if (doc->intSubset || doc->extSubset) {
    problem = "XFDF with a DTD is not read";
  } else if (!root || strcmp(reinterpret_cast<const char*>(root->name), "xfdf")) {
    problem = "not XFDF";
  } else {
    const int pages = zebkit_pdf_page_count(pdf);
    for (xmlNode* section = root->children; section && problem.empty(); section = section->next) {
      if (section->type != XML_ELEMENT_NODE || strcmp(reinterpret_cast<const char*>(section->name), "annots"))
        continue;
      for (xmlNode* node = section->children; node && problem.empty(); node = node->next) {
        if (node->type != XML_ELEMENT_NODE)
          continue;
        if (out->size() >= kMaxAnnotations) {
          problem = "over 100000 annotations";
          break;
        }
        Pending pending;
        problem = ReadXfdfAnnotation(pdf, node, pages, &pending);
        if (problem.empty())
          out->push_back(std::move(pending));
      }
    }
  }
  xmlFreeDoc(doc);
  return problem;
}

std::string ReadFdf(zebkit_pdf* pdf, const void* data, size_t size, std::vector<Pending>* out) {
  std::vector<zebkit::pdf::FdfAnnotation> annotations;
  if (!zebkit::pdf::ParseFdf(static_cast<const uint8_t*>(data), size, &annotations))
    return "not FDF";
  const int pages = zebkit_pdf_page_count(pdf);
  for (const zebkit::pdf::FdfAnnotation& annotation : annotations) {
    base::Value json = zebkit::pdf::EntriesToJson(annotation.entries);
    if (!json.is_dict())
      return "an annotation of the file is not a dictionary";
    base::DictValue& entries = json.GetDict();
    const std::string* subtype = entries.FindString("Subtype");
    if (!subtype || subtype->size() < 2)
      return "an annotation of the file has no subtype";
    std::string type;
    const std::string lower = base::ToLowerASCII(subtype->substr(1));
    for (const auto& entry : kElements) {
      if (lower == entry.second)
        type = entry.first;
    }
    if (type.empty() && zebkit::pdf::FindKind(pdf, subtype->substr(1)))
      type = subtype->substr(1);
    if (type.empty())
      return "an annotation of the file is of type " + subtype->substr(1);
    const std::optional<int> page = entries.FindInt("Page");
    const base::ListValue* rect = entries.FindList("Rect");
    if (!page || *page < 0 || *page >= pages)
      return "the page of an annotation is outside the document";
    if (!rect || rect->size() != 4)
      return "the rect of an annotation is not four numbers";
    Pending pending;
    pending.page = *page;
    pending.object = annotation.object;
    if (const base::DictValue* irt = entries.FindDict("IRT"))
      pending.reply_object = irt->FindInt("ref").value_or(0);
    if (const std::string* name = entries.FindString("NM"))
      pending.name = *name;
    pending.annotation.Set("type", type);
    pending.annotation.Set("rect", rect->Clone());
    // Actions are not taken from a file of comments; nor links to what is not here.
    for (const char* key : {"Type", "Subtype", "Rect", "Page", "Popup", "Parent", "P", "AP", "A", "AA", "IRT",
                            "ZebkitOwnAppearance", "PDFIUM_HasGeneratedAP", "FS"}) {
      entries.Remove(key);
    }
    for (auto [key, item] : entries)
      DropReferences(item);
    // A kind's data, under its name in JSON.
    if (std::shared_ptr<zebkit::pdf::Hook> kind = zebkit::pdf::FindKind(pdf, type)) {
      if (const base::Value* data_value = entries.Find(kind->prefix + "Data")) {
        pending.annotation.Set("data", data_value->Clone());
        entries.Remove(kind->prefix + "Data");
      }
    }
    if (const base::ListValue* box = entries.FindList("ZebkitBox")) {
      pending.annotation.Set("rect", box->Clone());
      entries.Remove("ZebkitBox");
    }
    pending.annotation.Set("entries", std::move(entries));
    out->push_back(std::move(pending));
  }
  return std::string();
}

}  // namespace

extern "C" {

uint8_t* zebkit_pdf_annotations_export(zebkit_pdf* pdf,
                                       zebkit_pdf_interchange format,
                                       int page,
                                       int index,
                                       size_t* size,
                                       char** error) {
  if (!pdf) {
    if (error)
      *error = strdup("no document");
    return nullptr;
  }
  std::vector<Written> all;
  // A whole page or document for what an annotation refers to; one is one.
  if (!Gather(pdf, page, index, &all, error))
    return nullptr;
  const std::string out = format == ZEBKIT_PDF_FDF ? WriteFdf(all) : WriteXfdf(pdf, all);
  uint8_t* bytes = static_cast<uint8_t*>(malloc(out.size() ? out.size() : 1));
  if (!bytes) {
    if (error)
      *error = strdup("not enough memory");
    return nullptr;
  }
  memcpy(bytes, out.data(), out.size());
  if (size)
    *size = out.size();
  return bytes;
}

int zebkit_pdf_annotations_import(zebkit_pdf* pdf,
                                  zebkit_pdf_interchange format,
                                  const void* data,
                                  size_t size,
                                  char** error) {
  auto fail = [&](const std::string& message) {
    if (error)
      *error = strdup(message.c_str());
    return -1;
  };
  if (!pdf || !data || !size || size > kMaxFile)
    return fail("no file, or one over 64 MiB");
  // Each annotation's hooks may close the document, which stays for all of this.
  zebkit::pdf::ApiScope scope(pdf);
  std::vector<Pending> pending;
  const std::string problem =
      format == ZEBKIT_PDF_FDF ? ReadFdf(pdf, data, size, &pending) : ReadXfdf(pdf, data, size, &pending);
  if (!problem.empty())
    return fail(problem);
  if (pending.size() > kMaxAnnotations)
    return fail("over 100000 annotations");

  // Who answers whom, by the file's names or numbers.
  std::map<std::string, size_t> by_name;
  std::map<int, size_t> by_object;
  for (size_t i = 0; i < pending.size(); ++i) {
    if (!pending[i].name.empty())
      by_name[pending[i].name] = i;
    if (pending[i].object)
      by_object[pending[i].object] = i;
  }
  std::vector<int> target(pending.size(), -1);
  for (size_t i = 0; i < pending.size(); ++i) {
    if (!pending[i].reply_to.empty()) {
      auto found = by_name.find(pending[i].reply_to);
      if (found != by_name.end())
        target[i] = static_cast<int>(found->second);
    } else if (pending[i].reply_object) {
      auto found = by_object.find(pending[i].reply_object);
      if (found != by_object.end())
        target[i] = static_cast<int>(found->second);
    }
  }
  // A cycle of replies in the file is an error of the file.
  // Each one once, whatever the length of the chains: a file's can be its whole.
  enum : uint8_t { kUnseen, kOnThisChain, kFine };
  std::vector<uint8_t> state(pending.size(), kUnseen);
  for (size_t i = 0; i < pending.size(); ++i) {
    int at = static_cast<int>(i);
    while (at >= 0 && state[at] == kUnseen) {
      state[at] = kOnThisChain;
      at = target[at];
    }
    if (at >= 0 && state[at] == kOnThisChain)
      return fail("the file's replies go round in a cycle");
    for (at = static_cast<int>(i); at >= 0 && state[at] == kOnThisChain; at = target[at])
      state[at] = kFine;
  }

  // Added one by one; undone if one of them cannot be.
  std::vector<std::pair<int, int>> added;
  auto undo = [&]() {
    for (auto it = added.rbegin(); it != added.rend(); ++it)
      zebkit_pdf_annotation_remove(pdf, it->first, it->second, nullptr);
  };
  for (const Pending& item : pending) {
    const std::string json = base::WriteJson(item.annotation).value_or("{}");
    const int at = zebkit_pdf_annotation_add(pdf, item.page, json.c_str(), error);
    if (at < 0) {
      undo();
      return -1;
    }
    added.emplace_back(item.page, at);
    if (pdf->closing)
      return fail("a hook closed the document");
  }
  // The replies: the objects there now.
  std::map<int, base::Value> read;
  for (size_t i = 0; i < pending.size(); ++i) {
    if (target[i] < 0)
      continue;
    const auto [page, at] = added[target[i]];
    if (!read.count(page)) {
      std::optional<base::Value> annotations = ReadAnnotations(pdf, page, error);
      if (!annotations) {
        undo();
        return -1;
      }
      read.emplace(page, std::move(*annotations));
    }
    const base::ListValue& list = read.at(page).GetList();
    const std::optional<int> object =
        static_cast<size_t>(at) < list.size() ? list[at].GetDict().FindInt("object") : std::nullopt;
    if (!object)
      continue;
    base::DictValue entries;
    entries.Set("IRT", base::DictValue().Set("ref", *object));
    base::DictValue changes;
    changes.Set("entries", std::move(entries));
    changes.Set("regenerate", false);
    const std::string json = base::WriteJson(changes).value_or("{}");
    if (!zebkit_pdf_annotation_update(pdf, added[i].first, added[i].second, json.c_str(), error)) {
      undo();
      return -1;
    }
    if (pdf->closing)
      return fail("a hook closed the document");
  }
  return static_cast<int>(pending.size());
}

}  // extern "C"
