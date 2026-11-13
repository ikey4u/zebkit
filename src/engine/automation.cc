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
#include <array>
#include <cmath>
#include <deque>
#include <functional>
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
#include "base/barrier_closure.h"
#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/numerics/safe_conversions.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted_memory.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/utf_string_conversions.h"
#include "components/version_info/version_info.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "net/base/mime_util.h"
#include "crypto/openssl_util.h"
#include "net/cert/x509_certificate.h"
#include "net/cert/x509_util.h"
#include "net/ssl/openssl_private_key.h"
#include "net/ssl/ssl_cert_request_info.h"
#include "net/ssl/ssl_private_key.h"
#include "content/public/browser/client_certificate_delegate.h"
#include "third_party/boringssl/src/include/openssl/bio.h"
#include "third_party/boringssl/src/include/openssl/err.h"
#include "third_party/boringssl/src/include/openssl/evp.h"
#include "third_party/boringssl/src/include/openssl/pem.h"
#include "third_party/boringssl/src/include/openssl/pkcs8.h"
#include "third_party/boringssl/src/include/openssl/x509.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "build/build_config.h"
#include "components/input/native_web_keyboard_event.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "content/public/browser/console_message.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/service_worker_running_info.h"
#include "content/public/browser/service_worker_context_observer.h"
#include "content/public/browser/service_worker_context.h"
#include "content/public/browser/devtools_agent_host_client.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/browser/devtools/service_worker_devtools_agent_host.h"
#include "content/browser/renderer_host/frame_tree_node.h"
#include "content/browser/renderer_host/render_frame_host_impl.h"
#include "content/browser/devtools/service_worker_devtools_manager.h"
#include "content/public/browser/file_select_listener.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
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
#include "zebkit/engine/window.h"
#include "zebkit/engine/download_manager.h"
#include "zebkit/engine/permission_manager.h"
#include "third_party/blink/public/common/permissions/permission_utils.h"
#include "url/origin.h"
#include "mojo/public/cpp/bindings/associated_receiver_set.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "net/base/filename_util.h"
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
#include "content/public/browser/content_browser_client.h"
#include "content/public/common/content_client.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/gfx/geometry/rect_f.h"
#include "zebkit/engine/automation.h"
#include "zebkit/engine/automation_compare.h"
#include "zebkit/engine/automation_webauthn.h"
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

// patches/1081: the picture of a Page.captureScreenshot of format
// "zebkit-raw", by the key its data is.
namespace content::protocol {
SkBitmap TakeZebkitRawScreenshot(const std::string& key);
}  // namespace content::protocol

namespace zebkit {

// callLog.ts's compressCallLog(): a block of lines repeated in a row once,
// with its count ("3 × waiting for ..."), each line as "  - line".
std::vector<std::string> CompressCallLog(const std::vector<std::string>& log) {
  std::vector<std::string> lines;
  const size_t n = log.size();
  for (size_t i = 0; i < n;) {
    size_t best_count = 1, best_length = 1;
    for (size_t p = 1; p <= n - i; ++p) {
      size_t k = 1;
      while (i + p * (k + 1) <= n &&
             std::equal(log.begin() + i, log.begin() + i + p, log.begin() + i + p * k)) {
        ++k;
      }
      if (k > 1 && k * p > best_count * best_length) {
        best_count = k;
        best_length = p;
      }
    }
    const std::string count = base::NumberToString(best_count) + " \xC3\x97 ";
    for (size_t j = 0; j < best_length; ++j) {
      const std::string& line = log[i + j];
      const size_t text = line.find_first_not_of(' ');
      const std::string indent = "  " + line.substr(0, text == std::string::npos ? line.size() : text);
      const std::string trimmed = text == std::string::npos ? std::string() : line.substr(text);
      if (best_count > 1 && j == 0)
        lines.push_back(indent + count + trimmed);
      else if (best_count > 1)
        lines.push_back(indent + std::string(count.size() - 3, ' ') + "- " + trimmed);
      else
        lines.push_back(indent + "- " + trimmed);
    }
    i += best_length * best_count;
  }
  return lines;
}

namespace {

using mojom::AutomationExpression;
using mojom::AutomationHandlePtr;
using mojom::AutomationStatus;
using mojom::AutomationWorld;
using Type = blink::WebInputEvent::Type;
using Button = blink::WebPointerProperties::Button;

constexpr int kDefaultTimeoutMs = 30000;
constexpr char kFrameDetachedDot[] = "Frame has been detached.";
// Playwright's messages.
constexpr char kClosed[] = "Target page, context or browser has been closed";
constexpr char kNavigated[] =
    "Execution context was destroyed, most likely because of a navigation";
constexpr char kDetached[] = "Element is not attached to the DOM";
// Not Playwright's: it has no such case, as its round trips hide it.
constexpr char kInputDropped[] =
    "The browser dropped the input: the page's widget did not take input";
constexpr char kDisposed[] = "JSHandle is disposed";

// An evaluation in a dedicated worker over its own DevTools session
// (Runtime.callFunctionOn), with Playwright's serialized values as the
// agent has them (utilityScriptSerializers.ts): (isFunction, hasThis,
// byValue, arg, ...handles), the expression in place of __EXPRESSION__ (no
// eval, which a worker's CSP may refuse). Its `this` is the handle's object.
constexpr char kWorkerEvaluate[] = R"JS(
async function (isFunction, hasThis, byValue, arg, ...handles) {
  const parse = (v, refs = new Map()) => {
    if (v === null || typeof v !== 'object') return v;
    if ('h' in v) return handles[v.h];
    if ('ref' in v) return refs.get(v.ref);
    if ('v' in v) return ({ undefined: undefined, null: null, NaN: NaN, Infinity: Infinity,
                            '-Infinity': -Infinity, '-0': -0 })[v.v];
    if ('d' in v) return new Date(v.d);
    if ('u' in v) return new URL(v.u);
    if ('bi' in v) return BigInt(v.bi);
    if ('r' in v) return new RegExp(v.r.p, v.r.f);
    if ('e' in v) { const e = new Error(v.e.m); e.name = v.e.n; e.stack = v.e.s; return e; }
    if ('a' in v) { const a = []; refs.set(v.id, a); for (const x of v.a) a.push(parse(x, refs)); return a; }
    if ('o' in v) { const o = {}; refs.set(v.id, o); for (const { k, v: x } of v.o) o[k] = parse(x, refs); return o; }
    return undefined;
  };
  const serialize = (v, seen = new Map()) => {
    if (typeof v === 'symbol' || typeof v === 'function') return undefined;
    if (v === undefined) return { v: 'undefined' };
    if (v === null) return { v: 'null' };
    if (typeof v === 'number') {
      if (Number.isNaN(v)) return { v: 'NaN' };
      if (v === Infinity) return { v: 'Infinity' };
      if (v === -Infinity) return { v: '-Infinity' };
      if (Object.is(v, -0)) return { v: '-0' };
      return v;
    }
    if (typeof v === 'bigint') return { bi: v.toString() };
    if (typeof v !== 'object') return v;
    if (seen.has(v)) return { ref: seen.get(v) };
    if (v instanceof Date) return { d: v.toJSON() };
    if (v instanceof URL) return { u: v.href };
    if (v instanceof RegExp) return { r: { p: v.source, f: v.flags } };
    if (v instanceof Error) return { e: { n: v.name, m: v.message, s: v.stack } };
    const id = seen.size + 1;
    seen.set(v, id);
    if (Array.isArray(v)) return { a: v.map(x => serialize(x, seen)), id };
    const o = [];
    for (const k of Object.keys(v)) {
      let x;
      try { x = v[k]; } catch { continue; }
      if (k === 'toJSON' && typeof x === 'function') continue;
      o.push({ k, v: serialize(x, seen) });
    }
    return { o, id };
  };
  const value = parse(arg);
  const f = await (__EXPRESSION__);
  const result = isFunction !== false && typeof f === 'function'
      ? await (hasThis ? f(this, value) : f(value)) : f;
  return byValue ? serialize(result) : result;
}
)JS";
// The binding Playwright's WebSocket mock reports through.
constexpr char kWebSocketBinding[] = "__pwWebSocketBinding";
// credentials.ts's binding, answered here.
constexpr char kWebAuthnBinding[] = "__pwWebAuthnBinding";
constexpr char kFrameDetached[] = "Frame was detached";
// How a selector enters the next frame (frameSelectors.ts).
constexpr char kEnterFrame[] = " >> internal:control=enter-frame >> ";
// frameLocator() without a selector: the frame of the subtree that matches.
constexpr char kAnyFrame[] = "internal:control=any-frame";

// A selector in a call log as Playwright's asLocator() shows a CSS one; one of
// its own engines' chains stays as it is.
std::string LocatorText(const std::string& selector) {
  if (selector.find(" >> ") != std::string::npos ||
      selector.find("internal:") != std::string::npos ||
      selector.find('=') != std::string::npos) {
    return selector;
  }
  return "locator('" + selector + "')";
}

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

class Call;
// An evaluation's answer after what the page logged meanwhile
// (Automation::AfterConsole()): false when nothing waits, and the answer
// goes now; else @answer runs once the page's console messages are out.
// @barrier: after what the page's DevTools session has to tell as well.
bool AnswerAfterConsole(zebkit_view* view, bool barrier, base::RepeatingClosure answer);
// Calls given an "abortId", which Automation.abort ends (the endpoint's
// __abort__, Playwright's AbortSignal).
// A command's own calls may carry its parameters, and so its id: all go.
std::multimap<std::string, Call*>& AbortableCalls() {
  static base::NoDestructor<std::multimap<std::string, Call*>> calls;
  return *calls;
}

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
      timeout_ms_ = base::saturated_cast<int>(*timeout);  // NaN is 0, none
    if (timeout_ms_ > 0) {
      deadline_ = base::TimeTicks::Now() + base::Milliseconds(timeout_ms_);
      timer_.Start(FROM_HERE, base::Milliseconds(timeout_ms_),
                   base::BindOnce(&Call::TimedOut, base::Unretained(this)));
    }
    if (const std::string* abort = params_.FindString("abortId")) {
      abort_id_ = *abort;
      AbortableCalls().emplace(abort_id_, this);
    }
  }

  const base::DictValue& params() const { return params_; }
  base::DictValue& params() { return params_; }
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
  // An assertion through frameLocator() that matched in no frame yet: it
  // searches the frames again unless it passes.
  bool any_frame_retry = false;

  bool Expired() const {
    return timeout_ms_ > 0 && base::TimeTicks::Now() >= deadline_;
  }

  // Lines repeat as Playwright's do; the error compresses them.
  void Log(const std::string& line) {
    if (log_.size() < kMaxLogLines) {
      log_.push_back(line);
      log_times_.push_back(base::TimeTicks::Now());
    }
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
    if (Defer()) {
      deferred_result_ = std::move(result);
      return;
    }
    finished_ = true;
    timer_.Stop();
    // A traced call's log, for the endpoint's trace (tracing.ts's log
    // events): [[ms on the TimeTicks clock, line], ...].
    if (Flag("traceLog") && result.is_dict() && !log_.empty()) {
      base::ListValue lines;
      for (size_t i = 0; i < log_.size(); ++i) {
        base::ListValue line;
        line.Append((log_times_[i] - base::TimeTicks()).InMillisecondsF());
        line.Append(log_[i]);
        lines.Append(std::move(line));
      }
      result.GetDict().Set("__log", std::move(lines));
    }
    if (Flag("traceLog") && result.is_dict()) {
      if (const base::DictValue* point = params_.FindDict("__inputPoint"))
        result.GetDict().Set("__input", point->Clone());
      if (const base::Value* snapshot = params_.Find("__actionSnapshot"))
        result.GetDict().Set("__actionSnapshot", snapshot->Clone());
    }
    const std::string json = Json(result);
    if (func_)
      func_(view_, json.c_str(), nullptr, data_);
    else if (pixels_func_)
      pixels_func_(view_, nullptr, 0, 0, 0, "zebkit: no pixels", data_);
    Release();
  }

  // zebkit_automation_screenshot_pixels()'s: the answer is pixels, not JSON.
  void set_pixels_func(zebkit_automation_pixels_func func) { pixels_func_ = func; }
  bool WantsPixels() const { return pixels_func_ != nullptr; }
  // @pixels, RGBA 8-bit unpremultiplied, rows of @stride bytes, for the
  // function's call only.
  void DonePixels(const uint8_t* pixels, int width, int height, int stride) {
    if (finished_)
      return;
    finished_ = true;
    timer_.Stop();
    if (pixels_func_)
      pixels_func_(view_, pixels, width, height, stride, nullptr, data_);
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
    // What a polled function logged before its timeout comes first.
    if (Name() == "waitForFunction" && Defer()) {
      deferred_error_ = message;
      return;
    }
    finished_ = true;
    error_ = message;
    timer_.Stop();
    std::string text = message;
    if (!log_.empty()) {
      text += "\nCall log:";
      for (const std::string& line : CompressCallLog(log_))
        text += "\n" + line;
    }
    if (func_)
      func_(view_, nullptr, text.c_str(), data_);
    else if (pixels_func_)
      pixels_func_(view_, nullptr, 0, 0, 0, text.c_str(), data_);
    Release();
  }

  void TimedOut() {
    // The page's own end comes first when it has one (a polled function
    // stops there, frames.ts's aborted).
    if (grace_ms_ > 0) {
      timer_.Start(FROM_HERE, base::Milliseconds(std::exchange(grace_ms_, 0)),
                   base::BindOnce(&Call::TimedOut, base::Unretained(this)));
      return;
    }
    if (on_timeout_) {
      std::move(on_timeout_).Run();
      return;
    }
    TimedOutNow();
  }
  // Run at the timeout instead of failing with it: the call answers itself.
  void OnTimeout(base::OnceClosure closure) { on_timeout_ = std::move(closure); }
  const std::vector<std::string>& log() const { return log_; }
  int timeout_ms() const { return timeout_ms_; }
  void TimedOutNow() {
    Fail("Timeout " + base::NumberToString(timeout_ms_) + "ms exceeded.");
  }
  // How long the page may take past the timeout to answer it.
  void set_grace(int ms) { grace_ms_ = ms; }

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
  // An evaluation's answer waits for the console messages the page made
  // before it, as CDP's events come before its answers (crPage.ts).
  bool Defer() {
    // A navigation's: after the messages already told, which its load
    // follows.
    const std::string name = Name();
    // An action's too: what its events' handlers logged comes first, as
    // Input.dispatchMouseEvent answers after them.
    static const base::NoDestructor<std::set<std::string>> kActions(std::set<std::string>{
        "click", "dblclick", "tap", "check", "uncheck", "press", "dispatchEvent"});
    const bool evaluation = name.find("valuate") != std::string::npos ||
                            name == "waitForFunction" || kActions->contains(name);
    if (deferring_ || (!evaluation && name != "goto" && name != "reload" && name != "setContent"))
      return false;
    deferring_ = true;
    return AnswerAfterConsole(view_, evaluation, base::BindRepeating(&Call::Deferred, GetWeakPtr()));
  }
  void Deferred() {
    if (finished_)
      return;
    if (deferred_error_)
      Fail(*deferred_error_);
    else
      Done(deferred_result_ ? std::move(*deferred_result_) : base::Value(base::DictValue()));
  }

  void Release() {
    if (!abort_id_.empty()) {
      auto [first, last] = AbortableCalls().equal_range(abort_id_);
      for (auto it = first; it != last; ++it) {
        if (it->second == this) {
          AbortableCalls().erase(it);
          break;
        }
      }
      abort_id_.clear();
    }
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
  zebkit_automation_pixels_func pixels_func_ = nullptr;
  raw_ptr<void> data_;
  // A call that waits for long logs a line every half second at most.
  static constexpr size_t kMaxLogLines = 2000;
  int timeout_ms_ = kDefaultTimeoutMs;
  base::TimeTicks deadline_;
  base::OneShotTimer timer_;
  std::vector<std::string> log_;
  std::vector<base::TimeTicks> log_times_;
  bool finished_ = false;
  bool deferring_ = false;
  int grace_ms_ = 0;
  std::optional<base::Value> deferred_result_;
  std::optional<std::string> deferred_error_;
  std::string error_;
  std::string abort_id_;
  base::OnceClosure on_release_;
  base::OnceClosure on_timeout_;
  base::WeakPtrFactory<Call> weak_factory_{this};
};

// Automation.abort {abortId, reason}: the call fails with progress.ts's
// "The operation was aborted" in its log.
void AbortCall(Call* call) {
  std::vector<base::WeakPtr<Call>> aborted;
  auto [first, last] = AbortableCalls().equal_range(call->String("abortId"));
  for (auto it = first; it != last; ++it)
    aborted.push_back(it->second->GetWeakPtr());
  const std::string reason = call->String("reason");
  for (const base::WeakPtr<Call>& each : aborted) {
    if (!each)
      continue;
    each->Log("operation was aborted" + (reason.empty() ? std::string() : ": " + reason));
    each->Fail(reason.empty() ? "The operation was aborted" : reason);
  }
  call->Done();
}

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
  // frameLocator() found nothing yet: a step waits this long at most, after
  // which the frames are searched again (Call::any_frame_retry).
  int cap_ms = 0;
  // Entered through an <iframe> that is hidden: its elements are too
  // (frameSelectors.ts's frameVisible).
  bool frame_visible = true;
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
// Chromium holds a window.open() popup for its DevTools client while a browser
// session auto-attaches (TargetHandler::ShouldThrottlePopups()), as with
// Playwright's own: the opener's script waits in window.open() until the
// popup's page target is released. This session attaches to nothing itself,
// and auto-attaches only from an automated opener's allowed window.open() to
// its popup's view (zebkit::PopupHoldHook()), since a browser session that
// stays would change how every new page starts; each popup's view holds and
// releases its own target (zebkit_view::HoldStart()), after its automation
// has applied the context (init scripts, viewport).
class PopupHold : public content::DevToolsAgentHostClient {
 public:
  static void Install() { zebkit::PopupHoldHook() = &PopupHold::Hook; }

  static void Hook(content::WebContents* opener, bool hold);

  void Set(bool hold) {
    if (hold == armed_)
      return;
    if (!host_) {
      host_ = content::DevToolsAgentHost::CreateForBrowser(nullptr, {});
      if (!host_ || !host_->AttachClient(this)) {
        host_ = nullptr;
        return;
      }
    }
    base::DictValue params;
    params.Set("autoAttach", hold);
    params.Set("waitForDebuggerOnStart", false);
    params.Set("flatten", true);
    base::ListValue filter;
    base::DictValue none;
    none.Set("exclude", true);
    filter.Append(std::move(none));
    params.Set("filter", std::move(filter));
    base::DictValue message;
    message.Set("id", ++next_id_);
    message.Set("method", "Target.setAutoAttach");
    message.Set("params", std::move(params));
    const std::string json = Json(base::Value(std::move(message)));
    host_->DispatchProtocolMessage(this, base::as_byte_span(json));
    armed_ = hold;
    zebkit::PopupStartsHeld() = hold;
    // Not kept between popups: a browser session attached changes what the
    // pages' own sessions are told (their WebSockets' events stopped).
    if (!hold && host_) {
      host_->DetachClient(this);
      host_ = nullptr;
    }
  }

  static PopupHold& Get() {
    static base::NoDestructor<PopupHold> hold;
    return *hold;
  }

  // content::DevToolsAgentHostClient
  void DispatchProtocolMessage(content::DevToolsAgentHost*,
                               base::span<const uint8_t>) override {}
  void AgentHostClosed(content::DevToolsAgentHost*) override {
    armed_ = false;
    zebkit::PopupStartsHeld() = false;
    host_ = nullptr;
  }

 private:
  scoped_refptr<content::DevToolsAgentHost> host_;
  bool armed_ = false;
  int next_id_ = 0;
};

struct ContextState {
  base::DictValue options;
  std::optional<content::GeolocationOverride> geolocation;
  base::ListValue credentials;
  base::ListValue extra_headers;
  bool offline = false;
  // Commands of the context's for each of its pages, new ones too
  // (addInitScript, exposeBinding, routes, subscriptions, clock): their
  // page's method and parameters.
  std::vector<std::pair<std::string, base::DictValue>> replay;
  // Origins whose storage was given to the context (storageState), which
  // storageState() visits when no page shows them (browserContext.ts's
  // _origins).
  std::set<std::string> storage_origins;
  // Its virtual WebAuthn authenticator's passkeys (credentials.ts's
  // registry, in their order), and whether pages use it (install()).
  std::vector<Passkey> passkeys;
  bool webauthn = false;
  // selectors.ts's custom engines ({name, source, contentScript}) and the
  // test id attribute, for the injected script of each document.
  base::ListValue selector_engines;
  std::string test_id_attribute;
  // The origins its frames have shown (browserContext.ts's _origins).
  std::set<std::string> visited_origins;
};

base::ListValue PasskeysOf(const ContextState& state) {
  base::ListValue list;
  for (const Passkey& passkey : state.passkeys)
    list.Append(PasskeyValue(passkey));
  return list;
}

// credentials.ts's create(): a passkey of the context's, one of the same id
// replaced.
bool AddPasskey(ContextState& state, const base::DictValue& options, base::DictValue* made, std::string* error) {
  Passkey passkey;
  if (!CreatePasskey(options, &passkey, error))
    return false;
  std::erase_if(state.passkeys, [&](const Passkey& other) { return other.id == passkey.id; });
  if (made)
    *made = PasskeyValue(passkey);
  state.passkeys.push_back(std::move(passkey));
  return true;
}

// Each of @origins in a page of @profile no client sees, whose every request
// is answered with an empty document, as browserContext.ts's visitOrigins():
// @expression evaluated there (made for the origin's entry), the values given
// to @done by origin.
using OriginValues = std::vector<std::pair<std::string, base::Value>>;
void VisitOrigins(content::BrowserContext* profile,
                  std::vector<base::DictValue> origins,
                  base::RepeatingCallback<std::string(const base::DictValue&)> expression,
                  base::OnceCallback<void(OriginValues)> done);
std::string StorageScriptCall(const std::string& call);
zebkit_context* RegisteredContext(const std::string& id);
std::string ContextIdOfProfile(content::BrowserContext* profile);

std::map<content::BrowserContext*, ContextState>& ContextStates() {
  static base::NoDestructor<std::map<content::BrowserContext*, ContextState>>
      states;
  return *states;
}

// A view's automation: its page's agents and handles, the keyboard's and
// the mouse's state, the navigations commands wait for, and its events.
// BrowserContext.restoreStorage (below).
void RestoreStorage(Call* call);

// A profile's service workers as Playwright's Worker objects (below).
void WatchServiceWorkers(content::BrowserContext* profile);
void ServiceWorkerTargets_Watch(content::BrowserContext* profile);
void UpdateServiceWorkers(content::BrowserContext* profile);
bool ServiceWorkerWatch_Running(content::BrowserContext* profile);

class Automation : public content::WebContentsObserver,
                   public content::WebContentsUserData<Automation>,
                   public mojom::AutomationHost,
                   public content::RenderWidgetHost::InputEventObserver,
                   public content::RenderWidgetHostObserver {
 private:
  // Dedicated workers' own DevTools sessions (crPage.ts's _workerSessions),
  // by session; each matched with its worker of workers_ by URL, in order.
  struct DevToolsWorker {
    std::string target;
    std::string url;
    std::string id;  // of workers_, once matched
    int context = 0;
    bool script_loaded = false;
    std::vector<base::OnceClosure> waiting;  // for its script
    std::vector<base::DictValue> console;    // until it is matched
  };
  // Handles of the workers' objects: CDP's remote objects.
  struct CdpHandle {
    std::string session;
    std::string object;  // objectId; none for a primitive
    base::Value value;   // a primitive's, in the agent's serialized form
  };

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

  // The context's exposed functions (BrowserContext.exposeBinding), which a
  // popup has from its first script on.
  std::vector<std::string> ContextBindings() {
    std::vector<std::string> names;
    for (const auto& [method, params] : Context().replay) {
      const std::string* name = params.FindString("name");
      if (method == "Page.exposeBinding" && name)
        names.push_back(*name);
    }
    return names;
  }

  // navigator.onLine and the network's offline state, as crNetworkManager.ts
  // emulates it on each page's session (Network.emulateNetworkConditions).
  void EmulateOffline(bool offline) {
    if (Network* network = Network::For(web_contents()))
      network->SetOffline(offline);
    if (!view_ || (!offline && !emulated_offline_))
      return;
    emulated_offline_ = offline;
    view_->Send("Network.emulateNetworkConditions", NetworkConditions());
    for (const std::string& session : frame_sessions_)
      view_->SendToSession(session, "Network.emulateNetworkConditions", NetworkConditions());
  }

  base::DictValue NetworkConditions() const {
    base::DictValue params;
    params.Set("offline", emulated_offline_);
    params.Set("latency", 0);
    params.Set("downloadThroughput", -1);
    params.Set("uploadThroughput", -1);
    return params;
  }

  // What crPage.ts's FrameSession sends an other site's frame's session for
  // the context's options before the frame runs; the rest (the viewport,
  // media, geolocation, headers) is the page's and reaches its frames.
  void InitializeFrameSession(const std::string& session) {
    frame_sessions_.insert(session);
    const base::DictValue& options = ContextStates()[web_contents()->GetBrowserContext()].options;
    auto send = [&](const char* method, base::DictValue params) {
      view_->SendToSession(session, method, std::move(params));
    };
    if (options.FindBool("bypassCSP").value_or(false)) {
      base::DictValue params;
      params.Set("enabled", true);
      send("Page.setBypassCSP", std::move(params));
    }
    if (options.FindBool("ignoreHTTPSErrors").value_or(false)) {
      base::DictValue params;
      params.Set("ignore", true);
      send("Security.setIgnoreCertificateErrors", std::move(params));
    }
    if (options.FindBool("hasTouch").value_or(false)) {
      base::DictValue params;
      params.Set("enabled", true);
      send("Emulation.setTouchEmulationEnabled", std::move(params));
    }
    if (options.FindBool("javaScriptEnabled") == false) {
      base::DictValue params;
      params.Set("value", true);
      send("Emulation.setScriptExecutionDisabled", std::move(params));
    }
    if (const std::string* locale = options.FindString("locale")) {
      base::DictValue params;
      params.Set("locale", *locale);
      send("Emulation.setLocaleOverride", std::move(params));
    }
    if (const std::string* timezone = options.FindString("timezoneId")) {
      base::DictValue params;
      params.Set("timezoneId", *timezone);
      send("Emulation.setTimezoneOverride", std::move(params));
    }
    if (emulated_offline_)
      send("Network.emulateNetworkConditions", NetworkConditions());
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
    PopupHold::Install();
    WatchServiceWorkers(web_contents()->GetBrowserContext());
    ServiceWorkerTargets_Watch(web_contents()->GetBrowserContext());
    MainFrame()->ForEachRenderFrameHost([this](content::RenderFrameHost* frame) {
      if (frame->IsRenderFrameLive())
        content::UpdateSubresourceLoaderFactories(frame);
      // What it loaded before is not seen: idle from now on unless it asks.
      StartIdleTimer(frame->GetFrameTreeNodeId());
    });
    ApplyContext();
    WatchDevToolsNetwork();
  }

  // The page's DevTools session's Network domain, for what only Blink and the
  // network's own observer know: XHR and fetch() apart, and the headers as the
  // network sent and received them (Cookie, Set-Cookie), as Playwright has
  // them from CDP.
  void WatchDevToolsNetwork() {
    if (!view_)
      return;
    view_->SetProtocolEvents(base::BindRepeating(&Automation::DevToolsEvent,
                                                 weak_factory_.GetWeakPtr()));
    // Other sites' frames as sessions of their own, as crPage.ts has them,
    // in a context of Browser.newContext's (the endpoint's), not on the
    // native API's own pages: there an attached frame's geolocation was
    // never answered (tests/engine/permission_hooks.py's frame denial).
    if (!ContextStates()[web_contents()->GetBrowserContext()].options.empty())
      view_->AttachFrames();
    view_->Send("Network.enable", base::DictValue());
    // The page's uncaught exceptions as crPage.ts has them (Runtime.exceptionThrown):
    // the thrown value's description, name and stack.
    view_->Send("Runtime.enable", base::DictValue());
    // What the browser itself logs (Log.entryAdded), as crPage.ts has it.
    view_->Send("Log.enable", base::DictValue());
    runtime_exceptions_ = true;
  }

  // ---- Coverage (Page.startJSCoverage, startCSSCoverage) ---------------
  // crCoverage.ts on the page's DevTools session: V8's precise coverage of
  // the scripts parsed meanwhile, and the style rules Blink used.

  struct JsCoverage {
    bool reset = true;
    bool anonymous = false;
    std::set<std::string> scripts;
    std::map<std::string, std::string> sources;
  };
  struct CssCoverage {
    bool reset = true;
    // Each sheet's URL and text, by its id.
    std::map<std::string, std::pair<std::string, std::string>> sheets;
  };

  void CoverageEvent(const std::string& method, const base::DictValue& params) {
    if (method == "Debugger.paused") {
      if (js_coverage_)
        view_->Send("Debugger.resume", base::DictValue());
      return;
    }
    if (method == "Debugger.scriptParsed" && js_coverage_) {
      const std::string* id = params.FindString("scriptId");
      const std::string* url = params.FindString("url");
      if (!id)
        return;
      js_coverage_->scripts.insert(*id);
      if ((!url || url->empty()) && !js_coverage_->anonymous)
        return;
      base::DictValue source;
      source.Set("scriptId", *id);
      view_->Send("Debugger.getScriptSource", std::move(source),
                  base::BindOnce(
                      [](base::WeakPtr<Automation> self, std::string id, const base::DictValue* result,
                         const std::string&) {
                        const std::string* text = result ? result->FindString("scriptSource") : nullptr;
                        if (self && self->js_coverage_ && text)
                          self->js_coverage_->sources[id] = *text;
                      },
                      weak_factory_.GetWeakPtr(), *id));
      return;
    }
    if (method == "CSS.styleSheetAdded" && css_coverage_) {
      const std::string* id = params.FindStringByDottedPath("header.styleSheetId");
      const std::string* url = params.FindStringByDottedPath("header.sourceURL");
      if (!id || !url || url->empty())
        return;
      base::DictValue text;
      text.Set("styleSheetId", *id);
      view_->Send("CSS.getStyleSheetText", std::move(text),
                  base::BindOnce(
                      [](base::WeakPtr<Automation> self, std::string id, std::string url,
                         const base::DictValue* result, const std::string&) {
                        const std::string* text = result ? result->FindString("text") : nullptr;
                        if (self && self->css_coverage_ && text)
                          self->css_coverage_->sheets[id] = {url, *text};
                      },
                      weak_factory_.GetWeakPtr(), *id, *url));
    }
  }

  // The commands' answers in order: @done once the last has answered.
  void SendAll(std::vector<std::pair<std::string, base::DictValue>> commands, base::OnceClosure done) {
    auto left = std::make_shared<size_t>(commands.size());
    auto once = std::make_shared<base::OnceClosure>(std::move(done));
    if (commands.empty()) {
      std::move(*once).Run();
      return;
    }
    for (auto& [method, params] : commands) {
      view_->Send(method, std::move(params),
                  base::BindOnce(
                      [](std::shared_ptr<size_t> left, std::shared_ptr<base::OnceClosure> once,
                         const base::DictValue*, const std::string&) {
                        if (--*left == 0 && *once)
                          std::move(*once).Run();
                      },
                      left, once));
    }
  }

  void StartJSCoverage(Call* call) {
    if (js_coverage_) {
      call->Fail("JSCoverage is already enabled");
      return;
    }
    js_coverage_.emplace();
    js_coverage_->reset = call->params().FindBool("resetOnNavigation").value_or(true);
    js_coverage_->anonymous = call->Flag("reportAnonymousScripts");
    base::DictValue precise;
    precise.Set("callCount", true);
    precise.Set("detailed", true);
    base::DictValue skip;
    skip.Set("skip", true);
    std::vector<std::pair<std::string, base::DictValue>> commands;
    commands.emplace_back("Profiler.enable", base::DictValue());
    commands.emplace_back("Profiler.startPreciseCoverage", std::move(precise));
    commands.emplace_back("Debugger.enable", base::DictValue());
    commands.emplace_back("Debugger.setSkipAllPauses", std::move(skip));
    SendAll(std::move(commands), base::BindOnce(
                                     [](base::WeakPtr<Call> call) {
                                       if (call)
                                         call->Done();
                                     },
                                     call->GetWeakPtr()));
  }

  void StopJSCoverage(Call* call) {
    if (!js_coverage_) {
      base::DictValue answer;
      answer.Set("entries", base::ListValue());
      call->Done(base::Value(std::move(answer)));
      return;
    }
    view_->Send(
        "Profiler.takePreciseCoverage", base::DictValue(),
        base::BindOnce(
            [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call, const base::DictValue* result,
               const std::string& error) {
              if (!self || !call)
                return;
              std::optional<JsCoverage> coverage = std::move(self->js_coverage_);
              self->js_coverage_.reset();
              base::ListValue entries;
              const base::ListValue* given = result ? result->FindList("result") : nullptr;
              const base::ListValue empty;
              for (const base::Value& item : given ? *given : empty) {
                if (!item.is_dict() || !coverage)
                  continue;
                const std::string* id = item.GetDict().FindString("scriptId");
                const std::string* url = item.GetDict().FindString("url");
                if (!id || !coverage->scripts.contains(*id))
                  continue;
                if ((!url || url->empty()) && !coverage->anonymous)
                  continue;
                base::DictValue entry = item.GetDict().Clone();
                if (auto source = coverage->sources.find(*id); source != coverage->sources.end())
                  entry.Set("source", source->second);
                entries.Append(std::move(entry));
              }
              base::DictValue answer;
              answer.Set("entries", std::move(entries));
              call->Done(base::Value(std::move(answer)));
            },
            weak_factory_.GetWeakPtr(), call->GetWeakPtr()));
    view_->Send("Profiler.stopPreciseCoverage", base::DictValue());
    view_->Send("Profiler.disable", base::DictValue());
    view_->Send("Debugger.disable", base::DictValue());
  }

  void StartCSSCoverage(Call* call) {
    if (css_coverage_) {
      call->Fail("CSSCoverage is already enabled");
      return;
    }
    css_coverage_.emplace();
    css_coverage_->reset = call->params().FindBool("resetOnNavigation").value_or(true);
    std::vector<std::pair<std::string, base::DictValue>> commands;
    commands.emplace_back("DOM.enable", base::DictValue());
    commands.emplace_back("CSS.enable", base::DictValue());
    commands.emplace_back("CSS.startRuleUsageTracking", base::DictValue());
    SendAll(std::move(commands), base::BindOnce(
                                     [](base::WeakPtr<Call> call) {
                                       if (call)
                                         call->Done();
                                     },
                                     call->GetWeakPtr()));
  }

  // crCoverage.ts's convertToDisjointRanges(): the used parts of nested
  // rule ranges, as disjoint ones.
  static base::ListValue DisjointRanges(std::vector<std::tuple<int, int, int>> ranges) {
    struct Point {
      int offset;
      int type;  // 0 start, 1 end
      int start, end, count;
    };
    std::vector<Point> points;
    for (const auto& [start, end, count] : ranges) {
      points.push_back({start, 0, start, end, count});
      points.push_back({end, 1, start, end, count});
    }
    std::sort(points.begin(), points.end(), [](const Point& a, const Point& b) {
      if (a.offset != b.offset)
        return a.offset < b.offset;
      if (a.type != b.type)
        return a.type > b.type;
      const int a_length = a.end - a.start;
      const int b_length = b.end - b.start;
      if (a.type == 0)
        return a_length > b_length;
      return a_length < b_length;
    });
    std::vector<int> counts;
    std::vector<std::pair<int, int>> results;
    int last = 0;
    for (const Point& point : points) {
      if (!counts.empty() && last < point.offset && counts.back() > 0) {
        if (!results.empty() && results.back().second == last)
          results.back().second = point.offset;
        else
          results.emplace_back(last, point.offset);
      }
      last = point.offset;
      if (point.type == 0)
        counts.push_back(point.count);
      else if (!counts.empty())
        counts.pop_back();
    }
    base::ListValue list;
    for (const auto& [start, end] : results) {
      if (end - start <= 1)
        continue;
      base::DictValue range;
      range.Set("start", start);
      range.Set("end", end);
      list.Append(std::move(range));
    }
    return list;
  }

  void StopCSSCoverage(Call* call) {
    if (!css_coverage_) {
      base::DictValue answer;
      answer.Set("entries", base::ListValue());
      call->Done(base::Value(std::move(answer)));
      return;
    }
    view_->Send(
        "CSS.stopRuleUsageTracking", base::DictValue(),
        base::BindOnce(
            [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call, const base::DictValue* result,
               const std::string& error) {
              if (!self || !call)
                return;
              std::optional<CssCoverage> coverage = std::move(self->css_coverage_);
              self->css_coverage_.reset();
              self->view_->Send("CSS.disable", base::DictValue());
              self->view_->Send("DOM.disable", base::DictValue());
              std::map<std::string, std::vector<std::tuple<int, int, int>>> by_sheet;
              const base::ListValue* usage = result ? result->FindList("ruleUsage") : nullptr;
              const base::ListValue empty;
              for (const base::Value& item : usage ? *usage : empty) {
                if (!item.is_dict())
                  continue;
                const std::string* id = item.GetDict().FindString("styleSheetId");
                if (!id)
                  continue;
                by_sheet[*id].emplace_back(
                    static_cast<int>(item.GetDict().FindDouble("startOffset").value_or(0)),
                    static_cast<int>(item.GetDict().FindDouble("endOffset").value_or(0)),
                    item.GetDict().FindBool("used").value_or(false) ? 1 : 0);
              }
              base::ListValue entries;
              if (coverage) {
                for (const auto& [id, sheet] : coverage->sheets) {
                  base::DictValue entry;
                  entry.Set("url", sheet.first);
                  entry.Set("text", sheet.second);
                  entry.Set("ranges", DisjointRanges(by_sheet[id]));
                  entries.Append(std::move(entry));
                }
              }
              base::DictValue answer;
              answer.Set("entries", std::move(entries));
              call->Done(base::Value(std::move(answer)));
            },
            weak_factory_.GetWeakPtr(), call->GetWeakPtr()));
  }

  // ---- Console (Runtime.consoleAPICalled, Log.entryAdded) --------------

  // crExecutionContext.ts's renderPreview().
  static std::string RenderPreview(const base::DictValue& object) {
    const std::string* type = object.FindString("type");
    if (type && *type == "undefined")
      return "undefined";
    if (const base::Value* value = object.Find("value")) {
      if (value->is_string())
        return value->GetString();
      if (value->is_none())
        return "null";
      if (value->is_bool())
        return value->GetBool() ? "true" : "false";
      if (value->is_int())
        return base::NumberToString(value->GetInt());
      return Json(*value);
    }
    if (const std::string* unserializable = object.FindString("unserializableValue"))
      return *unserializable;
    const std::string* description = object.FindString("description");
    const std::string* subtype = object.FindString("subtype");
    const base::ListValue* properties = object.FindListByDottedPath("preview.properties");
    if (description && *description == "Object" && properties) {
      std::vector<std::string> tokens;
      for (const base::Value& property : *properties) {
        const std::string* name = property.GetDict().FindString("name");
        const std::string* value = property.GetDict().FindString("value");
        tokens.push_back((name ? *name : std::string()) + ": " +
                         (value ? *value : std::string("undefined")));
      }
      return "{" + base::JoinString(tokens, ", ") + "}";
    }
    if (subtype && *subtype == "array" && properties) {
      // javascript.ts's sparseArrayToString().
      std::vector<std::pair<int, std::string>> entries;
      for (const base::Value& property : *properties) {
        const std::string* name = property.GetDict().FindString("name");
        const std::string* value = property.GetDict().FindString("value");
        int index = 0;
        if (!name || !base::StringToInt(*name, &index) || index < 0)
          continue;
        entries.emplace_back(index, value ? *value : std::string("undefined"));
      }
      std::sort(entries.begin(), entries.end());
      std::vector<std::string> tokens;
      int last = -1;
      for (const auto& [index, value] : entries) {
        const int empty = index - last - 1;
        if (empty == 1)
          tokens.push_back("empty");
        else if (empty > 1)
          tokens.push_back("empty x " + base::NumberToString(empty));
        tokens.push_back(value);
        last = index;
      }
      return "[" + base::JoinString(tokens, ", ") + "]";
    }
    return description ? *description : std::string();
  }

  content::RenderFrameHost* FrameOfDevToolsToken(const std::string& token) {
    content::RenderFrameHost* found = nullptr;
    MainFrame()->ForEachRenderFrameHost([&](content::RenderFrameHost* frame) {
      if (!found && frame->GetDevToolsFrameToken().ToString() == token)
        found = frame;
    });
    return found;
  }

  // A console call of the page's world, its arguments as handles as
  // crPage.ts's _onConsoleAPI() has them: left on the global object for a
  // moment by Runtime.callFunctionOn, taken from there by the agent.
  void ConsoleAPICalled(const base::DictValue& params) {
    const int context = params.FindInt("executionContextId").value_or(0);
    auto known = devtools_contexts_.find(context);
    if (context == 0 || known == devtools_contexts_.end())
      return;
    content::RenderFrameHost* frame = FrameOfDevToolsToken(known->second);
    if (!frame)
      return;
    const std::string* type = params.FindString("type");
    base::DictValue message;
    message.Set("type", type ? *type : std::string("log"));
    std::vector<std::string> previews;
    base::ListValue call_args;
    const std::string key = "__zebkit_console_" + base::NumberToString(++next_console_) + "_" +
                            base::NumberToString(base::RandUint64());
    base::DictValue first;
    first.Set("value", key);
    call_args.Append(std::move(first));
    if (const base::ListValue* args = params.FindList("args")) {
      for (const base::Value& arg : *args) {
        if (!arg.is_dict())
          continue;
        const base::DictValue& object = arg.GetDict();
        previews.push_back(RenderPreview(object));
        base::DictValue call_arg;
        if (const std::string* id = object.FindString("objectId"))
          call_arg.Set("objectId", *id);
        else if (const std::string* unserializable = object.FindString("unserializableValue"))
          call_arg.Set("unserializableValue", *unserializable);
        else if (const base::Value* value = object.Find("value"))
          call_arg.Set("value", value->Clone());
        call_args.Append(std::move(call_arg));
      }
    }
    message.Set("text", base::JoinString(previews, " "));
    message.Set("frame", FrameId(frame));
    base::DictValue location;
    const base::ListValue* frames = params.FindListByDottedPath("stackTrace.callFrames");
    const base::DictValue* top =
        frames && !frames->empty() && frames->front().is_dict() ? &frames->front().GetDict() : nullptr;
    const std::string* url = top ? top->FindString("url") : nullptr;
    location.Set("url", url ? *url : std::string());
    location.Set("lineNumber", top ? top->FindInt("lineNumber").value_or(0) : 0);
    location.Set("columnNumber", top ? top->FindInt("columnNumber").value_or(0) : 0);
    message.Set("location", std::move(location));
    const uint64_t sequence = next_console_;
    console_queue_[sequence] = std::move(message);
    base::DictValue command;
    command.Set("executionContextId", context);
    command.Set("functionDeclaration",
                "function (key, ...args) { Object.defineProperty(globalThis, key, "
                "{ value: args, configurable: true }); }");
    command.Set("arguments", std::move(call_args));
    command.Set("silent", true);
    view_->Send("Runtime.callFunctionOn", std::move(command),
                base::BindOnce(&Automation::ConsoleArgsLeft, weak_factory_.GetWeakPtr(), sequence,
                               frame->GetGlobalId(), key));
  }

  void ConsoleArgsLeft(uint64_t sequence,
                       content::GlobalRenderFrameHostId frame_id,
                       std::string key,
                       const base::DictValue* result,
                       const std::string& error) {
    content::RenderFrameHost* frame = content::RenderFrameHost::FromID(frame_id);
    if (!error.empty() || !frame || !frame->IsRenderFrameLive()) {
      ConsoleReady(sequence, base::ListValue());
      return;
    }
    base::DictValue arg;
    arg.Set("key", key);
    Agent(frame)->Step(
        AutomationWorld::kMain, "consoleArgs", Json(base::Value(std::move(arg))), {},
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(
            base::BindOnce(
                [](base::WeakPtr<Automation> self, uint64_t sequence,
                   content::GlobalRenderFrameHostId frame_id, AutomationStatus status,
                   const std::string& result, std::vector<AutomationHandlePtr> kept) {
                  if (!self)
                    return;
                  std::optional<base::Value> answer = Parse(result);
                  base::Value* value = status == AutomationStatus::kOk && answer && answer->is_dict()
                                           ? answer->GetDict().Find("value")
                                           : nullptr;
                  if (!value || !value->is_list()) {
                    self->ConsoleReady(sequence, base::ListValue());
                    return;
                  }
                  base::Value exposed = self->Expose(content::RenderFrameHost::FromID(frame_id),
                                                     std::move(*value), kept);
                  self->ConsoleReady(sequence, exposed.is_list() ? std::move(exposed.GetList())
                                                                 : base::ListValue());
                },
                weak_factory_.GetWeakPtr(), sequence, frame_id),
            AutomationStatus::kGone, std::string(), std::vector<AutomationHandlePtr>()));
  }

  // @answer once the page's DevTools session has told what came before now
  // (a command's answer follows its events) and those messages are out.
  bool AfterConsole(bool barrier, base::RepeatingClosure answer) {
    if (!runtime_exceptions_ || !view_)
      return false;
    if (!barrier && console_queue_.empty())
      return false;
    // Whatever becomes of the session.
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(FROM_HERE, answer,
                                                                    base::Milliseconds(500));
    if (!barrier) {
      console_answers_.push_back(answer);
      return true;
    }
    view_->Send("Runtime.getIsolateId", base::DictValue(),
                base::BindOnce(
                    [](base::WeakPtr<Automation> self, base::RepeatingClosure answer,
                       const base::DictValue*, const std::string&) {
                      if (self && !self->console_queue_.empty())
                        self->console_answers_.push_back(answer);
                      else
                        answer.Run();
                    },
                    weak_factory_.GetWeakPtr(), answer));
    return true;
  }

  // Console messages in the order the page made them.
  void ConsoleReady(uint64_t sequence, base::ListValue args) {
    auto found = console_queue_.find(sequence);
    if (found == console_queue_.end())
      return;
    if (!found->second.FindString("event"))
      found->second.Set("args", std::move(args));
    found->second.Set("ready", true);
    while (!console_queue_.empty() &&
           console_queue_.begin()->second.FindBool("ready").value_or(false)) {
      base::DictValue message = std::move(console_queue_.begin()->second);
      console_queue_.erase(console_queue_.begin());
      message.Remove("ready");
      std::optional<std::string> event = message.Extract("event").and_then(
          [](base::Value value) -> std::optional<std::string> {
            return value.is_string() ? std::optional<std::string>(value.GetString()) : std::nullopt;
          });
      Emit(event ? *event : "BrowserContext.console", std::move(message));
    }
    if (console_queue_.empty()) {
      std::vector<base::RepeatingClosure> answers = std::move(console_answers_);
      for (base::RepeatingClosure& answer : answers)
        answer.Run();
    }
  }

  // crPage.ts's _onLogEntryAdded(): what the browser logs, without arguments.
  void LogEntryAdded(const base::DictValue& params) {
    const base::DictValue* entry = params.FindDict("entry");
    const std::string* source = entry ? entry->FindString("source") : nullptr;
    if (!entry)
      return;
    const std::string* level = entry->FindString("level");
    std::string type = level ? *level : std::string("info");
    // A dedicated worker's console, which its page shows too (the worker's
    // own target has it for Playwright).
    if (source && *source == "worker") {
      type = type == "verbose" ? "debug" : type == "info" ? "log" : type;
      // Its uncaught exception is the page's error (crPage.ts's workers'
      // Runtime.exceptionThrown).
      const std::string* message = entry->FindString("text");
      if (type == "error" && message && message->starts_with("Uncaught ")) {
        const std::string thrown = message->substr(9);
        const size_t colon = thrown.find(": ");
        const bool named = colon != std::string::npos && thrown.find(' ') == colon + 1 &&
                           thrown.substr(0, colon).ends_with("Error");
        base::DictValue error;
        error.Set("name", named ? thrown.substr(0, colon) : std::string("Error"));
        error.Set("message", named ? thrown.substr(colon + 2) : thrown);
        error.Set("stack", thrown);
        base::DictValue where;
        const std::string* url = entry->FindString("url");
        where.Set("url", url ? *url : std::string());
        where.Set("lineNumber", entry->FindInt("lineNumber").value_or(0));
        where.Set("columnNumber", 0);
        base::DictValue event;
        event.Set("error", std::move(error));
        event.Set("location", std::move(where));
        event.Set("ready", true);
        event.Set("event", "Page.pageError");
        console_queue_[++next_console_] = std::move(event);
        ConsoleReady(next_console_, base::ListValue());
        return;
      }
    }
    // A worker with a session of its own has its console there, with its
    // arguments (WorkerSessionEvent()).
    if (source && *source == "worker" && !devtools_workers_.empty()) {
      const std::string* worker = entry->FindString("workerId");
      if (!worker)
        return;
      for (const auto& [session, known] : devtools_workers_)
        if (known.target == *worker)
          return;
    }
    const std::string* text = entry->FindString("text");
    const std::string* url = entry->FindString("url");
    base::DictValue message;
    message.Set("type", type);
    message.Set("text", text ? *text : std::string());
    message.Set("frame", FrameId(MainFrame()));
    base::DictValue location;
    location.Set("url", url ? *url : std::string());
    location.Set("lineNumber", entry->FindInt("lineNumber").value_or(0));
    location.Set("columnNumber", 0);
    message.Set("location", std::move(location));
    message.Set("args", base::ListValue());
    message.Set("ready", true);
    console_queue_[++next_console_] = std::move(message);
    ConsoleReady(next_console_, base::ListValue());
  }

  static base::ListValue HeaderList(const base::DictValue* headers) {
    base::ListValue list;
    if (!headers)
      return list;
    for (auto [name, value] : *headers) {
      if (!value.is_string())
        continue;
      // CDP joins repeated headers with new lines (crNetworkManager.ts splits).
      for (const std::string& line : base::SplitString(value.GetString(), "\n", base::KEEP_WHITESPACE,
                                                        base::SPLIT_WANT_ALL)) {
        base::DictValue header;
        header.Set("name", name);
        header.Set("value", line);
        list.Append(std::move(header));
      }
    }
    return list;
  }

  // crProtocolHelper.ts's exceptionToError() and stackTraceToLocation().
  void ExceptionThrown(const base::DictValue& params) {
    const base::DictValue* details = params.FindDict("exceptionDetails");
    if (!details)
      return;
    std::string text;
    if (const base::DictValue* exception = details->FindDict("exception")) {
      const std::string* description = exception->FindString("description");
      const base::Value* value = exception->Find("value");
      text = description ? *description
                         : value ? (value->is_string() ? value->GetString() : Json(*value)) : "undefined";
    } else {
      const std::string* message = details->FindString("text");
      text = message ? *message : std::string();
      if (const base::ListValue* frames = details->FindListByDottedPath("stackTrace.callFrames")) {
        for (const base::Value& frame : *frames) {
          const base::DictValue& call = frame.GetDict();
          const std::string* url = call.FindString("url");
          const std::string* function = call.FindString("functionName");
          text += "\n    at " + (function && !function->empty() ? *function : std::string("<anonymous>")) +
                  " (" + (url ? *url : std::string()) + ":" +
                  base::NumberToString(call.FindInt("lineNumber").value_or(0)) + ":" +
                  base::NumberToString(call.FindInt("columnNumber").value_or(0)) + ")";
        }
      }
    }
    std::vector<std::string> lines = base::SplitString(text, "\n", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
    size_t first_frame = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
      if (lines[i].starts_with("    at")) {
        first_frame = i;
        break;
      }
    }
    std::string with_name, stack;
    if (first_frame == lines.size()) {
      with_name = text;
    } else {
      with_name = base::JoinString(base::span(lines).first(first_frame), "\n");
      stack = text;
    }
    const size_t colon = with_name.find(':');
    std::string name = colon != std::string::npos ? with_name.substr(0, colon) : std::string();
    const std::string message = colon != std::string::npos && colon + 2 <= with_name.size()
                                    ? with_name.substr(colon + 2)
                                    : with_name;
    if (const base::ListValue* properties = details->FindListByDottedPath("exception.preview.properties")) {
      for (const base::Value& property : *properties) {
        const std::string* key = property.GetDict().FindString("name");
        if (key && *key == "name") {
          const std::string* value = property.GetDict().FindString("value");
          name = value ? *value : "Error";
        }
      }
    }
    base::DictValue error;
    error.Set("name", name);
    error.Set("message", message);
    error.Set("stack", stack);
    base::DictValue where;
    const base::DictValue* top = nullptr;
    if (const base::ListValue* frames = details->FindListByDottedPath("stackTrace.callFrames");
        frames && !frames->empty()) {
      top = &frames->front().GetDict();
    }
    const std::string* url = top ? top->FindString("url") : nullptr;
    where.Set("url", url ? *url : std::string());
    where.Set("lineNumber", top ? top->FindInt("lineNumber").value_or(0) : 0);
    where.Set("columnNumber", top ? top->FindInt("columnNumber").value_or(0) : 0);
    base::DictValue event;
    event.Set("error", std::move(error));
    event.Set("location", std::move(where));
    // After the console messages before it.
    event.Set("ready", true);
    event.Set("event", "Page.pageError");
    console_queue_[++next_console_] = std::move(event);
    ConsoleReady(next_console_, base::ListValue());
  }

  void DevToolsEvent(const std::string& method, const base::DictValue& params,
                     const std::string& session) {
    if (method == "Target.attachedToTarget") {
      const std::string* attached = params.FindString("sessionId");
      const std::string* type = params.FindStringByDottedPath("targetInfo.type");
      if (attached && type && *type == "iframe")
        InitializeFrameSession(*attached);
      else if (attached && type && *type == "worker")
        WorkerAttached(*attached, params);
      return;
    }
    if (method == "Target.detachedFromTarget") {
      if (const std::string* detached = params.FindString("sessionId")) {
        frame_sessions_.erase(*detached);
        WorkerDetached(*detached);
      }
      return;
    }
    if (auto worker = devtools_workers_.find(session); worker != devtools_workers_.end()) {
      WorkerSessionEvent(worker->second, session, method, params);
      return;
    }
    if (method == "Runtime.exceptionThrown") {
      ExceptionThrown(params);
      return;
    }
    if (method == "Runtime.executionContextCreated") {
      // The page's worlds (not isolated ones): their frames.
      const base::DictValue* context = params.FindDict("context");
      const std::string* frame = context ? context->FindStringByDottedPath("auxData.frameId") : nullptr;
      if (frame && context->FindBoolByDottedPath("auxData.isDefault").value_or(false))
        devtools_contexts_[context->FindInt("id").value_or(0)] = *frame;
      return;
    }
    if (method == "Runtime.executionContextDestroyed") {
      devtools_contexts_.erase(params.FindInt("executionContextId").value_or(0));
      return;
    }
    if (method == "Runtime.executionContextsCleared") {
      devtools_contexts_.clear();
      // crCoverage.ts: a new document starts the coverage again.
      if (js_coverage_ && js_coverage_->reset) {
        js_coverage_->scripts.clear();
        js_coverage_->sources.clear();
      }
      if (css_coverage_ && css_coverage_->reset)
        css_coverage_->sheets.clear();
      return;
    }
    if (method == "Debugger.scriptParsed" || method == "Debugger.paused" ||
        method == "CSS.styleSheetAdded") {
      CoverageEvent(method, params);
      return;
    }
    if (method == "Page.fileChooserOpened") {
      const std::string* frame = params.FindString("frameId");
      const std::optional<int> node = params.FindInt("backendNodeId");
      const std::string* mode = params.FindString("mode");
      if (!frame || !node || !intercept_file_choosers_)
        return;
      base::DictValue event;
      const std::string id = "c" + base::NumberToString(++next_chooser_);
      // For FileChooser.setFiles (the C API's): DOM.setFileInputFiles on it.
      devtools_choosers_[id] = *node;
      if (devtools_choosers_.size() > 100)
        devtools_choosers_.erase(devtools_choosers_.begin());
      event.Set("fileChooser", id);
      event.Set("frame", FrameOfDevTools(*frame));
      event.Set("isMultiple", mode && *mode == "selectMultiple");
      event.Set("isDirectory", false);
      event.Set("backendNodeId", *node);
      Emit("Page.fileChooser", std::move(event));
      return;
    }
    if (method == "Runtime.consoleAPICalled") {
      ConsoleAPICalled(params);
      return;
    }
    if (method == "Log.entryAdded") {
      LogEntryAdded(params);
      return;
    }
    Network* network = Network::For(web_contents());
    const std::string* request = params.FindString("requestId");
    if (!network || !request)
      return;
    if (method == "Network.requestWillBeSent") {
      if (const std::string* type = params.FindString("type"))
        network->DevToolsType(*request, *type);
      // Kept for a request the page's service worker may answer, which the
      // network never sees.
      devtools_requests_[*request] = params.Clone();
      if (devtools_requests_.size() > 500)
        devtools_requests_.erase(devtools_requests_.begin());
    } else if (method == "Network.requestServedFromCache") {
      // Blink's memory cache, which no request leaves for (crNetworkManager.ts
      // reports it as any other).
      memory_cached_.insert(*request);
      if (memory_cached_.size() > 500)
        memory_cached_.erase(memory_cached_.begin());
    } else if (method == "Network.responseReceived") {
      const base::DictValue* response = params.FindDict("response");
      auto sent = devtools_requests_.find(*request);
      const bool from_memory = memory_cached_.erase(*request) > 0;
      // A blob: URL's, which Blink reads without the network (crNetworkManager.ts
      // reports it as any other, data: URLs apart).
      const std::string* response_url = response ? response->FindString("url") : nullptr;
      const bool from_blob = response_url && response_url->starts_with("blob:");
      if (response && (from_memory || from_blob || response->FindBool("fromServiceWorker").value_or(false)) &&
          sent != devtools_requests_.end() && !network->SawDevToolsRequest(*request)) {
        const base::DictValue& will = sent->second;
        base::DictValue value;
        const std::string* url = will.FindStringByDottedPath("request.url");
        value.Set("url", url ? GURL(*url).GetWithoutRef().spec() : std::string());
        const std::string* verb = will.FindStringByDottedPath("request.method");
        value.Set("method", verb ? *verb : "GET");
        value.Set("headers", HeaderList(will.FindDictByDottedPath("request.headers")));
        const std::string* type = will.FindString("type");
        value.Set("resourceType", type ? base::ToLowerASCII(*type) : "other");
        value.Set("isNavigationRequest", type && *type == "Document");
        if (const std::string* frame = will.FindString("frameId"))
          value.Set("frame", FrameOfDevTools(*frame));
        base::DictValue answer;
        answer.Set("url", value.FindString("url") ? *value.FindString("url") : std::string());
        answer.Set("status", response->FindInt("status").value_or(200));
        const std::string* text = response->FindString("statusText");
        answer.Set("statusText", text ? *text : "");
        answer.Set("headers", HeaderList(response->FindDict("headers")));
        const std::string* mime = response->FindString("mimeType");
        answer.Set("mimeType", mime ? *mime : "");
        base::DictValue timing;
        timing.Set("startTime", base::Time::Now().InMillisecondsFSinceUnixEpoch());
        for (const char* key : {"domainLookupStart", "domainLookupEnd", "connectStart", "connectEnd",
                                "secureConnectionStart", "requestStart", "responseStart"})
          timing.Set(key, -1);
        answer.Set("timing", std::move(timing));
        if (from_memory || from_blob)
          answer.Set("fromServiceWorker", false);
        network->ServedByWorker(*request, std::move(value), std::move(answer));
        worker_served_.insert(*request);
      }
    } else if (method == "Network.loadingFinished" || method == "Network.loadingFailed") {
      auto sent = devtools_requests_.find(*request);
      // Failed before the network saw it, as a service worker's rejected
      // respondWith(): a request that failed.
      if (method == "Network.loadingFailed" && sent != devtools_requests_.end() &&
          !worker_served_.contains(*request) && !network->SawDevToolsRequest(*request) &&
          ServiceWorkerControlled(sent->second)) {
        const base::DictValue& will = sent->second;
        base::DictValue value;
        const std::string* url = will.FindStringByDottedPath("request.url");
        value.Set("url", url ? GURL(*url).GetWithoutRef().spec() : std::string());
        const std::string* verb = will.FindStringByDottedPath("request.method");
        value.Set("method", verb ? *verb : "GET");
        value.Set("headers", HeaderList(will.FindDictByDottedPath("request.headers")));
        const std::string* type = will.FindString("type");
        value.Set("resourceType", type ? base::ToLowerASCII(*type) : "other");
        value.Set("isNavigationRequest", type && *type == "Document");
        if (const std::string* frame = will.FindString("frameId"))
          value.Set("frame", FrameOfDevTools(*frame));
        const std::string* text = params.FindString("errorText");
        network->ServedByWorkerFailed(*request, std::move(value),
                                      text && !text->empty() ? *text : "net::ERR_FAILED");
      }
      devtools_requests_.erase(*request);
      if (!worker_served_.erase(*request))
        return;
      if (method == "Network.loadingFailed") {
        const std::string* text = params.FindString("errorText");
        network->ServedByWorkerEnded(*request, std::nullopt, text && !text->empty() ? *text : "net::ERR_FAILED");
        return;
      }
      base::DictValue body_params;
      body_params.Set("requestId", *request);
      view_->Send("Network.getResponseBody", std::move(body_params),
                  base::BindOnce(
                      [](base::WeakPtr<Automation> self, std::string id, const base::DictValue* result,
                         const std::string& error) {
                        if (!self)
                          return;
                        Network* network = Network::For(self->web_contents());
                        if (!network)
                          return;
                        std::optional<std::string> body;
                        if (const std::string* text = result ? result->FindString("body") : nullptr) {
                          std::string decoded;
                          if (result->FindBool("base64Encoded").value_or(false)) {
                            if (base::Base64Decode(*text, &decoded))
                              body = decoded;
                          } else {
                            body = *text;
                          }
                        }
                        network->ServedByWorkerEnded(id, std::move(body), std::string());
                      },
                      weak_factory_.GetWeakPtr(), *request));
    } else if (method == "Network.requestWillBeSentExtraInfo") {
      network->DevToolsExtraInfo("request", *request, HeaderList(params.FindDict("headers")));
    } else if (method == "Network.responseReceivedExtraInfo") {
      network->DevToolsExtraInfo("response", *request, HeaderList(params.FindDict("headers")));
    } else if (method == "Network.webSocketCreated") {
      // A page's WebSockets, as crNetworkManager.ts makes Playwright's.
      base::DictValue socket;
      socket.Set("socket", "s" + *request);
      const std::string* url = params.FindString("url");
      socket.Set("url", url ? *url : std::string());
      Emit("Page.webSocket", std::move(socket));
    } else if (method == "Network.webSocketWillSendHandshakeRequest") {
      // For HAR (harTracer.ts's WebSocket Request event).
      base::DictValue handshake;
      handshake.Set("socket", "s" + *request);
      handshake.Set("headers", HeaderList(params.FindDictByDottedPath("request.headers")));
      Emit("WebSocket.request", std::move(handshake));
    } else if (method == "Network.webSocketHandshakeResponseReceived") {
      {
        base::DictValue handshake;
        handshake.Set("socket", "s" + *request);
        handshake.Set("status", params.FindIntByDottedPath("response.status").value_or(101));
        const std::string* status_text = params.FindStringByDottedPath("response.statusText");
        handshake.Set("statusText", status_text ? *status_text : std::string());
        handshake.Set("headers", HeaderList(params.FindDictByDottedPath("response.headers")));
        Emit("WebSocket.response", std::move(handshake));
      }
      const int status = params.FindIntByDottedPath("response.status").value_or(101);
      if (status >= 400) {
        const std::string* text = params.FindStringByDottedPath("response.statusText");
        base::DictValue error;
        error.Set("socket", "s" + *request);
        error.Set("error", (text ? *text : std::string()) + ": " + base::NumberToString(status));
        Emit("WebSocket.socketError", std::move(error));
      }
    } else if (method == "Network.webSocketFrameSent" || method == "Network.webSocketFrameReceived") {
      base::DictValue frame;
      frame.Set("socket", "s" + *request);
      frame.Set("opcode", params.FindIntByDottedPath("response.opcode").value_or(1));
      const std::string* data = params.FindStringByDottedPath("response.payloadData");
      frame.Set("data", data ? *data : std::string());
      Emit(method == "Network.webSocketFrameSent" ? "WebSocket.frameSent" : "WebSocket.frameReceived",
           std::move(frame));
    } else if (method == "Network.webSocketFrameError") {
      base::DictValue error;
      error.Set("socket", "s" + *request);
      const std::string* message = params.FindString("errorMessage");
      error.Set("error", message ? *message : std::string());
      Emit("WebSocket.socketError", std::move(error));
    } else if (method == "Network.webSocketClosed") {
      base::DictValue closed;
      closed.Set("socket", "s" + *request);
      Emit("WebSocket.close", std::move(closed));
    }
  }

  // The context's settings for this page (Browser.newContext's options).
  void ApplyContext() {
    ContextState& state = ContextStates()[web_contents()->GetBrowserContext()];
    if (state.geolocation)
      content::SetGeolocationOverride(web_contents(), state.geolocation);
    EmulateOffline(state.offline);
    ApplyExtraHeaders();
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
    // What crPage.ts sets on each page's session for the context's options.
    if (options.FindBool("ignoreHTTPSErrors").value_or(false)) {
      base::DictValue params;
      params.Set("ignore", true);
      view_->Send("Security.setIgnoreCertificateErrors", std::move(params));
    }
    if (options.FindBool("bypassCSP").value_or(false)) {
      base::DictValue params;
      params.Set("enabled", true);
      view_->Send("Page.setBypassCSP", std::move(params));
    }
    if (const std::string* timezone = options.FindString("timezoneId")) {
      base::DictValue params;
      params.Set("timezoneId", *timezone);
      view_->Send("Emulation.setTimezoneOverride", std::move(params));
    }
    if (const std::string* locale = options.FindString("locale")) {
      base::DictValue params;
      params.Set("locale", *locale);
      view_->Send("Emulation.setLocaleOverride", std::move(params));
      const std::string* agent = options.FindString("userAgent");
      base::DictValue language;
      language.Set("userAgent", agent ? *agent : content::GetContentClientForTesting()->browser()->GetUserAgent());
      language.Set("acceptLanguage", *locale);
      // Kept by the view for its frames' targets too (crPage.ts's
      // _updateUserAgent in each FrameSession).
      view_->SetUserAgentOverride(std::move(language));
    }
    // A context's of Browser.newContext (not the native API's own pages).
    if (!options.empty() && !options.FindBool("noDefaultViewport").value_or(false)) {
      const base::DictValue* screen = options.FindDict("screen");
      const int width = options.FindIntByDottedPath("viewport.width").value_or(1280);
      const int height = options.FindIntByDottedPath("viewport.height").value_or(720);
      EmulateMetrics(width, height, screen ? screen->FindInt("width").value_or(width) : width,
                     screen ? screen->FindInt("height").value_or(height) : height);
    }
    // crPage.ts: touch as a touchscreen's, (hover: none) and (pointer: coarse).
    if (options.FindBool("hasTouch").value_or(false))
      EmulateTouch();
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

  // Whether the context's hasTouch has Chromium's touch emulation on.
  bool TouchEmulated() {
    return ContextStates()[web_contents()->GetBrowserContext()].options.FindBool("hasTouch").value_or(false);
  }

  // The context's hasTouch (frames.ts's tap(), input.ts's Touchscreen); a
  // page of no context's (the native API's) has touch.
  bool HasTouch() {
    auto found = ContextStates().find(web_contents()->GetBrowserContext());
    if (found == ContextStates().end() || found->second.options.empty())
      return true;
    return found->second.options.FindBool("hasTouch").value_or(false);
  }

  // crPage.ts's _updateViewport(): the viewport and the screen as the page
  // sees them (window.screen, device-width), the view itself sized apart
  // (NewPage, SetViewportSize), with the scale factor of its own unless the
  // context gives one.
  void EmulateMetrics(int width, int height, int screen_width, int screen_height) {
    // A page without its view yet (a popup Chromium opened with OpenURL):
    // DevTools' emulation needs it, so it waits for the page's view.
    if (!web_contents()->GetRenderWidgetHostView()) {
      pending_metrics_ = std::array<int, 4>{width, height, screen_width, screen_height};
      return;
    }
    pending_metrics_.reset();
    const base::DictValue& options = ContextStates()[web_contents()->GetBrowserContext()].options;
    const bool mobile = options.FindBool("isMobile").value_or(false);
    const std::optional<double> scale = options.FindDouble("deviceScaleFactor");
    base::DictValue params;
    params.Set("width", width);
    params.Set("height", height);
    params.Set("deviceScaleFactor", scale.value_or(mobile ? 1 : 0));
    params.Set("mobile", mobile);
    params.Set("screenWidth", screen_width);
    params.Set("screenHeight", screen_height);
    base::DictValue orientation;
    const bool landscape = screen_width > screen_height;
    orientation.Set("angle", mobile && landscape ? 90 : 0);
    orientation.Set("type", !mobile || landscape ? "landscapePrimary" : "portraitPrimary");
    params.Set("screenOrientation", std::move(orientation));
    view_->Send("Emulation.setDeviceMetricsOverride", std::move(params));
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
    Context().credentials = HttpCredentialsOf(call->params());
    call->Done();
  }

  // BrowserContext.setOffline and setExtraHTTPHeaders: for every page of
  // the context.
  void ContextOffline(Call* call) {
    Context().offline = call->Flag("offline");
    for (Automation* page : InProfile(web_contents()->GetBrowserContext()))
      page->EmulateOffline(call->Flag("offline"));
    UpdateServiceWorkers(web_contents()->GetBrowserContext());
    call->Done();
  }

  void ContextExtraHeaders(Call* call) {
    const base::ListValue* headers = call->params().FindList("headers");
    Context().extra_headers = headers ? headers->Clone() : base::ListValue();
    for (Automation* page : InProfile(web_contents()->GetBrowserContext()))
      page->ApplyExtraHeaders();
    UpdateServiceWorkers(web_contents()->GetBrowserContext());
    call->Done();
  }

  // The context's extra headers and the page's, the page's winning by name
  // (network.ts's mergeHeaders()).
  void ApplyExtraHeaders() {
    Network* network = Network::For(web_contents());
    if (!network)
      return;
    base::ListValue merged;
    auto overridden = [this](const std::string& name) {
      for (const base::Value& header : page_extra_headers_) {
        const std::string* other = header.is_dict() ? header.GetDict().FindString("name") : nullptr;
        if (other && base::EqualsCaseInsensitiveASCII(*other, name))
          return true;
      }
      return false;
    };
    for (const base::Value& header : Context().extra_headers) {
      const std::string* name = header.is_dict() ? header.GetDict().FindString("name") : nullptr;
      if (name && !overridden(*name))
        merged.Append(header.Clone());
    }
    for (const base::Value& header : page_extra_headers_)
      merged.Append(header.Clone());
    // And the session's, which a WebSocket's handshake has from the network
    // (Network.setExtraHTTPHeaders, as Playwright sets them).
    if (view_) {
      base::DictValue headers;
      for (const base::Value& header : merged) {
        const std::string* name = header.GetDict().FindString("name");
        const std::string* value = header.GetDict().FindString("value");
        if (name && value)
          headers.Set(*name, *value);
      }
      base::DictValue params;
      params.Set("headers", std::move(headers));
      view_->Send("Network.setExtraHTTPHeaders", std::move(params));
    }
    network->SetExtraHeaders(merged);
  }

  // The page's own extra header @name, if it has one.
  const std::string* PageExtraHeader(const std::string& name) const {
    for (const base::Value& header : page_extra_headers_) {
      const std::string* other = header.is_dict() ? header.GetDict().FindString("name") : nullptr;
      if (other && base::EqualsCaseInsensitiveASCII(*other, name))
        return header.GetDict().FindString("value");
    }
    return nullptr;
  }

  // Frames on demand (docs/AUTOMATION.md 4.6): while a command runs on a
  // page without a display, its compositor makes frames as soon as they
  // are asked for instead of at the display's 60 Hz, so that what waits
  // for animation frames (stability, the hit-target check) waits what a
  // frame takes to make.
  void FastFrames(bool fast) {
#if defined(USE_AURA)
    if (!HasNoDisplay(web_contents()))
      return;
    aura::Window* window = web_contents()->GetNativeView();
    aura::WindowTreeHost* host = window ? window->GetHost() : nullptr;
    if (!host || !host->compositor())
      return;
    host->compositor()->SetDisplayVSyncParameters(
        base::TimeTicks::Now(),
        fast ? base::Milliseconds(1) : base::Hertz(60));
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
    if (active_calls_++ == 0) {
      slow_frames_timer_.Stop();
      FastFrames(true);
    }
    if (tracing_ && call->method().rfind("Tracing.", 0) != 0 && !call->Flag("internalCall"))
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
    // The display's rate again a little after the last command: one that
    // comes at once (a script's next) finds the frames still on demand, and
    // the frames the last one left the page to make (a screenshot's
    // emulation undone) are not made at 60 Hz under it.
    if (--active_calls_ == 0) {
      slow_frames_timer_.Start(FROM_HERE, base::Milliseconds(100),
                               base::BindOnce(&Automation::FastFrames, base::Unretained(this), false));
    }
  }

  void SetEventHandler(zebkit_automation_event_func func, void* data) {
    on_event_ = func;
    on_event_data_ = data;
  }

  // Whether the page is going: an agent's call it cuts short fails as
  // Playwright's do when their page closes, not as on a navigation.
  bool Closing() const { return !web_contents() || web_contents()->IsBeingDestroyed(); }

  void Emit(const std::string& event, base::DictValue params) {
    TrackRequests(event, params);
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

  // Whether the page has a service worker in charge, which may answer a
  // request the network never sees.
  bool ServiceWorkerControlled(const base::DictValue&) {
    return ServiceWorkerWatch_Running(web_contents()->GetBrowserContext());
  }

  // The frame DevTools calls @token (its frameId), as FrameId() calls it.
  std::string FrameOfDevTools(const std::string& token) {
    std::string found;
    MainFrame()->ForEachRenderFrameHost([&](content::RenderFrameHost* frame) {
      if (found.empty() && frame->GetDevToolsFrameToken().ToString() == token)
        found = FrameId(frame);
    });
    return found.empty() ? FrameId(MainFrame()) : found;
  }

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
    // inspector_page_agent.cc's: a frame without a name has its element's id,
    // when its element is in the same process.
    std::string name = frame->GetFrameName();
    if (content::RenderFrameHost* parent = frame->GetParent();
        name.empty() && parent && parent->GetProcess() == frame->GetProcess()) {
      if (std::optional<std::string> id =
              static_cast<content::RenderFrameHostImpl*>(frame)->frame_tree_node()->html_id()) {
        name = *id;
      }
    }
    value.Set("name", name);
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
                     const std::vector<std::string>& ids,
                     const std::string& frame_id = std::string()) {
    if (value.is_list()) {
      base::ListValue list;
      for (base::Value& item : value.GetList())
        list.Append(Expose(std::move(item), handles, ids, frame_id));
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
          // The handle's own frame, which may not be the call's.
          if (!frame_id.empty())
            handle.Set("handleFrame", frame_id);
          return base::Value(std::move(handle));
        }
      }
    }
    base::DictValue result;
    for (auto [key, item] : dict)
      result.Set(key, Expose(std::move(item), handles, ids, frame_id));
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
    return Expose(std::move(value), handles, ids, FrameId(frame));
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
      if (arg.FindBool("doneIfGone").value_or(false)) {
        call->Done();
        return;
      }
      if (RetryThroughFrames(call))
        return;
      call->Fail(handles.empty() ? kFrameDetached : kDetached);
      return;
    }
    int remaining = call->Remaining();
    if (std::optional<int> cap = arg.FindInt("timeoutCap")) {
      remaining = remaining > 0 ? std::min(remaining, *cap) : *cap;
      arg.Remove("timeoutCap");
    }
    arg.Set("timeout", remaining);
    arg.Set("frameSeq", FrameSeq(frame));
    // dom.ts's injectedScript(): the context's selector engines and test id
    // attribute; selectors.ts's parseSelector(): a selector of an engine of
    // the page's world is resolved there.
    if (std::optional<base::DictValue> selectors = SelectorOptions()) {
      const std::string* selector = arg.FindString("selector");
      if (world == AutomationWorld::kUtility && selector && NeedsMainWorld(*selector))
        world = AutomationWorld::kMain;
      arg.Set("selectorOptions", std::move(*selectors));
    }
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

  // The context's selector engines and test id attribute, for the injected
  // script (dom.ts's injectedScript()); nullopt when it has none of its own.
  std::optional<base::DictValue> SelectorOptions() {
    const ContextState& context = Context();
    if (context.selector_engines.empty() && context.test_id_attribute.empty())
      return std::nullopt;
    base::DictValue selectors;
    base::ListValue engines;
    for (const base::Value& engine : context.selector_engines) {
      const std::string* name = engine.GetDict().FindString("name");
      const std::string* source = engine.GetDict().FindString("source");
      if (!name || !source)
        continue;
      base::DictValue custom;
      custom.Set("name", *name);
      custom.Set("source", "(" + *source + ")");
      engines.Append(std::move(custom));
    }
    selectors.Set("customEngines", std::move(engines));
    if (!context.test_id_attribute.empty())
      selectors.Set("testIdAttributeName", context.test_id_attribute);
    return selectors;
  }

  // selectors.ts's parseSelector(): an engine of the page's own world.
  bool NeedsMainWorld(const std::string& selector) {
    for (const base::Value& engine : Context().selector_engines) {
      const std::string* name = engine.GetDict().FindString("name");
      if (name && !engine.GetDict().FindBool("contentScript").value_or(false) &&
          selector.find(*name + "=") != std::string::npos) {
        return true;
      }
    }
    return false;
  }

  // The options to @frame's runner in @world before a call that is not a step
  // (Select): the agent's calls keep their order.
  void SendSelectorOptions(content::RenderFrameHost* frame, AutomationWorld world) {
    std::optional<base::DictValue> selectors = SelectorOptions();
    if (!selectors)
      return;
    base::DictValue arg;
    arg.Set("selectorOptions", std::move(*selectors));
    arg.Set("frameSeq", FrameSeq(frame));
    Agent(frame)->Step(world, "noop", Json(base::Value(std::move(arg))), {},
                       base::BindOnce([](AutomationStatus, const std::string&, std::vector<AutomationHandlePtr>) {}));
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
      if (arg.FindBool("doneIfGone").value_or(false)) {
        call->Done();
        return;
      }
      // An assertion outlives a navigation, as frames.ts retries it.
      if (step == "expect" && handles.empty())
        frame_ref.follow = true;
      if (!handles.empty() || !frame_ref.follow) {
        if (!Closing() && RetryThroughFrames(call.get()))
          return;
        call->Fail(Closing() ? kClosed : handles.empty() ? kNavigated : kDetached);
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

  // frameSelectors.ts resolves a selector's frames again at each try: a
  // wait through a frame that went (detached, navigated) starts over from
  // the call's own frame, until its timeout.
  bool RetryThroughFrames(Call* call) {
    if ((call->method().rfind("Frame.", 0) != 0 && call->method() != "ElementHandle.waitForSelector") ||
        call->Expired() ||
        call->String("selector").find("internal:control=enter-frame") == std::string::npos) {
      return false;
    }
    auto handler = Handlers().find(call->method());
    if (handler == Handlers().end())
      return false;
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(
            [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call, Handler handler) {
              if (self && call && !call->finished())
                (self.get()->*handler)(call.get());
            },
            weak_factory_.GetWeakPtr(), call->GetWeakPtr(), handler->second),
        base::Milliseconds(100));
    return true;
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
                  AutomationWorld world = AutomationWorld::kUtility,
                  std::vector<int32_t> extra = {}) {
    arg.Set("selector", target.selector);
    arg.Set("strict", target.strict);
    if (target.cap_ms)
      arg.Set("timeoutCap", target.cap_ms);
    if (!target.frame_visible)
      arg.Set("frameVisible", false);
    // The call's own selector in its log, not the part left in this frame.
    if (const std::string* own = call->params().FindString("selector");
        own && *own != target.selector) {
      arg.Set("displaySelector", *own);
    }
    std::vector<int32_t> handles;
    if (target.scope) {
      base::DictValue scope;
      scope.Set("$h", 0);
      arg.Set("scope", std::move(scope));
      handles.push_back(target.scope);
    }
    // More of the call's own handles: arg's "handles", {"$h": i} each.
    if (!extra.empty()) {
      base::ListValue references;
      for (int32_t handle : extra) {
        base::DictValue reference;
        reference.Set("$h", static_cast<int>(handles.size()));
        references.Append(std::move(reference));
        handles.push_back(handle);
      }
      arg.Set("handles", std::move(references));
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
      if (inside && IsAnyFrame(selector)) {
        AnyFrameTarget(call, std::move(target), std::move(done));
        return;
      }
      // Into frames from the element (selectors-frame.spec.ts's handles).
      if (inside && selector.find(kEnterFrame) != std::string::npos) {
        AnyFrameContinue(call, std::move(target), std::move(done));
        return;
      }
      std::move(done).Run(std::move(target));
      return;
    }
    content::RenderFrameHost* frame = FrameOf(call);
    if (!frame) {
      call->Fail(kFrameDetached);
      return;
    }
    // frameSelectors.ts's _jumpToAriaRefFrameIfNeeded(): a ref of a frame's
    // snapshot (f<seq>e<n>) is looked for in that frame.
    if (selector.starts_with("aria-ref=f")) {
      const size_t end = selector.find_first_of(" >", 10);
      const std::string body = selector.substr(9, end == std::string::npos ? std::string::npos : end - 9);
      const size_t e = body.find('e');
      int seq = 0;
      if (e != std::string::npos && base::StringToInt(std::string_view(body).substr(1, e - 1), &seq)) {
        content::RenderFrameHost* found = nullptr;
        MainFrame()->ForEachRenderFrameHost([&](content::RenderFrameHost* each) {
          if (!found && each->IsActive() && FrameSeq(each) == seq)
            found = each;
        });
        if (!found) {
          call->Fail("Invalid frame in aria-ref selector \"" + selector + "\"");
          return;
        }
        frame = found;
      }
    }
    target.frame = Follow(frame);
    target.strict = call->Flag("strict");
    if (IsAnyFrame(selector)) {
      target.selector = selector;
      AnyFrameTarget(call, std::move(target), std::move(done));
      return;
    }
    // selectorParser.ts: frameLocator() only first, and not in an operand.
    if (const size_t at = selector.find(kAnyFrame); at != std::string::npos) {
      const bool nested = at > 0 && (selector[at - 1] == '"' || selector[at - 1] == '\'');
      call->Fail(nested ? "frameLocator() is not allowed inside composite locators, while querying \"" +
                              selector + "\""
                        : "\"any-frame\" is only allowed as the first selector token, while parsing selector " +
                              selector);
      return;
    }
    std::vector<std::string> chunks;
    size_t start = 0;
    for (size_t at;
         (at = selector.find(kEnterFrame, start)) != std::string::npos;) {
      chunks.push_back(selector.substr(start, at - start));
      start = at + sizeof(kEnterFrame) - 1;
    }
    chunks.push_back(selector.substr(start));
    // frameSelectors.ts's splitSelectorByFrame().
    if (selector.starts_with("internal:control=enter-frame >> ") ||
        selector == "internal:control=enter-frame") {
      call->Fail("Selector cannot start with entering frame, select the iframe first");
      return;
    }
    if (selector.ends_with(" >> internal:control=enter-frame")) {
      call->Fail("Selector cannot end with entering frame, while parsing selector " + selector);
      return;
    }
    for (size_t i = 0; i + 1 < chunks.size(); ++i) {
      if (chunks[i].starts_with("*") || chunks[i].find(" >> *") != std::string::npos) {
        call->Fail(
            "Can not capture the selector before diving into the frame. Only use * after the "
            "last frame has been selected");
        return;
      }
    }
    target.selector = chunks.back();
    chunks.pop_back();
    EnterFrames(call->GetWeakPtr(), std::move(target), std::move(chunks), 0,
                std::move(done));
  }

  // ---- frameLocator(): internal:control=any-frame ------------------------

  static bool IsAnyFrame(const std::string& selector) {
    return selector == kAnyFrame || selector.starts_with(std::string(kAnyFrame) + " >> ");
  }

  static std::string AnyFrameRest(const std::string& selector) {
    const size_t prefix = sizeof(kAnyFrame) - 1 + 4;
    return selector.size() > prefix ? selector.substr(prefix) : std::string();
  }

  static std::string AnyFrameLocatorText(const std::string& selector) {
    const std::string rest = AnyFrameRest(selector);
    if (rest.find(" >> ") == std::string::npos && rest.find("internal:") == std::string::npos &&
        rest.find('=') == std::string::npos) {
      return "frameLocator().locator('" + rest + "')";
    }
    return selector;
  }

  // frameSelectors.ts's splitSelectorByFrame() checks, then the frame of the
  // subtree in which the rest matches (resolveFramesForSelector()).
  void AnyFrameTarget(Call* call, Target target, TargetDone done) {
    const std::string selector = target.selector;
    const std::string rest = AnyFrameRest(selector);
    if (rest.empty()) {
      call->Fail("Selector cannot be empty after frameLocator(), while parsing selector " + selector);
      return;
    }
    // Inside a composite locator's operand (quoted), or again later.
    if (const size_t at = rest.find(kAnyFrame); at != std::string::npos) {
      const bool nested = at > 0 && (rest[at - 1] == '"' || rest[at - 1] == '\'');
      call->Fail(nested ? "frameLocator() is not allowed inside composite locators, while querying \"" +
                              AnyFrameLocatorText(selector) + "\""
                        : "\"any-frame\" is only allowed as the first selector token, while parsing selector " +
                              selector);
      return;
    }
    if (rest.starts_with("internal:control=enter-frame")) {
      call->Fail("Selector cannot start with entering frame, select the iframe first");
      return;
    }
    if (rest.ends_with(" >> internal:control=enter-frame")) {
      call->Fail("Selector cannot end with entering frame, while parsing selector " + selector);
      return;
    }
    call->Log("waiting for " + AnyFrameLocatorText(selector));
    target.selector = rest;
    AnyFrameSearch(call->GetWeakPtr(), std::move(target), std::move(done));
  }

  struct AnyFrameProbe {
    base::WeakPtr<Call> call;
    Target target;
    TargetDone done;
    std::vector<content::GlobalRenderFrameHostId> frames;
    std::vector<int> counts;
    std::string error;
    size_t pending = 0;
  };

  // Each frame of the subtree asked how many elements the selector's first
  // part has there; the one frame that has some is where it continues.
  void AnyFrameSearch(base::WeakPtr<Call> call, Target target, TargetDone done) {
    if (!call || call->finished())
      return;
    content::RenderFrameHost* start = Resolve(target.frame);
    if (!start) {
      call->Fail(kFrameDetached);
      return;
    }
    if (!target.scope) {
      AnyFrameProbeFrames(call, std::move(target), std::move(done), std::nullopt);
      return;
    }
    // Within a scope: the frames of the <iframe>s inside it, and theirs
    // (frameSelectors.ts's _anyFrameCandidates()).
    Target owners = target;
    owners.selector = "css=frame,iframe";
    owners.strict = false;
    TargetStep(call.get(), owners, "queryAll", {},
               base::BindOnce(&Automation::AnyFrameOwners, weak_factory_.GetWeakPtr(), call,
                              std::move(target), std::move(done)));
  }

  void AnyFrameOwners(base::WeakPtr<Call> call, Target target, TargetDone done, base::Value value) {
    if (!call || call->finished())
      return;
    struct Owners {
      base::WeakPtr<Call> call;
      Target target;
      TargetDone done;
      std::vector<content::GlobalRenderFrameHostId> roots;
      size_t left = 0;
    };
    auto owners = std::make_shared<Owners>();
    owners->call = call;
    owners->target = std::move(target);
    owners->done = std::move(done);
    std::vector<std::string> handles;
    if (value.is_list()) {
      for (const base::Value& item : value.GetList()) {
        const std::string* id = item.is_dict() ? item.GetDict().FindString("handle") : nullptr;
        if (id && FindHandle(*id))
          handles.push_back(*id);
      }
    }
    auto finish = [](base::WeakPtr<Automation> self, std::shared_ptr<Owners> owners) {
      if (self && owners->call)
        self->AnyFrameProbeFrames(owners->call, std::move(owners->target), std::move(owners->done),
                                  std::move(owners->roots));
    };
    if (handles.empty()) {
      finish(weak_factory_.GetWeakPtr(), owners);
      return;
    }
    owners->left = handles.size();
    for (const std::string& id : handles) {
      const HandleRef* handle = FindHandle(id);
      content::RenderFrameHost* frame = content::RenderFrameHost::FromID(handle->frame);
      const int32_t element = handle->id;
      if (!frame) {
        Dispose(id);
        if (--owners->left == 0)
          finish(weak_factory_.GetWeakPtr(), owners);
        continue;
      }
      Agent(frame)->ContentFrame(
          element, mojo::WrapCallbackWithDefaultInvokeIfNotRun(
                       base::BindOnce(
                           [](base::WeakPtr<Automation> self, std::shared_ptr<Owners> owners,
                              std::string id, decltype(finish) finish,
                              const std::vector<int32_t>& path) {
                             if (!self)
                               return;
                             self->Dispose(id);
                             if (content::RenderFrameHost* child = self->FrameAtPath(path))
                               owners->roots.push_back(child->GetGlobalId());
                             if (--owners->left == 0)
                               finish(self, owners);
                           },
                           weak_factory_.GetWeakPtr(), owners, id, finish),
                       std::vector<int32_t>{-1}));
    }
  }

  // The frames asked: the starting frame and the subtree, or within a scope
  // the starting frame and the subtrees of @roots.
  void AnyFrameProbeFrames(base::WeakPtr<Call> call,
                           Target target,
                           TargetDone done,
                           std::optional<std::vector<content::GlobalRenderFrameHostId>> roots) {
    if (!call || call->finished())
      return;
    content::RenderFrameHost* start = Resolve(target.frame);
    if (!start) {
      call->Fail(kFrameDetached);
      return;
    }
    auto probe = std::make_shared<AnyFrameProbe>();
    probe->call = call;
    probe->done = std::move(done);
    const size_t enter = target.selector.find(kEnterFrame);
    const std::string first =
        enter == std::string::npos ? target.selector : target.selector.substr(0, enter);
    probe->frames.push_back(start->GetGlobalId());
    auto add_subtree = [&](content::RenderFrameHost* root) {
      root->ForEachRenderFrameHost([&](content::RenderFrameHost* frame) {
        if (frame != start && frame->IsActive() && frame->IsRenderFrameLive())
          probe->frames.push_back(frame->GetGlobalId());
      });
    };
    if (!roots) {
      add_subtree(start);
    } else {
      for (const auto& id : *roots) {
        if (content::RenderFrameHost* root = content::RenderFrameHost::FromID(id))
          add_subtree(root);
      }
    }
    probe->counts.assign(probe->frames.size(), 0);
    probe->pending = probe->frames.size();
    const int32_t scope = target.scope;
    probe->target = std::move(target);
    const content::GlobalRenderFrameHostId start_id = start->GetGlobalId();
    const std::vector<content::GlobalRenderFrameHostId> frames = probe->frames;
    for (size_t i = 0; i < frames.size(); ++i) {
      content::RenderFrameHost* frame = content::RenderFrameHost::FromID(frames[i]);
      base::DictValue arg;
      arg.Set("selector", first);
      arg.Set("strict", false);
      arg.Set("timeout", 0);
      std::vector<int32_t> handles;
      if (frames[i] == start_id && scope) {
        base::DictValue scope_ref;
        scope_ref.Set("$h", 0);
        arg.Set("scope", std::move(scope_ref));
        handles.push_back(scope);
      }
      Agent(frame)->Step(
          AutomationWorld::kUtility, "queryCount", Json(base::Value(std::move(arg))), handles,
          mojo::WrapCallbackWithDefaultInvokeIfNotRun(
              base::BindOnce(&Automation::AnyFrameCounted, weak_factory_.GetWeakPtr(), probe, i,
                             frames[i] == start_id),
              AutomationStatus::kGone, std::string(), std::vector<AutomationHandlePtr>()));
    }
  }

  void AnyFrameCounted(std::shared_ptr<AnyFrameProbe> probe,
                       size_t index,
                       bool start,
                       AutomationStatus status,
                       const std::string& result,
                       std::vector<AutomationHandlePtr>) {
    if (status == AutomationStatus::kOk) {
      std::optional<base::Value> answer = Parse(result);
      const base::DictValue* dict = answer && answer->is_dict() ? &answer->GetDict() : nullptr;
      if (dict && dict->FindBool("ok").value_or(false)) {
        const base::Value* value = dict->Find("value");
        if (value && value->is_dict())
          value = value->GetDict().Find("value");
        if (value && value->is_int())
          probe->counts[index] = value->GetInt();
        else if (value && value->is_double())
          probe->counts[index] = static_cast<int>(value->GetDouble());
      } else if (dict && start) {
        const std::string* error = dict->FindString("error");
        probe->error = error ? *error : "Error";
      }
    }
    if (--probe->pending)
      return;
    Call* call = probe->call.get();
    if (!call || call->finished())
      return;
    if (!probe->error.empty()) {
      call->Fail(probe->error);
      return;
    }
    std::vector<size_t> matched;
    for (size_t i = 0; i < probe->counts.size(); ++i) {
      if (probe->counts[i] > 0)
        matched.push_back(i);
    }
    Target target = std::move(probe->target);
    if (matched.size() > 1) {
      // frames.ts's expect(): a non-recoverable error is the assertion's.
      if (call->Name() == "expect") {
        base::DictValue failed;
        failed.Set("matches", call->Flag("isNot"));
        failed.Set("errorMessage", "frameLocator() matched elements in multiple frames.");
        call->Done(base::Value(std::move(failed)));
        return;
      }
      call->Fail("frameLocator() matched elements in multiple frames.");
      return;
    }
    if (matched.size() == 1) {
      content::RenderFrameHost* frame = content::RenderFrameHost::FromID(probe->frames[matched[0]]);
      if (!frame) {
        call->Fail(kFrameDetached);
        return;
      }
      if (Resolve(target.frame) != frame)
        target.scope = 0;
      target.frame = Follow(frame);
      AnyFrameContinue(call, std::move(target), std::move(probe->done));
      return;
    }
    // Nowhere yet: an answer of what there is, an assertion for a while in the
    // starting frame, anything else once the frames have it.
    const std::string name = call->Name();
    if (name == "queryCount" || name == "querySelector" || name == "querySelectorAll" ||
        name == "evalOnSelectorAll" || name == "isVisible" || name == "isHidden" ||
        (name == "expect" && target.selector.find(kEnterFrame) == std::string::npos)) {
      if (name == "expect" && !call->Expired()) {
        target.cap_ms = 250;
        call->any_frame_retry = true;
      }
      AnyFrameContinue(call, std::move(target), std::move(probe->done));
      return;
    }
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&Automation::AnyFrameSearch, weak_factory_.GetWeakPtr(), probe->call,
                       std::move(target), std::move(probe->done)),
        base::Milliseconds(100));
  }

  // The rest, frames entered as without frameLocator().
  void AnyFrameContinue(Call* call, Target target, TargetDone done) {
    std::vector<std::string> chunks;
    const std::string selector = target.selector;
    size_t start = 0;
    for (size_t at; (at = selector.find(kEnterFrame, start)) != std::string::npos;) {
      chunks.push_back(selector.substr(start, at - start));
      start = at + sizeof(kEnterFrame) - 1;
    }
    chunks.push_back(selector.substr(start));
    target.selector = chunks.back();
    chunks.pop_back();
    EnterFrames(call->GetWeakPtr(), std::move(target), std::move(chunks), 0, std::move(done));
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
    // A query does not wait for the frame (frameSelectors.ts's noStall): no
    // <iframe> is no element.
    // An assertion, or a wait for none, treats no <iframe> as no element and
    // looks again, as frames.ts's expect() polls.
    const std::string name = call->Name();
    const std::string state = call->String("state");
    const bool expecting = name == "expect" ||
                           (name == "waitForSelector" && (state == "hidden" || state == "detached"));
    if (name == "querySelector" || name == "querySelectorAll" || name == "queryCount" ||
        name == "evalOnSelector" || name == "evalOnSelectorAll" || expecting ||
        call->Flag("__noFrameWait")) {
      TargetStep(call.get(), frame_target, "query", {},
                 base::BindOnce(
                     [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call, Target target,
                        std::vector<std::string> chunks, size_t index, TargetDone done,
                        base::Value value) {
                       if (!self || !call)
                         return;
                       if (!value.is_dict()) {
                         target.selector = "css=:not(*)";
                         if (call->Name() == "expect" && !call->Expired()) {
                           target.cap_ms = 250;
                           call->any_frame_retry = true;
                         }
                         std::move(done).Run(std::move(target));
                         return;
                       }
                       // Through a frame that may go or change: looked for
                       // again each while (frames.ts re-resolves each poll).
                       if (call->Name() == "expect" && !call->Expired()) {
                         target.cap_ms = 250;
                         call->any_frame_retry = true;
                       }
                       base::DictValue element;
                       element.Set("element", std::move(value));
                       base::DictValue wrapped;
                       wrapped.Set("result", std::move(element));
                       self->FrameElementFound(call, std::move(target), std::move(chunks), index,
                                               std::move(done), base::Value(std::move(wrapped)));
                     },
                     weak_factory_.GetWeakPtr(), call, std::move(target), std::move(chunks), index,
                     std::move(done)));
      return;
    }
    call->Log("waiting for " + LocatorText(chunks[index]));
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
    // Whether the <iframe> shows, for what is inside it.
    Target owner;
    owner.frame = Exact(parent);
    owner.selector = ":scope";
    owner.scope = handle->id;
    owner.frame_visible = target.frame_visible;
    TargetStep(call.get(), owner, "isVisible", {},
               base::BindOnce(&Automation::FrameElementShown, weak_factory_.GetWeakPtr(), call,
                              kept, std::move(target), std::move(chunks), index, std::move(done)));
  }

  void FrameElementShown(base::WeakPtr<Call> call,
                         std::string kept,
                         Target target,
                         std::vector<std::string> chunks,
                         size_t index,
                         TargetDone done,
                         base::Value visible) {
    if (!call)
      return;
    const HandleRef* handle = FindHandle(kept);
    content::RenderFrameHost* parent =
        handle ? content::RenderFrameHost::FromID(handle->frame) : nullptr;
    if (!parent) {
      call->Fail(kFrameDetached);
      return;
    }
    if (visible.is_bool() && !visible.GetBool())
      target.frame_visible = false;
    Agent(parent)->ContentFrame(
        handle->id,
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(
            base::BindOnce(&Automation::FrameEntered,
                           weak_factory_.GetWeakPtr(), call, handle->frame,
                           kept, std::move(target), std::move(chunks), index,
                           std::move(done)),
            std::vector<int32_t>{-1}));
  }

  void FrameEntered(base::WeakPtr<Call> call,
                    content::GlobalRenderFrameHostId parent_id,
                    std::string kept,
                    Target target,
                    std::vector<std::string> chunks,
                    size_t index,
                    TargetDone done,
                    const std::vector<int32_t>& path) {
    // As frameSelectors.ts says it.
    const HandleRef* element = FindHandle(kept);
    const std::string preview = element ? element->preview : std::string();
    Dispose(kept);
    if (!call)
      return;
    content::RenderFrameHost* child = FrameAtPath(path);
    if (child && !InReachOf(content::RenderFrameHost::FromID(parent_id), child->GetParent()))
      child = nullptr;
    if (!child) {
      call->Fail("Selector \"" + chunks[index] + "\" resolved to " + preview +
                 ", <iframe> was expected");
      return;
    }
    target.frame = Follow(child);
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
      call->Fail(Closing() ? kClosed : kNavigated);
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
    if (call->OnHandle() && CdpHandleCall(call))
      return;
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
    SendSelectorOptions(frame, AutomationWorld::kMain);
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
    if (call->Has("pollingInterval") && call->Number("pollingInterval") <= 0) {
      call->Fail("Cannot poll with non-positive interval: " +
                 base::NumberToString(call->Number("pollingInterval")));
      return;
    }
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
    if (call->Has("selector") && args.size() > 1 && args[0] == '[') {
      base::DictValue on;
      on.Set("waitOnSelector", call->String("selector"));
      on.Set("strict", call->Flag("strict"));
      const bool empty = args[1] == ']';
      args = "[" + Json(base::Value(std::move(on))) + (empty ? "" : ",") + args.substr(1);
    }
    // Not AnswerTo()'s, which would answer as it goes when retried.
    auto answered = base::BindOnce(
        &Automation::Answered, weak_factory_.GetWeakPtr(), call->GetWeakPtr(), frame->GetGlobalId(),
                 Then(call, base::BindOnce([](Call* call, base::Value value) {
                        if (value.is_dict() &&
                            value.GetDict().FindBool("timedOut").value_or(false)) {
                          call->TimedOutNow();
                          return;
                        }
                        base::DictValue answer;
                        answer.Set("handle", std::move(value));
                        call->Done(base::Value(std::move(answer)));
                      })));
    if (call->Remaining() > 0)
      call->set_grace(1000);
    // frames.ts's waitForFunctionExpression() outlives a navigation: it
    // starts again in the new document, unless its arguments had handles.
    Agent(frame)->WaitForFunction(
        AutomationWorld::kMain, call->String("expression"), KindOf(call), args,
        handles, static_cast<int>(call->Number("pollingInterval")),
        call->Remaining(), !call->Flag("serialized"),
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(
            base::BindOnce(
                [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call, bool retry,
                   mojom::AutomationAgent::WaitForFunctionCallback answered,
                   AutomationStatus status, const std::string& result,
                   std::vector<AutomationHandlePtr> kept) {
                  if (status == AutomationStatus::kGone && retry && self && call &&
                      !call->finished() && !call->Expired() && !self->Closing()) {
                    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
                        FROM_HERE,
                        base::BindOnce(
                            [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call) {
                              if (self && call && !call->finished())
                                self->WaitForFunction(call.get());
                            },
                            self, call),
                        base::Milliseconds(20));
                    return;
                  }
                  std::move(answered).Run(status, result, std::move(kept));
                },
                weak_factory_.GetWeakPtr(), call->GetWeakPtr(), handles.empty(),
                std::move(answered)),
            AutomationStatus::kGone, std::string(), std::vector<AutomationHandlePtr>()));
  }

  // ---- JSHandle --------------------------------------------------------

  void GetProperty(Call* call) { Properties(call, call->String("name")); }
  void GetPropertyList(Call* call) { Properties(call, std::string()); }

  void Properties(Call* call, const std::string& name) {
    if (CdpHandleCall(call))
      return;
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
    if (CdpHandleCall(call))
      return;
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
    if (CdpHandleCall(call))
      return;
    Dispose(call->String("handle"));
    call->Done();
  }

  // ---- Reads -----------------------------------------------------------

  // A read that waits for its element (frames.ts's
  // _waitForFunctionOnSelector()).
  void Read(Call* call, const std::string& what, base::DictValue extra = {}) {
    extra.Set("what", what);
    if (!call->OnHandle())
      call->Log("waiting for " + LocatorText(call->String("selector")));
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
    call->Log("waiting for " + LocatorText(call->String("selector")) +
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
    content::RenderFrameHost* frame = wait_until == "networkidle" ? FrameOf(call) : nullptr;
    if (!frame) {
      FrameStep(call, "setContent", std::move(arg), AsDone());
      return;
    }
    // frames.ts's setContent() waits for the new document's lifecycle: the
    // network of the one it replaces is forgotten, the load waited for, then
    // 500 ms without requests.
    const content::FrameTreeNodeId node = frame->GetFrameTreeNodeId();
    lifecycles_[node].network_idle = false;
    ClearRequests(node);
    arg.Set("waitUntil", "load");
    FrameStep(call, "setContent", std::move(arg),
              base::BindOnce(
                  [](base::WeakPtr<Automation> self, content::FrameTreeNodeId node, Call* call,
                     base::Value) {
                    if (!self)
                      return;
                    self->navigation_waiters_.push_back(
                        {call->GetWeakPtr(), 0, "networkidle", base::Value(base::DictValue()), node});
                    self->Settle();
                  },
                  weak_factory_.GetWeakPtr(), node));
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
    // page.ts's ariaSnapshotJSONForFrame(): the iframes' snapshots stitched
    // in, unless of a locator not in "ai" mode.
    if (!call->OnHandle() && (call->String("selector").empty() || call->String("mode") == "ai")) {
      base::DictValue options;
      options.Set("mode", call->String("mode").empty() ? "default" : call->String("mode"));
      if (call->Has("depth"))
        options.Set("depth", static_cast<int>(call->Number("depth")));
      for (const char* key : {"boxes", "doNotRenderActive"}) {
        if (call->Flag(key))
          options.Set(key, true);
      }
      const std::string selector = call->String("selector");
      AriaTree(call->GetWeakPtr(), selector.empty() ? "body,frameset" : selector, !selector.empty(),
               std::move(options),
               base::BindOnce(
                   [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call, std::optional<base::Value> json,
                      std::string error) {
                     if (!self || !call)
                       return;
                     if (!json) {
                       call->Fail(error);
                       return;
                     }
                     if (call->Name() == "ariaSnapshotJSON") {
                       base::DictValue answer;
                       answer.Set("snapshot", std::move(*json));
                       call->Done(base::Value(std::move(answer)));
                       return;
                     }
                     base::DictValue arg;
                     arg.Set("json", std::move(*json));
                     self->FrameStep(call.get(), "renderAria", std::move(arg), AsKey("", "snapshot"));
                   },
                   weak_factory_.GetWeakPtr(), call->GetWeakPtr()));
      return;
    }
    base::DictValue arg;
    arg.Set("mode",
            call->String("mode").empty() ? "default" : call->String("mode"));
    if (call->Has("depth"))
      arg.Set("depth", static_cast<int>(call->Number("depth")));
    if (call->Flag("boxes"))
      arg.Set("boxes", true);
    arg.Set("json", call->Name() == "ariaSnapshotJSON");
    if (call->String("selector").empty() && !call->OnHandle()) {
      FrameStep(call, "ariaSnapshot", std::move(arg),
                AsKey("snapshot", "snapshot"));
      return;
    }
    StepOnTarget(call, "ariaSnapshot", std::move(arg),
                 AsPolledKey("snapshot", "snapshot"));
  }

  // One frame's snapshot (the element @selector, through frames, names),
  // then each iframe's it shows, merged as its children; the json to
  // @done, or nullopt and the error.
  using AriaDone = base::OnceCallback<void(std::optional<base::Value> json, std::string error)>;
  void AriaTree(base::WeakPtr<Call> call, std::string selector, bool strict, base::DictValue options,
                AriaDone done) {
    if (!call)
      return;
    base::DictValue params = options.Clone();
    const std::string frame = call->String("frame");
    if (!frame.empty())
      params.Set("frame", frame);
    params.Set("selector", selector);
    params.Set("strict", strict);
    params.Set("timeout", call->Remaining() ? call->Remaining() : kDefaultTimeoutMs);
    InnerCall("Frame.ariaSnapshotTree", &Automation::AriaSnapshotTree, std::move(params),
              base::BindOnce(&Automation::AriaTreeTaken, weak_factory_.GetWeakPtr(), call, selector,
                             std::move(options), std::move(done)));
  }

  void AriaTreeTaken(base::WeakPtr<Call> call, std::string selector, base::DictValue options, AriaDone done,
                     base::Value result, const std::string& error) {
    if (!call)
      return;
    base::Value* tree = result.is_dict() ? result.GetDict().Find("tree") : nullptr;
    base::Value* json = tree && tree->is_dict() ? tree->GetDict().Find("json") : nullptr;
    if (!json) {
      std::move(done).Run(std::nullopt, error.empty() ? std::string("zebkit: no snapshot") : error);
      return;
    }
    // Only the iframes it shows (not cut off by its depth).
    std::vector<std::pair<std::string, int>> refs;
    const base::ListValue* iframe_refs = tree->GetDict().FindList("iframeRefs");
    const base::DictValue* depths = tree->GetDict().FindDict("iframeDepths");
    if (iframe_refs) {
      for (const base::Value& ref : *iframe_refs) {
        if (!ref.is_string() || !depths || !depths->Find(ref.GetString()))
          continue;
        refs.emplace_back(ref.GetString(), depths->FindInt(ref.GetString()).value_or(0));
      }
    }
    struct Stitch {
      base::Value json;
      std::map<std::string, base::Value> children;
      AriaDone done;
    };
    auto stitch = std::make_shared<Stitch>();
    stitch->json = std::move(*json);
    stitch->done = std::move(done);
    auto finish = base::BarrierClosure(
        refs.size(), base::BindOnce(
                         [](std::shared_ptr<Stitch> stitch) {
                           // mergeIframeChildren().
                           std::function<void(base::Value&)> walk = [&](base::Value& node) {
                             if (!node.is_dict())
                               return;
                             const std::string* role = node.GetDict().FindString("role");
                             const std::string* ref = node.GetDict().FindString("ref");
                             if (role && *role == "iframe" && ref) {
                               auto child = stitch->children.find(*ref);
                               if (child != stitch->children.end() && child->second.is_list() &&
                                   !child->second.GetList().empty()) {
                                 node.GetDict().Set("children", std::move(child->second));
                               }
                               return;
                             }
                             if (base::ListValue* children = node.GetDict().FindList("children")) {
                               for (base::Value& item : *children)
                                 walk(item);
                             }
                           };
                           if (stitch->json.is_list()) {
                             for (base::Value& node : stitch->json.GetList())
                               walk(node);
                           }
                           std::move(stitch->done).Run(std::move(stitch->json), std::string());
                         },
                         stitch));
    for (const auto& [ref, depth] : refs) {
      base::DictValue child_options = options.Clone();
      if (std::optional<int> given = options.FindInt("depth"); given && *given)
        child_options.Set("depth", *given - depth - 1);
      // Non-strict: a frameset document has several.
      AriaTree(call, selector + " >> aria-ref=" + ref + kEnterFrame + "body,frameset", false,
               std::move(child_options),
               base::BindOnce(
                   [](std::shared_ptr<Stitch> stitch, std::string ref, base::RepeatingClosure finish,
                      std::optional<base::Value> json, std::string) {
                     stitch->children[ref] = json ? std::move(*json) : base::Value(base::ListValue());
                     finish.Run();
                   },
                   stitch, ref, finish));
    }
  }

  // Frame.resolveSelector {selector}: frames.ts's resolveSelector(), the
  // element's generated selector, through the frames it is in.
  void ResolveSelector(Call* call) {
    ResolveTarget(call, base::BindOnce(&Automation::ResolveSelectorOn, weak_factory_.GetWeakPtr(),
                                       call->GetWeakPtr()));
  }

  void ResolveSelectorOn(base::WeakPtr<Call> call, Target target) {
    if (!call)
      return;
    content::RenderFrameHost* frame = Resolve(target.frame);
    if (!frame) {
      call->Fail(kFrameDetached);
      return;
    }
    const content::GlobalRenderFrameHostId frame_id = frame->GetGlobalId();
    TargetStep(call.get(), target, "generateSelector", base::DictValue(),
               base::BindOnce(
                   [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call,
                      content::GlobalRenderFrameHostId frame_id, base::Value value) {
                     if (!self || !call)
                       return;
                     if (!value.is_string()) {
                       call->Fail("No element matching " + call->String("selector"));
                       return;
                     }
                     self->ResolveSelectorUp(call, frame_id, {value.GetString()});
                   },
                   weak_factory_.GetWeakPtr(), call, frame_id));
  }

  void ResolveSelectorUp(base::WeakPtr<Call> call,
                         content::GlobalRenderFrameHostId frame_id,
                         std::vector<std::string> chain) {
    if (!call)
      return;
    content::RenderFrameHost* frame = content::RenderFrameHost::FromID(frame_id);
    if (!frame) {
      call->Fail(kFrameDetached);
      return;
    }
    content::RenderFrameHost* parent = frame->GetParent();
    if (!parent) {
      std::reverse(chain.begin(), chain.end());
      base::DictValue answer;
      answer.Set("resolvedSelector", base::JoinString(chain, " >> internal:control=enter-frame >> "));
      call->Done(base::Value(std::move(answer)));
      return;
    }
    base::DictValue params;
    params.Set("frame", FrameId(frame));
    params.Set("timeout", call->Remaining() ? call->Remaining() : kDefaultTimeoutMs);
    const content::GlobalRenderFrameHostId parent_id = parent->GetGlobalId();
    InnerCall(
        "Frame.frameElement", &Automation::FrameElement, std::move(params),
        base::BindOnce(
            [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call, content::GlobalRenderFrameHostId parent_id,
               std::vector<std::string> chain, base::Value result, const std::string& error) {
              if (!self || !call)
                return;
              const std::string* id =
                  result.is_dict() ? result.GetDict().FindStringByDottedPath("element.handle") : nullptr;
              const HandleRef* handle = id ? self->FindHandle(*id) : nullptr;
              content::RenderFrameHost* owner = handle ? content::RenderFrameHost::FromID(handle->frame) : nullptr;
              if (!owner) {
                call->Fail("Unable to generate locator for " + call->String("selector"));
                return;
              }
              Target target;
              target.frame = Exact(owner);
              target.scope = handle->id;
              target.selector = ":scope";
              const std::string handle_id = *id;
              self->TargetStep(
                  call.get(), target, "generateSelector", base::DictValue(),
                  base::BindOnce(
                      [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call,
                         content::GlobalRenderFrameHostId parent_id, std::vector<std::string> chain,
                         std::string handle_id, base::Value value) {
                        if (!self || !call)
                          return;
                        self->Dispose(handle_id);
                        if (!value.is_string()) {
                          call->Fail("Unable to generate locator for " + call->String("selector"));
                          return;
                        }
                        chain.push_back(value.GetString());
                        self->ResolveSelectorUp(call, parent_id, std::move(chain));
                      },
                      self, call, parent_id, std::move(chain), handle_id));
            },
            weak_factory_.GetWeakPtr(), call, parent_id, std::move(chain)));
  }

  // Frame.ariaSnapshotTree (the endpoint's own): one frame's snapshot with
  // its iframes' refs, {tree: {json, iframeRefs, iframeDepths}}.
  void AriaSnapshotTree(Call* call) {
    base::DictValue arg;
    arg.Set("mode", call->String("mode").empty() ? "default" : call->String("mode"));
    if (call->Has("depth"))
      arg.Set("depth", static_cast<int>(call->Number("depth")));
    for (const char* key : {"boxes", "doNotRenderActive"}) {
      if (call->Flag(key))
        arg.Set(key, true);
    }
    StepOnTarget(call, "ariaSnapshotTree", std::move(arg), AsPolledKey("tree", "tree"));
  }

  // Frame.expect: Playwright's assertions (expect(locator).toHaveText() and
  // the rest), retried in the page until they hold or the time is out; the
  // answer is FrameExpectResult's: matches, received, timedOut,
  // errorMessage.
  void Expect(Call* call) {
    // frames.ts's expect() ends at its timeout with its result (timedOut),
    // which the page's step gives: its answer comes before the call's own
    // timeout does.
    if (call->Remaining() > 0)
      call->set_grace(1000);
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
    if (!IsAnyFrame(call->String("selector")))
      call->Log("waiting for " + LocatorText(call->String("selector")));
    // frames.ts: a property is of the page's own objects.
    StepOnTarget(call, "expect", std::move(arg),
                 base::BindOnce([](base::WeakPtr<Automation> self, Call* call, base::Value value) {
                   // Through frameLocator(), nowhere yet: again, in the frames
                   // there are now.
                   if (call->any_frame_retry && self && !call->Expired()) {
                     call->any_frame_retry = false;
                     const base::DictValue* dict = value.is_dict() ? &value.GetDict() : nullptr;
                     const bool is_not = call->Flag("isNot");
                     const bool passed = dict && dict->FindBool("matches").value_or(is_not) != is_not;
                     const std::string* error = dict ? dict->FindString("errorMessage") : nullptr;
                     if (dict && !passed && (!error || *error == "element(s) not found")) {
                       self->Expect(call);
                       return;
                     }
                   }
                   call->Done(std::move(value));
                 }, weak_factory_.GetWeakPtr()),
                 call->String("expression") == "to.have.property"
                     ? AutomationWorld::kMain
                     : AutomationWorld::kUtility);
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
    // A page opened by another (window.open, a link's target), which
    // Playwright's popup event names; not one the browser opened for a
    // modified click, which has none (CDP's openerId).
    answer.Set("hasOpener", web_contents()->HasLiveOriginalOpenerChain());
    call->Done(base::Value(std::move(answer)));
  }

  void NothingToDo(Call* call) { call->Done(); }

  // Page.captureSnapshot {streamer, reset, removeNoScript}: a DOM snapshot of
  // each frame, as snapshotter.ts takes it for a trace (snapshotterInjected.ts's
  // streamer, made in the frame's main world the first time):
  // {snapshots: [{frame, isMainFrame, data}]}, the main frame first.
  void CaptureSnapshot(Call* call) {
    const std::string streamer = call->String("streamer");
    const std::string reset = call->Has("reset") ? "\"" + call->String("reset") + "\"" : "undefined";
    const std::string ensure =
        "const s = " + Json(base::Value(streamer)) + "; if (!window[s]) (" +
        std::string(reinterpret_cast<const char*>(kSnapshotterScript), sizeof(kSnapshotterScript)) +
        ")(s, " + (call->Flag("removeNoScript") ? "true" : "false") + ");";
    // The html as its JSON: an element's attributes are an object, whose
    // order (the attributes' own) a dictionary here would not keep.
    const std::string expression = "(() => { " + ensure + " const r = window[s].captureSnapshot(" + reset +
                                   "); if (r) r.html = JSON.stringify(r.html); return r; })()";
    std::vector<content::RenderFrameHost*> frames;
    MainFrame()->ForEachRenderFrameHost([&](content::RenderFrameHost* frame) {
      if (frame->IsActive() && frame->IsRenderFrameLive())
        frames.push_back(frame);
    });
    // snapshotter.ts's _annotateFrameHierarchy(): the frames' elements marked
    // in their parents, before the parents' snapshots (the agent's calls keep
    // their order).
    if (const base::DictValue* guids = call->params().FindDict("frameGuids")) {
      const std::string mark = "(owner, id) => { " + ensure + " window[s].markIframe(owner, id); }";
      for (content::RenderFrameHost* frame : frames) {
        content::RenderFrameHost* parent = frame->GetParent();
        const std::string* guid = guids->FindString(FrameId(frame));
        if (!parent || !guid || !parent->IsRenderFrameLive())
          continue;
        const std::vector<content::RenderFrameHost*> children = Children(parent);
        const auto index = std::find(children.begin(), children.end(), frame);
        if (index == children.end())
          continue;
        base::ListValue args;
        base::DictValue owner;
        owner.Set("$h", 0);
        args.Append(std::move(owner));
        args.Append(*guid);
        Agent(parent)->Evaluate(AutomationWorld::kMain, mark, AutomationExpression::kFunction,
                                Json(base::Value(std::move(args))), {-1 - static_cast<int>(index - children.begin())},
                                true, true,
                                base::BindOnce([](AutomationStatus, const std::string&,
                                                  std::vector<AutomationHandlePtr>) {}));
      }
    }
    struct State {
      std::vector<base::Value> results;
      size_t left;
    };
    auto state = std::make_shared<State>();
    state->results.resize(frames.size());
    state->left = frames.size();
    auto finish = [](base::WeakPtr<Call> call, std::shared_ptr<State> state) {
      if (!call)
        return;
      base::ListValue list;
      for (base::Value& value : state->results) {
        if (value.is_dict())
          list.Append(std::move(value));
      }
      base::DictValue answer;
      answer.Set("snapshots", std::move(list));
      call->Done(base::Value(std::move(answer)));
    };
    if (frames.empty()) {
      finish(call->GetWeakPtr(), state);
      return;
    }
    for (size_t i = 0; i < frames.size(); ++i) {
      content::RenderFrameHost* frame = frames[i];
      base::DictValue entry;
      entry.Set("frame", FrameId(frame));
      entry.Set("isMainFrame", !frame->GetParent());
      Agent(frame)->Evaluate(
          AutomationWorld::kMain, expression, AutomationExpression::kValue, "[]", {}, true, true,
          mojo::WrapCallbackWithDefaultInvokeIfNotRun(
              base::BindOnce(
                  [](base::WeakPtr<Call> call, std::shared_ptr<State> state, size_t index,
                     base::DictValue entry, decltype(finish) finish, AutomationStatus status,
                     const std::string& result, std::vector<AutomationHandlePtr>) {
                    std::optional<base::Value> answer = Parse(result);
                    base::Value* value = answer && answer->is_dict() ? answer->GetDict().Find("value") : nullptr;
                    if (status == AutomationStatus::kOk && value && value->is_dict()) {
                      entry.Set("data", std::move(*value));
                      state->results[index] = base::Value(std::move(entry));
                    }
                    if (--state->left == 0)
                      finish(call, state);
                  },
                  call->GetWeakPtr(), state, i, std::move(entry), finish),
              AutomationStatus::kGone, std::string(), std::vector<AutomationHandlePtr>()));
    }
  }

  // Page.waitForInitialNavigation: answers once the page's main frame has
  // committed a document other than its initial empty one, as crPage.ts
  // reports a new page only then; {failed: true} when the navigation ended
  // without one (a download, a 204).
  void WaitForInitialNavigation(Call* call) {
    content::NavigationEntry* entry = web_contents()->GetController().GetLastCommittedEntry();
    if (entry && !entry->IsInitialEntry()) {
      call->Done();
      return;
    }
    initial_navigation_waiters_.push_back(call->GetWeakPtr());
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
            base::BindOnce(&Automation::FrameAnswer, weak_factory_.GetWeakPtr(),
                           call->GetWeakPtr(), frame->GetGlobalId(), true,
                           std::optional<std::string>()),
            std::vector<int32_t>{-1}));
  }

  void OwnerFrame(Call* call) {
    const HandleRef* handle = FindHandle(call->String("handle"));
    content::RenderFrameHost* frame =
        handle ? content::RenderFrameHost::FromID(handle->frame) : nullptr;
    if (!frame) {
      call->Fail(kDisposed);
      return;
    }
    // A node that is no element (a document, a text) is its frame's.
    Agent(frame)->OwnerFrame(
        handle->id,
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(
            base::BindOnce(&Automation::FrameAnswer, weak_factory_.GetWeakPtr(),
                           call->GetWeakPtr(), frame->GetGlobalId(), false,
                           std::optional<std::string>(FrameId(frame))),
            std::vector<int32_t>{-1}));
  }

  // The frame the renderer's ContentFrame or OwnerFrame named: indexes among
  // children from the main frame down, [-1] for none.
  content::RenderFrameHost* FrameAtPath(const std::vector<int32_t>& path) {
    content::RenderFrameHost* found = MainFrame();
    for (int32_t index : path) {
      if (!found || index < 0)
        return nullptr;
      std::vector<content::RenderFrameHost*> children = Children(found);
      found = static_cast<size_t>(index) < children.size() ? children[index]
                                                           : nullptr;
    }
    return found;
  }

  // A renderer's path is its word: the document it names must be one it could
  // have had an element of, a document in its own process.
  static bool InReachOf(content::RenderFrameHost* requester,
                        content::RenderFrameHost* document) {
    return requester && document &&
           requester->GetProcess() == document->GetProcess();
  }

  // {frame} at @path from the main frame; @otherwise when there is none. The
  // element is in @content_frame's frame's parent's document, or in that frame's.
  void FrameAnswer(base::WeakPtr<Call> call,
                   content::GlobalRenderFrameHostId requester,
                   bool content_frame,
                   std::optional<std::string> otherwise,
                   const std::vector<int32_t>& path) {
    if (!call)
      return;
    content::RenderFrameHost* found = FrameAtPath(path);
    if (found && !InReachOf(content::RenderFrameHost::FromID(requester),
                            content_frame ? found->GetParent() : found)) {
      found = nullptr;
    }
    base::DictValue answer;
    if (found)
      answer.Set("frame", FrameId(found));
    else if (otherwise)
      answer.Set("frame", *otherwise);
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
    // The agent gives the step the child's element (automation.mojom's Step).
    base::DictValue owner;
    owner.Set("$h", 0);
    base::DictValue arg;
    arg.Set("owner", std::move(owner));
    Agent(parent)->Step(
        AutomationWorld::kUtility, "childFrameBox",
        Json(base::Value(std::move(arg))), {-1 - index},
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
    // Frame.drop: no input, the drag events of dom.ts's _drop() at the point.
    bool drop = false;
  };

  // Frame.drop {selector, payloads, localPaths, data, position}: frames.ts's
  // drop(), the files read here as the server reads them.
  void Drop(Call* call) {
    const base::ListValue* payloads = call->params().FindList("payloads");
    const base::ListValue* paths = call->params().FindList("localPaths");
    const base::ListValue* data = call->params().FindList("data");
    const bool files = (payloads && !payloads->empty()) || (paths && !paths->empty());
    if (!files && !(data && !data->empty())) {
      call->Fail("At least one of \"files\" or \"data\" must be provided.");
      return;
    }
    std::vector<base::FilePath> local;
    if (paths && !(payloads && !payloads->empty())) {
      for (const base::Value& path : *paths) {
        if (path.is_string())
          local.push_back(base::FilePath::FromUTF8Unsafe(path.GetString()));
      }
    }
    if (local.empty()) {
      DropRead(call->GetWeakPtr(), std::nullopt);
      return;
    }
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock()},
        base::BindOnce(
            [](std::vector<base::FilePath> paths) -> std::optional<base::ListValue> {
              base::ListValue payloads;
              for (const base::FilePath& path : paths) {
                std::string bytes;
                base::File::Info info;
                if (!base::ReadFileToString(path, &bytes) || !base::GetFileInfo(path, &info))
                  return std::nullopt;
                std::string type;
                if (!net::GetMimeTypeFromFile(path, &type))
                  type = "application/octet-stream";
                base::DictValue payload;
                payload.Set("name", path.BaseName().AsUTF8Unsafe());
                payload.Set("mimeType", type);
                payload.Set("buffer", base::Base64Encode(bytes));
                payload.Set("lastModifiedMs", info.last_modified.InMillisecondsFSinceUnixEpoch());
                payloads.Append(std::move(payload));
              }
              return payloads;
            },
            std::move(local)),
        base::BindOnce(&Automation::DropRead, weak_factory_.GetWeakPtr(), call->GetWeakPtr()));
  }

  void DropRead(base::WeakPtr<Call> call, std::optional<base::ListValue> read) {
    if (!call)
      return;
    if (read)
      call->params().Set("payloads", std::move(*read));
    else if (!call->params().FindList("payloads"))
      call->params().Set("payloads", base::ListValue());
    PointerAction action;
    action.name = "drop";
    action.click = false;
    action.wait_for_enabled = false;
    action.drop = true;
    Pointer(call.get(), action);
  }

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
    if (!HasTouch()) {
      call->Fail("The page does not support tap. Use hasTouch context option to enable touch support.");
      return;
    }
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
    arg.Set("name", action.name);
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
    if (!call->Flag("force") && !call->OnHandle() && !call->attempt)
      call->Log("waiting for " + LocatorText(target.selector));
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
    // tracing.ts's input event's box: the element's, in the frame, until the
    // frame's offset in the page is known.
    if (const base::DictValue* box = prepared.GetDict().FindDict("box"); box && call->Flag("traceLog")) {
      base::DictValue in_frame = box->Clone();
      in_frame.Set("pointX", where.x());
      in_frame.Set("pointY", where.y());
      call->params().Set("__inputBox", std::move(in_frame));
    }
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
    // A trial run's input goes too, into the hit target interceptor that
    // blocks the page's events (dom.ts performs the action either way); a
    // drop's has nothing to send.
    if (call->Flag("trial") && action.drop) {
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
    if (call->Flag("trial") && action.drop) {
      std::move(finish).Run();
      return;
    }
    // Navigations the input starts are waited for (waitForSignalsCreatedBy).
    watched_navigations_.erase(call.get());
    watching_.insert(call.get());
    if (!action.drop)
      call->Log("  performing " + action.name + " action");
    // tracing.ts's input event: where the action points, in the page.
    if (call->Flag("traceLog")) {
      base::DictValue point;
      point.Set("x", in_root.x());
      point.Set("y", in_root.y());
      mouse_in_root_ = in_root;
      if (const base::DictValue* box = call->params().FindDict("__inputBox")) {
        const double dx = in_root.x() - box->FindDouble("pointX").value_or(in_root.x());
        const double dy = in_root.y() - box->FindDouble("pointY").value_or(in_root.y());
        base::DictValue in_page;
        in_page.Set("x", box->FindDouble("x").value_or(0) + dx);
        in_page.Set("y", box->FindDouble("y").value_or(0) + dy);
        in_page.Set("width", box->FindDouble("width").value_or(0));
        in_page.Set("height", box->FindDouble("height").value_or(0));
        point.Set("box", std::move(in_page));
      }
      call->params().Set("__inputPoint", std::move(point));
    }
    // The trace's "action" snapshot, here, before the input (dom.ts).
    if (const base::DictValue* snapshot = call->params().FindDict("actionSnapshot")) {
      auto* inner = new Call(
          view_, "Page.captureSnapshot", snapshot->Clone(),
          [](zebkit_view*, const char* json, const char*, void* data) {
            std::unique_ptr<base::WeakPtr<Call>> outer(static_cast<base::WeakPtr<Call>*>(data));
            std::optional<base::Value> taken = json ? Parse(json) : std::nullopt;
            if (*outer && taken && taken->is_dict())
              (*outer)->params().Set("__actionSnapshot", std::move(*taken));
          },
          new base::WeakPtr<Call>(call));
      call->params().Remove("actionSnapshot");
      CaptureSnapshot(inner);
    }
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
    if (action.drop) {
      base::DictValue arg;
      const base::ListValue* payloads = call->params().FindList("payloads");
      arg.Set("payloads", payloads ? payloads->Clone() : base::ListValue());
      const base::ListValue* data = call->params().FindList("data");
      arg.Set("data", data ? data->Clone() : base::ListValue());
      base::DictValue point;
      point.Set("x", in_root.x());
      point.Set("y", in_root.y());
      arg.Set("point", std::move(point));
      call->Log("  performing drop action");
      TargetStep(call.get(), target, "drop", std::move(arg),
                 base::BindOnce(
                     [](base::WeakPtr<Call> call, base::OnceClosure restored, base::Value value) {
                       if (!call)
                         return;
                       if (value.is_string() && value.GetString() == "not-accepted") {
                         call->Fail("Drop target did not accept the drop \u2014 its dragover handler did not "
                                    "call preventDefault()");
                         return;
                       }
                       std::move(restored).Run();
                     },
                     call, std::move(restored)));
      return;
    }
    if (action.tap) {
      SendTap(widget, in_widget, std::move(restored));
      return;
    }
    const Button button = ButtonOf(call->String("button"));
    const int click_count =
        static_cast<int>(call->Number("clickCount", action.click_count));
    const int delay = static_cast<int>(call->Number("delay"));
    const int steps = std::max(1, static_cast<int>(call->Number("steps", 1)));
    MouseStepsThen(
        widget, in_widget, in_root, action.down_only ? 1 : steps,
        base::BindOnce(
            [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call,
               content::RenderWidgetHost* widget, bool down_only, bool up_only, bool click,
               Button button, int click_count, int delay, base::OnceClosure restored) {
              if (!self || !call || self->mouse_widget_.get() != widget) {
                std::move(restored).Run();
                return;
              }
              if (down_only) {
                self->MouseDownIn(widget, button, 1);
                self->WhenAcked(Type::kMouseDown, std::move(restored));
              } else if (up_only) {
                // After the move, which may start the page's drag
                // (dragAndDrop()): the release ends it.
                self->WhenAcked(
                    Type::kMouseMove,
                    base::BindOnce(
                        [](base::WeakPtr<Automation> self, content::RenderWidgetHost* widget, Button button,
                           base::OnceClosure restored) {
                          if (!self || self->mouse_widget_.get() != widget) {
                            std::move(restored).Run();
                            return;
                          }
                          self->MouseUpIn(widget, button, 1);
                          self->WhenAcked(Type::kMouseUp, std::move(restored));
                        },
                        self, widget, button, std::move(restored)));
              } else if (click) {
                self->PressAndRelease(call.get(), widget, button, click_count, 1, delay,
                                      std::move(restored));
              } else {
                self->WhenAcked(Type::kMouseMove, std::move(restored));
              }
            },
            weak_factory_.GetWeakPtr(), call->GetWeakPtr(), widget, action.down_only,
            action.up_only, action.click, button, click_count, delay, std::move(restored)));
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
    // The input is in: a frame the action took away (it detached the frame)
    // ends the action, as dom.ts's hit target check does.
    finish.Set("doneIfGone", true);
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
      LocatorHandlersCheckpoint(call.get(), base::BindOnce(&Automation::PointerOn,
                                                           weak_factory_.GetWeakPtr(), call,
                                                           action, std::move(target)));
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
    // dom.ts's _performPointerAction(): what it says once the input is in.
    call->Log(std::string("  ") + (call->Flag("trial") ? "trial " : "") + action.name + " action done");
    call->Log("  waiting for scheduled navigations to finish");
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
      call->Log("  navigations have finished");
      call->Done();
      return;
    }
    navigation_waiters_.push_back({call->GetWeakPtr(), it->second, "commit",
                                   base::Value(),
                                   MainFrame()->GetFrameTreeNodeId(), true});
    watched_navigations_.erase(it);
  }

  // dom.ts's _fill(): the element's states and the injected script's fill,
  // the text then committed by the agent in the same call.
  void Fill(Call* call) {
    base::DictValue arg;
    arg.Set("value", call->String("value"));
    arg.Set("force", call->Flag("force"));
    if (!call->OnHandle())
      call->Log("waiting for " + LocatorText(call->String("selector")));
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
      call->Log("waiting for " + LocatorText(call->String("selector")));
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
  // A serialized eventInit (Playwright's) has handles of the target's frame.
  void DispatchEvent(Call* call) {
    base::DictValue arg;
    arg.Set("type", call->String("type"));
    if (const base::Value* init = call->params().Find("eventInit"))
      arg.Set("eventInit", init->Clone());
    std::vector<std::string> ids;
    if (call->Flag("serialized")) {
      arg.Set("serializedInit", true);
      if (const base::ListValue* given = call->params().FindList("eventInitHandles")) {
        for (const base::Value& id : *given)
          ids.push_back(id.is_string() ? id.GetString() : std::string());
      }
    }
    ResolveTarget(
        call, base::BindOnce(
                  [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call, base::DictValue arg,
                     std::vector<std::string> ids, Target target) {
                    if (!self || !call)
                      return;
                    content::RenderFrameHost* frame = self->Resolve(target.frame);
                    std::vector<int32_t> extra;
                    for (const std::string& id : ids) {
                      const HandleRef* handle = self->FindHandle(id);
                      if (!handle) {
                        call->Fail(kDisposed);
                        return;
                      }
                      if (!frame || handle->frame != frame->GetGlobalId()) {
                        call->Fail("JSHandles can be evaluated only in the context they were created!");
                        return;
                      }
                      extra.push_back(handle->id);
                    }
                    self->TargetStep(call.get(), target, "dispatchEvent", std::move(arg),
                                     base::BindOnce(
                                         [](base::WeakPtr<Call> call, base::Value) {
                                           if (call)
                                             call->Done();
                                         },
                                         call),
                                     AutomationWorld::kMain, std::move(extra));
                  },
                  weak_factory_.GetWeakPtr(), call->GetWeakPtr(), std::move(arg), std::move(ids)));
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
    // An action's navigation (frames.ts's scheduled navigations): one that
    // fails or becomes a download ends the wait, not the action.
    bool input = false;
  };

  // The lifecycle of a frame's document, as Playwright's loadstate events.
  struct Lifecycle {
    bool dom_content_loaded = false;
    bool loaded = false;
    bool network_idle = false;
  };

  void Goto(Call* call) {
    // helper.ts's completeUserURL().
    std::string spec = call->String("url");
    if (spec.starts_with("localhost") || spec.starts_with("127.0.0.1"))
      spec = "http://" + spec;
    const GURL url(spec);
    content::RenderFrameHost* frame = FrameOf(call);
    if (!frame) {
      call->Fail(kFrameDetached);
      return;
    }
    // frames.ts's gotoImpl(): the referer, given or the page's extra header.
    std::optional<std::string> referer;
    if (const std::string* header = PageExtraHeader("referer"))
      referer = *header;
    if (call->Has("referer")) {
      const std::string given = call->String("referer");
      if (referer && *referer != given) {
        std::string wait_until = call->String("waitUntil");
        call->Log("navigating to \"" + call->String("url") + "\", waiting until \"" +
                  (wait_until.empty() ? std::string("load") : wait_until) + "\"");
        call->Fail("\"referer\" is already specified as extra HTTP header");
        return;
      }
      referer = given;
    }
    if (!url.is_valid()) {
      call->Fail(
          "Protocol error (Page.navigate): Cannot navigate to invalid URL");
      return;
    }
    if (referer) {
      // As Page.navigate's referrer with crPage.ts's referrerPolicy
      // "unsafeUrl": sent as given, whatever the page's policy.
      StartNavigation(call, frame,
                      base::BindOnce(
                          [](content::WebContents* contents, content::FrameTreeNodeId node,
                             bool main, GURL url, GURL referer) {
                            content::NavigationController::LoadURLParams params(url);
                            if (!main)
                              params.frame_tree_node_id = node;
                            params.referrer = content::Referrer(
                                referer, network::mojom::ReferrerPolicy::kAlways);
                            params.transition_type =
                                main ? ui::PageTransitionFromInt(ui::PAGE_TRANSITION_TYPED |
                                                                 ui::PAGE_TRANSITION_FROM_ADDRESS_BAR)
                                     : ui::PAGE_TRANSITION_LINK;
                            contents->GetController().LoadURLWithParams(params);
                          },
                          web_contents(), frame->GetFrameTreeNodeId(), frame == MainFrame(),
                          url, GURL(*referer)));
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
    // frames.ts: the main frame numbered again when it leaves a real document,
    // so that ARIA refs of the old one do not resolve in the new.
    if (navigation->IsInPrimaryMainFrame() && navigation->HasCommitted() && !navigation->IsSameDocument()) {
      const GURL previous = navigation->GetPreviousPrimaryMainFrameURL();
      if (previous.is_valid() && !previous.IsAboutBlank())
        frame_seqs_[navigation->GetFrameTreeNodeId()] = next_frame_seq_++;
    }
    if (navigation->HasCommitted() && !navigation->IsErrorPage() && navigation->GetRenderFrameHost()) {
      const url::Origin origin = navigation->GetRenderFrameHost()->GetLastCommittedOrigin();
      if (!origin.opaque())
        Context().visited_origins.insert(origin.Serialize());
    }
    if (navigation->IsInPrimaryMainFrame() && !initial_navigation_waiters_.empty() &&
        !navigation->IsSameDocument()) {
      content::NavigationEntry* entry = web_contents()->GetController().GetLastCommittedEntry();
      const bool committed = navigation->HasCommitted() && entry && !entry->IsInitialEntry();
      if (committed || !navigation->HasCommitted()) {
        std::vector<base::WeakPtr<Call>> waiters = std::move(initial_navigation_waiters_);
        for (const base::WeakPtr<Call>& waiter : waiters) {
          if (!waiter)
            continue;
          base::DictValue answer;
          if (!committed)
            answer.Set("failed", true);
          waiter->Done(base::Value(std::move(answer)));
        }
      }
    }
    const int64_t id = navigation->GetNavigationId();
    const content::FrameTreeNodeId node = navigation->GetFrameTreeNodeId();
    if (navigation->IsInPrimaryMainFrame() && navigation->HasCommitted() &&
        !navigation->IsSameDocument() && !workers_.empty()) {
      // page.ts's _clearWorkers(): the page's workers end with its document.
      std::vector<std::string> ended;
      for (const auto& [worker, info] : workers_)
        ended.push_back(worker);
      workers_.clear();
      for (const std::string& worker : ended) {
        base::DictValue params;
        params.Set("worker", worker);
        Emit("Worker.close", std::move(params));
      }
    }
    if (navigation->HasCommitted() && !navigation->IsSameDocument()) {
      // A new document: its lifecycle starts again. One restored from the
      // back-forward cache has loaded long ago: no load event comes again.
      Lifecycle fresh;
      if (navigation->IsServedFromBackForwardCache())
        fresh.dom_content_loaded = fresh.loaded = fresh.network_idle = true;
      lifecycles_[node] = fresh;
      ClearRequests(node);
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
        if (waiter.input) {
          waiter.call->Done();
          continue;
        }
        waiter.call->Fail(net::ErrorToString(navigation->GetNetErrorCode()) +
                          " at " + navigation->GetURL().spec());
        continue;
      }
      // A challenge no credentials answered: Chrome's login prompt cancels
      // the navigation (LoginTabHelper), as Playwright's tests know it.
      if (navigation->IsInPrimaryMainFrame() && unanswered_auth_.contains(navigation->GetURL()) &&
          navigation->GetResponseHeaders() && navigation->GetResponseHeaders()->response_code() == 401 &&
          !waiter.input) {
        waiter.call->Fail(net::ErrorToString(net::ERR_INVALID_AUTH_CREDENTIALS) + " at " +
                          navigation->GetURL().spec());
        continue;
      }
      waiter.response = base::Value(ResponseOf(navigation));
      if (waiter.wait_until == "commit" || navigation->IsSameDocument())
        Answer(waiter);
    }
    --settling_;
    if (navigation->IsInPrimaryMainFrame())
      unanswered_auth_.erase(navigation->GetURL());
    Settle();
  }

  // A main frame navigation's challenge that no credentials answered.
  void UnansweredAuth(const GURL& url) { unanswered_auth_.insert(url); }

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

  // The main frame's load: when its load event has run, as Chromium's
  // lifecycle event "load", which Playwright waits for over CDP. Blink lays
  // the page out after that and only then sends DidFinishLoad, on a page of
  // some size several milliseconds later.
  void DocumentOnLoadCompletedInPrimaryMainFrame() override {
    content::RenderFrameHost* frame = web_contents()->GetPrimaryMainFrame();
    Lifecycle& lifecycle = lifecycles_[frame->GetFrameTreeNodeId()];
    lifecycle.dom_content_loaded = lifecycle.loaded = true;
    LoadState(frame, "load");
  }

  void DidFinishLoad(content::RenderFrameHost* frame, const GURL&) override {
    Lifecycle& lifecycle = lifecycles_[frame->GetFrameTreeNodeId()];
    lifecycle.dom_content_loaded = true;
    // The main frame's "load" is its load event's (above): a document whose
    // loading window.stop() ended has none, and Chromium's lifecycle no
    // "load" either, though the browser hears the loading finish.
    if (frame == web_contents()->GetPrimaryMainFrame())
      return;
    if (!lifecycle.loaded) {
      lifecycle.loaded = true;
      LoadState(frame, "load");
    }
  }

  // ---- networkidle, as frames.ts has it -------------------------------
  // A frame is idle 500 ms after its last request in flight ended (or its
  // document committed without one); "networkidle" when it and every frame
  // under it are.

  void TrackRequests(const std::string& event, const base::DictValue& params) {
    const std::string* request = params.FindString("request");
    if (!request)
      return;
    if (event == "Page.request") {
      const std::string* frame = params.FindString("frame");
      int value = 0;
      if (!frame || frame->size() < 2 ||
          !base::StringToInt(std::string_view(*frame).substr(1), &value)) {
        return;
      }
      // frames.ts's _isExcludedFromNetworkIdle(): a favicon, an EventSource.
      const std::string* url = params.FindString("url");
      const std::string* type = params.FindString("resourceType");
      if ((url && url->ends_with("/favicon.ico")) || (type && *type == "eventsource"))
        return;
      const content::FrameTreeNodeId node(value);
      request_frames_[*request] = node;
      if (params.FindBool("isNavigationRequest").value_or(false))
        navigation_requests_.insert(*request);
      std::set<std::string>& inflight = inflight_[node];
      inflight.insert(*request);
      if (inflight.size() == 1)
        StopIdleTimer(node);
      return;
    }
    if (event != "Page.requestFinished" && event != "Page.requestFailed")
      return;
    auto found = request_frames_.find(*request);
    if (found == request_frames_.end())
      return;
    const content::FrameTreeNodeId node = found->second;
    request_frames_.erase(found);
    navigation_requests_.erase(*request);
    auto inflight = inflight_.find(node);
    if (inflight == inflight_.end() || !inflight->second.erase(*request))
      return;
    if (inflight->second.empty())
      StartIdleTimer(node);
  }

  void StartIdleTimer(content::FrameTreeNodeId node) {
    auto lifecycle = lifecycles_.find(node);
    if (lifecycle != lifecycles_.end() && lifecycle->second.network_idle)
      return;
    idle_timers_[node].Start(FROM_HERE, base::Milliseconds(500),
                             base::BindOnce(
                                 [](Automation* self, content::FrameTreeNodeId node) {
                                   self->idle_self_.insert(node);
                                   self->RecalculateNetworkIdle();
                                 },
                                 base::Unretained(this), node));
  }

  void StopIdleTimer(content::FrameTreeNodeId node) {
    idle_timers_.erase(node);
    idle_self_.erase(node);
  }

  // A new document in @node: only its own request stays in flight.
  void ClearRequests(content::FrameTreeNodeId node) {
    std::set<std::string>& inflight = inflight_[node];
    std::erase_if(inflight, [this](const std::string& request) {
      return !navigation_requests_.contains(request);
    });
    StopIdleTimer(node);
    if (inflight.empty())
      StartIdleTimer(node);
  }

  void RecalculateNetworkIdle() {
    content::RenderFrameHost* main = MainFrame();
    if (!main)
      return;
    // Children before their parent: the tree's frames in reverse order.
    std::vector<content::RenderFrameHost*> frames;
    main->ForEachRenderFrameHost([&](content::RenderFrameHost* frame) {
      if (frame->IsActive())
        frames.push_back(frame);
    });
    std::map<content::RenderFrameHost*, bool> idle;
    for (auto it = frames.rbegin(); it != frames.rend(); ++it)
      idle[*it] = idle_self_.contains((*it)->GetFrameTreeNodeId());
    for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
      content::RenderFrameHost* frame = *it;
      if (content::RenderFrameHost* parent = frame->GetParent(); parent && idle.contains(parent)) {
        if (!idle[frame])
          idle[parent] = false;
      }
    }
    for (content::RenderFrameHost* frame : frames) {
      if (!idle[frame])
        continue;
      Lifecycle& lifecycle = lifecycles_[frame->GetFrameTreeNodeId()];
      if (lifecycle.network_idle)
        continue;
      lifecycle.network_idle = true;
      LoadState(frame, "networkidle");
    }
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
    idle_self_.erase(node);
    inflight_.erase(node);
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&Automation::RecalculateNetworkIdle, weak_factory_.GetWeakPtr()));
    base::DictValue params;
    params.Set("frame", "f" + base::NumberToString(node.value()));
    Emit("Page.frameDetached", std::move(params));
  }

  void PrimaryPageChanged(content::Page&) override {
    // The last move's widget may live on in the back-forward cache.
    mouse_widget_ = nullptr;
    EmulateFocus();
  }

  void RenderViewReady() override {
    if (!web_contents()->GetRenderWidgetHostView())
      return;
    if (pending_metrics_) {
      const std::array<int, 4> metrics = *pending_metrics_;
      EmulateMetrics(metrics[0], metrics[1], metrics[2], metrics[3]);
    }
    if (pending_touch_)
      EmulateTouch();
  }

  // Emulation.setTouchEmulationEnabled, once the page has its view.
  void EmulateTouch() {
    pending_touch_ = !web_contents()->GetRenderWidgetHostView();
    if (pending_touch_)
      return;
    base::DictValue params;
    params.Set("enabled", true);
    view_->Send("Emulation.setTouchEmulationEnabled", std::move(params));
  }

  // frames.ts's Frame.seq: the page's frames numbered as they are made, the
  // main frame 0 (the injected script's refs of a frame's ARIA snapshot).
  int FrameSeq(content::RenderFrameHost* frame) {
    if (!frame->GetParent()) {
      auto main = frame_seqs_.find(frame->GetFrameTreeNodeId());
      return main == frame_seqs_.end() ? 0 : main->second;
    }
    auto [found, made] = frame_seqs_.try_emplace(frame->GetFrameTreeNodeId(), next_frame_seq_);
    if (made)
      ++next_frame_seq_;
    return found->second;
  }

  void RenderFrameCreated(content::RenderFrameHost* frame) override {
    FrameSeq(frame);
    // A frame is idle 500 ms after it is made with nothing to load, as
    // frames.ts starts its timer when a Frame is made (a javascript: or a
    // broken URL's frame commits nothing).
    if (inflight_[frame->GetFrameTreeNodeId()].empty())
      StartIdleTimer(frame->GetFrameTreeNodeId());
    if (!media_type_.empty())
      Agent(frame)->SetMediaType(media_type_);
    if (!exposed_.empty())
      Agent(frame)->Expose(exposed_, ContextBindings());
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
      const std::optional<std::u16string>& stack) override {
    // An uncaught exception is the page's error, not a console message, as
    // Runtime.exceptionThrown is apart from Runtime.consoleAPICalled. The page's
    // own console.error("Uncaught x") stays a console message (patches/1071
    // marks its stack).
    if (level == blink::mojom::ConsoleMessageLevel::kError &&
        message.starts_with(u"Uncaught ") &&
        !(stack && stack->starts_with(u'\x01'))) {
      // The page's DevTools session tells those of its process
      // (ExceptionThrown()); a frame of another process's is told here.
      if (runtime_exceptions_ && frame->GetProcess() == MainFrame()->GetProcess())
        return;
      base::DictValue where;
      where.Set("url", base::UTF16ToUTF8(source));
      where.Set("lineNumber", line);
      where.Set("columnNumber", 0);
      const std::string text = base::UTF16ToUTF8(message.substr(9));
      base::DictValue error;
      const size_t colon = text.find(": ");
      const bool named = colon != std::string::npos &&
                         text.find(' ') == colon + 1 &&
                         text.substr(0, colon).ends_with("Error");
      error.Set("name", named ? text.substr(0, colon) : std::string("Error"));
      error.Set("message", named ? text.substr(colon + 2) : text);
      std::string trace = text;
      if (stack && !stack->empty())
        trace += "\n" + base::UTF16ToUTF8(*stack);
      error.Set("stack", trace);
      base::DictValue params;
      params.Set("error", std::move(error));
      params.Set("location", std::move(where));
      Emit("Page.pageError", std::move(params));
      return;
    }
    // The page's DevTools session tells those of its process
    // (ConsoleAPICalled(), LogEntryAdded()); a frame of another process's
    // is told here.
    if (runtime_exceptions_ && frame->GetProcess() == MainFrame()->GetProcess())
      return;
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
      if (!call)
        continue;
      // An action whose input is in, which closed the page (window.close()
      // from a click): done, as dom.ts's hit target check ends it.
      if (watching_.contains(call.get()))
        call->Done();
      else
        call->Fail(kClosed);
    }
  }

  // ---- Dialogs and file choosers (Page.updateSubscription) -------------

  void UpdateSubscription(Call* call) {
    const std::string event = call->String("event");
    const bool enabled = call->Flag("enabled");
    if (event == "dialog") {
      hold_dialogs_ = enabled;
    } else if (event == "dialogClosed") {
      report_closed_dialogs_ = enabled;
    } else if (event == "fileChooser") {
      intercept_file_choosers_ = enabled;
      // crPage.ts's Page.setInterceptFileChooserDialog: Blink names the
      // input that opened the chooser (Page.fileChooserOpened), even one not
      // in the document; a chooser it does not intercept (a frame of another
      // process) still comes through TakeFileChooser().
      // (Blink's page agent hears of choosers once enabled.)
      if (enabled)
        view_->Send("Page.enable", base::DictValue());
      base::DictValue intercept;
      intercept.Set("enabled", enabled);
      view_->Send("Page.setInterceptFileChooserDialog", std::move(intercept));
    }
    // Other events (console, request, response, ...) are always sent.
    call->Done();
  }

  // A client that only hears of closed dialogs still gets them: held, then
  // dismissed at once as Playwright's dialog.ts does without a handler.
  bool holds_dialogs() const { return hold_dialogs_ || report_closed_dialogs_; }

  void DialogOpened(const zebkit_dialog& dialog, bool held_elsewhere) {
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
    // Without a listener of the client's, a dialog held for it alone is
    // dismissed (dialog.ts); one held for the application or a CDP client
    // as well is theirs to answer.
    const bool dismiss = dialog.held && !hold_dialogs_ && !held_elsewhere;
    if (dismiss)
      params.Set("auto", true);
    Emit("BrowserContext.dialog", std::move(params));
    if (!dismiss)
      return;
    // After the dialog manager has finished opening it.
    const bool accept = dialog.type == ZEBKIT_DIALOG_BEFOREUNLOAD;
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(
                       [](base::WeakPtr<Automation> self, uint64_t id, bool accept) {
                         if (self && self->view_)
                           zebkit_view_dialog_respond(self->view_, id, accept, nullptr);
                       },
                       weak_factory_.GetWeakPtr(), dialog.id, accept));
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
    if (auto opened = devtools_choosers_.find(call->String("fileChooser"));
        found == file_choosers_.end() && opened != devtools_choosers_.end() && view_) {
      // One DevTools reported (Page.fileChooserOpened): its input's files,
      // as crPage.ts sets them.
      base::DictValue params;
      params.Set("backendNodeId", opened->second);
      base::ListValue files;
      if (const base::ListValue* paths = call->params().FindList("localPaths"))
        files = paths->Clone();
      params.Set("files", std::move(files));
      devtools_choosers_.erase(opened);
      view_->Send("DOM.setFileInputFiles", std::move(params),
                  base::BindOnce(
                      [](base::WeakPtr<Call> call, const base::DictValue*, const std::string& error) {
                        if (!call)
                          return;
                        if (!error.empty())
                          call->Fail(error);
                        else
                          call->Done();
                      },
                      call->GetWeakPtr()));
      return;
    }
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

  // ---- Locator handlers (page.ts) --------------------------------------

  // A command of the endpoint's own, which no trace records: @handler with
  // @params, its answer (or error) to @reply.
  using InnerReply = base::OnceCallback<void(base::Value result, const std::string& error)>;
  void InnerCall(const std::string& method, Handler handler, base::DictValue params, InnerReply reply) {
    params.Set("internalCall", true);
    auto* call = new Call(
        view_, method, std::move(params),
        [](zebkit_view*, const char* json, const char* error, void* data) {
          std::unique_ptr<InnerReply> reply(static_cast<InnerReply*>(data));
          std::optional<base::Value> result = json ? Parse(json) : std::nullopt;
          std::move(*reply).Run(result ? std::move(*result) : base::Value(), error ? error : "");
        },
        new InnerReply(std::move(reply)));
    Run(call, handler);
  }

  // Page.registerLocatorHandler {selector, noWaitAfter}: {uid}.
  void RegisterLocatorHandler(Call* call) {
    base::DictValue arg;
    arg.Set("selector", call->String("selector"));
    FrameStep(call, "locatorText", std::move(arg),
              base::BindOnce(
                  [](base::WeakPtr<Automation> self, Call* call, base::Value text) {
                    if (!self)
                      return;
                    LocatorHandler handler;
                    handler.selector = call->String("selector");
                    handler.text = text.is_string() ? text.GetString() : LocatorText(handler.selector);
                    handler.no_wait_after = call->Flag("noWaitAfter");
                    const int uid = ++self->last_locator_handler_;
                    self->locator_handlers_[uid] = std::move(handler);
                    base::DictValue answer;
                    answer.Set("uid", uid);
                    call->Done(base::Value(std::move(answer)));
                  },
                  weak_factory_.GetWeakPtr()));
  }

  // Page.resolveLocatorHandlerNoReply {uid, remove}.
  void ResolveLocatorHandler(Call* call) {
    const int uid = static_cast<int>(call->Number("uid"));
    auto found = locator_handlers_.find(uid);
    std::vector<base::OnceClosure> resolved;
    if (found != locator_handlers_.end()) {
      resolved = std::move(found->second.resolved);
      found->second.resolved.clear();
      found->second.triggered = false;
      if (call->Flag("remove"))
        locator_handlers_.erase(found);
    }
    call->Done();
    for (base::OnceClosure& closure : resolved)
      std::move(closure).Run();
  }

  // Page.unregisterLocatorHandler {uid}.
  void UnregisterLocatorHandler(Call* call) {
    locator_handlers_.erase(static_cast<int>(call->Number("uid")));
    call->Done();
  }

  bool InLocatorHandler() {
    std::erase_if(in_locator_handler_, [](const base::WeakPtr<Call>& call) { return !call; });
    return !in_locator_handler_.empty();
  }

  // page.ts's _performLocatorHandlersCheckpoint(): each handler whose
  // locator is visible is triggered, the client runs it, and the action
  // goes on once the locator is hidden (unless noWaitAfter).
  void LocatorHandlersCheckpoint(Call* call, base::OnceClosure next) {
    if (locator_handlers_.empty() || call->Flag("force") || call->Flag("internalCall") ||
        InLocatorHandler()) {
      std::move(next).Run();
      return;
    }
    std::vector<int> uids;
    for (const auto& [uid, handler] : locator_handlers_)
      uids.push_back(uid);
    CheckLocatorHandler(call->GetWeakPtr(), std::move(uids), 0, std::move(next));
  }

  void CheckLocatorHandler(base::WeakPtr<Call> call, std::vector<int> uids, size_t index,
                           base::OnceClosure next) {
    if (!call)
      return;
    if (index >= uids.size()) {
      std::move(next).Run();
      return;
    }
    auto found = locator_handlers_.find(uids[index]);
    if (found == locator_handlers_.end()) {
      CheckLocatorHandler(call, std::move(uids), index + 1, std::move(next));
      return;
    }
    if (found->second.triggered) {
      RunLocatorHandler(call, std::move(uids), index, std::move(next));
      return;
    }
    base::DictValue params;
    params.Set("frame", FrameId(MainFrame()));
    params.Set("selector", found->second.selector);
    params.Set("strict", true);
    params.Set("timeout", call->Remaining() ? call->Remaining() : kDefaultTimeoutMs);
    InnerCall("Frame.isVisible", &Automation::IsVisible, std::move(params),
              base::BindOnce(
                  [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call, std::vector<int> uids,
                     size_t index, base::OnceClosure next, base::Value result, const std::string&) {
                    if (!self || !call)
                      return;
                    auto found = self->locator_handlers_.find(uids[index]);
                    const base::Value* value = result.is_dict() ? result.GetDict().Find("value") : nullptr;
                    const bool visible = value && value->is_bool() && value->GetBool();
                    if (found != self->locator_handlers_.end() && visible && !found->second.triggered) {
                      found->second.triggered = true;
                      base::DictValue event;
                      event.Set("uid", uids[index]);
                      self->Emit("Page.locatorHandlerTriggered", std::move(event));
                    }
                    if (found != self->locator_handlers_.end() && found->second.triggered) {
                      self->RunLocatorHandler(call, std::move(uids), index, std::move(next));
                      return;
                    }
                    self->CheckLocatorHandler(call, std::move(uids), index + 1, std::move(next));
                  },
                  weak_factory_.GetWeakPtr(), call, std::move(uids), index, std::move(next)));
  }

  void RunLocatorHandler(base::WeakPtr<Call> call, std::vector<int> uids, size_t index,
                         base::OnceClosure next) {
    auto found = locator_handlers_.find(uids[index]);
    if (!call || found == locator_handlers_.end())
      return;
    in_locator_handler_.push_back(call);
    call->Log("  found " + found->second.text + ", intercepting action to run the handler");
    const std::string selector = found->second.selector;
    const std::string text = found->second.text;
    const bool no_wait_after = found->second.no_wait_after;
    auto finished = base::BindOnce(
        [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call, std::vector<int> uids,
           size_t index, base::OnceClosure next) {
          if (!self || !call)
            return;
          std::erase_if(self->in_locator_handler_, [&](const base::WeakPtr<Call>& each) {
            return !each || each.get() == call.get();
          });
          call->Log("  interception handler has finished, continuing");
          self->CheckLocatorHandler(call, std::move(uids), index + 1, std::move(next));
        },
        weak_factory_.GetWeakPtr(), call, std::move(uids), index, std::move(next));
    found->second.resolved.push_back(base::BindOnce(
        [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call, std::string selector,
           std::string text, bool no_wait_after, base::OnceClosure finished) {
          if (!self || !call)
            return;
          if (no_wait_after) {
            call->Log("  locator handler has finished");
            std::move(finished).Run();
            return;
          }
          call->Log("  locator handler has finished, waiting for " + text + " to be hidden");
          base::DictValue params;
          params.Set("frame", FrameId(self->MainFrame()));
          params.Set("selector", selector);
          params.Set("state", "hidden");
          params.Set("omitReturnValue", true);
          params.Set("timeout", call->Remaining() ? call->Remaining() : kDefaultTimeoutMs);
          self->InnerCall("Frame.waitForSelector", &Automation::WaitForSelector, std::move(params),
                          base::BindOnce(
                              [](base::OnceClosure finished, base::Value, const std::string&) {
                                std::move(finished).Run();
                              },
                              std::move(finished)));
        },
        weak_factory_.GetWeakPtr(), call, selector, text, no_wait_after, std::move(finished)));
  }

  // An action of frames.ts's _retryWithProgressIfNotConnected() or dom.ts's
  // _retryAction(), or an assertion: the locator handlers first.
  template <void (Automation::*method)(Call*)>
  void Prechecked(Call* call) {
    LocatorHandlersCheckpoint(call, base::BindOnce(
                                        [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call) {
                                          if (self && call)
                                            (self.get()->*method)(call.get());
                                        },
                                        weak_factory_.GetWeakPtr(), call->GetWeakPtr()));
  }

  // ---- WebAuthn (server/credentials.ts) --------------------------------

  // install() in this page: the binding in its frames, the script in its
  // documents now and to come.
  void InstallWebAuthn() {
    MainFrame()->ForEachRenderFrameHost([&](content::RenderFrameHost* frame) {
      if (frame->IsRenderFrameLive())
        Agent(frame)->Expose(exposed_, ContextBindings());
    });
    const std::string script(reinterpret_cast<const char*>(kWebAuthnScript), sizeof(kWebAuthnScript));
    zebkit_view_add_script(view_, script.c_str());
    MainFrame()->ForEachRenderFrameHost([&](content::RenderFrameHost* frame) {
      if (!frame->IsRenderFrameLive())
        return;
      Agent(frame)->Evaluate(AutomationWorld::kMain, script, AutomationExpression::kValue, "[]", {}, true, true,
                             base::BindOnce([](AutomationStatus, const std::string&,
                                               std::vector<AutomationHandlePtr>) {}));
    });
  }

  // ---- WebMCP (server/webmcp.ts) ---------------------------------------

  // assertBrowserSupport(): Chromium's WebMCP is on with
  // --enable-features=WebMCP, which Playwright checks for in its launch.
  static bool WebMcpEnabled() {
    const base::FeatureList* features = base::FeatureList::GetInstance();
    return features && features->IsFeatureOverriddenFromCommandLine("WebMCP");
  }

  // @body run with `webmcp`, the frame's WebMCPScript, in its utility world.
  void RunWebMcp(Call* call, const std::string& body, Answerer answer) {
    if (!WebMcpEnabled()) {
      call->Fail("WebMCP is not enabled. Launch the browser with the \"--enable-features=WebMCP\" argument.");
      return;
    }
    content::RenderFrameHost* frame = FrameOf(call);
    if (!frame) {
      call->Fail(kFrameDetached);
      return;
    }
    const std::string expression =
        "(async () => {\nconst webmcp = new (" +
        std::string(reinterpret_cast<const char*>(kWebMcpScript), sizeof(kWebMcpScript)) +
        ")(globalThis);\nconst tools = (await webmcp.tools()).map(tool => {\n"
        "  const annotations = {};\n"
        "  for (const key of ['readOnly', 'untrustedContent', 'consequential'])\n"
        "    if (tool.annotations?.[key]) annotations[key] = true;\n"
        "  return { name: tool.name, description: tool.description,\n"
        "    ...(tool.inputSchema !== undefined ? { inputSchema: tool.inputSchema } : {}),\n"
        "    ...(Object.keys(annotations).length ? { annotations } : {}) };\n"
        "});\n" +
        body + "\n})()";
    Agent(frame)->Evaluate(AutomationWorld::kUtility, expression, AutomationExpression::kValue, "[]", {},
                           true, true, AnswerTo(call, frame->GetGlobalId(), Then(call, std::move(answer))));
  }

  // Frame.webmcpTools: {tools}.
  void WebMcpTools(Call* call) {
    RunWebMcp(call, "return tools;", AsKey("", "tools"));
  }

  // Frame.webmcpCallTool {name, input}: {result}.
  void WebMcpCallTool(Call* call) {
    const base::Value* input = call->params().Find("input");
    const std::string body =
        "const name = " + Json(base::Value(call->String("name"))) +
        ";\nif (!tools.some(tool => tool.name === name)) return { missing: tools.map(tool => tool.name) };\n"
        "return { result: await webmcp.callTool(name, " +
        (input && !input->is_none() ? Json(*input) : std::string("{}")) + ") };";
    RunWebMcp(call, body, base::BindOnce([](Call* call, base::Value value) {
                if (!value.is_dict()) {
                  call->Fail("zebkit: no answer from WebMCP");
                  return;
                }
                if (const base::ListValue* missing = value.GetDict().FindList("missing")) {
                  std::vector<std::string> names;
                  for (const base::Value& name : *missing) {
                    if (name.is_string())
                      names.push_back(name.GetString());
                  }
                  call->Fail("No WebMCP tool named \"" + call->String("name") + "\"." +
                             (names.empty() ? std::string(" The frame does not register any WebMCP tools.")
                                            : " Available tools: " + base::JoinString(names, ", ") + "."));
                  return;
                }
                base::DictValue answer;
                if (base::Value* result = value.GetDict().Find("result"))
                  answer.Set("result", std::move(*result));
                call->Done(base::Value(std::move(answer)));
              }));
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
    // During a drag the page started, Escape cancels it (drag.cc).
    zebkit_key_event drag_key = {};
    drag_key.type = down ? ZEBKIT_KEY_DOWN : ZEBKIT_KEY_UP;
    drag_key.key_code = description.key_code_without_location;
    drag_key.code = description.code;
    drag_key.key = description.key;
    if (DragTakesKey(view_, drag_key))
      return type;
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

  // tracing.ts's input event of a mouse command: where the mouse goes, or
  // is (input.ts's _currentPoint()).
  void TraceMousePoint(Call* call, std::optional<gfx::PointF> to) {
    if (to)
      mouse_in_root_ = *to;
    if (!call->Flag("traceLog"))
      return;
    base::DictValue point;
    point.Set("x", mouse_in_root_.x());
    point.Set("y", mouse_in_root_.y());
    call->params().Set("__inputPoint", std::move(point));
  }

  void MouseMoveCommand(Call* call) {
    const gfx::PointF to(call->Number("x"), call->Number("y"));
    TraceMousePoint(call, to);
    AtPoint(to, base::BindOnce(
                    [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call,
                       gfx::PointF to, content::RenderWidgetHost* widget,
                       gfx::PointF local) {
                      if (!self || !call)
                        return;
                      base::OnceClosure moved = base::BindOnce(
                          [](base::WeakPtr<Automation> self, base::OnceClosure done) {
                            if (self)
                              self->WhenAcked(Type::kMouseMove, std::move(done));
                          },
                          self, self->InputDone(call.get()));
                      self->MouseStepsThen(
                          widget, local, to,
                          std::max(1, static_cast<int>(call->Number("steps", 1))),
                          std::move(moved));
                    },
                    weak_factory_.GetWeakPtr(), call->GetWeakPtr(), to));
  }

  void MouseDownCommand(Call* call) {
    TraceMousePoint(call, std::nullopt);
    MouseDownIn(Widget(), ButtonOf(call->String("button")),
                static_cast<int>(call->Number("clickCount", 1)));
    WhenAcked(Type::kMouseDown, InputDone(call));
  }

  void MouseUpCommand(Call* call) {
    TraceMousePoint(call, std::nullopt);
    MouseUpIn(Widget(), ButtonOf(call->String("button")),
              static_cast<int>(call->Number("clickCount", 1)));
    WhenAcked(Type::kMouseUp, InputDone(call));
  }

  void MouseClickCommand(Call* call) {
    const gfx::PointF to(call->Number("x"), call->Number("y"));
    TraceMousePoint(call, to);
    AtPoint(to, base::BindOnce(
                    [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call,
                       gfx::PointF to, content::RenderWidgetHost* widget,
                       gfx::PointF local) {
                      if (!self || !call)
                        return;
                      self->MouseStepsThen(
                          widget, local, to,
                          std::max(1, static_cast<int>(call->Number("steps", 1))),
                          base::BindOnce(
                              [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call,
                                 content::RenderWidgetHost* widget) {
                                if (!self || !call || self->mouse_widget_.get() != widget)
                                  return;
                                self->PressAndRelease(
                                    call.get(), widget, ButtonOf(call->String("button")),
                                    static_cast<int>(call->Number("clickCount", 1)), 1,
                                    static_cast<int>(call->Number("delay")),
                                    self->InputDone(call.get()));
                              },
                              self, call, widget));
                    },
                    weak_factory_.GetWeakPtr(), call->GetWeakPtr(), to));
  }

  void MouseWheelCommand(Call* call) {
    TraceMousePoint(call, std::nullopt);
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
    if (!HasTouch()) {
      call->Fail("hasTouch must be enabled on the browser context before using the touchscreen.");
      return;
    }
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

  // input.ts's move(): each step acknowledged before the next, as each CDP call of
  // it is; sent at once, the input queue coalesces them into the last.
  void MouseStepsThen(content::RenderWidgetHost* widget,
                      const gfx::PointF& to,
                      const gfx::PointF& root,
                      int steps,
                      base::OnceClosure then) {
    if (steps <= 1) {
      MouseMoveIn(widget, to, root, 1);
      std::move(then).Run();
      return;
    }
    const gfx::PointF from = widget == Widget() ? mouse_in_widget_ : to;
    mouse_widget_ = widget;
    mouse_in_widget_ = to;
    mouse_ = root;
    NextMouseStep(widget, from, to, 1, steps, std::move(then));
  }

  void NextMouseStep(content::RenderWidgetHost* widget,
                     gfx::PointF from,
                     gfx::PointF to,
                     int step,
                     int steps,
                     base::OnceClosure then) {
    // The widget went while it was waited for (RenderWidgetHostDestroyed).
    if (mouse_widget_.get() != widget || step > steps) {
      std::move(then).Run();
      return;
    }
    const gfx::PointF middle(from.x() + (to.x() - from.x()) * step / steps,
                             from.y() + (to.y() - from.y()) * step / steps);
    SendMouse(widget, Type::kMouseMove, middle, last_button_, 0);
    WhenAcked(Type::kMouseMove,
              base::BindOnce(&Automation::NextMouseStep, weak_factory_.GetWeakPtr(), widget, from,
                             to, step + 1, steps, std::move(then)));
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
    // In the environment of the test that shows what the wait is for, not a switch
    // of the engine's that an application could pass on.
    if (!widget || getenv("ZEBKIT_TEST_NO_INPUT_WAIT")) {
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
    // A drag the page started goes on from these, over the page (drag.cc),
    // as an application's mouse events carry it: no event reaches the page
    // for them, so nothing waits for an acknowledgement.
    if (type == Type::kMouseMove || type == Type::kMouseUp) {
      gfx::PointF root = point;
      if (content::RenderWidgetHostView* view = widget->GetView())
        root = view->TransformPointToRootCoordSpaceF(point);
      if (DragTakesMouse(view_, type == Type::kMouseMove ? ZEBKIT_MOUSE_MOVE : ZEBKIT_MOUSE_UP,
                         root.x(), root.y(), 0)) {
        return;
      }
      NoteMousePosition(view_, root.x(), root.y());
    }
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
  // Page.screenshot, ElementHandle.screenshot: as screenshotter.ts, over
  // Page.captureScreenshot (formats, clips beyond the view, scales).
  void Screenshot(Call* call) {
    // zebkit_automation_screenshot_pixels(): the pixels, in no format.
    if (call->WantsPixels() && (call->Has("type") || call->Has("quality") || call->Has("path"))) {
      call->Fail("zebkit: options.type, quality and path are not for the pixels");
      return;
    }
    // screenshotter.ts's validateScreenshotOptions().
    const std::string type = call->String("type");
    if (!type.empty() && type != "png" && type != "jpeg" && type != "webp") {
      call->Fail("Unknown options.type value: " + type);
      return;
    }
    if (call->Has("quality")) {
      const double quality = call->Number("quality");
      if (type.empty() || type == "png") {
        call->Fail("options.quality is unsupported for the png screenshots");
        return;
      }
      if (quality != static_cast<int>(quality)) {
        call->Fail("Expected options.quality to be an integer");
        return;
      }
      if (quality < 0 || quality > 100) {
        call->Fail("Expected options.quality to be between 0 and 100 (inclusive), got " +
                   base::NumberToString(static_cast<int>(quality)));
        return;
      }
    }
    if (const base::DictValue* clip = call->params().FindDict("clip")) {
      if (clip->FindDouble("width").value_or(0) <= 0) {
        call->Fail("Expected options.clip.width to be greater than 0.");
        return;
      }
      if (clip->FindDouble("height").value_or(0) <= 0) {
        call->Fail("Expected options.clip.height to be greater than 0.");
        return;
      }
    }
    // screenshotter.ts's queue: a page's screenshots one at a time (each
    // one's styles and masks are undone before the next's).
    if (screenshot_current_ && screenshot_current_.get() != call && !screenshot_current_->finished()) {
      base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE,
          base::BindOnce(
              [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call) {
                if (self && call && !call->finished())
                  self->Screenshot(call.get());
              },
              weak_factory_.GetWeakPtr(), call->GetWeakPtr()),
          base::Milliseconds(5));
      return;
    }
    screenshot_current_ = call->GetWeakPtr();
    // screenshotter.ts's log.
    call->Log(call->OnHandle() ? "taking element screenshot" : "taking page screenshot");
    if (call->String("animations") == "disabled")
      call->Log("  disabled all CSS animations");
    call->Log("waiting for fonts to load...");
    ScreenshotStyled(call, base::BindOnce(&Automation::ScreenshotPrepared,
                                          weak_factory_.GetWeakPtr(),
                                          call->GetWeakPtr()));
  }

  // ---- Page.expectScreenshot (page.ts's expectScreenshot()) -----------

  struct ScreenshotExpectation {
    std::optional<std::string> expected;
    std::optional<std::string> actual;
    std::optional<std::string> previous;
    bool first = true;
    size_t tries = 0;
    ImageCompareOptions options;
    // The last comparison that failed: its message, diff, actual, previous.
    std::optional<std::string> message;
    std::optional<std::string> diff;
    std::optional<std::string> failed_actual;
    std::optional<std::string> failed_previous;
  };

  void ExpectScreenshot(Call* call) {
    auto expectation = std::make_shared<ScreenshotExpectation>();
    if (const std::string* title = call->params().FindString("expectTitle")) {
      call->Log(*title + (call->timeout_ms() > 0
                              ? " with timeout " + base::NumberToString(call->timeout_ms()) + "ms"
                              : std::string()));
    }
    if (const std::string* expected = call->params().FindString("expected")) {
      std::string bytes;
      if (base::Base64Decode(*expected, &bytes))
        expectation->expected = std::move(bytes);
    }
    expectation->options.threshold = call->params().FindDouble("threshold");
    expectation->options.max_diff_pixels = call->params().FindDouble("maxDiffPixels");
    expectation->options.max_diff_pixel_ratio = call->params().FindDouble("maxDiffPixelRatio");
    expectation->options.comparator = call->String("comparator");
    if (!expectation->expected && call->Flag("isNot")) {
      ExpectScreenshotFailed(call->GetWeakPtr(), expectation, "\"not\" matcher requires expected result",
                             false);
      return;
    }
    call->OnTimeout(base::BindOnce(
        [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call,
           std::shared_ptr<ScreenshotExpectation> expectation) {
          if (!self || !call)
            return;
          const std::string timeout = "Timeout " + base::NumberToString(call->timeout_ms()) + "ms exceeded.";
          self->ExpectScreenshotFailed(call, expectation,
                                       expectation->failed_previous
                                           ? std::string("Failed to take two consecutive stable screenshots.")
                                           : timeout,
                                       true, timeout);
        },
        weak_factory_.GetWeakPtr(), call->GetWeakPtr(), expectation));
    call->Log(expectation->expected ? "  verifying given screenshot expectation"
                                    : "  generating new stable screenshot expectation");
    ExpectScreenshotTry(call->GetWeakPtr(), expectation);
  }

  void ExpectScreenshotTry(base::WeakPtr<Call> call, std::shared_ptr<ScreenshotExpectation> expectation) {
    if (!call)
      return;
    if (Closing()) {
      ExpectScreenshotFailed(call, expectation, "The page has closed", false);
      return;
    }
    static constexpr int kWaits[] = {0, 100, 250, 500};
    const int wait = expectation->tries < std::size(kWaits) ? kWaits[expectation->tries] : 1000;
    ++expectation->tries;
    if (wait)
      call->Log("waiting " + base::NumberToString(wait) + "ms before taking screenshot");
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&Automation::ExpectScreenshotTake, weak_factory_.GetWeakPtr(), call,
                       expectation),
        base::Milliseconds(wait));
  }

  void ExpectScreenshotTake(base::WeakPtr<Call> call, std::shared_ptr<ScreenshotExpectation> expectation) {
    if (!call)
      return;
    base::DictValue params = call->params().Clone();
    for (const char* key : {"expected", "isNot", "locator", "comparator", "threshold", "maxDiffPixels",
                            "maxDiffPixelRatio", "expectTitle", "abortId", "timeout"}) {
      params.Remove(key);
    }
    params.Set("timeout", call->Remaining() ? call->Remaining() : kDefaultTimeoutMs);
    const base::DictValue* locator = call->params().FindDict("locator");
    if (!locator) {
      InnerCall("Page.screenshot", &Automation::Screenshot, std::move(params),
                base::BindOnce(&Automation::ExpectScreenshotTaken, weak_factory_.GetWeakPtr(), call,
                               expectation));
      return;
    }
    // rafrafTimeoutScreenshotElementWithProgress(): the locator's element,
    // then its screenshot.
    base::DictValue wait;
    if (const std::string* frame = locator->FindString("frame"))
      wait.Set("frame", *frame);
    const std::string* selector = locator->FindString("selector");
    wait.Set("selector", selector ? *selector : std::string());
    wait.Set("strict", true);
    wait.Set("state", "visible");
    wait.Set("timeout", call->Remaining() ? call->Remaining() : kDefaultTimeoutMs);
    InnerCall(
        "Frame.waitForSelector", &Automation::WaitForSelector, std::move(wait),
        base::BindOnce(
            [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call,
               std::shared_ptr<ScreenshotExpectation> expectation, base::DictValue params, base::Value result,
               const std::string& error) {
              if (!self || !call)
                return;
              const std::string* handle =
                  result.is_dict() ? result.GetDict().FindStringByDottedPath("element.handle") : nullptr;
              if (!handle) {
                self->ExpectScreenshotTaken(call, expectation, base::Value(),
                                            error.empty() ? std::string("no element") : error);
                return;
              }
              const std::string id = *handle;
              params.Set("handle", id);
              self->InnerCall("ElementHandle.screenshot", &Automation::Screenshot, std::move(params),
                              base::BindOnce(
                                  [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call,
                                     std::shared_ptr<ScreenshotExpectation> expectation, std::string id,
                                     base::Value result, const std::string& error) {
                                    if (!self)
                                      return;
                                    self->Dispose(id);
                                    self->ExpectScreenshotTaken(call, expectation, std::move(result), error);
                                  },
                                  self, call, expectation, id));
            },
            weak_factory_.GetWeakPtr(), call, expectation, std::move(params)));
  }

  // areEqualScreenshots(): whether @actual and @expected are as the
  // assertion wants (equal, or not with isNot), keeping the failed result.
  bool ScreenshotsAgree(Call* call, ScreenshotExpectation& expectation, const std::string& actual,
                        const std::string* expected, const std::string* previous, std::string* error) {
    if (!expected)
      return false;
    std::optional<ImageDifference> difference =
        CompareImages(base::as_byte_span(actual), base::as_byte_span(*expected), expectation.options, error);
    if (!error->empty())
      return false;
    if (difference.has_value() == call->Flag("isNot"))
      return true;
    if (difference) {
      expectation.message = difference->message;
      expectation.diff = std::string(difference->diff.begin(), difference->diff.end());
      expectation.failed_actual = actual;
      expectation.failed_previous = previous ? std::optional<std::string>(*previous) : std::nullopt;
    }
    return false;
  }

  void ExpectScreenshotTaken(base::WeakPtr<Call> call, std::shared_ptr<ScreenshotExpectation> expectation,
                             base::Value result, const std::string& error) {
    if (!call)
      return;
    const std::string* binary = result.is_dict() ? result.GetDict().FindString("binary") : nullptr;
    std::string bytes;
    if (!error.empty() || !binary || !base::Base64Decode(*binary, &bytes)) {
      call->Log("failed to take screenshot - " + (error.empty() ? std::string("no picture") : error));
      ExpectScreenshotTry(call, expectation);
      return;
    }
    expectation->previous = std::move(expectation->actual);
    expectation->actual = std::move(bytes);
    const std::string* against = expectation->expected && expectation->first
                                     ? &*expectation->expected
                                     : (expectation->previous ? &*expectation->previous : nullptr);
    std::string compare_error;
    const bool agree = ScreenshotsAgree(call.get(), *expectation, *expectation->actual, against,
                                        expectation->previous ? &*expectation->previous : nullptr,
                                        &compare_error);
    if (!compare_error.empty()) {
      ExpectScreenshotFailed(call, expectation, compare_error, false);
      return;
    }
    if (!agree) {
      if (expectation->message)
        call->Log(*expectation->message);
      expectation->first = false;
      ExpectScreenshotTry(call, expectation);
      return;
    }
    if (!expectation->first)
      call->Log("captured a stable screenshot");
    if (!expectation->expected) {
      base::DictValue answer;
      answer.Set("actual", base::Base64Encode(*expectation->actual));
      call->Done(base::Value(std::move(answer)));
      return;
    }
    if (!expectation->first &&
        !ScreenshotsAgree(call.get(), *expectation, *expectation->actual, &*expectation->expected, nullptr,
                          &compare_error)) {
      ExpectScreenshotFailed(call, expectation,
                             compare_error.empty() ? expectation->message.value_or("") : compare_error, false);
      return;
    }
    call->Log("screenshot matched expectation");
    call->Done();
  }

  // page.ts's catch: the error with the screenshots, for the client's diff.
  void ExpectScreenshotFailed(base::WeakPtr<Call> call, std::shared_ptr<ScreenshotExpectation> expectation,
                              const std::string& message, bool timed_out,
                              const std::string& last_line = std::string()) {
    if (!call)
      return;
    base::DictValue answer;
    answer.Set("expectScreenshotFailed", true);
    answer.Set("errorMessage", message);
    answer.Set("timedOut", timed_out);
    if (expectation->failed_actual)
      answer.Set("actual", base::Base64Encode(*expectation->failed_actual));
    if (expectation->failed_previous)
      answer.Set("previous", base::Base64Encode(*expectation->failed_previous));
    if (expectation->diff)
      answer.Set("diff", base::Base64Encode(*expectation->diff));
    base::ListValue log;
    for (const std::string& line : call->log())
      log.Append(line);
    log.Append(last_line.empty() ? message : last_line);
    answer.Set("log", std::move(log));
    call->Done(base::Value(std::move(answer)));
  }

  void ScreenshotStyled(Call* call, base::OnceClosure next) {
    base::DictValue arg;
    for (const char* key : {"caret", "animations", "style", "maskColor"}) {
      if (const std::string* value = call->params().FindString(key))
        arg.Set(key, *value);
    }
    // Those in frames are masked in their frames, once the page's are.
    std::vector<std::string> in_frames;
    if (const base::ListValue* mask = call->params().FindList("mask")) {
      base::ListValue selectors;
      for (const base::Value& item : *mask) {
        const std::string* selector =
            item.is_dict() ? item.GetDict().FindString("selector") : nullptr;
        if (!selector)
          continue;
        if (selector->find(kEnterFrame) != std::string::npos)
          in_frames.push_back(*selector);
        else
          selectors.Append(*selector);
      }
      arg.Set("mask", std::move(selectors));
    }
    RunStep(call, Follow(MainFrame()), AutomationWorld::kUtility,
            "prepareScreenshot", std::move(arg), {},
            base::BindOnce(
                [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call,
                   std::vector<std::string> in_frames, base::OnceClosure next, base::Value) {
                  if (!self || !call)
                    return;
                  call->Log("fonts loaded");
                  self->MaskInFrames(call, std::move(in_frames), std::move(next));
                },
                weak_factory_.GetWeakPtr(), call->GetWeakPtr(), std::move(in_frames), std::move(next)));
  }

  // A mask's selector through frames: its frames entered as a query enters
  // them (no wait), then its elements masked in the last one.
  void MaskInFrames(base::WeakPtr<Call> call, std::vector<std::string> selectors, base::OnceClosure next) {
    if (!call)
      return;
    if (selectors.empty()) {
      call->params().Remove("__noFrameWait");
      std::move(next).Run();
      return;
    }
    const std::string selector = selectors.back();
    selectors.pop_back();
    std::vector<std::string> chunks;
    size_t start = 0;
    for (size_t at; (at = selector.find(kEnterFrame, start)) != std::string::npos;) {
      chunks.push_back(selector.substr(start, at - start));
      start = at + sizeof(kEnterFrame) - 1;
    }
    Target target;
    target.frame = Follow(MainFrame());
    target.selector = selector.substr(start);
    call->params().Set("__noFrameWait", true);
    masked_in_frames_ = true;
    EnterFrames(
        call, std::move(target), std::move(chunks), 0,
        base::BindOnce(
            [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call, std::vector<std::string> selectors,
               std::string color, base::OnceClosure next, Target target) {
              if (!self || !call)
                return;
              base::DictValue arg;
              arg.Set("selector", target.selector);
              arg.Set("color", color);
              self->RunStep(call.get(), target.frame, AutomationWorld::kUtility, "maskElements", std::move(arg), {},
                            base::BindOnce(
                                [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call,
                                   std::vector<std::string> selectors, base::OnceClosure next, base::Value) {
                                  if (self)
                                    self->MaskInFrames(call, std::move(selectors), std::move(next));
                                },
                                self, call, std::move(selectors), std::move(next)));
            },
            weak_factory_.GetWeakPtr(), call, std::move(selectors),
            call->params().FindString("maskColor") ? *call->params().FindString("maskColor") : std::string("#F0F"),
            std::move(next)));
  }

  void ScreenshotPrepared(base::WeakPtr<Call> call) {
    if (!call)
      return;
    if (call->OnHandle()) {
      StepOnTarget(call.get(), "scrollIntoViewForScreenshot", {},
                   base::BindOnce(
                       [](base::WeakPtr<Automation> self, Call* call,
                          base::Value) {
                         if (self)
                           self->OnTarget(call, &Automation::ScreenshotElement);
                       },
                       weak_factory_.GetWeakPtr()));
      return;
    }
    std::optional<gfx::RectF> clip;
    if (const base::DictValue* given = call->params().FindDict("clip")) {
      clip = gfx::RectF(given->FindDouble("x").value_or(0),
                        given->FindDouble("y").value_or(0),
                        given->FindDouble("width").value_or(0),
                        given->FindDouble("height").value_or(0));
    }
    ScreenshotWithMetrics(call, clip);
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
                call->Fail("Node is either not visible or not an HTMLElement");
                return;
              }
              if (box.GetDict().FindDouble("width").value_or(0) == 0) {
                call->Fail("Node has 0 width.");
                return;
              }
              if (box.GetDict().FindDouble("height").value_or(0) == 0) {
                call->Fail("Node has 0 height.");
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
                          self->ScreenshotWithMetrics(call, gfx::RectF(root, size));
                        }
                      },
                      self, call, size));
            },
            weak_factory_.GetWeakPtr(), call, target.frame));
  }

  // The clip, in the viewport's coordinates, as Page.captureScreenshot's in
  // the document's; the whole document for fullPage; none for the viewport.
  void ScreenshotWithMetrics(base::WeakPtr<Call> call,
                             std::optional<gfx::RectF> clip) {
    if (!call)
      return;
    RunStep(call.get(), Follow(MainFrame()), AutomationWorld::kUtility,
            "screenshotMetrics", {}, {},
            base::BindOnce(&Automation::ScreenshotMeasured,
                           weak_factory_.GetWeakPtr(), call, clip));
  }

  void ScreenshotMeasured(base::WeakPtr<Call> call,
                          std::optional<gfx::RectF> clip,
                          base::Value metrics) {
    if (!call)
      return;
    if (!metrics.is_dict()) {
      // A document that went meanwhile (a navigation): measured again in the
      // next, as waitForFunctionValueInUtility() polls.
      if (!call->Expired()) {
        base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
            FROM_HERE, base::BindOnce(&Automation::ScreenshotWithMetrics, weak_factory_.GetWeakPtr(), call, clip),
            base::Milliseconds(20));
        return;
      }
      RestoreScreenshot(call, std::string(), SkBitmap(), "zebkit: no metrics of the page");
      return;
    }
    const base::DictValue& m = metrics.GetDict();
    const double scroll_x = m.FindDouble("scrollX").value_or(0);
    const double scroll_y = m.FindDouble("scrollY").value_or(0);
    // screenshotter.ts: the emulated viewport's size when there is one (a
    // mobile page's layout may be wider), else the window's.
    double viewport_width = m.FindDouble("viewportWidth").value_or(0);
    double viewport_height = m.FindDouble("viewportHeight").value_or(0);
    if (viewport_) {
      viewport_width = viewport_->width();
      viewport_height = viewport_->height();
    } else if (const base::DictValue* given = Context().options.FindDict("viewport")) {
      viewport_width = given->FindInt("width").value_or(static_cast<int>(viewport_width));
      viewport_height = given->FindInt("height").value_or(static_cast<int>(viewport_height));
    }
    const double scale_factor = m.FindDouble("deviceScaleFactor").value_or(1);
    // screenshotter.ts and crPage.ts's takeScreenshot(): a clip always, in the
    // document's coordinates; beyond the viewport only when it does not fit.
    auto enclosing = [](const gfx::RectF& rect) {
      const double x = std::floor(rect.x() + 1e-3), y = std::floor(rect.y() + 1e-3);
      const double x2 = std::ceil(rect.right() - 1e-3), y2 = std::ceil(rect.bottom() - 1e-3);
      return gfx::RectF(x, y, x2 - x, y2 - y);
    };
    // trimClipToSize().
    auto trim = [](const gfx::RectF& clip, double width, double height) {
      const double x1 = std::max(0.0, std::min<double>(clip.x(), width));
      const double y1 = std::max(0.0, std::min<double>(clip.y(), height));
      const double x2 = std::max(0.0, std::min<double>(clip.right(), width));
      const double y2 = std::max(0.0, std::min<double>(clip.bottom(), height));
      return gfx::RectF(x1, y1, x2 - x1, y2 - y1);
    };
    gfx::RectF document;
    bool fits = true;
    double visual_scale = 1;
    if (call->OnHandle()) {
      // The element's box, of the viewport, moved by the scroll.
      document = enclosing(gfx::RectF(clip->x() + scroll_x, clip->y() + scroll_y,
                                      clip->width(), clip->height()));
      fits = clip->width() <= viewport_width && clip->height() <= viewport_height;
    } else if (call->Flag("fullPage")) {
      const double width = m.FindDouble("documentWidth").value_or(0);
      const double height = m.FindDouble("documentHeight").value_or(0);
      document = gfx::RectF(0, 0, width, height);
      fits = width <= viewport_width && height <= viewport_height;
      if (clip)
        document = trim(*clip, width, height);
    } else {
      // crPage.ts: the visual viewport's, at its scale (a zoomed-out mobile
      // layout is taken as it shows).
      const gfx::RectF view = clip ? trim(*clip, viewport_width, viewport_height)
                                   : gfx::RectF(0, 0, viewport_width, viewport_height);
      visual_scale = m.FindDouble("visualScale").value_or(1);
      if (visual_scale <= 0)
        visual_scale = 1;
      document = gfx::RectF(m.FindDouble("visualPageX").value_or(scroll_x) + view.x(),
                            m.FindDouble("visualPageY").value_or(scroll_y) + view.y(),
                            std::ceil(view.width() / visual_scale - 1e-3),
                            std::ceil(view.height() / visual_scale - 1e-3));
    }
    if (document.width() <= 0 || document.height() <= 0) {
      RestoreScreenshot(call, std::string(), SkBitmap(),
                        "Clipped area is either empty or outside the resulting image");
      return;
    }
    const bool pixels = call->WantsPixels();
    const std::string type = pixels ? "zebkit-raw" : call->String("type").empty() ? "png" : call->String("type");
    base::DictValue params;
    params.Set("format", type);
    // jpeg 80 and webp 100 (lossless) unless given, as screenshotter.ts.
    if (type == "jpeg" || type == "webp")
      params.Set("quality", call->Has("quality") ? static_cast<int>(call->Number("quality"))
                                                 : type == "jpeg" ? 80 : 100);
    // A PNG at deflate level 1 with the Up filter (Chromium's PNG for
    // screencasts): 4 ms where Chrome's Page.captureScreenshot default
    // takes 16 for a 1280 by 800 page, about half as many bytes again
    // (docs/AUTOMATION.md, 6). The same pixels.
    if (type == "png")
      params.Set("optimizeForSpeed", true);
    params.Set("captureBeyondViewport", !fits);
    base::DictValue region;
    region.Set("x", document.x());
    region.Set("y", document.y());
    region.Set("width", document.width());
    region.Set("height", document.height());
    region.Set("scale", call->String("scale") == "css" ? visual_scale / scale_factor : visual_scale);
    params.Set("clip", std::move(region));
    // jpeg has no transparency.
    const bool transparent = call->Flag("omitBackground") && type != "jpeg";
    if (transparent) {
      base::DictValue color;
      color.Set("r", 0);
      color.Set("g", 0);
      color.Set("b", 0);
      color.Set("a", 0);
      base::DictValue background;
      background.Set("color", std::move(color));
      call->view()->Send("Emulation.setDefaultBackgroundColorOverride",
                         std::move(background));
    }
    call->view()->Send(
        "Page.captureScreenshot", std::move(params),
        base::BindOnce(
            [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call,
               base::WeakPtr<zebkit_view> view, bool transparent, bool beyond,
               std::optional<gfx::RectF> clip, bool pixels, const base::DictValue* result,
               const std::string& error) {
              // Also when the call has timed out: the page keeps no override.
              if (transparent && view) {
                view->Send("Emulation.setDefaultBackgroundColorOverride",
                           base::DictValue());
              }
              // crPage.ts: capturing beyond the viewport resets Chromium's
              // touch emulation, which goes back.
              if (beyond && view && self && self->TouchEmulated()) {
                base::DictValue touch;
                touch.Set("enabled", true);
                view->Send("Emulation.setTouchEmulationEnabled", std::move(touch));
              }
              const std::string* data = result ? result->FindString("data") : nullptr;
              // The pixels kept for "zebkit-raw" are taken at once, whether
              // the call is still there or not.
              SkBitmap bitmap;
              if (data && pixels) {
                std::string key;
                if (base::Base64Decode(*data, &key))
                  bitmap = content::protocol::TakeZebkitRawScreenshot(key);
              }
              if (!self || !call)
                return;
              // Between two documents of a navigation the page's target takes
              // no capture: again once the next has committed, as Chrome's
              // capture waits for a frame of the page.
              if (!data && error.find("Not attached to an active page") != std::string::npos) {
                base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
                    FROM_HERE,
                    base::BindOnce(&Automation::ScreenshotWithMetrics, self, call, clip),
                    base::Milliseconds(30));
                return;
              }
              if (pixels && data && bitmap.drawsNothing()) {
                self->RestoreScreenshot(call, std::string(), SkBitmap(), "zebkit: no picture");
                return;
              }
              self->RestoreScreenshot(
                  call, data && !pixels ? *data : std::string(), std::move(bitmap),
                  data ? std::string()
                       : (error.empty() ? std::string("zebkit: no picture") : error));
            },
            weak_factory_.GetWeakPtr(), call, call->view()->GetWeakPtr(), transparent, !fits,
            clip, pixels));
  }

  // The page as it was, then the answer: @base64 (@bitmap for the pixels)
  // or @error.
  void RestoreScreenshot(base::WeakPtr<Call> call, std::string base64, SkBitmap bitmap, std::string error) {
    if (!call)
      return;
    // The masks in frames, undone in each frame.
    if (masked_in_frames_) {
      masked_in_frames_ = false;
      MainFrame()->ForEachRenderFrameHost([&](content::RenderFrameHost* frame) {
        if (frame != MainFrame() && frame->IsRenderFrameLive())
          RunStep(call.get(), Follow(frame), AutomationWorld::kUtility, "restoreScreenshot", {}, {},
                  base::BindOnce([](base::Value) {}));
      });
    }
    RunStep(call.get(), Follow(MainFrame()), AutomationWorld::kUtility,
            "restoreScreenshot", {}, {},
            base::BindOnce(
                [](base::WeakPtr<Call> call, std::string base64, SkBitmap bitmap, std::string error,
                   base::Value) {
                  if (!call)
                    return;
                  if (!error.empty()) {
                    call->Fail(error);
                    return;
                  }
                  if (call->WantsPixels()) {
                    DeliverPixels(call, bitmap);
                    return;
                  }
                  std::string bytes;
                  if (!base::Base64Decode(base64, &bytes)) {
                    call->Fail("zebkit: the picture could not be decoded");
                    return;
                  }
                  Deliver(call, std::vector<uint8_t>(bytes.begin(), bytes.end()));
                },
                call, std::move(base64), std::move(bitmap), std::move(error)));
  }

  // zebkit_automation_screenshot_pixels()'s answer: the picture (Skia's N32,
  // premultiplied) as RGBA unpremultiplied, the PNG's pixels.
  static void DeliverPixels(base::WeakPtr<Call> call, const SkBitmap& bitmap) {
    const SkImageInfo info = SkImageInfo::Make(bitmap.width(), bitmap.height(), kRGBA_8888_SkColorType,
                                               kUnpremul_SkAlphaType);
    // Kept for the next (the engine's thread's only): memory already made
    // costs no page faults, a millisecond for 1280 by 800; a picture over
    // 16 MiB gets its own.
    static base::NoDestructor<std::vector<uint8_t>> kept;
    std::vector<uint8_t> own;
    const size_t size = info.computeMinByteSize();
    std::vector<uint8_t>& pixels = size <= 16u << 20 ? *kept : own;
    pixels.resize(size);
    if (bitmap.drawsNothing() || pixels.empty() ||
        !bitmap.readPixels(info, pixels.data(), info.minRowBytes(), 0, 0)) {
      call->Fail("zebkit: no picture");
      return;
    }
    call->DonePixels(pixels.data(), info.width(), info.height(), static_cast<int>(info.minRowBytes()));
  }

  // The picture to the client or to "path".
  static void Deliver(base::WeakPtr<Call> call, std::vector<uint8_t> bytes) {
    const std::string path = call->String("path");
    if (path.empty()) {
      base::DictValue answer;
      answer.Set("binary", base::Base64Encode(bytes));
      call->Done(base::Value(std::move(answer)));
      return;
    }
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock()},
        base::BindOnce(
            [](std::string path, std::vector<uint8_t> bytes) {
              return base::WriteFile(base::FilePath::FromUTF8Unsafe(path), bytes);
            },
            path, std::move(bytes)),
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

  // Frame.frameElement: the element of the frame in its parent's document,
  // found by the parent's script (what crPage.ts asks DOM.getFrameOwner).
  void FrameElement(Call* call) {
    content::RenderFrameHost* frame = FrameOf(call);
    content::RenderFrameHost* parent = frame ? frame->GetParent() : nullptr;
    if (!parent) {
      call->Fail(kFrameDetachedDot);
      return;
    }
    const std::vector<content::RenderFrameHost*> children = Children(parent);
    const auto found = std::find(children.begin(), children.end(), frame);
    // The agent gives the step the child's element (automation.mojom's Step),
    // in a shadow tree too, where window.frames does not count it.
    base::DictValue owner;
    owner.Set("$h", 0);
    base::DictValue arg;
    arg.Set("owner", std::move(owner));
    RunStep(call, Follow(parent), AutomationWorld::kUtility, "frameOwner",
            std::move(arg), {-1 - static_cast<int>(found - children.begin())},
            base::BindOnce(
                [](base::WeakPtr<Call> call, Answerer answer, base::Value value) {
                  if (call)
                    std::move(answer).Run(call.get(), std::move(value));
                },
                call->GetWeakPtr(), AsKey("element", "element")));
  }

  // Frame.nodeHandle {backendNodeId}: {element}, the frame's node DevTools
  // names (crPage.ts's adoptBackendNodeId()), a node out of the document too.
  void NodeHandle(Call* call) {
    content::RenderFrameHost* frame = FrameOf(call);
    if (!frame) {
      call->Fail(kFrameDetached);
      return;
    }
    const int node = static_cast<int>(call->Number("backendNodeId"));
    base::DictValue owner;
    owner.Set("$h", 0);
    base::DictValue arg;
    arg.Set("owner", std::move(owner));
    RunStep(call, Follow(frame), AutomationWorld::kUtility, "chooserInput", std::move(arg),
            {kDomNodeHandleBase - node},
            base::BindOnce(
                [](base::WeakPtr<Call> call, base::Value value) {
                  if (call)
                    call->Done(std::move(value));
                },
                call->GetWeakPtr()));
  }

  // Page.requestGC, as crPage.ts.
  void RequestGC(Call* call) {
    call->view()->Send("HeapProfiler.collectGarbage", base::DictValue(),
                       base::BindOnce(
                           [](base::WeakPtr<Call> call, const base::DictValue*,
                              const std::string& error) {
                             if (!call)
                               return;
                             if (error.empty())
                               call->Done();
                             else
                               call->Fail(error);
                           },
                           call->GetWeakPtr()));
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
    viewport_ = gfx::Size(width, height);
    // page.ts's setViewportSize(): the screen the same size.
    if (!ContextStates()[web_contents()->GetBrowserContext()].options.empty())
      EmulateMetrics(width, height, width, height);
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
        }, weak_factory_.GetWeakPtr(), call->GetWeakPtr(), id), /*all_frames=*/true,
        /*run_immediately=*/false);
  }

  void RemoveRegistration(Call* call) {
    const std::string id = call->String("registration");
    if (auto binding = binding_registrations_.find(id); binding != binding_registrations_.end()) {
      std::erase(exposed_, binding->second);
      binding_registrations_.erase(binding);
      MainFrame()->ForEachRenderFrameHost([&](content::RenderFrameHost* frame) {
        if (frame->IsRenderFrameLive())
          Agent(frame)->Expose(exposed_, ContextBindings());
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
    // page.close({runBeforeUnload: true}): the page's beforeunload handlers
    // decide (their dialog is the client's), as crPage.ts's Page.close does.
    if (call->Flag("runBeforeUnload")) {
      RunBeforeUnload(call);
      return;
    }
    zebkit_view* view = call->view();
    call->Done();
    zebkit_view_close(view);
  }

  // page.close({runBeforeUnload: true}): the page's handlers decide, and it
  // goes on its own when they let it.
  void RunBeforeUnload(Call* call) {
    // The view's own request to close (dialog.cc): its beforeunload dialog is
    // held for the client, and the view closes once the handlers let it.
    zebkit_view* view = call->view();
    call->Done();
    zebkit_view_request_close(view);
  }

  // Page.webStorage*: of the main frame's origin.
  void WebStorage(Call* call) {
    base::DictValue arg;
    for (const char* key : {"kind", "name", "value"}) {
      if (const std::string* value = call->params().FindString(key))
        arg.Set(key, *value);
    }
    arg.Set("command", call->Name());
    RunStep(call, Follow(MainFrame()), AutomationWorld::kUtility, "webStorage",
            std::move(arg), {},
            base::BindOnce(
                [](base::WeakPtr<Call> call, base::Value value) {
                  if (call)
                    call->Done(std::move(value));
                },
                call->GetWeakPtr()));
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
    if (!network->SetPatterns(patterns ? patterns->Clone() : base::ListValue(),
                              call->Flag("ofContext") ||
                                  call->method().starts_with("BrowserContext."),
                              &error)) {
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
    page_extra_headers_ = headers ? headers->Clone() : base::ListValue();
    ApplyExtraHeaders();
    call->Done();
  }

  void RouteContinue(Call* call) {
    Network* network = NetworkOf(call);
    // network.ts's Route.continue().
    if (network && call->Has("url")) {
      const std::optional<std::string> old_url = network->RouteUrl(call->String("route"));
      if (old_url && GURL(*old_url).scheme() != GURL(call->String("url")).scheme()) {
        call->Fail("New URL must have same protocol as overridden URL");
        return;
      }
    }
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
    network->WhenBody(
        call->String("request"),
        base::BindOnce(
            [](base::WeakPtr<Call> call, std::optional<std::string> body) {
              if (!call)
                return;
              if (!body) {
                call->Fail("Response body is unavailable for redirect responses, or it has not arrived yet");
                return;
              }
              base::DictValue answer;
              answer.Set("binary", base::Base64Encode(*body));
              call->Done(base::Value(std::move(answer)));
            },
            call->GetWeakPtr()));
  }

  void SetOffline(Call* call) {
    if (NetworkOf(call)) {
      EmulateOffline(call->Flag("offline"));
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
        Agent(frame)->Expose(exposed_, ContextBindings());
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
                     const std::string& serialized,
                     BindingCalledCallback callback) override {
    content::RenderFrameHost* frame =
        content::RenderFrameHost::FromID(hosts_.current_context());
    if (name == kWebSocketBinding) {
      std::optional<base::Value> parsed = Parse(args);
      WebSocketReport(frame, parsed ? std::move(*parsed) : base::Value());
      std::move(callback).Run(true, "null", false, {});
      return;
    }
    if (name == kWebAuthnBinding) {
      std::optional<base::Value> parsed = Parse(args);
      const base::DictValue* request = parsed && parsed->is_list() && !parsed->GetList().empty()
                                           ? parsed->GetList()[0].GetIfDict()
                                           : nullptr;
      base::DictValue answer =
          request ? PasskeyRequest(Context().passkeys, *request) : PasskeyRequest(Context().passkeys, base::DictValue());
      std::move(callback).Run(true, Json(base::Value(std::move(answer))), false, {});
      return;
    }
    std::optional<base::Value> serialized_args = Parse(serialized);
    // page.ts's PageBinding.dispatch(), on what a page's toJSON made of them.
    if (!serialized.empty() && (!serialized_args || !serialized_args->is_list())) {
      std::move(callback).Run(false,
                              "serializedArgs is not an array. This can happen when "
                              "Array.prototype.toJSON is defined incorrectly",
                              false, {});
      return;
    }
    const std::string id = "b" + base::NumberToString(++next_binding_);
    bindings_[id] = std::move(callback);
    base::DictValue params;
    params.Set("binding", id);
    params.Set("name", name);
    std::optional<base::Value> parsed = Parse(args);
    params.Set("args", parsed ? std::move(*parsed) : base::Value(base::ListValue()));
    if (serialized_args)
      params.Set("serializedArgs", std::move(*serialized_args));
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
        Agent(frame)->Expose(exposed_, ContextBindings());
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
    MatchWorkers();
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
    // Its handles answer that it is gone (crPage.ts's execution context).
    for (auto& [session, devtools] : devtools_workers_) {
      if (devtools.id != id)
        continue;
      for (auto& [handle, object] : cdp_handles_)
        if (object.session == session)
          object.session.clear();
      devtools.id = "-";
    }
    base::DictValue params;
    params.Set("worker", id);
    Emit("Worker.close", std::move(params));
  }

  // ---- Workers' own DevTools sessions (crPage.ts) -----------------------

  void WorkerAttached(const std::string& session, const base::DictValue& params) {
    DevToolsWorker& worker = devtools_workers_[session];
    const std::string* target = params.FindStringByDottedPath("targetInfo.targetId");
    const std::string* url = params.FindStringByDottedPath("targetInfo.url");
    worker.target = target ? *target : std::string();
    worker.url = url ? *url : std::string();
    // Before it runs (the view releases it after this).
    view_->SendToSession(session, "Runtime.enable", base::DictValue());
    MatchWorkers();
  }

  void WorkerDetached(const std::string& session) {
    if (!devtools_workers_.erase(session))
      return;
    for (auto& [handle, object] : cdp_handles_)
      if (object.session == session)
        object.session.clear();
  }

  // Each session with the worker the renderer told of with its URL, in the
  // order they came.
  void MatchWorkers() {
    std::set<std::string> matched;
    for (const auto& [session, worker] : devtools_workers_)
      if (!worker.id.empty())
        matched.insert(worker.id);
    std::vector<std::string> flush;
    for (auto& [session, worker] : devtools_workers_) {
      if (!worker.id.empty())
        continue;
      for (const auto& [id, ref] : workers_) {
        if (ref.url == worker.url && !matched.contains(id)) {
          worker.id = id;
          matched.insert(id);
          flush.push_back(session);
          break;
        }
      }
    }
    for (const std::string& session : flush) {
      auto found = devtools_workers_.find(session);
      if (found == devtools_workers_.end())
        continue;
      std::vector<base::DictValue> messages = std::move(found->second.console);
      for (base::DictValue& message : messages)
        WorkerConsole(found->second.id, std::move(message));
    }
  }

  DevToolsWorker* DevToolsWorkerOf(const std::string& id, std::string* session) {
    for (auto& [key, worker] : devtools_workers_) {
      if (worker.id == id) {
        *session = key;
        return &worker;
      }
    }
    return nullptr;
  }

  void WorkerSessionEvent(DevToolsWorker& worker, const std::string& session,
                          const std::string& method, const base::DictValue& params) {
    if (method == "Inspector.workerScriptLoaded" || method == "Runtime.executionContextCreated") {
      if (method == "Inspector.workerScriptLoaded")
        worker.script_loaded = true;
      else
        worker.context = params.FindIntByDottedPath("context.id").value_or(worker.context);
      if (worker.script_loaded && worker.context) {
        std::vector<base::OnceClosure> waiting = std::move(worker.waiting);
        for (base::OnceClosure& run : waiting)
          std::move(run).Run();
      }
      return;
    }
    if (method != "Runtime.consoleAPICalled")
      return;
    const std::string* type = params.FindString("type");
    base::DictValue message;
    message.Set("type", type ? *type : std::string("log"));
    std::vector<std::string> previews;
    base::ListValue args;
    if (const base::ListValue* given = params.FindList("args")) {
      for (const base::Value& arg : *given) {
        if (!arg.is_dict())
          continue;
        previews.push_back(RenderPreview(arg.GetDict()));
        args.Append(NewCdpHandle(session, arg.GetDict()));
      }
    }
    message.Set("text", base::JoinString(previews, " "));
    message.Set("args", std::move(args));
    base::DictValue location;
    const base::ListValue* frames = params.FindListByDottedPath("stackTrace.callFrames");
    const base::DictValue* top =
        frames && !frames->empty() && frames->front().is_dict() ? &frames->front().GetDict() : nullptr;
    const std::string* url = top ? top->FindString("url") : nullptr;
    location.Set("url", url ? *url : std::string());
    location.Set("lineNumber", top ? top->FindInt("lineNumber").value_or(0) : 0);
    location.Set("columnNumber", top ? top->FindInt("columnNumber").value_or(0) : 0);
    message.Set("location", std::move(location));
    if (worker.id.empty()) {
      worker.console.push_back(std::move(message));
      return;
    }
    WorkerConsole(worker.id, std::move(message));
  }

  // The worker's console message: its page's, of the worker, in order with
  // the page's own.
  void WorkerConsole(const std::string& id, base::DictValue message) {
    auto ref = workers_.find(id);
    content::RenderFrameHost* frame =
        ref == workers_.end() ? nullptr : content::RenderFrameHost::FromID(ref->second.frame);
    message.Set("frame", FrameId(frame ? frame : MainFrame()));
    message.Set("worker", id);
    base::ListValue args = message.Extract("args")
                               .and_then([](base::Value value) -> std::optional<base::ListValue> {
                                 if (!value.is_list())
                                   return std::nullopt;
                                 return std::move(value.GetList());
                               })
                               .value_or(base::ListValue());
    console_queue_[++next_console_] = std::move(message);
    ConsoleReady(next_console_, std::move(args));
  }

  // A remote object of a worker's session as the handle the server is given.
  base::DictValue NewCdpHandle(const std::string& session, const base::DictValue& remote) {
    const std::string id = "w" + base::NumberToString(++next_cdp_handle_);
    CdpHandle handle;
    handle.session = session;
    if (const std::string* object = remote.FindString("objectId")) {
      handle.object = *object;
    } else {
      const std::string* type = remote.FindString("type");
      const std::string* unserializable = remote.FindString("unserializableValue");
      const base::Value* value = remote.Find("value");
      base::DictValue special;
      if (type && *type == "undefined") {
        special.Set("v", "undefined");
        handle.value = base::Value(std::move(special));
      } else if (unserializable && unserializable->ends_with("n")) {
        special.Set("bi", unserializable->substr(0, unserializable->size() - 1));
        handle.value = base::Value(std::move(special));
      } else if (unserializable) {
        special.Set("v", *unserializable);
        handle.value = base::Value(std::move(special));
      } else if (!value || value->is_none()) {
        special.Set("v", "null");
        handle.value = base::Value(std::move(special));
      } else {
        handle.value = value->Clone();
      }
    }
    cdp_handles_[id] = std::move(handle);
    base::DictValue exposed;
    exposed.Set("handle", id);
    exposed.Set("preview", RenderPreview(remote));
    return exposed;
  }

  // kWorkerEvaluate in the worker of @session, on @object or in its context.
  void WorkerFunction(Call* call, const std::string& session, const std::string& object,
                      const std::string& expression, std::optional<bool> is_function,
                      bool by_value, base::Value arg, base::ListValue handles,
                      base::OnceCallback<void(Call*, const base::DictValue&)> then) {
    auto found = devtools_workers_.find(session);
    if (found == devtools_workers_.end()) {
      call->Fail(kClosed);
      return;
    }
    DevToolsWorker& worker = found->second;
    // Once its script has loaded (crPage.ts's workerScriptLoaded()).
    if (object.empty() && !(worker.script_loaded && worker.context)) {
      worker.waiting.push_back(base::BindOnce(
          [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call, std::string session,
             std::string expression, std::optional<bool> is_function, bool by_value,
             base::Value arg, base::ListValue handles,
             base::OnceCallback<void(Call*, const base::DictValue&)> then) {
            if (self && call)
              self->WorkerFunction(call.get(), session, std::string(), expression, is_function,
                                   by_value, std::move(arg), std::move(handles), std::move(then));
          },
          weak_factory_.GetWeakPtr(), call->GetWeakPtr(), session, expression, is_function,
          by_value, std::move(arg), std::move(handles), std::move(then)));
      return;
    }
    std::string declaration = kWorkerEvaluate;
    const size_t at = declaration.find("__EXPRESSION__");
    declaration.replace(at, strlen("__EXPRESSION__"), "\n" + expression + "\n");
    base::ListValue arguments;
    auto plain = [&](base::Value value) {
      base::DictValue argument;
      argument.Set("value", std::move(value));
      arguments.Append(std::move(argument));
    };
    plain(is_function ? base::Value(*is_function) : base::Value());
    plain(base::Value(!object.empty()));
    plain(base::Value(by_value));
    plain(std::move(arg));
    for (base::Value& handle : handles)
      arguments.Append(std::move(handle));
    base::DictValue command;
    command.Set("functionDeclaration", declaration);
    command.Set("arguments", std::move(arguments));
    if (!object.empty())
      command.Set("objectId", object);
    else
      command.Set("executionContextId", worker.context);
    command.Set("returnByValue", by_value);
    command.Set("awaitPromise", true);
    view_->SendToSession(
        session, "Runtime.callFunctionOn", std::move(command),
        base::BindOnce(
            [](base::WeakPtr<Call> call, base::OnceCallback<void(Call*, const base::DictValue&)> then,
               const base::DictValue* result, const std::string& error) {
              if (!call)
                return;
              if (!result) {
                call->Fail(kClosed);
                return;
              }
              if (const base::DictValue* details = result->FindDict("exceptionDetails")) {
                const std::string* description = details->FindStringByDottedPath("exception.description");
                const std::string* text = details->FindString("text");
                call->Fail(description ? *description : text ? *text : "Error");
                return;
              }
              const base::DictValue* remote = result->FindDict("result");
              const base::DictValue none;
              std::move(then).Run(call.get(), remote ? *remote : none);
            },
            call->GetWeakPtr(), std::move(then)));
  }

  // The arguments' handles of a call on a worker's: CDP's {objectId}, or
  // the value of a primitive's.
  bool WorkerArgHandles(Call* call, const std::string& session, base::ListValue* handles) {
    if (const base::ListValue* given = call->params().FindList("argHandles")) {
      for (const base::Value& id : *given) {
        auto handle = id.is_string() ? cdp_handles_.find(id.GetString()) : cdp_handles_.end();
        if (handle == cdp_handles_.end() || handle->second.session != session) {
          call->Fail(handle == cdp_handles_.end() || !handle->second.session.empty()
                         ? "JSHandles can be evaluated only in the context they were created!"
                         : kClosed);
          return false;
        }
        base::DictValue argument;
        if (handle->second.object.empty())
          argument.Set("value", base::Value());
        else
          argument.Set("objectId", handle->second.object);
        handles->Append(std::move(argument));
      }
    }
    return true;
  }

  // Worker.evaluateExpressionHandle: the worker's object as a handle.
  void WorkerEvaluateHandle(Call* call) {
    std::string session;
    if (!DevToolsWorkerOf(call->String("worker"), &session)) {
      call->Fail(kClosed);
      return;
    }
    base::ListValue handles;
    if (!WorkerArgHandles(call, session, &handles))
      return;
    const base::Value* arg = call->params().Find("arg");
    WorkerFunction(call, session, std::string(), call->String("expression"),
                   call->params().FindBool("isFunction"), false, arg ? arg->Clone() : base::Value(),
                   std::move(handles),
                   base::BindOnce(
                       [](base::WeakPtr<Automation> self, std::string session, Call* call,
                          const base::DictValue& remote) {
                         if (!self)
                           return;
                         base::DictValue answer;
                         answer.Set("handle", self->NewCdpHandle(session, remote));
                         call->Done(base::Value(std::move(answer)));
                       },
                       weak_factory_.GetWeakPtr(), session));
  }

  // A JSHandle of a worker's (getProperty, getPropertyList, jsonValue,
  // evaluateExpression(Handle) on it, dispose): false if it is not one.
  bool CdpHandleCall(Call* call) {
    auto found = cdp_handles_.find(call->String("handle"));
    if (found == cdp_handles_.end())
      return false;
    const std::string name = call->Name();
    if (name == "dispose") {
      if (!found->second.session.empty() && !found->second.object.empty()) {
        base::DictValue release;
        release.Set("objectId", found->second.object);
        view_->SendToSession(found->second.session, "Runtime.releaseObject", std::move(release));
      }
      cdp_handles_.erase(found);
      call->Done();
      return true;
    }
    const CdpHandle& handle = found->second;
    if (handle.session.empty()) {
      call->Fail(kClosed);
      return true;
    }
    const std::string session = handle.session;
    if (handle.object.empty()) {
      // A primitive's: its value, and none of its properties.
      if (name == "jsonValue") {
        call->Answer(handle.value.Clone());
      } else if (name == "getPropertyList") {
        base::DictValue answer;
        answer.Set("properties", base::ListValue());
        call->Done(base::Value(std::move(answer)));
      } else if (name == "getProperty") {
        base::DictValue undefined;
        undefined.Set("type", "undefined");
        base::DictValue answer;
        answer.Set("handle", NewCdpHandle(session, undefined));
        call->Done(base::Value(std::move(answer)));
      } else {
        call->Fail("zebkit: a worker's primitive handle; evaluate with its value");
      }
      return true;
    }
    const std::string object = handle.object;
    auto as_handle = base::BindOnce(
        [](base::WeakPtr<Automation> self, std::string session, Call* call,
           const base::DictValue& remote) {
          if (!self)
            return;
          base::DictValue answer;
          answer.Set("handle", self->NewCdpHandle(session, remote));
          call->Done(base::Value(std::move(answer)));
        },
        weak_factory_.GetWeakPtr(), session);
    auto as_value = base::BindOnce([](Call* call, const base::DictValue& remote) {
      const base::Value* value = remote.Find("value");
      call->Answer(value ? value->Clone() : base::Value());
    });
    if (name == "getProperty") {
      WorkerFunction(call, session, object, "(o, name) => o[name]", true, false,
                     base::Value(call->String("name")), base::ListValue(), std::move(as_handle));
    } else if (name == "jsonValue") {
      WorkerFunction(call, session, object, "o => o", true, true, base::Value(), base::ListValue(),
                     std::move(as_value));
    } else if (name == "evaluateExpression" || name == "evaluateExpressionHandle") {
      base::ListValue handles;
      if (!WorkerArgHandles(call, session, &handles))
        return true;
      const base::Value* arg = call->params().Find("arg");
      const bool by_value = name == "evaluateExpression";
      WorkerFunction(call, session, object, call->String("expression"),
                     call->params().FindBool("isFunction"), by_value,
                     arg ? arg->Clone() : base::Value(), std::move(handles),
                     by_value ? std::move(as_value) : std::move(as_handle));
    } else if (name == "getPropertyList") {
      base::DictValue params;
      params.Set("objectId", object);
      params.Set("ownProperties", true);
      view_->SendToSession(
          session, "Runtime.getProperties", std::move(params),
          base::BindOnce(
              [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call, std::string session,
                 const base::DictValue* result, const std::string& error) {
                if (!self || !call)
                  return;
                if (!result) {
                  call->Fail(kClosed);
                  return;
                }
                base::ListValue properties;
                if (const base::ListValue* list = result->FindList("result")) {
                  for (const base::Value& item : *list) {
                    const base::DictValue* property = item.is_dict() ? &item.GetDict() : nullptr;
                    const base::DictValue* value = property ? property->FindDict("value") : nullptr;
                    const std::string* key = property ? property->FindString("name") : nullptr;
                    if (!value || !key || !property->FindBool("enumerable").value_or(false))
                      continue;
                    base::DictValue entry;
                    entry.Set("name", *key);
                    entry.Set("value", self->NewCdpHandle(session, *value));
                    properties.Append(std::move(entry));
                  }
                }
                base::DictValue answer;
                answer.Set("properties", std::move(properties));
                call->Done(base::Value(std::move(answer)));
              },
              weak_factory_.GetWeakPtr(), call->GetWeakPtr(), session));
    } else {
      call->Fail("zebkit: not on a worker's handle: " + name);
    }
    return true;
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
    if (call->Flag("serialized")) {
      // Playwright's own answer: a serialized value ("arg") whose {h: i} are
      // "argHandles"[i].
      std::vector<int32_t> handles;
      if (const base::ListValue* given = call->params().FindList("argHandles")) {
        for (const base::Value& handle_id : *given) {
          const HandleRef* handle = handle_id.is_string() ? FindHandle(handle_id.GetString()) : nullptr;
          handles.push_back(handle ? handle->id : 0);
        }
      }
      const base::Value* arg = call->params().Find("arg");
      base::DictValue undefined;
      undefined.Set("v", "undefined");
      std::move(callback).Run(ok, arg ? Json(*arg) : Json(base::Value(std::move(undefined))), true,
                              handles);
    } else if (ok) {
      const base::Value* result = call->params().Find("result");
      std::move(callback).Run(true, result ? Json(*result) : "null", false, {});
    } else {
      std::move(callback).Run(false, call->String("error"), false, {});
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
    const std::string given = call->String("harId");
    const std::string id = given.empty() ? "har@" + base::NumberToString(++next_har_) : given;
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
                std::string(version_info::GetVersionNumber()), zebkit_version(),
                call->String("mode") == "archive",
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
      // clock.ts's parseTime(): what new Date() cannot hold is refused.
      if (!ticks && call->Has(number) && !(std::abs(call->Number(number)) <= 8.64e15)) {
        const double given = call->Number(number);
        call->Fail("Invalid date: " + JsNumber(given));
        return;
      }
      if (!ticks && call->Has(text) && !call->Flag("parsedTime")) {
        // Parsed as the page's Date parses it, then given as a number.
        Agent(MainFrame())->Evaluate(
            AutomationWorld::kMain, TimeOf(call->String(text)), AutomationExpression::kValue,
            "[]", {}, true, true,
            mojo::WrapCallbackWithDefaultInvokeIfNotRun(
                base::BindOnce(
                    [](base::WeakPtr<Automation> self, base::WeakPtr<Call> call,
                       std::string method, AutomationStatus status, const std::string& result,
                       std::vector<AutomationHandlePtr>) {
                      if (!self || !call)
                        return;
                      std::optional<base::Value> answer = Parse(result);
                      const base::Value* value =
                          answer && answer->is_dict() ? answer->GetDict().Find("value") : nullptr;
                      if (status == AutomationStatus::kOk && value &&
                          !(value->is_int() || value->is_double())) {
                        call->Fail("Invalid date: " + call->String("timeString"));
                        return;
                      }
                      call->params().Set("parsedTime", true);
                      if (value && (value->is_int() || value->is_double())) {
                        call->params().Remove("timeString");
                        call->params().Set("timeNumber", value->GetDouble());
                      }
                      self->Clock(call.get(), method, false);
                    },
                    weak_factory_.GetWeakPtr(), call->GetWeakPtr(), method),
                AutomationStatus::kGone, std::string(), std::vector<AutomationHandlePtr>()));
        return;
      }
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
    // An integer, as server/clock.ts's Date.now().
    const std::string now = base::NumberToString(
        base::Time::Now().InMillisecondsSinceUnixEpoch());
    const std::string log = "globalThis.__pwClock.controller.log('" + method +
                            "', " + now + (value.empty() ? "" : ", " + value) +
                            ")";
    // For documents to come, as server/clock.ts's addInitScript: scripts here
    // also run at once, and this document gets the command itself below; a new
    // document has no root element yet.
    const std::string replay = "if (!document.documentElement) " + log + ";";
    zebkit_view_add_script(call->view(), replay.c_str());
    scripts += "\nglobalThis.__pwClock.controller." + method + "(" + value + ");";
    EvaluateInAllFrames(call, scripts);
  }

  // A number as JavaScript's String() writes it: an integer below 1e21 in
  // full, whatever exponent the shortest form has.
  static std::string JsNumber(double value) {
    std::string text = base::NumberToString(value);
    const size_t e = text.find('e');
    if (e == std::string::npos || !std::isfinite(value))
      return text;
    int exponent = 0;
    if (!base::StringToInt(text.substr(e + 1), &exponent) || exponent < 0 || exponent >= 21)
      return text;
    std::string mantissa = text.substr(0, e);
    const bool negative = !mantissa.empty() && mantissa[0] == '-';
    if (negative)
      mantissa.erase(0, 1);
    const size_t dot = mantissa.find('.');
    std::string digits = mantissa;
    int point = static_cast<int>(mantissa.size());
    if (dot != std::string::npos) {
      digits.erase(dot, 1);
      point = static_cast<int>(dot);
    }
    point += exponent;
    if (point >= static_cast<int>(digits.size()))
      digits.append(point - digits.size(), '0');
    else
      digits.insert(point, ".");
    return (negative ? "-" : "") + digits;
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

  // Page.evaluateInFrames {expression}: its value in the main world of each
  // frame, as frame.nonStallingRawEvaluateInExistingMainContext() gives it
  // (the endpoint's tracing coverage): {values, errors}.
  void EvaluateInFrames(Call* call) {
    struct State {
      base::WeakPtr<Call> call;
      size_t left = 0;
      base::ListValue values;
      base::ListValue errors;
    };
    auto state = std::make_shared<State>();
    state->call = call->GetWeakPtr();
    std::vector<content::RenderFrameHost*> frames;
    MainFrame()->ForEachRenderFrameHost([&](content::RenderFrameHost* frame) {
      if (frame->IsRenderFrameLive())
        frames.push_back(frame);
    });
    state->left = frames.size();
    auto finish = [](std::shared_ptr<State> state) {
      if (!state->call)
        return;
      base::DictValue answer;
      answer.Set("values", std::move(state->values));
      answer.Set("errors", std::move(state->errors));
      state->call->Done(base::Value(std::move(answer)));
    };
    if (frames.empty()) {
      finish(state);
      return;
    }
    for (content::RenderFrameHost* frame : frames) {
      Agent(frame)->Evaluate(
          AutomationWorld::kMain, call->String("expression"), AutomationExpression::kValue, "[]", {}, true,
          true,
          mojo::WrapCallbackWithDefaultInvokeIfNotRun(
              base::BindOnce(
                  [](std::shared_ptr<State> state, decltype(finish) finish, AutomationStatus status,
                     const std::string& result, std::vector<AutomationHandlePtr>) {
                    std::optional<base::Value> answer = Parse(result);
                    if (status == AutomationStatus::kOk && answer && answer->is_dict()) {
                      if (answer->GetDict().FindBool("ok").value_or(false)) {
                        base::Value* value = answer->GetDict().Find("value");
                        state->values.Append(value ? std::move(*value) : base::Value());
                      } else if (const std::string* error = answer->GetDict().FindString("error")) {
                        state->errors.Append(*error);
                      }
                    }
                    if (--state->left == 0)
                      finish(state);
                  },
                  state, finish),
              AutomationStatus::kGone, std::string(), std::vector<AutomationHandlePtr>()));
    }
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
    StorageWithCookies(web_contents()->GetBrowserContext(), call, std::move(cookies));
  }

  // The answer of storageState: the cookies, and every origin's local storage
  // the profile has (ContextLocalStorage()), pages or none.
  static void StorageWithCookies(content::BrowserContext* profile,
                                 base::WeakPtr<Call> call,
                                 base::Value cookies) {
    ContextLocalStorage(
        profile, base::BindOnce(
                     [](content::BrowserContext* profile, base::WeakPtr<Call> call, base::Value cookies,
                        base::Value storage, const std::string& error) {
                       if (!call)
                         return;
                       base::DictValue answer;
                       if (cookies.is_dict()) {
                         if (base::Value* list = cookies.GetDict().Find("cookies"))
                           answer.Set("cookies", std::move(*list));
                       }
                       base::Value* origins = storage.is_dict() ? storage.GetDict().Find("origins") : nullptr;
                       answer.Set("origins", origins ? std::move(*origins) : base::Value(base::ListValue()));
                       if (call->Flag("credentials"))
                         answer.Set("credentials", PasskeysOf(ContextStates()[profile]));
                       if (call->Flag("indexedDB") || call->Flag("opfs")) {
                         AddPageStorage(profile, call, std::move(answer));
                         return;
                       }
                       call->Done(base::Value(std::move(answer)));
                     },
                     profile, call, std::move(cookies)));
  }

  // storageState({indexedDB, opfs}): each origin's databases and files, as
  // browserContext.ts collects them with Playwright's storage script in a page
  // of the origin; here the profile's pages that show one.
  static void AddPageStorage(content::BrowserContext* profile, base::WeakPtr<Call> call,
                             base::DictValue answer) {
    std::vector<std::pair<Automation*, content::RenderFrameHost*>> frames;
    std::set<std::string> seen;
    if (profile) {
      for (Automation* page : InProfile(profile)) {
        content::RenderFrameHost* frame = page->MainFrame();
        const url::Origin origin = frame->GetLastCommittedOrigin();
        if (origin.opaque() || !frame->IsRenderFrameLive() || !seen.insert(origin.Serialize()).second)
          continue;
        frames.emplace_back(page, frame);
      }
    }
    struct State {
      base::WeakPtr<Call> call;
      base::DictValue answer;
      size_t left = 0;
      raw_ptr<content::BrowserContext> profile;
      std::set<std::string> shown;
      std::string collect;
    };
    auto state = std::make_shared<State>();
    state->call = call;
    state->answer = std::move(answer);
    state->left = frames.size();
    state->profile = profile;
    state->shown = seen;
    state->collect = std::string("collect({ indexedDB: ") + (call->Flag("indexedDB") ? "true" : "false") +
                     ", opfs: " + (call->Flag("opfs") ? "true" : "false") + " })";
    auto finish = [](std::shared_ptr<State> state) {
      if (!state->call)
        return;
      // The origins given to the context that no page shows: visited.
      std::vector<base::DictValue> others;
      content::BrowserContext* profile = state->profile;
      if (profile) {
        std::set<std::string> have;
        for (const base::Value& item : *state->answer.FindList("origins")) {
          if (const std::string* name = item.GetDict().FindString("origin"))
            have.insert(*name);
        }
        for (const std::string& origin : ContextStates()[profile].storage_origins) {
          if (have.contains(origin) || state->shown.contains(origin))
            continue;
          base::DictValue entry;
          entry.Set("origin", origin);
          others.push_back(std::move(entry));
        }
      }
      if (others.empty()) {
        state->call->Done(base::Value(std::move(state->answer)));
        return;
      }
      const std::string expression = StorageScriptCall(state->collect);
      VisitOrigins(profile, std::move(others),
                   base::BindRepeating([](std::string expression, const base::DictValue&) { return expression; },
                                       expression),
                   base::BindOnce(
                       [](std::shared_ptr<State> state, OriginValues values) {
                         if (!state->call)
                           return;
                         base::ListValue* origins = state->answer.FindList("origins");
                         for (auto& [origin, value] : values) {
                           if (!value.is_dict())
                             continue;
                           const base::ListValue* local = value.GetDict().FindList("localStorage");
                           const base::ListValue* databases = value.GetDict().FindList("indexedDB");
                           const base::ListValue* files = value.GetDict().FindList("opfs");
                           if ((!local || local->empty()) && (!databases || databases->empty()) &&
                               (!files || files->empty())) {
                             continue;
                           }
                           base::DictValue entry;
                           entry.Set("origin", origin);
                           entry.Set("localStorage", local ? local->Clone() : base::ListValue());
                           if (databases)
                             entry.Set("indexedDB", databases->Clone());
                           if (files && !files->empty())
                             entry.Set("opfs", files->Clone());
                           origins->Append(std::move(entry));
                         }
                         state->call->Done(base::Value(std::move(state->answer)));
                       },
                       state));
    };
    if (frames.empty()) {
      finish(state);
      return;
    }
    const std::string expression =
        "(() => { const script = new (" +
        std::string(reinterpret_cast<const char*>(kStorageScript), sizeof(kStorageScript)) +
        ")('chromium'); return script.collect({ indexedDB: " + (call->Flag("indexedDB") ? "true" : "false") +
        ", opfs: " + (call->Flag("opfs") ? "true" : "false") + " }); })()";
    for (auto [page, frame] : frames) {
      const std::string origin = frame->GetLastCommittedOrigin().Serialize();
      page->Agent(frame)->Evaluate(
          AutomationWorld::kUtility, expression, AutomationExpression::kValue, "[]", {}, true, true,
          mojo::WrapCallbackWithDefaultInvokeIfNotRun(
              base::BindOnce(
                  [](std::shared_ptr<State> state, std::string origin, decltype(finish) finish,
                     AutomationStatus status, const std::string& result, std::vector<AutomationHandlePtr>) {
                    std::optional<base::Value> parsed = Parse(result);
                    base::Value* value = parsed && parsed->is_dict() ? parsed->GetDict().Find("value") : nullptr;
                    if (status == AutomationStatus::kOk && value && value->is_dict()) {
                      base::ListValue* origins = state->answer.FindList("origins");
                      base::DictValue* entry = nullptr;
                      for (base::Value& item : *origins) {
                        const std::string* name = item.GetDict().FindString("origin");
                        if (name && *name == origin)
                          entry = &item.GetDict();
                      }
                      const base::ListValue* databases = value->GetDict().FindList("indexedDB");
                      const base::ListValue* files = value->GetDict().FindList("opfs");
                      const bool has = (databases && !databases->empty()) || (files && !files->empty());
                      if (!entry && has) {
                        base::DictValue added;
                        added.Set("origin", origin);
                        added.Set("localStorage", base::ListValue());
                        origins->Append(std::move(added));
                        entry = &origins->back().GetDict();
                      }
                      if (entry && databases)
                        entry->Set("indexedDB", databases->Clone());
                      if (entry && files && !files->empty())
                        entry->Set("opfs", files->Clone());
                    }
                    if (--state->left == 0)
                      finish(state);
                  },
                  state, origin, finish),
              AutomationStatus::kGone, std::string(), std::vector<AutomationHandlePtr>()));
    }
  }

  // BrowserContext.setStorageState: its cookies set; the local storage of
  // its origins given to the frames that show them now.
  void SetStorageState(Call* call) {
    // IndexedDB and OPFS first, through pages of their origins.
    if (!call->Flag("restored")) {
      const base::DictValue* given = call->params().FindDict("storageState");
      const base::ListValue* origins = given ? given->FindList("origins") : call->params().FindList("origins");
      base::DictValue restore;
      restore.Set("context", call->String("context").empty()
                                 ? ContextIdOfProfile(web_contents()->GetBrowserContext())
                                 : call->String("context"));
      restore.Set("origins", origins ? origins->Clone() : base::ListValue());
      call->params().Set("restored", true);
      auto* inner = new Call(nullptr, "BrowserContext.restoreStorage", std::move(restore),
                             [](zebkit_view*, const char*, const char*, void* data) {
                               auto* pending = static_cast<std::pair<base::WeakPtr<Automation>, base::WeakPtr<Call>>*>(data);
                               if (pending->first && pending->second)
                                 pending->first->SetStorageState(pending->second.get());
                               delete pending;
                             },
                             new std::pair<base::WeakPtr<Automation>, base::WeakPtr<Call>>(
                                 weak_factory_.GetWeakPtr(), call->GetWeakPtr()));
      RestoreStorage(inner);
      return;
    }
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
        "; try { localStorage.clear(); } catch {} for (const o of origins) if (o.origin === "
        "location.origin) for (const i of o.localStorage || []) localStorage.setItem(i.name, "
        "i.value); })()";
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
    // And into the profile's storage, for origins no page shows now.
    ContextSetLocalStorage(web_contents()->GetBrowserContext(),
                           origins ? origins->Clone() : base::ListValue(), base::DoNothing(), true);
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
    if (frame_sessions_.empty()) {
      InContext(call, &ContextEmulateMedia);
      return;
    }
    // Other sites' frames have the page's preferences by their own process:
    // answered once each frame's renderer has had them (crPage.ts waits for
    // each FrameSession's Emulation.setEmulatedMedia).
    ContextEmulateMedia(web_contents(), call->params(), base::DoNothing());
    auto done = base::BarrierClosure(
        frame_sessions_.size(),
        base::BindOnce([](base::WeakPtr<Call> call) { if (call) call->Done(); }, call->GetWeakPtr()));
    for (const std::string& session : frame_sessions_) {
      base::DictValue params;
      params.Set("expression", "0");
      view_->SendToSession(session, "Runtime.evaluate", std::move(params),
                           base::BindOnce([](base::RepeatingClosure done, const base::DictValue*,
                                             const std::string&) { done.Run(); },
                                          done));
    }
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
  // Where the mouse is in the page, for traces' input events.
  gfx::PointF mouse_in_root_;
  // A screenshot masked elements of frames other than the main one.
  bool masked_in_frames_ = false;
  // The screenshot being taken; others wait for it.
  base::WeakPtr<Call> screenshot_current_;
  std::map<std::string, std::string> binding_registrations_;
  bool clock_installed_ = false;
  std::unique_ptr<TraceRecorder> tracing_;
  std::map<Call*, std::string> traced_;
  std::map<std::string, std::unique_ptr<HarRecorder>> hars_;
  int next_har_ = 0;
  std::unique_ptr<VideoRecorder> video_;
  bool web_socket_mock_ = false;
  bool emulated_offline_ = false;
  // The sessions of other sites' frames (crPage.ts's FrameSessions).
  std::set<std::string> frame_sessions_;
  // Runtime.exceptionThrown comes from the page's DevTools session.
  bool runtime_exceptions_ = false;
  // The viewport's emulation waiting for the page's view.
  std::optional<std::array<int, 4>> pending_metrics_;
  bool pending_touch_ = false;
  // What Page.startJSCoverage and startCSSCoverage collect, while on.
  std::optional<JsCoverage> js_coverage_;
  std::optional<CssCoverage> css_coverage_;
  // Requests Blink's memory cache answered (Network.requestServedFromCache).
  std::set<std::string> memory_cached_;
  std::set<GURL> unanswered_auth_;
  std::map<content::FrameTreeNodeId, int> frame_seqs_;
  int next_frame_seq_ = 1;
  // page.ts's locator handlers, by uid.
  struct LocatorHandler {
    std::string selector;
    // asLocator()'s text of the selector, for the call logs.
    std::string text;
    bool no_wait_after = false;
    // Triggered, until the client resolves it: the checkpoints that wait.
    bool triggered = false;
    std::vector<base::OnceClosure> resolved;
  };
  std::map<int, LocatorHandler> locator_handlers_;
  int last_locator_handler_ = 0;
  // The calls waiting for a handler (page.ts's _locatorHandlerRunningCounter):
  // while one lives, the handler's own actions skip the checkpoint.
  std::vector<base::WeakPtr<Call>> in_locator_handler_;
  // The page's worlds as DevTools numbers them: their frames' tokens.
  std::map<int, std::string> devtools_contexts_;
  // Console messages waiting for their arguments' handles, in order.
  std::map<uint64_t, base::DictValue> console_queue_;
  uint64_t next_console_ = 0;
  std::vector<base::RepeatingClosure> console_answers_;
  // DevTools' requests of the page by their ids, for those a service worker
  // answers (Network.requestWillBeSent), and those reported so.
  std::map<std::string, base::DictValue> devtools_requests_;
  std::set<std::string> worker_served_;
  // Page.setViewportSize's, which outranks the context's.
  std::optional<gfx::Size> viewport_;
  struct WorkerRef {
    content::GlobalRenderFrameHostId frame;
    int32_t number;
    std::string url;
  };
  std::map<std::string, WorkerRef> workers_;
  std::map<std::string, DevToolsWorker> devtools_workers_;
  std::map<std::string, CdpHandle> cdp_handles_;
  uint64_t next_cdp_handle_ = 0;
  std::map<std::string, content::GlobalRenderFrameHostId> web_sockets_;
  std::string media_type_;
  int active_calls_ = 0;
  base::OneShotTimer slow_frames_timer_;
  mojo::AssociatedReceiverSet<mojom::AutomationHost,
                              content::GlobalRenderFrameHostId>
      hosts_;
  std::map<std::string, BindingCalledCallback> bindings_;
  int next_binding_ = 0;

  zebkit_automation_event_func on_event_ = nullptr;
  raw_ptr<void> on_event_data_ = nullptr;
  bool hold_dialogs_ = false;
  bool report_closed_dialogs_ = false;
  bool intercept_file_choosers_ = false;
  std::map<std::string, FileChooser> file_choosers_;
  std::map<std::string, int> devtools_choosers_;
  int next_chooser_ = 0;

  // The frames' lifecycles since their last navigation.
  std::map<content::FrameTreeNodeId, Lifecycle> lifecycles_;
  std::map<content::FrameTreeNodeId, base::OneShotTimer> idle_timers_;
  // networkidle's: each frame's requests in flight, each request's frame, the
  // documents' own requests, and the frames idle themselves.
  std::map<content::FrameTreeNodeId, std::set<std::string>> inflight_;
  std::map<std::string, content::FrameTreeNodeId> request_frames_;
  std::set<std::string> navigation_requests_;
  std::set<content::FrameTreeNodeId> idle_self_;
  std::deque<NavigationWaiter> navigation_waiters_;
  // Page.setExtraHTTPHeaders's, merged with the context's.
  base::ListValue page_extra_headers_;
  // Page.waitForInitialNavigation's calls.
  std::vector<base::WeakPtr<Call>> initial_navigation_waiters_;
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

// ---- Service workers ---------------------------------------------------

// A profile's running service workers, for the requests of its pages that
// one may answer (the workers themselves are ServiceWorkerTargets').
class ServiceWorkerWatch : public content::ServiceWorkerContextObserver {
 public:
  static std::map<content::BrowserContext*, std::unique_ptr<ServiceWorkerWatch>>& All() {
    static base::NoDestructor<std::map<content::BrowserContext*, std::unique_ptr<ServiceWorkerWatch>>> all;
    return *all;
  }

  ServiceWorkerWatch(content::BrowserContext* profile, content::ServiceWorkerContext* context)
      : profile_(profile), context_(context) {
    context_->AddObserver(this);
  }
  ~ServiceWorkerWatch() override {
    if (context_)
      context_->RemoveObserver(this);
  }

  bool Running() const { return !running_.empty(); }

  // content::ServiceWorkerContextObserver
  void OnVersionStartedRunning(int64_t version_id,
                               const content::ServiceWorkerRunningInfo& info) override {
    const std::string id = "sw" + base::NumberToString(version_id);
    running_[id] = info.script_url;
  }
  void OnVersionStoppedRunning(int64_t version_id) override {
    const std::string id = "sw" + base::NumberToString(version_id);
    running_.erase(id);
  }
  void OnDestruct(content::ServiceWorkerContext* context) override {
    context_->RemoveObserver(this);
    context_ = nullptr;
    content::BrowserContext* profile = profile_;
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce([](content::BrowserContext* profile) { All().erase(profile); }, profile));
  }

 private:
  raw_ptr<content::BrowserContext> profile_;
  raw_ptr<content::ServiceWorkerContext> context_;
  std::map<std::string, GURL> running_;
};

bool ServiceWorkerWatch_Running(content::BrowserContext* profile) {
  auto found = ServiceWorkerWatch::All().find(profile);
  return found != ServiceWorkerWatch::All().end() && found->second->Running();
}

void WatchServiceWorkers(content::BrowserContext* profile) {
  if (!profile || ServiceWorkerWatch::All().contains(profile))
    return;
  content::ServiceWorkerContext* context =
      profile->GetDefaultStoragePartition()->GetServiceWorkerContext();
  if (!context)
    return;
  ServiceWorkerWatch::All()[profile] = std::make_unique<ServiceWorkerWatch>(profile, context);
}

// The context's service workers as crServiceWorker.ts has them: each one's
// own DevTools target, auto-attached from a browser session and held until
// its domains are on (Target.setAutoAttach's waitForDebuggerOnStart), so
// that the worker is the context's before its script is fetched. Its
// requests (crNetworkManager.ts on the worker's session: Network for the
// events, Fetch for the context's routes), its console (Runtime), and
// evaluation in it. Told through one of the profile's automated pages, as
// the context's events (ServiceWorker.*, by the worker's id "sw<n>").
class ServiceWorkerTargets : public content::DevToolsAgentHostClient,
                             public content::ServiceWorkerDevToolsManager::Observer {
 public:
  using Reply = base::OnceCallback<void(const base::DictValue* result, const std::string& error)>;

  static ServiceWorkerTargets& Get() {
    static base::NoDestructor<ServiceWorkerTargets> targets;
    return *targets;
  }

  // From a page's first automation of @profile on. Each worker's target as
  // a browser session's auto-attach has it (BrowserAutoAttacher), without
  // the browser session: that one forces a DevTools host for every frame,
  // and the pages' own sessions then stop hearing of their WebSockets.
  void Watch(content::BrowserContext* profile) {
    profiles_.insert(profile->UniqueId());
    if (!observing_) {
      observing_ = true;
      content::ServiceWorkerDevToolsManager::GetInstance()->AddObserver(this);
    }
    // Those running already (ReattachServiceWorkers()).
    content::ServiceWorkerDevToolsAgentHost::List hosts;
    content::ServiceWorkerDevToolsManager::GetInstance()->AddAllAgentHosts(&hosts);
    for (const auto& host : hosts) {
      if (host->GetBrowserContext() == profile && !workers_.contains(host->GetId()) &&
          host->AttachClient(this)) {
        Attached(host.get(), profile);
      }
    }
  }

  // content::ServiceWorkerDevToolsManager::Observer: held at its start until
  // its domains are on (Runtime.runIfWaitingForDebugger).
  void WorkerCreated(content::ServiceWorkerDevToolsAgentHost* host, bool* should_pause_on_start) override {
    content::BrowserContext* profile = host->GetBrowserContext();
    if (!profile || !AutomatedProfile(profile->UniqueId()) || workers_.contains(host->GetId()))
      return;
    if (!host->AttachClient(this))
      return;
    *should_pause_on_start = true;
    Attached(host, profile);
  }
  void WorkerDestroyed(content::ServiceWorkerDevToolsAgentHost* host) override {
    Detached(host->GetId());
  }

  // The context's routes, offline state or extra headers changed.
  void ContextChanged(content::BrowserContext* profile) {
    for (auto& [session, worker] : workers_) {
      if (worker.profile == profile)
        Apply(worker, false);
    }
  }

  // Whether @id is a running worker's; its profile.
  content::BrowserContext* ProfileOf(const std::string& id) {
    Worker* worker = ById(id);
    return worker ? worker->profile.get() : nullptr;
  }

  // Runtime.evaluate in the worker, by value.
  void Evaluate(const std::string& id, const std::string& expression, Reply reply) {
    Worker* worker = ById(id);
    if (!worker) {
      std::move(reply).Run(nullptr, kClosed);
      return;
    }
    base::DictValue params;
    params.Set("expression", expression);
    params.Set("returnByValue", true);
    params.Set("awaitPromise", true);
    // Once its script has loaded (crServiceWorker.ts's workerScriptLoaded()):
    // an evaluation while the script streams in brings the worker down.
    if (!worker->script_loaded) {
      worker->evaluations.emplace_back(std::move(params), std::move(reply));
      return;
    }
    Send(worker->session, "Runtime.evaluate", std::move(params), std::move(reply));
  }

  // A route of a worker's request: Fetch's answer to it. False when it is
  // not held (handled already, or its worker gone).
  bool Route(const std::string& route, const std::string& how, const base::DictValue& given) {
    auto found = routes_.find(route);
    if (found == routes_.end())
      return false;
    const auto [session, paused] = found->second;
    routes_.erase(found);
    if (!workers_.contains(session))
      return true;
    base::DictValue params;
    params.Set("requestId", paused);
    if (how == "continue") {
      if (const std::string* url = given.FindString("url"))
        params.Set("url", *url);
      if (const std::string* method = given.FindString("method"))
        params.Set("method", *method);
      if (const std::string* post = given.FindString("postData"))
        params.Set("postData", *post);
      if (const base::ListValue* headers = given.FindList("headers"))
        params.Set("headers", headers->Clone());
      Send(session, "Fetch.continueRequest", std::move(params), Reply());
    } else if (how == "fulfill") {
      params.Set("responseCode", given.FindInt("status").value_or(200));
      base::ListValue headers;
      if (const base::ListValue* list = given.FindList("headers"))
        headers = list->Clone();
      params.Set("responseHeaders", std::move(headers));
      std::string body;
      if (const std::string* text = given.FindString("body"))
        body = given.FindBool("isBase64").value_or(false) ? *text : base::Base64Encode(*text);
      params.Set("body", body);
      Send(session, "Fetch.fulfillRequest", std::move(params), Reply());
    } else {
      params.Set("errorReason", ErrorReason(given.FindString("errorCode") ? *given.FindString("errorCode") : ""));
      Send(session, "Fetch.failRequest", std::move(params), Reply());
    }
    return true;
  }

  // Response.body of a worker's request: Network.getResponseBody, as base64.
  void Body(const std::string& request, Reply reply) {
    auto found = bodies_.find(request);
    if (found == bodies_.end() || !workers_.contains(found->second.first)) {
      std::move(reply).Run(nullptr, "Response body is unavailable for redirect responses");
      return;
    }
    base::DictValue params;
    params.Set("requestId", found->second.second);
    Send(found->second.first, "Network.getResponseBody", std::move(params), std::move(reply));
  }

  // content::DevToolsAgentHostClient
  void DispatchProtocolMessage(content::DevToolsAgentHost* host, base::span<const uint8_t> bytes) override {
    std::optional<base::Value> message =
        base::JSONReader::Read(base::as_string_view(bytes), base::JSON_PARSE_CHROMIUM_EXTENSIONS);
    if (!message || !message->is_dict())
      return;
    base::DictValue& dict = message->GetDict();
    if (std::optional<int> id = dict.FindInt("id")) {
      auto found = replies_.find(*id);
      if (found == replies_.end())
        return;
      Reply reply = std::move(found->second);
      replies_.erase(found);
      if (!reply)
        return;
      if (const std::string* error = dict.FindStringByDottedPath("error.message")) {
        std::move(reply).Run(nullptr, *error);
        return;
      }
      const base::DictValue* result = dict.FindDict("result");
      base::DictValue empty;
      std::move(reply).Run(result ? result : &empty, std::string());
      return;
    }
    const std::string* method = dict.FindString("method");
    base::DictValue* params = dict.FindDict("params");
    auto found = workers_.find(host->GetId());
    if (!method || !params || found == workers_.end())
      return;
    WorkerEvent(found->second, *method, *params);
  }
  void AgentHostClosed(content::DevToolsAgentHost* host) override {
    auto found = workers_.find(host->GetId());
    if (found != workers_.end())
      found->second.host = nullptr;
    Detached(host->GetId());
  }

 private:
  // A request of the worker's, from Network.requestWillBeSent on, and the
  // Fetch interception that holds it (crNetworkManager.ts pairs them by
  // networkId, in either order).
  struct Request {
    std::string id;  // the endpoint's: "s<n>", one per hop
    double timestamp = 0;
    double wall_time = 0;
    bool emitted = false;
    std::optional<base::DictValue> paused;
  };
  struct Worker {
    std::string id;
    std::string session;  // its DevTools host's id
    scoped_refptr<content::DevToolsAgentHost> host;
    raw_ptr<content::BrowserContext> profile;
    bool intercepting = false;
    std::string headers_applied;
    std::optional<bool> offline_applied;
    std::map<std::string, Request> requests;  // by DevTools' requestId
    std::map<std::string, base::DictValue> paused;  // by networkId, before their request
    bool script_loaded = false;
    std::vector<std::pair<base::DictValue, Reply>> evaluations;  // until it is
    std::map<int, base::DictValue> console;  // waiting for their values, in order
    int console_next = 0;
  };

  friend class base::NoDestructor<ServiceWorkerTargets>;
  ServiceWorkerTargets() = default;
  ~ServiceWorkerTargets() override = default;

  Worker* ById(const std::string& id) {
    for (auto& [session, worker] : workers_) {
      if (worker.id == id)
        return &worker;
    }
    return nullptr;
  }

  void Send(const std::string& session, const std::string& method, base::DictValue params, Reply reply) {
    auto worker = workers_.find(session);
    if (worker == workers_.end() || !worker->second.host) {
      if (reply)
        std::move(reply).Run(nullptr, kClosed);
      return;
    }
    const int id = ++next_id_;
    replies_[id] = std::move(reply);
    base::DictValue message;
    message.Set("id", id);
    message.Set("method", method);
    message.Set("params", std::move(params));
    const std::string json = Json(base::Value(std::move(message)));
    scoped_refptr<content::DevToolsAgentHost> host = worker->second.host;
    host->DispatchProtocolMessage(this, base::as_byte_span(json));
  }

  content::BrowserContext* AutomatedProfile(const std::string& unique_id) {
    if (!profiles_.contains(unique_id))
      return nullptr;
    for (zebkit_view* view : zebkit_view::Views()) {
      content::WebContents* contents = view->web_contents();
      if (contents && contents->GetBrowserContext()->UniqueId() == unique_id)
        return contents->GetBrowserContext();
    }
    return nullptr;
  }

  void Attached(content::ServiceWorkerDevToolsAgentHost* host, content::BrowserContext* profile) {
    const std::string session = host->GetId();
    Worker& worker = workers_[session];
    worker.id = "sw" + base::NumberToString(++next_worker_);
    worker.session = session;
    worker.host = host;
    worker.profile = profile;
    {
      base::DictValue told;
      told.Set("worker", worker.id);
      told.Set("url", host->GetURL().spec());
      Tell(profile, "BrowserContext.serviceWorker", std::move(told));
    }
    // crServiceWorker.ts: its network unless the context blocks service
    // workers, then Runtime, then on.
    const std::string* policy = ContextStates()[profile].options.FindString("serviceWorkers");
    if (!policy || *policy != "block") {
      Send(session, "Network.enable", base::DictValue(), Reply());
      Apply(worker, true);
    }
    Send(session, "Runtime.enable", base::DictValue(), Reply());
    Send(session, "Runtime.runIfWaitingForDebugger", base::DictValue(), Reply());
  }

  void Detached(const std::string& session) {
    auto found = workers_.find(session);
    if (found == workers_.end())
      return;
    Worker worker = std::move(found->second);
    workers_.erase(found);
    if (worker.host)
      worker.host->DetachClient(this);
    for (auto& [evaluate, reply] : worker.evaluations)
      std::move(reply).Run(nullptr, kClosed);
    std::erase_if(routes_, [&](const auto& entry) { return entry.second.first == session; });
    std::erase_if(bodies_, [&](const auto& entry) { return entry.second.first == session; });
    base::DictValue told;
    told.Set("worker", worker.id);
    Tell(worker.profile, "ServiceWorker.close", std::move(told));
  }

  // The context's settings on the worker's session (crServiceWorker.ts's
  // updateRequestInterception(), updateExtraHTTPHeaders(), updateOffline()).
  void Apply(Worker& worker, bool first) {
    const std::string* policy = ContextStates()[worker.profile].options.FindString("serviceWorkers");
    if (policy && *policy == "block")
      return;
    const ContextState& state = ContextStates()[worker.profile];
    const bool intercept = !Patterns(state).empty();
    if (intercept != worker.intercepting) {
      worker.intercepting = intercept;
      if (intercept) {
        base::DictValue params;
        base::ListValue patterns;
        base::DictValue all;
        all.Set("urlPattern", "*");
        patterns.Append(std::move(all));
        params.Set("patterns", std::move(patterns));
        Send(worker.session, "Fetch.enable", std::move(params), Reply());
      } else {
        Send(worker.session, "Fetch.disable", base::DictValue(), Reply());
      }
      base::DictValue cache;
      cache.Set("cacheDisabled", intercept);
      Send(worker.session, "Network.setCacheDisabled", std::move(cache), Reply());
    }
    base::DictValue headers;
    for (const base::Value& header : state.extra_headers) {
      const std::string* name = header.is_dict() ? header.GetDict().FindString("name") : nullptr;
      const std::string* value = header.is_dict() ? header.GetDict().FindString("value") : nullptr;
      if (name && value)
        headers.Set(*name, *value);
    }
    const std::string headers_json = Json(base::Value(headers.Clone()));
    if (headers_json != worker.headers_applied && (!first || !headers.empty())) {
      worker.headers_applied = headers_json;
      base::DictValue params;
      params.Set("headers", std::move(headers));
      Send(worker.session, "Network.setExtraHTTPHeaders", std::move(params), Reply());
    }
    if (worker.offline_applied != state.offline && (!first || state.offline)) {
      worker.offline_applied = state.offline;
      base::DictValue params;
      params.Set("offline", state.offline);
      params.Set("latency", 0);
      params.Set("downloadThroughput", -1);
      params.Set("uploadThroughput", -1);
      Send(worker.session, "Network.emulateNetworkConditions", std::move(params), Reply());
    }
  }

  static base::ListValue Patterns(const ContextState& state) {
    for (const auto& [method, params] : state.replay) {
      if (method == "Page.setNetworkInterceptionPatterns") {
        if (const base::ListValue* patterns = params.FindList("patterns"))
          return patterns->Clone();
      }
    }
    return base::ListValue();
  }

  void Tell(content::BrowserContext* profile, const std::string& event, base::DictValue params) {
    std::vector<Automation*> pages = Automation::InProfile(profile);
    if (!pages.empty())
      pages.front()->Emit(event, std::move(params));
  }

  // crNetworkManager.ts's errorReasons.
  static std::string ErrorReason(const std::string& code) {
    static const base::NoDestructor<std::map<std::string, std::string>> kReasons(
        std::map<std::string, std::string>{
            {"aborted", "Aborted"},
            {"accessdenied", "AccessDenied"},
            {"addressunreachable", "AddressUnreachable"},
            {"blockedbyclient", "BlockedByClient"},
            {"blockedbyresponse", "BlockedByResponse"},
            {"connectionaborted", "ConnectionAborted"},
            {"connectionclosed", "ConnectionClosed"},
            {"connectionfailed", "ConnectionFailed"},
            {"connectionrefused", "ConnectionRefused"},
            {"connectionreset", "ConnectionReset"},
            {"internetdisconnected", "InternetDisconnected"},
            {"namenotresolved", "NameNotResolved"},
            {"timedout", "TimedOut"},
            {"failed", "Failed"},
        });
    auto found = kReasons->find(code);
    return found == kReasons->end() ? "Failed" : found->second;
  }

  // DevTools' headers object as the protocol's list (several values of a
  // name are on lines of their own).
  static base::ListValue HeaderList(const base::DictValue* headers) {
    base::ListValue list;
    if (!headers)
      return list;
    for (const auto [name, value] : *headers) {
      if (!value.is_string())
        continue;
      for (const std::string& line :
           base::SplitString(value.GetString(), "\n", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL)) {
        base::DictValue header;
        header.Set("name", name);
        header.Set("value", line);
        list.Append(std::move(header));
      }
    }
    return list;
  }

  // crExecutionContext.ts's renderPreview().
  static std::string Preview(const base::DictValue& object) {
    const std::string* type = object.FindString("type");
    if (type && *type == "undefined")
      return "undefined";
    if (const std::string* unserializable = object.FindString("unserializableValue"))
      return *unserializable;
    if (const base::Value* value = object.Find("value")) {
      if (value->is_string())
        return value->GetString();
      if (value->is_none())
        return "null";
      if (value->is_bool())
        return value->GetBool() ? "true" : "false";
      if (value->is_int())
        return base::NumberToString(value->GetInt());
      if (value->is_double()) {
        const double number = value->GetDouble();
        if (number == static_cast<double>(static_cast<int64_t>(number)) && std::abs(number) < 1e15)
          return base::NumberToString(static_cast<int64_t>(number));
        return base::NumberToString(number);
      }
      return Json(*value);
    }
    const std::string* description = object.FindString("description");
    return description ? *description : std::string();
  }

  void WorkerEvent(Worker& worker, const std::string& method, base::DictValue& params) {
    if (method == "Inspector.workerScriptLoaded") {
      worker.script_loaded = true;
      // Started: a restart of the same version is not held again (nothing
      // here would release it).
      if (worker.host)
        static_cast<content::ServiceWorkerDevToolsAgentHost*>(worker.host.get())->set_should_pause_on_start(false);
      auto evaluations = std::move(worker.evaluations);
      for (auto& [evaluate, reply] : evaluations)
        Send(worker.session, "Runtime.evaluate", std::move(evaluate), std::move(reply));
      return;
    }
    if (method == "Runtime.consoleAPICalled") {
      base::DictValue told;
      told.Set("worker", worker.id);
      const std::string* type = params.FindString("type");
      told.Set("type", type ? *type : "log");
      std::vector<std::string> parts;
      if (const base::ListValue* args = params.FindList("args")) {
        for (const base::Value& arg : *args) {
          if (arg.is_dict())
            parts.push_back(Preview(arg.GetDict()));
        }
      }
      told.Set("text", base::JoinString(parts, " "));
      base::DictValue location;
      if (const base::ListValue* frames = params.FindListByDottedPath("stackTrace.callFrames");
          frames && !frames->empty() && frames->front().is_dict()) {
        const base::DictValue& top = frames->front().GetDict();
        const std::string* url = top.FindString("url");
        location.Set("url", url ? *url : std::string());
        location.Set("lineNumber", top.FindInt("lineNumber").value_or(0));
        location.Set("columnNumber", top.FindInt("columnNumber").value_or(0));
      }
      told.Set("location", std::move(location));
      if (std::optional<double> timestamp = params.FindDouble("timestamp"))
        told.Set("timestamp", *timestamp);
      // Its arguments' values (the client's handles of them answer
      // jsonValue()), in the messages' order.
      base::ListValue arguments;
      std::string object_id;
      if (const base::ListValue* args = params.FindList("args")) {
        for (const base::Value& arg : *args) {
          if (!arg.is_dict())
            continue;
          base::DictValue argument;
          if (const std::string* id = arg.GetDict().FindString("objectId")) {
            argument.Set("objectId", *id);
            if (object_id.empty())
              object_id = *id;
          } else if (const std::string* unserializable = arg.GetDict().FindString("unserializableValue")) {
            argument.Set("unserializableValue", *unserializable);
          } else if (const base::Value* value = arg.GetDict().Find("value")) {
            argument.Set("value", value->Clone());
          }
          base::DictValue preview;
          preview.Set("preview", Preview(arg.GetDict()));
          told.EnsureList("previews")->Append(std::move(preview));
          arguments.Append(std::move(argument));
        }
      }
      const int order = ++worker.console_next;
      worker.console[order] = std::move(told);
      if (object_id.empty()) {
        ConsoleValues(worker.session, order, base::ListValue(), params.FindList("args"));
        return;
      }
      base::DictValue call;
      call.Set("objectId", object_id);
      call.Set("functionDeclaration", "function(...a) { return a; }");
      call.Set("arguments", std::move(arguments));
      call.Set("returnByValue", true);
      base::ListValue fallback = params.FindList("args") ? params.FindList("args")->Clone() : base::ListValue();
      Send(worker.session, "Runtime.callFunctionOn", std::move(call),
           base::BindOnce(
               [](std::string session, int order, base::ListValue args, const base::DictValue* result,
                  const std::string&) {
                 const base::ListValue* values =
                     result ? result->FindListByDottedPath("result.value") : nullptr;
                 Get().ConsoleValues(session, order, values ? values->Clone() : base::ListValue(), &args);
               },
               worker.session, order, std::move(fallback)));
    } else if (method == "Network.requestWillBeSent") {
      RequestWillBeSent(worker, params);
    } else if (method == "Fetch.requestPaused") {
      const std::string* network_id = params.FindString("networkId");
      if (network_id) {
        auto found = worker.requests.find(*network_id);
        if (found != worker.requests.end() && found->second.emitted && !found->second.paused) {
          Paused(worker, found->second, params);
          return;
        }
        worker.paused[*network_id] = params.Clone();
        return;
      }
      // One the Network domain does not tell: on as it is.
      base::DictValue go;
      if (const std::string* id = params.FindString("requestId"))
        go.Set("requestId", *id);
      Send(worker.session, "Fetch.continueRequest", std::move(go), Reply());
    } else if (method == "Network.responseReceived") {
      const std::string* request_id = params.FindString("requestId");
      auto found = request_id ? worker.requests.find(*request_id) : worker.requests.end();
      if (found == worker.requests.end() || !found->second.emitted)
        return;
      if (const base::DictValue* response = params.FindDict("response"))
        Responded(worker, found->second, *response);
      bodies_[found->second.id] = {worker.session, *request_id};
      if (bodies_.size() > 500)
        bodies_.erase(bodies_.begin());
    } else if (method == "Network.loadingFinished" || method == "Network.loadingFailed") {
      const std::string* request_id = params.FindString("requestId");
      auto found = request_id ? worker.requests.find(*request_id) : worker.requests.end();
      if (found == worker.requests.end())
        return;
      Request request = std::move(found->second);
      worker.requests.erase(found);
      worker.paused.erase(*request_id);
      if (!request.emitted)
        return;
      base::DictValue told;
      told.Set("worker", worker.id);
      told.Set("request", request.id);
      if (method == "Network.loadingFinished") {
        const double end = params.FindDouble("timestamp").value_or(request.timestamp);
        told.Set("responseEndTiming", (end - request.timestamp) * 1000);
        Tell(worker.profile, "ServiceWorker.requestFinished", std::move(told));
      } else {
        const std::string* text = params.FindString("errorText");
        told.Set("failureText", text ? *text : std::string("net::ERR_FAILED"));
        Tell(worker.profile, "ServiceWorker.requestFailed", std::move(told));
      }
    }
  }

  // A console message's values come (@values, else those of @args that
  // have one): told with every message before it that has its values.
  void ConsoleValues(const std::string& session, int order, base::ListValue values,
                     const base::ListValue* args) {
    auto found = workers_.find(session);
    if (found == workers_.end())
      return;
    Worker& worker = found->second;
    auto message = worker.console.find(order);
    if (message == worker.console.end())
      return;
    if (values.empty() && args) {
      for (const base::Value& arg : *args) {
        const base::Value* value = arg.is_dict() ? arg.GetDict().Find("value") : nullptr;
        values.Append(value ? value->Clone() : base::Value());
      }
    }
    message->second.Set("values", std::move(values));
    message->second.Set("ready", true);
    while (!worker.console.empty() && worker.console.begin()->second.FindBool("ready").value_or(false)) {
      base::DictValue told = std::move(worker.console.begin()->second);
      worker.console.erase(worker.console.begin());
      told.Remove("ready");
      Tell(worker.profile, "ServiceWorker.console", std::move(told));
    }
  }

  void RequestWillBeSent(Worker& worker, base::DictValue& params) {
    const std::string* request_id = params.FindString("requestId");
    const base::DictValue* request = params.FindDict("request");
    if (!request_id || !request)
      return;
    std::string redirected_from;
    auto existing = worker.requests.find(*request_id);
    if (existing != worker.requests.end()) {
      // A redirect: the hop before ends with its response.
      if (const base::DictValue* response = params.FindDict("redirectResponse");
          response && existing->second.emitted) {
        Responded(worker, existing->second, *response);
        base::DictValue told;
        told.Set("worker", worker.id);
        told.Set("request", existing->second.id);
        told.Set("responseEndTiming", -1);
        Tell(worker.profile, "ServiceWorker.requestFinished", std::move(told));
        redirected_from = existing->second.id;
      }
      worker.requests.erase(existing);
    }
    Request& hop = worker.requests[*request_id];
    hop.id = "s" + base::NumberToString(++next_request_);
    hop.timestamp = params.FindDouble("timestamp").value_or(0);
    hop.wall_time = params.FindDouble("wallTime").value_or(base::Time::Now().InSecondsFSinceUnixEpoch());
    base::DictValue told;
    told.Set("worker", worker.id);
    told.Set("request", hop.id);
    std::string url = request->FindString("url") ? *request->FindString("url") : std::string();
    if (const std::string* fragment = request->FindString("urlFragment"))
      url += *fragment;
    told.Set("url", GURL(url).is_valid() ? GURL(url).GetWithoutRef().spec() : url);
    const std::string* method = request->FindString("method");
    told.Set("method", method ? *method : "GET");
    told.Set("headers", HeaderList(request->FindDict("headers")));
    const std::string* type = params.FindString("type");
    told.Set("resourceType", type ? base::ToLowerASCII(*type) : std::string("other"));
    told.Set("isNavigationRequest", false);
    if (!redirected_from.empty())
      told.Set("redirectedFrom", redirected_from);
    if (const base::ListValue* entries = request->FindList("postDataEntries")) {
      std::string body;
      for (const base::Value& entry : *entries) {
        const std::string* bytes = entry.is_dict() ? entry.GetDict().FindString("bytes") : nullptr;
        std::string decoded;
        if (bytes && base::Base64Decode(*bytes, &decoded))
          body += decoded;
      }
      if (!body.empty())
        told.Set("postData", base::Base64Encode(body));
    } else if (const std::string* post = request->FindString("postData")) {
      told.Set("postData", base::Base64Encode(*post));
    }
    hop.emitted = true;
    Tell(worker.profile, "ServiceWorker.request", std::move(told));
    // Held already by Fetch: routed now.
    auto paused = worker.paused.find(*request_id);
    if (paused != worker.paused.end()) {
      base::DictValue event = std::move(paused->second);
      worker.paused.erase(paused);
      Paused(worker, hop, event);
    }
  }

  void Paused(Worker& worker, Request& request, const base::DictValue& params) {
    request.paused = params.Clone();
    const std::string* paused = params.FindString("requestId");
    const std::string* url = params.FindStringByDottedPath("request.url");
    if (!paused)
      return;
    const base::ListValue patterns = Patterns(ContextStates()[worker.profile]);
    if (!url || patterns.empty() || !NetworkPatternsMatch(patterns, *url)) {
      base::DictValue go;
      go.Set("requestId", *paused);
      Send(worker.session, "Fetch.continueRequest", std::move(go), Reply());
      return;
    }
    const std::string route = "swr" + base::NumberToString(++next_route_);
    routes_[route] = {worker.session, *paused};
    base::DictValue told;
    told.Set("worker", worker.id);
    told.Set("route", route);
    base::DictValue held;
    held.Set("request", request.id);
    told.Set("request", std::move(held));
    Tell(worker.profile, "ServiceWorker.route", std::move(told));
  }

  void Responded(Worker& worker, const Request& request, const base::DictValue& response) {
    base::DictValue told;
    told.Set("worker", worker.id);
    told.Set("request", request.id);
    const std::string* url = response.FindString("url");
    told.Set("url", url ? GURL(*url).GetWithoutRef().spec() : std::string());
    told.Set("status", response.FindInt("status").value_or(200));
    const std::string* text = response.FindString("statusText");
    told.Set("statusText", text ? *text : std::string());
    told.Set("headers", HeaderList(response.FindDict("headers")));
    const std::string* mime = response.FindString("mimeType");
    told.Set("mimeType", mime ? *mime : std::string());
    told.Set("fromServiceWorker", response.FindBool("fromServiceWorker").value_or(false));
    base::DictValue timing;
    if (const base::DictValue* given = response.FindDict("timing")) {
      const double start = given->FindDouble("requestTime").value_or(request.timestamp);
      timing.Set("startTime", (start - request.timestamp + request.wall_time) * 1000);
      for (const auto& [to, from] :
           {std::pair{"domainLookupStart", "dnsStart"}, {"domainLookupEnd", "dnsEnd"},
            {"connectStart", "connectStart"}, {"secureConnectionStart", "sslStart"},
            {"connectEnd", "connectEnd"}, {"requestStart", "sendStart"},
            {"responseStart", "receiveHeadersEnd"}}) {
        timing.Set(to, given->FindDouble(from).value_or(-1));
      }
    } else {
      timing.Set("startTime", request.wall_time * 1000);
      for (const char* key : {"domainLookupStart", "domainLookupEnd", "connectStart",
                              "secureConnectionStart", "connectEnd", "requestStart", "responseStart"}) {
        timing.Set(key, -1);
      }
    }
    told.Set("timing", std::move(timing));
    if (const std::string* address = response.FindString("remoteIPAddress")) {
      std::string host = *address;
      if (host.size() > 2 && host.front() == '[' && host.back() == ']')
        host = host.substr(1, host.size() - 2);
      told.Set("serverAddr", host);
    }
    if (std::optional<int> port = response.FindInt("remotePort"))
      told.Set("serverPort", *port);
    if (const std::string* protocol = response.FindString("protocol")) {
      if (*protocol == "h2")
        told.Set("httpVersion", "2.0");
      else if (*protocol == "h3")
        told.Set("httpVersion", "3.0");
      else if (protocol->starts_with("http/"))
        told.Set("httpVersion", protocol->substr(5));
    }
    Tell(worker.profile, "ServiceWorker.response", std::move(told));
  }

  bool observing_ = false;
  std::set<std::string> profiles_;
  std::map<std::string, Worker> workers_;  // by DevTools session
  std::map<std::string, std::pair<std::string, std::string>> routes_;  // route: session, Fetch id
  std::map<std::string, std::pair<std::string, std::string>> bodies_;  // request: session, Network id
  std::map<int, Reply> replies_;
  int next_id_ = 0;
  int next_worker_ = 0;
  int next_request_ = 0;
  int next_route_ = 0;
};

void UpdateServiceWorkers(content::BrowserContext* profile) {
  ServiceWorkerTargets::Get().ContextChanged(profile);
}

void ServiceWorkerTargets_Watch(content::BrowserContext* profile) {
  if (profile)
    ServiceWorkerTargets::Get().Watch(profile);
}

// ServiceWorker.Route.continue / fulfill / abort {route, ...}.
void ServiceWorkerRoute(Call* call) {
  const std::string how = call->Name().substr(call->Name().find('.') + 1);
  if (!ServiceWorkerTargets::Get().Route(call->String("route"), how, call->params()))
    call->Fail("Route is already handled!");
  else
    call->Done();
}

// ServiceWorker.Response.body {request}: {binary}.
void ServiceWorkerBody(Call* call) {
  ServiceWorkerTargets::Get().Body(
      call->String("request"),
      base::BindOnce(
          [](base::WeakPtr<Call> call, const base::DictValue* result, const std::string& error) {
            if (!call)
              return;
            if (!result) {
              call->Fail(error);
              return;
            }
            const std::string* body = result->FindString("body");
            base::DictValue answer;
            answer.Set("binary", !body ? std::string()
                                 : result->FindBool("base64Encoded").value_or(false) ? *body
                                                                                     : base::Base64Encode(*body));
            call->Done(base::Value(std::move(answer)));
          },
          call->GetWeakPtr()));
}

// BrowserContext.restoreStorage {context, origins}: each origin's IndexedDB
// and OPFS put back, as browserContext.ts's setStorageState() restores them in
// a page of the origin; the origins remembered for storageState().
void RestoreStorage(Call* call) {
  zebkit_context* context = RegisteredContext(call->String("context"));
  content::BrowserContext* profile = context ? ProfileOf(context) : nullptr;
  std::vector<base::DictValue> origins;
  if (const base::ListValue* given = call->params().FindList("origins")) {
    for (const base::Value& item : *given) {
      if (!item.is_dict())
        continue;
      const std::string* origin = item.GetDict().FindString("origin");
      if (profile && origin)
        ContextStates()[profile].storage_origins.insert(*origin);
      const base::ListValue* databases = item.GetDict().FindList("indexedDB");
      const base::ListValue* files = item.GetDict().FindList("opfs");
      if ((databases && !databases->empty()) || (files && !files->empty()))
        origins.push_back(item.GetDict().Clone());
    }
  }
  if (!profile || origins.empty()) {
    call->Done();
    return;
  }
  VisitOrigins(profile, std::move(origins),
               base::BindRepeating([](const base::DictValue& entry) {
                 return StorageScriptCall("restore(" + Json(base::Value(entry.Clone())) + ")");
               }),
               base::BindOnce([](base::WeakPtr<Call> call, OriginValues) {
                 if (call)
                   call->Done();
               },
                              call->GetWeakPtr()));
}

// ServiceWorker.evaluateExpression {worker, expression, isFunction, arg}.
void ServiceWorkerEvaluate(Call* call) {
  const std::string id = call->String("worker");
  if (!ServiceWorkerTargets::Get().ProfileOf(id)) {
    call->Fail(kClosed);
    return;
  }
  const base::Value* arg = call->params().Find("arg");
  const std::string arg_json = arg ? Json(*arg) : "undefined";
  const std::string expression = call->String("expression");
  const std::optional<bool> is_function = call->params().FindBool("isFunction");
  const std::string source =
      is_function.value_or(false)
          ? "(" + expression + ")(" + arg_json + ")"
          : is_function.has_value()
                ? expression
                : "(() => { const f = (" + expression + "); return typeof f === 'function' ? f(" + arg_json +
                      ") : f; })()";
  ServiceWorkerTargets::Get().Evaluate(
      id, source,
      base::BindOnce(
          [](base::WeakPtr<Call> call, const base::DictValue* result, const std::string& error) {
            if (!call)
              return;
            if (!result) {
              call->Fail(error);
              return;
            }
            if (const base::DictValue* details = result->FindDict("exceptionDetails")) {
              const std::string* description = details->FindStringByDottedPath("exception.description");
              const std::string* text = details->FindString("text");
              call->Fail(description ? *description : text ? *text : "Error");
              return;
            }
            const base::Value* value = result->FindByDottedPath("result.value");
            call->Answer(value ? value->Clone() : base::Value());
          },
          call->GetWeakPtr()));
}

// An automated page's window.open(): held until the popup has its view.
void PopupHold::Hook(content::WebContents* opener, bool hold) {
  if (hold && (!opener || !Automation::FromWebContents(opener)))
    return;
  Get().Set(hold);
}

// The dialogs of pages with automation, for Playwright's dialog events.
bool AnswerAfterConsole(zebkit_view* view, bool barrier, base::RepeatingClosure answer) {
  content::WebContents* contents = view ? view->web_contents() : nullptr;
  Automation* automation = contents ? Automation::FromWebContents(contents) : nullptr;
  return automation && automation->AfterConsole(barrier, std::move(answer));
}

class AutomationDialogs : public DialogObserver {
 public:
  bool HoldsDialogs(content::WebContents* contents) override {
    Automation* automation = Automation::FromWebContents(contents);
    return automation && automation->holds_dialogs();
  }
  void DialogOpened(content::WebContents* contents,
                    const zebkit_dialog& dialog,
                    bool held_elsewhere) override {
    if (Automation* automation = Automation::FromWebContents(contents))
      automation->DialogOpened(dialog, held_elsewhere);
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
    if (auto* delegate = static_cast<EngineDownloadManagerDelegate*>(
            context->GetDownloadManagerDelegate())) {
      delegate->SetDownloadDirectory(directory_.GetPath());
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
    // The name the download is likely to get, as Chrome's DevTools gives
    // Playwright (Page.downloadWillBegin): from Content-Disposition, the
    // page's download attribute or the URL, before a target is chosen.
    params.Set("suggestedFilename",
               base::UTF16ToUTF8(net::GetSuggestedFilename(item->GetURL(), item->GetContentDisposition(),
                                                           std::string(), item->GetSuggestedFilename(),
                                                           item->GetMimeType(), "download")));
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
          {"Frame.waitForSelector", &A::Prechecked<&A::WaitForSelector>},
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
          {"Frame.expect", &A::Prechecked<&A::Expect>},
          {"Frame.click", &A::Prechecked<&A::Click>},
          {"Frame.dblclick", &A::Prechecked<&A::Dblclick>},
          {"Frame.hover", &A::Prechecked<&A::Hover>},
          {"Frame.tap", &A::Prechecked<&A::Tap>},
          {"Frame.check", &A::Prechecked<&A::Check>},
          {"Frame.uncheck", &A::Prechecked<&A::Uncheck>},
          {"Frame.dragAndDrop", &A::Prechecked<&A::DragAndDrop>},
          {"Frame.fill", &A::Prechecked<&A::Fill>},
          {"Frame.focus", &A::Prechecked<&A::Focus>},
          {"Frame.blur", &A::Prechecked<&A::Blur>},
          {"Frame.press", &A::Prechecked<&A::FramePress>},
          {"Frame.type", &A::Prechecked<&A::FrameType>},
          {"Frame.selectOption", &A::Prechecked<&A::SelectOption>},
          {"Frame.setInputFiles", &A::Prechecked<&A::SetInputFiles>},
          {"Frame.dispatchEvent", &A::DispatchEvent},
          {"Frame.drop", &A::Prechecked<&A::Drop>},
          {"Frame.goto", &A::Goto},
          {"Frame.webmcpTools", &A::WebMcpTools},
          {"Frame.webmcpCallTool", &A::WebMcpCallTool},
          // ElementHandle
          {"ElementHandle.evaluateExpression", &A::Evaluate},
          {"ElementHandle.evaluateExpressionHandle", &A::Evaluate},
          {"ElementHandle.evalOnSelector", &A::EvalOnSelector},
          {"ElementHandle.evalOnSelectorAll", &A::EvalOnSelector},
          {"ElementHandle.querySelector", &A::QuerySelector},
          {"ElementHandle.querySelectorAll", &A::QuerySelector},
          {"ElementHandle.boundingBox", &A::BoundingBox},
          {"ElementHandle.check", &A::Prechecked<&A::Check>},
          {"ElementHandle.uncheck", &A::Prechecked<&A::Uncheck>},
          {"ElementHandle.setChecked", &A::Prechecked<&A::SetChecked>},
          {"ElementHandle.click", &A::Prechecked<&A::Click>},
          {"ElementHandle.dblclick", &A::Prechecked<&A::Dblclick>},
          {"ElementHandle.hover", &A::Prechecked<&A::Hover>},
          {"ElementHandle.tap", &A::Prechecked<&A::Tap>},
          {"ElementHandle.contentFrame", &A::ContentFrame},
          {"ElementHandle.ownerFrame", &A::OwnerFrame},
          {"ElementHandle.dispatchEvent", &A::DispatchEvent},
          {"ElementHandle.fill", &A::Prechecked<&A::Fill>},
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
          {"ElementHandle.selectOption", &A::Prechecked<&A::SelectOption>},
          {"ElementHandle.selectText", &A::SelectText},
          {"ElementHandle.setInputFiles", &A::SetInputFiles},
          {"ElementHandle.textContent", &A::TextContent},
          {"ElementHandle.waitForElementState", &A::WaitForElementState},
          {"ElementHandle.waitForSelector", &A::Prechecked<&A::WaitForSelector>},
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
          {"Page.waitForInitialNavigation", &A::WaitForInitialNavigation},
          // Playwright's screencast overlays are its server's own drawings in
          // the page: with none drawn, hiding them is nothing to do.
          {"Page.screencastSetOverlayVisible", &A::NothingToDo},
          {"Page.captureSnapshot", &A::CaptureSnapshot},
          {"Page.screencastHideActions", &A::NothingToDo},
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
          {"Page.expectScreenshot", &A::Prechecked<&A::ExpectScreenshot>},
          {"Page.setViewportSize", &A::SetViewportSize},
          {"Page.startJSCoverage", &A::StartJSCoverage},
          {"Page.registerLocatorHandler", &A::RegisterLocatorHandler},
          {"Page.resolveLocatorHandlerNoReply", &A::ResolveLocatorHandler},
          {"Page.unregisterLocatorHandler", &A::UnregisterLocatorHandler},
          {"Page.stopJSCoverage", &A::StopJSCoverage},
          {"Page.startCSSCoverage", &A::StartCSSCoverage},
          {"Page.stopCSSCoverage", &A::StopCSSCoverage},
          {"Page.addInitScript", &A::AddInitScript},
          {"Page.evaluateInFrames", &A::EvaluateInFrames},
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
          {"Page.requestGC", &A::RequestGC},
          {"Page.runBeforeUnload", &A::RunBeforeUnload},
          {"Page.webStorageItems", &A::WebStorage},
          {"Page.webStorageGetItem", &A::WebStorage},
          {"Page.webStorageSetItem", &A::WebStorage},
          {"Page.webStorageRemoveItem", &A::WebStorage},
          {"Page.webStorageClear", &A::WebStorage},
          {"Frame.frameElement", &A::FrameElement},
          {"Frame.nodeHandle", &A::NodeHandle},
          {"Frame.resolveSelector", &A::ResolveSelector},
          {"Page.exposeBinding", &A::ExposeBinding},
          {"Page.workers", &A::Workers},
          {"Worker.evaluateExpression", &A::WorkerEvaluate},
          {"Worker.evaluateExpressionHandle", &A::WorkerEvaluateHandle},
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

// ---- Contexts and pages: commands of no page ------------------------------

// The pages and contexts automation made or was given, by their ids.
struct Registry {
  std::map<std::string, raw_ptr<zebkit_context>> contexts;
  std::map<std::string, base::WeakPtr<zebkit_view>> pages;
  // The other way, looked up, not scanned; an address a later view has again
  // is told apart by the weak pointer.
  std::map<zebkit_view*, std::string> ids;
  int next = 0;

  std::string IdOf(zebkit_view* view) {
    auto known = ids.find(view);
    if (known != ids.end()) {
      auto page = pages.find(known->second);
      if (page != pages.end() && page->second.get() == view)
        return known->second;
      ids.erase(known);
    }
    // Closed pages go as a new one comes, so that these stay the size of
    // what is open.
    std::erase_if(pages, [](const auto& entry) { return !entry.second; });
    std::erase_if(ids, [this](const auto& entry) { return !pages.contains(entry.second); });
    const std::string id = "p" + base::NumberToString(++next);
    pages[id] = view->GetWeakPtr();
    ids[view] = id;
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

zebkit_context* RegisteredContext(const std::string& id) {
  return Registered().Context(id);
}

std::string ContextIdOfProfile(content::BrowserContext* profile) {
  for (auto& [id, context] : Registered().contexts) {
    if (context && ProfileOf(context.get()) == profile)
      return id;
  }
  return std::string();
}

std::string StorageScriptCall(const std::string& call) {
  return "(() => { const script = new (" +
         std::string(reinterpret_cast<const char*>(kStorageScript), sizeof(kStorageScript)) +
         ")('chromium'); return script." + call + "; })()";
}

namespace {
zebkit_context* ContextOfProfile(content::BrowserContext* profile) {
  for (auto& [id, context] : Registered().contexts) {
    if (context && ProfileOf(context.get()) == profile)
      return context.get();
  }
  return nullptr;
}

struct OriginVisit {
  base::WeakPtr<zebkit_view> view;
  std::vector<base::DictValue> origins;
  size_t next = 0;
  base::RepeatingCallback<std::string(const base::DictValue&)> expression;
  base::OnceCallback<void(OriginValues)> done;
  OriginValues values;
};

void VisitNextOrigin(std::unique_ptr<OriginVisit> visit);

void OriginEvaluated(zebkit_view*, const char* json, const char*, void* data) {
  std::unique_ptr<OriginVisit> visit(static_cast<OriginVisit*>(data));
  std::optional<base::Value> parsed = json ? Parse(json) : std::nullopt;
  base::Value* value = parsed && parsed->is_dict() ? parsed->GetDict().Find("value") : nullptr;
  const std::string* origin = visit->origins[visit->next].FindString("origin");
  visit->values.emplace_back(origin ? *origin : std::string(), value ? std::move(*value) : base::Value());
  ++visit->next;
  VisitNextOrigin(std::move(visit));
}

void OriginLoaded(zebkit_view* view, const char*, const char*, void* data) {
  std::unique_ptr<OriginVisit> visit(static_cast<OriginVisit*>(data));
  if (!visit->view) {
    VisitNextOrigin(std::move(visit));
    return;
  }
  base::DictValue params;
  params.Set("expression", visit->expression.Run(visit->origins[visit->next]));
  params.Set("isFunction", false);
  const std::string json = Json(base::Value(std::move(params)));
  zebkit_automation_call(visit->view.get(), "Frame.evaluateExpression", json.c_str(), &OriginEvaluated,
                         visit.release());
}

void VisitNextOrigin(std::unique_ptr<OriginVisit> visit) {
  if (!visit->view || visit->next >= visit->origins.size()) {
    if (visit->view)
      zebkit_view_close(visit->view.get());
    std::move(visit->done).Run(std::move(visit->values));
    return;
  }
  const std::string* origin = visit->origins[visit->next].FindString("origin");
  base::DictValue params;
  params.Set("url", (origin ? *origin : std::string()) + "/");
  params.Set("waitUntil", "load");
  params.Set("timeout", 10000);
  const std::string json = Json(base::Value(std::move(params)));
  zebkit_view* view = visit->view.get();
  zebkit_automation_call(view, "Frame.goto", json.c_str(), &OriginLoaded, visit.release());
}
}  // namespace

void VisitOrigins(content::BrowserContext* profile,
                  std::vector<base::DictValue> origins,
                  base::RepeatingCallback<std::string(const base::DictValue&)> expression,
                  base::OnceCallback<void(OriginValues)> done) {
  zebkit_context* context = ContextOfProfile(profile);
  zebkit_view* view = origins.empty() ? nullptr : zebkit_view_new_offscreen_in(context, nullptr, 800, 600, 1);
  if (!zebkit::LiveView(view) || !view->web_contents()) {
    std::move(done).Run(OriginValues());
    return;
  }
  Automation::For(view);
  if (Network* network = Network::For(view->web_contents()))
    network->SetBlankDocuments(true);
  auto visit = std::make_unique<OriginVisit>();
  visit->view = view->GetWeakPtr();
  visit->origins = std::move(origins);
  visit->expression = std::move(expression);
  visit->done = std::move(done);
  VisitNextOrigin(std::move(visit));
}

std::string Automation::Registered_IdOf() {
  return Registered().IdOf(view_.get());
}

void InstallWebAuthn(content::BrowserContext* profile);

// What Node's tls.createSecureContext() says of a client certificate's PFX that
// does not load (socksClientCertificatesInterceptor.ts), "" when it does.
std::string PfxError(const base::DictValue& given) {
  const std::string* pfx64 = given.FindString("pfx");
  std::string pfx;
  if (!pfx64 || !base::Base64Decode(*pfx64, &pfx) || pfx.empty())
    return std::string();
  const std::string* passphrase = given.FindString("passphrase");
  crypto::OpenSSLErrStackTracer clear_errors(FROM_HERE);
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(pfx.data());
  bssl::UniquePtr<PKCS12> p12(d2i_PKCS12(nullptr, &bytes, pfx.size()));
  EVP_PKEY* key = nullptr;
  X509* cert = nullptr;
  STACK_OF(X509)* chain = nullptr;
  if (p12 && PKCS12_parse(p12.get(), passphrase ? passphrase->c_str() : "", &key, &cert, &chain)) {
    EVP_PKEY_free(key);
    X509_free(cert);
    sk_X509_pop_free(chain, X509_free);
    return std::string();
  }
  const uint32_t error = ERR_peek_last_error();
  if (ERR_GET_LIB(error) == ERR_LIB_PKCS8 && ERR_GET_REASON(error) == PKCS8_R_INCORRECT_PASSWORD)
    return "Failed to load client certificate: mac verify failure";
  return "Unsupported TLS certificate. Most likely you are using an old certificate format.";
}

using ProxyRules = std::pair<std::string, std::string>;
// The proxy of the context being made, until its network context asks.
std::optional<ProxyRules>& PendingProxy() {
  static base::NoDestructor<std::optional<ProxyRules>> pending;
  return *pending;
}
std::map<content::BrowserContext*, ProxyRules>& ContextProxies() {
  static base::NoDestructor<std::map<content::BrowserContext*, ProxyRules>> proxies;
  return *proxies;
}

// Browser.newContext: a profile of its own, in memory, with Playwright's
// options for its pages (viewport, userAgent, hasTouch, javaScriptEnabled,
// colorScheme, reducedMotion, forcedColors, geolocation, permissions,
// extraHTTPHeaders, offline, httpCredentials, storageState's cookies, proxy).
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
  // browserContext.ts's verifyClientCertificates().
  if (const base::ListValue* certificates = call->params().FindList("clientCertificates")) {
    for (const base::Value& entry : *certificates) {
      const base::DictValue* given = entry.GetIfDict();
      if (!given)
        continue;
      auto has = [&](const char* key) {
        const std::string* value = given->FindString(key);
        return value && !value->empty();
      };
      const char* error = !has("origin") ? "clientCertificates.origin is required"
                          : !has("cert") && !has("key") && !has("passphrase") && !has("pfx")
                              ? "None of cert, key, passphrase or pfx is specified"
                          : has("cert") && !has("key") ? "cert is specified without key"
                          : !has("cert") && has("key") ? "key is specified without cert"
                          : has("pfx") && (has("cert") || has("key"))
                              ? "pfx is specified together with cert, key or passphrase"
                              : nullptr;
      if (error) {
        call->Fail(error);
        return;
      }
      if (const std::string pfx = PfxError(*given); !pfx.empty()) {
        call->Fail(pfx);
        return;
      }
    }
  }
  // browserContext.ts's normalizeProxySettings(), then crBrowser.ts's
  // proxyServer and proxyBypassList; the network context takes them
  // (AutomationProxyFor()).
  std::optional<ProxyRules> proxy;
  if (const base::DictValue* given = call->params().FindDict("proxy")) {
    const std::string* server = given->FindString("server");
    const std::string* username = given->FindString("username");
    const std::string* password = given->FindString("password");
    GURL url(server ? *server : std::string());
    if (!url.is_valid() || !url.has_host())
      url = GURL("http://" + (server ? *server : std::string()));
    const bool credentials = (username && !username->empty()) || (password && !password->empty());
    if (url.SchemeIs("socks4") && credentials) {
      call->Fail("Socks4 proxy protocol does not support authentication");
      return;
    }
    if (url.SchemeIs("socks5") && credentials) {
      call->Fail("Browser does not support socks5 proxy authentication");
      return;
    }
    std::string bypass;
    if (const std::string* list = given->FindString("bypass")) {
      std::vector<std::string> hosts = base::SplitString(*list, ",", base::TRIM_WHITESPACE,
                                                         base::SPLIT_WANT_ALL);
      bypass = base::JoinString(hosts, ",");
    }
    // shouldProxyLoopback(): unless the bypass names a loopback host.
    bool loopback = true;
    for (const std::string& host : base::SplitString(bypass, ",", base::TRIM_WHITESPACE,
                                                     base::SPLIT_WANT_ALL)) {
      for (const char* name : {"localhost", "127.0.0.1", "::1", "[::]", "[::1]", "<loopback>", "<-loopback>"})
        loopback &= host != name;
    }
    if (loopback)
      bypass = "<-loopback>" + (bypass.empty() ? std::string() : "," + bypass);
    std::string host(url.host());
    if (url.HostIsIPAddress() && host.find(':') != std::string::npos && !host.starts_with("["))
      host = "[" + host + "]";
    proxy = ProxyRules(std::string(url.scheme()) + "://" + host +
                           (url.has_port() ? ":" + std::string(url.port()) : std::string()),
                       bypass);
  }
  PendingProxy() = proxy;
  zebkit_context* context = zebkit_context_new();
  const std::string id = "x" + base::NumberToString(++Registered().next);
  Registered().contexts[id] = context;
  content::BrowserContext* profile = ProfileOf(context);
  if (PendingProxy()) {
    ContextProxies()[profile] = *PendingProxy();
    PendingProxy().reset();
  }
  ContextState& state = ContextStates()[profile];
  state.options = call->params().Clone();
  if (const base::DictValue* geolocation = state.options.FindDict("geolocation")) {
    state.geolocation = content::GeolocationOverride{
        geolocation->FindDouble("latitude").value_or(0),
        geolocation->FindDouble("longitude").value_or(0),
        geolocation->FindDouble("accuracy").value_or(0)};
  }
  state.credentials = HttpCredentialsOf(state.options);
  // browserContext.ts's authenticateProxyViaCredentials().
  if (const std::string* username = state.options.FindStringByDottedPath("proxy.username");
      username && !username->empty()) {
    const std::string* password = state.options.FindStringByDottedPath("proxy.password");
    base::DictValue entry;
    entry.Set("username", *username);
    entry.Set("password", password ? *password : std::string());
    state.credentials = base::ListValue();
    state.credentials.Append(std::move(entry));
  }
  if (const base::ListValue* headers = state.options.FindList("extraHTTPHeaders"))
    state.extra_headers = headers->Clone();
  state.offline = state.options.FindBool("offline").value_or(false);
  if (const base::ListValue* engines = state.options.FindList("selectorEngines"))
    state.selector_engines = engines->Clone();
  if (const std::string* attribute = state.options.FindString("testIdAttributeName"))
    state.test_id_attribute = *attribute;
  // setStorageState()'s credentials: the passkeys, and the authenticator.
  if (const base::ListValue* passkeys = state.options.FindListByDottedPath("storageState.credentials");
      passkeys && !passkeys->empty()) {
    for (const base::Value& passkey : *passkeys) {
      std::string error;
      if (passkey.is_dict())
        AddPasskey(state, passkey.GetDict(), nullptr, &error);
    }
    InstallWebAuthn(profile);
  }
  // browserContext.ts's _initialize(): serviceWorkers "block".
  if (const std::string* workers = state.options.FindString("serviceWorkers"); workers && *workers == "block") {
    base::DictValue script;
    script.Set("source",
               "\nif (self.ServiceWorkerContainer) ServiceWorkerContainer.prototype.register = async () => { "
               "console.warn('Service Worker registration blocked by Playwright'); };\n");
    script.Set("registration", RegistrationId(script));
    state.replay.emplace_back("Page.addInitScript", std::move(script));
  }
  base::DictValue answer;
  answer.Set("context", id);
  auto complete = base::BindOnce(
      [](base::WeakPtr<Call> pending, content::BrowserContext* profile,
         base::DictValue options, base::DictValue answer) {
        if (!pending) return;
        // The origins' local storage written first, before any page reads it.
        if (const auto* origins = options.FindListByDottedPath("storageState.origins");
            origins && !origins->empty()) {
          base::ListValue given = origins->Clone();
          options.RemoveByDottedPath("storageState.origins");
          auto again = base::BindOnce(
              [](base::WeakPtr<Call> pending, content::BrowserContext* profile,
                 base::DictValue options, base::DictValue answer, base::Value, const std::string&) {
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
              },
              pending, profile, std::move(options), std::move(answer));
          ContextSetLocalStorage(profile, given, std::move(again));
          return;
        }
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
  const std::string id = call->String("context");
  zebkit_context* context = id.empty() ? nullptr : Registered().Context(id);
  // Not the default profile's pages for a context that has closed.
  if (!id.empty() && !context) {
    call->Fail(kClosed);
    return;
  }
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
// credentials.ts's install(): the binding and the script for the context's
// pages to come (its replay), and in those it has.
void InstallWebAuthn(content::BrowserContext* profile) {
  ContextState& state = ContextStates()[profile];
  if (state.webauthn)
    return;
  state.webauthn = true;
  base::DictValue binding;
  binding.Set("name", kWebAuthnBinding);
  binding.Set("registration", RegistrationId(base::DictValue()));
  state.replay.emplace_back("Page.exposeBinding", std::move(binding));
  base::DictValue script;
  script.Set("source", std::string(reinterpret_cast<const char*>(kWebAuthnScript), sizeof(kWebAuthnScript)));
  script.Set("registration", RegistrationId(base::DictValue()));
  state.replay.emplace_back("Page.addInitScript", std::move(script));
  for (zebkit_view* view : zebkit_view::Views()) {
    if (!view->web_contents() || view->web_contents()->GetBrowserContext() != profile)
      continue;
    if (Automation* automation = Automation::FromWebContents(view->web_contents()))
      automation->InstallWebAuthn();
  }
}

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
  // selectors.ts's register(), for the context's documents from now on.
  if (name == "registerSelectorEngine") {
    const base::DictValue* engine = params.FindDict("selectorEngine");
    const std::string* engine_name = engine ? engine->FindString("name") : nullptr;
    if (!engine_name || engine_name->empty() ||
        !std::all_of(engine_name->begin(), engine_name->end(),
                     [](char c) { return base::IsAsciiAlphaNumeric(c) || c == '_' || c == '-'; })) {
      call->Fail("Selector engine name may only contain [a-zA-Z0-9_] characters");
      return;
    }
    static constexpr const char* kBuiltin[] = {
        "css", "css:light", "xpath", "xpath:light", "_react", "_vue", "text", "text:light", "id",
        "id:light", "data-testid", "data-testid:light", "data-test-id", "data-test-id:light", "data-test",
        "data-test:light", "nth", "visible", "internal:control", "internal:has", "internal:has-not",
        "internal:has-text", "internal:has-not-text", "internal:and", "internal:or", "internal:chain",
        "role", "internal:attr", "internal:label", "internal:text", "internal:role", "internal:testid",
        "internal:describe", "aria-ref", "aria-template", "zs", "zs:light"};
    for (const char* builtin : kBuiltin) {
      if (*engine_name == builtin) {
        call->Fail("\"" + *engine_name + "\" is a predefined selector engine");
        return;
      }
    }
    for (const base::Value& existing : state.selector_engines) {
      const std::string* other = existing.GetDict().FindString("name");
      if (other && *other == *engine_name) {
        call->Fail("\"" + *engine_name + "\" selector engine has been already registered");
        return;
      }
    }
    state.selector_engines.Append(engine->Clone());
    call->Done();
    return;
  }
  if (name == "setTestIdAttributeName") {
    state.test_id_attribute = call->String("testIdAttributeName");
    call->Done();
    return;
  }
  // coverageRecorder.ts's _harvestOriginsWithoutPage(): @expression in a
  // page of each origin the context showed that no page shows now.
  if (name == "harvestOrigins") {
    std::set<std::string> live;
    for (zebkit_view* view : pages) {
      view->web_contents()->GetPrimaryMainFrame()->ForEachRenderFrameHost([&](content::RenderFrameHost* frame) {
        if (!frame->GetLastCommittedOrigin().opaque())
          live.insert(frame->GetLastCommittedOrigin().Serialize());
      });
    }
    std::vector<base::DictValue> origins;
    for (const std::string& origin : state.visited_origins) {
      if (live.contains(origin))
        continue;
      base::DictValue entry;
      entry.Set("origin", origin);
      origins.push_back(std::move(entry));
    }
    if (origins.empty()) {
      base::DictValue answer;
      answer.Set("values", base::ListValue());
      call->Done(base::Value(std::move(answer)));
      return;
    }
    VisitOrigins(profile, std::move(origins),
                 base::BindRepeating([](std::string expression, const base::DictValue&) { return expression; },
                                     call->String("expression")),
                 base::BindOnce(
                     [](base::WeakPtr<Call> call, OriginValues values) {
                       if (!call)
                         return;
                       base::ListValue list;
                       for (auto& [origin, value] : values)
                         list.Append(std::move(value));
                       base::DictValue answer;
                       answer.Set("values", std::move(list));
                       call->Done(base::Value(std::move(answer)));
                     },
                     call->GetWeakPtr()));
    return;
  }
  // credentials.ts: the context's passkeys, and its authenticator.
  if (name == "credentialsCreate") {
    base::DictValue made;
    std::string error;
    if (!AddPasskey(state, params, &made, &error)) {
      call->Fail(error);
      return;
    }
    base::DictValue answer;
    answer.Set("credential", std::move(made));
    call->Done(base::Value(std::move(answer)));
    return;
  }
  if (name == "credentialsGet") {
    const std::string* rp_id = params.FindString("rpId");
    const std::string* wanted = params.FindString("id");
    base::ListValue list;
    for (const Passkey& passkey : state.passkeys) {
      if ((rp_id && !rp_id->empty() && passkey.rp_id != *rp_id) ||
          (wanted && !wanted->empty() && passkey.id != *wanted))
        continue;
      list.Append(PasskeyValue(passkey));
    }
    base::DictValue answer;
    answer.Set("credentials", std::move(list));
    call->Done(base::Value(std::move(answer)));
    return;
  }
  if (name == "credentialsDelete") {
    const std::string* wanted = params.FindString("id");
    std::erase_if(state.passkeys, [&](const Passkey& passkey) { return wanted && passkey.id == *wanted; });
    call->Done();
    return;
  }
  if (name == "credentialsInstall") {
    InstallWebAuthn(profile);
    call->Done();
    return;
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
    // The context's routes apart from each page's own.
    if (name == "setNetworkInterceptionPatterns")
      params.Set("ofContext", true);
    if (name == "setNetworkInterceptionPatterns" ||
        name == "setWebSocketInterceptionPatterns") {
      std::erase_if(state.replay,
                    [&](const auto& entry) { return entry.first == method; });
    }
    state.replay.emplace_back(method, params.Clone());
    if (name == "setNetworkInterceptionPatterns")
      UpdateServiceWorkers(profile);
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
      UpdateServiceWorkers(profile);
    } else if (name == "setExtraHTTPHeaders") {
      if (const base::ListValue* headers = params.FindList("headers"))
        state.extra_headers = headers->Clone();
      UpdateServiceWorkers(profile);
    } else if (name == "setHTTPCredentials") {
      state.credentials = HttpCredentialsOf(params);
    } else if (name == "storageState") {
      ContextCookies(profile, base::DictValue(),
                     base::BindOnce(
                         [](content::BrowserContext* profile, base::WeakPtr<Call> call,
                            base::Value cookies, const std::string& error) {
                           if (!call)
                             return;
                           Automation::StorageWithCookies(profile, call, std::move(cookies));
                         },
                         profile, call->GetWeakPtr()));
      return;
    } else if (name == "setStorageState") {
      // No page: cookies and local storage into the profile, then IndexedDB and
      // OPFS through pages of their origins.
      const base::DictValue* state = params.FindDict("storageState");
      base::DictValue cookies;
      const base::ListValue* list = state ? state->FindList("cookies") : nullptr;
      cookies.Set("cookies", list ? list->Clone() : base::ListValue());
      const base::ListValue* origins = state ? state->FindList("origins") : nullptr;
      base::ListValue given = origins ? origins->Clone() : base::ListValue();
      ContextAddCookies(
          profile, cookies,
          base::BindOnce(
              [](content::BrowserContext* profile, base::WeakPtr<Call> call, std::string context,
                 base::ListValue origins, base::Value, const std::string& error) {
                if (!call)
                  return;
                if (!error.empty()) {
                  call->Fail(error);
                  return;
                }
                ContextSetLocalStorage(
                    profile, origins,
                    base::BindOnce(
                        [](base::WeakPtr<Call> call, std::string context, base::ListValue origins, base::Value,
                           const std::string&) {
                          if (!call)
                            return;
                          base::DictValue restore;
                          restore.Set("context", context);
                          restore.Set("origins", std::move(origins));
                          auto* inner = new Call(nullptr, "BrowserContext.restoreStorage", std::move(restore),
                                                 [](zebkit_view*, const char*, const char*, void* data) {
                                                   auto* outer = static_cast<base::WeakPtr<Call>*>(data);
                                                   if (*outer)
                                                     (*outer)->Done();
                                                   delete outer;
                                                 },
                                                 new base::WeakPtr<Call>(call));
                          RestoreStorage(inner);
                        },
                        call, context, origins.Clone()),
                    true);
              },
              profile, call->GetWeakPtr(), id, std::move(given)));
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
  // Not in the default profile, with its cookies, for a context or page that has closed.
  if ((call->view() && !call->view()->web_contents()) ||
      (!call->view() && !id.empty() && !context)) {
    call->Fail(kClosed);
    return;
  }
  content::BrowserContext* profile =
      call->view() ? call->view()->web_contents()->GetBrowserContext()
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
          {"ServiceWorker.evaluateExpression", &ServiceWorkerEvaluate},
          {"ServiceWorker.Route.continue", &ServiceWorkerRoute},
          {"ServiceWorker.Route.fulfill", &ServiceWorkerRoute},
          {"ServiceWorker.Route.abort", &ServiceWorkerRoute},
          {"ServiceWorker.Response.body", &ServiceWorkerBody},
          {"BrowserContext.restoreStorage", &RestoreStorage},
          {"BrowserContext.pages", &Pages},
          {"BrowserContext.close", &CloseContext},
          {"APIRequestContext.fetch", &ContextFetchCommand},
          {"Automation.abort", &AbortCall},
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


std::optional<std::pair<std::string, std::string>> AutomationProxyFor(
    content::BrowserContext* profile) {
  if (auto found = ContextProxies().find(profile); found != ContextProxies().end())
    return found->second;
  if (PendingProxy()) {
    ContextProxies()[profile] = *PendingProxy();
    PendingProxy().reset();
    return ContextProxies()[profile];
  }
  return std::nullopt;
}

bool AutomationDrivesInput(content::WebContents* contents) {
  return contents && Automation::FromWebContents(contents);
}

void ForgetAutomationContext(content::BrowserContext* profile) {
  ContextStates().erase(profile);
  ContextProxies().erase(profile);
}

namespace {

// One of clientCertificates ({cert, key, passphrase} or {pfx, passphrase},
// base64), as the identity Chromium sends; nullptr pair when it is not one.
std::pair<scoped_refptr<net::X509Certificate>, scoped_refptr<net::SSLPrivateKey>> ClientIdentity(
    const base::DictValue& given) {
  auto decoded = [&](const char* key) {
    std::string bytes;
    const std::string* text = given.FindString(key);
    if (text)
      base::Base64Decode(*text, &bytes);
    return bytes;
  };
  const std::string* passphrase = given.FindString("passphrase");
  crypto::OpenSSLErrStackTracer clear_errors(FROM_HERE);
  bssl::UniquePtr<EVP_PKEY> key;
  scoped_refptr<net::X509Certificate> leaf;
  if (const std::string pfx = decoded("pfx"); !pfx.empty()) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(pfx.data());
    bssl::UniquePtr<PKCS12> p12(d2i_PKCS12(nullptr, &bytes, pfx.size()));
    EVP_PKEY* parsed_key = nullptr;
    X509* parsed_cert = nullptr;
    STACK_OF(X509)* chain = nullptr;
    if (!p12 || !PKCS12_parse(p12.get(), passphrase ? passphrase->c_str() : "", &parsed_key, &parsed_cert, &chain))
      return {};
    key.reset(parsed_key);
    bssl::UniquePtr<X509> cert(parsed_cert);
    bssl::UniquePtr<STACK_OF(X509)> intermediates(chain);
    uint8_t* der = nullptr;
    const int length = cert ? i2d_X509(cert.get(), &der) : -1;
    if (length <= 0)
      return {};
    bssl::UniquePtr<uint8_t> owned(der);
    std::vector<bssl::UniquePtr<CRYPTO_BUFFER>> others;
    for (size_t i = 0; intermediates && i < sk_X509_num(intermediates.get()); ++i) {
      uint8_t* other = nullptr;
      const int other_length = i2d_X509(sk_X509_value(intermediates.get(), i), &other);
      if (other_length <= 0)
        continue;
      bssl::UniquePtr<uint8_t> other_owned(other);
      others.push_back(net::x509_util::CreateCryptoBuffer(
          base::span<const uint8_t>(other, static_cast<size_t>(other_length))));
    }
    leaf = net::X509Certificate::CreateFromBuffer(
        net::x509_util::CreateCryptoBuffer(base::span<const uint8_t>(der, static_cast<size_t>(length))),
        std::move(others));
  } else {
    const std::string cert_pem = decoded("cert");
    const std::string key_pem = decoded("key");
    net::CertificateList list = net::X509Certificate::CreateCertificateListFromBytes(
        base::as_byte_span(cert_pem), net::X509Certificate::FORMAT_PEM_CERT_SEQUENCE);
    if (list.empty())
      return {};
    std::vector<bssl::UniquePtr<CRYPTO_BUFFER>> intermediates;
    for (size_t i = 1; i < list.size(); ++i)
      intermediates.push_back(bssl::UpRef(list[i]->cert_buffer()));
    leaf = net::X509Certificate::CreateFromBuffer(bssl::UpRef(list[0]->cert_buffer()), std::move(intermediates));
    bssl::UniquePtr<BIO> bio(BIO_new_mem_buf(key_pem.data(), key_pem.size()));
    key.reset(bio ? PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr,
                                            passphrase ? const_cast<char*>(passphrase->c_str()) : nullptr)
                  : nullptr);
  }
  if (!leaf || !key)
    return {};
  return {std::move(leaf), net::WrapOpenSSLPrivateKey(std::move(key))};
}

}  // namespace

bool AutomationSelectClientCertificate(
    content::BrowserContext* profile,
    const net::SSLCertRequestInfo& info,
    std::unique_ptr<content::ClientCertificateDelegate>* delegate) {
  auto found = ContextStates().find(profile);
  const base::ListValue* certificates =
      found != ContextStates().end() ? found->second.options.FindList("clientCertificates") : nullptr;
  if (!certificates || info.is_proxy)
    return false;
  const url::Origin server = url::Origin::Create(GURL("https://" + info.host_and_port.ToString()));
  for (const base::Value& entry : *certificates) {
    const std::string* origin = entry.is_dict() ? entry.GetDict().FindString("origin") : nullptr;
    if (!origin || !url::Origin::Create(GURL(*origin)).IsSameOriginWith(server))
      continue;
    auto [certificate, key] = ClientIdentity(entry.GetDict());
    if (!certificate || !key)
      continue;
    (*delegate)->ContinueWithCertificate(std::move(certificate), std::move(key));
    delegate->reset();
    return true;
  }
  return false;
}

std::unique_ptr<content::LoginDelegate> CreateAutomationLoginDelegate(
    const net::AuthChallengeInfo& challenge,
    content::BrowserContext* profile,
    content::WebContents* contents,
    bool main_frame_navigation,
    const GURL& url,
    bool first_attempt,
    content::LoginDelegate::LoginAuthRequiredCallback callback) {
  auto found = ContextStates().find(profile);
  if (found == ContextStates().end())
    return nullptr;
  const base::DictValue* matching =
      MatchingHttpCredentials(found->second.credentials, challenge.challenger.GetURL());
  if (!matching) {
    Automation* automation = contents ? Automation::FromWebContents(contents) : nullptr;
    if (automation && main_frame_navigation && !challenge.is_proxy)
      automation->UnansweredAuth(url);
    return nullptr;
  }
  const base::DictValue& credentials = *matching;
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
  Window::SetFileChooserHandler(base::BindRepeating(
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
        if (HasNoDisplay(contents)) {
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
  // The engine's thread only, in release builds too (zebkit.h, "Threads").
  if (!zebkit::CurrentlyOnEngineThread())
    return;
  // NULL is for the commands of no page; a view that has closed is not
  // touched, by those either: callers may keep its address.
  if (view && !zebkit::LiveView(view)) {
    if (func)
      func(view, nullptr, zebkit::kClosed, data);
    return;
  }
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
  // A view that has closed is not touched: callers may keep its address.
  Automation* automation = zebkit::LiveView(view) ? Automation::For(view) : nullptr;
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

void zebkit_automation_screenshot_pixels(zebkit_view* view,
                                         const char* method,
                                         const char* params,
                                         zebkit_automation_pixels_func func,
                                         void* data) {
  if (!zebkit::CurrentlyOnEngineThread())
    return;
  using zebkit::Automation;
  auto fail = [&](const std::string& error) {
    if (func)
      func(view, nullptr, 0, 0, 0, error.c_str(), data);
  };
  const std::string name = method ? method : "";
  if (name != "Page.screenshot" && name != "ElementHandle.screenshot") {
    fail("zebkit: the pixels are of Page.screenshot or ElementHandle.screenshot, not " + name);
    return;
  }
  std::optional<base::Value> parsed = base::JSONReader::Read(
      params && *params ? params : "{}", base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    fail("params: expected a JSON object");
    return;
  }
  Automation* automation = zebkit::LiveView(view) ? Automation::For(view) : nullptr;
  if (!automation) {
    fail(zebkit::kClosed);
    return;
  }
  // Deletes itself once answered.
  auto* call = new zebkit::Call(view, name, std::move(parsed->GetDict()), nullptr, data);
  call->set_pixels_func(func);
  automation->Run(call, Automation::Handlers().at(name));
}

void zebkit_automation_on_event(zebkit_view* view,
                                zebkit_automation_event_func func,
                                void* data) {
  if (!zebkit::CurrentlyOnEngineThread())
    return;
  if (zebkit::Automation* automation =
          zebkit::LiveView(view) ? zebkit::Automation::For(view) : nullptr) {
    automation->SetEventHandler(func, data);
  }
}

zebkit_view* zebkit_automation_page(const char* page) {
  if (!zebkit::CurrentlyOnEngineThread())
    return nullptr;
  return page ? zebkit::Registered().Page(page) : nullptr;
}

char* zebkit_automation_page_id(zebkit_view* view) {
  if (!zebkit::CurrentlyOnEngineThread())
    return nullptr;
  // NULL for NULL or a closed view, as the C API's rule has it.
  return zebkit::LiveView(view) ? strdup(zebkit::Registered().IdOf(view).c_str()) : nullptr;
}

char* zebkit_automation_commands(void) {
  if (!zebkit::CurrentlyOnEngineThread())
    return nullptr;
  base::ListValue names;
  for (const auto& [name, handler] : zebkit::Automation::Handlers())
    names.Append(name);
  for (const auto& [name, handler] : zebkit::GlobalHandlers())
    names.Append(name);
  return strdup(zebkit::Json(base::Value(std::move(names))).c_str());
}
