// Find in page (<zebkit/page.h>), and Zebkit.find* over CDP: Chrome's own,
// its FindTabHelper (components/find_in_page) over WebContents::Find(), which
// content's FindRequestManager runs in every frame of the page, other sites'
// frames too, and Blink's TextFinder over the flat tree, shadow trees
// included. Blink highlights the matches (text-match markers, the active one
// distinctly) and scrolls to the active one. The frames' replies reach the
// helper as a WebContentsObserver, and its observers, this file among them.

#ifdef UNSAFE_BUFFERS_BUILD
// The C API takes and returns C strings.
#pragma allow_unsafe_buffers
#endif

#include <stdlib.h>
#include <string.h>

#include <set>
#include <string>
#include <string_view>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "components/find_in_page/find_result_observer.h"
#include "components/find_in_page/find_tab_helper.h"
#include "components/find_in_page/find_types.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/devtools_agent_host_client_channel.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "ui/gfx/geometry/rect.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/media_capture.h"
#include "zebkit/engine/page_internal.h"
#include "zebkit/engine/view.h"

namespace zebkit {

namespace {

// What a page's searches are told to: the application's function and the
// CDP sessions that searched it.
class FindState : public content::WebContentsObserver,
                  public content::WebContentsUserData<FindState>,
                  public find_in_page::FindResultObserver {
 public:
  ~FindState() override {
    if (helper_)
      helper_->RemoveObserver(this);
  }

  static FindState* Of(content::WebContents* contents) {
    if (!contents)
      return nullptr;
    find_in_page::FindTabHelper::CreateForWebContents(contents);
    CreateForWebContents(contents);
    return FromWebContents(contents);
  }

  find_in_page::FindTabHelper* helper() { return helper_; }

  int Find(const std::u16string& text, unsigned flags) {
    if (text.empty()) {
      Stop(find_in_page::SelectionAction::kClear);
      return 0;
    }
    helper_->StartFinding(text, !(flags & ZEBKIT_FIND_BACKWARD),
                          flags & ZEBKIT_FIND_MATCH_CASE,
                          !(flags & ZEBKIT_FIND_HIGHLIGHT_ONLY));
    // The same request again (highlighting what is highlighted) sends none.
    return helper_->current_find_request_id() + 1;
  }

  void Stop(find_in_page::SelectionAction action) {
    const bool was_on = helper_->is_find_session_active() ||
                        !helper_->find_text().empty();
    const int request = helper_->find_result().request_id();
    helper_->StopFinding(action);
    if (was_on)
      Tell(Result(/*stopped=*/true, request));
  }

  base::DictValue Result(bool stopped, int request) const {
    const find_in_page::FindNotificationDetails& found =
        helper_->find_result();
    base::DictValue result;
    result.Set("requestId", (stopped ? request : found.request_id()) + 1);
    result.Set("text", stopped ? std::string()
                               : base::UTF16ToUTF8(helper_->find_text()));
    const int matches = stopped ? 0 : found.number_of_matches();
    const int active = stopped ? 0 : found.active_match_ordinal();
    result.Set("matches", matches < 0 ? 0 : matches);
    result.Set("activeMatch", active < 0 ? 0 : active);
    result.Set("final", stopped || found.final_update());
    const gfx::Rect rect = found.selection_rect();
    if (stopped || active <= 0 || rect.IsEmpty()) {
      result.Set("rect", base::Value());
    } else {
      base::DictValue box;
      box.Set("x", rect.x());
      box.Set("y", rect.y());
      box.Set("width", rect.width());
      box.Set("height", rect.height());
      result.Set("rect", std::move(box));
    }
    const bool on = helper_->is_find_session_active();
    result.Set("stopped", stopped || !on);
    return result;
  }

  std::string ResultJson() const {
    const bool on = helper_->is_find_session_active();
    return base::WriteJson(Result(!on, helper_->find_result().request_id()))
        .value_or("{}");
  }

  void SetFunction(zebkit_find_func func, void* data) {
    func_ = func;
    data_ = data;
  }
  void AddChannel(content::DevToolsAgentHostClientChannel* channel) {
    channels_.insert(channel);
  }
  void RemoveChannel(content::DevToolsAgentHostClientChannel* channel) {
    channels_.erase(channel);
  }

  // find_in_page::FindResultObserver
  void OnFindResultAvailable(content::WebContents*) override {
    Tell(Result(/*stopped=*/false, 0));
  }
  void OnFindTabHelperDestroyed(find_in_page::FindTabHelper* helper) override {
    if (helper == helper_) {
      helper_->RemoveObserver(this);
      helper_ = nullptr;
    }
  }

  // content::WebContentsObserver: as Chrome's find bar, a search ends
  // with its document.
  void PrimaryPageChanged(content::Page&) override {
    if (helper_ && helper_->is_find_session_active())
      Stop(find_in_page::SelectionAction::kClear);
  }

 private:
  friend class content::WebContentsUserData<FindState>;
  explicit FindState(content::WebContents* contents)
      : content::WebContentsObserver(contents),
        content::WebContentsUserData<FindState>(*contents),
        helper_(find_in_page::FindTabHelper::FromWebContents(contents)) {
    helper_->AddObserver(this);
  }

  void Tell(base::DictValue result) {
    const std::string json = base::WriteJson(result).value_or("{}");
    for (auto it = channels_.begin(); it != channels_.end();) {
      base::DictValue message;
      message.Set("method", "Zebkit.findResult");
      message.Set("params", result.Clone());
      // A session that has gone is forgotten.
      if (SendToDevToolsClient(*it, std::move(message)))
        ++it;
      else
        it = channels_.erase(it);
    }
    zebkit_view* view = ViewOf(web_contents());
    if (view && func_)
      func_(view, json.c_str(), data_);
  }

  raw_ptr<find_in_page::FindTabHelper> helper_;
  zebkit_find_func func_ = nullptr;
  void* data_ = nullptr;
  std::set<raw_ptr<content::DevToolsAgentHostClientChannel, DanglingUntriaged>>
      channels_;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(FindState);

FindState* StateOf(zebkit_view* view) {
  return LiveView(view) ? FindState::Of(view->web_contents()) : nullptr;
}

find_in_page::SelectionAction ActionOf(int action) {
  switch (action) {
    case ZEBKIT_FIND_STOP_KEEP:
      return find_in_page::SelectionAction::kKeep;
    case ZEBKIT_FIND_STOP_ACTIVATE:
      return find_in_page::SelectionAction::kActivate;
    default:
      return find_in_page::SelectionAction::kClear;
  }
}

void SendCdp(content::DevToolsAgentHostClientChannel* channel,
             int id,
             base::DictValue result) {
  base::DictValue message;
  message.Set("id", id);
  message.Set("result", std::move(result));
  SendToDevToolsClient(channel, std::move(message));
}

void SendCdpError(content::DevToolsAgentHostClientChannel* channel,
                  int id,
                  int code,
                  const std::string& text) {
  base::DictValue error;
  error.Set("code", code);
  error.Set("message", text);
  base::DictValue message;
  message.Set("id", id);
  message.Set("error", std::move(error));
  SendToDevToolsClient(channel, std::move(message));
}

}  // namespace

bool IsFindCommand(std::string_view method) {
  return method == "Zebkit.find" || method == "Zebkit.stopFinding" ||
         method == "Zebkit.getFindResult";
}

void HandleFindCommand(content::DevToolsAgentHostClientChannel* channel,
                       int id,
                       std::string_view method,
                       const std::string& params_json) {
  std::optional<base::Value> parsed = base::JSONReader::Read(params_json, 0);
  const base::DictValue empty;
  const base::DictValue& params =
      parsed && parsed->is_dict() ? parsed->GetDict() : empty;
  content::DevToolsAgentHost* host = channel->GetAgentHost();
  content::WebContents* contents = host ? host->GetWebContents() : nullptr;
  if (!contents || host->GetType() != content::DevToolsAgentHost::kTypePage) {
    SendCdpError(channel, id, -32000, "Command can only be executed on a page");
    return;
  }
  FindState* state = FindState::Of(contents);
  state->AddChannel(channel);
  if (method == "Zebkit.find") {
    const std::string* text = params.FindString("text");
    if (!text || text->empty() || !base::IsStringUTF8(*text)) {
      SendCdpError(channel, id, -32602, "text is needed");
      return;
    }
    unsigned flags = 0;
    if (params.FindBool("matchCase").value_or(false))
      flags |= ZEBKIT_FIND_MATCH_CASE;
    if (params.FindBool("backward").value_or(false))
      flags |= ZEBKIT_FIND_BACKWARD;
    if (params.FindBool("highlightOnly").value_or(false))
      flags |= ZEBKIT_FIND_HIGHLIGHT_ONLY;
    base::DictValue result;
    result.Set("requestId", state->Find(base::UTF8ToUTF16(*text), flags));
    SendCdp(channel, id, std::move(result));
    return;
  }
  if (method == "Zebkit.stopFinding") {
    const std::string* action = params.FindString("action");
    const std::string name = action ? *action : "clear";
    if (name != "clear" && name != "keep" && name != "activate") {
      SendCdpError(channel, id, -32602,
                   "action is clear, keep or activate");
      return;
    }
    state->Stop(ActionOf(name == "keep"       ? ZEBKIT_FIND_STOP_KEEP
                         : name == "activate" ? ZEBKIT_FIND_STOP_ACTIVATE
                                              : ZEBKIT_FIND_STOP_CLEAR));
    SendCdp(channel, id, base::DictValue());
    return;
  }
  // Zebkit.getFindResult
  base::DictValue result;
  result.Set("result", base::JSONReader::Read(state->ResultJson(), 0)
                           .value_or(base::Value(base::DictValue())));
  SendCdp(channel, id, std::move(result));
}

void OnFindClientDetached(content::DevToolsAgentHostClientChannel* channel) {
  content::DevToolsAgentHost* host = channel->GetAgentHost();
  content::WebContents* contents = host ? host->GetWebContents() : nullptr;
  if (FindState* state = contents ? FindState::FromWebContents(contents)
                                  : nullptr) {
    state->RemoveChannel(channel);
  }
}

}  // namespace zebkit

extern "C" {

int zebkit_view_find(zebkit_view* view, const char* text, unsigned flags) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view, 0);
  zebkit::FindState* state = zebkit::StateOf(view);
  if (!state || !text || !base::IsStringUTF8(text))
    return 0;
  return state->Find(base::UTF8ToUTF16(text), flags);
}

void zebkit_view_stop_finding(zebkit_view* view,
                              zebkit_find_stop_action action) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view);
  if (zebkit::FindState* state = zebkit::StateOf(view))
    state->Stop(zebkit::ActionOf(action));
}

void zebkit_view_on_find(zebkit_view* view, zebkit_find_func func, void* data) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view);
  if (zebkit::FindState* state = zebkit::StateOf(view))
    state->SetFunction(func, data);
}

char* zebkit_view_find_result(zebkit_view* view) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view, nullptr);
  zebkit::FindState* state = zebkit::StateOf(view);
  return state ? strdup(state->ResultJson().c_str()) : nullptr;
}

}  // extern "C"
