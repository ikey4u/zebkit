// The registry of <zebkit/pdf.h>'s hooks, and running them: never with the
// lock around PDFium held.

#ifndef ZEBKIT_ENGINE_PDF_HOOKS_H_
#define ZEBKIT_ENGINE_PDF_HOOKS_H_

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "base/values.h"
#include "zebkit/engine/include/zebkit/pdf.h"
#include "zebkit/engine/pdf_internal.h"

namespace zebkit::pdf {

enum class HookType {
  kBefore,
  kAfter,
  kAppearance,
  kKind,
  kLayer,
  kOcr,
  kFont,
  kTrust,
  kAction,
  kInput,
};

struct Hook {
  ~Hook();
  int id = 0;
  HookType type = HookType::kBefore;
  zebkit_pdf* pdf = nullptr;
  std::string key;  // the event, type, tool or kind
  void* func = nullptr;
  void* data = nullptr;
  void (*destroy)(void*) = nullptr;
  std::atomic<bool> removed{false};
  // A kind's.
  std::string prefix, subtype;
  base::Value schema;
  char* (*check)(const char*, void*) = nullptr;
  bool (*hit_test)(zebkit_pdf*, int, int, const char*, double, double, void*) = nullptr;
};

using Hooks = std::vector<std::shared_ptr<Hook>>;

// A document's and all documents' hooks of @type for @key or "*", by age; ""
// for every key.
Hooks HooksFor(HookType type, zebkit_pdf* pdf, const std::string& key);
// Whether any is registered: what spares a document with no hooks the JSON.
bool AnyHooks();
bool HasEventHooks(zebkit_pdf* pdf, const char* event, bool before);

// Runs the "before" hooks of @event over @operation (which gets "event" and may
// be replaced). False, with *@error, when one refuses or closes the document.
bool RunBefore(zebkit_pdf* pdf, const char* event, base::DictValue& operation, char** error);
void RunAfter(zebkit_pdf* pdf, const char* event, base::DictValue operation);

// A document's hooks go with it.
void RemoveHooksOf(zebkit_pdf* pdf);

// The kind of @name, or that an annotation with these entries is of.
std::shared_ptr<Hook> FindKind(zebkit_pdf* pdf, const std::string& name);
std::shared_ptr<Hook> KindOf(zebkit_pdf* pdf, const std::string& subtype, const base::DictValue* entries);
// Every registered prefix for @pdf.
std::vector<std::string> KindPrefixes(zebkit_pdf* pdf);
// Whether @data is what @kind allows; "" or what is wrong.
std::string CheckKindData(const Hook& kind, const base::Value& data);

// A dictionary's plain values as "entries" has them (pdf.cc).
base::Value EntriesToJson(const Value& value);

}  // namespace zebkit::pdf

#endif  // ZEBKIT_ENGINE_PDF_HOOKS_H_
