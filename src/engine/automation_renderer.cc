// zebkit's automation, the renderer's side (docs/AUTOMATION.md): each
// frame's agent keeps a runner of automation_steps.js in the page's world
// and one in zebkit's isolated world, over Playwright's injected script,
// and the objects the browser holds as handles; it calls the runners from
// V8 directly, as CDP's Runtime.callFunctionOn does, and commits text as an
// IME does. The browser's side is automation.cc.

#include <algorithm>
#include <map>
#include <set>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/values.h"
#include "content/public/common/isolated_world_ids.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_frame_observer.h"
#include "content/public/renderer/worker_thread.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "mojo/public/cpp/bindings/associated_receiver_set.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_registry.h"
#include "third_party/blink/public/mojom/frame/user_activation_notification_type.mojom-shared.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_element.h"
#include "third_party/blink/public/web/web_frame.h"
#include "third_party/blink/public/web/web_frame_widget.h"
#include "third_party/blink/public/web/web_input_element.h"
#include "third_party/blink/public/web/web_input_method_controller.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_media_emulation.h"
#include "third_party/blink/public/web/web_view.h"
#include "third_party/blink/public/web/web_range.h"
#include "third_party/blink/public/web/web_script_source.h"
#include "v8/include/v8.h"
#include "zebkit/engine/automation.h"
#include "zebkit/engine/automation.mojom.h"
#include "zebkit/engine/automation_scripts.h"

namespace zebkit {

namespace {

using mojom::AutomationExpression;
using mojom::AutomationHandle;
using mojom::AutomationHandlePtr;
using mojom::AutomationStatus;
using mojom::AutomationWorld;

// The world the injected script does what the page must not see in: its
// own globals, which the page's script neither sees nor changes. One past
// content's own.
constexpr int32_t kUtilityWorldId = content::ISOLATED_WORLD_ID_CONTENT_END + 1;

using Reply = base::OnceCallback<
    void(AutomationStatus, const std::string&, std::vector<AutomationHandlePtr>)>;

std::string Text(v8::Isolate* isolate, v8::Local<v8::Value> value) {
  if (value.IsEmpty())
    return std::string();
  v8::String::Utf8Value utf8(isolate, value);
  return *utf8 ? std::string(*utf8, utf8.length()) : std::string();
}

v8::Local<v8::String> String(v8::Isolate* isolate, std::string_view text) {
  return v8::String::NewFromUtf8(isolate, text.data(),
                                 v8::NewStringType::kNormal,
                                 static_cast<int>(text.size()))
      .ToLocalChecked();
}

class Agent;

// ---- Workers ----------------------------------------------------------------

// The renderer's main thread, where the agents are.
scoped_refptr<base::SequencedTaskRunner>& MainThread() {
  static base::NoDestructor<scoped_refptr<base::SequencedTaskRunner>> main;
  return *main;
}

// The workers of this renderer: their worker thread's id and URL, by the
// renderer's number for them. Read and written from the threads.
struct WorkerEntry {
  int thread;
  std::string url;
};
base::Lock& WorkersLock() {
  static base::NoDestructor<base::Lock> lock;
  return *lock;
}
std::map<int32_t, WorkerEntry>& Workers() {
  static base::NoDestructor<std::map<int32_t, WorkerEntry>> workers;
  return *workers;
}

// What a worker thread keeps of its worker: its context, and the
// evaluations it answers later.
struct WorkerState : public content::WorkerThread::Observer {
  int32_t id = 0;
  raw_ptr<v8::Isolate> isolate = nullptr;
  v8::Global<v8::Context> context;
  std::map<int64_t, base::OnceCallback<void(AutomationStatus, std::string)>>
      pending;
  int64_t next = 0;

