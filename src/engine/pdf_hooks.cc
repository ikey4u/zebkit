// Hooks of <zebkit/pdf.h>: who is registered, and how they are run.

#ifdef UNSAFE_BUFFERS_BUILD
// The C API takes and returns C strings and arrays.
#pragma allow_unsafe_buffers
#endif

#include "zebkit/engine/pdf_hooks.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "zebkit/engine/pdf_document.h"

namespace zebkit::pdf {

namespace {

constexpr int kMaxHookDepth = 16;

struct Registry {
  base::Lock lock;
  Hooks hooks;
  int next_id = 1;
};

Registry& TheRegistry() {
  static base::NoDestructor<Registry> registry;
  return *registry;
}

std::atomic<int> g_hook_count{0};

int Register(HookType type,
             zebkit_pdf* pdf,
             const std::string& key,
             void* func,
             void* data,
             void (*destroy)(void*),
             std::shared_ptr<Hook> hook = nullptr) {
  if (!func && type != HookType::kKind)
    return -1;
  if (!hook)
    hook = std::make_shared<Hook>();
  hook->type = type;
  hook->pdf = pdf;
  hook->key = key;
  hook->func = func;
  hook->data = data;
  hook->destroy = destroy;
  Registry& registry = TheRegistry();
  base::AutoLock lock(registry.lock);
  hook->id = registry.next_id++;
  registry.hooks.push_back(hook);
  g_hook_count.fetch_add(1, std::memory_order_relaxed);
  return hook->id;
}

std::string Write(const base::DictValue& value) {
  return base::WriteJson(value).value_or("{}");
}

}  // namespace

Hook::~Hook() {
  if (destroy)
    destroy(data);
}

Hooks HooksFor(HookType type, zebkit_pdf* pdf, const std::string& key) {
  Hooks found;
  if (!AnyHooks())
    return found;
  Registry& registry = TheRegistry();
  base::AutoLock lock(registry.lock);
  for (const auto& hook : registry.hooks) {
    if (hook->type == type && (!hook->pdf || hook->pdf == pdf) &&
        (key.empty() || hook->key == key || hook->key == "*")) {
      found.push_back(hook);
    }
  }
  return found;
}

bool AnyHooks() {
  return g_hook_count.load(std::memory_order_relaxed) > 0;
}

bool HasEventHooks(zebkit_pdf* pdf, const char* event, bool before) {
  return AnyHooks() &&
         !HooksFor(before ? HookType::kBefore : HookType::kAfter, pdf, event).empty();
}

bool RunBefore(zebkit_pdf* pdf, const char* event, base::DictValue& operation, char** error) {
  const Hooks hooks = HooksFor(HookType::kBefore, pdf, event);
  if (hooks.empty())
    return true;
  operation.Set("event", event);
  if (pdf && pdf->hook_depth >= kMaxHookDepth) {
    if (error)
      *error = strdup("hooks are nested too deep");
    return false;
  }
  if (pdf)
    ++pdf->hook_depth;
  bool go_on = true;
  for (const auto& hook : hooks) {
    if (hook->removed.load())
      continue;
    char* change = nullptr;
    char* reason = nullptr;
    const std::string json = Write(operation);
    const auto func = reinterpret_cast<zebkit_pdf_before_func>(hook->func);
    const zebkit_pdf_decision decision = func(pdf, json.c_str(), &change, &reason, hook->data);
    if (pdf && pdf->closing) {
      free(change);
      free(reason);
      if (error)
        *error = strdup("a hook closed the document");
      go_on = false;
      break;
    }
    if (decision != ZEBKIT_PDF_CONTINUE) {
      if (error)
        *error = strdup(reason ? reason : "refused by a hook");
      free(change);
      free(reason);
      go_on = false;
      break;
    }
    free(reason);
    if (change) {
      std::optional<base::Value> parsed =
          base::JSONReader::Read(change, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
      free(change);
      if (!parsed || !parsed->is_dict()) {
        if (error)
          *error = strdup("a hook's change is not a JSON object");
        go_on = false;
        break;
      }
      operation = std::move(parsed->GetDict());
      operation.Set("event", event);
    }
  }
  if (pdf)
    --pdf->hook_depth;
  return go_on;
}

void RunAfter(zebkit_pdf* pdf, const char* event, base::DictValue operation) {
  const Hooks hooks = HooksFor(HookType::kAfter, pdf, event);
  if (hooks.empty() || (pdf && pdf->hook_depth >= kMaxHookDepth))
    return;
  operation.Set("event", event);
  const std::string json = Write(operation);
  if (pdf)
    ++pdf->hook_depth;
  for (const auto& hook : hooks) {
    if (hook->removed.load() || (pdf && pdf->closing))
      continue;
    reinterpret_cast<zebkit_pdf_after_func>(hook->func)(pdf, json.c_str(), hook->data);
  }
  if (pdf)
    --pdf->hook_depth;
}

void RemoveHooksOf(zebkit_pdf* pdf) {
  Hooks gone;  // their destroy functions run outside the lock
  Registry& registry = TheRegistry();
  {
    base::AutoLock lock(registry.lock);
    for (auto it = registry.hooks.begin(); it != registry.hooks.end();) {
      if ((*it)->pdf == pdf) {
        (*it)->removed.store(true);
        gone.push_back(std::move(*it));
        it = registry.hooks.erase(it);
        g_hook_count.fetch_sub(1, std::memory_order_relaxed);
      } else {
        ++it;
      }
    }
  }
}

std::shared_ptr<Hook> FindKind(zebkit_pdf* pdf, const std::string& name) {
  if (name.empty())
    return nullptr;
  Hooks kinds = HooksFor(HookType::kKind, pdf, name);
  // The document's own first; "*" is not a kind's name.
  for (auto it = kinds.rbegin(); it != kinds.rend(); ++it) {
    if ((*it)->key == name && !(*it)->removed.load())
      return *it;
  }
  return nullptr;
}

std::shared_ptr<Hook> KindOf(zebkit_pdf* pdf, const std::string& subtype, const base::DictValue* entries) {
  if (!AnyHooks())
    return nullptr;
  const Hooks kinds = HooksFor(HookType::kKind, pdf, "");
  for (auto it = kinds.rbegin(); it != kinds.rend(); ++it) {
    const Hook& kind = **it;
    if (kind.removed.load())
      continue;
    if (kind.subtype.empty()) {
      if (kind.key == subtype)
        return *it;
    } else if (entries && kind.subtype == subtype) {
      const std::string* name = entries->FindString(kind.prefix + "Kind");
      if (name && *name == "/" + kind.key)
        return *it;
    }
  }
  return nullptr;
}

std::vector<std::string> KindPrefixes(zebkit_pdf* pdf) {
  std::vector<std::string> prefixes;
  for (const auto& kind : HooksFor(HookType::kKind, pdf, "")) {
    if (std::find(prefixes.begin(), prefixes.end(), kind->prefix) == prefixes.end())
      prefixes.push_back(kind->prefix);
  }
  return prefixes;
}

std::string CheckKindData(const Hook& kind, const base::Value& data) {
  if (!data.is_dict())
    return "data is not an object";
  const base::DictValue& dict = data.GetDict();
  if (kind.schema.is_dict()) {
    const base::DictValue& schema = kind.schema.GetDict();
    for (const auto [key, value] : dict) {
      if (!schema.contains(key))
        return "data has no property " + key;
    }
    for (const auto [key, rule_value] : schema) {
      if (!rule_value.is_dict())
        continue;
      const base::DictValue& rule = rule_value.GetDict();
      const base::Value* value = dict.Find(key);
      if (!value) {
        if (rule.FindBool("required").value_or(false))
          return "data needs " + key;
        continue;
      }
      if (const std::string* type = rule.FindString("type")) {
        bool ok = false;
        if (*type == "string")
          ok = value->is_string();
        else if (*type == "number")
          ok = value->is_int() || value->is_double();
        else if (*type == "integer")
          ok = value->is_int();
        else if (*type == "boolean")
          ok = value->is_bool();
        else if (*type == "array")
          ok = value->is_list();
        else if (*type == "object")
          ok = value->is_dict();
        if (!ok)
          return key + " is not of type " + *type;
      }
      if (value->is_int() || value->is_double()) {
        const double number = value->GetDouble();
        if (std::optional<double> min = rule.FindDouble("min"); min && number < *min)
          return key + " is below its minimum";
        if (std::optional<double> max = rule.FindDouble("max"); max && number > *max)
          return key + " is above its maximum";
      }
      if (value->is_string()) {
        if (std::optional<int> max = rule.FindInt("maxLength");
            max && value->GetString().size() > static_cast<size_t>(*max)) {
          return key + " is too long";
        }
      }
      if (const base::ListValue* allowed = rule.FindList("enum")) {
        if (std::find(allowed->begin(), allowed->end(), *value) == allowed->end())
          return key + " is not one of those allowed";
      }
    }
  }
  if (kind.check) {
    const std::string json = base::WriteJson(data).value_or("{}");
    if (char* problem = kind.check(json.c_str(), kind.data)) {
      std::string message = problem;
      free(problem);
      return message;
    }
  }
  return std::string();
}

}  // namespace zebkit::pdf

using zebkit::pdf::Hook;
using zebkit::pdf::HookType;
using zebkit::pdf::Register;

extern "C" {

char* zebkit_pdf_string(const char* text) {
  return text ? strdup(text) : nullptr;
}

uint8_t* zebkit_pdf_bytes(const void* data, size_t size) {
  uint8_t* copy = static_cast<uint8_t*>(malloc(size ? size : 1));
  if (copy && size)
    memcpy(copy, data, size);
  return copy;
}

bool zebkit_pdf_hook_remove(int id) {
  std::shared_ptr<Hook> gone;  // its destroy function runs outside the lock
  auto& registry = zebkit::pdf::TheRegistry();
  {
    base::AutoLock lock(registry.lock);
    for (auto it = registry.hooks.begin(); it != registry.hooks.end(); ++it) {
      if ((*it)->id == id) {
        (*it)->removed.store(true);
        gone = std::move(*it);
        registry.hooks.erase(it);
        zebkit::pdf::g_hook_count.fetch_sub(1, std::memory_order_relaxed);
        return true;
      }
    }
  }
  return false;
}

int zebkit_pdf_hook_before(zebkit_pdf* pdf, const char* event, zebkit_pdf_before_func func, void* data,
                           void (*destroy)(void*)) {
  if (!event)
    return -1;
  return Register(HookType::kBefore, pdf, event, reinterpret_cast<void*>(func), data, destroy);
}

int zebkit_pdf_hook_after(zebkit_pdf* pdf, const char* event, zebkit_pdf_after_func func, void* data,
                          void (*destroy)(void*)) {
  if (!event)
    return -1;
  return Register(HookType::kAfter, pdf, event, reinterpret_cast<void*>(func), data, destroy);
}

int zebkit_pdf_hook_appearance(zebkit_pdf* pdf, const char* type, zebkit_pdf_appearance_func func, void* data,
                               void (*destroy)(void*)) {
  if (!type)
    return -1;
  return Register(HookType::kAppearance, pdf, type, reinterpret_cast<void*>(func), data, destroy);
}

int zebkit_pdf_hook_kind(zebkit_pdf* pdf, const zebkit_pdf_kind* kind, char** error) {
  auto fail = [&](const char* message) {
    if (error)
      *error = strdup(message);
    return -1;
  };
  if (!kind || !kind->name || !*kind->name || !kind->prefix || !*kind->prefix)
    return fail("a kind needs a name and a prefix");
  // Names go into the file as they are: letters, digits, '_', '.' and '-'.
  auto clean = [](const char* text) {
    for (const char* c = text; *c; ++c) {
      if (!isalnum(static_cast<unsigned char>(*c)) && *c != '_' && *c != '.' && *c != '-')
        return false;
    }
    return true;
  };
  if (!clean(kind->name) || !clean(kind->prefix) || (kind->subtype && !clean(kind->subtype)))
    return fail("a kind's names are letters, digits, '_', '.' and '-'");
  if (!std::string_view(kind->name).starts_with(kind->prefix) && !kind->subtype)
    return fail("a kind's name starts with its prefix, unless it has a standard subtype");
  auto hook = std::make_shared<Hook>();
  hook->prefix = kind->prefix;
  hook->subtype = kind->subtype ? kind->subtype : "";
  if (kind->schema) {
    std::optional<base::Value> schema =
        base::JSONReader::Read(kind->schema, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
    if (!schema || !schema->is_dict())
      return fail("a kind's schema is not a JSON object");
    hook->schema = std::move(*schema);
  }
  hook->check = kind->check;
  hook->hit_test = kind->hit_test;
  return Register(HookType::kKind, pdf, kind->name, reinterpret_cast<void*>(kind->draw), kind->data,
                  kind->destroy, std::move(hook));
}

int zebkit_pdf_hook_layer(zebkit_pdf* pdf, zebkit_pdf_layer_position position, zebkit_pdf_layer_func func,
                          void* data, void (*destroy)(void*)) {
  return Register(HookType::kLayer, pdf, position == ZEBKIT_PDF_LAYER_ABOVE ? "above" : "below",
                  reinterpret_cast<void*>(func), data, destroy);
}

int zebkit_pdf_hook_ocr(zebkit_pdf* pdf, zebkit_pdf_ocr_func func, void* data, void (*destroy)(void*)) {
  return Register(HookType::kOcr, pdf, "*", reinterpret_cast<void*>(func), data, destroy);
}

int zebkit_pdf_hook_font(zebkit_pdf* pdf, zebkit_pdf_font_func func, void* data, void (*destroy)(void*)) {
  return Register(HookType::kFont, pdf, "*", reinterpret_cast<void*>(func), data, destroy);
}

int zebkit_pdf_hook_trust(zebkit_pdf* pdf, zebkit_pdf_trust_func func, void* data, void (*destroy)(void*)) {
  return Register(HookType::kTrust, pdf, "*", reinterpret_cast<void*>(func), data, destroy);
}

int zebkit_pdf_hook_action(zebkit_pdf* pdf, zebkit_pdf_action_func func, void* data, void (*destroy)(void*)) {
  return Register(HookType::kAction, pdf, "*", reinterpret_cast<void*>(func), data, destroy);
}

int zebkit_pdf_hook_input(zebkit_pdf* pdf, const char* tool, zebkit_pdf_input_func func, void* data,
                          void (*destroy)(void*)) {
  if (!tool)
    return -1;
  return Register(HookType::kInput, pdf, tool, reinterpret_cast<void*>(func), data, destroy);
}

}  // extern "C"
