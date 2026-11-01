// zebkit's automation, the browser's side (<zebkit/automation.h>,
// docs/AUTOMATION.md): Playwright's commands, with what Playwright's server
// does around each (frames.ts, dom.ts, page.ts, input.ts, crInput.ts,
// frameSelectors.ts), and each frame's agent (automation_renderer.cc) for
// the steps in the page. No CDP: input goes to the page's widgets,
// navigations through its controller, screenshots from its surface.

#ifdef UNSAFE_BUFFERS_BUILD
// The C API takes and returns C strings.
#pragma allow_unsafe_buffers
#endif

#include "zebkit/engine/include/zebkit/automation.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted_memory.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "components/version_info/version_info.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "build/build_config.h"
#include "components/input/native_web_keyboard_event.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "content/public/browser/file_select_listener.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/page.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/child_process_security_policy.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_observer.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "content/public/browser/zebkit_automation.h"
#include "content/public/browser/login_delegate.h"
#if defined(USE_AURA)
#include "ui/aura/window.h"
#include "ui/aura/window_tree_host.h"
#include "ui/compositor/compositor.h"
#endif
#include "third_party/blink/public/common/input/web_touch_event.h"
#include "third_party/blink/public/common/web_preferences/web_preferences.h"
#include "ui/events/base_event_utils.h"
#include "base/files/scoped_temp_dir.h"
#include "components/download/public/common/download_interrupt_reasons.h"
#include "components/download/public/common/download_item.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/download_item_utils.h"
#include "content/public/browser/download_manager.h"
#include "content/shell/browser/shell.h"
#include "content/shell/browser/shell_download_manager_delegate.h"
#include "content/shell/browser/shell_permission_manager.h"
#include "third_party/blink/public/common/permissions/permission_utils.h"
#include "url/origin.h"
#include "mojo/public/cpp/bindings/associated_receiver_set.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "third_party/blink/public/common/input/web_gesture_event.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "third_party/blink/public/common/input/web_mouse_wheel_event.h"
#include "third_party/blink/public/mojom/choosers/file_chooser.mojom.h"
#include "third_party/blink/public/mojom/devtools/console_message.mojom-shared.h"
#include "ui/events/keycodes/dom/keycode_converter.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/gfx/geometry/rect_f.h"
#include "zebkit/engine/automation.h"
#include "zebkit/engine/permission_types.h"
#include "content/public/browser/embedder_permission_hooks.h"
#include "zebkit/engine/automation.mojom.h"
#include "zebkit/engine/automation_context.h"
#include "zebkit/engine/automation_network.h"
#include "zebkit/engine/automation_scripts.h"
#include "zebkit/engine/automation_trace.h"
#include "ui/gfx/codec/jpeg_codec.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/include/zebkit/page.h"
#include "zebkit/engine/page_hooks.h"
#include "zebkit/engine/page_internal.h"
#include "zebkit/engine/playwright/us_keyboard_layout.h"
#include "zebkit/engine/view.h"

namespace zebkit {

namespace {

using mojom::AutomationExpression;
using mojom::AutomationHandlePtr;
using mojom::AutomationStatus;
using mojom::AutomationWorld;
using Type = blink::WebInputEvent::Type;
using Button = blink::WebPointerProperties::Button;

constexpr int kDefaultTimeoutMs = 30000;
// Playwright's messages.
constexpr char kClosed[] = "Target page, context or browser has been closed";
constexpr char kNavigated[] =
    "Execution context was destroyed, most likely because of a navigation";
constexpr char kDetached[] = "Element is not attached to the DOM";
// Not Playwright's: it has no such case, as its round trips hide it.
constexpr char kInputDropped[] =
    "The browser dropped the input: the page's widget did not take input";
constexpr char kDisposed[] = "JSHandle is disposed";
// The binding Playwright's WebSocket mock reports through.
constexpr char kWebSocketBinding[] = "__pwWebSocketBinding";
constexpr char kFrameDetached[] = "Frame was detached";
// How a selector enters the next frame (frameSelectors.ts).
constexpr char kEnterFrame[] = " >> internal:control=enter-frame >> ";

std::string Json(const base::Value& value) {
  return base::WriteJson(value).value_or("null");
}

std::optional<base::Value> Parse(const std::string& json) {
  return base::JSONReader::Read(json, base::JSON_PARSE_RFC);
}

// ---- Keys, as input.ts and crInput.ts make them -------------------------

const playwright::KeyEntry* FindKey(std::string_view name) {
  for (const playwright::KeyEntry& entry : playwright::kUsKeyboardLayout) {
    if (name == entry.name)
      return &entry;
  }
  return nullptr;
}

bool IsModifier(std::string_view key) {
  return key == "Alt" || key == "Control" || key == "Meta" || key == "Shift";
}

int ModifierBit(std::string_view key) {
  if (key == "Alt")
    return blink::WebInputEvent::kAltKey;
  if (key == "Control")
    return blink::WebInputEvent::kControlKey;
  if (key == "Meta")
    return blink::WebInputEvent::kMetaKey;
  if (key == "Shift")
    return blink::WebInputEvent::kShiftKey;
  return 0;
}

// input.ts's resolveSmartModifierString().
std::string ResolveSmartModifier(const std::string& key) {
  if (key == "ControlOrMeta") {
#if BUILDFLAG(IS_MAC)
    return "Meta";
#else
    return "Control";
#endif
  }
  return key;
}

// Keyboard.press()'s split(): "Shift++" is Shift and +.
std::vector<std::string> SplitKeys(const std::string& keys) {
  std::vector<std::string> result;
  std::string building;
  for (char c : keys) {
    if (c == '+' && !building.empty()) {
      result.push_back(building);
      building.clear();
    } else {
      building += c;
    }
  }
  result.push_back(building);
  return result;
}

// The code points of @text, each as its UTF-8.
std::vector<std::string> CodePoints(const std::string& text) {
  std::vector<std::string> result;
  for (size_t i = 0; i < text.size();) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    const size_t length =
        c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xe ? 3 : 4;
    result.push_back(text.substr(i, length));
    i += length;
  }
  return result;
}

Button ButtonOf(const std::string& name) {
  if (name == "right")
    return Button::kRight;
  if (name == "middle")
    return Button::kMiddle;
  return Button::kLeft;
}

int ButtonBit(Button button) {
  switch (button) {
    case Button::kLeft:
      return blink::WebInputEvent::kLeftButtonDown;
    case Button::kRight:
      return blink::WebInputEvent::kRightButtonDown;
    case Button::kMiddle:
      return blink::WebInputEvent::kMiddleButtonDown;
    default:
      return 0;
  }
}

// ---- A command ----------------------------------------------------------

// One command, from zebkit_automation_call() to its answer.
class Call {
 public:
  Call(zebkit_view* view,
       std::string method,
       base::DictValue params,
       zebkit_automation_func func,
       void* data)
      : view_(view),
        method_(std::move(method)),
        params_(std::move(params)),
        func_(func),
        data_(data) {
    if (std::optional<double> timeout = params_.FindDouble("timeout"))
      timeout_ms_ = static_cast<int>(*timeout);
    if (timeout_ms_ > 0) {
      deadline_ = base::TimeTicks::Now() + base::Milliseconds(timeout_ms_);
      timer_.Start(FROM_HERE, base::Milliseconds(timeout_ms_),
                   base::BindOnce(&Call::TimedOut, base::Unretained(this)));
    }
  }

  const base::DictValue& params() const { return params_; }
  zebkit_view* view() const { return view_; }
  const std::string& method() const { return method_; }
  // "click" of "Frame.click" and "ElementHandle.click".
  std::string Name() const {
    const size_t dot = method_.find('.');
    return dot == std::string::npos ? method_ : method_.substr(dot + 1);
  }
  bool OnHandle() const {
    return method_.rfind("ElementHandle.", 0) == 0 ||
           method_.rfind("JSHandle.", 0) == 0;
  }

  std::string String(std::string_view key) const {
    const std::string* value = params_.FindString(key);
    return value ? *value : std::string();
  }
  double Number(std::string_view key, double fallback = 0) const {
    return params_.FindDouble(key).value_or(fallback);
  }
  bool Flag(std::string_view key) const {
    return params_.FindBool(key).value_or(false);
  }
  bool Has(std::string_view key) const { return params_.Find(key); }

  // What is left of the timeout for a step in the page; 0 for none.
  int Remaining() const {
    if (timeout_ms_ <= 0)
      return 0;
    const int left = static_cast<int>(
        (deadline_ - base::TimeTicks::Now()).InMilliseconds());
    return left > 0 ? left : 1;
  }
  bool Expired() const {
    return timeout_ms_ > 0 && base::TimeTicks::Now() >= deadline_;
  }

  void Log(const std::string& line) {
    if (log_.empty() || log_.back() != line)
      log_.push_back(line);
  }
  void Log(const base::ListValue* lines) {
    if (!lines)
      return;
    for (const base::Value& line : *lines) {
      if (line.is_string())
        Log(line.GetString());
    }
  }

  void Done(base::Value result = base::Value(base::DictValue())) {
    if (finished_)
      return;
    finished_ = true;
    timer_.Stop();
    const std::string json = Json(result);
    if (func_)
      func_(view_, json.c_str(), nullptr, data_);
    Release();
  }

  // An answer that is one value: {value}.
  void Answer(base::Value value) {
    base::DictValue answer;
    answer.Set("value", std::move(value));
    Done(base::Value(std::move(answer)));
  }

  void Fail(const std::string& message) {
    if (finished_)
      return;
    finished_ = true;
    error_ = message;
    timer_.Stop();
    std::string text = message;
    if (!log_.empty()) {
      text += "\nCall log:";
      for (const std::string& line : log_)
        text += "\n  - " + line;
    }
    if (func_)
      func_(view_, nullptr, text.c_str(), data_);
    Release();
  }

  void TimedOut() {
    Fail("Timeout " + base::NumberToString(timeout_ms_) + "ms exceeded.");
  }

  bool finished() const { return finished_; }
  const std::string& error() const { return error_; }

  base::WeakPtr<Call> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

  // Run when the call has its answer.
  void OnRelease(base::OnceClosure closure) {
    on_release_ = std::move(closure);
  }

  // How many times the action was tried, for the scroll alignments.
  int attempt = 0;

 private:
  void Release() {
    if (on_release_)
      std::move(on_release_).Run();
    // After the callers on the stack have returned.
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE,
                                                               this);
  }

  raw_ptr<zebkit_view> view_;
  std::string method_;
  base::DictValue params_;
  zebkit_automation_func func_;
  raw_ptr<void> data_;
  int timeout_ms_ = kDefaultTimeoutMs;
  base::TimeTicks deadline_;
  base::OneShotTimer timer_;
  std::vector<std::string> log_;
  bool finished_ = false;
  std::string error_;
  base::OnceClosure on_release_;
  base::WeakPtrFactory<Call> weak_factory_{this};
};

// What a step answered: its value, with the handles it refers to as
// {"handle": id, "preview": ...}.
using StepDone = base::OnceCallback<void(base::Value value)>;

// A frame a command runs in: its document (@exact), or whatever document
// the frame has when a step runs (@follow, for a selector, which is looked
// for again after a navigation).
struct FrameRef {
  content::GlobalRenderFrameHostId exact;
  content::FrameTreeNodeId node;
  bool follow = false;
};

// What a command acts on: a frame, a selector there, maybe inside an
// element.
struct Target {
  FrameRef frame;
  std::string selector;
  bool strict = false;
  // The agent's handle of the element the selector is inside; 0 for none.
  int32_t scope = 0;
};

using TargetDone = base::OnceCallback<void(Target target)>;

// A handle the browser gives out: an object an agent keeps.
struct HandleRef {
  content::GlobalRenderFrameHostId frame;
  int32_t id = 0;
  bool element = false;
  std::string preview;
};

void WatchDownloads(content::WebContents* contents);

// UI-thread registration IDs are shared by page and context registrations.
std::string RegistrationId(const base::DictValue& params) {
  if (const std::string* id = params.FindString("registration"))
    return *id;
  static uint64_t next = 0;
  return "registration" + base::NumberToString(++next);
}

base::Value RegistrationResult(const std::string& id, const std::string& kind) {
  base::DictValue result;
  result.Set("registration", id);
  result.Set("kind", kind);
  return base::Value(std::move(result));
}

// What a context (a profile) has for all its pages: Browser.newContext's
// options and the BrowserContext commands that change them.
struct ContextState {
  base::DictValue options;
  std::optional<content::GeolocationOverride> geolocation;
  std::optional<base::DictValue> credentials;
  base::ListValue extra_headers;
  bool offline = false;
  // Commands of the context's for each of its pages, new ones too
  // (addInitScript, exposeBinding, routes, subscriptions, clock): their
  // page's method and parameters.
  std::vector<std::pair<std::string, base::DictValue>> replay;
};

std::map<content::BrowserContext*, ContextState>& ContextStates() {
  static base::NoDestructor<std::map<content::BrowserContext*, ContextState>>
      states;
  return *states;
}