  void WillStopCurrentWorkerThread() override;
};

thread_local WorkerState* g_worker = nullptr;

void NotifyWorker(int32_t worker, std::string url, bool started);

void WorkerState::WillStopCurrentWorkerThread() {
  {
    base::AutoLock lock(WorkersLock());
    Workers().erase(id);
  }
  for (auto& [request, done] : pending)
    std::move(done).Run(AutomationStatus::kGone, std::string());
  if (MainThread()) {
    MainThread()->PostTask(FROM_HERE, base::BindOnce(&NotifyWorker, id,
                                                     std::string(), false));
  }
  content::WorkerThread::RemoveObserver(this);
  g_worker = nullptr;
  delete this;
}

// A worker's evaluation settled, on its thread.
void WorkerSettled(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (!g_worker)
    return;
  v8::Isolate* isolate = info.GetIsolate();
  v8::Local<v8::Array> data = info.Data().As<v8::Array>();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const int64_t request = static_cast<int64_t>(
      data->Get(context, 0).ToLocalChecked().As<v8::Number>()->Value());
  const bool ok = data->Get(context, 1).ToLocalChecked()->IsTrue();
  auto found = g_worker->pending.find(request);
  if (found == g_worker->pending.end())
    return;
  auto done = std::move(found->second);
  g_worker->pending.erase(found);
  v8::String::Utf8Value text(isolate, info[0]);
  std::string value = *text ? std::string(*text, text.length()) : std::string();
  if (ok) {
    std::move(done).Run(AutomationStatus::kOk,
                        "{\"ok\":true,\"value\":" + value + "}");
  } else {
    std::string escaped;
    for (char c : value) {
      if (c == '"' || c == '\\')
        escaped += '\\';
      if (c == '\n') {
        escaped += "\\n";
        continue;
      }
      escaped += c;
    }
    std::move(done).Run(AutomationStatus::kOk,
                        "{\"ok\":false,\"error\":\"" + escaped + "\"}");
  }
}

// @source evaluated in the worker of this thread; @done on the main thread.
void EvaluateInWorker(std::string source,
                      base::OnceCallback<void(AutomationStatus, std::string)> done) {
  auto reply = base::BindOnce(
      [](base::OnceCallback<void(AutomationStatus, std::string)> done,
         AutomationStatus status, std::string result) {
        MainThread()->PostTask(FROM_HERE, base::BindOnce(std::move(done), status,
                                                         std::move(result)));
      },
      std::move(done));
  if (!g_worker) {
    std::move(reply).Run(AutomationStatus::kGone, std::string());
    return;
  }
  v8::Isolate* isolate = g_worker->isolate;
  v8::HandleScope scope(isolate);
  v8::Local<v8::Context> context = g_worker->context.Get(isolate);
  v8::Context::Scope context_scope(context);
  v8::MicrotasksScope microtasks(context, v8::MicrotasksScope::kRunMicrotasks);
  v8::TryCatch try_catch(isolate);
  v8::Local<v8::Script> script;
  v8::Local<v8::Value> result;
  if (!v8::Script::Compile(context, v8::String::NewFromUtf8(isolate, source.c_str()).ToLocalChecked())
           .ToLocal(&script) ||
      !script->Run(context).ToLocal(&result) || !result->IsPromise()) {
    v8::String::Utf8Value text(isolate, try_catch.Exception());
    std::string message = *text ? *text : "Error";
    std::move(reply).Run(AutomationStatus::kError, message);
    return;
  }
  const int64_t request = ++g_worker->next;
  g_worker->pending[request] = std::move(reply);
  auto reaction = [&](bool ok) {
    v8::Local<v8::Array> data = v8::Array::New(isolate, 3);
    data->Set(context, 0, v8::Number::New(isolate, static_cast<double>(request)))
        .Check();
    data->Set(context, 1, v8::Boolean::New(isolate, ok)).Check();
    return v8::Function::New(context, &WorkerSettled, data).ToLocalChecked();
  };
  std::ignore = result.As<v8::Promise>()->Then(context, reaction(true), reaction(false));
}


// A worker's URL, once its script runs: its location is set when the
// script is fetched (WorkerGlobalScope::Initialize), after its context is
// made, and reading it before then is a CHECK. So the first call into the
// worker's script posts this, which reads it and tells the agents.
void ReportWorker() {
  WorkerState* state = g_worker;
  if (!state || !state->isolate)
    return;
  v8::Isolate* isolate = state->isolate;
  v8::HandleScope scope(isolate);
  v8::Local<v8::Context> context = state->context.Get(isolate);
  if (context.IsEmpty())
    return;
  v8::Context::Scope context_scope(context);
  std::string url;
  {
    v8::TryCatch try_catch(isolate);
    v8::MicrotasksScope microtasks(context, v8::MicrotasksScope::kDoNotRunMicrotasks);
    v8::Local<v8::Script> script;
    v8::Local<v8::Value> value;
    if (v8::Script::Compile(context, v8::String::NewFromUtf8Literal(
                                         isolate, "String(self.location.href)"))
            .ToLocal(&script) &&
        script->Run(context).ToLocal(&value)) {
      v8::String::Utf8Value text(isolate, value);
      if (*text)
        url = std::string(*text, text.length());
    }
  }
  if (url.empty())
    return;
  {
    base::AutoLock lock(WorkersLock());
    Workers()[state->id].url = url;
  }
  if (MainThread()) {
    MainThread()->PostTask(FROM_HERE,
                           base::BindOnce(&NotifyWorker, state->id, url, true));
  }
}

void WorkerCallEntered(v8::Isolate* isolate) {
  isolate->RemoveBeforeCallEnteredCallback(&WorkerCallEntered);
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&ReportWorker));
}

}  // namespace

// ContentRendererClient::DidInitializeWorkerContextOnWorkerThread(), on the
// worker's thread: the worker known to the renderer's agents once its script
// runs (ReportWorker).
void AutomationWorkerStarted(v8::Local<v8::Context> context) {
  const int thread = content::WorkerThread::GetCurrentId();
  if (thread == content::WorkerThread::kInvalidWorkerThreadId || g_worker)
    return;
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  v8::HandleScope scope(isolate);
  v8::Context::Scope context_scope(context);
  // Only workers' own scopes, not worklets'.
  bool worker = false;
  {
    v8::TryCatch try_catch(isolate);
    v8::MicrotasksScope microtasks(context, v8::MicrotasksScope::kDoNotRunMicrotasks);
    v8::Local<v8::Script> script;
    v8::Local<v8::Value> value;
    worker = v8::Script::Compile(
                 context, v8::String::NewFromUtf8Literal(
                              isolate,
                              "typeof WorkerGlobalScope !== 'undefined' && "
                              "self instanceof WorkerGlobalScope"))
                 .ToLocal(&script) &&
             script->Run(context).ToLocal(&value) && value->IsTrue();
  }
  if (!worker)
    return;
  static int32_t next_worker = 0;
  auto* state = new WorkerState;
  {
    base::AutoLock lock(WorkersLock());
    state->id = ++next_worker;
    Workers()[state->id] = {thread, std::string()};
  }
  state->isolate = isolate;
  state->context.Reset(isolate, context);
  g_worker = state;
  content::WorkerThread::AddObserver(state);
  isolate->AddBeforeCallEnteredCallback(&WorkerCallEntered);
}

