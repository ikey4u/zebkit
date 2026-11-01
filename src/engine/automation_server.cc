// zebkit's automation as Playwright's own endpoint (docs/AUTOMATION.md):
// Playwright's protocol over a WebSocket, what `chromium.connect(url)`
// speaks in each of Playwright's languages, served by the engine with the
// native commands of automation.cc underneath. The objects of the protocol
// (Playwright, Browser, BrowserContext, Page, Frame, handles, Request,
// Response, Route, Dialog, ...) are made as Playwright's dispatchers make
// them (packages/playwright-core/src/server/dispatchers), with the
// initializers of packages/protocol/spec.

#ifdef UNSAFE_BUFFERS_BUILD
#pragma allow_unsafe_buffers
#endif

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/escape.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/threading/thread.h"
#include "base/values.h"
#include "components/version_info/version_info.h"
#include "content/public/browser/browser_thread.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/log/net_log_source.h"
#include "net/server/http_server.h"
#include "net/server/http_server_request_info.h"
#include "net/socket/tcp_server_socket.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "url/gurl.h"
#include "zebkit/engine/include/zebkit/automation.h"
#include "zebkit/engine/include/zebkit/hooks.h"
#include "zebkit/engine/include/zebkit/zebkit.h"

namespace zebkit {

namespace {

constexpr net::NetworkTrafficAnnotationTag kAnnotation =
    net::DefineNetworkTrafficAnnotation("zebkit_playwright_endpoint", R"(
      semantics {
        sender: "zebkit automation endpoint"
        description: "Playwright's protocol to a client the application lets connect."
        trigger: "zebkit_automation_listen()."
        data: "Playwright's protocol."
        destination: LOCAL
      }
      policy {
        cookies_allowed: NO
        setting: "The application decides."
        policy_exception_justification: "An embedder's own endpoint."
      })");

std::string Json(const base::Value& value) {
  return base::WriteJson(value).value_or("null");
}

std::optional<base::Value> Parse(const std::string& json) {
  return base::JSONReader::Read(json, base::JSON_PARSE_RFC);
}

// ---- Values: Playwright's serialized ones and plain JSON -----------------

// Playwright's protocol's form (protocol/src/serializers.ts) of a plain JSON
// value.
constexpr char kBindingsController[] =
    "if (!globalThis.__playwright__binding__controller__) {"
    "  const controller = {"
    "    callBinding: (name, ...args) => globalThis[name](...args),"
    "    parseInitScriptArg: value => {"
    "      const parse = v => {"
    "        if (v === null || typeof v !== 'object') return v;"
    "        if ('v' in v) return ({ null: null, NaN: NaN, Infinity: Infinity, '-Infinity': -Infinity, '-0': -0 })[v.v];"
    "        if ('d' in v) return new Date(v.d);"
    "        if ('u' in v) return new URL(v.u);"
    "        if ('bi' in v) return BigInt(v.bi);"
    "        if ('r' in v) return new RegExp(v.r.p, v.r.f);"
    "        if ('a' in v) return v.a.map(parse);"
    "        if ('o' in v) { const o = {}; for (const { k, v: item } of v.o) o[k] = parse(item); return o; }"
    "        if ('fn' in v) { const name = v.fn; return (...args) => controller.callBinding(name, ...args); }"
    "        return v;"
    "      };"
    "      return parse(value);"
    "    },"
    "  };"
    "  Object.defineProperty(globalThis, '__playwright__binding__controller__', { value: controller, configurable: true });"
    "}\n";

base::Value Serialize(const base::Value& value, int* next_id) {
  switch (value.type()) {
    case base::Value::Type::BOOLEAN:
    case base::Value::Type::INTEGER:
    case base::Value::Type::DOUBLE:
    case base::Value::Type::STRING: {
      base::DictValue v;
      v.Set(value.is_bool() ? "b" : value.is_string() ? "s" : "n", value.Clone());
      return base::Value(std::move(v));
    }
    case base::Value::Type::NONE: {
      base::DictValue v;
      v.Set("v", "null");
      return base::Value(std::move(v));
    }
    case base::Value::Type::LIST: {
      base::ListValue items;
      for (const base::Value& item : value.GetList())
        items.Append(Serialize(item, next_id));
      base::DictValue a;
      a.Set("a", std::move(items));
      a.Set("id", ++*next_id);
      return base::Value(std::move(a));
    }
    case base::Value::Type::DICT: {
      base::ListValue entries;
      for (auto [key, item] : value.GetDict()) {
        base::DictValue entry;
        entry.Set("k", key);
        entry.Set("v", Serialize(item, next_id));
        entries.Append(std::move(entry));
      }
      base::DictValue o;
      o.Set("o", std::move(entries));
      o.Set("id", ++*next_id);
      return base::Value(std::move(o));
    }
    default:
      return value.Clone();
  }
}

base::Value Serialize(const base::Value& value) {
  int next = 0;
  return Serialize(value, &next);
}

// Playwright has two serialized forms: the protocol's
// (protocol/src/serializers.ts: {n: 1}, {s: "x"}, {b: true}) and the page's
// (isomorphic/utilityScriptSerializers.ts, which the agent speaks: 1, "x",
// true as they are); the rest ({v}, {d}, {a, id}, {o, id}, {h}, ...) is the
// same in both. The protocol's as the page's:
base::Value ToPage(const base::Value& value) {
  if (!value.is_dict())
    return value.Clone();
  const base::DictValue& dict = value.GetDict();
  for (const char* key : {"n", "s", "b"}) {
    const base::Value* primitive = dict.Find(key);
    if (primitive && !primitive->is_list() && !primitive->is_dict())
      return primitive->Clone();
  }
  base::DictValue result = dict.Clone();
  if (const base::ListValue* a = dict.FindList("a")) {
    base::ListValue items;
    for (const base::Value& item : *a)
      items.Append(ToPage(item));
    result.Set("a", std::move(items));
  } else if (const base::ListValue* o = dict.FindList("o")) {
    base::ListValue entries;
    for (const base::Value& entry : *o) {
      base::DictValue converted = entry.is_dict() ? entry.GetDict().Clone() : base::DictValue();
      if (const base::Value* v = converted.Find("v"))
        converted.Set("v", ToPage(*v));
      entries.Append(std::move(converted));
    }
    result.Set("o", std::move(entries));
  }
  return base::Value(std::move(result));
}

// The page's as the protocol's.
base::Value ToProtocol(const base::Value& value) {
  if (value.is_bool() || value.is_int() || value.is_double() || value.is_string()) {
    base::DictValue v;
    v.Set(value.is_bool() ? "b" : value.is_string() ? "s" : "n", value.Clone());
    return base::Value(std::move(v));
  }
  if (!value.is_dict())
    return value.Clone();
  const base::DictValue& dict = value.GetDict();
  base::DictValue result = dict.Clone();
  if (const base::ListValue* a = dict.FindList("a")) {
    base::ListValue items;
    for (const base::Value& item : *a)
      items.Append(ToProtocol(item));
    result.Set("a", std::move(items));
  } else if (const base::ListValue* o = dict.FindList("o")) {
    base::ListValue entries;
    for (const base::Value& entry : *o) {
      base::DictValue converted = entry.is_dict() ? entry.GetDict().Clone() : base::DictValue();
      if (const base::Value* v = converted.Find("v"))
        converted.Set("v", ToProtocol(*v));
      entries.Append(std::move(converted));
    }
    result.Set("o", std::move(entries));
  }
  return base::Value(std::move(result));
}

// Without the fields of dictionaries that are null, at any depth.
void RemoveNulls(base::Value& value) {
  if (value.is_list()) {
    for (base::Value& item : value.GetList())
      RemoveNulls(item);
  } else if (value.is_dict()) {
    std::vector<std::string> empty;
    for (auto [key, item] : value.GetDict()) {
      if (item.is_none())
        empty.push_back(key);
      else
        RemoveNulls(item);
    }
    for (const std::string& key : empty)
      value.GetDict().Remove(key);
  }
}

// A serialized value (the protocol's) as plain JSON (what is lost:
// undefined, dates, ...).
base::Value Plain(const base::Value& value) {
  if (!value.is_dict())
    return value.Clone();
  const base::DictValue& dict = value.GetDict();
  for (const char* key : {"n", "s", "b"}) {
    const base::Value* primitive = dict.Find(key);
    if (primitive && !primitive->is_list() && !primitive->is_dict())
      return primitive->Clone();
  }
  if (const std::string* v = dict.FindString("v")) {
    if (*v == "null" || *v == "undefined")
      return base::Value();
    return base::Value(*v);
  }
  if (const std::string* d = dict.FindString("d"))
    return base::Value(*d);
  if (const std::string* u = dict.FindString("u"))
    return base::Value(*u);
  if (const std::string* bi = dict.FindString("bi"))
    return base::Value(*bi);
  if (const base::ListValue* a = dict.FindList("a")) {
    base::ListValue items;
    for (const base::Value& item : *a)
      items.Append(Plain(item));
    return base::Value(std::move(items));
  }
  if (const base::ListValue* o = dict.FindList("o")) {
    base::DictValue result;
    for (const base::Value& entry : *o) {
      if (!entry.is_dict())
        continue;
      const std::string* k = entry.GetDict().FindString("k");
      const base::Value* v = entry.GetDict().Find("v");
      if (k && v)
        result.Set(*k, Plain(*v));
    }
    return base::Value(std::move(result));
  }
  if (const base::DictValue* e = dict.FindDict("e")) {
    base::DictValue error;
    if (const std::string* m = e->FindString("m"))
      error.Set("message", *m);
    if (const std::string* n = e->FindString("n"))
      error.Set("name", *n);
    return base::Value(std::move(error));
  }
  return value.Clone();
}

// ---- One client ----------------------------------------------------------

class Endpoint;

class Connection {
 public:
  using SendFunc = base::RepeatingCallback<void(std::string)>;

  explicit Connection(SendFunc send) : send_(std::move(send)) {}

  ~Connection() {
    // No more events to this connection, before its pages go.
    for (const auto& [view, guid] : pages_) {
      zebkit_automation_on_event(view, nullptr, nullptr);
      zebkit_view_on_window_open(view, nullptr, nullptr);
    }
    // The client's contexts go with it, as the pages of Playwright's
    // launchServer() do when its client disconnects.
    for (const auto& [guid, object] : objects_) {
      if ((object.type == "BrowserContext" ||
           (object.type == "APIRequestContext" && object.data.FindBool("ownContext").value_or(false))) &&
          !object.id.empty()) {
        base::DictValue params;
        params.Set("context", object.id);
        zebkit_automation_call(nullptr, "BrowserContext.close",
                               Json(base::Value(std::move(params))).c_str(),
                               nullptr, nullptr);
      }
    }
  }

  void Received(const std::string& message);

 private:
  struct Object {
    std::string type;
    // The native id: a context's "x1", a page's "p2", a frame's "f12", a
    // handle's "h3", a request's "q4", ...
    std::string id;
    std::string page;     // its page's guid
    std::string context;  // its context's guid
    base::DictValue data;
  };

  // A command's answer to the client, or its error.
  base::Value TakeId(int id) {
    auto found = ids_.find(id);
    if (found == ids_.end())
      return base::Value(id);
    base::Value value = std::move(found->second);
    ids_.erase(found);
    return value;
  }

