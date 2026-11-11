// JavaScript dialogs (<zebkit/page.h>): alert(), confirm(), prompt(), a
// beforeunload handler's "leave this page?", held open and reported to the
// application, or to a CDP client, as each view's policy says, instead of
// the system's dialog, or their dismissal.
//
// Content asks the window's JavaScriptDialogManager for each dialog, after
// telling every DevTools session with the Page domain enabled
// (Page.javascriptDialogOpening). Whoever answers first runs the dialog's
// callback: the application (zebkit_view_dialog_respond), a CDP client
// (Page.handleJavaScriptDialog, after which content tells the manager with
// HandleJavaScriptDialog), or the policy at once. A held dialog lives in the
// page's DialogState until then.

#ifdef UNSAFE_BUFFERS_BUILD
// The C API takes and returns C strings.
#pragma allow_unsafe_buffers
#endif

#include <stdlib.h>
#include <string.h>

#include <list>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/javascript_dialog_manager.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/js_dialog_manager.h"
#include "zebkit/engine/window.h"
#include "url/gurl.h"
#include "zebkit/engine/page_internal.h"
#include "zebkit/engine/settings.h"
#include "zebkit/engine/view.h"

namespace zebkit {

namespace {

DialogObserver* g_dialog_observer = nullptr;

const char* TypeName(zebkit_dialog_type type) {
  switch (type) {
    case ZEBKIT_DIALOG_CONFIRM:
      return "confirm";
    case ZEBKIT_DIALOG_PROMPT:
      return "prompt";
    case ZEBKIT_DIALOG_BEFOREUNLOAD:
      return "beforeunload";
    case ZEBKIT_DIALOG_PRINT:
      return "print";
    default:
      return "alert";
  }
}

uint64_t NextDialogId() {
  static uint64_t next = 0;
  return ++next;
}

// A page's dialogs and what the application asked about them.
class DialogState : public content::WebContentsObserver,
                    public content::WebContentsUserData<DialogState> {
 public:
  ~DialogState() override = default;

  struct Open {
    uint64_t id = 0;
    zebkit_dialog_type type = ZEBKIT_DIALOG_ALERT;
    std::string message;
    std::string default_prompt;
    std::string url;
    bool is_reload = false;
    // Null for print, which waits for nothing.
    content::JavaScriptDialogManager::DialogClosedCallback callback;
  };

  // Unset: the engine's (zebkit_set_engine_settings), else the default.
  std::optional<zebkit_dialog_policy> policy;
  zebkit_dialog_func on_dialog = nullptr;
  void* on_dialog_data = nullptr;
  zebkit_dialog_closed_func on_closed = nullptr;
  void* on_closed_data = nullptr;
  bool close_requested = false;

  zebkit_dialog_policy Policy() const {
    return policy.value_or(EngineDialogPolicy().value_or(ZEBKIT_DIALOG_POLICY_DEFAULT));
  }

  // What becomes of a new dialog: held, or answered now (true or false).
  enum class Decision { kHold, kAccept, kDismiss, kNative };
  Decision Decide() {
    switch (Policy()) {
      case ZEBKIT_DIALOG_POLICY_HOLD:
        return Decision::kHold;
      case ZEBKIT_DIALOG_POLICY_ACCEPT:
        return Decision::kAccept;
      case ZEBKIT_DIALOG_POLICY_DISMISS:
        return Decision::kDismiss;
      default:
        if (on_dialog || HasExternalDevToolsClient(web_contents()) ||
            (g_dialog_observer &&
             g_dialog_observer->HoldsDialogs(web_contents()))) {
          return Decision::kHold;
        }
        return Decision::kNative;
    }
  }

  // Reports @open, and keeps it when it is @held.
  void Add(Open open, bool held) {
    // Kept before the application hears of it, which may answer at once.
    const Open* kept = &open;
    if (held) {
      open_.push_back(std::move(open));
      kept = &open_.back();
    }
    zebkit_dialog info = {};
    info.id = kept->id;
    info.type = kept->type;
    info.message = kept->message.c_str();
    info.default_prompt = kept->default_prompt.c_str();
    info.url = kept->url.c_str();
    info.is_reload = kept->is_reload;
    info.held = held;
    // Copies: an answer in the call removes what @kept points to.
    const std::string message = info.message, prompt = info.default_prompt,
                      url = info.url;
    info.message = message.c_str();
    info.default_prompt = prompt.c_str();
    info.url = url.c_str();
    zebkit_view* view = ViewOf(web_contents());
    // Native automation without a dialog listener dismisses what it alone
    // holds, as Playwright does, and leaves the others' to them.
    const bool held_elsewhere =
        held && (Policy() == ZEBKIT_DIALOG_POLICY_HOLD || on_dialog ||
                 HasExternalDevToolsClient(web_contents()));
    if (g_dialog_observer)
      g_dialog_observer->DialogOpened(web_contents(), info, held_elsewhere);
    if (view && on_dialog)
      on_dialog(view, &info, on_dialog_data);
  }

  void ReportClosed(uint64_t id, bool accepted, const std::string& input) {
    if (g_dialog_observer)
      g_dialog_observer->DialogClosed(web_contents(), id, accepted, input);
    zebkit_view* view = ViewOf(web_contents());
    if (view && on_closed)
      on_closed(view, id, accepted, input.c_str(), on_closed_data);
  }

  // Answers dialog @id; false when it is not open.
  bool Respond(uint64_t id, bool accept, const std::string* text) {
    for (auto it = open_.begin(); it != open_.end(); ++it) {
      if (it->id != id)
        continue;
      Open open = std::move(*it);
      open_.erase(it);
      std::string input;
      if (open.type == ZEBKIT_DIALOG_PROMPT && accept)
        input = text ? *text : open.default_prompt;
      if (open.callback)
        std::move(open.callback).Run(accept, base::UTF8ToUTF16(input));
      ReportClosed(open.id, accept, input);
      return true;
    }
    return false;
  }

  // A CDP client answered the page's dialog: content ran its callback.
  bool AnsweredElsewhere(bool accept, const std::u16string* prompt) {
    for (auto it = open_.begin(); it != open_.end(); ++it) {
      if (!it->callback)
        continue;
      Open open = std::move(*it);
      open_.erase(it);
      std::string input;
      if (open.type == ZEBKIT_DIALOG_PROMPT && accept)
        input = prompt ? base::UTF16ToUTF8(*prompt) : open.default_prompt;
      ReportClosed(open.id, accept, input);
      return true;
    }
    return false;
  }

  // The page went, or navigated: its dialogs close unanswered.
  void CancelAll() {
    std::list<Open> open;
    open.swap(open_);
    for (Open& dialog : open) {
      if (dialog.callback)
        std::move(dialog.callback).Run(false, std::u16string());
      ReportClosed(dialog.id, false, std::string());
    }
  }

  std::string Json() const {
    base::ListValue list;
    for (const Open& open : open_) {
      base::DictValue dialog;
      dialog.Set("id", static_cast<double>(open.id));
      dialog.Set("type", TypeName(open.type));
      dialog.Set("message", open.message);
      dialog.Set("defaultPrompt", open.default_prompt);
      dialog.Set("url", open.url);
      dialog.Set("isReload", open.is_reload);
      list.Append(std::move(dialog));
    }
    return base::WriteJson(list).value_or("[]");
  }

  // content::WebContentsObserver: zebkit_view_request_close().
  void BeforeUnloadFired(bool proceed) override {
    if (!close_requested)
      return;
    close_requested = false;
    if (proceed)
      CloseView();
  }
  void BeforeUnloadDialogCancelled() override { close_requested = false; }

  void CloseView() {
    if (zebkit_view* view = ViewOf(web_contents()))
      zebkit_view_close(view);
  }

 private:
  friend class content::WebContentsUserData<DialogState>;
  explicit DialogState(content::WebContents* contents)
      : content::WebContentsObserver(contents),
        content::WebContentsUserData<DialogState>(*contents) {}

  std::list<Open> open_;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(DialogState);

DialogState* StateOf(content::WebContents* contents) {
  if (!contents)
    return nullptr;
  DialogState::CreateForWebContents(contents);
  return DialogState::FromWebContents(contents);
}

DialogState* StateOf(zebkit_view* view) {
  return zebkit::LiveView(view) ? StateOf(view->web_contents()) : nullptr;
}

// A window's manager: the policy's, and the system's own when the
// policy leaves the dialog to it (the system's alert on macOS, nothing on
// Linux and without a window).
class DialogManager : public content::JavaScriptDialogManager {
 public:
  DialogManager() = default;

  void RunJavaScriptDialog(content::WebContents* contents,
                           content::RenderFrameHost* frame,
                           content::JavaScriptDialogType type,
                           const std::u16string& message,
                           const std::u16string& default_prompt,
                           DialogClosedCallback callback,
                           bool* did_suppress_message) override {
    DialogState* state = StateOf(contents);
    DialogState::Decision decision = state->Decide();
    if (decision == DialogState::Decision::kNative) {
      if (HasNoDisplay(contents)) {
        *did_suppress_message = true;
        return;
      }
      native_.RunJavaScriptDialog(contents, frame, type, message,
                                 default_prompt, std::move(callback),
                                 did_suppress_message);
      return;
    }
    *did_suppress_message = false;
    DialogState::Open open;
    open.id = NextDialogId();
    open.type = type == content::JAVASCRIPT_DIALOG_TYPE_CONFIRM
                    ? ZEBKIT_DIALOG_CONFIRM
                : type == content::JAVASCRIPT_DIALOG_TYPE_PROMPT
                    ? ZEBKIT_DIALOG_PROMPT
                    : ZEBKIT_DIALOG_ALERT;
    open.message = base::UTF16ToUTF8(message);
    open.default_prompt = base::UTF16ToUTF8(default_prompt);
    open.url = frame ? frame->GetLastCommittedURL().spec() : std::string();
    Settle(state, decision, std::move(open), std::move(callback));
  }

  void RunBeforeUnloadDialog(content::WebContents* contents,
                             content::RenderFrameHost* frame,
                             bool is_reload,
                             DialogClosedCallback callback) override {
    DialogState* state = StateOf(contents);
    DialogState::Decision decision = state->Decide();
    if (decision == DialogState::Decision::kNative) {
      if (HasNoDisplay(contents)) {
        std::move(callback).Run(true, std::u16string());
        return;
      }
      native_.RunBeforeUnloadDialog(contents, frame, is_reload,
                                   std::move(callback));
      return;
    }
    DialogState::Open open;
    open.id = NextDialogId();
    open.type = ZEBKIT_DIALOG_BEFOREUNLOAD;
    open.url = frame ? frame->GetLastCommittedURL().spec() : std::string();
    open.is_reload = is_reload;
    Settle(state, decision, std::move(open), std::move(callback));
  }

  bool HandleJavaScriptDialog(content::WebContents* contents,
                              bool accept,
                              const std::u16string* prompt_override) override {
    if (DialogState* state = DialogState::FromWebContents(contents);
        state && state->AnsweredElsewhere(accept, prompt_override)) {
      return true;
    }
    return native_.HandleJavaScriptDialog(contents, accept, prompt_override);
  }

  void CancelDialogs(content::WebContents* contents, bool reset_state) override {
    if (DialogState* state = DialogState::FromWebContents(contents))
      state->CancelAll();
    native_.CancelDialogs(contents, reset_state);
  }

 private:
  // A dialog held, or answered now as the policy says; reported either way.
  void Settle(DialogState* state,
              DialogState::Decision decision,
              DialogState::Open open,
              DialogClosedCallback callback) {
    if (decision == DialogState::Decision::kHold) {
      open.callback = std::move(callback);
      state->Add(std::move(open), /*held=*/true);
      return;
    }
    const bool accept = decision == DialogState::Decision::kAccept;
    const std::string input = (accept && open.type == ZEBKIT_DIALOG_PROMPT)
                                  ? open.default_prompt
                                  : std::string();
    const uint64_t id = open.id;
    base::WeakPtr<content::WebContents> contents =
        state->web_contents()->GetWeakPtr();
    state->Add(std::move(open), /*held=*/false);
    std::move(callback).Run(accept, base::UTF8ToUTF16(input));
    if (contents) {
      if (DialogState* still = DialogState::FromWebContents(contents.get()))
        still->ReportClosed(id, accept, input);
    }
  }

  JavaScriptDialogManagerImpl native_;
};

}  // namespace

std::unique_ptr<content::JavaScriptDialogManager> CreateDialogManager(
    Window* window) {
  return std::make_unique<DialogManager>();
}

void SetDialogObserver(DialogObserver* observer) {
  g_dialog_observer = observer;
}

void ReportPrintDialog(content::WebContents* contents,
                       content::RenderFrameHost* frame) {
  DialogState* state = StateOf(contents);
  if (!state)
    return;
  DialogState::Decision decision = state->Decide();
  // Nobody to tell: print() does nothing.
  if (decision == DialogState::Decision::kNative ||
      (decision == DialogState::Decision::kHold && !state->on_dialog)) {
    return;
  }
  DialogState::Open open;
  open.id = NextDialogId();
  open.type = ZEBKIT_DIALOG_PRINT;
  open.url = frame ? frame->GetLastCommittedURL().spec() : std::string();
  const uint64_t id = open.id;
  const bool held = decision == DialogState::Decision::kHold;
  state->Add(std::move(open), held);
  if (!held)
    state->ReportClosed(id, decision == DialogState::Decision::kAccept, "");
}

std::optional<zebkit_dialog_policy> ExplicitDialogPolicy(zebkit_view* view) {
  DialogState* state = StateOf(view);
  return state ? state->policy : std::nullopt;
}

void ClearDialogPolicy(zebkit_view* view) {
  if (DialogState* state = StateOf(view))
    state->policy.reset();
}

}  // namespace zebkit

using zebkit::EngineDialogPolicy;
using zebkit::StateOf;

extern "C" {

void zebkit_view_on_dialog(zebkit_view* view,
                           zebkit_dialog_func func,
                           void* data) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view);
  if (zebkit::DialogState* state = StateOf(view)) {
    state->on_dialog = func;
    state->on_dialog_data = data;
  }
}

