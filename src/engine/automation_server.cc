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
#include "build/build_config.h"
#include "zebkit/engine/playwright/protocol_metainfo.h"
#include "third_party/zlib/google/zip.h"
#include "base/hash/sha1.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/escape.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/threading/thread.h"
#include "base/unguessable_token.h"
#include "base/values.h"
#include "components/version_info/version_info.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/devtools_agent_host_client.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/log/net_log_source.h"
#include "net/http/http_status_code.h"
#include "net/server/http_server.h"
#include "net/server/http_server_request_info.h"
#include "net/socket/tcp_server_socket.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "url/gurl.h"
#include "zebkit/engine/automation_context.h"
#include "zebkit/engine/automation_network.h"
#include "zebkit/engine/automation_trace.h"
#include "zebkit/engine/view.h"
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
  // An ArrayBuffer is an object without keys once Playwright's server has
  // parsed it: its protocol has no form of its own for one.
  if (dict.Find("ab")) {
    base::DictValue empty;
    empty.Set("o", base::ListValue());
    empty.Set("id", 0);
    return base::Value(std::move(empty));
  }
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

// The Playwright release whose protocol this endpoint speaks, which a HAR it
// makes names as its creator (tests/playwright-suite runs that release's tests).
constexpr char kPlaywrightVersion[] = "1.64.1";

// The id of @view's main frame, as automation's events name frames.
std::string FrameIdOfMain(zebkit_view* view) {
  content::WebContents* contents = view ? view->web_contents() : nullptr;
  return contents ? "f" + base::NumberToString(
                              contents->GetPrimaryMainFrame()->GetFrameTreeNodeId().value())
                  : std::string();
}

// ---- CDP sessions ----------------------------------------------------------

// A CDPSession object's DevTools session (Browser.newBrowserCDPSession,
// BrowserContext.newCDPSession): Chromium's own DevTools in this process, the
// browser's target or a page's or a frame's, as Playwright's server attaches
// to them over its pipe. Trusted, as the pipe Playwright launches Chrome with.
class CdpClient : public content::DevToolsAgentHostClient {
 public:
  // Each message the target sends: an answer (with "id") or an event.
  using MessageFunc = base::RepeatingCallback<void(base::DictValue)>;
  using ClosedFunc = base::OnceClosure;

  CdpClient(scoped_refptr<content::DevToolsAgentHost> host,
            MessageFunc on_message,
            ClosedFunc on_closed)
      : host_(std::move(host)),
        on_message_(std::move(on_message)),
        on_closed_(std::move(on_closed)) {}
  ~CdpClient() override {
    if (host_)
      host_->DetachClient(this);
  }

  bool Attach() { return host_ && host_->AttachClient(this); }

  // The id of the next message: its answer may come inside Send().
  int NextId() { return next_id_ + 1; }

  // Sends @method with @params; the answer comes to on_message with the id
  // returned.
  int Send(const std::string& method, base::DictValue params) {
    base::DictValue message;
    const int id = ++next_id_;
    message.Set("id", id);
    message.Set("method", method);
    message.Set("params", std::move(params));
    const std::string json = Json(base::Value(std::move(message)));
    if (host_)
      host_->DispatchProtocolMessage(this, base::as_byte_span(json));
    return id;
  }

  void Detach() {
    if (scoped_refptr<content::DevToolsAgentHost> host = std::move(host_))
      host->DetachClient(this);
  }

  // content::DevToolsAgentHostClient
  void DispatchProtocolMessage(content::DevToolsAgentHost*,
                               base::span<const uint8_t> message) override {
    std::optional<base::Value> value =
        Parse(std::string(base::as_string_view(message)));
    if (value && value->is_dict())
      on_message_.Run(std::move(value->GetDict()));
  }
  void AgentHostClosed(content::DevToolsAgentHost*) override {
    host_ = nullptr;
    if (on_closed_)
      std::move(on_closed_).Run();
  }
  bool IsTrusted() override { return true; }
  bool MayReadLocalFiles() override { return true; }
  bool MayWriteLocalFiles() override { return true; }
  bool AllowUnsafeOperations() override { return true; }