  // An answer; none (base::Value()) for a method that answers nothing, as
  // Playwright's void methods do.
  void Reply(int id, base::Value result) {
    base::DictValue message;
    message.Set("id", TakeId(id));
    // The protocol's optional fields are absent, never null: the client's
    // validator throws on null, in a place where its call then never ends.
    RemoveNulls(result);
    if (!result.is_none())
      message.Set("result", std::move(result));
    send_.Run(Json(base::Value(std::move(message))));
  }

  void ReplyError(int id, const std::string& error) {
    base::DictValue inner;
    inner.Set("message", error);
    inner.Set("name", error.rfind("Timeout ", 0) == 0 ? "TimeoutError" : "Error");
    inner.Set("stack", "");
    base::DictValue outer;
    outer.Set("error", std::move(inner));
    base::DictValue message;
    message.Set("id", TakeId(id));
    message.Set("error", std::move(outer));
    send_.Run(Json(base::Value(std::move(message))));
  }

  // Frame.expect's failure, as Playwright's ExpectError reaches its client:
  // the protocol's errorDetails, and the call's log.
  void ReplyExpectFailed(int id, base::DictValue details, base::ListValue log) {
    base::DictValue inner;
    inner.Set("message", "Expect failed");
    inner.Set("name", "Error");
    inner.Set("stack", "");
    base::DictValue outer;
    outer.Set("error", std::move(inner));
    base::DictValue message;
    message.Set("id", TakeId(id));
    message.Set("error", std::move(outer));
    message.Set("errorDetails", std::move(details));
    message.Set("log", std::move(log));
    send_.Run(Json(base::Value(std::move(message))));
  }

  void Event(const std::string& guid, const std::string& method, base::DictValue params) {
    base::DictValue message;
    message.Set("guid", guid);
    message.Set("method", method);
    message.Set("params", std::move(params));
    send_.Run(Json(base::Value(std::move(message))));
  }

  static base::DictValue Ref(const std::string& guid) {
    base::DictValue ref;
    ref.Set("guid", guid);
    return ref;
  }

  // A protocol object, created in @parent's scope.
  std::string Create(const std::string& parent,
                     const std::string& type,
                     base::DictValue initializer,
                     Object object = Object()) {
    std::string lower = type;
    lower[0] = static_cast<char>(tolower(lower[0]));
    const std::string guid = lower + "@" + base::NumberToString(++next_guid_);
    object.type = type;
    objects_[guid] = std::move(object);
    base::DictValue params;
    params.Set("type", type);
    params.Set("initializer", std::move(initializer));
    params.Set("guid", guid);
    Event(parent, "__create__", std::move(params));
    return guid;
  }

  void Adopt(const std::string& parent, const std::string& guid) {
    base::DictValue params;
    params.Set("guid", guid);
    Event(parent, "__adopt__", std::move(params));
  }

  void Dispose(const std::string& guid) {
    if (!objects_.erase(guid))
      return;
    base::DictValue params;
    Event(guid, "__dispose__", std::move(params));
  }

  Object* Find(const std::string& guid) {
    auto found = objects_.find(guid);
    return found == objects_.end() ? nullptr : &found->second;
  }

  // The guid of the object of @type with native @id on @page; empty when
  // there is none yet.
  std::string Known(const std::string& type, const std::string& page,
                    const std::string& id) {
    auto found = known_.find(type + ":" + page + ":" + id);
    return found == known_.end() ? std::string() : found->second;
  }
  void Remember(const std::string& type, const std::string& page,
                const std::string& id, const std::string& guid) {
    known_[type + ":" + page + ":" + id] = guid;
  }

  zebkit_view* ViewOf(const Object& object) {
    const Object* page = object.type == "Page" ? &object : Find(object.page);
    return page ? zebkit_automation_page(page->id.c_str()) : nullptr;
  }

  void Initialize(int id);
  // A disposable retains the native token and owning page/context.
  void ReplyDisposable(int id, const std::string& parent, base::Value result) {
    Object object;
    object.page = parent;
    if (result.is_dict())
      object.data = std::move(result.GetDict());
    base::DictValue answer;
    answer.Set("disposable", Ref(Create(parent, "Disposable", base::DictValue(), std::move(object))));
    Reply(id, base::Value(std::move(answer)));
  }
  void Command(int id, const std::string& guid, const std::string& method,
               base::DictValue params);
  // A native command, its answer turned back into the protocol's by @then.
  using Then = base::OnceCallback<void(base::Value result)>;
  void Native(int id, zebkit_view* view, const std::string& method,
              base::DictValue params, Then then);
  base::Value Expose(const std::string& page, const std::string& frame,
                     base::Value value);
  std::string FrameGuid(const std::string& page_guid, const std::string& frame_id,
                        const base::DictValue* initial = nullptr);
  std::string RequestGuid(const std::string& page_guid, const base::DictValue& request);
  std::string ResponseGuid(const std::string& page_guid, const std::string& request_guid,
                           const base::DictValue& response);
  void NewContext(int id, base::DictValue params);
  void NewRequest(int id, const std::string& parent, base::DictValue params);
  void PrepareFetch(base::DictValue& params, const base::DictValue& defaults);
  void NewPage(int id, const std::string& context_guid);
  std::string AdoptPage(const std::string& context_guid, const std::string& page_id,
                        const base::Value& frames,
                        const std::string& opener = std::string());
  static void OnWindowOpen(zebkit_view* opener, zebkit_view* opened, const char* url, void* data);
  void PageEvent(const std::string& page_guid, const std::string& event,
                 base::DictValue params);
  void Tracing(int id, const std::string& guid, const std::string& method,
               base::DictValue params);
  void CreateTempFiles(int id, base::DictValue params);
  bool ToNativeArgs(base::DictValue& params);

  static void OnEvent(zebkit_view* view, const char* event, const char* json,
                      void* data);