// A view's automation: its page's agents and handles, the keyboard's and
// the mouse's state, the navigations commands wait for, and its events.
class Automation : public content::WebContentsObserver,
                   public content::WebContentsUserData<Automation>,
                   public mojom::AutomationHost,
                   public content::RenderWidgetHost::InputEventObserver,
                   public content::RenderWidgetHostObserver {
 public:
  ~Automation() override {
    for (content::RenderWidgetHost* widget : observed_) {
      widget->RemoveInputEventObserver(this);
      widget->RemoveObserver(this);
    }
  }

  bool HasBinding(const std::string& name) const {
    return std::find(exposed_.begin(), exposed_.end(), name) != exposed_.end();
  }

  static Automation* For(zebkit_view* view) {
    content::WebContents* contents = view->web_contents();
    if (!contents)
      return nullptr;
    if (!FromWebContents(contents)) {
      CreateForWebContents(contents);
      // The page's requests from its next factories on: its next document.
      Network::Create(contents,
                      base::BindRepeating(&Automation::Emit,
                                          FromWebContents(contents)->GetWeakPtr()));
      WatchDownloads(contents);
      FromWebContents(contents)->view_ = view;
      FromWebContents(contents)->Started();
    }
    Automation* automation = FromWebContents(contents);
    automation->view_ = view;
    automation->EmulateFocus();
    return automation;
  }

  // A page's first automation: its context's settings, and its frames'
  // requests through the network's proxy from now on (as DevTools' Fetch
  // domain, patches/1038).
  void Started() {
    MainFrame()->ForEachRenderFrameHost([](content::RenderFrameHost* frame) {
      if (frame->IsRenderFrameLive())
        content::UpdateSubresourceLoaderFactories(frame);
    });
    ApplyContext();
  }

  // The context's settings for this page (Browser.newContext's options).
  void ApplyContext() {
    ContextState& state = ContextStates()[web_contents()->GetBrowserContext()];
    if (state.geolocation)
      content::SetGeolocationOverride(web_contents(), state.geolocation);
    if (Network* network = Network::For(web_contents())) {
      network->SetOffline(state.offline);
      network->SetExtraHeaders(state.extra_headers);
    }
    const base::DictValue& options = state.options;
    if (const std::string* agent = options.FindString("userAgent"))
      zebkit_view_set_user_agent(view_, agent->c_str());
    blink::web_pref::WebPreferences prefs =
        web_contents()->GetOrCreateWebPreferences();
    bool changed = false;
    if (options.FindBool("hasTouch").value_or(false)) {
      prefs.touch_event_feature_detection_enabled = true;
      changed = true;
    }
    if (std::optional<bool> enabled = options.FindBool("javaScriptEnabled")) {
      prefs.javascript_enabled = *enabled;
      changed = true;
    }
    if (changed)
      web_contents()->SetWebPreferences(prefs);
    base::DictValue media;
    for (const char* key : {"colorScheme", "reducedMotion", "forcedColors", "contrast"}) {
      if (const std::string* value = options.FindString(key))
        media.Set(key, *value);
    }
    if (!media.empty())
      ContextEmulateMedia(web_contents(), media, base::DoNothing());
    // The context's commands, again for this page, once it has its
    // automation.
    for (const auto& [method, params] : state.replay) {
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE,
          base::BindOnce(
              [](base::WeakPtr<zebkit_view> view, std::string method,
                 std::string params) {
                if (view)
                  zebkit_automation_call(view.get(), method.c_str(),
                                         params.c_str(), nullptr, nullptr);
              },
              view_->GetWeakPtr(), method, Json(base::Value(params.Clone()))));
    }
  }

  // Every page with automation of @profile.
  static std::vector<Automation*> InProfile(content::BrowserContext* profile) {
    std::vector<Automation*> pages;
    for (zebkit_view* view : zebkit_view::Views()) {
      content::WebContents* contents = view->web_contents();
      if (contents && contents->GetBrowserContext() == profile) {
        if (Automation* automation = FromWebContents(contents))
          pages.push_back(automation);
      }
    }
    return pages;
  }

  ContextState& Context() {
    return ContextStates()[web_contents()->GetBrowserContext()];
  }

  // BrowserContext.setGeolocation: {geolocation: {latitude, longitude,
  // accuracy}}, or none for the device's (patches/1038).
  void SetGeolocation(Call* call) {
    const base::DictValue* given = call->params().FindDict("geolocation");
    std::optional<content::GeolocationOverride> position;
    if (given) {
      const std::optional<double> latitude = given->FindDouble("latitude");
      const std::optional<double> longitude = given->FindDouble("longitude");
      if (!latitude || !longitude || *latitude < -90 || *latitude > 90 ||
          *longitude < -180 || *longitude > 180) {
        call->Fail("geolocation: latitude in [-90, 90] and longitude in [-180, 180] expected");
        return;
      }
      position = content::GeolocationOverride{
          *latitude, *longitude, given->FindDouble("accuracy").value_or(0)};
    }
    Context().geolocation = position;
    for (Automation* page : InProfile(web_contents()->GetBrowserContext()))
      content::SetGeolocationOverride(page->web_contents(), position);
    call->Done();
  }

  // BrowserContext.setHTTPCredentials: {httpCredentials: {username,
  // password, origin}}, answering the context's HTTP authentication
  // (CreateAutomationLoginDelegate()).
  void SetHttpCredentials(Call* call) {
    const base::DictValue* credentials = call->params().FindDict("httpCredentials");
    Context().credentials =
        credentials ? std::optional<base::DictValue>(credentials->Clone())
                    : std::nullopt;
    call->Done();
  }

  // BrowserContext.setOffline and setExtraHTTPHeaders: for every page of
  // the context.
  void ContextOffline(Call* call) {
    Context().offline = call->Flag("offline");
    for (Automation* page : InProfile(web_contents()->GetBrowserContext())) {
      if (Network* network = Network::For(page->web_contents()))
        network->SetOffline(call->Flag("offline"));
    }
    call->Done();
  }

  void ContextExtraHeaders(Call* call) {
    const base::ListValue* headers = call->params().FindList("headers");
    Context().extra_headers = headers ? headers->Clone() : base::ListValue();
    for (Automation* page : InProfile(web_contents()->GetBrowserContext())) {
      if (Network* network = Network::For(page->web_contents()))
        network->SetExtraHeaders(Context().extra_headers);
    }
    call->Done();
  }

  // Frames on demand (docs/AUTOMATION.md 4.6): while a command runs on a
  // page without a display, its compositor makes frames as soon as they
  // are asked for instead of at the display's 60 Hz, so that what waits
  // for animation frames (stability, the hit-target check) waits what a
  // frame takes to make.
  void FastFrames(bool fast) {
#if defined(USE_AURA)
    if (!zebkit_is_headless())
      return;
    aura::Window* window = web_contents()->GetNativeView();
    aura::WindowTreeHost* host = window ? window->GetHost() : nullptr;
    if (!host || !host->compositor())
      return;
    host->compositor()->SetDisplayVSyncParameters(
        base::TimeTicks::Now(),
        fast ? base::Milliseconds(2) : base::Hertz(60));
#endif
  }

  // The page has the focus, as Playwright's focus emulation gives a
  // headless page (Emulation.setFocusEmulationEnabled): a view without a
  // window would otherwise never have it (document.hasFocus()).
  void EmulateFocus() {
    if (content::RenderWidgetHost* widget = MainWidget()) {
      if (focused_widget_ != widget) {
        focused_widget_ = widget;
        widget->Focus();
      }
    }
  }

  base::WeakPtr<Automation> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

  using Handler = void (Automation::*)(Call*);
  static const std::map<std::string, Handler>& Handlers();

  void Run(Call* call, Handler handler) {
    if (active_calls_++ == 0)
      FastFrames(true);
    if (tracing_ && call->method().rfind("Tracing.", 0) != 0)
      traced_[call] = tracing_->Before(call->method(), call->params());
    std::erase_if(calls_,
                  [](const base::WeakPtr<Call>& call) { return !call; });
    calls_.push_back(call->GetWeakPtr());
    call->OnRelease(
        base::BindOnce(&Automation::Forget, weak_factory_.GetWeakPtr(), call));
    (this->*handler)(call);
  }

  // What is kept of @call by its address, which a later call may have.
  void Forget(Call* call) {
    watching_.erase(call);
    watched_navigations_.erase(call);
    if (auto traced = traced_.find(call); traced != traced_.end()) {
      if (tracing_) {
        tracing_->After(traced->second, call->error());
        if (tracing_->screenshots())
          TraceScreenshot();
      }
      traced_.erase(traced);
    }
    if (--active_calls_ == 0)
      FastFrames(false);
  }

  void SetEventHandler(zebkit_automation_event_func func, void* data) {
    on_event_ = func;
    on_event_data_ = data;
  }

  void Emit(const std::string& event, base::DictValue params) {
    if (tracing_)
      tracing_->Event(event, params);
    for (auto& [id, har] : hars_)
      har->Record(event, params);
    if (!on_event_)
      return;
    const std::string json = Json(base::Value(std::move(params)));
    on_event_(view_, event.c_str(), json.c_str(), on_event_data_);
  }

  // ---- Frames ----------------------------------------------------------

  static std::string FrameId(content::RenderFrameHost* frame) {
    return "f" + base::NumberToString(frame->GetFrameTreeNodeId().value());
  }

  content::RenderFrameHost* FrameByNode(content::FrameTreeNodeId node) {
    content::RenderFrameHost* frame =
        web_contents()->UnsafeFindFrameByFrameTreeNodeId(node);
    return frame && frame->IsActive() ? frame : nullptr;
  }

  content::RenderFrameHost* FrameById(const std::string& id) {
    int value = 0;
    if (id.size() < 2 || id[0] != 'f' ||
        !base::StringToInt(std::string_view(id).substr(1), &value)) {
      return nullptr;
    }
    return FrameByNode(content::FrameTreeNodeId(value));
  }

  content::RenderFrameHost* MainFrame() {
    return web_contents()->GetPrimaryMainFrame();
  }

  // The frame a command names ("frame"), the main frame by default.
  content::RenderFrameHost* FrameOf(Call* call) {
    const std::string id = call->String("frame");
    return id.empty() ? MainFrame() : FrameById(id);
  }

  content::RenderFrameHost* Resolve(const FrameRef& frame) {
    return frame.follow ? FrameByNode(frame.node)
                        : content::RenderFrameHost::FromID(frame.exact);
  }

  static FrameRef Follow(content::RenderFrameHost* frame) {
    return {frame->GetGlobalId(), frame->GetFrameTreeNodeId(), true};
  }
  static FrameRef Exact(content::RenderFrameHost* frame) {
    return {frame->GetGlobalId(), frame->GetFrameTreeNodeId(), false};
  }

  // The children of @frame, in the frame tree's order.
  std::vector<content::RenderFrameHost*> Children(
      content::RenderFrameHost* frame) {
    std::vector<content::RenderFrameHost*> children;
    frame->ForEachRenderFrameHost([&](content::RenderFrameHost* child) {
      if (child->GetParent() == frame)
        children.push_back(child);
    });
    return children;
  }

  base::DictValue FrameValue(content::RenderFrameHost* frame) {
    base::DictValue value;
    value.Set("frame", FrameId(frame));
    value.Set("name", frame->GetFrameName());
    value.Set("url", frame->GetLastCommittedURL().spec());
    if (content::RenderFrameHost* parent = frame->GetParent())
      value.Set("parentFrame", FrameId(parent));
    return value;
  }

  // ---- Handles ---------------------------------------------------------

  std::string Keep(content::RenderFrameHost* frame,
                   const mojom::AutomationHandle& handle) {
    const std::string id = "h" + base::NumberToString(++next_handle_);
    handles_[id] = {frame->GetGlobalId(), handle.id, handle.is_element,
                    handle.preview};
    return id;
  }

  const HandleRef* FindHandle(const std::string& id) {
    auto found = handles_.find(id);
    return found == handles_.end() ? nullptr : &found->second;
  }

  // The answer's {"$h": i} as the handles the browser gives out: {handle,
  // preview, element}.
  base::Value Expose(base::Value value,
                     const std::vector<AutomationHandlePtr>& handles,
                     const std::vector<std::string>& ids) {
    if (value.is_list()) {
      base::ListValue list;
      for (base::Value& item : value.GetList())
        list.Append(Expose(std::move(item), handles, ids));
      return base::Value(std::move(list));
    }
    if (!value.is_dict())
      return value;
    base::DictValue& dict = value.GetDict();
    if (dict.size() == 1) {
      if (std::optional<int> index = dict.FindInt("$h")) {
        if (*index >= 0 && static_cast<size_t>(*index) < ids.size()) {
          base::DictValue handle;
          handle.Set("handle", ids[*index]);
          handle.Set("preview", handles[*index]->preview);
          if (handles[*index]->is_element)
            handle.Set("element", true);
          return base::Value(std::move(handle));
        }
      }
    }
    base::DictValue result;
    for (auto [key, item] : dict)
      result.Set(key, Expose(std::move(item), handles, ids));
    return base::Value(std::move(result));
  }

  base::Value Expose(content::RenderFrameHost* frame,
                     base::Value value,
                     const std::vector<AutomationHandlePtr>& handles) {
    if (!frame || handles.empty())
      return value;
    std::vector<std::string> ids;
    for (const AutomationHandlePtr& handle : handles)
      ids.push_back(Keep(frame, *handle));
    return Expose(std::move(value), handles, ids);
  }

  // The handles of a command's argument, {"handle": id}, as {"$h": i} for
  // the agent's @ids, all of @frame's document.
  bool Adopt(base::Value& value,
             content::GlobalRenderFrameHostId frame,
             std::vector<int32_t>* ids,
             std::string* error) {
    if (value.is_list()) {
      for (base::Value& item : value.GetList()) {
        if (!Adopt(item, frame, ids, error))
          return false;
      }
      return true;
    }
    if (!value.is_dict())
      return true;
    base::DictValue& dict = value.GetDict();
    if (const std::string* id = dict.FindString("handle")) {
      const HandleRef* handle = FindHandle(*id);
      if (!handle) {
        *error = kDisposed;
        return false;
      }
      if (handle->frame != frame) {
        *error =
            "JSHandles can be evaluated only in the context they were created!";
        return false;
      }
      base::DictValue reference;
      reference.Set("$h", static_cast<int>(ids->size()));
      ids->push_back(handle->id);
      value = base::Value(std::move(reference));
      return true;
    }
    for (auto [key, item] : dict) {
      if (!Adopt(item, frame, ids, error))
        return false;
    }
    return true;
  }

  void Dispose(const std::string& id) {
    auto found = handles_.find(id);
    if (found == handles_.end())
      return;
    if (content::RenderFrameHost* frame =
            content::RenderFrameHost::FromID(found->second.frame)) {
      Agent(frame)->Release({found->second.id});
    }
    handles_.erase(found);
  }

  // ---- Steps in the page -----------------------------------------------

  // Runs @step in @frame's agent, in the next document when a navigation
  // takes this one away (unless it acts on handles, which went with it),
  // until the call's timeout. @done gets the step's value; a step's error
  // or timeout ends the call.
  void RunStep(Call* call,
               FrameRef frame_ref,
               AutomationWorld world,
               const std::string& step,
               base::DictValue arg,
               std::vector<int32_t> handles,
               StepDone done) {
    if (call->finished())
      return;
    if (call->Expired()) {
      call->TimedOut();
      return;
    }
    content::RenderFrameHost* frame = Resolve(frame_ref);
    if (!frame) {
      call->Fail(handles.empty() ? kFrameDetached : kDetached);
      return;
    }
    arg.Set("timeout", call->Remaining());
    const std::string json = Json(base::Value(arg.Clone()));
    const content::GlobalRenderFrameHostId frame_id = frame->GetGlobalId();
    Agent(frame)->Step(
        world, step, json, handles,
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(
            base::BindOnce(&Automation::StepAnswered,
                           weak_factory_.GetWeakPtr(), call->GetWeakPtr(),
                           frame_ref, frame_id, world, step, std::move(arg),
                           handles, std::move(done)),
            AutomationStatus::kGone, std::string(),
            std::vector<AutomationHandlePtr>()));
  }

  void StepAnswered(base::WeakPtr<Call> call,
                    FrameRef frame_ref,
                    content::GlobalRenderFrameHostId frame_id,
                    AutomationWorld world,
                    std::string step,
                    base::DictValue arg,
                    std::vector<int32_t> handles,
                    StepDone done,
                    AutomationStatus status,
                    const std::string& result,
                    std::vector<AutomationHandlePtr> kept) {
    if (!call || call->finished())
      return;
    if (status == AutomationStatus::kGone) {
      if (!handles.empty() || !frame_ref.follow) {
        call->Fail(handles.empty() ? kNavigated : kDetached);
        return;
      }
      // A new document is coming: try there.
      base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE,
          base::BindOnce(&Automation::RunStepAgain, weak_factory_.GetWeakPtr(),
                         call, frame_ref, world, std::move(step),
                         std::move(arg), std::move(handles), std::move(done)),
          base::Milliseconds(20));
      return;
    }
    std::optional<base::Value> answer = Parse(result);
    if (status == AutomationStatus::kError || !answer || !answer->is_dict()) {
      call->Fail(result.empty() ? "zebkit: no answer" : result);
      return;
    }
    if (!answer->GetDict().FindBool("ok").value_or(false)) {
      const std::string* error = answer->GetDict().FindString("error");
      call->Fail(error ? *error : "Error");
      return;
    }
    base::Value value =
        answer->GetDict().Extract("value").value_or(base::Value());
    if (value.is_dict()) {
      call->Log(value.GetDict().FindList("log"));
      if (value.GetDict().FindBool("timedOut").value_or(false) &&
          step != "expect") {
        call->TimedOut();
        return;
      }
    }
    value = Expose(content::RenderFrameHost::FromID(frame_id), std::move(value),
                   kept);
    std::move(done).Run(std::move(value));
  }

  void RunStepAgain(base::WeakPtr<Call> call,
                    FrameRef frame_ref,
                    AutomationWorld world,
                    std::string step,
                    base::DictValue arg,
                    std::vector<int32_t> handles,
                    StepDone done) {
    if (call)
      RunStep(call.get(), frame_ref, world, step, std::move(arg),
              std::move(handles), std::move(done));
  }

  // The answer of a polling step, whose value is {result, log}.
  static base::Value ResultOf(base::Value& value) {
    if (value.is_dict()) {
      if (base::Value* result = value.GetDict().Find("result"))
        return std::move(*result);
    }
    return base::Value();
  }

  // A step on a target: {selector, strict, scope} and the scope's handle.
  void TargetStep(Call* call,
                  const Target& target,
                  const std::string& step,
                  base::DictValue arg,
                  StepDone done,
                  AutomationWorld world = AutomationWorld::kUtility) {
    arg.Set("selector", target.selector);
    arg.Set("strict", target.strict);
    std::vector<int32_t> handles;
    if (target.scope) {
      base::DictValue scope;
      scope.Set("$h", 0);
      arg.Set("scope", std::move(scope));
      handles.push_back(target.scope);
    }
    RunStep(call, target.frame, world, step, std::move(arg), std::move(handles),
            std::move(done));
  }

  // What a selector command acts on (frameSelectors.ts): the frame, with
  // the frames its selector enters, an ElementHandle's own element or the
  // elements inside it.
  void ResolveTarget(Call* call, TargetDone done) {
    Target target;
    const std::string selector = call->String("selector");
    if (call->OnHandle()) {
      const HandleRef* handle = FindHandle(call->String("handle"));
      content::RenderFrameHost* frame =
          handle ? content::RenderFrameHost::FromID(handle->frame) : nullptr;
      if (!frame) {
        call->Fail(kDisposed);
        return;
      }
      target.frame = Exact(frame);
      target.scope = handle->id;
      const std::string name = call->Name();
      const bool inside = name == "querySelector" ||
                          name == "querySelectorAll" ||
                          name == "evalOnSelector" ||
                          name == "evalOnSelectorAll" ||
                          name == "waitForSelector";
      target.selector = inside ? selector : ":scope";
      target.strict = inside && call->Flag("strict");
      std::move(done).Run(std::move(target));
      return;
    }
    content::RenderFrameHost* frame = FrameOf(call);
    if (!frame) {
      call->Fail(kFrameDetached);
      return;
    }
    target.frame = Follow(frame);
    target.strict = call->Flag("strict");
    std::vector<std::string> chunks;
    size_t start = 0;
    for (size_t at;
         (at = selector.find(kEnterFrame, start)) != std::string::npos;) {
      chunks.push_back(selector.substr(start, at - start));
      start = at + sizeof(kEnterFrame) - 1;
    }
    chunks.push_back(selector.substr(start));
    target.selector = chunks.back();
    chunks.pop_back();
    EnterFrames(call->GetWeakPtr(), std::move(target), std::move(chunks), 0,
                std::move(done));
  }

  // Each frame's <iframe> found in its parent and its frame entered, until
  // the selector's last part.
  void EnterFrames(base::WeakPtr<Call> call,
                   Target target,
                   std::vector<std::string> chunks,
                   size_t index,
                   TargetDone done) {
    if (!call)
      return;
    if (index >= chunks.size()) {
      std::move(done).Run(std::move(target));
      return;
    }
    Target frame_target = target;
    frame_target.selector = chunks[index];
    call->Log("waiting for " + chunks[index]);
    base::DictValue arg;
    arg.Set("state", "attached");
    TargetStep(call.get(), frame_target, "waitForSelector", std::move(arg),
               base::BindOnce(&Automation::FrameElementFound,
                              weak_factory_.GetWeakPtr(), call,
                              std::move(target), std::move(chunks), index,
                              std::move(done)));
  }

  void FrameElementFound(base::WeakPtr<Call> call,
                         Target target,
                         std::vector<std::string> chunks,
                         size_t index,
                         TargetDone done,
                         base::Value value) {
    if (!call)
      return;
    base::Value result = ResultOf(value);
    const std::string* id =
        result.is_dict()
            ? result.GetDict().FindStringByDottedPath("element.handle")
            : nullptr;
    const HandleRef* handle = id ? FindHandle(*id) : nullptr;
    content::RenderFrameHost* parent =
        handle ? content::RenderFrameHost::FromID(handle->frame) : nullptr;
    if (!parent) {
      call->Fail(kFrameDetached);
      return;
    }
    const std::string kept = *id;
    Agent(parent)->ContentFrame(
        handle->id,
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(
            base::BindOnce(&Automation::FrameEntered,
                           weak_factory_.GetWeakPtr(), call, handle->frame,
                           kept, std::move(target), std::move(chunks), index,
                           std::move(done)),
            -1));
  }

  void FrameEntered(base::WeakPtr<Call> call,
                    content::GlobalRenderFrameHostId parent_id,
                    std::string kept,
                    Target target,
                    std::vector<std::string> chunks,
                    size_t index,
                    TargetDone done,
                    int32_t child) {
    Dispose(kept);
    if (!call)
      return;
    content::RenderFrameHost* parent =
        content::RenderFrameHost::FromID(parent_id);
    std::vector<content::RenderFrameHost*> children =
        parent ? Children(parent) : std::vector<content::RenderFrameHost*>();
    if (child < 0 || static_cast<size_t>(child) >= children.size()) {
      call->Fail("Selector \"" + chunks[index] +
                 "\" resolved to an element that is not an <iframe>");
      return;
    }
    target.frame = Follow(children[child]);
    target.scope = 0;
    EnterFrames(call, std::move(target), std::move(chunks), index + 1,
                std::move(done));
  }

  // A command whose target is resolved first.
  using TargetCommand = void (Automation::*)(base::WeakPtr<Call>, Target);
  void OnTarget(Call* call, TargetCommand command) {
    ResolveTarget(
        call, base::BindOnce(
                  [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call,
                     TargetCommand command, Target target) {
                    if (self && call)
                      ((*self).*command)(call, std::move(target));
                  },
                  weak_factory_.GetWeakPtr(), call->GetWeakPtr(), command));
  }

  // A step on the target whose answer is the step's value, transformed by
  // @answer.
  using Answerer = base::OnceCallback<void(Call*, base::Value)>;
  void StepOnTarget(Call* call,
                    std::string step,
                    base::DictValue arg,
                    Answerer answer,
                    AutomationWorld world = AutomationWorld::kUtility) {
    ResolveTarget(
        call,
        base::BindOnce(
            [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call,
               std::string step, base::DictValue arg, Answerer answer,
               AutomationWorld world, Target target) {
              if (!self || !call)
                return;
              self->TargetStep(
                  call.get(), target, step, std::move(arg),
                  base::BindOnce(
                      [](base::WeakPtr<Call> call, Answerer answer,
                         base::Value value) {
                        if (call)
                          std::move(answer).Run(call.get(), std::move(value));
                      },
                      call, std::move(answer)),
                  world);
            },
            weak_factory_.GetWeakPtr(), call->GetWeakPtr(), std::move(step),
            std::move(arg), std::move(answer), world));
  }

  // Answers: the value; {value} of a polled result; {} when done; one key
  // of a polled result.
  static Answerer AsValue() {
    return base::BindOnce(
        [](Call* call, base::Value value) { call->Answer(std::move(value)); });
  }
  static Answerer AsPolledValue() {
    return base::BindOnce([](Call* call, base::Value value) {
      base::Value result = ResultOf(value);
      base::Value* found =
          result.is_dict() ? result.GetDict().Find("value") : nullptr;
      call->Answer(found ? std::move(*found) : base::Value());
    });
  }
  static Answerer AsDone() {
    return base::BindOnce([](Call* call, base::Value) { call->Done(); });
  }
  static Answerer AsPolledKey(std::string key, std::string as) {
    return base::BindOnce(
        [](std::string key, std::string as, Call* call, base::Value value) {
          base::Value result = ResultOf(value);
          base::DictValue answer;
          if (result.is_dict()) {
            if (base::Value* found = result.GetDict().Find(key);
                found && !found->is_none()) {
              answer.Set(as, std::move(*found));
            }
          }
          call->Done(base::Value(std::move(answer)));
        },
        std::move(key), std::move(as));
  }
  static Answerer AsKey(std::string key, std::string as) {
    return base::BindOnce(
        [](std::string key, std::string as, Call* call, base::Value value) {
          base::DictValue answer;
          if (key.empty()) {
            if (!value.is_none())
              answer.Set(as, std::move(value));
          } else if (value.is_dict()) {
            if (base::Value* found = value.GetDict().Find(key);
                found && !found->is_none()) {
              answer.Set(as, std::move(*found));
            }
          }
          call->Done(base::Value(std::move(answer)));
        },
        std::move(key), std::move(as));
  }

  // A step of the frame itself.
  void FrameStep(Call* call,
                 const std::string& step,
                 base::DictValue arg,
                 Answerer answer) {
    content::RenderFrameHost* frame = FrameOf(call);
    if (!frame) {
      call->Fail(kFrameDetached);
      return;
    }
    RunStep(call, Follow(frame), AutomationWorld::kUtility, step,
            std::move(arg), {},
            base::BindOnce(
                [](base::WeakPtr<Call> call, Answerer answer,
                   base::Value value) {
                  if (call)
                    std::move(answer).Run(call.get(), std::move(value));
                },
                call->GetWeakPtr(), std::move(answer)));
  }

  // ---- Evaluation ------------------------------------------------------

  static AutomationExpression KindOf(Call* call) {
    const base::Value* is_function = call->params().Find("isFunction");
    if (!is_function || !is_function->is_bool())
      return AutomationExpression::kAuto;
    return is_function->GetBool() ? AutomationExpression::kFunction
                                  : AutomationExpression::kValue;
  }

  // The answer of an agent call that is not a step: {ok, value} with its
  // handles.
  void Answered(base::WeakPtr<Call> call,
                content::GlobalRenderFrameHostId frame_id,
                StepDone done,
                AutomationStatus status,
                const std::string& result,
                std::vector<AutomationHandlePtr> kept) {
    if (!call)
      return;
    if (status == AutomationStatus::kGone) {
      call->Fail(kNavigated);
      return;
    }
    std::optional<base::Value> answer = Parse(result);
    if (status == AutomationStatus::kError || !answer || !answer->is_dict()) {
      call->Fail(result.empty() ? "zebkit: no answer" : result);
      return;
    }
    if (!answer->GetDict().FindBool("ok").value_or(false)) {
      const std::string* error = answer->GetDict().FindString("error");
      call->Fail(error ? *error : "Error");
      return;
    }
    base::Value value =
        answer->GetDict().Extract("value").value_or(base::Value());
    value = Expose(content::RenderFrameHost::FromID(frame_id),
                   std::move(value), kept);
    std::move(done).Run(std::move(value));
  }

  auto AnswerTo(Call* call,
                content::GlobalRenderFrameHostId frame,
                StepDone done) {
    return mojo::WrapCallbackWithDefaultInvokeIfNotRun(
        base::BindOnce(&Automation::Answered, weak_factory_.GetWeakPtr(),
                       call->GetWeakPtr(), frame, std::move(done)),
        AutomationStatus::kGone, std::string(),
        std::vector<AutomationHandlePtr>());
  }

  static StepDone Then(Call* call, Answerer answer) {
    return base::BindOnce(
        [](base::WeakPtr<Call> call, Answerer answer, base::Value value) {
          if (call)
            std::move(answer).Run(call.get(), std::move(value));
        },
        call->GetWeakPtr(), std::move(answer));
  }

  // The arguments of an evaluation, with the handle they are called on
  // first (an ElementHandle's element, a JSHandle's object): a JSON array,
  // and the agent's handles.
  bool EvaluationArgs(Call* call,
                      content::GlobalRenderFrameHostId frame,
                      std::optional<int32_t> first,
                      std::string* json,
                      std::vector<int32_t>* handles) {
    base::ListValue args;
    if (call->Flag("serialized")) {
      // Playwright's own: "arg" a serialized value whose {h: i} are
      // "argHandles"[i]; the handle the call is on after them.
      if (const base::ListValue* given = call->params().FindList("argHandles")) {
        for (const base::Value& id : *given) {
          const HandleRef* handle = id.is_string() ? FindHandle(id.GetString()) : nullptr;
          if (!handle || handle->frame != frame) {
            call->Fail(handle ? "JSHandles can be evaluated only in the context they were created!"
                              : kDisposed);
            return false;
          }
          handles->push_back(handle->id);
        }
      }
      if (first) {
        base::DictValue reference;
        reference.Set("h", static_cast<int>(handles->size()));
        args.Append(std::move(reference));
        handles->push_back(*first);
      }
      const base::Value* arg = call->params().Find("arg");
      base::DictValue undefined;
      undefined.Set("v", "undefined");
      args.Append(arg ? arg->Clone() : base::Value(std::move(undefined)));
      *json = Json(base::Value(std::move(args)));
      return true;
    }
    if (first) {
      base::DictValue reference;
      reference.Set("$h", 0);
      args.Append(std::move(reference));
      handles->push_back(*first);
    }
    if (const base::Value* arg = call->params().Find("arg")) {
      base::Value copy = arg->Clone();
      std::string error;
      if (!Adopt(copy, frame, handles, &error)) {
        call->Fail(error);
        return false;
      }
      args.Append(std::move(copy));
    }
    *json = Json(base::Value(std::move(args)));
    return true;
  }

  // Frame.evaluateExpression(Handle), JSHandle's and ElementHandle's: in
  // the page's world, with a user gesture.
  void Evaluate(Call* call) {
    content::RenderFrameHost* frame = nullptr;
    std::optional<int32_t> first;
    if (call->OnHandle()) {
      const HandleRef* handle = FindHandle(call->String("handle"));
      frame = handle ? content::RenderFrameHost::FromID(handle->frame) : nullptr;
      if (!frame) {
        call->Fail(kDisposed);
        return;
      }
      first = handle->id;
    } else {
      frame = FrameOf(call);
      if (!frame) {
        call->Fail(kFrameDetached);
        return;
      }
    }
    std::string args;
    std::vector<int32_t> handles;
    if (!EvaluationArgs(call, frame->GetGlobalId(), first, &args, &handles))
      return;
    const bool by_value = call->Name() == "evaluateExpression";
    Agent(frame)->Evaluate(
        AutomationWorld::kMain, call->String("expression"), KindOf(call), args,
        handles, by_value, !call->Flag("serialized"),
        AnswerTo(call, frame->GetGlobalId(),
                 Then(call, by_value ? AsValue() : AsKey("", "handle"))));
  }

  // Frame.evalOnSelector, evalOnSelectorAll, ElementHandle's: the element
  // (or the array of them) selected in the page's world, then the function
  // evaluated on it.
  void EvalOnSelector(Call* call) { OnTarget(call, &Automation::EvalOn); }

  void EvalOn(base::WeakPtr<Call> call, Target target) {
    const bool all = call->Name() == "evalOnSelectorAll";
    content::RenderFrameHost* frame = Resolve(target.frame);
    if (!frame) {
      call->Fail(kFrameDetached);
      return;
    }
    Agent(frame)->Select(
        AutomationWorld::kMain, target.selector, target.strict && !all,
        target.scope, all,
        AnswerTo(call.get(), frame->GetGlobalId(),
                 base::BindOnce(&Automation::EvalOnSelected,
                                weak_factory_.GetWeakPtr(), call,
                                frame->GetGlobalId())));
  }

  void EvalOnSelected(base::WeakPtr<Call> call,
                      content::GlobalRenderFrameHostId frame,
                      base::Value selected) {
    if (!call)
      return;
    const std::string* id =
        selected.is_dict() ? selected.GetDict().FindString("handle") : nullptr;
    const HandleRef* handle = id ? FindHandle(*id) : nullptr;
    content::RenderFrameHost* rfh = content::RenderFrameHost::FromID(frame);
    if (!handle || !rfh) {
      call->Fail("Failed to find element matching selector \"" +
                 call->String("selector") + "\"");
      return;
    }
    const std::string kept = *id;
    std::string args;
    std::vector<int32_t> handles;
    if (!EvaluationArgs(call.get(), frame, handle->id, &args, &handles)) {
      Dispose(kept);
      return;
    }
    Agent(rfh)->Evaluate(
        AutomationWorld::kMain, call->String("expression"), KindOf(call.get()),
        args, handles, true, !call->Flag("serialized"),
        AnswerTo(call.get(), frame,
                 base::BindOnce(
                     [](base::WeakPtr<Automation> self,
                        base::WeakPtr<Call> call, std::string kept,
                        base::Value value) {
                       if (self)
                         self->Dispose(kept);
                       if (call)
                         call->Answer(std::move(value));
                     },
                     weak_factory_.GetWeakPtr(), call, kept)));
  }

  // Frame.waitForFunction: the predicate in the page's world, polled there;
  // its value as a handle.
  void WaitForFunction(Call* call) {
    content::RenderFrameHost* frame = FrameOf(call);
    if (!frame) {
      call->Fail(kFrameDetached);
      return;
    }
    std::string args;
    std::vector<int32_t> handles;
    if (!EvaluationArgs(call, frame->GetGlobalId(), std::nullopt, &args,
                        &handles)) {
      return;
    }
    Agent(frame)->WaitForFunction(
        AutomationWorld::kMain, call->String("expression"), KindOf(call), args,
        handles, static_cast<int>(call->Number("pollingInterval")),
        call->Remaining(), !call->Flag("serialized"),
        AnswerTo(call, frame->GetGlobalId(),
                 Then(call, base::BindOnce([](Call* call, base::Value value) {
                        if (value.is_dict() &&
                            value.GetDict().FindBool("timedOut").value_or(false)) {
                          call->TimedOut();
                          return;
                        }
                        base::DictValue answer;
                        answer.Set("handle", std::move(value));
                        call->Done(base::Value(std::move(answer)));
                      }))));
  }

  // ---- JSHandle --------------------------------------------------------

  void GetProperty(Call* call) { Properties(call, call->String("name")); }
  void GetPropertyList(Call* call) { Properties(call, std::string()); }

  void Properties(Call* call, const std::string& name) {
    const HandleRef* handle = FindHandle(call->String("handle"));
    content::RenderFrameHost* frame =
        handle ? content::RenderFrameHost::FromID(handle->frame) : nullptr;
    if (!frame) {
      call->Fail(kDisposed);
      return;
    }
    Agent(frame)->Properties(
        handle->id, name,
        AnswerTo(call, handle->frame,
                 Then(call, base::BindOnce(
                                [](std::string name, Call* call,
                                   base::Value value) {
                                  base::DictValue answer;
                                  if (!name.empty()) {
                                    base::Value* handle =
                                        value.is_dict()
                                            ? value.GetDict().Find(name)
                                            : nullptr;
                                    answer.Set("handle",
                                               handle ? std::move(*handle)
                                                      : base::Value());
                                  } else {
                                    base::ListValue properties;
                                    if (value.is_dict()) {
                                      for (auto [key, item] : value.GetDict()) {
                                        base::DictValue property;
                                        property.Set("name", key);
                                        property.Set("value", std::move(item));
                                        properties.Append(std::move(property));
                                      }
                                    }
                                    answer.Set("properties",
                                               std::move(properties));
                                  }
                                  call->Done(base::Value(std::move(answer)));
                                },
                                name))));
  }

  void JsonValue(Call* call) {
    const HandleRef* handle = FindHandle(call->String("handle"));
    content::RenderFrameHost* frame =
        handle ? content::RenderFrameHost::FromID(handle->frame) : nullptr;
    if (!frame) {
      call->Fail(kDisposed);
      return;
    }
    Agent(frame)->JsonValue(handle->id, !call->Flag("serialized"),
                            AnswerTo(call, handle->frame, Then(call, AsValue())));
  }

  void DisposeCommand(Call* call) {
    Dispose(call->String("handle"));
    call->Done();
  }

  // ---- Reads -----------------------------------------------------------

  // A read that waits for its element (frames.ts's
  // _waitForFunctionOnSelector()).
  void Read(Call* call, const std::string& what, base::DictValue extra = {}) {
    extra.Set("what", what);
    if (!call->OnHandle())
      call->Log("waiting for " + call->String("selector"));
    StepOnTarget(call, "read", std::move(extra), AsPolledValue());
  }

  void TextContent(Call* call) { Read(call, "textContent"); }
  void InnerText(Call* call) { Read(call, "innerText"); }
  void InnerHtml(Call* call) { Read(call, "innerHTML"); }
  void InputValue(Call* call) { Read(call, "inputValue"); }
  void GetAttribute(Call* call) {
    base::DictValue extra;
    extra.Set("name", call->String("name"));
    Read(call, "getAttribute", std::move(extra));
  }
  void State(Call* call, const char* state) {
    base::DictValue extra;
    extra.Set("state", state);
    Read(call, "elementState", std::move(extra));
  }
  void IsChecked(Call* call) { State(call, "checked"); }
  void IsDisabled(Call* call) { State(call, "disabled"); }
  void IsEnabled(Call* call) { State(call, "enabled"); }
  void IsEditable(Call* call) { State(call, "editable"); }

  void QueryCount(Call* call) { StepOnTarget(call, "queryCount", {}, AsValue()); }
  void IsVisible(Call* call) { StepOnTarget(call, "isVisible", {}, AsValue()); }
  void IsHidden(Call* call) {
    StepOnTarget(call, "isVisible", {},
                 base::BindOnce([](Call* call, base::Value value) {
                   call->Answer(
                       base::Value(!(value.is_bool() && value.GetBool())));
                 }));
  }

  // Frame.querySelector, querySelectorAll, ElementHandle's.
  void QuerySelector(Call* call) {
    const bool all = call->Name() == "querySelectorAll";
    StepOnTarget(call, all ? "queryAll" : "query", {},
                 AsKey("", all ? "elements" : "element"));
  }

  // Frame.waitForSelector, ElementHandle's.
  void WaitForSelector(Call* call) {
    std::string state = call->String("state");
    if (!state.empty() && state != "attached" && state != "detached" &&
        state != "visible" && state != "hidden") {
      call->Fail("state: expected one of (attached|detached|visible|hidden)");
      return;
    }
    if (state.empty())
      state = "visible";
    call->Log("waiting for " + call->String("selector") +
              (state == "attached" ? "" : " to be " + state));
    base::DictValue arg;
    arg.Set("state", state);
    arg.Set("omitReturnValue", call->Flag("omitReturnValue"));
    StepOnTarget(call, "waitForSelector", std::move(arg),
                 AsPolledKey("element", "element"));
  }

  void WaitForElementState(Call* call) {
    base::DictValue arg;
    arg.Set("state", call->String("state"));
    StepOnTarget(call, "waitForElementState", std::move(arg), AsDone());
  }

  void BoundingBox(Call* call) { OnTarget(call, &Automation::BoundingBoxOf); }

  // In the main frame's viewport, as Playwright's boxes are.
  void BoundingBoxOf(base::WeakPtr<Call> call, Target target) {
    TargetStep(
        call.get(), target, "boundingBox", {},
        base::BindOnce(
            [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call,
               FrameRef frame, base::Value box) {
              if (!self || !call)
                return;
              if (!box.is_dict()) {
                call->Answer(base::Value());
                return;
              }
              const gfx::PointF origin(box.GetDict().FindDouble("x").value_or(0),
                                       box.GetDict().FindDouble("y").value_or(0));
              self->ToRoot(
                  frame, origin,
                  base::BindOnce(
                      [](base::WeakPtr<Call> call, base::Value box,
                         content::RenderWidgetHost*, gfx::PointF,
                         gfx::PointF root) {
                        if (!call)
                          return;
                        base::DictValue answer;
                        answer.Set("x", root.x());
                        answer.Set("y", root.y());
                        answer.Set("width",
                                   box.GetDict().FindDouble("width").value_or(0));
                        answer.Set("height",
                                   box.GetDict().FindDouble("height").value_or(0));
                        call->Answer(base::Value(std::move(answer)));
                      },
                      call, std::move(box)));
            },
            weak_factory_.GetWeakPtr(), call, target.frame));
  }

  void Title(Call* call) { FrameStep(call, "title", {}, AsValue()); }
  void Content(Call* call) { FrameStep(call, "content", {}, AsValue()); }

  void SetContent(Call* call) {
    base::DictValue arg;
    arg.Set("html", call->String("html"));
    const std::string wait_until = call->String("waitUntil");
    arg.Set("waitUntil", wait_until.empty() ? "load" : wait_until);
    FrameStep(call, "setContent", std::move(arg), AsDone());
  }

  void AddScriptTag(Call* call) { AddTag(call, "script"); }
  void AddStyleTag(Call* call) { AddTag(call, "style"); }
  void AddTag(Call* call, const char* tag) {
    base::DictValue arg;
    arg.Set("tag", tag);
    for (const char* key : {"url", "content", "type"}) {
      if (call->Has(key))
        arg.Set(key, call->String(key));
    }
    FrameStep(call, "addTag", std::move(arg), AsKey("element", "element"));
  }

  void AriaSnapshot(Call* call) {
    base::DictValue arg;
    arg.Set("mode",
            call->String("mode").empty() ? "default" : call->String("mode"));
    if (call->Has("depth"))
      arg.Set("depth", static_cast<int>(call->Number("depth")));
    arg.Set("json", call->Name() == "ariaSnapshotJSON");
    if (call->String("selector").empty() && !call->OnHandle()) {
      FrameStep(call, "ariaSnapshot", std::move(arg),
                AsKey("snapshot", "snapshot"));
      return;
    }
    StepOnTarget(call, "ariaSnapshot", std::move(arg),
                 AsPolledKey("snapshot", "snapshot"));
  }

  // Frame.expect: Playwright's assertions (expect(locator).toHaveText() and
  // the rest), retried in the page until they hold or the time is out; the
  // answer is FrameExpectResult's: matches, received, timedOut,
  // errorMessage.
  void Expect(Call* call) {
    base::DictValue options = call->params().Clone();
    for (const char* key : {"selector", "timeout", "frame", "handle"})
      options.Remove(key);
    base::DictValue arg;
    arg.Set("options", std::move(options));
    if (!call->Has("selector") && !call->OnHandle()) {
      // A page's assertion (toHaveTitle, toHaveURL): of its root.
      FrameStep(call, "expect", std::move(arg),
                base::BindOnce([](Call* call, base::Value value) {
                  call->Done(std::move(value));
                }));
      return;
    }
    call->Log("waiting for " + call->String("selector"));
    StepOnTarget(call, "expect", std::move(arg),
                 base::BindOnce([](Call* call, base::Value value) {
                   call->Done(std::move(value));
                 }));
  }

  void WaitForTimeout(Call* call) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, Done(call),
        base::Milliseconds(call->Number("waitTimeout")));
  }

  // Page.frames (zebkit's: Playwright's client keeps them from events).
  void Frames(Call* call) {
    base::ListValue frames;
    MainFrame()->ForEachRenderFrameHost([&](content::RenderFrameHost* frame) {
      if (frame->IsActive())
        frames.Append(FrameValue(frame));
    });
    base::DictValue answer;
    answer.Set("frames", std::move(frames));
    call->Done(base::Value(std::move(answer)));
  }

  // ElementHandle.contentFrame and ownerFrame.
  void ContentFrame(Call* call) {
    const HandleRef* handle = FindHandle(call->String("handle"));
    content::RenderFrameHost* frame =
        handle ? content::RenderFrameHost::FromID(handle->frame) : nullptr;
    if (!frame) {
      call->Fail(kDisposed);
      return;
    }
    Agent(frame)->ContentFrame(
        handle->id,
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(
            base::BindOnce(
                [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call,
                   content::GlobalRenderFrameHostId parent_id, int32_t index) {
                  if (!self || !call)
                    return;
                  content::RenderFrameHost* parent =
                      content::RenderFrameHost::FromID(parent_id);
                  std::vector<content::RenderFrameHost*> children =
                      parent ? self->Children(parent)
                             : std::vector<content::RenderFrameHost*>();
                  base::DictValue answer;
                  if (index >= 0 &&
                      static_cast<size_t>(index) < children.size()) {
                    answer.Set("frame", FrameId(children[index]));
                  }
                  call->Done(base::Value(std::move(answer)));
                },
                weak_factory_.GetWeakPtr(), call->GetWeakPtr(), handle->frame),
            -1));
  }

  void OwnerFrame(Call* call) {
    const HandleRef* handle = FindHandle(call->String("handle"));
    content::RenderFrameHost* frame =
        handle ? content::RenderFrameHost::FromID(handle->frame) : nullptr;
    if (!frame) {
      call->Fail(kDisposed);
      return;
    }
    base::DictValue answer;
    answer.Set("frame", FrameId(frame));
    call->Done(base::Value(std::move(answer)));
  }

  // ---- Points across frames --------------------------------------------

  using PointDone = base::OnceCallback<void(content::RenderWidgetHost* widget,
                                            gfx::PointF in_widget,
                                            gfx::PointF in_root)>;

  // @point of @frame's viewport in its widget (the main frame's, or a frame
  // of another site's own) and in the main frame's viewport: the frames
  // above added, each where its <iframe>'s content box is in its parent.
  void ToRoot(FrameRef frame_ref, gfx::PointF point, PointDone done) {
    content::RenderFrameHost* frame = Resolve(frame_ref);
    if (!frame) {
      std::move(done).Run(nullptr, point, point);
      return;
    }
    content::RenderFrameHost* parent = frame->GetParent();
    content::RenderWidgetHost* widget = frame->GetRenderWidgetHost();
    if (!parent || parent->GetRenderWidgetHost() != widget) {
      // A local root: its widget's own coordinates.
      gfx::PointF root = point;
      if (content::RenderWidgetHostView* view = widget->GetView())
        root = view->TransformPointToRootCoordSpaceF(point);
      std::move(done).Run(widget, point, root);
      return;
    }
    std::vector<content::RenderFrameHost*> children = Children(parent);
    const int index = static_cast<int>(
        std::find(children.begin(), children.end(), frame) - children.begin());
    base::DictValue arg;
    arg.Set("index", index);
    Agent(parent)->Step(
        AutomationWorld::kUtility, "childFrameBox",
        Json(base::Value(std::move(arg))), {},
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(
            base::BindOnce(
                [](base::WeakPtr<Automation> self, FrameRef parent,
                   gfx::PointF point, PointDone done, AutomationStatus,
                   const std::string& result,
                   std::vector<AutomationHandlePtr>) {
                  if (!self)
                    return;
                  std::optional<base::Value> answer = Parse(result);
                  const base::DictValue* box =
                      answer && answer->is_dict()
                          ? answer->GetDict().FindDict("value")
                          : nullptr;
                  if (box) {
                    point.Offset(box->FindDouble("x").value_or(0),
                                 box->FindDouble("y").value_or(0));
                  }
                  self->ToRoot(parent, point, std::move(done));
                },
                weak_factory_.GetWeakPtr(), Exact(parent), point,
                std::move(done)),
            AutomationStatus::kGone, std::string(),
            std::vector<AutomationHandlePtr>()));
  }

  // ---- Actions ---------------------------------------------------------

  struct PointerAction {
    // The hit-target check's kind: mouse, hover, tap or drag.
    std::string action = "mouse";
    // What the call log says it tries.
    std::string name = "click";
    bool click = true;
    bool tap = false;
    int click_count = 1;
    bool wait_for_enabled = true;
    std::optional<bool> checked;
    // dragAndDrop's halves: down at the source, then moved and up at the
    // target.
    bool down_only = false;
    bool up_only = false;
  };

  void Click(Call* call) { Pointer(call, {}); }
  void Dblclick(Call* call) {
    PointerAction action;
    action.click_count = 2;
    action.name = "dblclick";
    Pointer(call, action);
  }
  void Hover(Call* call) {
    PointerAction action;
    action.action = "hover";
    action.name = "hover";
    action.click = false;
    action.wait_for_enabled = false;
    Pointer(call, action);
  }
  void Tap(Call* call) {
    PointerAction action;
    action.action = "tap";
    action.name = "tap";
    action.click = false;
    action.tap = true;
    Pointer(call, action);
  }
  void Check(Call* call) {
    PointerAction action;
    action.checked = true;
    action.name = "check";
    Pointer(call, action);
  }
  void Uncheck(Call* call) {
    PointerAction action;
    action.checked = false;
    action.name = "uncheck";
    Pointer(call, action);
  }
  void SetChecked(Call* call) {
    if (call->Flag("checked"))
      Check(call);
    else
      Uncheck(call);
  }

  void Pointer(Call* call, PointerAction action) {
    ResolveTarget(call, base::BindOnce(&Automation::PointerOn,
                                       weak_factory_.GetWeakPtr(),
                                       call->GetWeakPtr(), action));
  }

  // dom.ts's _retryPointerAction(): one renderer call checks the element
  // and sets up the hit-target check, the input goes from here, a second
  // call says whether it reached the element; again until it does.
  void PointerOn(base::WeakPtr<Call> call, PointerAction action, Target target) {
    if (!call)
      return;
    base::DictValue arg;
    arg.Set("action", action.action);
    arg.Set("waitForEnabled", action.wait_for_enabled);
    arg.Set("force", call->Flag("force"));
    arg.Set("trial", call->Flag("trial"));
    arg.Set("attempt", call->attempt);
    const char* position_key = action.up_only     ? "targetPosition"
                               : action.down_only ? "sourcePosition"
                                                  : "position";
    if (const base::DictValue* position = call->params().FindDict(position_key))
      arg.Set("position", position->Clone());
    if (call->String("scroll") == "none")
      arg.Set("scroll", "none");
    if (action.checked)
      arg.Set("checked", *action.checked);
    if (!call->Flag("force") && !call->OnHandle())
      call->Log("waiting for " + target.selector);
    TargetStep(call.get(), target, "pointerPrepare", std::move(arg),
               base::BindOnce(&Automation::PointerPrepared,
                              weak_factory_.GetWeakPtr(), call, action, target));
  }

  void PointerPrepared(base::WeakPtr<Call> call,
                       PointerAction action,
                       Target target,
                       base::Value value) {
    if (!call)
      return;
    base::Value prepared = ResultOf(value);
    if (!prepared.is_dict()) {
      call->Fail("zebkit: no point to act on");
      return;
    }
    if (prepared.GetDict().FindBool("done").value_or(false)) {
      call->Done();
      return;
    }
    const base::DictValue* point = prepared.GetDict().FindDict("point");
    const gfx::PointF where(point ? point->FindDouble("x").value_or(0) : 0,
                            point ? point->FindDouble("y").value_or(0) : 0);
    const int interceptor =
        prepared.GetDict().FindInt("interceptor").value_or(0);
    ToRoot(target.frame, where,
           base::BindOnce(&Automation::PointerAt, weak_factory_.GetWeakPtr(),
                          call, action, target, interceptor));
  }

  // The input goes to the widget under the point, as a person's would: a
  // frame of another site's is its own (patches/1038).
  void PointerAt(base::WeakPtr<Call> call,
                 PointerAction action,
                 Target target,
                 int interceptor,
                 content::RenderWidgetHost* widget,
                 gfx::PointF in_widget,
                 gfx::PointF in_root) {
    if (!call)
      return;
    content::FindWidgetAtPoint(
        web_contents(), in_root,
        base::BindOnce(
            [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call,
               PointerAction action, Target target, int interceptor,
               content::RenderWidgetHost* fallback, gfx::PointF fallback_point,
               gfx::PointF in_root, content::RenderWidgetHost* widget,
               gfx::PointF point) {
              if (!self)
                return;
              if (!widget) {
                widget = fallback;
                point = fallback_point;
              }
              self->PointerIn(call, action, target, interceptor, widget, point,
                              in_root);
            },
            weak_factory_.GetWeakPtr(), call, action, target, interceptor,
            widget, in_widget, in_root));
  }

  void PointerIn(base::WeakPtr<Call> call,
                 PointerAction action,
                 Target target,
                 int interceptor,
                 content::RenderWidgetHost* widget,
                 gfx::PointF in_widget,
                 gfx::PointF in_root) {
    if (!call)
      return;
    if (!widget) {
      call->Fail(kFrameDetached);
      return;
    }
    if (call->Flag("trial")) {
      PointerSend(call, action, std::move(target), interceptor, widget,
                  in_widget, in_root);
      return;
    }
    TakeInputDropped();
    Observe(widget);
    WhenInputReady(base::BindOnce(
        [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call,
           PointerAction action, Target target, int interceptor, int process,
           int routing, gfx::PointF in_widget, gfx::PointF in_root) {
          if (!self || !call)
            return;
          content::RenderWidgetHost* widget =
              content::RenderWidgetHost::FromID(process, routing);
          if (!widget) {
            call->Fail(kFrameDetached);
            return;
          }
          self->PointerSend(call, action, std::move(target), interceptor,
                            widget, in_widget, in_root);
        },
        weak_factory_.GetWeakPtr(), call, action, std::move(target),
        interceptor, widget->GetProcess()->GetDeprecatedID(),
        widget->GetRoutingID(), in_widget, in_root));
  }

  void PointerSend(base::WeakPtr<Call> call,
                   PointerAction action,
                   Target target,
                   int interceptor,
                   content::RenderWidgetHost* widget,
                   gfx::PointF in_widget,
                   gfx::PointF in_root) {
    if (!call)
      return;
    auto finish = base::BindOnce(&Automation::PointerDone,
                                 weak_factory_.GetWeakPtr(), call, action,
                                 target, interceptor);
    if (call->Flag("trial")) {
      std::move(finish).Run();
      return;
    }
    // Navigations the input starts are waited for (waitForSignalsCreatedBy).
    watched_navigations_.erase(call.get());
    watching_.insert(call.get());
    std::set<std::string> restore = pressed_modifiers_;
    if (const base::ListValue* list = call->params().FindList("modifiers")) {
      std::set<std::string> modifiers;
      for (const base::Value& modifier : *list) {
        if (modifier.is_string())
          modifiers.insert(ResolveSmartModifier(modifier.GetString()));
      }
      EnsureModifiers(modifiers);
    }
    auto restored = base::BindOnce(
        [](base::WeakPtr<Automation> self, std::set<std::string> restore,
           base::OnceClosure finish) {
          if (self)
            self->EnsureModifiers(restore);
          std::move(finish).Run();
        },
        weak_factory_.GetWeakPtr(), std::move(restore), std::move(finish));
    if (action.tap) {
      SendTap(widget, in_widget, std::move(restored));
      return;
    }
    const Button button = ButtonOf(call->String("button"));
    const int click_count =
        static_cast<int>(call->Number("clickCount", action.click_count));
    const int delay = static_cast<int>(call->Number("delay"));
    const int steps = std::max(1, static_cast<int>(call->Number("steps", 1)));
    MouseMoveIn(widget, in_widget, in_root, action.up_only ? steps : 1);
    if (action.down_only) {
      MouseDownIn(widget, button, 1);
      WhenAcked(Type::kMouseDown, std::move(restored));
    } else if (action.up_only) {
      MouseUpIn(widget, button, 1);
      WhenAcked(Type::kMouseUp, std::move(restored));
    } else if (action.click) {
      PressAndRelease(call.get(), widget, button, click_count, 1, delay,
                      std::move(restored));
    } else {
      WhenAcked(Type::kMouseMove, std::move(restored));
    }
  }

  void PointerDone(base::WeakPtr<Call> call,
                   PointerAction action,
                   Target target,
                   int interceptor) {
    if (!call)
      return;
    if (TakeInputDropped()) {
      watching_.erase(call.get());
      call->Fail(kInputDropped);
      return;
    }
    base::DictValue finish;
    finish.Set("interceptor", interceptor);
    if (action.checked)
      finish.Set("checked", *action.checked);
    TargetStep(call.get(), target, "pointerFinish", std::move(finish),
               base::BindOnce(&Automation::PointerFinished,
                              weak_factory_.GetWeakPtr(), call, action, target));
  }

  void PointerFinished(base::WeakPtr<Call> call,
                       PointerAction action,
                       Target target,
                       base::Value value) {
    if (!call)
      return;
    watching_.erase(call.get());
    const base::Value* result =
        value.is_dict() ? value.GetDict().Find("result") : nullptr;
    if (result && result->is_dict()) {
      // Something else is under the point: again, as _retryAction() does.
      if (const std::string* description =
              result->GetDict().FindString("hitTargetDescription")) {
        call->Log(*description + " intercepts pointer events");
      }
      call->Log("retrying " + action.name + " action");
      ++call->attempt;
      PointerOn(call, action, std::move(target));
      return;
    }
    if (action.checked && !call->Flag("trial")) {
      std::optional<bool> checked =
          value.is_dict() ? value.GetDict().FindBool("checked") : std::nullopt;
      if (checked != action.checked) {
        call->Fail("Clicking the checkbox did not change its state");
        return;
      }
    }
    if (action.down_only) {
      DragTo(call.get());
      return;
    }
    AfterInput(call.get());
  }

  // Frame.dragAndDrop: down at the source ("source"), moved and up at the
  // target ("target"), each found as a pointer action finds its element.
  void DragAndDrop(Call* call) {
    PointerAction action;
    action.action = "drag";
    action.name = "move and down";
    action.down_only = true;
    action.wait_for_enabled = false;
    content::RenderFrameHost* frame = FrameOf(call);
    if (!frame) {
      call->Fail(kFrameDetached);
      return;
    }
    Target target;
    target.frame = Follow(frame);
    target.selector = call->String("source");
    target.strict = call->Flag("strict");
    PointerOn(call->GetWeakPtr(), action, std::move(target));
  }

  void DragTo(Call* call) {
    content::RenderFrameHost* frame = FrameOf(call);
    if (!frame) {
      call->Fail(kFrameDetached);
      return;
    }
    PointerAction action;
    action.action = "drag";
    action.name = "move and up";
    action.up_only = true;
    action.wait_for_enabled = false;
    call->attempt = 0;
    Target target;
    target.frame = Follow(frame);
    target.selector = call->String("target");
    target.strict = call->Flag("strict");
    PointerOn(call->GetWeakPtr(), action, std::move(target));
  }

  // A navigation the input started is waited for until it commits, unless
  // noWaitAfter.
  void AfterInput(Call* call) {
    auto it = watched_navigations_.find(call);
    if (it == watched_navigations_.end() || call->Flag("noWaitAfter")) {
      watched_navigations_.erase(call);
      call->Done();
      return;
    }
    call->Log("waiting for scheduled navigations to finish");
    navigation_waiters_.push_back({call->GetWeakPtr(), it->second, "commit",
                                   base::Value(),
                                   MainFrame()->GetFrameTreeNodeId()});
    watched_navigations_.erase(it);
  }

  // dom.ts's _fill(): the element's states and the injected script's fill,
  // the text then committed by the agent in the same call.
  void Fill(Call* call) {
    base::DictValue arg;
    arg.Set("value", call->String("value"));
    arg.Set("force", call->Flag("force"));
    if (!call->OnHandle())
      call->Log("waiting for " + call->String("selector"));
    StepOnTarget(call, "fill", std::move(arg),
                 base::BindOnce(
                     [](base::WeakPtr<Automation> self, Call* call,
                        base::Value value) {
                       base::Value result = ResultOf(value);
                       if (self && result.is_dict() &&
                           result.GetDict().FindBool("pressDelete").value_or(false)) {
                         self->Press(call, "Delete", 0, Done(call));
                         return;
                       }
                       call->Done();
                     },
                     weak_factory_.GetWeakPtr()));
  }

  void Focus(Call* call) { FocusThen(call, false, Done(call)); }

  void FocusThen(Call* call, bool reset_selection, base::OnceClosure then) {
    base::DictValue arg;
    arg.Set("resetSelection", reset_selection);
    if (!call->OnHandle())
      call->Log("waiting for " + call->String("selector"));
    StepOnTarget(call, "focus", std::move(arg),
                 base::BindOnce([](base::OnceClosure then, Call*,
                                   base::Value) { std::move(then).Run(); },
                                std::move(then)));
  }

  void Blur(Call* call) { StepOnTarget(call, "blur", {}, AsDone()); }

  void SelectText(Call* call) {
    base::DictValue arg;
    arg.Set("force", call->Flag("force"));
    StepOnTarget(call, "selectText", std::move(arg), AsDone());
  }

  void ScrollIntoViewIfNeeded(Call* call) {
    StepOnTarget(call, "scrollIntoViewIfNeeded", {}, AsDone());
  }

  // Frame.dispatchEvent, in the page's world as frames.ts has it.
  void DispatchEvent(Call* call) {
    base::DictValue arg;
    arg.Set("type", call->String("type"));
    if (const base::Value* init = call->params().Find("eventInit"))
      arg.Set("eventInit", init->Clone());
    StepOnTarget(call, "dispatchEvent", std::move(arg), AsDone(),
                 AutomationWorld::kMain);
  }

  // dom.ts's _selectOption(): options as {valueOrLabel}, {value}, {label},
  // {index}, or the handles of <option> elements ("elements").
  void SelectOption(Call* call) { OnTarget(call, &Automation::SelectOptionOn); }

  void SelectOptionOn(base::WeakPtr<Call> call, Target target) {
    base::DictValue arg;
    arg.Set("force", call->Flag("force"));
    base::ListValue options;
    if (const base::ListValue* given = call->params().FindList("options")) {
      for (const base::Value& option : *given)
        options.Append(option.Clone());
    }
    arg.Set("options", std::move(options));
    arg.Set("selector", target.selector);
    arg.Set("strict", target.strict);
    std::vector<int32_t> handles;
    if (target.scope) {
      base::DictValue scope;
      scope.Set("$h", 0);
      arg.Set("scope", std::move(scope));
      handles.push_back(target.scope);
    }
    content::RenderFrameHost* frame = Resolve(target.frame);
    base::ListValue elements;
    if (const base::ListValue* given = call->params().FindList("elements")) {
      for (const base::Value& element : *given) {
        const std::string* id =
            element.is_dict() ? element.GetDict().FindString("handle")
            : element.is_string() ? &element.GetString()
                                  : nullptr;
        const HandleRef* handle = id ? FindHandle(*id) : nullptr;
        if (!handle || !frame || handle->frame != frame->GetGlobalId()) {
          call->Fail(
              "JSHandles can be evaluated only in the context they were created!");
          return;
        }
        base::DictValue reference;
        reference.Set("$h", static_cast<int>(handles.size()));
        handles.push_back(handle->id);
        elements.Append(std::move(reference));
      }
    }
    arg.Set("elements", std::move(elements));
    RunStep(call.get(), target.frame, AutomationWorld::kUtility, "selectOption",
            std::move(arg), std::move(handles),
            Then(call.get(), AsPolledKey("values", "values")));
  }

  // dom.ts's _setInputFiles(): contents ("payloads": name, mimeType, buffer
  // as base64) set by the injected script; paths ("localPaths",
  // "localDirectory") by the agent, as a person choosing them would.
  void SetInputFiles(Call* call) {
    base::DictValue arg;
    std::vector<std::string> paths;
    if (const base::ListValue* payloads = call->params().FindList("payloads"))
      arg.Set("payloads", payloads->Clone());
    if (const base::ListValue* local = call->params().FindList("localPaths")) {
      for (const base::Value& path : *local) {
        if (path.is_string())
          paths.push_back(path.GetString());
      }
      arg.Set("localPaths", local->Clone());
    }
    if (call->Has("localDirectory")) {
      paths = {call->String("localDirectory")};
      arg.Set("localDirectory", call->String("localDirectory"));
    }
    for (const std::string& path : paths) {
      if (!base::PathExists(base::FilePath::FromUTF8Unsafe(path))) {
        call->Fail("ENOENT: no such file or directory, access '" + path + "'");
        return;
      }
    }
    StepOnTarget(call, "setInputFiles", std::move(arg),
                 base::BindOnce(&Automation::InputFilesChecked,
                                weak_factory_.GetWeakPtr(), std::move(paths)));
  }

  void InputFilesChecked(std::vector<std::string> paths,
                         Call* call,
                         base::Value value) {
    base::Value result = ResultOf(value);
    if (!result.is_dict() ||
        result.GetDict().FindBool("done").value_or(false)) {
      call->Done();
      return;
    }
    const std::string* id =
        result.GetDict().FindStringByDottedPath("input.handle");
    const HandleRef* handle = id ? FindHandle(*id) : nullptr;
    content::RenderFrameHost* frame =
        handle ? content::RenderFrameHost::FromID(handle->frame) : nullptr;
    if (!frame) {
      call->Fail(kDetached);
      return;
    }
    // As DevTools' DOM.setFileInputFiles: without it the renderer cannot read
    // them, and leaving the page, whose state names them, gets it killed
    // (RFH_CAN_ACCESS_FILES_OF_PAGE_STATE).
    auto* security = content::ChildProcessSecurityPolicy::GetInstance();
    for (const std::string& path : paths) {
      security->GrantReadFile(frame->GetProcess()->GetID(),
                              base::FilePath::FromUTF8Unsafe(path));
    }
    const std::string kept = *id;
    Agent(frame)->SetInputFiles(
        handle->id, paths,
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(
            base::BindOnce(
                [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call,
                   std::string kept, bool ok) {
                  if (self)
                    self->Dispose(kept);
                  if (!call)
                    return;
                  if (!ok) {
                    call->Fail("Node is not an HTMLInputElement");
                    return;
                  }
                  call->Done();
                },
                weak_factory_.GetWeakPtr(), call->GetWeakPtr(), kept),
            false));
  }

  // dom.ts's _press() and _type(): the element focused, then the keys.
  void FramePress(Call* call) {
    FocusThen(call, true,
              base::BindOnce(
                  [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call) {
                    if (!self || !call)
                      return;
                    self->Gated<&Automation::FramePressKeys>(call.get());
                  },
                  weak_factory_.GetWeakPtr(), call->GetWeakPtr()));
  }

  void FramePressKeys(Call* call) {
    watched_navigations_.erase(call);
    watching_.insert(call);
    Press(call, call->String("key"),
          static_cast<int>(call->Number("delay")),
          base::BindOnce(
              [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call) {
                if (!self || !call)
                  return;
                self->watching_.erase(call.get());
                if (self->TakeInputDropped()) {
                  call->Fail(kInputDropped);
                  return;
                }
                self->AfterInput(call.get());
              },
              weak_factory_.GetWeakPtr(), call->GetWeakPtr()));
  }

  void FrameTypeKeys(Call* call) {
    TypeText(call, CodePoints(call->String("text")), 0,
             static_cast<int>(call->Number("delay")));
  }

  void FrameType(Call* call) {
    FocusThen(call, true,
              base::BindOnce(
                  [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call) {
                    if (self && call)
                      self->Gated<&Automation::FrameTypeKeys>(call.get());
                  },
                  weak_factory_.GetWeakPtr(), call->GetWeakPtr()));
  }

  // ---- Navigation ------------------------------------------------------

  struct NavigationWaiter {
    base::WeakPtr<Call> call;
    // The navigation; -1 until the one asked for starts, 0 for a lifecycle
    // state (waitForLoadState).
    int64_t navigation_id;
    // commit, domcontentloaded, load or networkidle.
    std::string wait_until;
    // The main resource's response, for the command's answer.
    base::Value response;
    // The frame navigated.
    content::FrameTreeNodeId frame;
  };

  // The lifecycle of a frame's document, as Playwright's loadstate events.
  struct Lifecycle {
    bool dom_content_loaded = false;
    bool loaded = false;
    bool network_idle = false;
  };

  void Goto(Call* call) {
    const GURL url(call->String("url"));
    content::RenderFrameHost* frame = FrameOf(call);
    if (!frame) {
      call->Fail(kFrameDetached);
      return;
    }
    if (!url.is_valid()) {
      call->Fail(
          "Protocol error (Page.navigate): Cannot navigate to invalid URL");
      return;
    }
    if (frame == MainFrame()) {
      StartNavigation(call, frame,
                      base::BindOnce([](zebkit_view* view,
                                        GURL url) { view->LoadUrl(url); },
                                     call->view(), url));
      return;
    }
    StartNavigation(call, frame,
                    base::BindOnce(
                        [](content::WebContents* contents,
                           content::FrameTreeNodeId node, GURL url) {
                          content::NavigationController::LoadURLParams params(url);
                          params.frame_tree_node_id = node;
                          params.transition_type = ui::PAGE_TRANSITION_LINK;
                          contents->GetController().LoadURLWithParams(params);
                        },
                        web_contents(), frame->GetFrameTreeNodeId(), url));
  }

  void Reload(Call* call) {
    StartNavigation(call, MainFrame(),
                    base::BindOnce(
                        [](content::WebContents* contents) {
                          contents->GetController().Reload(
                              content::ReloadType::NORMAL, false);
                        },
                        web_contents()));
  }

  void GoBack(Call* call) { History(call, -1); }
  void GoForward(Call* call) { History(call, 1); }

  void History(Call* call, int offset) {
    content::NavigationController& controller = web_contents()->GetController();
    if (!controller.CanGoToOffset(offset)) {
      call->Done();
      return;
    }
    StartNavigation(call, MainFrame(),
                    base::BindOnce(
                        [](content::WebContents* contents, int offset) {
                          contents->GetController().GoToOffset(offset);
                        },
                        web_contents(), offset));
  }

  void StartNavigation(Call* call,
                       content::RenderFrameHost* frame,
                       base::OnceClosure navigate) {
    std::string wait_until = call->String("waitUntil");
    if (wait_until.empty())
      wait_until = "load";
    if (wait_until != "load" && wait_until != "domcontentloaded" &&
        wait_until != "commit" && wait_until != "networkidle") {
      call->Fail(
          "waitUntil: expected one of (load|domcontentloaded|networkidle|commit)");
      return;
    }
    if (call->Has("url")) {
      call->Log("navigating to \"" + call->String("url") +
                "\", waiting until \"" + wait_until + "\"");
    }
    navigation_waiters_.push_back({call->GetWeakPtr(), -1, wait_until,
                                   base::Value(), frame->GetFrameTreeNodeId()});
    // As a task: a client's next command comes from inside the answer to its last,
    // which may be inside the navigation that ended, or a renderer's exit, where
    // content does not take a new navigation.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(
                       [](base::WeakPtr<Call> call, base::OnceClosure navigate) {
                         if (call && !call->finished())
                           std::move(navigate).Run();
                       },
                       call->GetWeakPtr(), std::move(navigate)));
  }

  // Frame.waitForLoadState: answered when the frame's document has reached
  // @state (Playwright's client waits for loadstate events).
  void WaitForLoadState(Call* call) {
    content::RenderFrameHost* frame = FrameOf(call);
    if (!frame) {
      call->Fail(kFrameDetached);
      return;
    }
    std::string state = call->String("state");
    if (state.empty())
      state = "load";
    const content::FrameTreeNodeId node = frame->GetFrameTreeNodeId();
    auto found = lifecycles_.find(node);
    // A document loaded before automation looked is loaded.
    const bool unknown = found == lifecycles_.end();
    const Lifecycle lifecycle = unknown ? Lifecycle() : found->second;
    const bool reached =
        (unknown && !web_contents()->IsLoading()) ||
        (state == "domcontentloaded" && lifecycle.dom_content_loaded) ||
        (state == "load" && lifecycle.loaded) ||
        (state == "networkidle" && lifecycle.network_idle);
    if (reached) {
      call->Done();
      return;
    }
    navigation_waiters_.push_back({call->GetWeakPtr(), 0, state,
                                   base::Value(base::DictValue()), node});
  }

  // content::WebContentsObserver
  void DidStartNavigation(content::NavigationHandle* navigation) override {
    for (NavigationWaiter& waiter : navigation_waiters_) {
      if (waiter.navigation_id == -1 &&
          navigation->GetFrameTreeNodeId() == waiter.frame &&
          !navigation->IsRendererInitiated()) {
        waiter.navigation_id = navigation->GetNavigationId();
        break;
      }
    }
    if (!navigation->IsInPrimaryMainFrame())
      return;
    for (Call* call : watching_)
      watched_navigations_[call] = navigation->GetNavigationId();
  }

  void DidFinishNavigation(content::NavigationHandle* navigation) override {
    const int64_t id = navigation->GetNavigationId();
    const content::FrameTreeNodeId node = navigation->GetFrameTreeNodeId();
    if (navigation->HasCommitted() && !navigation->IsSameDocument()) {
      // A new document: its lifecycle starts again.
      lifecycles_[node] = Lifecycle();
    }
    if (navigation->HasCommitted() && navigation->GetRenderFrameHost()) {
      base::DictValue params = FrameValue(navigation->GetRenderFrameHost());
      params.Set("sameDocument", navigation->IsSameDocument());
      Emit("Frame.navigated", std::move(params));
    }
    for (auto it = watched_navigations_.begin();
         it != watched_navigations_.end();) {
      // An input's navigation that ended: nothing to wait for.
      if (it->second == id)
        it = watched_navigations_.erase(it);
      else
        ++it;
    }
    // An answer's callback may send the next command, which adds a waiter (a
    // deque: this one stays where it is) and may come back here.
    ++settling_;
    for (size_t i = 0; i < navigation_waiters_.size(); ++i) {
      NavigationWaiter& waiter = navigation_waiters_[i];
      if (waiter.navigation_id != id || !waiter.call)
        continue;
      if (!navigation->HasCommitted() || navigation->IsErrorPage()) {
        waiter.call->Fail(net::ErrorToString(navigation->GetNetErrorCode()) +
                          " at " + navigation->GetURL().spec());
        continue;
      }
      waiter.response = base::Value(ResponseOf(navigation));
      if (waiter.wait_until == "commit" || navigation->IsSameDocument())
        Answer(waiter);
    }
    --settling_;
    Settle();
  }

  base::DictValue ResponseOf(content::NavigationHandle* navigation) {
    // What was kept is of an earlier load: these two load nothing.
    Network* network = navigation->IsSameDocument() || navigation->IsServedFromBackForwardCache()
        ? nullptr : Network::For(web_contents());
    if (network) {
      if (auto response = network->NavigationResponse(navigation->GetURL().spec()))
        return std::move(*response);
    }
    base::DictValue response;
    response.Set("url", navigation->GetURL().spec());
    const net::HttpResponseHeaders* headers = navigation->GetResponseHeaders();
    if (!headers) {
      response.Set("status", 200);
      return response;
    }
    response.Set("status", headers->response_code());
    response.Set("statusText", headers->GetStatusText());
    base::ListValue list;
    size_t iterator = 0;
    std::string name, value;
    while (headers->EnumerateHeaderLines(&iterator, &name, &value)) {
      base::DictValue header;
      header.Set("name", name);
      header.Set("value", value);
      list.Append(std::move(header));
    }
    response.Set("headers", std::move(list));
    return response;
  }

  void LoadState(content::RenderFrameHost* frame, const char* state) {
    base::DictValue params;
    params.Set("frame", FrameId(frame));
    params.Set("state", state);
    Emit("Frame.loadstate", std::move(params));
    Settle();
  }

  void DOMContentLoaded(content::RenderFrameHost* frame) override {
    lifecycles_[frame->GetFrameTreeNodeId()].dom_content_loaded = true;
    LoadState(frame, "domcontentloaded");
  }

  void DidFinishLoad(content::RenderFrameHost* frame, const GURL&) override {
    Lifecycle& lifecycle = lifecycles_[frame->GetFrameTreeNodeId()];
    lifecycle.dom_content_loaded = lifecycle.loaded = true;
    LoadState(frame, "load");
    // networkidle: 500 ms with no new navigation after the load, as
    // Playwright's 500 ms without requests (the network's own events come
    // with the network hook, docs/AUTOMATION.md).
    idle_timers_[frame->GetFrameTreeNodeId()].Start(
        FROM_HERE, base::Milliseconds(500),
        base::BindOnce(&Automation::NetworkIdle, base::Unretained(this),
                       frame->GetFrameTreeNodeId()));
  }

  void NetworkIdle(content::FrameTreeNodeId node) {
    auto found = lifecycles_.find(node);
    if (found == lifecycles_.end() || !found->second.loaded)
      return;
    found->second.network_idle = true;
    if (content::RenderFrameHost* frame = FrameByNode(node))
      LoadState(frame, "networkidle");
  }

  // Answers the navigations whose lifecycle state has come.
  void Settle() {
    ++settling_;
    for (size_t i = 0; i < navigation_waiters_.size(); ++i) {
      NavigationWaiter& waiter = navigation_waiters_[i];
      if (!waiter.call || waiter.response.is_none())
        continue;
      auto found = lifecycles_.find(waiter.frame);
      if (found == lifecycles_.end())
        continue;
      const Lifecycle& lifecycle = found->second;
      if ((waiter.wait_until == "domcontentloaded" &&
           lifecycle.dom_content_loaded) ||
          (waiter.wait_until == "load" && lifecycle.loaded) ||
          (waiter.wait_until == "networkidle" && lifecycle.network_idle)) {
        Answer(waiter);
      }
    }
    // Only the outermost: an inner one would move what the outer one is walking.
    if (--settling_)
      return;
    std::erase_if(navigation_waiters_, [](const NavigationWaiter& waiter) {
      return !waiter.call || waiter.call->finished();
    });
  }

  // Navigations answer with their response; an action that navigated, and
  // a load state, with nothing.
  void Answer(NavigationWaiter& waiter) {
    const std::string name = waiter.call->Name();
    base::DictValue answer;
    if (name == "goto" || name == "reload" || name == "goBack" ||
        name == "goForward") {
      answer.Set("response", waiter.response.Clone());
    }
    waiter.call->Done(base::Value(std::move(answer)));
  }

  void FrameDeleted(content::FrameTreeNodeId node) override {
    lifecycles_.erase(node);
    idle_timers_.erase(node);
    base::DictValue params;
    params.Set("frame", "f" + base::NumberToString(node.value()));
    Emit("Page.frameDetached", std::move(params));
  }

  void PrimaryPageChanged(content::Page&) override {
    // The last move's widget may live on in the back-forward cache.
    mouse_widget_ = nullptr;
    EmulateFocus();
  }

  void RenderFrameCreated(content::RenderFrameHost* frame) override {
    if (!media_type_.empty())
      Agent(frame)->SetMediaType(media_type_);
    if (!exposed_.empty())
      Agent(frame)->Expose(exposed_);
    if (frame->GetParent() && frame->IsActive())
      Emit("Page.frameAttached", FrameValue(frame));
  }

  void RenderFrameDeleted(content::RenderFrameHost* frame) override {
    const content::GlobalRenderFrameHostId id = frame->GetGlobalId();
    agents_.erase(id);
    std::erase_if(handles_,
                  [id](const auto& entry) { return entry.second.frame == id; });
  }

  void OnDidAddMessageToConsole(
      content::RenderFrameHost* frame,
      blink::mojom::ConsoleMessageLevel level,
      const std::u16string& message,
      int32_t line,
      const std::u16string& source,
      const std::optional<std::u16string>&) override {
    const char* type = "log";
    switch (level) {
      case blink::mojom::ConsoleMessageLevel::kVerbose:
        type = "debug";
        break;
      case blink::mojom::ConsoleMessageLevel::kWarning:
        type = "warning";
        break;
      case blink::mojom::ConsoleMessageLevel::kError:
        type = "error";
        break;
      default:
        break;
    }
    base::DictValue params;
    params.Set("type", type);
    params.Set("text", base::UTF16ToUTF8(message));
    params.Set("frame", FrameId(frame));
    base::DictValue location;
    location.Set("url", base::UTF16ToUTF8(source));
    location.Set("lineNumber", line);
    params.Set("location", std::move(location));
    Emit("BrowserContext.console", std::move(params));
  }

  void PrimaryMainFrameRenderProcessGone(base::TerminationStatus) override {
    Emit("Page.crash", {});
  }

  void WebContentsDestroyed() override {
    if (video_) {
      // Playwright's recordVideo: the file finished when the page closes.
      VideoRecorder* raw = video_.get();
      raw->Stop(base::BindOnce(
          [](std::unique_ptr<VideoRecorder>, const base::FilePath&,
             const std::string&) {},
          std::move(video_)));
    }
    Emit("Page.close", {});
    std::vector<base::WeakPtr<Call>> calls = std::move(calls_);
    for (const base::WeakPtr<Call>& call : calls) {
      if (call)
        call->Fail(kClosed);
    }
  }

  // ---- Dialogs and file choosers (Page.updateSubscription) -------------

  void UpdateSubscription(Call* call) {
    const std::string event = call->String("event");
    const bool enabled = call->Flag("enabled");
    if (event == "dialog") {
      hold_dialogs_ = enabled;
    } else if (event == "fileChooser") {
      intercept_file_choosers_ = enabled;
    }
    // Other events (console, request, response, ...) are always sent.
    call->Done();
  }

  bool holds_dialogs() const { return hold_dialogs_; }

  void DialogOpened(const zebkit_dialog& dialog) {
    static const char* const kTypes[] = {"alert", "confirm", "prompt",
                                         "beforeunload", "print"};
    if (dialog.type == ZEBKIT_DIALOG_PRINT)
      return;
    base::DictValue params;
    params.Set("dialog", "d" + base::NumberToString(dialog.id));
    params.Set("type", kTypes[dialog.type]);
    params.Set("message", dialog.message ? dialog.message : "");
    params.Set("defaultValue",
               dialog.default_prompt ? dialog.default_prompt : "");
    params.Set("held", dialog.held);
    Emit("BrowserContext.dialog", std::move(params));
  }

  // Dialog.accept and dismiss.
  void DialogAccept(Call* call) { RespondToDialog(call, true); }
  void DialogDismiss(Call* call) { RespondToDialog(call, false); }
  void RespondToDialog(Call* call, bool accept) {
    const std::string id = call->String("dialog");
    uint64_t number = 0;
    const std::string prompt = call->String("promptText");
    if (id.size() < 2 ||
        !base::StringToUint64(std::string_view(id).substr(1), &number) ||
        !zebkit_view_dialog_respond(call->view(), number, accept,
                                    call->Has("promptText") ? prompt.c_str()
                                                            : nullptr)) {
      call->Fail("Cannot accept dialog which is already handled!");
      return;
    }
    call->Done();
  }

  // A file chooser the page opened while the client takes them: kept for
  // FileChooser.setFiles, reported.
  bool TakeFileChooser(content::RenderFrameHost* frame,
                       scoped_refptr<content::FileSelectListener> listener,
                       const blink::mojom::FileChooserParams& params) {
    if (!intercept_file_choosers_)
      return false;
    const std::string id = "c" + base::NumberToString(++next_chooser_);
    file_choosers_[id] = {std::move(listener), params.mode};
    base::DictValue event;
    event.Set("fileChooser", id);
    event.Set("frame", FrameId(frame));
    event.Set("isMultiple",
              params.mode == blink::mojom::FileChooserParams::Mode::kOpenMultiple);
    event.Set("isDirectory",
              params.mode == blink::mojom::FileChooserParams::Mode::kUploadFolder);
    Emit("Page.fileChooser", std::move(event));
    return true;
  }

  // FileChooser.setFiles: the files ("localPaths"; none to cancel) given to
  // the page as the person's choice.
  void FileChooserSetFiles(Call* call) {
    auto found = file_choosers_.find(call->String("fileChooser"));
    if (found == file_choosers_.end()) {
      call->Fail("File chooser is already handled");
      return;
    }
    FileChooser chooser = std::move(found->second);
    file_choosers_.erase(found);
    std::vector<blink::mojom::FileChooserFileInfoPtr> files;
    if (const base::ListValue* paths = call->params().FindList("localPaths")) {
      for (const base::Value& path : *paths) {
        if (!path.is_string())
          continue;
        files.push_back(blink::mojom::FileChooserFileInfo::NewNativeFile(
            blink::mojom::NativeFileInfo::New(
                base::FilePath::FromUTF8Unsafe(path.GetString()),
                std::u16string(), std::vector<std::u16string>())));
      }
    }
    if (files.empty()) {
      chooser.listener->FileSelectionCanceled();
    } else {
      chooser.listener->FileSelected(std::move(files), base::FilePath(),
                                     chooser.mode);
    }
    call->Done();
  }

  // ---- Keyboard --------------------------------------------------------

  // An input command, once the page's widget takes input.
  template <void (Automation::*method)(Call*)>
  void Gated(Call* call) {
    TakeInputDropped();
    WhenInputReady(base::BindOnce(
        [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call) {
          if (self && call)
            (self.get()->*method)(call.get());
        },
        weak_factory_.GetWeakPtr(), call->GetWeakPtr()));
  }

  void KeyboardDown(Call* call) {
    std::optional<Type> sent = KeyDown(call, call->String("key"));
    if (sent)
      WhenAcked(*sent, InputDone(call));
  }

  void KeyboardUp(Call* call) {
    if (!KeyUp(call, call->String("key")))
      return;
    WhenAcked(Type::kKeyUp, InputDone(call));
  }

  void KeyboardPress(Call* call) {
    Press(call, call->String("key"), static_cast<int>(call->Number("delay")),
          InputDone(call));
  }

  void KeyboardType(Call* call) {
    TypeText(call, CodePoints(call->String("text")), 0,
             static_cast<int>(call->Number("delay")));
  }

  void KeyboardInsertText(Call* call) {
    InsertText(call->String("text"), Done(call));
  }

  static base::OnceClosure Done(Call* call) {
    return base::BindOnce(
        [](base::WeakPtr<Call> call) {
          if (call)
            call->Done();
        },
        call->GetWeakPtr());
  }

  // input.ts's _keyDescriptionForString(): the key with Shift held, and no
  // text when another modifier is.
  std::optional<playwright::KeyDescription> Describe(Call* call,
                                                     const std::string& name) {
    const std::string key = ResolveSmartModifier(name);
    const playwright::KeyEntry* entry = FindKey(key);
    if (!entry) {
      call->Fail("Unknown key: \"" + key + "\"");
      return std::nullopt;
    }
    playwright::KeyDescription description = entry->description;
    if (pressed_modifiers_.count("Shift") && entry->has_shifted)
      description = entry->shifted;
    if (pressed_modifiers_.size() > pressed_modifiers_.count("Shift"))
      description.text = "";
    return description;
  }

  // The event sent: a key down with text or without.
  std::optional<Type> KeyDown(Call* call, const std::string& name) {
    std::optional<playwright::KeyDescription> description = Describe(call, name);
    if (!description)
      return std::nullopt;
    const bool auto_repeat = pressed_keys_.count(description->code) > 0;
    pressed_keys_.insert(description->code);
    if (IsModifier(description->key))
      pressed_modifiers_.insert(description->key);
    return SendKey(*description, /*down=*/true, auto_repeat);
  }

  bool KeyUp(Call* call, const std::string& name) {
    std::optional<playwright::KeyDescription> description = Describe(call, name);
    if (!description)
      return false;
    if (IsModifier(description->key))
      pressed_modifiers_.erase(description->key);
    pressed_keys_.erase(description->code);
    SendKey(*description, /*down=*/false, false);
    return true;
  }

  // Keyboard.press(): "Control+Shift+A" is the modifiers down, the key down
  // and up, the modifiers up.
  void Press(Call* call,
             const std::string& keys,
             int delay,
             base::OnceClosure done) {
    std::vector<std::string> tokens = SplitKeys(keys);
    for (const std::string& token : tokens) {
      if (!KeyDown(call, token))
        return;
    }
    auto release = base::BindOnce(
        [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call,
           std::vector<std::string> tokens, base::OnceClosure done) {
          if (!self || !call)
            return;
          for (auto it = tokens.rbegin(); it != tokens.rend(); ++it) {
            if (!self->KeyUp(call.get(), *it))
              return;
          }
          self->WhenAcked(Type::kKeyUp, std::move(done));
        },
        weak_factory_.GetWeakPtr(), call->GetWeakPtr(), std::move(tokens),
        std::move(done));
    if (delay) {
      base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE, std::move(release), base::Milliseconds(delay));
    } else {
      std::move(release).Run();
    }
  }

  // Keyboard.type(): a key of the layout pressed, other text inserted.
  void TypeText(Call* call,
                std::vector<std::string> characters,
                size_t index,
                int delay) {
    if (call->finished())
      return;
    if (index >= characters.size()) {
      if (TakeInputDropped())
        call->Fail(kInputDropped);
      else
        call->Done();
      return;
    }
    auto next = base::BindOnce(
        [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call,
           std::vector<std::string> characters, size_t index, int delay) {
          if (self && call) {
            self->TypeText(call.get(), std::move(characters), index + 1,
                           delay);
          }
        },
        weak_factory_.GetWeakPtr(), call->GetWeakPtr(), characters, index,
        delay);
    const std::string& character = characters[index];
    if (FindKey(character)) {
      Press(call, character, delay, std::move(next));
      return;
    }
    auto insert = base::BindOnce(
        [](base::WeakPtr<Automation> self, std::string text,
           base::OnceClosure next) {
          if (self)
            self->InsertText(text, std::move(next));
        },
        weak_factory_.GetWeakPtr(), character, std::move(next));
    if (delay) {
      base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE, std::move(insert), base::Milliseconds(delay));
    } else {
      std::move(insert).Run();
    }
  }

  void InsertText(const std::string& text, base::OnceClosure done) {
    content::RenderFrameHost* frame = web_contents()->GetFocusedFrame();
    if (!frame)
      frame = MainFrame();
    Agent(frame)->InsertText(
        text, mojo::WrapCallbackWithDefaultInvokeIfNotRun(std::move(done)));
  }

  int Modifiers() const {
    int modifiers = 0;
    for (const std::string& modifier : pressed_modifiers_)
      modifiers |= ModifierBit(modifier);
    return modifiers;
  }

  // Keyboard.ensureModifiers(): exactly @wanted held.
  void EnsureModifiers(const std::set<std::string>& wanted) {
    for (const char* key : {"Alt", "Control", "Meta", "Shift"}) {
      const bool want = wanted.count(key) > 0;
      const bool down = pressed_modifiers_.count(key) > 0;
      const playwright::KeyEntry* entry = FindKey(key);
      if (!entry || want == down)
        continue;
      if (want) {
        pressed_modifiers_.insert(key);
        pressed_keys_.insert(entry->description.code);
      } else {
        pressed_modifiers_.erase(key);
        pressed_keys_.erase(entry->description.code);
      }
      SendKey(entry->description, want, false);
    }
  }

  // crInput.ts's keydown() and keyup(), as CDP's Input.dispatchKeyEvent
  // makes the event (input_handler.cc), to the focused frame's widget.
  Type SendKey(const playwright::KeyDescription& description,
               bool down,
               bool auto_repeat) {
    const std::string text = down ? description.text : "";
    const Type type = !down          ? Type::kKeyUp
                      : text.empty() ? Type::kRawKeyDown
                                     : Type::kKeyDown;
    int modifiers = Modifiers();
    if (auto_repeat)
      modifiers |= blink::WebInputEvent::kIsAutoRepeat;
    if (description.location == playwright::kKeypadLocation)
      modifiers |= blink::WebInputEvent::kIsKeyPad;
    else if (description.location == 1)
      modifiers |= blink::WebInputEvent::kIsLeft;
    else if (description.location == 2)
      modifiers |= blink::WebInputEvent::kIsRight;
    input::NativeWebKeyboardEvent event(type, modifiers,
                                        base::TimeTicks::Now());
    const std::u16string text16 = base::UTF8ToUTF16(text);
    for (size_t i = 0;
         i < text16.size() && i + 1 < blink::WebKeyboardEvent::kTextLengthCap;
         ++i) {
      event.text[i] = text16[i];
      event.unmodified_text[i] = text16[i];
    }
    event.windows_key_code = description.key_code_without_location;
    event.dom_code = static_cast<int>(
        ui::KeycodeConverter::CodeStringToDomCode(description.code));
    event.dom_key = static_cast<int>(
        ui::KeycodeConverter::KeyStringToDomKey(description.key));
    // As CDP's: without a native event, an unhandled key does not go on to
    // the browser's own shortcuts.
    event.skip_if_unhandled = true;
    content::RenderFrameHost* frame = web_contents()->GetFocusedFrame();
    if (!frame)
      frame = MainFrame();
    content::RenderWidgetHost* widget = frame->GetRenderWidgetHost();
    Observe(widget);
    last_sent_[type] = event.TimeStamp();
    widget->ForwardKeyboardEvent(event);
    return type;
  }

  // ---- Mouse and touch -------------------------------------------------

  // @done with the widget under @point of the page and the point in it.
  void AtPoint(const gfx::PointF& point,
               base::OnceCallback<void(content::RenderWidgetHost*, gfx::PointF)>
                   done) {
    content::FindWidgetAtPoint(
        web_contents(), point,
        base::BindOnce(
            [](base::WeakPtr<Automation> self, gfx::PointF point,
               base::OnceCallback<void(content::RenderWidgetHost*, gfx::PointF)>
                   done,
               content::RenderWidgetHost* widget, gfx::PointF local) {
              if (!self)
                return;
              if (!widget) {
                widget = self->MainWidget();
                local = point;
              }
              std::move(done).Run(widget, local);
            },
            weak_factory_.GetWeakPtr(), point, std::move(done)));
  }

  void MouseMoveCommand(Call* call) {
    const gfx::PointF to(call->Number("x"), call->Number("y"));
    AtPoint(to, base::BindOnce(
                    [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call,
                       gfx::PointF to, content::RenderWidgetHost* widget,
                       gfx::PointF local) {
                      if (!self || !call)
                        return;
                      self->MouseMoveIn(
                          widget, local, to,
                          std::max(1, static_cast<int>(call->Number("steps", 1))));
                      self->WhenAcked(Type::kMouseMove, self->InputDone(call.get()));
                    },
                    weak_factory_.GetWeakPtr(), call->GetWeakPtr(), to));
  }

  void MouseDownCommand(Call* call) {
    MouseDownIn(Widget(), ButtonOf(call->String("button")),
                static_cast<int>(call->Number("clickCount", 1)));
    WhenAcked(Type::kMouseDown, InputDone(call));
  }

  void MouseUpCommand(Call* call) {
    MouseUpIn(Widget(), ButtonOf(call->String("button")),
              static_cast<int>(call->Number("clickCount", 1)));
    WhenAcked(Type::kMouseUp, InputDone(call));
  }

  void MouseClickCommand(Call* call) {
    const gfx::PointF to(call->Number("x"), call->Number("y"));
    AtPoint(to, base::BindOnce(
                    [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call,
                       gfx::PointF to, content::RenderWidgetHost* widget,
                       gfx::PointF local) {
                      if (!self || !call)
                        return;
                      self->MouseMoveIn(
                          widget, local, to,
                          std::max(1, static_cast<int>(call->Number("steps", 1))));
                      self->PressAndRelease(
                          call.get(), widget, ButtonOf(call->String("button")),
                          static_cast<int>(call->Number("clickCount", 1)), 1,
                          static_cast<int>(call->Number("delay")),
                          self->InputDone(call.get()));
                    },
                    weak_factory_.GetWeakPtr(), call->GetWeakPtr(), to));
  }

  void MouseWheelCommand(Call* call) {
    content::RenderWidgetHost* widget = Widget();
    if (!widget) {
      call->Fail(kClosed);
      return;
    }
    Observe(widget);
    for (bool begin : {true, false}) {
      blink::WebMouseWheelEvent event(Type::kMouseWheel,
                                      Modifiers() | ButtonModifiers(),
                                      base::TimeTicks::Now());
      event.SetPositionInWidget(mouse_in_widget_);
      event.SetPositionInScreen(ScreenPoint(widget, mouse_in_widget_));
      event.delta_units = ui::ScrollGranularity::kScrollByPrecisePixel;
      event.dispatch_type = blink::WebInputEvent::DispatchType::kBlocking;
      if (begin) {
        event.delta_x = -call->Number("deltaX");
        event.delta_y = -call->Number("deltaY");
        event.phase = blink::WebMouseWheelEvent::kPhaseBegan;
      } else {
        event.phase = blink::WebMouseWheelEvent::kPhaseEnded;
      }
      last_sent_[Type::kMouseWheel] = event.TimeStamp();
      widget->ForwardWheelEvent(event);
    }
    WhenAcked(Type::kMouseWheel, Done(call));
  }

  void TouchscreenTap(Call* call) {
    const gfx::PointF at(call->Number("x"), call->Number("y"));
    AtPoint(at, base::BindOnce(
                    [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call,
                       content::RenderWidgetHost* widget, gfx::PointF local) {
                      if (self && call)
                        self->SendTap(widget, local, Done(call.get()));
                    },
                    weak_factory_.GetWeakPtr(), call->GetWeakPtr()));
  }

  // crInput.ts's tap(): a touch started and ended at the point, as a
  // touchscreen's (patches/1038); the browser's gesture recognizer makes
  // the tap and the click of it. The page's touch events on first.
  void SendTap(content::RenderWidgetHost* widget,
               gfx::PointF point,
               base::OnceClosure done) {
    if (!widget) {
      std::move(done).Run();
      return;
    }
    EnableTouch();
    Observe(widget);
    for (Type type : {Type::kTouchStart, Type::kTouchEnd}) {
      blink::WebTouchEvent event(type, Modifiers(), base::TimeTicks::Now());
      event.touches_length = 1;
      blink::WebTouchPoint& touch = event.touches[0];
      touch.id = 0;
      touch.state = type == Type::kTouchStart
                        ? blink::WebTouchPoint::State::kStatePressed
                        : blink::WebTouchPoint::State::kStateReleased;
      touch.SetPositionInWidget(point);
      touch.SetPositionInScreen(ScreenPoint(widget, point));
      touch.radius_x = touch.radius_y = 1;
      touch.force = 1;
      touch.pointer_type = blink::WebPointerProperties::PointerType::kTouch;
      event.dispatch_type = blink::WebInputEvent::DispatchType::kBlocking;
      event.unique_touch_event_id = ui::GetNextTouchEventId();
      last_sent_[type] = event.TimeStamp();
      content::ForwardTouchEvent(widget, event);
    }
    WhenAcked(Type::kTouchEnd, std::move(done));
  }

  // Touch events for the page, as a context with hasTouch has them.
  void EnableTouch() {
    blink::web_pref::WebPreferences prefs =
        web_contents()->GetOrCreateWebPreferences();
    if (prefs.touch_event_feature_detection_enabled)
      return;
    prefs.touch_event_feature_detection_enabled = true;
    web_contents()->SetWebPreferences(prefs);
  }

  int ButtonModifiers() const {
    int modifiers = 0;
    for (Button button : buttons_)
      modifiers |= ButtonBit(button);
    return modifiers;
  }

  // The widget the mouse is over: the one the last move went to.
  content::RenderWidgetHost* Widget() {
    return mouse_widget_ && observed_.contains(mouse_widget_.get())
               ? mouse_widget_.get()
               : MainWidget();
  }

  // Mouse.move(): from where the mouse is, in @steps, in @widget.
  void MouseMoveIn(content::RenderWidgetHost* widget,
                   const gfx::PointF& to,
                   const gfx::PointF& root,
                   int steps) {
    const gfx::PointF from = widget == Widget() ? mouse_in_widget_ : to;
    for (int i = 1; i <= steps; ++i) {
      const gfx::PointF middle(from.x() + (to.x() - from.x()) * i / steps,
                               from.y() + (to.y() - from.y()) * i / steps);
      SendMouse(widget, Type::kMouseMove, middle, last_button_, 0);
    }
    mouse_widget_ = widget;
    mouse_in_widget_ = to;
    mouse_ = root;
  }

  void MouseDownIn(content::RenderWidgetHost* widget,
                   Button button,
                   int click_count) {
    last_button_ = button;
    buttons_.insert(button);
    SendMouse(widget, Type::kMouseDown, mouse_in_widget_, button, click_count);
  }

  void MouseUpIn(content::RenderWidgetHost* widget,
                 Button button,
                 int click_count) {
    last_button_ = Button::kNoButton;
    buttons_.erase(button);
    SendMouse(widget, Type::kMouseUp, mouse_in_widget_, button, click_count);
  }

  // Mouse.click()'s presses, the count going up as a double click's does.
  void PressAndRelease(Call* call,
                       content::RenderWidgetHost* widget,
                       Button button,
                       int click_count,
                       int current,
                       int delay,
                       base::OnceClosure done) {
    if (current > click_count) {
      std::move(done).Run();
      return;
    }
    MouseDownIn(widget, button, current);
    auto up = base::BindOnce(&Automation::ReleaseButton,
                             weak_factory_.GetWeakPtr(), call->GetWeakPtr(),
                             widget, button, click_count, current, delay,
                             std::move(done));
    if (delay) {
      base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE, std::move(up), base::Milliseconds(delay));
    } else {
      std::move(up).Run();
    }
  }

  void ReleaseButton(base::WeakPtr<Call> call,
                     content::RenderWidgetHost* widget,
                     Button button,
                     int click_count,
                     int current,
                     int delay,
                     base::OnceClosure done) {
    // The widget may have gone with its page since.
    if (!call || !observed_.contains(widget))
      return;
    MouseUpIn(widget, button, current);
    WhenAcked(Type::kMouseUp,
              base::BindOnce(&Automation::Released, weak_factory_.GetWeakPtr(),
                             call, widget, button, click_count, current, delay,
                             std::move(done)));
  }

  void Released(base::WeakPtr<Call> call,
                content::RenderWidgetHost* widget,
                Button button,
                int click_count,
                int current,
                int delay,
                base::OnceClosure done) {
    if (!call || !observed_.contains(widget))
      return;
    if (current < click_count && delay) {
      base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE,
          base::BindOnce(&Automation::PressAgain, weak_factory_.GetWeakPtr(),
                         call, widget, button, click_count, current + 1, delay,
                         std::move(done)),
          base::Milliseconds(delay));
      return;
    }
    PressAndRelease(call.get(), widget, button, click_count, current + 1, delay,
                    std::move(done));
  }

  void PressAgain(base::WeakPtr<Call> call,
                  content::RenderWidgetHost* widget,
                  Button button,
                  int click_count,
                  int current,
                  int delay,
                  base::OnceClosure done) {
    if (call && observed_.contains(widget)) {
      PressAndRelease(call.get(), widget, button, click_count, current, delay,
                      std::move(done));
    }
  }

  content::RenderWidgetHost* MainWidget() {
    content::RenderFrameHost* frame = MainFrame();
    return frame ? frame->GetRenderWidgetHost() : nullptr;
  }

  gfx::PointF ScreenPoint(content::RenderWidgetHost* widget,
                          const gfx::PointF& point) {
    content::RenderWidgetHostView* view = widget ? widget->GetView() : nullptr;
    const gfx::Point origin =
        view ? view->GetViewBounds().origin() : gfx::Point();
    return gfx::PointF(origin.x() + point.x(), origin.y() + point.y());
  }

  // ---- Input held back after a navigation ------------------------------
  //
  // A new top-level widget's input router stays inactive until the widget's
  // first frame is activated (or Chromium's new-content timeout passes): while
  // paint holding shows the old page, RenderInputRouter::FilterInputEvent
  // acknowledges every event at once with kNoConsumerExists, and the page never
  // hears of it. A person cannot act on a page not yet shown; a command can, so
  // commands wait for the same signal. Frames of other sites are never held.
  void WhenInputReady(base::OnceClosure then) {
    content::RenderWidgetHost* widget = MainWidget();
    if (!widget || base::CommandLine::ForCurrentProcess()->HasSwitch(
                       "zebkit-test-no-input-wait")) {
      std::move(then).Run();
      return;
    }
    Observe(widget);
    InputWaiters& waiters = input_waiters_[widget];
    waiters.closures.push_back(std::move(then));
    if (waiters.closures.size() > 1)
      return;
    const int generation = ++input_wait_generation_;
    waiters.generation = generation;
    // A page that never draws (a view hidden for good): after the timeout an
    // event may be dropped, which InputDone() and PointerDone() then report.
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&Automation::InputReady, weak_factory_.GetWeakPtr(),
                       widget, generation),
        base::Seconds(5));
    content::WhenReadyForInput(
        widget, base::BindOnce(&Automation::InputReady,
                               weak_factory_.GetWeakPtr(), widget, generation));
  }

  void InputReady(content::RenderWidgetHost* widget, int generation) {
    auto it = input_waiters_.find(widget);
    if (it == input_waiters_.end() || it->second.generation != generation)
      return;
    std::vector<base::OnceClosure> closures = std::move(it->second.closures);
    input_waiters_.erase(it);
    for (base::OnceClosure& closure : closures)
      std::move(closure).Run();
  }

  // What was sent since the command began and never reached the renderer.
  bool TakeInputDropped() {
    return std::exchange(input_dropped_, false);
  }

  // @call done, or failed if the browser dropped what it sent: never a success
  // for input the page did not get.
  base::OnceClosure InputDone(Call* call) {
    return base::BindOnce(
        [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call) {
          if (!self || !call)
            return;
          if (self->TakeInputDropped())
            call->Fail(kInputDropped);
          else
            call->Done();
        },
        weak_factory_.GetWeakPtr(), call->GetWeakPtr());
  }

  // crInput.ts's RawMouseImpl, as CDP's Input.dispatchMouseEvent makes the
  // event: buttons held, modifiers, a pressure while pressed.
  void SendMouse(content::RenderWidgetHost* widget,
                 Type type,
                 const gfx::PointF& point,
                 Button button,
                 int click_count) {
    if (!widget)
      return;
    blink::WebMouseEvent event(type, Modifiers() | ButtonModifiers(),
                               base::TimeTicks::Now());
    event.button = button;
    event.click_count = click_count;
    event.pointer_type = blink::WebPointerProperties::PointerType::kMouse;
    event.force = buttons_.empty() ? 0 : 0.5;
    event.SetPositionInWidget(point);
    event.SetPositionInScreen(ScreenPoint(widget, point));
    Observe(widget);
    last_sent_[type] = event.TimeStamp();
    widget->ForwardMouseEvent(event);
  }

  // ---- Acknowledgements ------------------------------------------------

  struct Ack {
    Type type;
    base::TimeTicks time;
    base::OnceClosure done;
  };

  // @done once the renderer has handled the last event of @type sent, as
  // CDP's Input.dispatch*Event answers.
  void WhenAcked(Type type, base::OnceClosure done) {
    auto sent = last_sent_.find(type);
    if (sent == last_sent_.end() || acked_[type] >= sent->second) {
      std::move(done).Run();
      return;
    }
    acks_.push_back({type, sent->second, std::move(done)});
  }

  void Observe(content::RenderWidgetHost* widget) {
    if (observed_.insert(widget).second) {
      widget->AddInputEventObserver(this);
      widget->AddObserver(this);
    }
  }

  // content::RenderWidgetHost::InputEventObserver
  void OnInputEventAck(const content::RenderWidgetHost&,
                       blink::mojom::InputEventResultSource source,
                       blink::mojom::InputEventResultState state,
                       const blink::WebInputEvent& event) override {
    const Type type = event.GetType();
    // Filtered in the browser, not by a page without a listener, which the
    // renderer answers (wheel and touch say the same from there).
    if (source == blink::mojom::InputEventResultSource::kBrowser &&
        state == blink::mojom::InputEventResultState::kNoConsumerExists &&
        (blink::WebInputEvent::IsMouseEventType(type) ||
         blink::WebInputEvent::IsKeyboardEventType(type))) {
      input_dropped_ = true;
    }
    // A key down with text is acknowledged as the key down it is.
    if (type == Type::kChar)
      return;
    if (event.TimeStamp() > acked_[type])
      acked_[type] = event.TimeStamp();
    std::vector<base::OnceClosure> ready;
    for (auto it = acks_.begin(); it != acks_.end();) {
      if (it->type == type && it->time <= acked_[type]) {
        ready.push_back(std::move(it->done));
        it = acks_.erase(it);
      } else {
        ++it;
      }
    }
    for (base::OnceClosure& done : ready)
      std::move(done).Run();
  }

  // content::RenderWidgetHostObserver
  void RenderWidgetHostDestroyed(content::RenderWidgetHost* widget) override {
    widget->RemoveInputEventObserver(this);
    widget->RemoveObserver(this);
    observed_.erase(widget);
    if (mouse_widget_.get() == widget)
      mouse_widget_ = nullptr;
    if (auto it = input_waiters_.find(widget); it != input_waiters_.end())
      InputReady(widget, it->second.generation);
    // What it would have acknowledged will not be: the page went.
    std::vector<Ack> acks = std::move(acks_);
    for (Ack& ack : acks)
      std::move(ack.done).Run();
  }

  // ---- Page ------------------------------------------------------------

  // Page.screenshot and ElementHandle.screenshot, from the view's surface:
  // the viewport, "clip" ({x, y, width, height} in CSS pixels), or the
  // element's box; PNG.
  void Screenshot(Call* call) {
    if (call->OnHandle()) {
      StepOnTarget(call, "scrollIntoViewIfNeeded", {},
                   base::BindOnce(
                       [](base::WeakPtr<Automation> self, Call* call,
                          base::Value) {
                         if (self)
                           self->OnTarget(call, &Automation::ScreenshotElement);
                       },
                       weak_factory_.GetWeakPtr()));
      return;
    }
    std::optional<gfx::Rect> clip;
    if (const base::DictValue* given = call->params().FindDict("clip")) {
      clip = gfx::ToEnclosingRect(gfx::RectF(
          given->FindDouble("x").value_or(0),
          given->FindDouble("y").value_or(0),
          given->FindDouble("width").value_or(0),
          given->FindDouble("height").value_or(0)));
    }
    Capture(call, clip);
  }

  void ScreenshotElement(base::WeakPtr<Call> call, Target target) {
    TargetStep(
        call.get(), target, "boundingBox", {},
        base::BindOnce(
            [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call,
               FrameRef frame, base::Value box) {
              if (!self || !call)
                return;
              if (!box.is_dict()) {
                call->Fail("Element is not visible");
                return;
              }
              const gfx::PointF origin(box.GetDict().FindDouble("x").value_or(0),
                                       box.GetDict().FindDouble("y").value_or(0));
              const gfx::SizeF size(box.GetDict().FindDouble("width").value_or(0),
                                    box.GetDict().FindDouble("height").value_or(0));
              self->ToRoot(
                  frame, origin,
                  base::BindOnce(
                      [](base::WeakPtr<Automation> self,
                         base::WeakPtr<Call> call, gfx::SizeF size,
                         content::RenderWidgetHost*, gfx::PointF,
                         gfx::PointF root) {
                        if (self && call) {
                          self->Capture(call.get(), gfx::ToEnclosingRect(
                                                        gfx::RectF(root, size)));
                        }
                      },
                      self, call, size));
            },
            weak_factory_.GetWeakPtr(), call, target.frame));
  }

  void Capture(Call* call, std::optional<gfx::Rect> clip) {
    content::RenderWidgetHost* widget = MainWidget();
    content::RenderWidgetHostView* view = widget ? widget->GetView() : nullptr;
    if (!view) {
      call->Fail(kClosed);
      return;
    }
    gfx::Rect source;
    if (clip) {
      source = *clip;
      source.Intersect(gfx::Rect(view->GetViewBounds().size()));
      if (source.IsEmpty()) {
        call->Fail(
            "Clipped area is either empty or outside the resulting image");
        return;
      }
    }
    view->CopyFromSurface(
        source, gfx::Size(), base::Seconds(10),
        base::BindOnce(&Automation::Captured, call->GetWeakPtr()));
  }

  static void Captured(base::WeakPtr<Call> call,
                       const content::CopyFromSurfaceResult& result) {
    if (!call)
      return;
    if (!result.has_value() || result->bitmap.drawsNothing()) {
      call->Fail("zebkit: the page has no picture yet");
      return;
    }
    std::optional<std::vector<uint8_t>> png =
        gfx::PNGCodec::EncodeBGRASkBitmap(result->bitmap, false);
    if (!png) {
      call->Fail("zebkit: the picture could not be encoded");
      return;
    }
    const std::string path = call->String("path");
    if (path.empty()) {
      base::DictValue answer;
      answer.Set("binary", base::Base64Encode(*png));
      call->Done(base::Value(std::move(answer)));
      return;
    }
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock()},
        base::BindOnce(
            [](std::string path, std::vector<uint8_t> png) {
              return base::WriteFile(base::FilePath::FromUTF8Unsafe(path), png);
            },
            path, std::move(*png)),
        base::BindOnce(
            [](base::WeakPtr<Call> call, std::string path, bool written) {
              if (!call)
                return;
              if (!written) {
                call->Fail("zebkit: cannot write " + path);
                return;
              }
              base::DictValue answer;
              answer.Set("path", path);
              call->Done(base::Value(std::move(answer)));
            },
            call, path));
  }

  void SetViewportSize(Call* call) {
    const base::DictValue* size = call->params().FindDict("viewportSize");
    const int width = size ? size->FindInt("width").value_or(0) : 0;
    const int height = size ? size->FindInt("height").value_or(0) : 0;
    if (width <= 0 || height <= 0) {
      call->Fail("viewportSize: expected width and height");
      return;
    }
    // The off-screen view's own size: a resize behind its back lasts only
    // until its next frame.
    zebkit_view_set_size(call->view(), width, height, 0);
    WaitForViewport(call->GetWeakPtr(), width, height);
  }

  // Until the page has the size: resizing is asynchronous.
  void WaitForViewport(base::WeakPtr<Call> call, int width, int height) {
    if (!call)
      return;
    RunStep(call.get(), Follow(MainFrame()), AutomationWorld::kUtility,
            "viewport", {}, {},
            base::BindOnce(
                [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call,
                   int width, int height, base::Value value) {
                  if (!self || !call)
                    return;
                  if (value.is_dict() &&
                      value.GetDict().FindInt("width") == width &&
                      value.GetDict().FindInt("height") == height) {
                    call->Done();
                    return;
                  }
                  base::SequencedTaskRunner::GetCurrentDefault()
                      ->PostDelayedTask(
                          FROM_HERE,
                          base::BindOnce(&Automation::WaitForViewport, self,
                                         call, width, height),
                          base::Milliseconds(16));
                },
                weak_factory_.GetWeakPtr(), call, width, height));
  }

  // Page.addInitScript: in every document from now on, before the page's
  // own.
  void AddInitScript(Call* call) {
    const std::string id = RegistrationId(call->params());
    call->view()->AddScript(call->String("source"), base::BindOnce(
        [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call,
           std::string id, const base::DictValue* result, const std::string& error) {
          if (!self || !call)
            return;
          const std::string* native_id = result ? result->FindString("identifier") : nullptr;
          if (!error.empty() || !native_id) {
            call->Fail(error.empty() ? "Script registration failed" : error);
            return;
          }
          self->init_scripts_[id] = *native_id;
          call->Done(RegistrationResult(id, "script"));
        }, weak_factory_.GetWeakPtr(), call->GetWeakPtr(), id));
  }

  void RemoveRegistration(Call* call) {
    const std::string id = call->String("registration");
    if (auto binding = binding_registrations_.find(id); binding != binding_registrations_.end()) {
      std::erase(exposed_, binding->second);
      binding_registrations_.erase(binding);
      MainFrame()->ForEachRenderFrameHost([&](content::RenderFrameHost* frame) {
        if (frame->IsRenderFrameLive())
          Agent(frame)->Expose(exposed_);
      });
    }
    auto script = init_scripts_.find(id);
    if (script == init_scripts_.end()) {
      call->Done();
      return;
    }
    base::DictValue params;
    params.Set("identifier", script->second);
    init_scripts_.erase(script);
    call->view()->Send("Page.removeScriptToEvaluateOnNewDocument", std::move(params),
        base::BindOnce([](base::WeakPtr<Call> call, const base::DictValue*, const std::string& error) {
          if (!call) return;
          if (error.empty()) call->Done(); else call->Fail(error);
        }, call->GetWeakPtr()));
  }

  // Page.pdf, as Page.printToPDF prints: "pdf" in base64.
  void Pdf(Call* call) {
    base::DictValue options = call->params().Clone();
    options.Remove("timeout");
    PrintToPdf(web_contents(), Json(base::Value(std::move(options))),
               base::BindOnce(
                   [](base::WeakPtr<Call> call,
                      scoped_refptr<base::RefCountedMemory> pdf,
                      const std::string& error, bool) {
                     if (!call)
                       return;
                     if (!pdf) {
                       call->Fail(error);
                       return;
                     }
                     base::DictValue answer;
                     answer.Set("pdf", base::Base64Encode(std::string_view(
                         reinterpret_cast<const char*>(pdf->front()), pdf->size())));
                     call->Done(base::Value(std::move(answer)));
                   },
                   call->GetWeakPtr()));
  }

  void Close(Call* call) {
    zebkit_view* view = call->view();
    call->Done();
    zebkit_view_close(view);
  }

  void BringToFront(Call* call) {
    zebkit_view_focus(call->view());
    call->Done();
  }

  // ---- Network (automation_network.cc) ---------------------------------

  Network* NetworkOf(Call* call) {
    Network* network = Network::For(web_contents());
    if (!network)
      call->Fail(kClosed);
    return network;
  }

  // Page.setNetworkInterceptionPatterns, BrowserContext's: the requests
  // whose URL matches are held as routes (BrowserContext.route events).
  void SetInterceptionPatterns(Call* call) {
    Network* network = NetworkOf(call);
    if (!network)
      return;
    std::string error;
    const base::ListValue* patterns = call->params().FindList("patterns");
    if (!network->SetPatterns(patterns ? patterns->Clone() : base::ListValue(), &error)) {
      call->Fail(error);
      return;
    }
    call->Done();
  }

  void SetExtraHttpHeaders(Call* call) {
    Network* network = NetworkOf(call);
    if (!network)
      return;
    const base::ListValue* headers = call->params().FindList("headers");
    network->SetExtraHeaders(headers ? headers->Clone() : base::ListValue());
    call->Done();
  }

  void RouteContinue(Call* call) {
    Network* network = NetworkOf(call);
    if (network && !network->Continue(call->String("route"), call->params()))
      call->Fail("Route is already handled!");
    else if (network)
      call->Done();
  }

  void RouteFulfill(Call* call) {
    Network* network = NetworkOf(call);
    if (network && !network->Fulfill(call->String("route"), call->params()))
      call->Fail("Route is already handled!");
    else if (network)
      call->Done();
  }

  void RouteAbort(Call* call) {
    Network* network = NetworkOf(call);
    if (network && !network->Abort(call->String("route"), call->String("errorCode")))
      call->Fail("Route is already handled!");
    else if (network)
      call->Done();
  }

  // Response.body: {binary} in base64.
  void ResponseBody(Call* call) {
    Network* network = NetworkOf(call);
    if (!network)
      return;
    std::optional<std::string> body = network->Body(call->String("request"));
    if (!body) {
      call->Fail("Response body is unavailable for redirect responses, or it has not arrived yet");
      return;
    }
    base::DictValue answer;
    answer.Set("binary", base::Base64Encode(*body));
    call->Done(base::Value(std::move(answer)));
  }

  void SetOffline(Call* call) {
    if (Network* network = NetworkOf(call)) {
      network->SetOffline(call->Flag("offline"));
      call->Done();
    }
  }

  // ---- Bindings (Page.exposeBinding) -----------------------------------

  // Page.exposeBinding, BrowserContext's: window[name](...args) in every
  // frame's documents from now on, its calls BrowserContext.bindingCall
  // events, answered with BindingCall.resolve or reject.
  void ExposeBinding(Call* call) {
    const std::string name = call->String("name");
    if (name.empty()) {
      call->Fail("name: expected a string");
      return;
    }
    if (std::find(exposed_.begin(), exposed_.end(), name) != exposed_.end()) {
      call->Fail("Function \"" + name + "\" has been already registered");
      return;
    }
    const std::string id = RegistrationId(call->params());
    binding_registrations_[id] = name;
    exposed_.push_back(name);
    MainFrame()->ForEachRenderFrameHost([&](content::RenderFrameHost* frame) {
      if (frame->IsRenderFrameLive())
        Agent(frame)->Expose(exposed_);
    });
    call->Done(RegistrationResult(id, "binding"));
  }

  void BindHost(content::RenderFrameHost* frame,
                mojo::PendingAssociatedReceiver<mojom::AutomationHost> receiver) {
    hosts_.Add(this, std::move(receiver), frame->GetGlobalId());
  }

  // mojom::AutomationHost
  void BindingCalled(const std::string& name,
                     const std::string& args,
                     BindingCalledCallback callback) override {
    content::RenderFrameHost* frame =
        content::RenderFrameHost::FromID(hosts_.current_context());
    if (name == kWebSocketBinding) {
      std::optional<base::Value> parsed = Parse(args);
      WebSocketReport(frame, parsed ? std::move(*parsed) : base::Value());
      std::move(callback).Run(true, "null");
      return;
    }
    const std::string id = "b" + base::NumberToString(++next_binding_);
    bindings_[id] = std::move(callback);
    base::DictValue params;
    params.Set("binding", id);
    params.Set("name", name);
    std::optional<base::Value> parsed = Parse(args);
    params.Set("args", parsed ? std::move(*parsed) : base::Value(base::ListValue()));
    if (frame)
      params.Set("frame", FrameId(frame));
    Emit("BrowserContext.bindingCall", std::move(params));
  }

  // ---- WebSocket routes (Page.setWebSocketInterceptionPatterns) ---------

  // webSocketRouteDispatcher.ts's install(): Playwright's WebSocket mock in
  // every document from now on and in those there are, reporting through
  // a binding of the engine's own; a WebSocket whose URL matches is
  // WebSocketRoute's (Page.webSocketRoute), others go to their server.
  void SetWebSocketPatterns(Call* call) {
    Network* network = NetworkOf(call);
    if (!network)
      return;
    std::string error;
    const base::ListValue* patterns = call->params().FindList("patterns");
    if (!network->SetWebSocketPatterns(
            patterns ? patterns->Clone() : base::ListValue(), &error)) {
      call->Fail(error);
      return;
    }
    if (web_socket_mock_) {
      call->Done();
      return;
    }
    web_socket_mock_ = true;
    exposed_.push_back(kWebSocketBinding);
    MainFrame()->ForEachRenderFrameHost([&](content::RenderFrameHost* frame) {
      if (frame->IsRenderFrameLive())
        Agent(frame)->Expose(exposed_);
    });
    const std::string mock(reinterpret_cast<const char*>(kWebSocketMockScript),
                           sizeof(kWebSocketMockScript));
    zebkit_view_add_script(call->view(), mock.c_str());
    EvaluateInAllFrames(call, mock);
  }

  // The mock's reports: a WebSocket made, its messages and closes.
  void WebSocketReport(content::RenderFrameHost* frame, const base::Value& args) {
    const base::DictValue* payload =
        args.is_list() && !args.GetList().empty() && args.GetList()[0].is_dict()
            ? &args.GetList()[0].GetDict()
            : nullptr;
    if (!payload || !frame)
      return;
    const std::string* type = payload->FindString("type");
    const std::string* id = payload->FindString("id");
    if (!type || !id)
      return;
    if (*type == "onCreate") {
      const std::string* url = payload->FindString("url");
      Network* network = Network::For(web_contents());
      if (url && network && network->RoutesWebSocket(*url)) {
        web_sockets_[*id] = frame->GetGlobalId();
        base::DictValue params;
        params.Set("webSocketRoute", *id);
        params.Set("url", *url);
        if (const base::ListValue* protocols = payload->FindList("protocols"))
          params.Set("protocols", protocols->Clone());
        params.Set("frame", FrameId(frame));
        Emit("Page.webSocketRoute", std::move(params));
      } else {
        DispatchToWebSocket(frame, "{\"type\":\"passthrough\",\"id\":" +
                                       Json(base::Value(*id)) + "}");
      }
      return;
    }
    static const std::map<std::string, std::string> kEvents = {
        {"onMessageFromPage", "WebSocketRoute.messageFromPage"},
        {"onMessageFromServer", "WebSocketRoute.messageFromServer"},
        {"onClosePage", "WebSocketRoute.closePage"},
        {"onCloseServer", "WebSocketRoute.closeServer"},
    };
    auto event = kEvents.find(*type);
    if (event == kEvents.end())
      return;
    base::DictValue params;
    params.Set("webSocketRoute", *id);
    if (const base::DictValue* data = payload->FindDict("data")) {
      if (const std::string* message = data->FindString("data"))
        params.Set("message", *message);
      params.Set("isBase64", data->FindBool("isBase64").value_or(false));
    }
    if (std::optional<int> code = payload->FindInt("code"))
      params.Set("code", *code);
    if (const std::string* reason = payload->FindString("reason"))
      params.Set("reason", *reason);
    if (std::optional<bool> clean = payload->FindBool("wasClean"))
      params.Set("wasClean", *clean);
    Emit(event->second, std::move(params));
  }

  void DispatchToWebSocket(content::RenderFrameHost* frame,
                           const std::string& request) {
    Agent(frame)->Evaluate(
        AutomationWorld::kMain,
        "globalThis.__pwWebSocketDispatch(" + request + ")",
        AutomationExpression::kValue, "[]", {}, true, true, base::DoNothing());
  }

  // WebSocketRoute.connect, ensureOpened, sendToPage, sendToServer,
  // closePage, closeServer: the mock's requests, in the WebSocket's frame.
  void WebSocketCommand(Call* call) {
    const std::string id = call->String("webSocketRoute");
    auto found = web_sockets_.find(id);
    content::RenderFrameHost* frame =
        found == web_sockets_.end()
            ? nullptr
            : content::RenderFrameHost::FromID(found->second);
    if (!frame) {
      call->Done();
      return;
    }
    base::DictValue request;
    request.Set("type", call->Name());
    request.Set("id", id);
    if (call->Has("message")) {
      base::DictValue data;
      data.Set("data", call->String("message"));
      data.Set("isBase64", call->Flag("isBase64"));
      request.Set("data", std::move(data));
    }
    if (call->Has("code"))
      request.Set("code", static_cast<int>(call->Number("code")));
    if (call->Has("reason"))
      request.Set("reason", call->String("reason"));
    if (call->Has("wasClean"))
      request.Set("wasClean", call->Flag("wasClean"));
    Agent(frame)->Evaluate(
        AutomationWorld::kMain,
        "globalThis.__pwWebSocketDispatch(" + Json(base::Value(std::move(request))) + ")",
        AutomationExpression::kValue, "[]", {}, true, true,
        AnswerTo(call, frame->GetGlobalId(), Then(call, AsDone())));
  }

  // ---- Workers --------------------------------------------------------

  // A worker of the page's (automation_renderer.cc): Page.worker, once
  // per page, with its id ("k<renderer>.<n>").
  void WorkerStarted(int32_t worker, const std::string& url) override {
    content::RenderFrameHost* frame =
        content::RenderFrameHost::FromID(hosts_.current_context());
    if (!frame)
      return;
    const std::string id = "k" +
                           base::NumberToString(frame->GetProcess()->GetDeprecatedID()) +
                           "." + base::NumberToString(worker);
    if (workers_.contains(id))
      return;
    workers_[id] = {frame->GetGlobalId(), worker, url};
    base::DictValue params;
    params.Set("worker", id);
    params.Set("url", url);
    params.Set("frame", FrameId(frame));
    Emit("Page.worker", std::move(params));
  }

  void WorkerEnded(int32_t worker) override {
    content::RenderFrameHost* frame =
        content::RenderFrameHost::FromID(hosts_.current_context());
    if (!frame)
      return;
    const std::string id = "k" +
                           base::NumberToString(frame->GetProcess()->GetDeprecatedID()) +
                           "." + base::NumberToString(worker);
    if (!workers_.erase(id))
      return;
    base::DictValue params;
    params.Set("worker", id);
    Emit("Worker.close", std::move(params));
  }

  // Page.workers (zebkit's: Playwright's client keeps them from events).
  void Workers(Call* call) {
    base::ListValue list;
    for (const auto& [id, worker] : workers_) {
      base::DictValue entry;
      entry.Set("worker", id);
      entry.Set("url", worker.url);
      list.Append(std::move(entry));
    }
    base::DictValue answer;
    answer.Set("workers", std::move(list));
    call->Done(base::Value(std::move(answer)));
  }

  // Worker.evaluateExpression: in the worker's thread, its value as JSON.
  void WorkerEvaluate(Call* call) {
    auto found = workers_.find(call->String("worker"));
    content::RenderFrameHost* frame =
        found == workers_.end() ? nullptr
                                : content::RenderFrameHost::FromID(found->second.frame);
    if (!frame) {
      call->Fail(kClosed);
      return;
    }
    base::ListValue args;
    if (const base::Value* arg = call->params().Find("arg"))
      args.Append(arg->Clone());
    Agent(frame)->WorkerEvaluate(
        found->second.number, call->String("expression"), KindOf(call),
        Json(base::Value(std::move(args))),
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(
            base::BindOnce(
                [](base::WeakPtr<Call> call, AutomationStatus status,
                   const std::string& result) {
                  if (!call)
                    return;
                  if (status == AutomationStatus::kGone) {
                    call->Fail(kClosed);
                    return;
                  }
                  std::optional<base::Value> answer = Parse(result);
                  if (status != AutomationStatus::kOk || !answer ||
                      !answer->is_dict()) {
                    call->Fail(result);
                    return;
                  }
                  if (!answer->GetDict().FindBool("ok").value_or(false)) {
                    const std::string* error = answer->GetDict().FindString("error");
                    call->Fail(error ? *error : "Error");
                    return;
                  }
                  base::Value* value = answer->GetDict().Find("value");
                  call->Answer(value ? std::move(*value) : base::Value());
                },
                call->GetWeakPtr()),
            AutomationStatus::kGone, std::string()));
  }

  void BindingResolve(Call* call) { AnswerBinding(call, true); }
  void BindingReject(Call* call) { AnswerBinding(call, false); }
  void AnswerBinding(Call* call, bool ok) {
    auto found = bindings_.find(call->String("binding"));
    if (found == bindings_.end()) {
      call->Fail("The binding call is already answered");
      return;
    }
    BindingCalledCallback callback = std::move(found->second);
    bindings_.erase(found);
    if (ok) {
      const base::Value* result = call->params().Find("result");
      std::move(callback).Run(true, result ? Json(*result) : "null");
    } else {
      std::move(callback).Run(false, call->String("error"));
    }
    call->Done();
  }

  // ---- Tracing, HAR, video (automation_trace.cc) ------------------------

  HarRecorder::BodyFunc Bodies() {
    return base::BindRepeating(
        [](base::WeakPtr<content::WebContents> contents,
           const std::string& request) -> std::optional<std::string> {
          Network* network = contents ? Network::For(contents.get()) : nullptr;
          return network ? network->Body(request) : std::nullopt;
        },
        web_contents()->GetWeakPtr());
  }

  static void Written(base::WeakPtr<Call> call,
                      const base::FilePath& path,
                      const std::string& error) {
    if (!call)
      return;
    if (!error.empty()) {
      call->Fail(error);
      return;
    }
    base::DictValue answer;
    answer.Set("path", path.AsUTF8Unsafe());
    call->Done(base::Value(std::move(answer)));
  }

  // Tracing.tracingStart: {screenshots, snapshots, title}; the commands,
  // console and network of the page from now on, and a picture after each
  // command with screenshots.
  void TracingStart(Call* call) {
    if (tracing_) {
      call->Fail("Tracing has been already started");
      return;
    }
    tracing_ = std::make_unique<TraceRecorder>(Registered_IdOf(), call->params());
    call->Done();
  }

  void TracingStartChunk(Call* call) {
    if (!tracing_) {
      call->Fail("Must start tracing before starting a new chunk");
      return;
    }
    tracing_->StartChunk(call->String("title"));
    call->Done();
  }

  // Tracing.tracingStopChunk: the chunk as Playwright's trace zip ({path},
  // at "path" when given), or discarded ({mode: "discard"}).
  void TracingStopChunk(Call* call) {
    if (!tracing_) {
      call->Fail("Must start tracing before stopping");
      return;
    }
    if (call->String("mode") == "discard") {
      tracing_->StartChunk(std::string());
      call->Done();
      return;
    }
    tracing_->Export(base::FilePath::FromUTF8Unsafe(call->String("path")),
                     Bodies(),
                     base::BindOnce(&Automation::Written, call->GetWeakPtr()));
  }

  void TracingStop(Call* call) {
    tracing_.reset();
    traced_.clear();
    call->Done();
  }

  void TracingGroup(Call* call) { call->Done(); }

  // A JPEG of the page for the trace's screencast.
  void TraceScreenshot() {
    content::RenderWidgetHostView* view =
        MainWidget() ? MainWidget()->GetView() : nullptr;
    if (!view)
      return;
    view->CopyFromSurface(
        gfx::Rect(), gfx::Size(), base::Seconds(2),
        base::BindOnce(
            [](base::WeakPtr<Automation> self,
               const content::CopyFromSurfaceResult& result) {
              if (!self || !self->tracing_ || !result.has_value())
                return;
              const SkBitmap& bitmap = result->bitmap;
              std::optional<std::vector<uint8_t>> jpeg =
                  gfx::JPEGCodec::Encode(bitmap, 80);
              if (jpeg)
                self->tracing_->Screencast(std::move(*jpeg), bitmap.width(),
                                           bitmap.height());
            },
            weak_factory_.GetWeakPtr()));
  }

  // Tracing.harStart: {options: {content, mode}}; {harId}.
  void HarStart(Call* call) {
    const base::DictValue* options = call->params().FindDict("options");
    const std::string id = "har@" + base::NumberToString(++next_har_);
    hars_[id] = std::make_unique<HarRecorder>(options ? options->Clone() : base::DictValue());
    base::DictValue answer;
    answer.Set("harId", id);
    call->Done(base::Value(std::move(answer)));
  }

  // Tracing.harExport: the HAR so far to {path}, and the recording ended.
  void HarExport(Call* call) {
    auto found = hars_.find(call->String("harId"));
    if (found == hars_.end()) {
      call->Fail("No HAR recording " + call->String("harId"));
      return;
    }
    std::unique_ptr<HarRecorder> har = std::move(found->second);
    hars_.erase(found);
    har->Export(base::FilePath::FromUTF8Unsafe(call->String("path")), Bodies(),
                std::string(version_info::GetVersionNumber()),
                base::BindOnce(&Automation::Written, call->GetWeakPtr()));
  }

  // Page.startVideo (zebkit's; Playwright's recordVideo of a context):
  // {path, size: {width, height}}, the page's frames as WebM.
  void StartVideo(Call* call) {
    if (video_) {
      call->Fail("The page is already recorded");
      return;
    }
    const base::DictValue* size = call->params().FindDict("size");
    content::RenderWidgetHostView* view = web_contents()->GetRenderWidgetHostView();
    const gfx::Size viewport = view ? view->GetViewBounds().size() : gfx::Size(800, 600);
    const int width = size ? size->FindInt("width").value_or(viewport.width())
                           : std::min(viewport.width(), 800);
    const int height = size ? size->FindInt("height").value_or(viewport.height())
                            : viewport.height() * std::min(viewport.width(), 800) /
                                  std::max(viewport.width(), 1);
    video_ = std::make_unique<VideoRecorder>(
        web_contents(), base::FilePath::FromUTF8Unsafe(call->String("path")),
        width, height);
    base::DictValue answer;
    answer.Set("path", video_->path().AsUTF8Unsafe());
    call->Done(base::Value(std::move(answer)));
  }

  // Page.stopVideo: the file finished, {path}.
  void StopVideo(Call* call) {
    if (!video_) {
      call->Fail("The page is not recorded");
      return;
    }
    std::unique_ptr<VideoRecorder> video = std::move(video_);
    VideoRecorder* raw = video.get();
    raw->Stop(base::BindOnce(
        [](std::unique_ptr<VideoRecorder> video, base::WeakPtr<Call> call,
           const base::FilePath& path, const std::string& error) {
          Written(call, path, error);
        },
        std::move(video), call->GetWeakPtr()));
  }

  std::string Registered_IdOf();

  // ---- Clock (BrowserContext.clock*) -----------------------------------

  // server/clock.ts: Playwright's clock in every document from now on and
  // in those there are, each command logged for documents to come and run
  // in every frame.
  void Clock(Call* call, const std::string& method, bool ticks) {
    std::string value;
    if (method != "resume") {
      const char* number = ticks ? "ticksNumber" : "timeNumber";
      const char* text = ticks ? "ticksString" : "timeString";
      if (call->Has(number)) {
        value = base::NumberToString(call->Number(number));
      } else if (call->Has(text)) {
        value = ticks ? Ticks(call->String(text)) : TimeOf(call->String(text));
        if (value.empty()) {
          call->Fail(ticks ? "Clock only understands numbers, 'mm:ss' and 'hh:mm:ss'"
                           : "Invalid date: " + call->String(text));
          return;
        }
      } else if (method == "install") {
        value = "Date.now()";
      } else {
        call->Fail("time or ticks: expected a number or a string");
        return;
      }
    }
    std::string scripts;
    if (!clock_installed_) {
      clock_installed_ = true;
      scripts = std::string(reinterpret_cast<const char*>(kClockScript),
                            sizeof(kClockScript));
      zebkit_view_add_script(call->view(), scripts.c_str());
    }
    const std::string now = base::NumberToString(
        base::Time::Now().InMillisecondsFSinceUnixEpoch());
    const std::string log = "globalThis.__pwClock.controller.log('" + method +
                            "', " + now + (value.empty() ? "" : ", " + value) +
                            ")";
    zebkit_view_add_script(call->view(), log.c_str());
    scripts += "\nglobalThis.__pwClock.controller." + method + "(" + value + ");";
    EvaluateInAllFrames(call, scripts);
  }

  // server/clock.ts's parseTicks(): 1000, "01:10:00".
  static std::string Ticks(const std::string& text) {
    int64_t ms = 0;
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t at; (at = text.find(':', start)) != std::string::npos; start = at + 1)
      parts.push_back(text.substr(start, at - start));
    parts.push_back(text.substr(start));
    if (parts.size() > 3)
      return std::string();
    for (const std::string& part : parts) {
      int value = 0;
      if (part.empty() || part.size() > 2 || !base::StringToInt(part, &value) ||
          value >= 60) {
        return std::string();
      }
      ms = ms * 60 + value;
    }
    return base::NumberToString(ms * 1000);
  }

  // A date's milliseconds, read by the page as Date.parse() reads it.
  static std::string TimeOf(const std::string& text) {
    return "Date.parse(" + Json(base::Value(text)) + ")";
  }

  void EvaluateInAllFrames(Call* call, const std::string& script) {
    std::vector<content::RenderFrameHost*> frames;
    MainFrame()->ForEachRenderFrameHost([&](content::RenderFrameHost* frame) {
      if (frame->IsRenderFrameLive())
        frames.push_back(frame);
    });
    auto left = std::make_shared<size_t>(frames.size());
    auto error = std::make_shared<std::string>();
    if (frames.empty()) {
      call->Done();
      return;
    }
    for (content::RenderFrameHost* frame : frames) {
      Agent(frame)->Evaluate(
          AutomationWorld::kMain, script, AutomationExpression::kValue, "[]", {},
          true, true,
          mojo::WrapCallbackWithDefaultInvokeIfNotRun(
              base::BindOnce(
                  [](base::WeakPtr<Call> call, std::shared_ptr<size_t> left,
                     std::shared_ptr<std::string> error, AutomationStatus status,
                     const std::string& result, std::vector<AutomationHandlePtr>) {
                    std::optional<base::Value> answer = Parse(result);
                    if (status == AutomationStatus::kOk && answer &&
                        answer->is_dict() &&
                        !answer->GetDict().FindBool("ok").value_or(false) &&
                        error->empty()) {
                      if (const std::string* message =
                              answer->GetDict().FindString("error")) {
                        *error = *message;
                      }
                    }
                    if (--*left || !call)
                      return;
                    if (error->empty())
                      call->Done();
                    else
                      call->Fail(*error);
                  },
                  call->GetWeakPtr(), left, error),
              AutomationStatus::kGone, std::string(),
              std::vector<AutomationHandlePtr>()));
    }
  }

  void ClockInstall(Call* call) { Clock(call, "install", false); }
  void ClockFastForward(Call* call) { Clock(call, "fastForward", true); }
  void ClockPauseAt(Call* call) { Clock(call, "pauseAt", false); }
  void ClockResume(Call* call) { Clock(call, "resume", false); }
  void ClockRunFor(Call* call) { Clock(call, "runFor", true); }
  void ClockSetFixedTime(Call* call) { Clock(call, "setFixedTime", false); }
  void ClockSetSystemTime(Call* call) { Clock(call, "setSystemTime", false); }

  void Download(Call* call);

  void GrantPermissions(Call* call);
  void ClearPermissions(Call* call);

  // ---- The profile (automation_context.cc) -----------------------------

  using ContextCommand = void (*)(content::WebContents*, const base::DictValue&,
                                  ContextDone);
  void InContext(Call* call, ContextCommand command) {
    command(web_contents(), call->params(),
            base::BindOnce(
                [](base::WeakPtr<Call> call, base::Value result,
                   const std::string& error) {
                  if (!call)
                    return;
                  if (!error.empty())
                    call->Fail(error);
                  else
                    call->Done(std::move(result));
                },
                call->GetWeakPtr()));
  }
  using ProfileCommand = void (*)(content::BrowserContext*,
                                  const base::DictValue&, ContextDone);
  void InProfile(Call* call, ProfileCommand command) {
    command(web_contents()->GetBrowserContext(), call->params(),
            base::BindOnce(
                [](base::WeakPtr<Call> call, base::Value result,
                   const std::string& error) {
                  if (!call)
                    return;
                  if (!error.empty())
                    call->Fail(error);
                  else
                    call->Done(std::move(result));
                },
                call->GetWeakPtr()));
  }
  void Cookies(Call* call) { InProfile(call, &ContextCookies); }

  // BrowserContext.storageState: the profile's cookies, and the local
  // storage of the origins the page's frames show ({cookies, origins:
  // [{origin, localStorage: [{name, value}]}]}).
  void StorageState(Call* call) {
    ContextCookies(
        web_contents()->GetBrowserContext(), base::DictValue(),
        base::BindOnce(&Automation::StorageOrigins, weak_factory_.GetWeakPtr(),
                       call->GetWeakPtr()));
  }

  void StorageOrigins(base::WeakPtr<Call> call,
                      base::Value cookies,
                      const std::string& error) {
    if (!call)
      return;
    if (!error.empty()) {
      call->Fail(error);
      return;
    }
    std::vector<content::RenderFrameHost*> frames;
    MainFrame()->ForEachRenderFrameHost([&](content::RenderFrameHost* frame) {
      if (frame->IsRenderFrameLive() &&
          !frame->GetLastCommittedOrigin().opaque()) {
        frames.push_back(frame);
      }
    });
    struct State {
      base::DictValue answer;
      std::map<std::string, base::Value> origins;
      size_t left;
    };
    auto state = std::make_shared<State>();
    if (cookies.is_dict()) {
      if (base::Value* list = cookies.GetDict().Find("cookies"))
        state->answer.Set("cookies", std::move(*list));
    }
    state->left = frames.size();
    auto finish = [](base::WeakPtr<Call> call, std::shared_ptr<State> state) {
      if (!call)
        return;
      base::ListValue origins;
      for (auto& [origin, value] : state->origins)
        origins.Append(std::move(value));
      state->answer.Set("origins", std::move(origins));
      call->Done(base::Value(std::move(state->answer)));
    };
    if (frames.empty()) {
      finish(call, state);
      return;
    }
    for (content::RenderFrameHost* frame : frames) {
      Agent(frame)->Evaluate(
          AutomationWorld::kUtility,
          "(() => ({ origin: location.origin, localStorage: Object.keys(localStorage)"
          ".map(name => ({ name, value: localStorage.getItem(name) })) }))()",
          AutomationExpression::kValue, "[]", {}, true, true,
          mojo::WrapCallbackWithDefaultInvokeIfNotRun(
              base::BindOnce(
                  [](base::WeakPtr<Call> call, std::shared_ptr<State> state,
                     decltype(finish) finish, AutomationStatus status,
                     const std::string& result,
                     std::vector<AutomationHandlePtr>) {
                    std::optional<base::Value> answer = Parse(result);
                    base::Value* value =
                        answer && answer->is_dict() ? answer->GetDict().Find("value")
                                                    : nullptr;
                    if (value && value->is_dict()) {
                      const std::string* origin = value->GetDict().FindString("origin");
                      const base::ListValue* items =
                          value->GetDict().FindList("localStorage");
                      if (origin && items && !items->empty())
                        state->origins[*origin] = value->Clone();
                    }
                    if (--state->left == 0)
                      finish(call, state);
                  },
                  call, state, finish),
              AutomationStatus::kGone, std::string(),
              std::vector<AutomationHandlePtr>()));
    }
  }

  // BrowserContext.setStorageState: its cookies set; the local storage of
  // its origins given to the frames that show them now.
  void SetStorageState(Call* call) {
    const base::DictValue* state = call->params().FindDict("storageState");
    if (!state)
      state = &call->params();
    base::DictValue cookies;
    if (const base::ListValue* list = state->FindList("cookies"))
      cookies.Set("cookies", list->Clone());
    else
      cookies.Set("cookies", base::ListValue());
    std::string script = "(() => { const origins = ";
    const base::ListValue* origins = state->FindList("origins");
    script += origins ? Json(base::Value(origins->Clone())) : "[]";
    script +=
        "; for (const o of origins) if (o.origin === location.origin) for (const i "
        "of o.localStorage || []) localStorage.setItem(i.name, i.value); })()";
    ContextAddCookies(
        web_contents()->GetBrowserContext(), cookies,
        base::BindOnce(
            [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call,
               std::string script, base::Value, const std::string& error) {
              if (!self || !call)
                return;
              if (!error.empty()) {
                call->Fail(error);
                return;
              }
              self->EvaluateInAllFrames(call.get(), script);
            },
            weak_factory_.GetWeakPtr(), call->GetWeakPtr(), script));
  }
  void AddCookies(Call* call) { InProfile(call, &ContextAddCookies); }
  void ClearCookies(Call* call) { InProfile(call, &ContextClearCookies); }
  void Fetch(Call* call) { InProfile(call, &ContextFetch); }

  // Page.emulateMedia: the media features in the page's preferences, the
  // media type in each frame's renderer (patches/1039).
  void EmulateMedia(Call* call) {
    if (call->Has("media")) {
      std::string media = call->String("media");
      if (media == "no-override" || media == "null")
        media.clear();
      media_type_ = media;
      ApplyMediaType();
    }
    InContext(call, &ContextEmulateMedia);
  }

  void ApplyMediaType() {
    MainFrame()->ForEachRenderFrameHost([&](content::RenderFrameHost* frame) {
      if (frame->IsRenderFrameLive())
        Agent(frame)->SetMediaType(media_type_);
    });
  }

  // ---- Agents ----------------------------------------------------------

  mojom::AutomationAgent* Agent(content::RenderFrameHost* frame) {
    mojo::AssociatedRemote<mojom::AutomationAgent>& agent =
        agents_[frame->GetGlobalId()];
    if (!agent.is_bound() || !agent.is_connected()) {
      agent.reset();
      frame->GetRemoteAssociatedInterfaces()->GetInterface(&agent);
    }
    return agent.get();
  }

 private:
  explicit Automation(content::WebContents* contents)
      : content::WebContentsObserver(contents),
        content::WebContentsUserData<Automation>(*contents) {}
  friend class content::WebContentsUserData<Automation>;
  WEB_CONTENTS_USER_DATA_KEY_DECL();

  struct FileChooser {
    scoped_refptr<content::FileSelectListener> listener;
    blink::mojom::FileChooserParams::Mode mode;
  };

  raw_ptr<zebkit_view> view_ = nullptr;
  raw_ptr<content::RenderWidgetHost, DisableDanglingPtrDetection>
      focused_widget_ = nullptr;
  std::vector<base::WeakPtr<Call>> calls_;
  std::map<content::GlobalRenderFrameHostId,
           mojo::AssociatedRemote<mojom::AutomationAgent>>
      agents_;
  std::map<std::string, HandleRef> handles_;
  int next_handle_ = 0;
  std::vector<std::string> exposed_;
  std::map<std::string, std::string> init_scripts_;
  std::map<std::string, std::string> binding_registrations_;
  bool clock_installed_ = false;
  std::unique_ptr<TraceRecorder> tracing_;
  std::map<Call*, std::string> traced_;
  std::map<std::string, std::unique_ptr<HarRecorder>> hars_;
  int next_har_ = 0;
  std::unique_ptr<VideoRecorder> video_;
  bool web_socket_mock_ = false;
  struct WorkerRef {
    content::GlobalRenderFrameHostId frame;
    int32_t number;
    std::string url;
  };
  std::map<std::string, WorkerRef> workers_;
  std::map<std::string, content::GlobalRenderFrameHostId> web_sockets_;
  std::string media_type_;
  int active_calls_ = 0;
  mojo::AssociatedReceiverSet<mojom::AutomationHost,
                              content::GlobalRenderFrameHostId>
      hosts_;
  std::map<std::string, BindingCalledCallback> bindings_;
  int next_binding_ = 0;

  zebkit_automation_event_func on_event_ = nullptr;
  raw_ptr<void> on_event_data_ = nullptr;
  bool hold_dialogs_ = false;
  bool intercept_file_choosers_ = false;
  std::map<std::string, FileChooser> file_choosers_;
  int next_chooser_ = 0;

  // The frames' lifecycles since their last navigation.
  std::map<content::FrameTreeNodeId, Lifecycle> lifecycles_;
  std::map<content::FrameTreeNodeId, base::OneShotTimer> idle_timers_;
  std::deque<NavigationWaiter> navigation_waiters_;
  int settling_ = 0;
  // Calls whose input is being sent, and the navigation it started.
  std::set<Call*> watching_;
  std::map<Call*, int64_t> watched_navigations_;

  // The keyboard's and the mouse's state, as input.ts keeps it.
  std::set<std::string> pressed_modifiers_;
  std::set<std::string> pressed_keys_;
  gfx::PointF mouse_;
  raw_ptr<content::RenderWidgetHost> mouse_widget_ = nullptr;
  struct InputWaiters {
    int generation = 0;
    std::vector<base::OnceClosure> closures;
  };
  std::map<content::RenderWidgetHost*, InputWaiters> input_waiters_;
  int input_wait_generation_ = 0;
  bool input_dropped_ = false;
  gfx::PointF mouse_in_widget_;
  Button last_button_ = Button::kNoButton;
  std::set<Button> buttons_;

  std::set<raw_ptr<content::RenderWidgetHost>> observed_;
  std::map<Type, base::TimeTicks> last_sent_;
  std::map<Type, base::TimeTicks> acked_;
  std::vector<Ack> acks_;

  base::WeakPtrFactory<Automation> weak_factory_{this};
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(Automation);

