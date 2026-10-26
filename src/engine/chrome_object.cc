// window.chrome in pages, as Chrome's renderer makes it: chrome.loadTimes()
// and chrome.csi(), the timing of the page's load, by Chrome's own code
// (chrome/renderer/loadtimes_bindings.cc, built here as it is). Sites read
// window.chrome to know that they run in Chrome; the content shell has none.
// The legacy chrome.app queries report that no hosted app is installed;
// these queries need no extension runtime. The contract is in
// chrome/common/extensions/api/app.json and
// chrome/renderer/extensions/api/app_hooks_delegate.cc.

#include "base/functional/bind.h"
#include "base/task/single_thread_task_runner.h"
#include "chrome/renderer/loadtimes_bindings.h"
#include "content/public/renderer/chrome_object_extensions_utils.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_frame_observer.h"
#include "gin/converter.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-template.h"
#include "zebkit/engine/engine.h"

namespace zebkit {

namespace {

// Chromium exposes these hosted-app queries to ordinary pages. Zebkit has
// no hosted-app registry, so the actual states are not installed / cannot run.
bool CheckNoArguments(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (info.Length() == 0)
    return true;
  info.GetIsolate()->ThrowException(v8::Exception::TypeError(
      gin::StringToV8(info.GetIsolate(), "No matching chrome.app signature")));
  return false;
}

void NotInstalled(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (CheckNoArguments(info))
    info.GetReturnValue().Set(false);
}

void NoDetails(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (CheckNoArguments(info))
    info.GetReturnValue().Set(v8::Null(info.GetIsolate()));
}

void CannotRun(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (CheckNoArguments(info))
    info.GetReturnValue().Set(gin::StringToV8(info.GetIsolate(), "cannot_run"));
}

void IsInstalled(v8::Local<v8::Name>,
                 const v8::PropertyCallbackInfo<v8::Value>& info) {
  info.GetReturnValue().Set(false);
}

void IgnoreInstallAssignment(v8::Local<v8::Name>, v8::Local<v8::Value>,
                             const v8::PropertyCallbackInfo<v8::Boolean>&) {}

void InstallState(const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto* isolate = info.GetIsolate();
  if (info.Length() != 1 || !info[0]->IsFunction()) {
    isolate->ThrowException(v8::Exception::TypeError(
        gin::StringToV8(isolate, "chrome.app.installState requires a callback")));
    return;
  }
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](v8::Isolate* isolate, v8::Global<v8::Context> saved_context,
             v8::Global<v8::Function> saved_callback) {
            v8::HandleScope handles(isolate);
            auto context = saved_context.Get(isolate);
            auto* frame = blink::WebLocalFrame::FrameForContext(context);
            // Navigation/removal must not run a callback in an old document.
            if (!frame || frame->MainWorldScriptContext() != context)
              return;
            v8::Context::Scope scope(context);
            v8::Local<v8::Value> args[] = {
                gin::StringToV8(isolate, "not_installed")};
            // Blink checks script execution policy, runs microtasks and
            // reports callback exceptions to the page as other async APIs do.
            (void)frame->ExecuteMethodAndReturnValue(
                saved_callback.Get(isolate), context->Global(), 1, args);
          },
          isolate, v8::Global<v8::Context>(isolate, isolate->GetCurrentContext()),
          v8::Global<v8::Function>(isolate, info[0].As<v8::Function>())));
}

void InstallLegacyApp(v8::Local<v8::Context> context) {
  auto* isolate = v8::Isolate::GetCurrent();
  v8::MicrotasksScope microtasks(isolate, context->GetMicrotaskQueue(),
                               v8::MicrotasksScope::kDoNotRunMicrotasks);
  v8::Context::Scope scope(context);
  auto app_template = v8::ObjectTemplate::New(isolate);
  app_template->SetNativeDataProperty(gin::StringToSymbol(isolate, "isInstalled"),
                                     IsInstalled, IgnoreInstallAssignment);
  auto app = app_template->NewInstance(context).ToLocalChecked();
  const struct {
    const char* name;
    v8::FunctionCallback callback;
  } methods[] = {{"getIsInstalled", NotInstalled}, {"getDetails", NoDetails},
                 {"runningState", CannotRun}, {"installState", InstallState}};
  for (auto method : methods) {
    app->Set(context, gin::StringToV8(isolate, method.name),
             v8::Function::New(context, method.callback).ToLocalChecked())
        .Check();
  }
  auto install_states = v8::Object::New(isolate);
  install_states
      ->Set(context, gin::StringToV8(isolate, "NOT_INSTALLED"),
            gin::StringToV8(isolate, "not_installed"))
      .Check();
  install_states
      ->Set(context, gin::StringToV8(isolate, "INSTALLED"),
            gin::StringToV8(isolate, "installed"))
      .Check();
  install_states
      ->Set(context, gin::StringToV8(isolate, "DISABLED"),
            gin::StringToV8(isolate, "disabled"))
      .Check();
  app->Set(context, gin::StringToV8(isolate, "InstallState"), install_states)
      .Check();
  auto running_states = v8::Object::New(isolate);
  running_states
      ->Set(context, gin::StringToV8(isolate, "RUNNING"),
            gin::StringToV8(isolate, "running"))
      .Check();
  running_states
      ->Set(context, gin::StringToV8(isolate, "CANNOT_RUN"),
            gin::StringToV8(isolate, "cannot_run"))
      .Check();
  running_states
      ->Set(context, gin::StringToV8(isolate, "READY_TO_RUN"),
            gin::StringToV8(isolate, "ready_to_run"))
      .Check();
  app->Set(context, gin::StringToV8(isolate, "RunningState"), running_states)
      .Check();
  content::GetOrCreateChromeObject(isolate, context)
      ->Set(context, gin::StringToV8(isolate, "app"), app)
      .Check();
}

// In every script context of the frame, as ChromeRenderFrameObserver
// installs them. Deletes itself with the frame.
class ChromeObject : public content::RenderFrameObserver {
 public:
  explicit ChromeObject(content::RenderFrame* render_frame)
      : content::RenderFrameObserver(render_frame) {}

  void DidCreateScriptContext(v8::Local<v8::Context> context,
                              int32_t world_id) override {
    LoadTimesBindings::Install(context);
    if (world_id == 0)
      InstallLegacyApp(context);
  }

  void OnDestruct() override { delete this; }
};

}  // namespace

void InstallChromeObject(content::RenderFrame* render_frame) {
  new ChromeObject(render_frame);
}

}  // namespace zebkit