  SendFunc send_;
  std::map<std::string, Object> objects_;
  std::map<std::string, std::string> known_;
  std::map<zebkit_view*, std::string> pages_;
  std::map<std::string, std::string> fetched_;
  std::map<std::string, base::ListValue> fetch_logs_;
  // page.consoleMessages(), pageErrors(), requests(): what the page has had,
  // as page.ts keeps it, and where the last main frame's navigation left them.
  struct PageRecords {
    std::vector<base::DictValue> console;
    std::vector<base::DictValue> errors;
    size_t console_mark = 0;
    size_t errors_mark = 0;
    std::vector<std::string> requests;
  };
  static constexpr size_t kMaxPageRecords = 200;
  static constexpr size_t kMaxPageRequests = 100;
  std::map<std::string, PageRecords> page_records_;
  // Stream objects' contents (Artifact.saveAsStream): what is left to read.
  std::map<std::string, std::string> streams_;
  // A remote client's files (BrowserContext.createTempFiles,
  // WritableStream), written in order.
  scoped_refptr<base::SequencedTaskRunner> files_ =
      base::ThreadPool::CreateSequencedTaskRunner({base::MayBlock()});
  std::string browser_;
  int next_guid_ = 0;
  int next_id_ = 0;
  std::map<int, base::Value> ids_;
  int next_fetch_ = 0;
  base::WeakPtrFactory<Connection> weak_factory_{this};
};

void Connection::Received(const std::string& text) {
  std::optional<base::Value> message = Parse(text);
  if (!message || !message->is_dict())
    return;
  base::DictValue& dict = message->GetDict();
  const std::string* method_name = dict.FindString("method");
  // What the client waits for, for its own logs: never answered, as
  // Playwright's dispatchers do not.
  if (method_name && *method_name == "__waitInfo__")
    return;
  // The client's id, any JSON value ("abcd@2" from Playwright 1.64), kept
  // for its answer.
  const int id = ++next_id_;
  if (const base::Value* given = dict.Find("id"))
    ids_[id] = given->Clone();
  const std::string* guid = dict.FindString("guid");
  const std::string* method = dict.FindString("method");
  base::DictValue* params = dict.FindDict("params");
  if (!guid || !method)
    return;
  if (guid->empty() && *method == "initialize") {
    Initialize(id);
    return;
  }
  base::DictValue command_params = params ? std::move(*params) : base::DictValue();
  // Since 1.60 or so the client's timeout travels in the call's metadata, not
  // in its parameters (channelOwner.ts); the native commands take "timeout".
  if (const base::DictValue* metadata = dict.FindDict("metadata")) {
    if (std::optional<double> timeout = metadata->FindDouble("timeout"))
      command_params.Set("timeout", *timeout);

    // Frame.expect's log begins with the call's title.
    if (const std::string* title = metadata->FindString("title");
        title && *method == "expect") {
      command_params.Set("expectTitle", *title);
    }
  }
  Command(id, *guid, *method, std::move(command_params));
}

void Connection::Initialize(int id) {
  auto browser_type = [&](const char* name) {
    base::DictValue initializer;
    initializer.Set("executablePath", "zebkit");
    initializer.Set("name", name);
    return Create("", "BrowserType", std::move(initializer));
  };
  const std::string chromium = browser_type("chromium");
  const std::string firefox = browser_type("firefox");
  const std::string webkit = browser_type("webkit");
  const std::string android = Create("", "Android", base::DictValue());
  const std::string electron = Create("", "Electron", base::DictValue());
  base::DictValue browser;
  // Playwright's browser.version(): the number ("155.0.8059.84").
  browser.Set("version", std::string(version_info::GetVersionNumber()));
  browser.Set("name", "chromium");
  browser.Set("browserName", "chromium");
  browser_ = Create("", "Browser", std::move(browser));
  base::DictValue playwright;
  playwright.Set("chromium", Ref(chromium));
  playwright.Set("firefox", Ref(firefox));
  playwright.Set("webkit", Ref(webkit));
  playwright.Set("android", Ref(android));
  playwright.Set("electron", Ref(electron));
  playwright.Set("preLaunchedBrowser", Ref(browser_));
  const std::string guid = Create("", "Playwright", std::move(playwright));
  base::DictValue result;
  result.Set("playwright", Ref(guid));
  Reply(id, base::Value(std::move(result)));
}

void Connection::Native(int id,
                        zebkit_view* view,
                        const std::string& method,
                        base::DictValue params,
                        Then then) {
  struct Pending {
    base::WeakPtr<Connection> connection;
    int id;
    Then then;
  };
  auto* pending = new Pending{weak_factory_.GetWeakPtr(), id, std::move(then)};
  zebkit_automation_call(
      view, method.c_str(), Json(base::Value(std::move(params))).c_str(),
      [](zebkit_view*, const char* json, const char* error, void* data) {
        std::unique_ptr<Pending> pending(static_cast<Pending*>(data));
        if (!pending->connection)
          return;
        if (error) {
          pending->connection->ReplyError(pending->id, error);
          return;
        }
        std::optional<base::Value> result = Parse(json ? json : "{}");
        std::move(pending->then).Run(result ? std::move(*result)
                                            : base::Value(base::DictValue()));
      },
      pending);
}

// The native answer's handles ({handle: "h3", preview, element}) and
// frames as the protocol's objects.
base::Value Connection::Expose(const std::string& page,
                               const std::string& frame,
                               base::Value value) {
  if (value.is_list()) {
    base::ListValue list;
    for (base::Value& item : value.GetList())
      list.Append(Expose(page, frame, std::move(item)));
    return base::Value(std::move(list));
  }
  if (!value.is_dict())
    return value;
  base::DictValue& dict = value.GetDict();
  const std::string* handle = dict.FindString("handle");
  const std::string* preview = dict.FindString("preview");
  if (handle && preview) {
    std::string guid = Known("handle", page, *handle);
    if (guid.empty()) {
      const bool element = dict.FindBool("element").value_or(false);
      base::DictValue initializer;
      initializer.Set("preview", *preview);
      Object object;
      object.id = *handle;
      object.page = page;
      guid = Create(frame.empty() ? page : frame,
                    element ? "ElementHandle" : "JSHandle", std::move(initializer),
                    std::move(object));
      Remember("handle", page, *handle, guid);
    }
    return base::Value(Ref(guid));
  }
  base::DictValue result;
  for (auto [key, item] : dict)
    result.Set(key, Expose(page, frame, std::move(item)));
  return base::Value(std::move(result));
}

std::string Connection::FrameGuid(const std::string& page_guid,
                                  const std::string& frame_id,
                                  const base::DictValue* initial) {
  std::string guid = Known("frame", page_guid, frame_id);
  if (!guid.empty())
    return guid;
  base::DictValue initializer;
  const std::string* url = initial ? initial->FindString("url") : nullptr;
  const std::string* name = initial ? initial->FindString("name") : nullptr;
  initializer.Set("url", url && !url->empty() ? *url : "about:blank");
  initializer.Set("name", name ? *name : "");
  if (const std::string* parent = initial ? initial->FindString("parentFrame") : nullptr) {
    const std::string parent_guid = Known("frame", page_guid, *parent);
    if (!parent_guid.empty())
      initializer.Set("parentFrame", Ref(parent_guid));
  }
  initializer.Set("loadStates", base::ListValue());
  Object object;
  object.id = frame_id;
  object.page = page_guid;
  Object* page = Find(page_guid);
  object.context = page ? page->context : std::string();
  guid = Create(page && !page->context.empty() ? page->context : page_guid, "Frame",
                std::move(initializer), std::move(object));
  Remember("frame", page_guid, frame_id, guid);
  return guid;
}

std::string Connection::RequestGuid(const std::string& page_guid,
                                    const base::DictValue& request) {
  const std::string* id = request.FindString("request");
  const std::string key = id ? *id : "nav" + base::NumberToString(next_guid_ + 1);
  if (std::string guid = Known("request", page_guid, key); !guid.empty())
    return guid;
  base::DictValue initializer;
  const std::string* frame = request.FindString("frame");
  if (frame) {
    if (std::string frame_guid = Known("frame", page_guid, *frame); !frame_guid.empty())
      initializer.Set("frame", Ref(frame_guid));
  }
  const std::string* url = request.FindString("url");
  initializer.Set("url", url ? *url : "");
  const std::string* type = request.FindString("resourceType");
  initializer.Set("resourceType", type ? *type : "document");
  const std::string* method = request.FindString("method");
  initializer.Set("method", method ? *method : "GET");
  const base::ListValue* headers = request.FindList("headers");
  initializer.Set("headers", headers ? headers->Clone() : base::ListValue());
  initializer.Set("isNavigationRequest",
                  request.FindBool("isNavigationRequest").value_or(!id));
  if (const std::string* from = request.FindString("redirectedFrom")) {
    if (std::string from_guid = Known("request", page_guid, *from); !from_guid.empty())
      initializer.Set("redirectedFrom", Ref(from_guid));
  }
  Object object;
  object.id = key;
  object.page = page_guid;
  object.data = request.Clone();
  Object* page = Find(page_guid);
  const std::string scope = page && !page->context.empty() ? page->context : page_guid;
  const std::string guid = Create(scope, "Request", std::move(initializer), std::move(object));
  Remember("request", page_guid, key, guid);
  std::vector<std::string>& requests = page_records_[page_guid].requests;
  requests.push_back(guid);
  if (requests.size() > kMaxPageRequests)
    requests.erase(requests.begin());
  return guid;
}

std::string Connection::ResponseGuid(const std::string& page_guid,
                                     const std::string& request_guid,
                                     const base::DictValue& response) {
  if (Object* request = Find(request_guid)) {
    if (const std::string* known = request->data.FindString("response"))
      return *known;
  }
  base::DictValue initializer;
  initializer.Set("request", Ref(request_guid));
  const std::string* url = response.FindString("url");
  initializer.Set("url", url ? *url : "");
  initializer.Set("status", response.FindInt("status").value_or(200));
  const std::string* text = response.FindString("statusText");
  initializer.Set("statusText", text ? *text : "");
  const base::ListValue* headers = response.FindList("headers");
  initializer.Set("headers", headers ? headers->Clone() : base::ListValue());
  base::DictValue timing;
  for (const char* key : {"startTime", "domainLookupStart", "domainLookupEnd",
                          "connectStart", "secureConnectionStart", "connectEnd",
                          "requestStart", "responseStart"}) {
    timing.Set(key, -1.0);
  }
  initializer.Set("timing", response.FindDict("timing") ? response.FindDict("timing")->Clone() : std::move(timing));
  initializer.Set("fromServiceWorker", response.FindBool("fromServiceWorker").value_or(false));
  Object object;
  Object* request = Find(request_guid);
  object.id = request ? request->id : std::string();
  object.page = page_guid;
  object.data = response.Clone();
  const std::string guid =
      Create(request_guid, "Response", std::move(initializer), std::move(object));
  if (request)
    request->data.Set("response", guid);
  return guid;
}

// Browser.newContext: a native context with the client's options; the
// APIRequestContext.fetch's parameters as the native command has them, what
// fetch.ts does before it sends: the query, the body with its content type,
// the headers and the request context's defaults.
void Connection::PrepareFetch(base::DictValue& params, const base::DictValue& defaults) {
  std::string url = params.FindString("url") ? *params.FindString("url") : std::string();
  std::string separator = url.find('?') == std::string::npos ? "?" : "&";
  if (const std::string* encoded = params.FindString("encodedParams")) {
    url += separator + *encoded;
    separator = "&";
  } else if (const base::ListValue* query = params.FindList("params")) {
    for (const base::Value& pair : *query) {
      const std::string* name = pair.is_dict() ? pair.GetDict().FindString("name") : nullptr;
      const std::string* value = pair.is_dict() ? pair.GetDict().FindString("value") : nullptr;
      if (name && value) {
        url += separator + base::EscapeQueryParamValue(*name, true) + "=" +
               base::EscapeQueryParamValue(*value, true);
        separator = "&";
      }
    }
  }
  params.Set("url", url);

  base::ListValue headers;
  if (const base::ListValue* given = defaults.FindList("extraHTTPHeaders"))
    headers = given->Clone();
  if (const std::string* agent = defaults.FindString("userAgent")) {
    base::DictValue header;
    header.Set("name", "User-Agent");
    header.Set("value", *agent);
    headers.Append(std::move(header));
  }
  if (const base::ListValue* given = params.FindList("headers")) {
    for (const base::Value& header : *given)
      headers.Append(header.Clone());
  }
  bool has_type = false;
  for (const base::Value& header : headers) {
    const std::string* name = header.is_dict() ? header.GetDict().FindString("name") : nullptr;
    has_type = has_type || (name && base::EqualsCaseInsensitiveASCII(*name, "content-type"));
  }
  std::string content_type;
  std::string body;
  bool has_body = false;
  if (const std::string* json = params.FindString("jsonData")) {
    body = *json;
    has_body = true;
    content_type = "application/json";
  } else if (const base::ListValue* form = params.FindList("formData")) {
    for (const base::Value& pair : *form) {
      const std::string* name = pair.is_dict() ? pair.GetDict().FindString("name") : nullptr;
      const std::string* value = pair.is_dict() ? pair.GetDict().FindString("value") : nullptr;
      if (name && value) {
        body += (body.empty() ? "" : "&") + base::EscapeQueryParamValue(*name, true) + "=" +
                base::EscapeQueryParamValue(*value, true);
      }
    }
    has_body = true;
    content_type = "application/x-www-form-urlencoded";
  } else if (const base::ListValue* parts = params.FindList("multipartData")) {
    const std::string boundary = "----zebkitFormBoundary" + base::NumberToString(++next_fetch_);
    for (const base::Value& part : *parts) {
      if (!part.is_dict())
        continue;
      const std::string* name = part.GetDict().FindString("name");
      const std::string* value = part.GetDict().FindString("value");
      const base::DictValue* file = part.GetDict().FindDict("file");
      if (!name || (!value && !file))
        continue;
      body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + *name + "\"";
      if (file) {
        const std::string* file_name = file->FindString("name");
        const std::string* mime = file->FindString("mimeType");
        const std::string* buffer = file->FindString("buffer");
        std::string bytes;
        if (buffer && !base::Base64Decode(*buffer, &bytes))
          bytes.clear();
        body += "; filename=\"" + (file_name ? *file_name : std::string()) +
                "\"\r\nContent-Type: " + (mime ? *mime : "application/octet-stream") +
                "\r\n\r\n" + bytes + "\r\n";
      } else {
        body += "\r\n\r\n" + *value + "\r\n";
      }
    }
    body += "--" + boundary + "--\r\n";
    has_body = true;
    content_type = "multipart/form-data; boundary=" + boundary;
  }
  if (has_body) {
    // The native command takes base64.
    params.Set("postData", base::Base64Encode(body));
    if (!has_type) {
      base::DictValue header;
      header.Set("name", "Content-Type");
      header.Set("value", content_type);
      headers.Append(std::move(header));
    }
  }
  params.Set("headers", std::move(headers));
  if (!params.FindInt("maxRedirects")) {
    if (std::optional<int> redirects = defaults.FindInt("maxRedirects"))
      params.Set("maxRedirects", *redirects);
  }
}

// Playwright.newRequest, playwright.request.newContext(): an APIRequestContext
// with a profile of its own, in memory.
void Connection::NewRequest(int id, const std::string& parent, base::DictValue params) {
  base::DictValue options;
  if (const base::Value* state = params.Find("storageState"))
    options.Set("storageState", state->Clone());
  Native(id, nullptr, "Browser.newContext", std::move(options),
         base::BindOnce(
             [](base::WeakPtr<Connection> self, int id, std::string parent, base::DictValue params,
                base::Value result) {
               if (!self)
                 return;
               const std::string* context =
                   result.is_dict() ? result.GetDict().FindString("context") : nullptr;
               if (!context) {
                 self->ReplyError(id, "zebkit: no context");
                 return;
               }
               Object tracing_object;
               tracing_object.id = *context;
               const std::string tracing =
                   self->Create(parent, "Tracing", base::DictValue(), std::move(tracing_object));
               base::DictValue initializer;
               initializer.Set("tracing", Ref(tracing));
               Object object;
               object.id = *context;
               object.data = std::move(params);
               object.data.Set("ownContext", true);
               const std::string request =
                   self->Create(parent, "APIRequestContext", std::move(initializer), std::move(object));
               base::DictValue answer;
               answer.Set("request", Ref(request));
               self->Reply(id, base::Value(std::move(answer)));
             },
             weak_factory_.GetWeakPtr(), id, parent, std::move(params)));
}

// BrowserContext with its Tracing, APIRequestContext and Debugger.
void Connection::NewContext(int id, base::DictValue params) {
  base::DictValue options = params.Clone();
  Native(id, nullptr, "Browser.newContext", std::move(params),
         base::BindOnce(
             [](base::WeakPtr<Connection> self, int id, base::DictValue options,
                base::Value result) {
               if (!self)
                 return;
               const std::string* context =
                   result.is_dict() ? result.GetDict().FindString("context") : nullptr;
               if (!context) {
                 self->ReplyError(id, "zebkit: no context");
                 return;
               }
               Object tracing_object;
               tracing_object.id = *context;
               const std::string tracing =
                   self->Create(self->browser_, "Tracing", base::DictValue(),
                                std::move(tracing_object));
               base::DictValue request_init;
               request_init.Set("tracing", Ref(tracing));
               Object request_object;
               request_object.id = *context;
               const std::string request = self->Create(
                   self->browser_, "APIRequestContext", std::move(request_init),
                   std::move(request_object));
               const std::string debugger =
                   self->Create(self->browser_, "Debugger", base::DictValue());
               base::DictValue initializer;
               initializer.Set("debugger", Ref(debugger));
               initializer.Set("requestContext", Ref(request));
               initializer.Set("tracing", Ref(tracing));
               Object object;
               object.id = *context;
               if (const std::string* base_url = options.FindString("baseURL"))
                 object.data.Set("baseURL", *base_url);
               if (const base::DictValue* viewport = options.FindDict("viewport"))
                 object.data.Set("viewport", viewport->Clone());
               if (options.FindBool("noDefaultViewport").value_or(false))
                 object.data.Set("noDefaultViewport", true);
               initializer.Set("options", std::move(options));
               const std::string guid = self->Create(self->browser_, "BrowserContext",
                                                     std::move(initializer), std::move(object));
               self->Find(tracing)->context = guid;
               self->Find(request)->context = guid;
               base::DictValue answer;
               answer.Set("context", Ref(guid));
               self->Reply(id, base::Value(std::move(answer)));
             },
             weak_factory_.GetWeakPtr(), id, std::move(options)));
}

// BrowserContext.newPage: a native page, its main frame, the Page.
void Connection::NewPage(int id, const std::string& context_guid) {
  Object* context = Find(context_guid);
  if (!context) {
    ReplyError(id, "Target page, context or browser has been closed");
    return;
  }
  base::DictValue params;
  params.Set("context", context->id);
  Native(id, nullptr, "BrowserContext.newPage", std::move(params),
         base::BindOnce(
             [](base::WeakPtr<Connection> self, int id, std::string context_guid,
                base::Value result) {
               if (!self)
                 return;
               const std::string* page =
                   result.is_dict() ? result.GetDict().FindString("page") : nullptr;
               zebkit_view* view = page ? zebkit_automation_page(page->c_str()) : nullptr;
               if (!view) {
                 self->ReplyError(id, "zebkit: no page");
                 return;
               }
               const std::string page_id = *page;
               self->Native(id, view, "Page.frames", base::DictValue(),
                            base::BindOnce(
                                [](base::WeakPtr<Connection> self, int id,
                                   std::string context_guid, std::string page_id,
                                   base::Value frames) {
                                  if (!self)
                                    return;
                                  const std::string guid =
                                      self->AdoptPage(context_guid, page_id, frames);
                                  base::DictValue answer;
                                  answer.Set("page", Ref(guid));
                                  self->Reply(id, base::Value(std::move(answer)));
                                },
                                self, id, context_guid, page_id));
             },
             weak_factory_.GetWeakPtr(), id, context_guid));
}

std::string Connection::AdoptPage(const std::string& context_guid,
                                  const std::string& page_id,
                                  const base::Value& frames,
                                  const std::string& opener) {
  zebkit_view* view = zebkit_automation_page(page_id.c_str());
  // The page's object first in name, so that its frames know it.
  Object page_object;
  page_object.id = page_id;
  page_object.context = context_guid;
  const std::string reserved = "page@" + base::NumberToString(++next_guid_);
  objects_[reserved] = std::move(page_object);
  objects_[reserved].type = "Page";
  std::string main_frame;
  if (const base::ListValue* list =
          frames.is_dict() ? frames.GetDict().FindList("frames") : nullptr) {
    for (const base::Value& frame : *list) {
      if (!frame.is_dict())
        continue;
      const std::string* id = frame.GetDict().FindString("frame");
      if (!id)
        continue;
      const std::string guid = FrameGuid(reserved, *id, &frame.GetDict());
      if (!frame.GetDict().FindString("parentFrame"))
        main_frame = guid;
    }
  }
  base::DictValue initializer;
  initializer.Set("mainFrame", Ref(main_frame));
  if (!opener.empty())
    initializer.Set("opener", Ref(opener));
  // The context's, or Playwright's default; none with noDefaultViewport.
  Object* context_object = Find(context_guid);
  const base::DictValue* given_viewport =
      context_object ? context_object->data.FindDict("viewport") : nullptr;
  if (given_viewport) {
    initializer.Set("viewportSize", given_viewport->Clone());
  } else if (!context_object ||
             !context_object->data.FindBool("noDefaultViewport").value_or(false)) {
    base::DictValue viewport;
    viewport.Set("width", 1280);
    viewport.Set("height", 720);
    initializer.Set("viewportSize", std::move(viewport));
  }
  initializer.Set("isClosed", false);
  base::DictValue params;
  params.Set("type", "Page");
  params.Set("initializer", std::move(initializer));
  params.Set("guid", reserved);
  Event(context_guid, "__create__", std::move(params));
  for (const auto& [guid, object] : objects_) {
    if (object.type == "Frame" && object.page == reserved)
      Adopt(reserved, guid);
  }
  pages_[view] = reserved;
  zebkit_automation_on_event(view, &Connection::OnEvent, this);
  // Playwright launches Chromium with --disable-popup-blocking; its popups
  // are pages of the context, with their opener.
  zebkit_view_set_popup_policy(view, ZEBKIT_POPUP_POLICY_ALLOW);
  zebkit_view_on_window_open(view, &Connection::OnWindowOpen, this);
  // A trace or a HAR the context started before it had this page.
  if (Object* context = Find(context_guid)) {
    for (const char* key : {"tracingStart", "tracingChunk", "har"}) {
      const base::DictValue* started = context->data.FindDict(key);
      if (!started)
        continue;
      const std::string method = std::string(key) == "tracingStart"   ? "Tracing.tracingStart"
                                 : std::string(key) == "tracingChunk" ? "Tracing.tracingStartChunk"
                                                                      : "Tracing.harStart";
      zebkit_automation_call(view, method.c_str(), Json(base::Value(started->Clone())).c_str(),
                             nullptr, nullptr);
    }
  }
  base::DictValue page_event;
  page_event.Set("page", Ref(reserved));
  Event(context_guid, "page", std::move(page_event));
  return reserved;
}

void Connection::OnWindowOpen(zebkit_view* opener,
                              zebkit_view* opened,
                              const char* url,
                              void* data) {
  auto* self = static_cast<Connection*>(data);
  auto found = self->pages_.find(opener);
  Object* page = found == self->pages_.end() ? nullptr : self->Find(found->second);
  if (!page)
    return;
  char* id = zebkit_automation_page_id(opened);
  if (!id)
    return;
  struct Pending {
    base::WeakPtr<Connection> connection;
    std::string context;
    std::string opener;
    std::string page;
  };
  auto* pending = new Pending{self->weak_factory_.GetWeakPtr(), page->context,
                              found->second, id};
  zebkit_free(id);
  zebkit_automation_call(
      opened, "Page.frames", "{}",
      [](zebkit_view*, const char* json, const char*, void* data) {
        std::unique_ptr<Pending> pending(static_cast<Pending*>(data));
        if (!pending->connection || !json)
          return;
        std::optional<base::Value> frames = Parse(json);
        if (frames) {
          pending->connection->AdoptPage(pending->context, pending->page, *frames,
                                         pending->opener);
        }
      },
      pending);
}

void Connection::OnEvent(zebkit_view* view,
                         const char* event,
                         const char* json,
                         void* data) {
  auto* self = static_cast<Connection*>(data);
  auto found = self->pages_.find(view);
  if (found == self->pages_.end())
    return;
  std::optional<base::Value> params = Parse(json ? json : "{}");
  self->PageEvent(found->second, event ? event : "",
                  params && params->is_dict() ? std::move(params->GetDict())
                                              : base::DictValue());
}

// The native events of a page as the protocol's, on its objects.
void Connection::PageEvent(const std::string& page_guid,
                           const std::string& event,
                           base::DictValue params) {
  Object* page = Find(page_guid);
  if (!page)
    return;
  const std::string context = page->context;
  auto frame_of = [&](const char* key) {
    const std::string* id = params.FindString(key);
    return id ? FrameGuid(page_guid, *id, &params) : std::string();
  };
  if (event == "Frame.navigated") {
    const std::string frame = frame_of("frame");
    base::DictValue navigated;
    const std::string* url = params.FindString("url");
    navigated.Set("url", url ? *url : "");
    const std::string* name = params.FindString("name");
    navigated.Set("name", name ? *name : "");
    if (!params.FindBool("sameDocument").value_or(false)) {
      if (!params.FindString("parentFrame")) {
        PageRecords& records = page_records_[page_guid];
        records.console_mark = records.console.size();
        records.errors_mark = records.errors.size();
      }
      navigated.Set("newDocument", base::DictValue());
      for (const char* state : {"domcontentloaded", "load", "networkidle"}) {
        if (Object* frame_object = Find(frame))
          frame_object->data.Remove(std::string("state.") + state);
        base::DictValue remove;
        remove.Set("remove", state);
        Event(frame, "loadstate", std::move(remove));
      }
    }
    Event(frame, "navigated", std::move(navigated));
  } else if (event == "Frame.loadstate") {
    const std::string frame = frame_of("frame");
    base::DictValue add;
    const std::string* state = params.FindString("state");
    // Once for a document, as frames.ts's _firedLifecycleEvents.
    Object* frame_object = Find(frame);
    const std::string name = state ? *state : "load";
    if (frame_object) {
      if (frame_object->data.FindBool("state." + name).value_or(false))
        return;
      frame_object->data.Set("state." + name, true);
    }
    add.Set("add", name);
    Event(frame, "loadstate", std::move(add));
  } else if (event == "Page.frameAttached") {
    const std::string frame = frame_of("frame");
    Adopt(page_guid, frame);
    base::DictValue attached;
    attached.Set("frame", Ref(frame));
    Event(page_guid, "frameAttached", std::move(attached));
  } else if (event == "Page.frameDetached") {
    const std::string* id = params.FindString("frame");
    const std::string frame = id ? Known("frame", page_guid, *id) : std::string();
    if (!frame.empty()) {
      base::DictValue detached;
      detached.Set("frame", Ref(frame));
      Event(page_guid, "frameDetached", std::move(detached));
    }
  } else if (event == "BrowserContext.console") {
    base::DictValue console;
    const std::string* type = params.FindString("type");
    console.Set("type", type ? *type : "log");
    const std::string* text = params.FindString("text");
    console.Set("text", text ? *text : "");
    console.Set("args", base::ListValue());
    base::DictValue location;
    if (const base::DictValue* given = params.FindDict("location"))
      location = given->Clone();
    location.Set("columnNumber", 0);
    if (!location.FindString("url"))
      location.Set("url", "");
    if (!location.FindInt("lineNumber"))
      location.Set("lineNumber", 0);
    console.Set("location", std::move(location));
    console.Set("timestamp", base::Time::Now().InMillisecondsFSinceUnixEpoch());
    PageRecords& records = page_records_[page_guid];
    records.console.push_back(console.Clone());
    if (records.console.size() > kMaxPageRecords) {
      records.console.erase(records.console.begin());
      if (records.console_mark > 0)
        --records.console_mark;
    }
    console.Set("page", Ref(page_guid));
    Event(context, "console", std::move(console));
  } else if (event == "Page.pageError") {
    base::DictValue serialized;
    if (const base::DictValue* error = params.FindDict("error"))
      serialized.Set("error", error->Clone());
    PageRecords& records = page_records_[page_guid];
    records.errors.push_back(serialized.Clone());
    if (records.errors.size() > kMaxPageRecords) {
      records.errors.erase(records.errors.begin());
      if (records.errors_mark > 0)
        --records.errors_mark;
    }
    base::DictValue message;
    message.Set("error", std::move(serialized));
    if (const base::DictValue* location = params.FindDict("location"))
      message.Set("location", location->Clone());
    message.Set("page", Ref(page_guid));
    Event(context, "pageError", std::move(message));
  } else if (event == "BrowserContext.dialog") {
    base::DictValue initializer;
    initializer.Set("page", Ref(page_guid));
    for (const char* key : {"type", "message", "defaultValue"}) {
      const std::string* value = params.FindString(key);
      initializer.Set(key, value ? *value : "");
    }
    Object object;
    const std::string* id = params.FindString("dialog");
    object.id = id ? *id : std::string();
    object.page = page_guid;
    const std::string guid = Create(context, "Dialog", std::move(initializer), std::move(object));
    base::DictValue dialog;
    dialog.Set("dialog", Ref(guid));
    Event(context, "dialog", std::move(dialog));
  } else if (event == "Page.request") {
    const std::string request = RequestGuid(page_guid, params);
    base::DictValue sent;
    sent.Set("request", Ref(request));
    sent.Set("page", Ref(page_guid));
    Event(context, "request", std::move(sent));
  } else if (event == "Page.response") {
    const std::string* id = params.FindString("request");
    const std::string request = id ? Known("request", page_guid, *id) : std::string();
    if (request.empty())
      return;
    const std::string response = ResponseGuid(page_guid, request, params);
    base::DictValue received;
    received.Set("response", Ref(response));
    received.Set("page", Ref(page_guid));
    Event(context, "response", std::move(received));
  } else if (event == "Page.requestFinished" || event == "Page.requestFailed") {
    const std::string* id = params.FindString("request");
    const std::string request = id ? Known("request", page_guid, *id) : std::string();
    if (request.empty())
      return;
    base::DictValue ended;
    ended.Set("request", Ref(request));
    ended.Set("responseEndTiming", params.FindDouble("responseEndTiming").value_or(-1.0));
    ended.Set("page", Ref(page_guid));
    if (event == "Page.requestFinished") {
      if (Object* object = Find(request)) {
        if (const std::string* response = object->data.FindString("response")) {
          ended.Set("response", Ref(*response));
          if (Object* response_object = Find(*response)) {
            if (const base::DictValue* sizes = params.FindDict("sizes"))
              response_object->data.Set("sizes", sizes->Clone());
          }
        }
      }
      Event(context, "requestFinished", std::move(ended));
    } else {
      const std::string* failure = params.FindString("failureText");
      ended.Set("failureText", failure ? *failure : "net::ERR_FAILED");
      Event(context, "requestFailed", std::move(ended));
    }
  } else if (event == "BrowserContext.route") {
    const base::DictValue* request = params.FindDict("request");
    if (!request)
      return;
    const std::string request_guid = RequestGuid(page_guid, *request);
    base::DictValue initializer;
    initializer.Set("request", Ref(request_guid));
    Object object;
    const std::string* id = params.FindString("route");
    object.id = id ? *id : std::string();
    object.page = page_guid;
    const bool on_page = page->data.FindBool("routes").value_or(false);
    const std::string guid = Create(on_page ? page_guid : context, "Route",
                                    std::move(initializer), std::move(object));
    base::DictValue routed;
    routed.Set("route", Ref(guid));
    Event(on_page ? page_guid : context, "route", std::move(routed));
  } else if (event == "BrowserContext.bindingCall") {
    base::DictValue initializer;
    initializer.Set("frame", Ref(frame_of("frame")));
    const std::string* name = params.FindString("name");
    initializer.Set("name", name ? *name : "");
    base::ListValue args;
    if (const base::ListValue* given = params.FindList("args")) {
      for (const base::Value& arg : *given)
        args.Append(Serialize(arg));
    }
    initializer.Set("args", std::move(args));
    Object object;
    const std::string* id = params.FindString("binding");
    object.id = id ? *id : std::string();
    object.page = page_guid;
    const std::string guid = Create(page_guid, "BindingCall", std::move(initializer),
                                    std::move(object));
    base::DictValue call;
    call.Set("binding", Ref(guid));
    Event(page_guid, "bindingCall", std::move(call));
  } else if (event == "Page.download") {
    base::DictValue initializer;
    initializer.Set("absolutePath", "");
    Object object;
    const std::string* id = params.FindString("download");
    object.id = id ? *id : std::string();
    object.page = page_guid;
    const std::string artifact = Create(page_guid, "Artifact", std::move(initializer),
                                        std::move(object));
    base::DictValue download;
    const std::string* url = params.FindString("url");
    download.Set("url", url ? *url : "");
    const std::string* name = params.FindString("suggestedFilename");
    download.Set("suggestedFilename", name ? *name : "");
    download.Set("artifact", Ref(artifact));
    Event(page_guid, "download", std::move(download));
  } else if (event == "Page.fileChooser") {
    // Playwright's FileChooser is the input's: found as the focused
    // element of the frame, the native chooser answered by the input.
    const std::string* chooser = params.FindString("fileChooser");
    const std::string* frame_id = params.FindString("frame");
    zebkit_view* view = ViewOf(*page);
    if (!chooser || !frame_id || !view)
      return;
    const std::string frame_guid = FrameGuid(page_guid, *frame_id);
    const bool multiple = params.FindBool("isMultiple").value_or(false);
    base::DictValue query;
    query.Set("frame", *frame_id);
    query.Set("expression",
              "() => { const e = document.activeElement; return e && e.type === 'file' ? e : "
              "document.querySelector('input[type=file]'); }");
    query.Set("isFunction", true);
    struct Chooser {
      base::WeakPtr<Connection> self;
      std::string page_guid, frame_guid, chooser;
      bool multiple;
    };
    auto* state = new Chooser{weak_factory_.GetWeakPtr(), page_guid, frame_guid, *chooser, multiple};
    zebkit_automation_call(
        view, "Frame.evaluateExpressionHandle", Json(base::Value(std::move(query))).c_str(),
        [](zebkit_view* view, const char* json, const char*, void* data) {
          std::unique_ptr<Chooser> state(static_cast<Chooser*>(data));
          // The native chooser itself is cancelled: the client gives the
          // input its files (ElementHandle.setInputFiles).
          base::DictValue cancel;
          cancel.Set("fileChooser", state->chooser);
          zebkit_automation_call(view, "FileChooser.setFiles",
                                 Json(base::Value(std::move(cancel))).c_str(), nullptr,
                                 nullptr);
          if (!state->self || !json)
            return;
          std::optional<base::Value> result = Parse(json);
          base::Value* handle = result && result->is_dict() ? result->GetDict().Find("handle")
                                                           : nullptr;
          if (!handle)
            return;
          base::Value element =
              state->self->Expose(state->page_guid, state->frame_guid, std::move(*handle));
          base::DictValue chosen;
          chosen.Set("element", std::move(element));
          chosen.Set("isMultiple", state->multiple);
          state->self->Event(state->page_guid, "fileChooser", std::move(chosen));
        },
        state);
  } else if (event == "Page.worker") {
    base::DictValue initializer;
    const std::string* url = params.FindString("url");
    initializer.Set("url", url ? *url : "");
    Object object;
    const std::string* id = params.FindString("worker");
    object.id = id ? *id : std::string();
    object.page = page_guid;
    const std::string guid = Create(page_guid, "Worker", std::move(initializer),
                                    std::move(object));
    Remember("worker", page_guid, object.id.empty() ? (id ? *id : "") : (id ? *id : ""), guid);
    base::DictValue worker;
    worker.Set("worker", Ref(guid));
    Event(page_guid, "worker", std::move(worker));
  } else if (event == "Worker.close") {
    const std::string* id = params.FindString("worker");
    const std::string guid = id ? Known("worker", page_guid, *id) : std::string();
    if (!guid.empty()) {
      Event(guid, "close", base::DictValue());
      Dispose(guid);
    }
  } else if (event == "Page.webSocketRoute") {
    base::DictValue initializer;
    const std::string* url = params.FindString("url");
    initializer.Set("url", url ? *url : "");
    const base::ListValue* protocols = params.FindList("protocols");
    initializer.Set("protocols", protocols ? protocols->Clone() : base::ListValue());
    Object object;
    const std::string* id = params.FindString("webSocketRoute");
    object.id = id ? *id : std::string();
    object.page = page_guid;
    const std::string guid = Create(page_guid, "WebSocketRoute", std::move(initializer),
                                    std::move(object));
    Remember("ws", page_guid, object.id, guid);
    base::DictValue route;
    route.Set("webSocketRoute", Ref(guid));
    Event(page_guid, "webSocketRoute", std::move(route));
  } else if (event.rfind("WebSocketRoute.", 0) == 0) {
    const std::string* id = params.FindString("webSocketRoute");
    const std::string guid = id ? Known("ws", page_guid, *id) : std::string();
    if (guid.empty())
      return;
    params.Remove("webSocketRoute");
    Event(guid, event.substr(sizeof("WebSocketRoute.") - 1), std::move(params));
  } else if (event == "Page.crash") {
    Event(page_guid, "crash", base::DictValue());
  } else if (event == "Page.close") {
    Event(page_guid, "close", base::DictValue());
    for (auto it = pages_.begin(); it != pages_.end(); ++it) {
      if (it->second == page_guid) {
        pages_.erase(it);
        break;
      }
    }
    Dispose(page_guid);
  }
}

// BrowserContext.createTempFiles: a directory of the engine's for a remote
// client's files, and a WritableStream for each (and for the directory).
void Connection::CreateTempFiles(int id, base::DictValue params) {
  const std::string* root_name = params.FindString("rootDirName");
  const std::string root = root_name ? *root_name : std::string();
  std::vector<std::pair<std::string, std::optional<double>>> items;
  if (const base::ListValue* given = params.FindList("items")) {
    for (const base::Value& item : *given) {
      const base::DictValue* dict = item.GetIfDict();
      const std::string* name = dict ? dict->FindString("name") : nullptr;
      if (name)
        items.emplace_back(*name, dict->FindDouble("lastModifiedMs"));
    }
  }
  std::vector<std::string> names;
  for (const auto& item : items)
    names.push_back(item.first);
  struct Made {
    base::FilePath root;
    std::vector<base::FilePath> files;
  };
  files_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(
          [](std::string root, std::vector<std::string> names) {
            Made made;
            base::FilePath directory;
            if (!base::CreateNewTempDirectory(FILE_PATH_LITERAL("zebkit-files-"), &directory))
              return made;
            made.root = root.empty() ? directory
                                     : directory.Append(base::FilePath::FromUTF8Unsafe(root).BaseName());
            for (const std::string& name : names) {
              // A name of the client's stays inside the directory.
              base::FilePath relative = base::FilePath::FromUTF8Unsafe(name);
              if (relative.IsAbsolute() || relative.ReferencesParent())
                relative = relative.BaseName();
              const base::FilePath file = made.root.Append(relative);
              base::CreateDirectory(file.DirName());
              base::WriteFile(file, std::string_view());
              made.files.push_back(file);
            }
            return made;
          },
          root, std::move(names)),
      base::BindOnce(
          [](base::WeakPtr<Connection> self, int id, bool directory,
             std::vector<std::pair<std::string, std::optional<double>>> items, Made made) {
            if (!self)
              return;
            if (made.root.empty() || made.files.size() != items.size()) {
              self->ReplyError(id, "zebkit: the files could not be made");
              return;
            }
            auto stream = [&](const base::FilePath& path, std::optional<double> modified) {
              Object object;
              object.data.Set("path", path.AsUTF8Unsafe());
              if (modified)
                object.data.Set("lastModifiedMs", *modified);
              return Ref(self->Create(self->browser_, "WritableStream", base::DictValue(),
                                      std::move(object)));
            };
            base::ListValue streams;
            for (size_t i = 0; i < made.files.size(); ++i)
              streams.Append(stream(made.files[i], items[i].second));
            base::DictValue answer;
            answer.Set("writableStreams", std::move(streams));
            if (directory)
              answer.Set("rootDir", stream(made.root, std::nullopt));
            self->Reply(id, base::Value(std::move(answer)));
          },
          weak_factory_.GetWeakPtr(), id, !root.empty(), std::move(items)));
}

// A context's Tracing: zebkit traces pages, so a context's trace is that of
// its first page, started again on the pages that come after (AdoptPage).
void Connection::Tracing(int id,
                         const std::string& guid,
                         const std::string& method,
                         base::DictValue params) {
  Object* tracing = Find(guid);
  Object* context = tracing ? Find(tracing->context) : nullptr;
  if (!context) {
    ReplyError(id, "Target page, context or browser has been closed");
    return;
  }
  if (method == "tracingStart") {
    context->data.Set("tracingStart", params.Clone());
  } else if (method == "tracingStartChunk") {
    context->data.Set("tracingChunk", params.Clone());
  } else if (method == "tracingStop") {
    context->data.Remove("tracingStart");
    context->data.Remove("tracingChunk");
  } else if (method == "harStart") {
    // The id is the client's from here on, whichever page records.
    params.Set("harId", "har@c" + base::NumberToString(++next_guid_));
    context->data.Set("har", params.Clone());
  }
  zebkit_view* view = nullptr;
  std::string page_guid;
  for (const auto& [page_view, page] : pages_) {
    Object* object = Find(page);
    if (object && object->context == tracing->context) {
      view = page_view;
      page_guid = page;
      break;
    }
  }
  const std::string title = params.FindString("title") ? *params.FindString("title") : "";
  if (!view) {
    // Kept for the context's first page.
    if (method == "tracingStopChunk" || method == "harExport") {
      ReplyError(id, "zebkit: the context has no page to trace");
      return;
    }
    base::DictValue answer;
    if (method == "tracingStartChunk")
      answer.Set("traceName", title.empty() ? "trace" : title);
    if (const std::string* har = method == "harStart" ? params.FindString("harId") : nullptr)
      answer.Set("harId", *har);
    Reply(id, base::Value(std::move(answer)));
    return;
  }
  Native(id, view, "Tracing." + method, std::move(params),
         base::BindOnce(
             [](base::WeakPtr<Connection> self, int id, std::string page_guid,
                std::string method, std::string title, base::Value result) {
               if (!self)
                 return;
               base::DictValue answer;
               const base::DictValue* given = result.is_dict() ? &result.GetDict() : nullptr;
               if (const std::string* path = given ? given->FindString("path") : nullptr) {
                 base::DictValue initializer;
                 initializer.Set("absolutePath", *path);
                 Object artifact;
                 artifact.id = *path;
                 artifact.page = page_guid;
                 artifact.data.Set("path", *path);
                 answer.Set("artifact",
                            Ref(self->Create(page_guid, "Artifact", std::move(initializer),
                                             std::move(artifact))));
               }
               if (const std::string* har = given ? given->FindString("harId") : nullptr)
                 answer.Set("harId", *har);
               if (method == "tracingStartChunk")
                 answer.Set("traceName", title.empty() ? "trace" : title);
               self->Reply(id, base::Value(std::move(answer)));
             },
             weak_factory_.GetWeakPtr(), id, page_guid, method, title));
}

// An evaluation's argument, {value, handles: [{guid}]}, as the native
// serialized form ("arg", "argHandles", "serialized").
bool Connection::ToNativeArgs(base::DictValue& params) {
  params.Set("serialized", true);
  base::Value* arg = params.Find("arg");
  if (!arg || !arg->is_dict())
    return true;
  base::DictValue given = std::move(arg->GetDict());
  base::ListValue handles;
  if (const base::ListValue* list = given.FindList("handles")) {
    for (const base::Value& handle : *list) {
      const std::string* guid = handle.is_dict() ? handle.GetDict().FindString("guid") : nullptr;
      Object* object = guid ? Find(*guid) : nullptr;
      if (!object)
        return false;
      handles.Append(object->id);
    }
  }
  params.Set("argHandles", std::move(handles));
  if (base::Value* value = given.Find("value"))
    params.Set("arg", ToPage(*value));
  else
    params.Remove("arg");
  return true;
}

void Connection::Command(int id,
                         const std::string& guid,
                         const std::string& method,
                         base::DictValue params) {
  Object* object = Find(guid);
  if (!object) {
    // A handle disposed twice is no error, as its dispatcher stays upstream.
    if (method == "dispose") {
      Reply(id, base::Value(base::DictValue()));
      return;
    }
    ReplyError(id, "Target page, context or browser has been closed");
    return;
  }
  // addInitScript's exposeFunctions: the script finds Playwright's bindings
  // controller in the document (clientHelper.ts); here the exposed bindings
  // are functions of the window.
  if (method == "addInitScript") {
    if (const std::string* source = params.FindString("source");
        source && source->find("__playwright__binding__controller__") != std::string::npos) {
      params.Set("source", std::string(kBindingsController) + *source);
    }
  }
  const std::string type = object->type;
  auto reply_as_is = base::BindOnce(
      [](base::WeakPtr<Connection> self, int id, base::Value result) {
        if (self)
          self->Reply(id, std::move(result));
      },
      weak_factory_.GetWeakPtr(), id);
  auto empty_reply = [&]() { Reply(id, base::Value(base::DictValue())); };

  if (type == "Browser") {
    if (method == "newContext") {
      NewContext(id, std::move(params));
    } else if (method == "close") {
      for (const auto& [object_guid, value] : objects_) {
        if (value.type == "BrowserContext") {
          base::DictValue close;
          close.Set("context", value.id);
          zebkit_automation_call(nullptr, "BrowserContext.close",
                                 Json(base::Value(std::move(close))).c_str(), nullptr,
                                 nullptr);
        }
      }
      empty_reply();
    } else if (method == "defaultUserAgentForTest") {
      base::DictValue answer;
      answer.Set("userAgent", "");
      Reply(id, base::Value(std::move(answer)));
    } else {
      empty_reply();
    }
    return;
  }
  if (type == "BrowserContext") {
    if (method == "newPage") {
      NewPage(id, guid);
      return;
    }
    if (method == "close") {
      base::DictValue close;
      close.Set("context", object->id);
      Native(id, nullptr, "BrowserContext.close", std::move(close),
             base::BindOnce(
                 [](base::WeakPtr<Connection> self, int id, std::string guid, base::Value) {
                   if (!self)
                     return;
                   self->Event(guid, "close", base::DictValue());
                   self->Reply(id, base::Value(base::DictValue()));
                 },
                 weak_factory_.GetWeakPtr(), id, guid));
      return;
    }
    params.Set("context", object->id);
    if (method == "createTempFiles") {
      CreateTempFiles(id, std::move(params));
      return;
    }
    if (method == "exposeBinding" || method == "addInitScript") {
      Native(id, nullptr, "BrowserContext." + method, std::move(params),
             base::BindOnce(&Connection::ReplyDisposable, weak_factory_.GetWeakPtr(), id, guid));
      return;
    }
    Native(id, nullptr, "BrowserContext." + method, std::move(params), std::move(reply_as_is));
    return;
  }
  if (type == "APIRequestContext") {
    if (method == "fetch") {
      params.Set("context", object->id);
      PrepareFetch(params, object->data);
      const std::string* given_method = params.FindString("method");
      std::string request_method = given_method ? *given_method : "GET";
      const bool fail_on_status =
          params.FindBool("failOnStatusCode")
              .value_or(object->data.FindBool("failOnStatusCode").value_or(false));
      Native(id, nullptr, "APIRequestContext.fetch", std::move(params),
             base::BindOnce(
                 [](base::WeakPtr<Connection> self, int id, bool fail_on_status,
                    std::string request_method, base::Value result) {
                   if (!self)
                     return;
                   base::DictValue* response =
                       result.is_dict() ? result.GetDict().FindDict("response") : nullptr;
                   if (!response) {
                     self->ReplyError(id, "zebkit: no response");
                     return;
                   }
                   const int status = response->FindInt("status").value_or(0);
                   if (fail_on_status && (status < 200 || status >= 400)) {
                     const std::string* text = response->FindString("statusText");
                     self->ReplyError(id, base::NumberToString(status) + " " + (text ? *text : std::string()));
                     return;
                   }
                   const std::string uid = "fetch" + base::NumberToString(++self->next_fetch_);
                   if (const std::string* body = response->FindString("body"))
                     self->fetched_[uid] = *body;
                   // fetch.ts's log of the exchange, shown by toBeOK().
                   {
                     const std::string* url = response->FindString("url");
                     const std::string* text = response->FindString("statusText");
                     base::ListValue log;
                     log.Append("\xE2\x86\x92 " + request_method + " " + (url ? *url : std::string()));
                     log.Append("\xE2\x86\x90 " + base::NumberToString(status) + " " +
                                (text ? *text : std::string()));
                     self->fetch_logs_[uid] = std::move(log);
                   }
                   response->Remove("body");
                   response->Set("fetchUid", uid);
                   base::DictValue answer;
                   answer.Set("response", std::move(*response));
                   self->Reply(id, base::Value(std::move(answer)));
                 },
                 weak_factory_.GetWeakPtr(), id, fail_on_status, std::move(request_method)));
    } else if (method == "fetchResponseBody") {
      const std::string* uid = params.FindString("fetchUid");
      base::DictValue answer;
      if (uid && fetched_.contains(*uid))
        answer.Set("binary", fetched_[*uid]);
      Reply(id, base::Value(std::move(answer)));
    } else if (method == "fetchLog") {
      base::DictValue answer;
      const std::string* uid = params.FindString("fetchUid");
      auto log = uid ? fetch_logs_.find(*uid) : fetch_logs_.end();
      answer.Set("log", log != fetch_logs_.end() ? log->second.Clone() : base::ListValue());
      Reply(id, base::Value(std::move(answer)));
    } else if (method == "disposeAPIResponse") {
      if (const std::string* uid = params.FindString("fetchUid")) {
        fetched_.erase(*uid);
        fetch_logs_.erase(*uid);
      }
      empty_reply();
    } else if (method == "storageState") {
      base::DictValue state;
      state.Set("context", object->id);
      Native(id, nullptr, "BrowserContext.storageState", std::move(state),
             std::move(reply_as_is));
    } else if (method == "dispose" && object->data.FindBool("ownContext").value_or(false)) {
      // playwright.request.newContext()'s: its profile goes with it.
      base::DictValue close;
      close.Set("context", object->id);
      zebkit_automation_call(nullptr, "BrowserContext.close",
                             Json(base::Value(std::move(close))).c_str(), nullptr, nullptr);
      empty_reply();
      Dispose(guid);
    } else {
      empty_reply();
    }
    return;
  }
  if (type == "Playwright" && method == "newRequest") {
    NewRequest(id, guid, std::move(params));
    return;
  }
  if (type == "Tracing") {
    Tracing(id, guid, method, std::move(params));
    return;
  }
  if (type == "Artifact") {
    if (const std::string* path = object->data.FindString("path")) {
      // A trace's, a HAR's, a video's file.
      if (method == "pathAfterFinished") {
        base::DictValue answer;
        answer.Set("value", *path);
        Reply(id, base::Value(std::move(answer)));
      } else if (method == "saveAsStream") {
        // What a remote client saves: the file's bytes, read in chunks.
        base::ThreadPool::PostTaskAndReplyWithResult(
            FROM_HERE, {base::MayBlock()},
            base::BindOnce(
                [](base::FilePath from) {
                  std::string content;
                  return base::ReadFileToString(from, &content)
                             ? std::optional<std::string>(std::move(content))
                             : std::nullopt;
                },
                base::FilePath::FromUTF8Unsafe(*path)),
            base::BindOnce(
                [](base::WeakPtr<Connection> self, int id, std::string parent,
                   std::optional<std::string> content) {
                  if (!self)
                    return;
                  if (!content) {
                    self->ReplyError(id, "zebkit: the file could not be read");
                    return;
                  }
                  const std::string stream =
                      self->Create(parent, "Stream", base::DictValue());
                  self->streams_[stream] = std::move(*content);
                  base::DictValue answer;
                  answer.Set("stream", Ref(stream));
                  self->Reply(id, base::Value(std::move(answer)));
                },
                weak_factory_.GetWeakPtr(), id, guid));
      } else if (method == "saveAs") {
        const std::string* to = params.FindString("path");
        const base::FilePath from = base::FilePath::FromUTF8Unsafe(*path);
        const base::FilePath destination =
            base::FilePath::FromUTF8Unsafe(to ? *to : std::string());
        base::ThreadPool::PostTaskAndReplyWithResult(
            FROM_HERE, {base::MayBlock()},
            base::BindOnce(
                [](base::FilePath from, base::FilePath to) {
                  base::CreateDirectory(to.DirName());
                  return base::CopyFile(from, to);
                },
                from, destination),
            base::BindOnce(
                [](base::WeakPtr<Connection> self, int id, bool ok) {
                  if (!self)
                    return;
                  if (ok)
                    self->Reply(id, base::Value(base::DictValue()));
                  else
                    self->ReplyError(id, "zebkit: the file could not be copied");
                },
                weak_factory_.GetWeakPtr(), id));
      } else if (method == "failure") {
        empty_reply();
      } else if (method == "delete") {
        base::ThreadPool::PostTask(FROM_HERE, {base::MayBlock()},
                                   base::BindOnce(base::IgnoreResult(&base::DeleteFile),
                                                  base::FilePath::FromUTF8Unsafe(*path)));
        empty_reply();
      } else {
        empty_reply();
      }
      return;
    }
    // A download's.
    params.Set("download", object->id);
    Native(id, ViewOf(*object), "Artifact." + method, std::move(params),
           std::move(reply_as_is));
    return;
  }
  if (type == "WritableStream") {
    const std::string* path = object->data.FindString("path");
    if (!path) {
      ReplyError(id, "zebkit: not a file");
      return;
    }
    const base::FilePath file = base::FilePath::FromUTF8Unsafe(*path);
    auto answered = base::BindOnce(
        [](base::WeakPtr<Connection> self, int id) {
          if (self)
            self->Reply(id, base::Value());
        },
        weak_factory_.GetWeakPtr(), id);
    if (method == "write") {
      std::string bytes;
      const std::string* binary = params.FindString("binary");
      if (!binary || !base::Base64Decode(*binary, &bytes)) {
        ReplyError(id, "binary: expected base64");
        return;
      }
      files_->PostTaskAndReply(
          FROM_HERE,
          base::BindOnce(
              [](base::FilePath file, std::string bytes) {
                std::ignore = base::AppendToFile(file, bytes);
              },
              file, std::move(bytes)),
          std::move(answered));
    } else {
      const std::optional<double> modified = object->data.FindDouble("lastModifiedMs");
      files_->PostTaskAndReply(
          FROM_HERE,
          base::BindOnce(
              [](base::FilePath file, std::optional<double> modified) {
                if (modified) {
                  const base::Time time = base::Time::FromMillisecondsSinceUnixEpoch(*modified);
                  base::TouchFile(file, time, time);
                }
              },
              file, modified),
          std::move(answered));
    }
    return;
  }
  if (type == "Disposable") {
    Object* owner = Find(object->page);
    const std::string* token = object->data.FindString("registration");
    if (!owner || !token) {
      empty_reply();
      Dispose(guid);
      return;
    }
    base::DictValue remove;
    remove.Set("registration", *token);
    const bool context = owner->type == "BrowserContext";
    if (context)
      remove.Set("context", owner->id);
    Native(id, context ? nullptr : ViewOf(*owner),
           context ? "BrowserContext.removeRegistration" : "Page.removeRegistration",
           std::move(remove), base::BindOnce(
               [](base::WeakPtr<Connection> self, int id, std::string guid, base::Value) {
                 if (!self) return;
                 self->Reply(id, base::Value(base::DictValue()));
                 self->Dispose(guid);
               }, weak_factory_.GetWeakPtr(), id, guid));
    return;
  }
  if (type == "Stream") {
    std::string& content = streams_[guid];
    if (method == "read") {
      const size_t size =
          static_cast<size_t>(std::max(1, params.FindInt("size").value_or(1 << 20)));
      const size_t n = std::min(size, content.size());
      base::DictValue answer;
      answer.Set("binary", base::Base64Encode(base::as_byte_span(content).first(n)));
      content.erase(0, n);
      Reply(id, base::Value(std::move(answer)));
    } else {
      streams_.erase(guid);
      empty_reply();
      Dispose(guid);
    }
    return;
  }
  if (type == "Page" && (method == "consoleMessages" || method == "pageErrors" ||
                         method == "clearConsoleMessages" || method == "clearPageErrors" ||
                         method == "requests")) {
    PageRecords& records = page_records_[guid];
    const std::string* filter = params.FindString("filter");
    const bool all = filter && *filter == "all";
    base::DictValue answer;
    base::ListValue list;
    if (method == "consoleMessages") {
      for (size_t i = all ? 0 : records.console_mark; i < records.console.size(); ++i)
        list.Append(records.console[i].Clone());
      answer.Set("messages", std::move(list));
    } else if (method == "pageErrors") {
      for (size_t i = all ? 0 : records.errors_mark; i < records.errors.size(); ++i) {
        if (const base::DictValue* error = records.errors[i].FindDict("error")) {
          base::DictValue serialized;
          serialized.Set("error", error->Clone());
          list.Append(std::move(serialized));
        }
      }
      answer.Set("errors", std::move(list));
    } else if (method == "requests") {
      for (const std::string& request : records.requests) {
        if (Find(request))
          list.Append(Ref(request));
      }
      answer.Set("requests", std::move(list));
    } else if (method == "clearConsoleMessages") {
      records.console.clear();
      records.console_mark = 0;
    } else {
      records.errors.clear();
      records.errors_mark = 0;
    }
    Reply(id, base::Value(std::move(answer)));
    return;
  }
  zebkit_view* view = ViewOf(*object);
  if (!view) {
    if (type == "Android" || type == "Electron" || type == "BrowserType" ||
        type == "Debugger" || type == "Playwright") {
      if (method == "launch" || method == "connectOverCDP")
        ReplyError(id, "zebkit: this endpoint is a browser of its own: connect to it");
      else
        empty_reply();
      return;
    }
    ReplyError(id, "Target page, context or browser has been closed");
    return;
  }
  const std::string page_guid = type == "Page" ? guid : object->page;
  std::string frame_guid;
  std::string native = type + "." + method;
  if (type == "Frame") {
    params.Set("frame", object->id);
    frame_guid = guid;
  } else if (type == "ElementHandle" || type == "JSHandle") {
    params.Set("handle", object->id);
  } else if (type == "Route") {
    params.Set("route", object->id);
    if (const std::string* body = params.FindString("body"))
      params.Set("body", *body);
    if (method == "fulfill" && !params.FindBool("isBase64"))
      params.Set("isBase64", true);
  } else if (type == "Dialog") {
    params.Set("dialog", object->id);
  } else if (type == "BindingCall") {
    params.Set("binding", object->id);
    if (method == "resolve") {
      base::Value* result = params.Find("result");
      const base::Value* value =
          result && result->is_dict() ? result->GetDict().Find("value") : nullptr;
      params.Set("result", value ? Plain(*value) : base::Value());
    } else if (method == "reject") {
      const std::string* message =
          params.FindStringByDottedPath("error.error.message");
      params.Set("error", message ? *message : "Error");
    }
  } else if (type == "WebSocketRoute") {
    params.Set("webSocketRoute", object->id);
  } else if (type == "Worker") {
    params.Set("worker", object->id);
    if (base::Value* arg = params.Find("arg"); arg && arg->is_dict()) {
      if (base::Value* value = arg->GetDict().Find("value"))
        params.Set("arg", Plain(*value));
    }
    Native(id, view, "Worker.evaluateExpression", std::move(params),
           base::BindOnce(
               [](base::WeakPtr<Connection> self, int id, base::Value result) {
                 if (!self)
                   return;
                 base::Value* value = result.is_dict() ? result.GetDict().Find("value") : nullptr;
                 base::DictValue answer;
                 answer.Set("value", value ? Serialize(*value) : Serialize(base::Value()));
                 self->Reply(id, base::Value(std::move(answer)));
               },
               weak_factory_.GetWeakPtr(), id));
    return;
  } else if (type == "Request") {
    base::DictValue answer;
    if (method == "response") {
      if (const std::string* response = object->data.FindString("response"))
        answer.Set("response", Ref(*response));
    } else if (method == "rawRequestHeaders") {
      answer.Set("headers", object->data.FindList("headers") ? object->data.FindList("headers")->Clone() : base::ListValue());
    }
    Reply(id, base::Value(std::move(answer)));
    return;
  } else if (type == "Response") {
    if (method == "body") {
      base::DictValue body;
      body.Set("request", object->id);
      Native(id, view, "Response.body", std::move(body), std::move(reply_as_is));
      return;
    }
    base::DictValue answer;
    if (method == "rawResponseHeaders") {
      answer.Set("headers", object->data.FindList("headers") ? object->data.FindList("headers")->Clone() : base::ListValue());
    } else if (method == "httpVersion") {
      const std::string* version = object->data.FindString("httpVersion");
      answer.Set("value", version ? "HTTP/" + *version : "HTTP/1.1");
    } else if (method == "sizes") {
      base::DictValue sizes;
      for (const char* key : {"requestBodySize", "requestHeadersSize", "responseBodySize",
                              "responseHeadersSize"}) {
        sizes.Set(key, 0);
      }
      answer.Set("sizes", object->data.FindDict("sizes") ? object->data.FindDict("sizes")->Clone() : std::move(sizes));
    }
    Reply(id, base::Value(std::move(answer)));
    return;
  } else if (type == "Page" && method == "setNetworkInterceptionPatterns") {
    const base::ListValue* patterns = params.FindList("patterns");
    object->data.Set("routes", patterns && !patterns->empty());
  }

  // Evaluations: Playwright's serialized values end to end.
  static const base::NoDestructor<std::set<std::string>> kEvaluations(
      std::set<std::string>{"evaluateExpression", "evaluateExpressionHandle",
                            "evalOnSelector", "evalOnSelectorAll", "waitForFunction",
                            "jsonValue"});
  const bool evaluation = kEvaluations->contains(method);
  if (evaluation && !ToNativeArgs(params)) {
    // In the words of the dispatchers' validator.
    ReplyError(id, "arg.handles: no object with guid");
    return;
  }
  if (method == "goto") {
    // frames.ts's constructURLBasedOnBaseURL().
    Object* page = Find(page_guid);
    Object* context = page ? Find(page->context) : nullptr;
    const std::string* base_url = context ? context->data.FindString("baseURL") : nullptr;
    const std::string* given = params.FindString("url");
    if (base_url && given) {
      const GURL resolved = GURL(*base_url).Resolve(*given);
      if (resolved.is_valid())
        params.Set("url", resolved.spec());
    }
  }
  if (method == "setInputFiles") {
    // A remote client's files, written to the engine's (createTempFiles).
    if (base::ListValue* streams = params.FindList("streams")) {
      base::ListValue paths;
      for (const base::Value& stream : *streams) {
        const std::string* stream_guid = stream.is_dict() ? stream.GetDict().FindString("guid") : nullptr;
        Object* file = stream_guid ? Find(*stream_guid) : nullptr;
        const std::string* path = file ? file->data.FindString("path") : nullptr;
        if (path)
          paths.Append(*path);
      }
      params.Remove("streams");
      params.Set("localPaths", std::move(paths));
    }
    if (const base::DictValue* directory = params.FindDict("directoryStream")) {
      const std::string* stream_guid = directory->FindString("guid");
      Object* file = stream_guid ? Find(*stream_guid) : nullptr;
      const std::string* path = file ? file->data.FindString("path") : nullptr;
      const std::string local = path ? *path : std::string();
      params.Remove("directoryStream");
      params.Set("localDirectory", local);
    }
  }
  if (method == "selectOption") {
    if (base::ListValue* elements = params.FindList("elements")) {
      base::ListValue handles;
      for (const base::Value& element : *elements) {
        const std::string* element_guid =
            element.is_dict() ? element.GetDict().FindString("guid") : nullptr;
        Object* handle = element_guid ? Find(*element_guid) : nullptr;
        if (handle)
          handles.Append(handle->id);
      }
      params.Set("elements", std::move(handles));
    }
  }
  bool is_not = false;
  if (method == "expect") {
    is_not = params.FindBool("isNot").value_or(false);
    // frames.ts checks once whatever the timeout (timeout: 1 passes when it
    // already holds), then retries until it: the step keeps that time, the
    // command's own limit only guards against a step that never answers.
    if (const std::optional<double> timeout = params.FindDouble("timeout");
        timeout && *timeout > 0) {
      params.Set("expectTimeout", *timeout);
      params.Set("timeout", *timeout + 2000);
    }
    // A serialized value, which the step makes the value it was (a Date, a
    // URL, undefined), as frameDispatcher.ts's parseArgument().
    if (base::Value* expected = params.Find("expectedValue"); expected && expected->is_dict()) {
      if (base::Value* value = expected->GetDict().Find("value")) {
        params.Set("expectedValue", ToPage(*value));
        params.Set("expectedValueSerialized", true);
      }
    }
  }
  const bool navigation = method == "goto" || method == "reload" ||
                          method == "goBack" || method == "goForward";
  const bool disposes = method == "dispose";
  Native(id, view, native, std::move(params),
         base::BindOnce(
             [](base::WeakPtr<Connection> self, int id, std::string page_guid,
                std::string frame_guid, std::string guid, std::string method,
                bool navigation, bool disposes, bool is_not, base::Value result) {
               if (!self)
                 return;
               if (disposes) {
                 self->Reply(id, base::Value(base::DictValue()));
                 self->Dispose(guid);
                 return;
               }
               if (navigation && result.is_dict()) {
                 if (base::DictValue* response = result.GetDict().FindDict("response")) {
                   // Chromium has no response for these, as Network has no event.
                   const std::string* response_url = response->FindString("url");
                   if (response_url && (response_url->starts_with("about:") ||
                                        response_url->starts_with("data:"))) {
                     self->Reply(id, base::Value(base::DictValue()));
                     return;
                   }
                   base::DictValue request = response->Clone();
                   Object* frame = self->Find(frame_guid);
                   if (frame)
                     request.Set("frame", frame->id);
                   request.Set("resourceType", "document");
                   const std::string request_guid = self->RequestGuid(page_guid, request);
                   const std::string response_guid =
                       self->ResponseGuid(page_guid, request_guid, *response);
                   base::DictValue answer;
                   answer.Set("response", Ref(response_guid));
                   self->Reply(id, base::Value(std::move(answer)));
                   return;
                 }
               }
               // frames.ts's expect(): nothing when it holds, else ExpectError.
               if (method == "expect" && result.is_dict()) {
                 const base::DictValue& answer = result.GetDict();
                 if (answer.FindBool("matches").value_or(false) != is_not) {
                   self->Reply(id, base::Value(base::DictValue()));
                   return;
                 }
                 base::DictValue details;
                 if (const base::Value* received = answer.Find("received")) {
                   base::DictValue out;
                   if (received->is_dict()) {
                     if (const base::Value* value = received->GetDict().Find("value"))
                       out.Set("value", ToProtocol(*value));
                     if (const std::string* aria = received->GetDict().FindString("ariaSnapshot"))
                       out.Set("ariaSnapshot", *aria);
                   }
                   details.Set("received", std::move(out));
                 }
                 if (answer.FindBool("timedOut").value_or(false))
                   details.Set("timedOut", true);
                 if (const std::string* error = answer.FindString("errorMessage"))
                   details.Set("customErrorMessage", *error);
                 const base::ListValue* log = answer.FindList("log");
                 self->ReplyExpectFailed(id, std::move(details), log ? log->Clone() : base::ListValue());
                 return;
               }
               if (method == "exposeBinding" || method == "addInitScript") {
                 self->ReplyDisposable(id, guid, std::move(result));
                 return;
               }
               if (method == "contentFrame" || method == "ownerFrame") {
                 base::DictValue answer;
                 if (const std::string* frame =
                         result.is_dict() ? result.GetDict().FindString("frame") : nullptr) {
                   answer.Set("frame", Ref(self->FrameGuid(page_guid, *frame)));
                 }
                 self->Reply(id, base::Value(std::move(answer)));
                 return;
               }
               // A value's answer in the protocol's form.
               if ((method == "evaluateExpression" || method == "evalOnSelector" ||
                    method == "evalOnSelectorAll" || method == "jsonValue") &&
                   result.is_dict()) {
                 if (base::Value* value = result.GetDict().Find("value"))
                   result.GetDict().Set("value", ToProtocol(*value));
               }
               self->Reply(id, self->Expose(page_guid, frame_guid, std::move(result)));
             },
             weak_factory_.GetWeakPtr(), id, page_guid, frame_guid, guid, method,
             navigation, disposes, is_not));
}

// ---- The socket -------------------------------------------------------------

// On the endpoint's own thread: the HTTP server and its WebSockets, each
// message to and from the engine's thread.
class Socket : public net::HttpServer::Delegate {
 public:
  using Received = base::RepeatingCallback<void(int connection, std::string message)>;
  using Opened = base::RepeatingCallback<void(int connection, bool open)>;