// The dialogs of pages with automation, for Playwright's dialog events.
class AutomationDialogs : public DialogObserver {
 public:
  bool HoldsDialogs(content::WebContents* contents) override {
    Automation* automation = Automation::FromWebContents(contents);
    return automation && automation->holds_dialogs();
  }
  void DialogOpened(content::WebContents* contents,
                    const zebkit_dialog& dialog) override {
    if (Automation* automation = Automation::FromWebContents(contents))
      automation->DialogOpened(dialog);
  }
  void DialogClosed(content::WebContents* contents,
                    uint64_t id,
                    bool accepted,
                    const std::string& input) override {
    Automation* automation = Automation::FromWebContents(contents);
    if (!automation)
      return;
    base::DictValue params;
    params.Set("dialog", "d" + base::NumberToString(id));
    params.Set("accepted", accepted);
    params.Set("userInput", input);
    automation->Emit("BrowserContext.dialogClosed", std::move(params));
  }
};

// Playwright grants use the same per-profile controller and subscription
// notifications as CDP, including workers. No global shell permission table.
class Permissions {
 public:
  static Permissions* Get() {
    static Permissions permissions;
    return &permissions;
  }
  bool Grant(content::BrowserContext* profile, const base::ListValue& names,
             const std::string& origin, base::OnceCallback<void(bool)> done,
             std::string* error) {
    std::vector<blink::PermissionType> types;
    for (const auto& name : names) {
      auto type = name.is_string() ? PermissionTypeNamed(name.GetString()) : std::nullopt;
      if (!type || *type == blink::PermissionType::GEOLOCATION_APPROXIMATE) {
        *error = "Unknown or unsupported permission: " + (name.is_string() ? name.GetString() : std::string());
        return false;
      }
      types.push_back(*type);
    }
    std::optional<url::Origin> scope;
    if (!origin.empty()) {
      scope = url::Origin::Create(GURL(origin));
      if (scope->opaque()) { *error = "Permission origin must be a non-opaque origin"; return false; }
    }
    content::GrantEmbedderContextPermissions(profile, scope, types, std::move(done));
    return true;
  }
  void Clear(content::BrowserContext* profile, base::OnceClosure done) {
    content::ResetEmbedderContextPermissions(profile, std::move(done));
  }
};