namespace {

// The calls whose runner answers later (a promise), by number, for the
// promise's reactions, which are plain functions.
std::map<int64_t, base::WeakPtr<Agent>>& Pending() {
  static base::NoDestructor<std::map<int64_t, base::WeakPtr<Agent>>> pending;
  return *pending;
}

// One for each frame; deletes itself with it.
class Agent : public content::RenderFrameObserver,
              public mojom::AutomationAgent {
 public:
  explicit Agent(content::RenderFrame* render_frame)
      : content::RenderFrameObserver(render_frame) {
    All().insert(this);
    render_frame->GetAssociatedInterfaceRegistry()
        ->AddInterface<mojom::AutomationAgent>(base::BindRepeating(
            &Agent::Bind, weak_factory_.GetWeakPtr()));
  }

  ~Agent() override {
    All().erase(this);
    if (id_)
      Agents().erase(id_);
    for (auto& [id, call] : calls_)
      std::move(call.reply).Run(AutomationStatus::kGone, std::string(), {});
  }

  // content::RenderFrameObserver
  void OnDestruct() override { delete this; }

  // A new document of the page's world: the exposed functions in it,
  // before its own script.
  void DidCreateScriptContext(v8::Local<v8::Context> context,
                              int32_t world_id) override {
    if (world_id == 0 && !exposed_.empty())
      Define(context, exposed_);
  }

  void WillReleaseScriptContext(v8::Local<v8::Context>,
                                int32_t world_id) override {
    if (world_id != 0 && world_id != kUtilityWorldId)
      return;
    // The document's objects go with it: its runner, its handles, and the
    // calls waiting on it.
    runners_.erase(world_id);
    std::erase_if(handles_, [world_id](const auto& entry) {
      return entry.second.world == world_id;
    });
    std::vector<Reply> gone;
    for (auto it = calls_.begin(); it != calls_.end();) {
      if (it->second.world == world_id) {
        Pending().erase(it->first);
        gone.push_back(std::move(it->second.reply));
        it = calls_.erase(it);
      } else {
        ++it;
      }
    }
    for (Reply& reply : gone)
      std::move(reply).Run(AutomationStatus::kGone, std::string(), {});
  }

  // mojom::AutomationAgent
  void Step(AutomationWorld world,
            const std::string& step,
            const std::string& arg,
            const std::vector<int32_t>& handles,
            StepCallback callback) override {
    const int32_t world_id = WorldId(world);
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    v8::HandleScope scope(isolate);
    v8::Local<v8::Context> context = Context(world_id);
    if (context.IsEmpty()) {
      std::move(callback).Run(AutomationStatus::kGone, std::string(), {});
      return;
    }
    v8::Context::Scope context_scope(context);
    v8::Local<v8::Value> args[] = {String(isolate, step), String(isolate, arg),
                                   Values(isolate, context, handles)};
    Invoke(world_id, "step", args,
           base::BindOnce(&Agent::StepAnswered, weak_factory_.GetWeakPtr(),
                          std::move(callback)));
  }

  void Evaluate(AutomationWorld world,
                const std::string& expression,
                AutomationExpression kind,
                const std::string& args,
                const std::vector<int32_t>& handles,
                bool return_by_value,
                bool plain,
                EvaluateCallback callback) override {
    const int32_t world_id = WorldId(world);
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    v8::HandleScope scope(isolate);
    v8::Local<v8::Context> context = Context(world_id);
    if (context.IsEmpty()) {
      std::move(callback).Run(AutomationStatus::kGone, std::string(), {});
      return;
    }
    v8::Context::Scope context_scope(context);
    std::string error;
    v8::Local<v8::Value> compiled = Compile(isolate, context, expression, kind, &error);
    if (compiled.IsEmpty()) {
      std::move(callback).Run(AutomationStatus::kOk, ErrorJson(error), {});
      return;
    }
    // As the person's act, as Playwright's Runtime.callFunctionOn with
    // userGesture: it may open a window, play media.
    if (world_id == 0) {
      render_frame()->GetWebFrame()->NotifyUserActivation(
          blink::mojom::UserActivationNotificationType::kDevTools);
    }
    v8::Local<v8::Value> argv[] = {
        compiled, Kind(isolate, kind), String(isolate, args),
        Values(isolate, context, handles),
        v8::Boolean::New(isolate, return_by_value),
        v8::Boolean::New(isolate, plain)};
    Invoke(world_id, "evaluate", argv, std::move(callback));
  }

  void WaitForFunction(AutomationWorld world,
                       const std::string& expression,
                       AutomationExpression kind,
                       const std::string& args,
                       const std::vector<int32_t>& handles,
                       int32_t polling,
                       int32_t timeout,
                       bool plain,
                       WaitForFunctionCallback callback) override {
    const int32_t world_id = WorldId(world);
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    v8::HandleScope scope(isolate);
    v8::Local<v8::Context> context = Context(world_id);
    if (context.IsEmpty()) {
      std::move(callback).Run(AutomationStatus::kGone, std::string(), {});
      return;
    }
    v8::Context::Scope context_scope(context);
    std::string error;
    v8::Local<v8::Value> compiled = Compile(isolate, context, expression, kind, &error);
    if (compiled.IsEmpty()) {
      std::move(callback).Run(AutomationStatus::kOk, ErrorJson(error), {});
      return;
    }
    v8::Local<v8::Value> argv[] = {
        compiled,
        Kind(isolate, kind),
        String(isolate, args),
        Values(isolate, context, handles),
        polling > 0 ? v8::Local<v8::Value>(v8::Integer::New(isolate, polling))
                    : v8::Local<v8::Value>(v8::Undefined(isolate)),
        v8::Integer::New(isolate, timeout),
        v8::Boolean::New(isolate, plain)};
    Invoke(world_id, "waitForFunction", argv, std::move(callback));
  }

  void Properties(int32_t handle,
                  const std::string& name,
                  PropertiesCallback callback) override {
    auto found = handles_.find(handle);
    if (found == handles_.end()) {
      std::move(callback).Run(AutomationStatus::kOk,
                              ErrorJson(kDisposed), {});
      return;
    }
    const int32_t world_id = found->second.world;
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    v8::HandleScope scope(isolate);
    v8::Local<v8::Context> context = Context(world_id);
    if (context.IsEmpty()) {
      std::move(callback).Run(AutomationStatus::kGone, std::string(), {});
      return;
    }
    v8::Context::Scope context_scope(context);
    v8::Local<v8::Value> argv[] = {
        found->second.value.Get(isolate),
        name.empty() ? v8::Local<v8::Value>(v8::Undefined(isolate))
                     : v8::Local<v8::Value>(String(isolate, name))};
    Invoke(world_id, "properties", argv, std::move(callback));
  }

  void JsonValue(int32_t handle,
                 bool plain,
                 JsonValueCallback callback) override {
    auto found = handles_.find(handle);
    if (found == handles_.end()) {
      std::move(callback).Run(AutomationStatus::kOk, ErrorJson(kDisposed), {});
      return;
    }
    const int32_t world_id = found->second.world;
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    v8::HandleScope scope(isolate);
    v8::Local<v8::Context> context = Context(world_id);
    if (context.IsEmpty()) {
      std::move(callback).Run(AutomationStatus::kGone, std::string(), {});
      return;
    }
    v8::Context::Scope context_scope(context);
    v8::Local<v8::Value> argv[] = {found->second.value.Get(isolate),
                                   v8::Boolean::New(isolate, plain)};
    Invoke(world_id, "jsonValue", argv, std::move(callback));
  }

  void Select(AutomationWorld world,
              const std::string& selector,
              bool strict,
              int32_t scope_handle,
              bool all,
              SelectCallback callback) override {
    const int32_t world_id = WorldId(world);
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    v8::HandleScope scope(isolate);
    v8::Local<v8::Context> context = Context(world_id);
    if (context.IsEmpty()) {
      std::move(callback).Run(AutomationStatus::kGone, std::string(), {});
      return;
    }
    v8::Context::Scope context_scope(context);
    v8::Local<v8::Value> scope_value = v8::Undefined(isolate);
    if (scope_handle) {
      v8::Local<v8::Array> values =
          Values(isolate, context, {scope_handle});
      scope_value = values->Get(context, 0).ToLocalChecked();
    }
    v8::Local<v8::Value> argv[] = {String(isolate, selector),
                                   v8::Boolean::New(isolate, strict),
                                   scope_value, v8::Boolean::New(isolate, all)};
    Invoke(world_id, "select", argv, std::move(callback));
  }

  void Expose(const std::vector<std::string>& names) override {
    std::vector<std::string> added;
    for (const std::string& name : names) {
      if (std::find(exposed_.begin(), exposed_.end(), name) == exposed_.end()) {
        added.push_back(name);
      }
    }
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    v8::HandleScope scope(isolate);
    v8::Local<v8::Context> context = Context(0);
    if (!context.IsEmpty()) {
      v8::Context::Scope context_scope(context);
      for (const std::string& previous : exposed_) {
        if (std::find(names.begin(), names.end(), previous) == names.end()) {
          binding_generations_.erase(previous);
          std::ignore = context->Global()->Delete(context, String(isolate, previous));
        }
      }
      if (!added.empty())
        Define(context, added);
    }
    exposed_ = names;
  }

  // window[@name](...args): a promise of what the application answers.
  static void Called(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* isolate = info.GetIsolate();
    v8::Local<v8::Context> context = isolate->GetCurrentContext();
    v8::Local<v8::Array> data = info.Data().As<v8::Array>();
    const int64_t agent_id = static_cast<int64_t>(
        data->Get(context, 0).ToLocalChecked().As<v8::Number>()->Value());
    const std::string name =
        Text(isolate, data->Get(context, 1).ToLocalChecked());
    v8::Local<v8::Promise::Resolver> resolver;
    if (!v8::Promise::Resolver::New(context).ToLocal(&resolver))
      return;
    info.GetReturnValue().Set(resolver->GetPromise());
    v8::Local<v8::Array> args = v8::Array::New(isolate, info.Length());
    for (int i = 0; i < info.Length(); ++i)
      args->Set(context, i, info[i]).Check();
    v8::Local<v8::String> json;
    if (!v8::JSON::Stringify(context, args).ToLocal(&json)) {
      resolver->Reject(context, v8::Exception::TypeError(String(
                                    isolate, "the arguments are not JSON")))
          .Check();
      return;
    }
    auto found = Agents().find(agent_id);
    if (found == Agents().end() || !found->second) {
      resolver->Reject(context, v8::Exception::Error(String(
                                    isolate, "the page is closing")))
          .Check();
      return;
    }
    const uint32_t generation = data->Get(context, 2).ToLocalChecked().As<v8::Uint32>()->Value();
    auto registered = found->second->binding_generations_.find(name);
    if (registered == found->second->binding_generations_.end() || registered->second != generation) {
      resolver->Reject(context, v8::Exception::Error(String(isolate, "binding was disposed"))).Check();
      return;
    }
    found->second->CallHost(name, Text(isolate, json), isolate, context,
                            resolver);
  }

  void SetMediaType(const std::string& media) override {
    if (blink::WebView* view = render_frame()->GetWebView())
      blink::SetMediaTypeOverride(view, blink::WebString::FromUtf8(media));
  }

  // The agents of this renderer, for its workers.
  static std::set<Agent*>& All() {
    static base::NoDestructor<std::set<Agent*>> all;
    return *all;
  }

  // A worker started or ended: told to the browser by the frames of its
  // origin (a worker is its creator's origin).
  void Worker(int32_t worker, const std::string& url, bool started) {
    if (!started) {
      if (reported_workers_.erase(worker))
        Host()->WorkerEnded(worker);
      return;
    }
    const url::Origin origin = url::Origin::Create(GURL(url));
    const url::Origin mine = render_frame()->GetWebFrame()->GetSecurityOrigin();
    if (origin != mine)
      return;
    reported_workers_.insert(worker);
    Host()->WorkerStarted(worker, url);
  }

  void WorkerEvaluate(int32_t worker,
                      const std::string& expression,
                      AutomationExpression kind,
                      const std::string& args,
                      WorkerEvaluateCallback callback) override {
    int thread = 0;
    {
      base::AutoLock lock(WorkersLock());
      auto found = Workers().find(worker);
      if (found == Workers().end()) {
        std::move(callback).Run(AutomationStatus::kGone, std::string());
        return;
      }
      thread = found->second.thread;
    }
    std::string source =
        "(async () => { const args = " + args + "; let value = (\n" +
        expression + "\n);\n";
    source += kind == AutomationExpression::kFunction
                  ? "value = await value(...args);"
              : kind == AutomationExpression::kValue
                  ? "value = await value;"
                  : "value = typeof value === 'function' ? await value(...args) "
                    ": await value;";
    source +=
        "\nreturn JSON.stringify(value === undefined ? null : value); })()"
        ".catch(e => { throw e instanceof Error ? e.name + ': ' + e.message : "
        "'Error: ' + String(e); })";
    content::WorkerThread::PostTask(
        thread, base::BindOnce(&EvaluateInWorker, std::move(source),
                               base::BindOnce(
                                   [](WorkerEvaluateCallback callback,
                                      AutomationStatus status, std::string result) {
                                     std::move(callback).Run(status, result);
                                   },
                                   std::move(callback))));
  }

  void Release(const std::vector<int32_t>& handles) override {
    for (int32_t handle : handles)
      handles_.erase(handle);
  }

  void InsertText(const std::string& text,
                  InsertTextCallback callback) override {
    Commit(text);
    std::move(callback).Run();
  }

  void SetInputFiles(int32_t handle,
                     const std::vector<std::string>& paths,
                     SetInputFilesCallback callback) override {
    blink::WebElement element = ElementOf(handle);
    blink::WebInputElement input =
        element.IsNull() ? blink::WebInputElement()
                         : element.DynamicTo<blink::WebInputElement>();
    if (input.IsNull()) {
      std::move(callback).Run(false);
      return;
    }
    std::vector<blink::WebString> files;
    for (const std::string& path : paths)
      files.push_back(blink::WebString::FromUtf8(path));
    input.SetFilesFromPaths(files);
    std::move(callback).Run(true);
  }

  void ContentFrame(int32_t handle, ContentFrameCallback callback) override {
    blink::WebElement element = ElementOf(handle);
    blink::WebFrame* child =
        element.IsNull() ? nullptr : blink::WebFrame::FromFrameOwnerElement(element);
    int32_t index = -1;
    if (child) {
      int32_t i = 0;
      for (blink::WebFrame* frame = render_frame()->GetWebFrame()->FirstChild();
           frame; frame = frame->NextSibling(), ++i) {
        if (frame == child) {
          index = i;
          break;
        }
      }
    }
    std::move(callback).Run(index);
  }

  // Called by the promise's reactions.
  static void Settled(const v8::FunctionCallbackInfo<v8::Value>& info) {
    const int64_t id = static_cast<int64_t>(
        info.Data().As<v8::Number>()->Value());
    auto found = Pending().find(id);
    if (found == Pending().end())
      return;
    base::WeakPtr<Agent> agent = found->second;
    Pending().erase(found);
    if (agent)
      agent->Answer(id, info.GetIsolate(), info[0]);
  }

 private:
  static constexpr char kDisposed[] = "JSHandle is disposed";

  struct Handle {
    int32_t world;
    v8::Global<v8::Value> value;
  };

  struct Call {
    int32_t world;
    Reply reply;
  };

  static std::map<int64_t, base::WeakPtr<Agent>>& Agents() {
    static base::NoDestructor<std::map<int64_t, base::WeakPtr<Agent>>> agents;
    return *agents;
  }

  // The exposed functions @names on @context's global.
  void Define(v8::Local<v8::Context> context,
              const std::vector<std::string>& names) {
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    v8::Context::Scope context_scope(context);
    v8::MicrotasksScope microtasks(context, v8::MicrotasksScope::kDoNotRunMicrotasks);
    if (!id_) {
      static int64_t next_agent = 0;
      id_ = ++next_agent;
      Agents()[id_] = weak_factory_.GetWeakPtr();
    }
    for (const std::string& name : names) {
      v8::Local<v8::Array> data = v8::Array::New(isolate, 2);
      data->Set(context, 0, v8::Number::New(isolate, static_cast<double>(id_)))
          .Check();
      data->Set(context, 1, String(isolate, name)).Check();
      const uint32_t generation = ++next_binding_generation_;
      binding_generations_[name] = generation;
      data->Set(context, 2, v8::Integer::NewFromUnsigned(isolate, generation)).Check();
      v8::Local<v8::Function> function;
      if (v8::Function::New(context, &Agent::Called, data).ToLocal(&function)) {
        std::ignore =
            context->Global()->Set(context, String(isolate, name), function);
      }
    }
  }

  mojom::AutomationHost* Host() {
    if (!host_.is_bound())
      render_frame()->GetRemoteAssociatedInterfaces()->GetInterface(&host_);
    return host_.get();
  }

  void CallHost(const std::string& name,
                const std::string& args,
                v8::Isolate* isolate,
                v8::Local<v8::Context> context,
                v8::Local<v8::Promise::Resolver> resolver) {
    Host();
    auto kept = std::make_unique<Binding>();
    kept->context.Reset(isolate, context);
    kept->resolver.Reset(isolate, resolver);
    host_->BindingCalled(
        name, args,
        base::BindOnce(
            [](std::unique_ptr<Binding> kept, bool ok,
               const std::string& result) {
              v8::Isolate* isolate = v8::Isolate::GetCurrent();
              v8::HandleScope scope(isolate);
              v8::Local<v8::Context> context = kept->context.Get(isolate);
              if (context.IsEmpty())
                return;
              v8::Context::Scope context_scope(context);
              v8::MicrotasksScope microtasks(
                  context, v8::MicrotasksScope::kRunMicrotasks);
              v8::Local<v8::Promise::Resolver> resolver =
                  kept->resolver.Get(isolate);
              if (!ok) {
                std::ignore = resolver->Reject(
                    context, v8::Exception::Error(String(isolate, result)));
                return;
              }
              v8::Local<v8::Value> value;
              if (!v8::JSON::Parse(context, String(isolate, result))
                       .ToLocal(&value)) {
                value = v8::Undefined(isolate);
              }
              std::ignore = resolver->Resolve(context, value);
            },
            std::move(kept)));
  }

  struct Binding {
    v8::Global<v8::Context> context;
    v8::Global<v8::Promise::Resolver> resolver;
  };

  static int32_t WorldId(AutomationWorld world) {
    return world == AutomationWorld::kMain ? 0 : kUtilityWorldId;
  }

  static std::string ErrorJson(const std::string& message) {
    std::string escaped;
    for (char c : message) {
      switch (c) {
        case '"':
          escaped += "\\\"";
          break;
        case '\\':
          escaped += "\\\\";
          break;
        case '\n':
          escaped += "\\n";
          break;
        default:
          if (static_cast<unsigned char>(c) < 0x20)
            escaped += ' ';
          else
            escaped += c;
      }
    }
    return "{\"ok\":false,\"error\":\"" + escaped + "\"}";
  }

  void Bind(mojo::PendingAssociatedReceiver<mojom::AutomationAgent> receiver) {
    receivers_.Add(this, std::move(receiver));
  }

  v8::Local<v8::Context> Context(int32_t world_id) {
    blink::WebLocalFrame* frame = render_frame()->GetWebFrame();
    // A frame that has not committed its document (a navigation's new
    // frame, which the browser tells of exposed functions at once) has no
    // context to give: asking for one would make it, and abort the load.
    // DidCreateScriptContext() does it once the document commits.
    if (frame->IsProvisional())
      return v8::Local<v8::Context>();
    if (world_id == 0)
      return frame->MainWorldScriptContext();
    return frame->GetScriptContextFromWorldId(v8::Isolate::GetCurrent(),
                                              world_id);
  }

  static v8::Local<v8::Value> Kind(v8::Isolate* isolate,
                                   AutomationExpression kind) {
    switch (kind) {
      case AutomationExpression::kFunction:
        return v8::True(isolate);
      case AutomationExpression::kValue:
        return v8::False(isolate);
      default:
        return v8::Undefined(isolate);
    }
  }

  // The world's runner, made the first time a document needs it.
  v8::Local<v8::Object> Runner(v8::Isolate* isolate,
                               v8::Local<v8::Context> context,
                               int32_t world_id) {
    auto found = runners_.find(world_id);
    if (found != runners_.end())
      return found->second.Get(isolate);
    const std::string_view source(
        reinterpret_cast<const char*>(kAutomationScript),
        sizeof(kAutomationScript));
    v8::TryCatch try_catch(isolate);
    v8::MicrotasksScope microtasks(context, v8::MicrotasksScope::kDoNotRunMicrotasks);
    v8::Local<v8::Script> script;
    v8::Local<v8::Value> made;
    if (!v8::Script::Compile(context, String(isolate, source)).ToLocal(&script) ||
        !script->Run(context).ToLocal(&made) || !made->IsFunction()) {
      LOG(ERROR) << "zebkit: the injected script failed to start: "
                 << Text(isolate, try_catch.Exception());
      return v8::Local<v8::Object>();
    }
    v8::Local<v8::Value> argv[] = {v8::Boolean::New(isolate, world_id != 0)};
    v8::Local<v8::Value> runner;
    if (!made.As<v8::Function>()
             ->Call(context, v8::Undefined(isolate), 1, argv)
             .ToLocal(&runner) ||
        !runner->IsObject()) {
      LOG(ERROR) << "zebkit: the injected script failed to start: "
                 << Text(isolate, try_catch.Exception());
      return v8::Local<v8::Object>();
    }
    runners_[world_id].Reset(isolate, runner.As<v8::Object>());
    return runner.As<v8::Object>();
  }

  // @expression's value, compiled in @context without eval(): a function's
  // source as an expression, else a script whose value is the last
  // statement's, as DevTools' console has it.
  v8::Local<v8::Value> Compile(v8::Isolate* isolate,
                               v8::Local<v8::Context> context,
                               const std::string& expression,
                               AutomationExpression kind,
                               std::string* error) {
    v8::TryCatch try_catch(isolate);
    v8::MicrotasksScope microtasks(context, v8::MicrotasksScope::kDoNotRunMicrotasks);
    v8::Local<v8::Script> script;
    v8::Local<v8::Value> value;
    if (kind != AutomationExpression::kValue &&
        v8::Script::Compile(context, String(isolate, "(" + expression + "\n)"))
            .ToLocal(&script) &&
        script->Run(context).ToLocal(&value)) {
      return value;
    }
    if (try_catch.HasCaught() && kind == AutomationExpression::kFunction &&
        !script.IsEmpty()) {
      *error = Text(isolate, try_catch.Exception());
      return v8::Local<v8::Value>();
    }
    try_catch.Reset();
    if (v8::Script::Compile(context, String(isolate, expression))
            .ToLocal(&script) &&
        script->Run(context).ToLocal(&value)) {
      return value;
    }
    *error = try_catch.HasCaught() ? Text(isolate, try_catch.Exception())
                                   : std::string("SyntaxError");
    return v8::Local<v8::Value>();
  }

  // The values of @handles in @context: a node of the other world is the
  // same node in this one, as Playwright adopts an ElementHandle; another
  // object cannot cross.
  v8::Local<v8::Array> Values(v8::Isolate* isolate,
                              v8::Local<v8::Context> context,
                              const std::vector<int32_t>& handles) {
    const int32_t world_id = WorldIdOf(context);
    v8::Local<v8::Array> values =
        v8::Array::New(isolate, static_cast<int>(handles.size()));
    for (size_t i = 0; i < handles.size(); ++i) {
      v8::Local<v8::Value> value = v8::Undefined(isolate);
      auto found = handles_.find(handles[i]);
      if (found != handles_.end()) {
        value = found->second.value.Get(isolate);
        if (found->second.world != world_id) {
          blink::WebElement element =
              blink::WebElement::FromV8Value(isolate, value);
          value = element.IsNull() ? v8::Local<v8::Value>(v8::Undefined(isolate))
                                   : element.ToV8Value(isolate);
        }
      }
      values->Set(context, static_cast<uint32_t>(i), value).Check();
    }
    return values;
  }

  int32_t WorldIdOf(v8::Local<v8::Context> context) {
    return render_frame()->GetWebFrame()->GetScriptContextWorldId(context);
  }

  blink::WebElement ElementOf(int32_t handle) {
    auto found = handles_.find(handle);
    if (found == handles_.end())
      return blink::WebElement();
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    v8::HandleScope scope(isolate);
    v8::Local<v8::Context> context = Context(found->second.world);
    if (context.IsEmpty())
      return blink::WebElement();
    v8::Context::Scope context_scope(context);
    return blink::WebElement::FromV8Value(isolate,
                                          found->second.value.Get(isolate));
  }

  // Calls the runner's @method; it answers [json, handles, previews], now
  // or as a promise.
  void Invoke(int32_t world_id,
              const char* method,
              base::span<v8::Local<v8::Value>> argv,
              Reply reply) {
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    v8::Local<v8::Context> context = isolate->GetCurrentContext();
    v8::Local<v8::Object> runner = Runner(isolate, context, world_id);
    if (runner.IsEmpty()) {
      std::move(reply).Run(AutomationStatus::kError,
                           "Error: the injected script failed to start", {});
      return;
    }
    v8::Local<v8::Value> function;
    if (!runner->Get(context, String(isolate, method)).ToLocal(&function) ||
        !function->IsFunction()) {
      std::move(reply).Run(AutomationStatus::kError,
                           std::string("Error: no runner method ") + method, {});
      return;
    }
    v8::TryCatch try_catch(isolate);
    v8::Local<v8::Value> result;
    if (!render_frame()
             ->GetWebFrame()
             ->CallFunctionEvenIfScriptDisabled(
                 function.As<v8::Function>(), runner,
                 static_cast<int>(argv.size()), argv.data())
             .ToLocal(&result)) {
      std::move(reply).Run(AutomationStatus::kError,
                           try_catch.HasCaught()
                               ? Text(isolate, try_catch.Exception())
                               : std::string("Error: the call failed"),
                           {});
      return;
    }
    static int64_t next_id = 0;
    const int64_t id = ++next_id;
    calls_[id] = {world_id, std::move(reply)};
    if (!result->IsPromise()) {
      Answer(id, isolate, result);
      return;
    }
    Pending()[id] = weak_factory_.GetWeakPtr();
    v8::Local<v8::Function> settled =
        v8::Function::New(context, &Agent::Settled,
                          v8::Number::New(isolate, static_cast<double>(id)))
            .ToLocalChecked();
    std::ignore = result.As<v8::Promise>()->Then(context, settled, settled);
  }

  // The runner's answer for call @id: its handles kept, the JSON as it is.
  void Answer(int64_t id, v8::Isolate* isolate, v8::Local<v8::Value> answer) {
    auto found = calls_.find(id);
    if (found == calls_.end())
      return;
    Call call = std::move(found->second);
    calls_.erase(found);
    v8::HandleScope scope(isolate);
    v8::Local<v8::Context> context = isolate->GetCurrentContext();
    if (!answer->IsArray()) {
      std::move(call.reply).Run(AutomationStatus::kError,
                                "Error: " + Text(isolate, answer), {});
      return;
    }
    v8::Local<v8::Array> parts = answer.As<v8::Array>();
    const std::string json =
        Text(isolate, parts->Get(context, 0).ToLocalChecked());
    std::vector<AutomationHandlePtr> handles;
    v8::Local<v8::Value> values = parts->Get(context, 1).ToLocalChecked();
    v8::Local<v8::Value> previews = parts->Get(context, 2).ToLocalChecked();
    if (values->IsArray() && previews->IsArray()) {
      v8::Local<v8::Array> list = values.As<v8::Array>();
      for (uint32_t i = 0; i < list->Length(); ++i) {
        v8::Local<v8::Value> value = list->Get(context, i).ToLocalChecked();
        const int32_t handle = next_handle_++;
        handles_[handle] = {call.world, v8::Global<v8::Value>(isolate, value)};
        handles.push_back(AutomationHandle::New(
            handle, !blink::WebElement::FromV8Value(isolate, value).IsNull(),
            Text(isolate, previews.As<v8::Array>()->Get(context, i).ToLocalChecked())));
      }
    }
    std::move(call.reply).Run(AutomationStatus::kOk, json, std::move(handles));
  }

  // Text a step asks for is typed before it answers: fill() is one call.
  void StepAnswered(StepCallback callback,
                    AutomationStatus status,
                    const std::string& result,
                    std::vector<AutomationHandlePtr> handles) {
    if (status == AutomationStatus::kOk &&
        result.find("\"insertText\"") != std::string::npos) {
      std::optional<base::Value> value = base::JSONReader::Read(result, base::JSON_PARSE_RFC);
      if (value && value->is_dict()) {
        if (const std::string* text = value->GetDict().FindStringByDottedPath(
                "value.result.insertText")) {
          Commit(*text);
        }
      }
    }
    std::move(callback).Run(status, result, std::move(handles));
  }

  // As an IME commits text: CDP's Input.insertText ends here
  // (WidgetInputHandler::ImeCommitText), in the frame that has the focus.
  void Commit(const std::string& text) {
    blink::WebFrameWidget* widget =
        render_frame()->GetWebFrame()->LocalRoot()->FrameWidget();
    if (!widget)
      return;
    if (blink::WebInputMethodController* controller =
            widget->GetActiveWebInputMethodController()) {
      controller->CommitText(blink::WebString::FromUtf8(text), {},
                             blink::WebRange(), 0);
    }
  }

  std::vector<std::string> exposed_;
  uint32_t next_binding_generation_ = 0;
  std::map<std::string, uint32_t> binding_generations_;
  std::set<int32_t> reported_workers_;
  int64_t id_ = 0;
  mojo::AssociatedRemote<mojom::AutomationHost> host_;
  std::map<int32_t, v8::Global<v8::Object>> runners_;
  std::map<int32_t, Handle> handles_;
  int32_t next_handle_ = 1;
  std::map<int64_t, Call> calls_;
  mojo::AssociatedReceiverSet<mojom::AutomationAgent> receivers_;
  base::WeakPtrFactory<Agent> weak_factory_{this};
};

void NotifyWorker(int32_t worker, std::string url, bool started) {
  for (Agent* agent : Agent::All())
    agent->Worker(worker, url, started);
}

}  // namespace

void InstallAutomationAgent(content::RenderFrame* render_frame) {
  if (!MainThread())
    MainThread() = base::SequencedTaskRunner::GetCurrentDefault();
  new Agent(render_frame);
}

}  // namespace zebkit