  Socket(scoped_refptr<base::SequencedTaskRunner> ui, Received received, Opened opened)
      : ui_(std::move(ui)), received_(std::move(received)), opened_(std::move(opened)) {}

  int Listen(int port) {
    auto socket = std::make_unique<net::TCPServerSocket>(nullptr, net::NetLogSource());
    if (socket->ListenWithAddressAndPort("127.0.0.1", static_cast<uint16_t>(port), 10) != net::OK)
      return 0;
    server_ = std::make_unique<net::HttpServer>(std::move(socket), this);
    net::IPEndPoint address;
    if (server_->GetLocalAddress(&address) != net::OK)
      return 0;
    return address.port();
  }

  void Send(int connection, std::string message) {
    if (server_)
      server_->SendOverWebSocket(connection, message, kAnnotation);
  }

  // net::HttpServer::Delegate
  void OnConnect(int connection) override {}
  void OnHttpRequest(int connection, const net::HttpServerRequestInfo&) override {
    server_->Send404(connection, kAnnotation);
  }
  void OnWebSocketRequest(int connection,
                          const net::HttpServerRequestInfo& request) override {
    server_->AcceptWebSocket(connection, request, kAnnotation);
    ui_->PostTask(FROM_HERE, base::BindOnce(opened_, connection, true));
  }
  void OnWebSocketMessage(int connection, std::string data) override {
    ui_->PostTask(FROM_HERE, base::BindOnce(received_, connection, std::move(data)));
  }
  void OnClose(int connection) override {
    ui_->PostTask(FROM_HERE, base::BindOnce(opened_, connection, false));
  }