// Downloads of pages with automation (Playwright's acceptDownloads): saved
// into a directory of the engine's without asking, reported as
// Page.download, their files reached with the Artifact commands.
class Downloads : public content::DownloadManager::Observer,
                  public download::DownloadItem::Observer {
 public:
  static Downloads* Get() {
    static base::NoDestructor<Downloads> downloads;
    return downloads.get();
  }

  // Downloads of @contents' profile from now on.
  void Watch(content::WebContents* contents) {
    content::BrowserContext* context = contents->GetBrowserContext();
    content::DownloadManager* manager = context->GetDownloadManager();
    if (!manager || managers_.contains(manager))
      return;
    if (!directory_.IsValid() && !directory_.CreateUniqueTempDir())
      return;
    if (auto* delegate = static_cast<content::ShellDownloadManagerDelegate*>(
            context->GetDownloadManagerDelegate())) {
      delegate->SetDownloadBehaviorForTesting(directory_.GetPath());
    }
    manager->AddObserver(this);
    managers_.insert(manager);
  }

  // content::DownloadManager::Observer
  void OnDownloadCreated(content::DownloadManager*,
                         download::DownloadItem* item) override {
    content::WebContents* contents =
        content::DownloadItemUtils::GetWebContents(item);
    Automation* automation =
        contents ? Automation::FromWebContents(contents) : nullptr;
    if (!automation)
      return;
    const std::string id = "w" + base::NumberToString(item->GetId());
    items_[id] = {item, {}};
    item->AddObserver(this);
    base::DictValue params;
    params.Set("download", id);
    params.Set("url", item->GetURL().spec());
    params.Set("suggestedFilename", item->GetSuggestedFilename().empty()
                                        ? item->GetTargetFilePath().BaseName().AsUTF8Unsafe()
                                        : item->GetSuggestedFilename());
    automation->Emit("Page.download", std::move(params));
  }

  void ManagerGoingDown(content::DownloadManager* manager) override {
    manager->RemoveObserver(this);
    managers_.erase(manager);
  }

  // download::DownloadItem::Observer
  void OnDownloadUpdated(download::DownloadItem* item) override {
    if (item->GetState() == download::DownloadItem::IN_PROGRESS)
      return;
    Entry* entry = Find(item);
    if (!entry)
      return;
    std::vector<base::OnceClosure> waiters = std::move(entry->waiters);
    for (base::OnceClosure& waiter : waiters)
      std::move(waiter).Run();
  }

  void OnDownloadDestroyed(download::DownloadItem* item) override {
    for (auto it = items_.begin(); it != items_.end(); ++it) {
      if (it->second.item == item) {
        std::vector<base::OnceClosure> waiters = std::move(it->second.waiters);
        items_.erase(it);
        for (base::OnceClosure& waiter : waiters)
          std::move(waiter).Run();
        return;
      }
    }
  }

  // Artifact.pathAfterFinished, saveAs, failure, cancel, delete.
  void Command(Call* call) {
    const std::string id = call->String("download");
    auto found = items_.find(id);
    if (found == items_.end()) {
      call->Fail("Download is gone");
      return;
    }
    const std::string name = call->Name();
    download::DownloadItem* item = found->second.item;
    if (name == "cancel") {
      item->Cancel(true);
      call->Done();
      return;
    }
    if (item->GetState() == download::DownloadItem::IN_PROGRESS) {
      found->second.waiters.push_back(base::BindOnce(
          [](base::WeakPtr<Call> call) {
            if (call)
              Downloads::Get()->Command(call.get());
          },
          call->GetWeakPtr()));
      return;
    }
    const bool complete = item->GetState() == download::DownloadItem::COMPLETE;
    if (name == "failure") {
      base::DictValue answer;
      if (!complete) {
        answer.Set("error", item->GetState() == download::DownloadItem::CANCELLED
                                ? "canceled"
                                : download::DownloadInterruptReasonToString(
                                      item->GetLastReason()));
      }
      call->Done(base::Value(std::move(answer)));
      return;
    }
    if (!complete) {
      call->Fail("Download failed: " +
                 download::DownloadInterruptReasonToString(item->GetLastReason()));
      return;
    }
    const base::FilePath path = item->GetTargetFilePath();
    if (name == "pathAfterFinished") {
      call->Answer(base::Value(path.AsUTF8Unsafe()));
      return;
    }
    const bool remove = name == "delete";
    const base::FilePath to = base::FilePath::FromUTF8Unsafe(call->String("path"));
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock()},
        base::BindOnce(
            [](base::FilePath from, base::FilePath to, bool remove) {
              if (remove)
                return base::DeleteFile(from);
              base::CreateDirectory(to.DirName());
              return base::CopyFile(from, to);
            },
            path, to, remove),
        base::BindOnce(
            [](base::WeakPtr<Call> call, bool ok) {
              if (!call)
                return;
              if (ok)
                call->Done();
              else
                call->Fail("zebkit: the download's file could not be copied");
            },
            call->GetWeakPtr()));
  }

 private:
  struct Entry {
    raw_ptr<download::DownloadItem> item;
    std::vector<base::OnceClosure> waiters;
  };

  Entry* Find(download::DownloadItem* item) {
    for (auto& [id, entry] : items_) {
      if (entry.item == item)
        return &entry;
    }
    return nullptr;
  }

  base::ScopedTempDir directory_;
  std::set<raw_ptr<content::DownloadManager>> managers_;
  std::map<std::string, Entry> items_;
};