 private:
  scoped_refptr<content::DevToolsAgentHost> host_;
  MessageFunc on_message_;
  ClosedFunc on_closed_;
  int next_id_ = 0;
};

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
    // Popups not yet reported, which may be gone.
    for (const std::string& page : prepared_) {
      zebkit_view* view = zebkit_automation_page(page.c_str());
      if (view && !pages_.contains(view)) {
        zebkit_view_on_window_open(view, nullptr, nullptr);
        zebkit_automation_on_event(view, nullptr, nullptr);
      }
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
    // An aborted call's: answered already.
    if (!ids_.contains(id))
      return;
    aborted_.erase(id);
    base::DictValue message;
    message.Set("id", TakeId(id));
    // The protocol's optional fields are absent, never null: the client's
    // validator throws on null, in a place where its call then never ends.
    RemoveNulls(result);
    base::Value traced_result = result.is_none() ? base::Value() : result.Clone();
    if (!result.is_none())
      message.Set("result", std::move(result));
    Answered(id, std::move(message), std::string(), std::move(traced_result));
  }

  void ReplyError(int id, const std::string& error, const std::string& name = std::string()) {
    if (!ids_.contains(id))
      return;
    // An aborted call: an AbortError, whatever its native answer says.
    const bool aborted = aborted_.erase(id) > 0;
    base::DictValue inner;
    std::string text = error;
    base::ListValue log;
    if (const size_t at = error.find("\nCall log:"); at != std::string::npos && aborted) {
      // The log apart, as dispatcher.ts sends it.
      text = error.substr(0, at);
      for (const std::string& line : base::SplitString(error.substr(at + 10), "\n",
                                                       base::KEEP_WHITESPACE,
                                                       base::SPLIT_WANT_NONEMPTY)) {
        log.Append(line);
      }
    }
    inner.Set("message", text);
    inner.Set("name", aborted || name == "AbortError" ? "AbortError"
                      : error.rfind("Timeout ", 0) == 0 ? "TimeoutError" : "Error");
    inner.Set("stack", "");
    base::DictValue outer;
    outer.Set("error", std::move(inner));
    base::DictValue message;
    message.Set("id", TakeId(id));
    if (!log.empty())
      message.Set("log", std::move(log));
    message.Set("error", std::move(outer));
    Answered(id, std::move(message), text.empty() ? std::string("Error") : text, base::Value());
  }

  // Frame.expect's failure, as Playwright's ExpectError reaches its client:
  // the protocol's errorDetails, and the call's log.
  void ReplyExpectFailed(int id, base::DictValue details, base::ListValue raw) {
    std::vector<std::string> lines;
    for (const base::Value& line : raw) {
      if (!line.is_string())
        continue;
      // The step's lines come as the client shows them ("  - line").
      std::string text = line.GetString();
      if (text.starts_with("  - "))
        text = text.substr(4);
      lines.push_back(std::move(text));
    }
    // As dispatcher.ts sends it: compressed, each line as the client shows it.
    base::ListValue log;
    for (const std::string& line : CompressCallLog(lines))
      log.Append(line);
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
    Answered(id, std::move(message), "Expect failed", base::Value());
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
  void PreparePage(zebkit_view* view, const std::string& context_guid);
  void ReportPopup(zebkit_view* view);
  // Popups not reported yet, and the events they had meanwhile.
  struct Popup {
    std::string context;
    std::string opener;
    std::string page;
    bool reporting = false;
    std::string frames;
    std::vector<std::pair<std::string, base::DictValue>> held;
  };
  std::map<zebkit_view*, Popup> popups_;
  // Pages whose PreparePage() has run, by their page id (a view's address
  // may be another's later).
  std::set<std::string> prepared_;
  void PageEvent(const std::string& page_guid, const std::string& event,
                 base::DictValue params);
  void Tracing(int id, const std::string& guid, const std::string& method,
               base::DictValue params);
  void CreateTempFiles(int id, base::DictValue params);
  bool ToNativeArgs(base::DictValue& params);
  // CDPSession: a session of @host's, created in @parent's scope.
  void NewCDPSession(int id, const std::string& parent,
                     scoped_refptr<content::DevToolsAgentHost> host);
  void CDPSessionCommand(int id, const std::string& guid, const std::string& method,
                         base::DictValue params);
  void CDPSessionMessage(const std::string& guid, base::DictValue message);
  void CDPSessionClosed(const std::string& guid);

  static void OnEvent(zebkit_view* view, const char* event, const char* json,
                      void* data);

  SendFunc send_;
  std::map<std::string, Object> objects_;
  std::map<std::string, std::string> known_;
  std::map<zebkit_view*, std::string> pages_;
  std::map<std::string, std::string> fetched_;
  std::map<std::string, base::ListValue> fetch_logs_;
  // CDPSession objects' sessions, and their commands in flight: the session's
  // message id to the client's call and the method.
  struct CdpSession {
    std::unique_ptr<CdpClient> client;
    std::map<int, std::pair<int, std::string>> calls;
  };
  std::map<std::string, CdpSession> cdp_sessions_;
  // HAR recordings (Tracing.harStart), as harRecorder.ts makes them in
  // Playwright's server: of a context's pages or of one page, and of its API
  // requests; by harId.
  struct Har {
    std::string context;
    std::string page;
    std::unique_ptr<HarRecorder> recorder;
  };
  std::map<std::string, Har> hars_;
  void HarEvent(const std::string& page_guid, const std::string& event,
                const base::DictValue& params);

  // ---- Traces, as Playwright's server records them (trace/recorder/tracing.ts):
  // the protocol's calls of a context's objects, before and after, with DOM
  // snapshots, console, page events, the network as HAR, screencast frames.
  struct Trace {
    base::DictValue options;
    std::string name;
    int chunk = 0;
    bool recording = false;
    bool snapshot_reset = true;
    std::vector<std::string> lines;
    std::map<std::string, std::string> chunk_files;
    std::map<std::string, std::string> cross_files;
    std::unique_ptr<HarRecorder> network;
    std::vector<std::string> groups;
    base::TimeTicks last_frame;
  };
  std::map<std::string, Trace> traces_;
  struct TracedCall {
    std::string context;
    std::string call_id;
    std::string page;
    bool snapshot_after = false;
  };
  std::map<int, TracedCall> traced_calls_;
  const base::TimeTicks origin_ = base::TimeTicks::Now();
  double Monotonic() const { return (base::TimeTicks::Now() - origin_).InMillisecondsF(); }
  std::string ContextOf(const std::string& guid);
  std::string PageOf(const std::string& guid);
  base::Value TraceValue(const base::Value& value);
  void TraceLine(Trace& trace, base::DictValue event);
  bool BeforeCall(int id, const std::string& guid, const std::string& method,
                  const base::DictValue& params, const base::DictValue* metadata,
                  base::OnceClosure run);
  void Answered(int id, base::DictValue message, const std::string& error, base::Value result);
  void CaptureTraceSnapshot(const std::string& context, const std::string& page,
                            const std::string& call_id, const std::string& phase,
                            base::OnceClosure then);
  void TraceScreencastFrame(const std::string& context, const std::string& page);
  void TraceEvent(const std::string& page_guid, const std::string& event,
                  const base::DictValue& params);
  void TraceCommand(int id, const std::string& context, const std::string& method,
                    base::DictValue params);
  void HarExport(int id, const std::string& har_id, const std::string& mode,
                 const std::string& parent);
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
  // Calls an AbortSignal ended, until their native answer comes.
  std::set<int> aborted_;
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
  // An AbortSignal's: the call it names fails at once with an AbortError, as
  // dispatcher.ts aborts its progress; its own answer later is dropped.
  if (*method == "__abort__") {
    --next_id_;
    ids_.erase(id);
    const base::Value* call_id = params ? params->Find("id") : nullptr;
    const std::string* reason = params ? params->FindString("reason") : nullptr;
    for (auto it = ids_.begin(); call_id && it != ids_.end(); ++it) {
      if (it->second != *call_id)
        continue;
      const int aborted = it->first;
      aborted_.insert(aborted);
      base::DictValue abort;
      abort.Set("abortId", "a" + base::NumberToString(aborted));
      abort.Set("reason", reason ? *reason : std::string());
      zebkit_automation_call(nullptr, "Automation.abort", Json(base::Value(std::move(abort))).c_str(),
                             nullptr, nullptr);
      // The native call answered it if it was one; else it is answered here.
      if (ids_.contains(aborted))
        ReplyError(aborted, reason ? *reason : "The operation was aborted", "AbortError");
      break;
    }
    return;
  }
  base::DictValue command_params = params ? std::move(*params) : base::DictValue();
  command_params.Set("abortId", "a" + base::NumberToString(id));
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
  base::OnceClosure run = base::BindOnce(
      [](base::WeakPtr<Connection> self, int id, std::string guid, std::string method,
         base::DictValue params) {
        if (self)
          self->Command(id, guid, method, std::move(params));
      },
      weak_factory_.GetWeakPtr(), id, *guid, *method, command_params.Clone());
  if (!traces_.empty()) {
    base::DictValue traced = params ? command_params.Clone() : base::DictValue();
    traced.Remove("abortId");
    traced.Remove("expectTitle");
    if (BeforeCall(id, *guid, *method, traced, dict.FindDict("metadata"), std::move(run)))
      return;
    // Not traced: run was not taken.
    run = base::BindOnce(
        [](base::WeakPtr<Connection> self, int id, std::string guid, std::string method,
           base::DictValue params) {
          if (self)
            self->Command(id, guid, method, std::move(params));
        },
        weak_factory_.GetWeakPtr(), id, *guid, *method, std::move(command_params));
  }
  std::move(run).Run();
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
        if (error && pending->id < 0) {
          // The connection's own (a trace's snapshot): its continuation runs
          // anyway, with {error}, as a command waits on it.
          base::DictValue failed;
          failed.Set("error", error);
          LOG(WARNING) << "zebkit: trace capture failed: " << error;
          std::move(pending->then).Run(base::Value(std::move(failed)));
          return;
        }
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
      // Of its own frame (a handle found through frames), else the call's.
      std::string parent = frame;
      if (const std::string* own = dict.FindString("handleFrame")) {
        const std::string known = Known("frame", page, *own);
        if (!known.empty())
          parent = known;
      }
      guid = Create(parent.empty() ? page : parent,
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
  // A new page's about:blank has loaded by the time a client has the page, as
  // Page.createTarget's does; its events, which come later here, are not sent.
  const bool loaded = initial && initial->FindBool("initialBlank").value_or(false);
  base::ListValue load_states;
  if (loaded) {
    for (const char* state : {"commit", "domcontentloaded", "load", "networkidle"})
      load_states.Append(state);
  }
  initializer.Set("loadStates", std::move(load_states));
  Object object;
  object.id = frame_id;
  object.page = page_guid;
  if (loaded) {
    object.data.Set("initialBlank", true);
    for (const char* state : {"domcontentloaded", "load", "networkidle"})
      object.data.Set(std::string("state.") + state, true);
  }
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
  if (const std::string* post_data = request.FindString("postData"))
    initializer.Set("postData", *post_data);
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
  if (request) {
    if (const base::ListValue* raw = request->data.FindList("rawResponseHeaders"))
      object.data.Set("rawHeaders", raw->Clone());
  }
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
  // network.ts's constructURLBasedOnBaseURL().
  if (const std::string* base_url = defaults.FindString("baseURL")) {
    const GURL resolved = GURL(*base_url).Resolve(url);
    if (resolved.is_valid())
      url = resolved.spec();
  }
  if (!params.FindBool("ignoreHTTPSErrors") &&
      defaults.FindBool("ignoreHTTPSErrors").value_or(false)) {
    params.Set("ignoreHTTPSErrors", true);
  }
  if (const base::Value* credentials = defaults.Find("httpCredentials"))
    params.Set("httpCredentials", credentials->Clone());
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
               self->Find(tracing)->context = request;
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
               // context.request's own (tracing.spec.ts: recorded apart).
               Object request_tracing_object;
               request_tracing_object.id = *context;
               const std::string request_tracing =
                   self->Create(self->browser_, "Tracing", base::DictValue(),
                                std::move(request_tracing_object));
               base::DictValue request_init;
               request_init.Set("tracing", Ref(request_tracing));
               // page.request's defaults: the context's options, as
               // BrowserContextAPIRequestContext._defaultOptions().
               Object request_object;
               request_object.id = *context;
               if (const std::string* base_url = options.FindString("baseURL"))
                 request_object.data.Set("baseURL", *base_url);
               if (options.FindBool("ignoreHTTPSErrors").value_or(false))
                 request_object.data.Set("ignoreHTTPSErrors", true);
               if (const base::Value* credentials = options.Find("httpCredentials"))
                 request_object.data.Set("httpCredentials", credentials->Clone());
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
               object.data.Set("contextOptions", options.Clone());
               initializer.Set("options", std::move(options));
               const std::string guid = self->Create(self->browser_, "BrowserContext",
                                                     std::move(initializer), std::move(object));
               self->Find(tracing)->context = guid;
               self->Find(request)->context = guid;
               self->Find(request_tracing)->context = request;
               base::DictValue answer;
               answer.Set("context", Ref(guid));
               // IndexedDB and OPFS of storageState's origins, put back in a
               // page of each (browserContext.ts's setStorageState()).
               const base::ListValue* origins =
                   self->Find(guid)->data.FindListByDottedPath("contextOptions.storageState.origins");
               if (origins && !origins->empty()) {
                 base::DictValue restore;
                 restore.Set("context", *context);
                 restore.Set("origins", origins->Clone());
                 self->Native(id, nullptr, "BrowserContext.restoreStorage", std::move(restore),
                              base::BindOnce(
                                  [](base::WeakPtr<Connection> self, int id, base::DictValue answer,
                                     base::Value) {
                                    if (self)
                                      self->Reply(id, base::Value(std::move(answer)));
                                  },
                                  self, id, std::move(answer)));
                 return;
               }
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

// navigator.webdriver, as Chromium under Playwright's --enable-automation: a
// page a Playwright client drives says so. A host's own views do not, and this
// is what opens a DevTools session on the page.
// What a page of the context has from its start, before the client knows it
// (a popup is reported once its first document commits): its popups, and a
// trace or a HAR the context started, which record its first requests.
void Connection::PreparePage(zebkit_view* view, const std::string& context_guid) {
  char* page_id = zebkit_automation_page_id(view);
  const std::string page = page_id ? page_id : "";
  zebkit_free(page_id);
  if (page.empty() || !prepared_.insert(page).second)
    return;
  // Playwright launches Chromium with --disable-popup-blocking; its popups
  // are pages of the context, with their opener.
  zebkit_view_set_popup_policy(view, ZEBKIT_POPUP_POLICY_ALLOW);
  zebkit_view_on_window_open(view, &Connection::OnWindowOpen, this);
  if (Object* context = Find(context_guid)) {
    for (const char* key : {"tracingStart", "tracingChunk"}) {
      const base::DictValue* started = context->data.FindDict(key);
      if (!started)
        continue;
      const std::string method = std::string(key) == "tracingStart" ? "Tracing.tracingStart"
                                                                    : "Tracing.tracingStartChunk";
      zebkit_automation_call(view, method.c_str(), Json(base::Value(started->Clone())).c_str(),
                             nullptr, nullptr);
    }
  }
}

static void MarkAutomated(zebkit_view* view) {
  if (!view)
    return;
  base::DictValue params;
  params.Set("enabled", true);
  view->Send("Emulation.setAutomationOverride", std::move(params));
}

std::string Connection::AdoptPage(const std::string& context_guid,
                                  const std::string& page_id,
                                  const base::Value& frames,
                                  const std::string& opener) {
  zebkit_view* view = zebkit_automation_page(page_id.c_str());
  MarkAutomated(view);
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
      base::DictValue known_frame = frame.GetDict().Clone();
      if (opener.empty() && !known_frame.FindString("parentFrame"))
        known_frame.Set("initialBlank", true);
      const std::string guid = FrameGuid(reserved, *id, &known_frame);
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
  PreparePage(view, context_guid);
  for (auto& [har_id, har] : hars_) {
    if (har.context == context_guid && har.page.empty())
      har.recorder->AddPage(reserved);
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
  // As soon as the popup is there, before its first document runs.
  MarkAutomated(opened);
  // Anew: a view's address may be one closed before it was reported.
  Popup& popup = self->popups_[opened] = Popup();
  popup.context = page->context;
  popup.opener = found->second;
  popup.page = id;
  zebkit_free(id);
  // crPage.ts reports a page that is to load a document once it has
  // committed it: its url is the document's.
  const std::string target = url ? url : "";
  const bool wait = !target.empty() && target != "about:blank";
  // Its context's settings at once (the first command gives them), what the
  // context records, and its events, held until it is reported.
  self->PreparePage(opened, page->context);
  zebkit_automation_on_event(opened, &Connection::OnEvent, self);
  struct Pending {
    base::WeakPtr<Connection> connection;
    bool wait;
  };
  zebkit_automation_call(
      opened, "Page.frames", "{}",
      [](zebkit_view* view, const char* json, const char*, void* data) {
        std::unique_ptr<Pending> pending(static_cast<Pending*>(data));
        if (!pending->connection || !json)
          return;
        // What it is reported with if it is gone by then.
        if (auto popup = pending->connection->popups_.find(view);
            popup != pending->connection->popups_.end()) {
          popup->second.frames = json;
        }
        if (!pending->wait) {
          pending->connection->ReportPopup(view);
          return;
        }
        zebkit_automation_call(
            view, "Page.waitForInitialNavigation", "{}",
            [](zebkit_view* view, const char* json, const char*, void* data) {
              auto connection = std::unique_ptr<base::WeakPtr<Connection>>(
                  static_cast<base::WeakPtr<Connection>*>(data));
              if (*connection && json)
                (*connection)->ReportPopup(view);
            },
            new base::WeakPtr<Connection>(pending->connection));
      },
      new Pending{self->weak_factory_.GetWeakPtr(), wait});
}

// A popup reported to the client: its Page, then the events it had meanwhile.
void Connection::ReportPopup(zebkit_view* view) {
  if (!popups_.contains(view) || popups_[view].reporting)
    return;
  popups_[view].reporting = true;
  zebkit_automation_call(
      view, "Page.frames", "{}",
      [](zebkit_view* view, const char* json, const char*, void* data) {
        auto connection = std::unique_ptr<base::WeakPtr<Connection>>(
            static_cast<base::WeakPtr<Connection>*>(data));
        Connection* self = connection->get();
        if (!self)
          return;
        auto found = self->popups_.find(view);
        if (found == self->popups_.end())
          return;
        Popup popup = std::move(found->second);
        self->popups_.erase(found);
        // Closed meanwhile: as it was, then its close, as Playwright reports a
        // popup closed at once.
        std::optional<base::Value> frames = Parse(json ? json : popup.frames);
        if (!frames || !frames->is_dict())
          return;
        // A page the browser opened (a modified click) has no opener.
        const bool has_opener = frames->GetDict().FindBool("hasOpener").value_or(true);
        const std::string guid = self->AdoptPage(popup.context, popup.page, *frames,
                                                 has_opener ? popup.opener : std::string());
        for (auto& [event, params] : popup.held) {
          if (!self->Find(guid))
            break;
          self->PageEvent(guid, event, std::move(params));
        }
      },
      new base::WeakPtr<Connection>(weak_factory_.GetWeakPtr()));
}

void Connection::OnEvent(zebkit_view* view,
                         const char* event,
                         const char* json,
                         void* data) {
  auto* self = static_cast<Connection*>(data);
  auto found = self->pages_.find(view);
  if (found == self->pages_.end()) {
    // A popup not reported yet: what it has is kept for when it is; what
    // waits for the client (a route, a dialog, a binding's call, ...)
    // reports it at once, as Playwright's dispatchers deliver those of a page
    // not reported yet.
    auto popup = self->popups_.find(view);
    if (popup == self->popups_.end())
      return;
    const std::string name = event ? event : "";
    std::optional<base::Value> params = Parse(json ? json : "{}");
    popup->second.held.emplace_back(
        name, params && params->is_dict() ? std::move(params->GetDict()) : base::DictValue());
    static const base::NoDestructor<std::set<std::string>> kWaiting(std::set<std::string>{
        "BrowserContext.route", "BrowserContext.dialog", "BrowserContext.bindingCall",
        "Page.fileChooser", "Page.download", "Page.webSocketRoute", "Page.close", "Page.crash"});
    if (kWaiting->contains(name))
      self->ReportPopup(view);
    return;
  }
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
  if (!hars_.empty())
    HarEvent(page_guid, event, params);
  if (!traces_.empty())
    TraceEvent(page_guid, event, params);
  // A page's WebSockets (crNetworkManager.ts's), and their frames.
  if (event == "Page.webSocket") {
    const std::string* socket = params.FindString("socket");
    const std::string* url = params.FindString("url");
    if (!socket)
      return;
    base::DictValue initializer;
    initializer.Set("url", url ? *url : std::string());
    Object object;
    object.id = *socket;
    object.page = page_guid;
    const std::string guid = Create(page_guid, "WebSocket", std::move(initializer), std::move(object));
    Remember("websocket", page_guid, *socket, guid);
    base::DictValue created;
    created.Set("webSocket", Ref(guid));
    Event(page_guid, "webSocket", std::move(created));
    return;
  }
  if (event.starts_with("WebSocket.") &&
      (event == "WebSocket.frameSent" || event == "WebSocket.frameReceived" ||
       event == "WebSocket.socketError" || event == "WebSocket.close")) {
    const std::string* socket = params.FindString("socket");
    const std::string guid = socket ? Known("websocket", page_guid, *socket) : std::string();
    if (!guid.empty()) {
      params.Remove("socket");
      Event(guid, event.substr(sizeof("WebSocket.") - 1), std::move(params));
      return;
    }
  }
  // The network's own headers (raw: Cookie, Set-Cookie), for allHeaders().
  if (event == "Page.requestExtraInfo" || event == "Page.responseExtraInfo") {
    const std::string* request = params.FindString("request");
    const base::ListValue* headers = params.FindList("headers");
    Object* request_object = request ? Find(Known("request", page_guid, *request)) : nullptr;
    if (!request_object || !headers)
      return;
    if (event == "Page.requestExtraInfo") {
      request_object->data.Set("rawHeaders", headers->Clone());
    } else if (const std::string* response = request_object->data.FindString("response");
               response && Find(*response)) {
      Find(*response)->data.Set("rawHeaders", headers->Clone());
    } else {
      request_object->data.Set("rawResponseHeaders", headers->Clone());
    }
    return;
  }
  const std::string context = page->context;
  auto frame_of = [&](const char* key) {
    const std::string* id = params.FindString(key);
    return id ? FrameGuid(page_guid, *id, &params) : std::string();
  };
  if (event == "Frame.navigated") {
    const std::string frame = frame_of("frame");
    base::DictValue navigated;
    const std::string* url = params.FindString("url");
    if (Object* frame_object = Find(frame);
        frame_object && frame_object->data.FindBool("initialBlank").value_or(false)) {
      frame_object->data.Remove("initialBlank");
      if (url && (url->empty() || *url == "about:blank"))
        return;
    }
    navigated.Set("url", url ? *url : "");
    const std::string* name = params.FindString("name");
    navigated.Set("name", name ? *name : "");
    if (!params.FindBool("sameDocument").value_or(false)) {
      if (!params.FindString("parentFrame")) {
        PageRecords& records = page_records_[page_guid];
        records.console_mark = records.console.size();
        records.errors_mark = records.errors.size();
      }
      // Its request, which waitForNavigation() answers with the response of.
      base::DictValue new_document;
      if (Object* frame_object = Find(frame)) {
        if (const std::string* request = frame_object->data.FindString("navigationRequest")) {
          if (Find(*request))
            new_document.Set("request", Ref(*request));
          frame_object->data.Remove("navigationRequest");
        }
      }
      navigated.Set("newDocument", std::move(new_document));
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
    // The event's location, as browserContext.yml has it: not the console's.
    const base::DictValue* location = params.FindDict("location");
    const std::string* location_url = location ? location->FindString("url") : nullptr;
    base::DictValue where;
    where.Set("url", location_url ? *location_url : std::string());
    where.Set("line", location ? location->FindInt("lineNumber").value_or(0) : 0);
    where.Set("column", location ? location->FindInt("columnNumber").value_or(0) : 0);
    message.Set("location", std::move(where));
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
    if (params.FindBool("isNavigationRequest").value_or(false)) {
      if (const std::string* frame_id = params.FindString("frame")) {
        if (Object* frame_object = Find(Known("frame", page_guid, *frame_id)))
          frame_object->data.Set("navigationRequest", request);
      }
    }
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
  } else if (event == "BrowserContext.serviceWorker") {
    // crServiceWorker.ts: a Worker of the context's.
    const std::string* id = params.FindString("worker");
    if (!id || !Known("serviceworker", "", *id).empty())
      return;
    base::DictValue initializer;
    const std::string* url = params.FindString("url");
    initializer.Set("url", url ? *url : "");
    Object object;
    object.id = *id;
    object.context = page->context;
    object.data.Set("serviceWorker", true);
    const std::string guid = Create(page->context, "Worker", std::move(initializer), std::move(object));
    Remember("serviceworker", "", *id, guid);
    base::DictValue worker;
    worker.Set("worker", Ref(guid));
    Event(page->context, "serviceWorker", std::move(worker));
  } else if (event == "ServiceWorker.close") {
    const std::string* id = params.FindString("worker");
    const std::string guid = id ? Known("serviceworker", "", *id) : std::string();
    if (!guid.empty()) {
      Event(guid, "close", base::DictValue());
      Dispose(guid);
      Remember("serviceworker", "", *id, std::string());
    }
  } else if (event == "ServiceWorker.console") {
    const std::string* id = params.FindString("worker");
    const std::string guid = id ? Known("serviceworker", "", *id) : std::string();
    if (guid.empty())
      return;
    base::DictValue console;
    const std::string* type = params.FindString("type");
    console.Set("type", type ? *type : "log");
    const std::string* text = params.FindString("text");
    console.Set("text", text ? *text : "");
    console.Set("args", base::ListValue());
    base::DictValue location;
    if (const base::DictValue* given = params.FindDict("location")) {
      const std::string* url = given->FindString("url");
      location.Set("url", url ? *url : "");
      location.Set("lineNumber", given->FindInt("lineNumber").value_or(0));
    }
    location.Set("columnNumber", 0);
    console.Set("location", std::move(location));
    console.Set("timestamp", base::Time::Now().InMillisecondsFSinceUnixEpoch());
    console.Set("worker", Ref(guid));
    Event(page->context, "console", std::move(console));
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
    const std::string route_id = object.id;
    const std::string guid = Create(page_guid, "WebSocketRoute", std::move(initializer),
                                    std::move(object));
    Remember("ws", page_guid, route_id, guid);
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
void Connection::NewCDPSession(int id,
                               const std::string& parent,
                               scoped_refptr<content::DevToolsAgentHost> host) {
  if (!host) {
    ReplyError(id, "Target page, context or browser has been closed");
    return;
  }
  const std::string guid = Create(parent, "CDPSession", base::DictValue());
  CdpSession& session = cdp_sessions_[guid];
  session.client = std::make_unique<CdpClient>(
      std::move(host),
      base::BindRepeating(&Connection::CDPSessionMessage, weak_factory_.GetWeakPtr(), guid),
      base::BindOnce(&Connection::CDPSessionClosed, weak_factory_.GetWeakPtr(), guid));
  if (!session.client->Attach()) {
    cdp_sessions_.erase(guid);
    Dispose(guid);
    ReplyError(id, "Target page, context or browser has been closed");
    return;
  }
  base::DictValue answer;
  answer.Set("session", Ref(guid));
  Reply(id, base::Value(std::move(answer)));
}

void Connection::CDPSessionCommand(int id,
                                   const std::string& guid,
                                   const std::string& method,
                                   base::DictValue params) {
  auto found = cdp_sessions_.find(guid);
  if (found == cdp_sessions_.end()) {
    ReplyError(id, "Target page, context or browser has been closed");
    return;
  }
  if (method == "send") {
    const std::string* cdp_method = params.FindString("method");
    base::DictValue* cdp_params = params.FindDict("params");
    const std::string name = cdp_method ? *cdp_method : std::string();
    // Before it is sent: the browser's own domains answer at once.
    found->second.calls[found->second.client->NextId()] = {id, name};
    found->second.client->Send(name, cdp_params ? std::move(*cdp_params) : base::DictValue());
    return;
  }
  if (method == "detach") {
    // crConnection.ts's detach(): the session goes, and its object with it,
    // its close told before the answer.
    CDPSessionClosed(guid);
    Reply(id, base::Value(base::DictValue()));
    return;
  }
  Reply(id, base::Value(base::DictValue()));
}

void Connection::CDPSessionMessage(const std::string& guid, base::DictValue message) {
  auto found = cdp_sessions_.find(guid);
  if (found == cdp_sessions_.end())
    return;
  if (std::optional<int> message_id = message.FindInt("id")) {
    auto call = found->second.calls.find(*message_id);
    if (call == found->second.calls.end())
      return;
    const auto [id, method] = call->second;
    found->second.calls.erase(call);
    if (const base::DictValue* error = message.FindDict("error")) {
      const std::string* text = error->FindString("message");
      ReplyError(id, "Protocol error (" + method + "): " + (text ? *text : std::string()));
      return;
    }
    base::DictValue* result = message.FindDict("result");
    base::DictValue answer;
    answer.Set("result", result ? std::move(*result) : base::DictValue());
    Reply(id, base::Value(std::move(answer)));
    return;
  }
  const std::string* method = message.FindString("method");
  if (!method)
    return;
  base::DictValue event;
  event.Set("method", *method);
  if (base::DictValue* params = message.FindDict("params"))
    event.Set("params", std::move(*params));
  Event(guid, "event", std::move(event));
}

void Connection::CDPSessionClosed(const std::string& guid) {
  auto found = cdp_sessions_.find(guid);
  if (found == cdp_sessions_.end())
    return;
  std::unique_ptr<CdpClient> client = std::move(found->second.client);
  std::map<int, std::pair<int, std::string>> calls = std::move(found->second.calls);
  cdp_sessions_.erase(found);
  for (const auto& [message, call] : calls)
    ReplyError(call.first, "Target page, context or browser has been closed");
  if (client) {
    client->Detach();
    // Not under its own call: this may come from its AgentHostClosed().
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE, std::move(client));
  }
  Event(guid, "close", base::DictValue());
  Dispose(guid);
}

void Connection::HarEvent(const std::string& page_guid,
                          const std::string& event,
                          const base::DictValue& params) {
  Object* page = Find(page_guid);
  if (!page)
    return;
  const bool network = event == "Page.request" || event == "Page.response" ||
                       event == "Page.requestContinued" || event == "Page.requestFinished" ||
                       event == "Page.requestFailed" || event == "Page.requestExtraInfo" ||
                       event == "Page.responseExtraInfo";
  const bool lifecycle = event == "Frame.loadstate";
  if (!network && !lifecycle)
    return;
  zebkit_view* view = zebkit_automation_page(page->id.c_str());
  for (auto& [har_id, har] : hars_) {
    if (har.context != page->context || (!har.page.empty() && har.page != page_guid))
      continue;
    if (lifecycle) {
      // The main frame's: the page's title and timings.
      const std::string* frame = params.FindString("frame");
      const std::string* state = params.FindString("state");
      const std::string main = view ? FrameIdOfMain(view) : std::string();
      if (frame && state && *frame == main) {
        har.recorder->PageLoadState(
            page_guid, *state,
            view && view->web_contents() ? base::UTF16ToUTF8(view->web_contents()->GetTitle())
                                         : std::string());
      }
      continue;
    }
    const std::string* frame = params.FindString("frame");
    const std::string frame_guid = frame ? Known("frame", page_guid, *frame) : std::string();
    har.recorder->Record(event, params, page_guid, frame_guid);
    // Its content, kept as it ends, as harTracer.ts's barrier on the body.
    const std::string* request = params.FindString("request");
    if (event == "Page.requestFinished" && request && har.recorder->WantsBody(*request) &&
        view && view->web_contents()) {
      if (Network* page_network = Network::For(view->web_contents())) {
        page_network->WhenBody(
            *request, base::BindOnce(
                          [](base::WeakPtr<Connection> self, std::string har_id,
                             std::string request, std::optional<std::string> body) {
                            if (!self)
                              return;
                            auto found = self->hars_.find(har_id);
                            if (found != self->hars_.end())
                              found->second.recorder->SetBody(request, std::move(body));
                          },
                          weak_factory_.GetWeakPtr(), har_id, *request));
      }
    }
  }
}

void Connection::HarExport(int id,
                           const std::string& har_id,
                           const std::string& mode,
                           const std::string& parent) {
  auto found = hars_.find(har_id);
  if (found == hars_.end()) {
    ReplyError(id, "No HAR recording " + har_id);
    return;
  }
  std::unique_ptr<HarRecorder> recorder = std::move(found->second.recorder);
  hars_.erase(found);
  HarRecorder* raw = recorder.get();
  const bool archive = mode != "entries";
  raw->Export(
      base::FilePath(), HarRecorder::BodyFunc(), std::string(version_info::GetVersionNumber()),
      kPlaywrightVersion, archive,
      base::BindOnce(
          [](base::WeakPtr<Connection> self, int id, std::string parent, bool archive,
             std::unique_ptr<HarRecorder>, const base::FilePath& path, const std::string& error) {
            if (!self)
              return;
            if (!error.empty()) {
              self->ReplyError(id, error);
              return;
            }
            base::DictValue answer;
            if (archive) {
              base::DictValue initializer;
              initializer.Set("absolutePath", path.AsUTF8Unsafe());
              Object artifact;
              artifact.id = path.AsUTF8Unsafe();
              artifact.data.Set("path", path.AsUTF8Unsafe());
              answer.Set("artifact", Ref(self->Create(parent, "Artifact", std::move(initializer),
                                                      std::move(artifact))));
            } else {
              base::ListValue entries;
              base::DictValue entry;
              entry.Set("name", "har.har");
              entry.Set("value", path.AsUTF8Unsafe());
              entries.Append(std::move(entry));
              answer.Set("entries", std::move(entries));
            }
            self->Reply(id, base::Value(std::move(answer)));
          },
          weak_factory_.GetWeakPtr(), id, parent, archive, std::move(recorder)));
}

// ---- Traces ---------------------------------------------------------------

std::string Connection::ContextOf(const std::string& guid) {
  Object* object = Find(guid);
  if (!object)
    return std::string();
  if (object->type == "BrowserContext")
    return guid;
  // An APIRequestContext is traced by its own Tracing.
  if (object->type == "APIRequestContext")
    return guid;
  if (object->type == "Tracing")
    return object->context.empty() ? guid : object->context;
  if (object->type == "Page")
    return object->context;
  Object* page = Find(object->page);
  return page ? page->context : std::string();
}

std::string Connection::PageOf(const std::string& guid) {
  Object* object = Find(guid);
  if (!object)
    return std::string();
  return object->type == "Page" ? guid : object->page;
}

// tracing.ts's visitTraceEvent(): a reference to a protocol object is its
// type, "<ElementHandle>".
base::Value Connection::TraceValue(const base::Value& value) {
  if (value.is_list()) {
    base::ListValue list;
    for (const base::Value& item : value.GetList())
      list.Append(TraceValue(item));
    return base::Value(std::move(list));
  }
  if (!value.is_dict())
    return value.Clone();
  const base::DictValue& dict = value.GetDict();
  if (dict.size() == 1) {
    if (const std::string* guid = dict.FindString("guid")) {
      Object* object = Find(*guid);
      const size_t at = guid->find('@');
      std::string type = object ? object->type : guid->substr(0, at);
      if (!object && !type.empty())
        type[0] = static_cast<char>(toupper(type[0]));
      return base::Value("<" + type + ">");
    }
  }
  base::DictValue result;
  for (const auto [key, item] : dict)
    result.Set(key, TraceValue(item));
  return base::Value(std::move(result));
}

void Connection::TraceLine(Trace& trace, base::DictValue event) {
  trace.lines.push_back(Json(base::Value(std::move(event))));
}

namespace {
const ProtocolMethodInfo* MethodInfo(const std::string& name) {
  for (const ProtocolMethodInfo& info : kProtocolMethods) {
    if (name == info.method)
      return &info;
  }
  return nullptr;
}
}  // namespace

bool Connection::BeforeCall(int id,
                            const std::string& guid,
                            const std::string& method,
                            const base::DictValue& params,
                            const base::DictValue* metadata,
                            base::OnceClosure run) {
  Object* object = Find(guid);
  if (!object || object->type == "Tracing" || object->type == "LocalUtils")
    return false;
  const std::string context = ContextOf(guid);
  auto found = traces_.find(context);
  if (found == traces_.end() || !found->second.recording)
    return false;
  const ProtocolMethodInfo* info = MethodInfo(object->type + "." + method);
  const bool internal = (metadata && metadata->FindBool("internal").value_or(false)) || (info && info->internal);
  if (internal)
    return false;
  Trace& trace = found->second;
  // dispatcher.ts's CallMetadata id.
  const base::Value* given = ids_.contains(id) ? &ids_[id] : nullptr;
  const std::string client_id = !given ? base::NumberToString(id)
                                : given->is_string() ? given->GetString()
                                : given->is_int() ? base::NumberToString(given->GetInt())
                                                  : Json(*given);
  TracedCall call{context, "call@" + client_id, PageOf(guid)};
  base::DictValue event;
  event.Set("type", "before");
  event.Set("callId", call.call_id);
  event.Set("startTime", Monotonic());
  if (const std::string* title = metadata ? metadata->FindString("title") : nullptr)
    event.Set("title", *title);
  event.Set("class", object->type);
  event.Set("method", method);
  base::Value traced_params = TraceValue(base::Value(params.Clone()));
  if (metadata) {
    if (std::optional<double> timeout = metadata->FindDouble("timeout"))
      traced_params.GetDict().Set("timeout", *timeout);
  }
  event.Set("params", std::move(traced_params));
  if (!trace.groups.empty())
    event.Set("parentId", trace.groups.back());
  TraceLine(trace, std::move(event));
  const bool snapshots = trace.options.FindBool("snapshotDom").value_or(false) && info && info->snapshot &&
                         !call.page.empty();
  call.snapshot_after = snapshots;
  const std::string call_id = call.call_id;
  const std::string page = call.page;
  traced_calls_[id] = std::move(call);
  if (snapshots && (!info->input || info->auto_waiting)) {
    CaptureTraceSnapshot(context, page, call_id, "before", std::move(run));
    return true;
  }
  std::move(run).Run();
  return true;
}

void Connection::Answered(int id, base::DictValue message, const std::string& error, base::Value result) {
  auto found = traced_calls_.find(id);
  if (found == traced_calls_.end()) {
    send_.Run(Json(base::Value(std::move(message))));
    return;
  }
  TracedCall call = std::move(found->second);
  traced_calls_.erase(found);
  auto trace = traces_.find(call.context);
  if (trace == traces_.end() || !trace->second.recording) {
    send_.Run(Json(base::Value(std::move(message))));
    return;
  }
  base::DictValue event;
  event.Set("type", "after");
  event.Set("callId", call.call_id);
  event.Set("endTime", Monotonic());
  if (!error.empty()) {
    base::DictValue failure;
    failure.Set("message", error);
    failure.Set("name", error.rfind("Timeout ", 0) == 0 ? "TimeoutError" : "Error");
    event.Set("error", std::move(failure));
  } else if (!result.is_none()) {
    event.Set("result", TraceValue(result));
  }
  TraceLine(trace->second, std::move(event));
  if (trace->second.options.FindBool("screencast").value_or(false) && !call.page.empty())
    TraceScreencastFrame(call.context, call.page);
  auto send = base::BindOnce(
      [](base::WeakPtr<Connection> self, std::string json) {
        if (self)
          self->send_.Run(json);
      },
      weak_factory_.GetWeakPtr(), Json(base::Value(std::move(message))));
  if (call.snapshot_after) {
    CaptureTraceSnapshot(call.context, call.page, call.call_id, "after", std::move(send));
    return;
  }
  std::move(send).Run();
}

void Connection::CaptureTraceSnapshot(const std::string& context,
                                      const std::string& page,
                                      const std::string& call_id,
                                      const std::string& phase,
                                      base::OnceClosure then) {
  Object* page_object = Find(page);
  zebkit_view* view = page_object ? zebkit_automation_page(page_object->id.c_str()) : nullptr;
  auto trace = traces_.find(context);
  if (!view || trace == traces_.end()) {
    std::move(then).Run();
    return;
  }
  base::DictValue params;
  params.Set("streamer", "__playwright_snapshot_streamer_zebkit");
  // The first of a chunk starts the history again; the first of an action, its
  // node references (snapshotter.ts).
  if (trace->second.snapshot_reset) {
    params.Set("reset", "history");
    trace->second.snapshot_reset = false;
  } else if (phase == "before") {
    params.Set("reset", "targets");
  }
  params.Set("removeNoScript", true);
  // A page that cannot answer (a dialog open) holds no command for long: the
  // snapshot is given up after a while, as snapshotter.ts's races do.
  auto once = std::make_shared<base::OnceClosure>(std::move(then));
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE, base::BindOnce([](std::shared_ptr<base::OnceClosure> once) {
        if (*once) std::move(*once).Run();
      }, once),
      base::Seconds(1));
  then = base::BindOnce([](std::shared_ptr<base::OnceClosure> once) {
    if (*once) std::move(*once).Run();
  }, once);
  Native(-1, view, "Page.captureSnapshot", std::move(params),
         base::BindOnce(
             [](base::WeakPtr<Connection> self, std::string context, std::string page,
                std::string call_id, std::string phase, base::OnceClosure then, base::Value result) {
               if (!self) {
                 std::move(then).Run();
                 return;
               }
               auto trace = self->traces_.find(context);
               const base::ListValue* snapshots =
                   result.is_dict() ? result.GetDict().FindList("snapshots") : nullptr;
               if (trace != self->traces_.end() && trace->second.recording && snapshots) {
                 for (const base::Value& item : *snapshots) {
                   const base::DictValue* data = item.GetDict().FindDict("data");
                   const std::string* frame = item.GetDict().FindString("frame");
                   if (!data || !frame)
                     continue;
                   base::DictValue snapshot;
                   snapshot.Set("callId", call_id);
                   snapshot.Set("phase", phase);
                   snapshot.Set("pageId", page);
                   snapshot.Set("frameId", self->Known("frame", page, *frame));
                   for (const char* key : {"doctype", "html", "viewport", "wallTime", "collectionTime"}) {
                     if (const base::Value* value = data->Find(key))
                       snapshot.Set(key, value->Clone());
                   }
                   if (const std::string* url = data->FindString("url"))
                     snapshot.Set("frameUrl", *url);
                   snapshot.Set("timestamp", self->Monotonic());
                   base::ListValue overrides;
                   if (const base::ListValue* given = data->FindList("resourceOverrides")) {
                     for (const base::Value& entry : *given) {
                       const std::string* url = entry.GetDict().FindString("url");
                       const base::Value* content = entry.GetDict().Find("content");
                       if (!url || !content)
                         continue;
                       base::DictValue override_entry;
                       override_entry.Set("url", *url);
                       if (content->is_string()) {
                         const std::string* type = entry.GetDict().FindString("contentType");
                         const std::string ext = type && type->find("css") != std::string::npos ? "css" : "dat";
                         const std::string name = "resources/" +
                             base::ToLowerASCII(base::HexEncode(base::SHA1Hash(base::as_byte_span(content->GetString())))) +
                             "." + ext;
                         trace->second.chunk_files[name] = content->GetString();
                         override_entry.Set("file", name);
                       } else {
                         override_entry.Set("ref", content->Clone());
                       }
                       overrides.Append(std::move(override_entry));
                     }
                   }
                   snapshot.Set("resourceOverrides", std::move(overrides));
                   snapshot.Set("isMainFrame", item.GetDict().FindBool("isMainFrame").value_or(false));
                   base::DictValue event;
                   event.Set("type", "frame-snapshot");
                   event.Set("snapshot", std::move(snapshot));
                   self->TraceLine(trace->second, std::move(event));
                 }
               }
               std::move(then).Run();
             },
             weak_factory_.GetWeakPtr(), context, page, call_id, phase, std::move(then)));
}

void Connection::TraceScreencastFrame(const std::string& context, const std::string& page) {
  auto trace = traces_.find(context);
  Object* page_object = Find(page);
  zebkit_view* view = page_object ? zebkit_automation_page(page_object->id.c_str()) : nullptr;
  if (trace == traces_.end() || !view)
    return;
  base::DictValue params;
  params.Set("type", "jpeg");
  params.Set("quality", 50);
  params.Set("scale", "css");
  Native(-1, view, "Page.screenshot", std::move(params),
         base::BindOnce(
             [](base::WeakPtr<Connection> self, std::string context, std::string page, base::Value result) {
               if (!self)
                 return;
               auto trace = self->traces_.find(context);
               const std::string* data = result.is_dict() ? result.GetDict().FindString("binary") : nullptr;
               std::string bytes;
               if (trace == self->traces_.end() || !trace->second.recording || !data ||
                   !base::Base64Decode(*data, &bytes)) {
                 return;
               }
               Object* page_object = self->Find(page);
               const base::DictValue* viewport = page_object ? page_object->data.FindDict("viewport") : nullptr;
               const std::string file = "screencast/" + page + "-" +
                                        base::NumberToString(static_cast<int64_t>(base::Time::Now().InMillisecondsFSinceUnixEpoch())) +
                                        ".jpeg";
               trace->second.chunk_files[file] = bytes;
               base::DictValue event;
               event.Set("type", "screencast-frame");
               event.Set("pageId", page);
               event.Set("file", file);
               event.Set("width", viewport ? viewport->FindInt("width").value_or(1280) : 1280);
               event.Set("height", viewport ? viewport->FindInt("height").value_or(720) : 720);
               event.Set("timestamp", self->Monotonic());
               self->TraceLine(trace->second, std::move(event));
             },
             weak_factory_.GetWeakPtr(), context, page));
}

void Connection::TraceEvent(const std::string& page_guid,
                            const std::string& event,
                            const base::DictValue& params) {
  Object* page = Find(page_guid);
  if (!page)
    return;
  auto found = traces_.find(page->context);
  if (found == traces_.end() || !found->second.recording)
    return;
  Trace& trace = found->second;
  if (event == "Page.request" || event == "Page.response" || event == "Page.requestFinished" ||
      event == "Page.requestFailed" || event == "Page.requestExtraInfo" ||
      event == "Page.responseExtraInfo") {
    const std::string* frame = params.FindString("frame");
    trace.network->Record(event, params, page_guid,
                          frame ? Known("frame", page_guid, *frame) : std::string());
    const std::string* request = params.FindString("request");
    zebkit_view* view = zebkit_automation_page(page->id.c_str());
    if (event == "Page.requestFinished" && request && view && view->web_contents()) {
      if (Network* network = Network::For(view->web_contents())) {
        network->WhenBody(*request, base::BindOnce(
                                        [](base::WeakPtr<Connection> self, std::string context,
                                           std::string request, std::optional<std::string> body) {
                                          if (!self)
                                            return;
                                          auto trace = self->traces_.find(context);
                                          if (trace != self->traces_.end())
                                            trace->second.network->SetBody(request, std::move(body));
                                        },
                                        weak_factory_.GetWeakPtr(), page->context, *request));
      }
    }
    return;
  }
  base::DictValue line;
  if (event == "BrowserContext.console") {
    line.Set("type", "console");
    const std::string* type = params.FindString("type");
    line.Set("messageType", type ? *type : "log");
    const std::string* text = params.FindString("text");
    line.Set("text", text ? *text : std::string());
    line.Set("args", base::ListValue());
    base::DictValue location;
    if (const base::DictValue* given = params.FindDict("location"))
      location = given->Clone();
    line.Set("location", std::move(location));
    line.Set("time", Monotonic());
    line.Set("pageId", page_guid);
  } else if (event == "Page.pageError") {
    line.Set("type", "event");
    line.Set("time", Monotonic());
    line.Set("class", "BrowserContext");
    line.Set("method", "pageError");
    base::DictValue values;
    if (const base::DictValue* error = params.FindDict("error")) {
      base::DictValue serialized;
      base::DictValue inner;
      for (const char* key : {"message", "name", "stack"}) {
        if (const std::string* value = error->FindString(key))
          inner.Set(key, *value);
      }
      serialized.Set("error", std::move(inner));
      values.Set("error", std::move(serialized));
    }
    if (const base::DictValue* where = params.FindDict("location")) {
      base::DictValue location;
      if (const std::string* url = where->FindString("url"))
        location.Set("url", *url);
      location.Set("line", where->FindInt("lineNumber").value_or(0));
      location.Set("column", where->FindInt("columnNumber").value_or(0));
      values.Set("location", std::move(location));
    }
    line.Set("params", std::move(values));
    line.Set("pageId", page_guid);
  } else if (event == "Page.close") {
    line.Set("type", "event");
    line.Set("time", Monotonic());
    line.Set("class", "BrowserContext");
    line.Set("method", "pageClosed");
    base::DictValue values;
    values.Set("pageId", page_guid);
    line.Set("params", std::move(values));
  } else if (event == "BrowserContext.dialog") {
    line.Set("type", "event");
    line.Set("time", Monotonic());
    line.Set("class", "BrowserContext");
    line.Set("method", "dialog");
    base::DictValue values;
    values.Set("pageId", page_guid);
    for (const char* key : {"type", "message", "defaultValue"}) {
      if (const std::string* value = params.FindString(key))
        values.Set(key, *value);
    }
    line.Set("params", std::move(values));
  } else {
    return;
  }
  TraceLine(trace, std::move(line));
}

void Connection::TraceCommand(int id,
                              const std::string& context,
                              const std::string& method,
                              base::DictValue params) {
  if (method == "tracingStart") {
    Trace& trace = traces_[context];
    if (trace.network) {
      ReplyError(id, "Tracing has been already started");
      return;
    }
    trace.options = params.Clone();
    const std::string* name = params.FindString("name");
    trace.name = name ? *name : base::UnguessableToken::Create().ToString();
    base::DictValue har;
    har.Set("content", "attach");
    har.Set("resourcesPrefix", "resources/");
    trace.network = std::make_unique<HarRecorder>(har);
    Reply(id, base::Value(base::DictValue()));
    return;
  }
  auto found = traces_.find(context);
  if (found == traces_.end() || !found->second.network) {
    if (method == "tracingStop" || method == "tracingGroupEnd" || method == "tracingGroup") {
      Reply(id, base::Value(base::DictValue()));
      return;
    }
    if (method == "tracingStopChunk" && params.FindString("mode") &&
        *params.FindString("mode") == "discard") {
      Reply(id, base::Value(base::DictValue()));
      return;
    }
    ReplyError(id, method == "tracingStartChunk" ? "Must start tracing before starting a new chunk"
                                                 : "Must start tracing before stopping");
    return;
  }
  Trace& trace = found->second;
  if (method == "tracingStartChunk") {
    if (const std::string* name = params.FindString("name"); name && *name != trace.name) {
      trace.name = *name;
      trace.chunk = 0;
    }
    trace.recording = true;
    trace.snapshot_reset = true;
    {
      base::DictValue har;
      har.Set("content", "attach");
      har.Set("resourcesPrefix", "resources/");
      trace.network = std::make_unique<HarRecorder>(har);
    }
    trace.lines.clear();
    trace.chunk_files.clear();
    trace.groups.clear();
    ++trace.chunk;
    Object* context_object = Find(context);
    base::DictValue event;
    event.Set("version", 10);
    event.Set("type", "context-options");
    event.Set("origin", "library");
    event.Set("browserName", "chromium");
    event.Set("channel", "chromium");
    event.Set("playwrightVersion", kPlaywrightVersion);
    const base::DictValue* options = context_object ? context_object->data.FindDict("contextOptions") : nullptr;
    event.Set("options", options ? options->Clone() : base::DictValue());
#if BUILDFLAG(IS_MAC)
    event.Set("platform", "darwin");
#elif BUILDFLAG(IS_WIN)
    event.Set("platform", "win32");
#else
    event.Set("platform", "linux");
#endif
    event.Set("wallTime", base::Time::Now().InMillisecondsFSinceUnixEpoch());
    event.Set("monotonicTime", Monotonic());
    event.Set("sdkLanguage", "javascript");
    event.Set("testIdAttributeName", "data-testid");
    if (const std::string* title = params.FindString("title"))
      event.Set("title", *title);
    TraceLine(trace, std::move(event));
    // The pages there are.
    for (const auto& [view, page_guid] : pages_) {
      Object* page = Find(page_guid);
      if (!page || page->context != context)
        continue;
      base::DictValue opened;
      opened.Set("type", "event");
      opened.Set("time", Monotonic());
      opened.Set("class", "BrowserContext");
      opened.Set("method", "page");
      base::DictValue values;
      values.Set("pageId", page_guid);
      opened.Set("params", std::move(values));
      TraceLine(trace, std::move(opened));
    }
    base::DictValue answer;
    answer.Set("traceName", trace.name);
    Reply(id, base::Value(std::move(answer)));
    return;
  }
  if (method == "tracingGroup") {
    base::DictValue event;
    const std::string call_id = "group@" + base::NumberToString(++next_guid_);
    event.Set("type", "before");
    event.Set("callId", call_id);
    event.Set("startTime", Monotonic());
    const std::string* name = params.FindString("name");
    event.Set("title", name ? *name : std::string());
    event.Set("class", "Tracing");
    event.Set("method", "tracingGroup");
    event.Set("params", base::DictValue());
    base::ListValue stack;
    if (const base::DictValue* location = params.FindDict("location")) {
      base::DictValue frame;
      const std::string* file = location->FindString("file");
      frame.Set("file", file ? *file : std::string());
      frame.Set("line", location->FindInt("line").value_or(0));
      frame.Set("column", location->FindInt("column").value_or(0));
      stack.Append(std::move(frame));
    }
    event.Set("stack", std::move(stack));
    if (!trace.groups.empty())
      event.Set("parentId", trace.groups.back());
    trace.groups.push_back(call_id);
    if (trace.recording)
      TraceLine(trace, std::move(event));
    Reply(id, base::Value(base::DictValue()));
    return;
  }
  auto end_groups = [&](Trace& trace) {
    while (!trace.groups.empty()) {
      base::DictValue event;
      event.Set("type", "after");
      event.Set("callId", trace.groups.back());
      event.Set("endTime", Monotonic());
      trace.groups.pop_back();
      if (trace.recording)
        TraceLine(trace, std::move(event));
    }
  };
  if (method == "tracingGroupEnd") {
    if (!trace.groups.empty()) {
      base::DictValue event;
      event.Set("type", "after");
      event.Set("callId", trace.groups.back());
      event.Set("endTime", Monotonic());
      trace.groups.pop_back();
      if (trace.recording)
        TraceLine(trace, std::move(event));
    }
    Reply(id, base::Value(base::DictValue()));
    return;
  }
  if (method == "tracingStop") {
    if (trace.recording) {
      ReplyError(id, "Must stop trace file before stopping tracing");
      return;
    }
    traces_.erase(found);
    Reply(id, base::Value(base::DictValue()));
    return;
  }
  // tracingStopChunk {mode: archive, discard, entries}.
  const std::string* mode_value = params.FindString("mode");
  const std::string mode = mode_value ? *mode_value : "archive";
  if (!trace.recording) {
    if (mode == "discard") {
      Reply(id, base::Value(base::DictValue()));
      return;
    }
    ReplyError(id, "Must start tracing before stopping");
    return;
  }
  end_groups(trace);
  trace.recording = false;
  if (mode == "discard") {
    Reply(id, base::Value(base::DictValue()));
    return;
  }
  std::map<std::string, std::string> files = std::move(trace.chunk_files);
  trace.chunk_files.clear();
  std::string text;
  for (const std::string& line : trace.lines)
    text += line + "\n";
  trace.lines.clear();
  files["trace.trace"] = std::move(text);
  std::string network;
  std::map<std::string, std::string> resources;
  base::ListValue network_entries = trace.network->Entries(
      base::BindRepeating([](const std::string&) -> std::optional<std::string> { return std::nullopt; }),
      &resources);
  for (base::Value& entry : network_entries) {
    base::DictValue line;
    line.Set("type", "resource-snapshot");
    line.Set("snapshot", std::move(entry));
    network += Json(base::Value(std::move(line))) + "\n";
  }
  for (auto& [name, bytes] : resources)
    trace.cross_files[name] = std::move(bytes);
  for (const auto& [name, bytes] : trace.cross_files)
    files[name] = bytes;
  files["trace.network"] = std::move(network);
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(
          [](std::map<std::string, std::string> files, bool archive) -> std::optional<base::FilePath> {
            base::FilePath directory;
            if (!base::CreateNewTempDirectory(FILE_PATH_LITERAL("zebkit-trace"), &directory))
              return std::nullopt;
            for (const auto& [name, bytes] : files) {
              const base::FilePath path = directory.AppendASCII(name);
              base::CreateDirectory(path.DirName());
              if (!base::WriteFile(path, bytes))
                return std::nullopt;
            }
            if (!archive)
              return directory;
            // Files only, as Playwright's archives: a client unpacking it
            // refuses directory entries ("resources/").
            const base::FilePath zip = directory.AddExtensionASCII(".zip");
            std::vector<base::FilePath> names;
            for (const auto& [name, bytes] : files)
              names.push_back(base::FilePath::FromASCII(name));
            zip::ZipParams params;
            params.src_dir = directory;
            params.dest_file = zip;
            params.src_files = names;
            if (!zip::Zip(params))
              return std::nullopt;
            base::DeletePathRecursively(directory);
            return zip;
          },
          std::move(files), mode == "archive"),
      base::BindOnce(
          [](base::WeakPtr<Connection> self, int id, std::string context, bool archive,
             std::optional<base::FilePath> path) {
            if (!self)
              return;
            if (!path) {
              self->ReplyError(id, "zebkit: the trace could not be written");
              return;
            }
            base::DictValue answer;
            if (archive) {
              base::DictValue initializer;
              initializer.Set("absolutePath", path->AsUTF8Unsafe());
              Object artifact;
              artifact.id = path->AsUTF8Unsafe();
              artifact.data.Set("path", path->AsUTF8Unsafe());
              answer.Set("artifact", Ref(self->Create(context, "Artifact", std::move(initializer),
                                                      std::move(artifact))));
            } else {
              base::ListValue entries;
              base::FileEnumerator files(*path, true, base::FileEnumerator::FILES);
              for (base::FilePath file = files.Next(); !file.empty(); file = files.Next()) {
                base::FilePath relative;
                path->AppendRelativePath(file, &relative);
                base::DictValue entry;
                entry.Set("name", relative.AsUTF8Unsafe());
                entry.Set("value", file.AsUTF8Unsafe());
                entries.Append(std::move(entry));
              }
              answer.Set("entries", std::move(entries));
            }
            self->Reply(id, base::Value(std::move(answer)));
          },
          weak_factory_.GetWeakPtr(), id, context, mode == "archive"));
}

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
  if (method == "tracingStart" || method == "tracingStartChunk" || method == "tracingStopChunk" ||
      method == "tracingStop" || method == "tracingGroup" || method == "tracingGroupEnd") {
    TraceCommand(id, tracing->context, method, std::move(params));
    return;
  } else if (method == "harStart") {
    const std::string har_id = "har@" + base::NumberToString(++next_guid_);
    const base::DictValue* options = params.FindDict("options");
    Har& har = hars_[har_id];
    har.context = tracing->context;
    if (const std::string* page = params.FindStringByDottedPath("page.guid"))
      har.page = *page;
    har.recorder = std::make_unique<HarRecorder>(options ? options->Clone() : base::DictValue());
    for (const auto& [page_view, page] : pages_) {
      Object* object = Find(page);
      if (object && object->context == har.context && (har.page.empty() || har.page == page))
        har.recorder->AddPage(page);
    }
    base::DictValue answer;
    answer.Set("harId", har_id);
    Reply(id, base::Value(std::move(answer)));
    return;
  } else if (method == "harExport") {
    const std::string* har_id = params.FindString("harId");
    const std::string* mode = params.FindString("mode");
    HarExport(id, har_id ? *har_id : std::string(), mode ? *mode : "archive", tracing->context);
    return;
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
    // Nothing was recorded without a page: no artifact, which the protocol
    // allows, rather than an error for a context that has no pages left.
    if (method == "tracingStopChunk" || method == "harExport") {
      Reply(id, base::Value(base::DictValue()));
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
    } else if (method == "newBrowserCDPSession") {
      NewCDPSession(id, guid, content::DevToolsAgentHost::CreateForBrowser(nullptr, {}));
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
    if (method == "newCDPSession") {
      // crBrowser.ts's newCDPSession(): the page's target, or the frame's when
      // it is one of its own (a frame of another site); else the error it gives.
      const std::string* page_ref = params.FindStringByDottedPath("page.guid");
      const std::string* frame_ref = params.FindStringByDottedPath("frame.guid");
      Object* target = Find(page_ref ? *page_ref : frame_ref ? *frame_ref : std::string());
      zebkit_view* view = target ? ViewOf(*target) : nullptr;
      content::WebContents* contents = view ? view->web_contents() : nullptr;
      if (!contents) {
        ReplyError(id, page_ref || frame_ref ? "Target page, context or browser has been closed"
                                             : "page: expected Page or Frame");
        return;
      }
      if (target->type == "Page") {
        NewCDPSession(id, guid, content::DevToolsAgentHost::GetOrCreateFor(contents));
        return;
      }
      int node = 0;
      content::RenderFrameHost* frame =
          target->id.size() > 1 && base::StringToInt(std::string_view(target->id).substr(1), &node)
              ? contents->UnsafeFindFrameByFrameTreeNodeId(content::FrameTreeNodeId(node))
              : nullptr;
      if (frame && !frame->GetParent()) {
        NewCDPSession(id, guid, content::DevToolsAgentHost::GetOrCreateFor(contents));
        return;
      }
      if (!frame || !frame->IsCrossProcessSubframe()) {
        ReplyError(id, "This frame does not have a separate CDP session, it is a part of the "
                       "parent frame's session");
        return;
      }
      // A frame's own target is named by its DevTools token, and made when
      // DevTools looks at every target.
      content::DevToolsAgentHost::GetOrCreateAll();
      NewCDPSession(id, guid, content::DevToolsAgentHost::GetForId(
                                  frame->GetDevToolsFrameToken().ToString()));
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
      // For a HAR of its context's (harTracer.ts's _onAPIRequest).
      const std::string har_context = object->context.empty() ? guid : object->context;
      base::DictValue har_request;
      for (const char* key : {"url", "method", "headers", "postData"}) {
        if (const base::Value* value = params.Find(key))
          har_request.Set(key, value->Clone());
      }
      Native(id, nullptr, "APIRequestContext.fetch", std::move(params),
             base::BindOnce(
                 [](base::WeakPtr<Connection> self, int id, bool fail_on_status,
                    std::string request_method, std::string har_context,
                    base::DictValue har_request, base::Value result) {
                   if (!self)
                     return;
                   base::DictValue* response =
                       result.is_dict() ? result.GetDict().FindDict("response") : nullptr;
                   if (!response) {
                     self->ReplyError(id, "zebkit: no response");
                     return;
                   }
                   for (auto& [har_id, har] : self->hars_) {
                     if (har.context == har_context)
                       har.recorder->RecordFetch(har_request, *response);
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
                 weak_factory_.GetWeakPtr(), id, fail_on_status, std::move(request_method),
                 har_context, std::move(har_request)));
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
  if (type == "CDPSession") {
    CDPSessionCommand(id, guid, method, std::move(params));
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
  // A service worker is the context's, of no page.
  if (!view && type == "Worker" && object->data.FindBool("serviceWorker").value_or(false)) {
    if (method == "evaluateExpressionHandle") {
      ReplyError(id, "zebkit: a service worker's handles are not kept; evaluate() answers by value");
      return;
    }
    if (method != "evaluateExpression") {
      empty_reply();
      return;
    }
    params.Set("worker", object->id);
    if (base::Value* arg = params.Find("arg"); arg && arg->is_dict()) {
      if (base::Value* value = arg->GetDict().Find("value"))
        params.Set("arg", Plain(*value));
    }
    Native(id, nullptr, "ServiceWorker.evaluateExpression", std::move(params),
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
  }
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
    // route.fulfill({response}) of route.fetch()'s: the client names the
    // fetched response, whose body the endpoint kept (network.ts).
    if (const std::string* uid = params.FindString("fetchResponseUid");
        method == "fulfill" && uid && !params.FindString("body")) {
      auto fetched = fetched_.find(*uid);
      params.Set("body", fetched != fetched_.end() ? fetched->second : std::string());
      params.Set("isBase64", true);
    }
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
      const base::ListValue* raw = object->data.FindList("rawHeaders");
      if (!raw)
        raw = object->data.FindList("headers");
      answer.Set("headers", raw ? raw->Clone() : base::ListValue());
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
      const base::ListValue* raw = object->data.FindList("rawHeaders");
      if (!raw)
        raw = object->data.FindList("headers");
      answer.Set("headers", raw ? raw->Clone() : base::ListValue());
    } else if (method == "httpVersion") {
      const std::string* version = object->data.FindString("httpVersion");
      answer.Set("value", version ? "HTTP/" + *version : "HTTP/1.1");
    } else if (method == "serverAddr") {
      if (const std::string* address = object->data.FindString("serverAddr")) {
        base::DictValue value;
        value.Set("ipAddress", *address);
        value.Set("port", object->data.FindInt("serverPort").value_or(0));
        answer.Set("value", std::move(value));
      }
    } else if (method == "securityDetails") {
      if (const base::DictValue* security = object->data.FindDict("securityDetails"))
        answer.Set("value", security->Clone());
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
  // urlMatch.ts's resolveGlobBase(): a glob relative to the context's
  // baseURL (ws: for WebSockets' of an http: one), and a URL without a path
  // as the browser writes it (with "/").
  if (method == "setNetworkInterceptionPatterns" || method == "setWebSocketInterceptionPatterns") {
    Object* page = type == "Page" ? object : nullptr;
    Object* context = page ? Find(page->context) : object;
    const std::string* base_url = context ? context->data.FindString("baseURL") : nullptr;
    const bool web_socket = method == "setWebSocketInterceptionPatterns";
    if (base::ListValue* patterns = params.FindList("patterns")) {
      for (base::Value& pattern : *patterns) {
        std::string* glob = pattern.is_dict() ? pattern.GetDict().FindString("glob") : nullptr;
        if (!glob || glob->empty() || glob->front() == '*')
          continue;
        std::string base = base_url ? *base_url : std::string();
        if (web_socket && base.size() > 4 && base::StartsWith(base, "http", base::CompareCase::INSENSITIVE_ASCII)) {
          const bool secure = base::StartsWith(base, "https", base::CompareCase::INSENSITIVE_ASCII);
          base = (secure ? "wss" : "ws") + base.substr(secure ? 5 : 4);
        }
        const bool has_scheme = glob->find("://") != std::string::npos;
        if (!has_scheme && !base.empty() && glob->find_first_of("*?{[") == std::string::npos) {
          const GURL resolved = GURL(base).Resolve(*glob);
          if (resolved.is_valid())
            *glob = resolved.spec();
        } else if (has_scheme && glob->find_first_of("*?{[") == std::string::npos) {
          const GURL url(*glob);
          if (url.is_valid())
            *glob = url.spec();
        }
      }
    }
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

  // Only a client that was given the url, with @path, connects (Endpoint::Url).
  Socket(scoped_refptr<base::SequencedTaskRunner> ui,
         std::string path,
         Received received,
         Opened opened)
      : ui_(std::move(ui)),
        path_(std::move(path)),
        received_(std::move(received)),
        opened_(std::move(opened)) {}

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
    // A web page's WebSocket always says its Origin, and Playwright's clients
    // say none: as Chromium's DevTools handler without --remote-allow-origins,
    // no origin is let in. The path is the secret launchServer()'s is, so that
    // nothing else on this machine connects either: this endpoint writes files.
    if (request.headers.contains("origin") || request.path != path_) {
      server_->Send(connection, net::HTTP_FORBIDDEN, "Connection rejected",
                    "text/plain", kAnnotation);
      return;
    }
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
  const std::string path_;
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
    path_ = "/" + base::UnguessableToken::Create().ToString();
    socket_ = new Socket(
        base::SequencedTaskRunner::GetCurrentDefault(),
        path_,
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
    return "ws://127.0.0.1:" + base::NumberToString(port_) + path_;
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
  // Random, in the url the host is given: launchServer()'s wsPath.
  std::string path_;
  std::map<int, std::unique_ptr<Connection>> connections_;
};

}  // namespace

}  // namespace zebkit

void zebkit_automation_listen(int port, zebkit_automation_listen_func func, void* data) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  zebkit::Endpoint::Get()->Listen(port, func, data);
}