void zebkit_view_on_dialog_closed(zebkit_view* view,
                                  zebkit_dialog_closed_func func,
                                  void* data) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view);
  if (zebkit::DialogState* state = StateOf(view)) {
    state->on_closed = func;
    state->on_closed_data = data;
  }
}

void zebkit_view_set_dialog_policy(zebkit_view* view,
                                   zebkit_dialog_policy policy) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view);
  if (zebkit::DialogState* state = StateOf(view)) {
    state->policy = (policy >= ZEBKIT_DIALOG_POLICY_DEFAULT &&
                     policy <= ZEBKIT_DIALOG_POLICY_DISMISS)
                        ? policy
                        : ZEBKIT_DIALOG_POLICY_DEFAULT;
  }
}

zebkit_dialog_policy zebkit_view_get_dialog_policy(zebkit_view* view) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view, EngineDialogPolicy().value_or(ZEBKIT_DIALOG_POLICY_DEFAULT));
  zebkit::DialogState* state = StateOf(view);
  return state ? state->Policy() : EngineDialogPolicy().value_or(ZEBKIT_DIALOG_POLICY_DEFAULT);
}

bool zebkit_view_dialog_respond(zebkit_view* view,
                                uint64_t id,
                                bool accept,
                                const char* prompt_text) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view, false);
  zebkit::DialogState* state = StateOf(view);
  if (!state)
    return false;
  const std::string text = prompt_text ? prompt_text : "";
  return state->Respond(id, accept, prompt_text ? &text : nullptr);
}

char* zebkit_view_dialogs(zebkit_view* view) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view, nullptr);
  zebkit::DialogState* state = StateOf(view);
  return strdup(state ? state->Json().c_str() : "[]");
}

void zebkit_view_request_close(zebkit_view* view) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view);
  content::WebContents* contents = view->web_contents();
  zebkit::DialogState* state = StateOf(contents);
  if (!state)
    return;
  if (!contents->NeedToFireBeforeUnloadOrUnloadEvents()) {
    state->CloseView();
    return;
  }
  state->close_requested = true;
  contents->DispatchBeforeUnload(/*auto_cancel=*/false);
}

}  // extern "C"