const std::map<std::string, Automation::Handler>& Automation::Handlers() {
  using A = Automation;
  static const base::NoDestructor<std::map<std::string, Handler>> handlers(
      std::map<std::string, Handler>{
          // Frame
          {"Frame.evaluateExpression", &A::Evaluate},
          {"Frame.evaluateExpressionHandle", &A::Evaluate},
          {"Frame.evalOnSelector", &A::EvalOnSelector},
          {"Frame.evalOnSelectorAll", &A::EvalOnSelector},
          {"Frame.querySelector", &A::QuerySelector},
          {"Frame.querySelectorAll", &A::QuerySelector},
          {"Frame.queryCount", &A::QueryCount},
          {"Frame.textContent", &A::TextContent},
          {"Frame.innerText", &A::InnerText},
          {"Frame.innerHTML", &A::InnerHtml},
          {"Frame.inputValue", &A::InputValue},
          {"Frame.getAttribute", &A::GetAttribute},
          {"Frame.isChecked", &A::IsChecked},
          {"Frame.isDisabled", &A::IsDisabled},
          {"Frame.isEnabled", &A::IsEnabled},
          {"Frame.isEditable", &A::IsEditable},
          {"Frame.isVisible", &A::IsVisible},
          {"Frame.isHidden", &A::IsHidden},
          {"Frame.waitForSelector", &A::WaitForSelector},
          {"Frame.waitForFunction", &A::WaitForFunction},
          {"Frame.waitForTimeout", &A::WaitForTimeout},
          {"Frame.waitForLoadState", &A::WaitForLoadState},
          {"Frame.title", &A::Title},
          {"Frame.content", &A::Content},
          {"Frame.setContent", &A::SetContent},
          {"Frame.addScriptTag", &A::AddScriptTag},
          {"Frame.addStyleTag", &A::AddStyleTag},
          {"Frame.ariaSnapshot", &A::AriaSnapshot},
          {"Frame.ariaSnapshotJSON", &A::AriaSnapshot},
          {"Frame.expect", &A::Expect},
          {"Frame.click", &A::Click},
          {"Frame.dblclick", &A::Dblclick},
          {"Frame.hover", &A::Hover},
          {"Frame.tap", &A::Tap},
          {"Frame.check", &A::Check},
          {"Frame.uncheck", &A::Uncheck},
          {"Frame.dragAndDrop", &A::DragAndDrop},
          {"Frame.fill", &A::Fill},
          {"Frame.focus", &A::Focus},
          {"Frame.blur", &A::Blur},
          {"Frame.press", &A::FramePress},
          {"Frame.type", &A::FrameType},
          {"Frame.selectOption", &A::SelectOption},
          {"Frame.setInputFiles", &A::SetInputFiles},
          {"Frame.dispatchEvent", &A::DispatchEvent},
          {"Frame.goto", &A::Goto},
          // ElementHandle
          {"ElementHandle.evaluateExpression", &A::Evaluate},
          {"ElementHandle.evaluateExpressionHandle", &A::Evaluate},
          {"ElementHandle.evalOnSelector", &A::EvalOnSelector},
          {"ElementHandle.evalOnSelectorAll", &A::EvalOnSelector},
          {"ElementHandle.querySelector", &A::QuerySelector},
          {"ElementHandle.querySelectorAll", &A::QuerySelector},
          {"ElementHandle.boundingBox", &A::BoundingBox},
          {"ElementHandle.check", &A::Check},
          {"ElementHandle.uncheck", &A::Uncheck},
          {"ElementHandle.setChecked", &A::SetChecked},
          {"ElementHandle.click", &A::Click},
          {"ElementHandle.dblclick", &A::Dblclick},
          {"ElementHandle.hover", &A::Hover},
          {"ElementHandle.tap", &A::Tap},
          {"ElementHandle.contentFrame", &A::ContentFrame},
          {"ElementHandle.ownerFrame", &A::OwnerFrame},
          {"ElementHandle.dispatchEvent", &A::DispatchEvent},
          {"ElementHandle.fill", &A::Fill},
          {"ElementHandle.focus", &A::Focus},
          {"ElementHandle.getAttribute", &A::GetAttribute},
          {"ElementHandle.innerHTML", &A::InnerHtml},
          {"ElementHandle.innerText", &A::InnerText},
          {"ElementHandle.inputValue", &A::InputValue},
          {"ElementHandle.isChecked", &A::IsChecked},
          {"ElementHandle.isDisabled", &A::IsDisabled},
          {"ElementHandle.isEditable", &A::IsEditable},
          {"ElementHandle.isEnabled", &A::IsEnabled},
          {"ElementHandle.isHidden", &A::IsHidden},
          {"ElementHandle.isVisible", &A::IsVisible},
          {"ElementHandle.press", &A::FramePress},
          {"ElementHandle.type", &A::FrameType},
          {"ElementHandle.screenshot", &A::Screenshot},
          {"ElementHandle.scrollIntoViewIfNeeded", &A::ScrollIntoViewIfNeeded},
          {"ElementHandle.selectOption", &A::SelectOption},
          {"ElementHandle.selectText", &A::SelectText},
          {"ElementHandle.setInputFiles", &A::SetInputFiles},
          {"ElementHandle.textContent", &A::TextContent},
          {"ElementHandle.waitForElementState", &A::WaitForElementState},
          {"ElementHandle.waitForSelector", &A::WaitForSelector},
          {"ElementHandle.getProperty", &A::GetProperty},
          {"ElementHandle.getPropertyList", &A::GetPropertyList},
          {"ElementHandle.jsonValue", &A::JsonValue},
          {"ElementHandle.dispose", &A::DisposeCommand},
          // JSHandle
          {"JSHandle.evaluateExpression", &A::Evaluate},
          {"JSHandle.evaluateExpressionHandle", &A::Evaluate},
          {"JSHandle.getProperty", &A::GetProperty},
          {"JSHandle.getPropertyList", &A::GetPropertyList},
          {"JSHandle.jsonValue", &A::JsonValue},
          {"JSHandle.dispose", &A::DisposeCommand},
          // Page
          {"Page.reload", &A::Reload},
          {"Page.goBack", &A::GoBack},
          {"Page.goForward", &A::GoForward},
          {"Page.frames", &A::Frames},
          {"Page.keyboardDown", &A::Gated<&A::KeyboardDown>},
          {"Page.keyboardUp", &A::Gated<&A::KeyboardUp>},
          {"Page.keyboardPress", &A::Gated<&A::KeyboardPress>},
          {"Page.keyboardType", &A::Gated<&A::KeyboardType>},
          {"Page.keyboardInsertText", &A::KeyboardInsertText},
          {"Page.mouseMove", &A::Gated<&A::MouseMoveCommand>},
          {"Page.mouseDown", &A::Gated<&A::MouseDownCommand>},
          {"Page.mouseUp", &A::Gated<&A::MouseUpCommand>},
          {"Page.mouseClick", &A::Gated<&A::MouseClickCommand>},
          {"Page.mouseWheel", &A::Gated<&A::MouseWheelCommand>},
          {"Page.touchscreenTap", &A::Gated<&A::TouchscreenTap>},
          {"Page.screenshot", &A::Screenshot},
          {"Page.setViewportSize", &A::SetViewportSize},
          {"Page.addInitScript", &A::AddInitScript},
          {"Page.removeRegistration", &A::RemoveRegistration},
          {"Page.pdf", &A::Pdf},
          {"Page.close", &A::Close},
          {"Page.bringToFront", &A::BringToFront},
          {"Page.updateSubscription", &A::UpdateSubscription},
          {"Page.setNetworkInterceptionPatterns", &A::SetInterceptionPatterns},
          {"BrowserContext.setNetworkInterceptionPatterns", &A::SetInterceptionPatterns},
          {"Page.setExtraHTTPHeaders", &A::SetExtraHttpHeaders},
          {"BrowserContext.setExtraHTTPHeaders", &A::ContextExtraHeaders},
          {"BrowserContext.setOffline", &A::ContextOffline},
          {"BrowserContext.setGeolocation", &A::SetGeolocation},
          {"BrowserContext.setHTTPCredentials", &A::SetHttpCredentials},
          {"BrowserContext.cookies", &A::Cookies},
          {"BrowserContext.addCookies", &A::AddCookies},
          {"BrowserContext.storageState", &A::StorageState},
          {"BrowserContext.setStorageState", &A::SetStorageState},
          {"BrowserContext.clearCookies", &A::ClearCookies},
          {"APIRequestContext.fetch", &A::Fetch},
          {"Page.emulateMedia", &A::EmulateMedia},
          {"Page.exposeBinding", &A::ExposeBinding},
          {"Page.workers", &A::Workers},
          {"Worker.evaluateExpression", &A::WorkerEvaluate},
          {"Page.setWebSocketInterceptionPatterns", &A::SetWebSocketPatterns},
          {"BrowserContext.setWebSocketInterceptionPatterns", &A::SetWebSocketPatterns},
          {"WebSocketRoute.connect", &A::WebSocketCommand},
          {"WebSocketRoute.ensureOpened", &A::WebSocketCommand},
          {"WebSocketRoute.sendToPage", &A::WebSocketCommand},
          {"WebSocketRoute.sendToServer", &A::WebSocketCommand},
          {"WebSocketRoute.closePage", &A::WebSocketCommand},
          {"WebSocketRoute.closeServer", &A::WebSocketCommand},
          {"BrowserContext.exposeBinding", &A::ExposeBinding},
          {"BindingCall.resolve", &A::BindingResolve},
          {"BindingCall.reject", &A::BindingReject},
          {"BrowserContext.clockInstall", &A::ClockInstall},
          {"BrowserContext.clockFastForward", &A::ClockFastForward},
          {"BrowserContext.clockPauseAt", &A::ClockPauseAt},
          {"BrowserContext.clockResume", &A::ClockResume},
          {"BrowserContext.clockRunFor", &A::ClockRunFor},
          {"BrowserContext.clockSetFixedTime", &A::ClockSetFixedTime},
          {"BrowserContext.clockSetSystemTime", &A::ClockSetSystemTime},
          {"Artifact.pathAfterFinished", &A::Download},
          {"Artifact.saveAs", &A::Download},
          {"Artifact.failure", &A::Download},
          {"Artifact.cancel", &A::Download},
          {"Artifact.delete", &A::Download},
          {"BrowserContext.grantPermissions", &A::GrantPermissions},
          {"BrowserContext.clearPermissions", &A::ClearPermissions},
          {"Response.body", &A::ResponseBody},
          {"Tracing.tracingStart", &A::TracingStart},
          {"Tracing.tracingStartChunk", &A::TracingStartChunk},
          {"Tracing.tracingStopChunk", &A::TracingStopChunk},
          {"Tracing.tracingStop", &A::TracingStop},
          {"Tracing.tracingGroup", &A::TracingGroup},
          {"Tracing.tracingGroupEnd", &A::TracingGroup},
          {"Tracing.harStart", &A::HarStart},
          {"Tracing.harExport", &A::HarExport},
          {"Page.startVideo", &A::StartVideo},
          {"Page.stopVideo", &A::StopVideo},
          // Route
          {"Route.continue", &A::RouteContinue},
          {"Route.fulfill", &A::RouteFulfill},
          {"Route.abort", &A::RouteAbort},
          // Dialog, FileChooser
          {"Dialog.accept", &A::DialogAccept},
          {"Dialog.dismiss", &A::DialogDismiss},
          {"FileChooser.setFiles", &A::FileChooserSetFiles},
      });
  return *handlers;
}