 private:
  scoped_refptr<base::SequencedTaskRunner> ui_;
  Received received_;
  Opened opened_;
  std::unique_ptr<net::HttpServer> server_;
};

// The engine's thread's side: the clients.
class Endpoint {
 public:
  static Endpoint* Get() {
    static base::NoDestructor<Endpoint> endpoint;
    return endpoint.get();
  }

  void Listen(int port, zebkit_automation_listen_func func, void* data) {
    if (thread_) {
      const std::string url = Url();
      if (func)
        func(url.c_str(), nullptr, data);
      return;
    }
    thread_ = std::make_unique<base::Thread>("zebkit-playwright");
    base::Thread::Options options(base::MessagePumpType::IO, 0);
    if (!thread_->StartWithOptions(std::move(options))) {
      thread_.reset();
      if (func)
        func(nullptr, "zebkit: the endpoint's thread could not start", data);
      return;
    }
    socket_ = new Socket(
        base::SequencedTaskRunner::GetCurrentDefault(),
        base::BindRepeating(&Endpoint::Received, base::Unretained(this)),
        base::BindRepeating(&Endpoint::Opened, base::Unretained(this)));
    thread_->task_runner()->PostTaskAndReplyWithResult(
        FROM_HERE,
        base::BindOnce([](Socket* socket, int port) { return socket->Listen(port); },
                       socket_, port),
        base::BindOnce(
            [](Endpoint* self, zebkit_automation_listen_func func, void* data, int port) {
              self->port_ = port;
              if (!func)
                return;
              if (!port) {
                func(nullptr, "zebkit: the endpoint could not listen", data);
                return;
              }
              const std::string url = self->Url();
              func(url.c_str(), nullptr, data);
            },
            base::Unretained(this), func, data));
  }

 private:
  std::string Url() const {
    return "ws://127.0.0.1:" + base::NumberToString(port_) + "/";
  }

  void Opened(int connection, bool open) {
    if (!open) {
      connections_.erase(connection);
      return;
    }
    connections_[connection] = std::make_unique<Connection>(base::BindRepeating(
        [](base::Thread* thread, Socket* socket, int connection, std::string message) {
          thread->task_runner()->PostTask(
              FROM_HERE, base::BindOnce(&Socket::Send, base::Unretained(socket),
                                        connection, std::move(message)));
        },
        base::Unretained(thread_.get()), base::Unretained(socket_), connection));
  }

  void Received(int connection, std::string message) {
    auto found = connections_.find(connection);
    if (found != connections_.end())
      found->second->Received(message);
  }

  std::unique_ptr<base::Thread> thread_;
  // Lives on the thread, as long as the engine.
  raw_ptr<Socket> socket_ = nullptr;
  int port_ = 0;
  std::map<int, std::unique_ptr<Connection>> connections_;
};

}  // namespace

}  // namespace zebkit

void zebkit_automation_listen(int port, zebkit_automation_listen_func func, void* data) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  zebkit::Endpoint::Get()->Listen(port, func, data);
}