void WatchDownloads(content::WebContents* contents) {
  Downloads::Get()->Watch(contents);
}

void Automation::GrantPermissions(Call* call) {
  std::string error;
  const base::ListValue* names = call->params().FindList("permissions");
  if (!Permissions::Get()->Grant(view_->web_contents()->GetBrowserContext(),
      names ? names->Clone() : base::ListValue(), call->String("origin"),
      base::BindOnce([](base::WeakPtr<Call> pending, bool ok) {
        if (!pending) return;
        if (ok) pending->Done(); else pending->Fail("Permissions cannot be overridden");
      }, call->GetWeakPtr()), &error)) call->Fail(error);
}

void Automation::ClearPermissions(Call* call) {
  Permissions::Get()->Clear(view_->web_contents()->GetBrowserContext(),
      base::BindOnce([](base::WeakPtr<Call> pending) { if (pending) pending->Done(); }, call->GetWeakPtr()));
}

void Automation::Download(Call* call) {
  Downloads::Get()->Command(call);
}

char* Copy(const std::string& text) {
  return strdup(text.c_str());
}

// ---- Contexts and pages: commands of no page ------------------------------

// The pages and contexts automation made or was given, by their ids.
struct Registry {
  std::map<std::string, raw_ptr<zebkit_context>> contexts;
  std::map<std::string, base::WeakPtr<zebkit_view>> pages;
  int next = 0;

  std::string IdOf(zebkit_view* view) {
    for (auto& [id, page] : pages) {
      if (page.get() == view)
        return id;
    }
    const std::string id = "p" + base::NumberToString(++next);
    pages[id] = view->GetWeakPtr();
    return id;
  }

  zebkit_view* Page(const std::string& id) {
    auto found = pages.find(id);
    return found == pages.end() ? nullptr : found->second.get();
  }

  zebkit_context* Context(const std::string& id) {
    auto found = contexts.find(id);
    return found == contexts.end() ? nullptr : found->second.get();
  }
};

Registry& Registered() {
  static base::NoDestructor<Registry> registry;
  return *registry;
}

std::string Automation::Registered_IdOf() {
  return Registered().IdOf(view_.get());
}

// Browser.newContext: a profile of its own, in memory, with Playwright's
// options for its pages (viewport, userAgent, hasTouch, javaScriptEnabled,
// colorScheme, reducedMotion, forcedColors, geolocation, permissions,
// extraHTTPHeaders, offline, httpCredentials, storageState's cookies).
void NewContext(Call* call) {
  if (const auto* permissions = call->params().Find("permissions")) {
    if (!permissions->is_list()) { call->Fail("permissions must be a list"); return; }
    for (const auto& name : permissions->GetList()) {
      const auto type = name.is_string() ? PermissionTypeNamed(name.GetString()) : std::nullopt;
      if (!type || *type == blink::PermissionType::GEOLOCATION_APPROXIMATE) {
        call->Fail("Unknown or unsupported initial permission"); return;
      }
    }
  }
  zebkit_context* context = zebkit_context_new();
  const std::string id = "x" + base::NumberToString(++Registered().next);
  Registered().contexts[id] = context;
  content::BrowserContext* profile = ProfileOf(context);
  ContextState& state = ContextStates()[profile];
  state.options = call->params().Clone();
  if (const base::DictValue* geolocation = state.options.FindDict("geolocation")) {
    state.geolocation = content::GeolocationOverride{
        geolocation->FindDouble("latitude").value_or(0),
        geolocation->FindDouble("longitude").value_or(0),
        geolocation->FindDouble("accuracy").value_or(0)};
  }
  if (const base::DictValue* credentials = state.options.FindDict("httpCredentials"))
    state.credentials = credentials->Clone();
  if (const base::ListValue* headers = state.options.FindList("extraHTTPHeaders"))
    state.extra_headers = headers->Clone();
  state.offline = state.options.FindBool("offline").value_or(false);
  base::DictValue answer;
  answer.Set("context", id);
  auto complete = base::BindOnce(
      [](base::WeakPtr<Call> pending, content::BrowserContext* profile,
         base::DictValue options, base::DictValue answer) {
        if (!pending) return;
        const auto* cookies = options.FindListByDottedPath("storageState.cookies");
        if (!cookies || cookies->empty()) {
          pending->Done(base::Value(std::move(answer)));
          return;
        }
        base::DictValue params;
        params.Set("cookies", cookies->Clone());
        ContextAddCookies(profile, params, base::BindOnce(
            [](base::WeakPtr<Call> call, base::DictValue answer,
               base::Value, const std::string& error) {
              if (!call) return;
              if (!error.empty()) call->Fail(error);
              else call->Done(base::Value(std::move(answer)));
            }, pending, std::move(answer)));
      }, call->GetWeakPtr(), profile, state.options.Clone(), std::move(answer));
  if (const auto* names = state.options.FindList("permissions"); names && !names->empty()) {
    std::string error;
    if (!Permissions::Get()->Grant(profile, *names, std::string(),
        base::BindOnce([](base::WeakPtr<Call> pending, base::OnceClosure complete, std::string id,
                          content::BrowserContext* profile, zebkit_context* context, bool ok) {
          if (!ok) {
            // The client never gets the id to close it with.
            Registered().contexts.erase(id);
            ContextStates().erase(profile);
            zebkit_context_close(context);
            if (pending) pending->Fail("Permissions cannot be overridden");
            return;
          }
          if (pending) std::move(complete).Run();
        }, call->GetWeakPtr(), std::move(complete), id, profile, context), &error)) {
      Registered().contexts.erase(id);
      ContextStates().erase(profile);
      zebkit_context_close(context);
      call->Fail(error);
    }
    return;
  }
  std::move(complete).Run();
}

// BrowserContext.newPage: a view in the context ("context"; the engine's
// own profile without), off-screen without a display, of the context's
// viewport (1280 by 720 by default); {page}, for zebkit_automation_page().
void NewPage(Call* call) {
  const std::string context_id = call->String("context");
  zebkit_context* context = nullptr;
  if (!context_id.empty()) {
    context = Registered().Context(context_id);
    if (!context) {
      call->Fail("Target page, context or browser has been closed");
      return;
    }
  }
  const ContextState& state = ContextStates()[ProfileOf(context)];
  const int width =
      state.options.FindIntByDottedPath("viewport.width").value_or(1280);
  const int height =
      state.options.FindIntByDottedPath("viewport.height").value_or(720);
  const float scale = static_cast<float>(
      state.options.FindDouble("deviceScaleFactor").value_or(1));
  zebkit_view* view =
      zebkit_is_headless()
          ? zebkit_view_new_offscreen_in(context, nullptr, width, height, scale)
          : zebkit_view_new_in(context, nullptr, width, height);
  if (!view) {
    call->Fail("zebkit: the page could not be made");
    return;
  }
  Automation::For(view);
  base::DictValue answer;
  answer.Set("page", Registered().IdOf(view));
  call->Done(base::Value(std::move(answer)));
}

// BrowserContext.pages: the context's pages ({pages: [ids]}).
void Pages(Call* call) {
  zebkit_context* context = Registered().Context(call->String("context"));
  content::BrowserContext* profile = ProfileOf(context);
  base::ListValue pages;
  for (zebkit_view* view : zebkit_view::Views()) {
    if (view->web_contents() && view->web_contents()->GetBrowserContext() == profile)
      pages.Append(Registered().IdOf(view));
  }
  base::DictValue answer;
  answer.Set("pages", std::move(pages));
  call->Done(base::Value(std::move(answer)));
}

// BrowserContext.close: its pages closed, the context gone.
void CloseContext(Call* call) {
  const std::string id = call->String("context");
  zebkit_context* context = Registered().Context(id);
  if (!context) {
    call->Done();
    return;
  }
  Registered().contexts.erase(id);
  zebkit_context_close(context);
  call->Done();
}

// A BrowserContext command with "context" and no page (Playwright's
// endpoint, automation_server.cc): for the profile itself, for every page
// of the context, and kept for its pages to come.
void ContextCommand(Call* call) {
  const std::string id = call->String("context");
  zebkit_context* context = id.empty() ? nullptr : Registered().Context(id);
  if (!id.empty() && !context) {
    call->Fail(kClosed);
    return;
  }
  content::BrowserContext* profile = ProfileOf(context);
  ContextState& state = ContextStates()[profile];
  std::vector<zebkit_view*> pages;
  for (zebkit_view* view : zebkit_view::Views()) {
    if (view->web_contents() && view->web_contents()->GetBrowserContext() == profile)
      pages.push_back(view);
  }
  const std::string name = call->Name();
  base::DictValue params = call->params().Clone();
  params.Remove("context");
  if (name == "exposeBinding") {
    const std::string* binding = params.FindString("name");
    if (!binding || binding->empty()) {
      call->Fail("name: expected a string");
      return;
    }
    bool duplicate = std::any_of(state.replay.begin(), state.replay.end(), [&](const auto& entry) {
      const std::string* existing = entry.second.FindString("name");
      return entry.first == "Page.exposeBinding" && existing && *existing == *binding;
    });
    for (zebkit_view* view : pages) {
      if (Automation* automation = Automation::FromWebContents(view->web_contents()))
        duplicate |= automation->HasBinding(*binding);
    }
    if (duplicate) {
      call->Fail("Function \"" + *binding + "\" has been already registered");
      return;
    }
  }
  const bool registration = name == "addInitScript" || name == "exposeBinding";
  if (registration)
    params.Set("registration", RegistrationId(params));
  const bool removal = name == "removeRegistration";
  if (removal) {
    const std::string* token = params.FindString("registration");
    std::erase_if(state.replay, [&](const auto& entry) {
      return token && entry.second.FindString("registration") &&
             *entry.second.FindString("registration") == *token;
    });
  }

  auto finish = base::BindOnce(
      [](base::WeakPtr<Call> call, base::Value result, const std::string& error) {
        if (!call)
          return;
        if (!error.empty())
          call->Fail(error);
        else
          call->Done(std::move(result));
      },
      call->GetWeakPtr());
  if (name == "cookies") {
    ContextCookies(profile, params, std::move(finish));
    return;
  }
  if (name == "addCookies") {
    ContextAddCookies(profile, params, std::move(finish));
    return;
  }
  if (name == "clearCookies") {
    ContextClearCookies(profile, params, std::move(finish));
    return;
  }
  if (name == "grantPermissions") {
    std::string error;
    const base::ListValue* names = params.FindList("permissions");
    const std::string* origin = params.FindString("origin");
    if (!Permissions::Get()->Grant(profile, names ? names->Clone() : base::ListValue(),
        origin ? *origin : std::string(), base::BindOnce([](base::WeakPtr<Call> pending, bool ok) {
          if (!pending) return;
          if (ok) pending->Done(); else pending->Fail("Permissions cannot be overridden");
        }, call->GetWeakPtr()), &error)) call->Fail(error);
    return;
  }
  if (name == "clearPermissions") {
    Permissions::Get()->Clear(profile,
        base::BindOnce([](base::WeakPtr<Call> pending) { if (pending) pending->Done(); }, call->GetWeakPtr()));
    return;
  }
  // Kept for the context's pages to come; what is not, a context without
  // pages takes for later.
  static const base::NoDestructor<std::map<std::string, std::string>> kReplayed(
      std::map<std::string, std::string>{
          {"addInitScript", "Page.addInitScript"},
          {"exposeBinding", "Page.exposeBinding"},
          {"setNetworkInterceptionPatterns", "Page.setNetworkInterceptionPatterns"},
          {"setWebSocketInterceptionPatterns", "Page.setWebSocketInterceptionPatterns"},
          {"updateSubscription", "Page.updateSubscription"},
          {"clockInstall", "BrowserContext.clockInstall"},
          {"clockFastForward", "BrowserContext.clockFastForward"},
          {"clockPauseAt", "BrowserContext.clockPauseAt"},
          {"clockResume", "BrowserContext.clockResume"},
          {"clockRunFor", "BrowserContext.clockRunFor"},
          {"clockSetFixedTime", "BrowserContext.clockSetFixedTime"},
          {"clockSetSystemTime", "BrowserContext.clockSetSystemTime"},
      });
  std::string method = removal ? "Page.removeRegistration" : "BrowserContext." + name;
  if (auto replayed = kReplayed->find(name); replayed != kReplayed->end()) {
    method = replayed->second;
    if (name == "setNetworkInterceptionPatterns" ||
        name == "setWebSocketInterceptionPatterns") {
      std::erase_if(state.replay,
                    [&](const auto& entry) { return entry.first == method; });
    }
    state.replay.emplace_back(method, params.Clone());
  } else if (pages.empty() && !removal) {
    if (name == "setGeolocation") {
      if (const base::DictValue* given = params.FindDict("geolocation")) {
        state.geolocation = content::GeolocationOverride{
            given->FindDouble("latitude").value_or(0),
            given->FindDouble("longitude").value_or(0),
            given->FindDouble("accuracy").value_or(0)};
      } else {
        state.geolocation.reset();
      }
    } else if (name == "setOffline") {
      state.offline = params.FindBool("offline").value_or(false);
    } else if (name == "setExtraHTTPHeaders") {
      if (const base::ListValue* headers = params.FindList("headers"))
        state.extra_headers = headers->Clone();
    } else if (name == "setHTTPCredentials") {
      if (const base::DictValue* credentials = params.FindDict("httpCredentials"))
        state.credentials = credentials->Clone();
      else
        state.credentials.reset();
    } else if (name == "storageState") {
      ContextCookies(profile, base::DictValue(),
                     base::BindOnce(
                         [](base::WeakPtr<Call> call, base::Value cookies,
                            const std::string& error) {
                           if (!call)
                             return;
                           base::DictValue answer;
                           if (cookies.is_dict()) {
                             if (base::Value* list = cookies.GetDict().Find("cookies"))
                               answer.Set("cookies", std::move(*list));
                           }
                           answer.Set("origins", base::ListValue());
                           call->Done(base::Value(std::move(answer)));
                         },
                         call->GetWeakPtr()));
      return;
    } else {
      call->Fail("zebkit: " + call->method() + " needs a page of the context");
      return;
    }
    call->Done();
    return;
  }
  // Context-wide commands go to one page (they act on the whole context);
  // the replayed ones to every page.
  const bool every = kReplayed->contains(name) || removal;
  std::vector<zebkit_view*> targets =
      every ? pages
            : std::vector<zebkit_view*>(pages.begin(),
                                        pages.empty() ? pages.begin() : pages.begin() + 1);
  if (targets.empty()) {
    if (registration)
      call->Done(RegistrationResult(*params.FindString("registration"), name == "addInitScript" ? "script" : "binding"));
    else
      call->Done();
    return;
  }
  struct Pending {
    base::WeakPtr<Call> call;
    size_t left;
    std::string error;
    base::Value result;
  };
  auto* pending = new Pending{call->GetWeakPtr(), targets.size(), std::string(), base::Value()};
  const std::string json = Json(base::Value(params.Clone()));
  for (zebkit_view* view : targets) {
    zebkit_automation_call(
        view, method.c_str(), json.c_str(),
        [](zebkit_view*, const char* result, const char* error, void* data) {
          auto* pending = static_cast<Pending*>(data);
          if (error && pending->error.empty())
            pending->error = error;
          if (result && pending->result.is_none()) {
            std::optional<base::Value> parsed = Parse(result);
            if (parsed)
              pending->result = std::move(*parsed);
          }
          if (--pending->left)
            return;
          if (pending->call) {
            if (!pending->error.empty())
              pending->call->Fail(pending->error);
            else
              pending->call->Done(pending->result.is_none()
                                      ? base::Value(base::DictValue())
                                      : std::move(pending->result));
          }
          delete pending;
        },
        pending);
  }
}

// APIRequestContext.fetch of a context ("context"), without a page.
void ContextFetchCommand(Call* call) {
  const std::string id = call->String("context");
  zebkit_context* context = id.empty() ? nullptr : Registered().Context(id);
  content::BrowserContext* profile =
      call->view() && call->view()->web_contents()
          ? call->view()->web_contents()->GetBrowserContext()
          : ProfileOf(context);
  ContextFetch(profile, call->params(),
               base::BindOnce(
                   [](base::WeakPtr<Call> call, base::Value result,
                      const std::string& error) {
                     if (!call)
                       return;
                     if (!error.empty())
                       call->Fail(error);
                     else
                       call->Done(std::move(result));
                   },
                   call->GetWeakPtr()));
}

using GlobalHandler = void (*)(Call*);
const std::map<std::string, GlobalHandler>& GlobalHandlers() {
  static const base::NoDestructor<std::map<std::string, GlobalHandler>> handlers(
      std::map<std::string, GlobalHandler>{
          {"Browser.newContext", &NewContext},
          {"BrowserContext.newPage", &NewPage},
          {"BrowserContext.pages", &Pages},
          {"BrowserContext.close", &CloseContext},
          {"APIRequestContext.fetch", &ContextFetchCommand},
      });
  return *handlers;
}

// HTTP authentication answered with the context's credentials
// (BrowserContext.setHTTPCredentials), once: a second challenge is
// cancelled, as Playwright's.
class LoginDelegate : public content::LoginDelegate {
 public:
  LoginDelegate(std::optional<net::AuthCredentials> credentials,
                LoginAuthRequiredCallback callback) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](base::WeakPtr<LoginDelegate> self,
               std::optional<net::AuthCredentials> credentials) {
              if (self && self->callback_)
                std::move(self->callback_).Run(credentials);
            },
            weak_factory_.GetWeakPtr(), std::move(credentials)));
    callback_ = std::move(callback);
  }

 private:
  LoginAuthRequiredCallback callback_;
  base::WeakPtrFactory<LoginDelegate> weak_factory_{this};
};

}  // namespace

std::unique_ptr<content::LoginDelegate> CreateAutomationLoginDelegate(
    const net::AuthChallengeInfo& challenge,
    content::BrowserContext* profile,
    bool first_attempt,
    content::LoginDelegate::LoginAuthRequiredCallback callback) {
  auto found = ContextStates().find(profile);
  if (found == ContextStates().end() || !found->second.credentials)
    return nullptr;
  const base::DictValue& credentials = *found->second.credentials;
  const std::string* origin = credentials.FindString("origin");
  if (origin && !origin->empty() &&
      url::SchemeHostPort(GURL(*origin)) != challenge.challenger) {
    return nullptr;
  }
  std::optional<net::AuthCredentials> answer;
  if (first_attempt) {
    const std::string* username = credentials.FindString("username");
    const std::string* password = credentials.FindString("password");
    answer = net::AuthCredentials(
        base::UTF8ToUTF16(username ? *username : std::string()),
        base::UTF8ToUTF16(password ? *password : std::string()));
  }
  return std::make_unique<LoginDelegate>(std::move(answer), std::move(callback));
}

void BindAutomationHost(
    content::RenderFrameHost* frame,
    mojo::PendingAssociatedReceiver<mojom::AutomationHost> receiver) {
  content::WebContents* contents =
      content::WebContents::FromRenderFrameHost(frame);
  if (Automation* automation =
          contents ? Automation::FromWebContents(contents) : nullptr) {
    automation->BindHost(frame, std::move(receiver));
  }
}

void InstallAutomationHooks() {
  static bool installed = false;
  if (installed)
    return;
  installed = true;
  static base::NoDestructor<AutomationDialogs> dialogs;
  SetDialogObserver(dialogs.get());
  content::Shell::SetFileChooserHandler(base::BindRepeating(
      [](content::RenderFrameHost* frame,
         scoped_refptr<content::FileSelectListener> listener,
         const blink::mojom::FileChooserParams& params) {
        if (HandleFileChooserHook(frame, listener, params))
          return true;
        content::WebContents* contents =
            content::WebContents::FromRenderFrameHost(frame);
        Automation* automation =
            contents ? Automation::FromWebContents(contents) : nullptr;
        if (automation && automation->TakeFileChooser(frame, listener, params))
          return true;
        // As headless Chrome: no system dialog without a display; the
        // chooser is cancelled.
        if (zebkit_is_headless()) {
          listener->FileSelectionCanceled();
          return true;
        }
        return false;
      }));
}

}  // namespace zebkit

void zebkit_automation_call(zebkit_view* view,
                            const char* method,
                            const char* params,
                            zebkit_automation_func func,
                            void* data) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  using zebkit::Automation;
  std::optional<base::Value> parsed = base::JSONReader::Read(
      params && *params ? params : "{}", base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    if (func)
      func(view, nullptr, "params: expected a JSON object", data);
    return;
  }
  const std::string name = method ? method : "";
  auto global = zebkit::GlobalHandlers().find(name);
  if (global == zebkit::GlobalHandlers().end() && !view &&
      name.rfind("BrowserContext.", 0) == 0) {
    zebkit::InstallAutomationHooks();
    auto* call = new zebkit::Call(view, name, std::move(parsed->GetDict()),
                                  func, data);
    zebkit::ContextCommand(call);
    return;
  }
  if (global != zebkit::GlobalHandlers().end()) {
    zebkit::InstallAutomationHooks();
    // Deletes itself once answered.
    auto* call = new zebkit::Call(view, name, std::move(parsed->GetDict()),
                                  func, data);
    global->second(call);
    return;
  }
  const auto& handlers = Automation::Handlers();
  auto handler = handlers.find(name);
  if (handler == handlers.end()) {
    if (func)
      func(view, nullptr, ("zebkit: no command " + name).c_str(), data);
    return;
  }
  Automation* automation = view ? Automation::For(view) : nullptr;
  if (!automation) {
    if (func)
      func(view, nullptr, zebkit::kClosed, data);
    return;
  }
  // Deletes itself once answered.
  auto* call = new zebkit::Call(view, name, std::move(parsed->GetDict()), func,
                                data);
  automation->Run(call, handler->second);
}

void zebkit_automation_on_event(zebkit_view* view,
                                zebkit_automation_event_func func,
                                void* data) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (zebkit::Automation* automation =
          view ? zebkit::Automation::For(view) : nullptr) {
    automation->SetEventHandler(func, data);
  }
}

zebkit_view* zebkit_automation_page(const char* page) {
  return page ? zebkit::Registered().Page(page) : nullptr;
}

char* zebkit_automation_page_id(zebkit_view* view) {
  return zebkit::Copy(view ? zebkit::Registered().IdOf(view) : std::string());
}

char* zebkit_automation_commands(void) {
  base::ListValue names;
  for (const auto& [name, handler] : zebkit::Automation::Handlers())
    names.Append(name);
  for (const auto& [name, handler] : zebkit::GlobalHandlers())
    names.Append(name);
  return zebkit::Copy(zebkit::Json(base::Value(std::move(names))));
}
