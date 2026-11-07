// zebkit_view and the calls a page makes to zebkit.* (view.cc, bridge.cc).

#ifndef ZEBKIT_ENGINE_VIEW_H_
#define ZEBKIT_ENGINE_VIEW_H_

#include <stdint.h>

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/devtools_agent_host_client.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_view_delegate.h"
#include "url/gurl.h"
#include "third_party/blink/public/mojom/window_features/window_features.mojom-forward.h"
#include "ui/base/window_open_disposition.h"
#include "zebkit/engine/include/zebkit/zebkit.h"
#include "zebkit/engine/include/zebkit/hooks.h"

namespace content {
class RenderFrameHost;
class WebContents;
}  // namespace content

namespace url {
class Origin;
}

namespace zebkit {

class Window;

struct NetworkRules;

// How a call is answered: true with the result, or false with the message.
// Bytes are binary values (base::Value::BlobStorage).
using CallReply = base::OnceCallback<void(bool ok, base::Value value)>;
// The same as text: when ok, JSON that IsPlainJson() passed and that has no
// {"$bytes"}; otherwise the message. For a page's calls (call.cc), which
// can then hand the application's answer on without parsing it.
using TextReply = base::OnceCallback<void(bool ok, std::string_view text)>;

// A call to zebkit.*: from a page's script, after the browser has decided
// which origin made it and what that origin may use, or from the
// application's own script (script.cc), which may use everything.
struct PageCall {
  PageCall();
  PageCall(PageCall&&);
  PageCall& operator=(PageCall&&);
  ~PageCall();

  // The page's view, for zebkit.app; null for the application's script.
  zebkit_view* view = nullptr;
  CallReply reply;
  // Set beside reply for a page's call; either answers it.
  TextReply text_reply;
  std::string origin;
  unsigned allow = 0;
  base::FilePath files_root;
  std::string module;
  std::string method;
  base::ListValue args;
  // zebkit.app's arguments as the page sent them, a JSON array that
  // IsPlainJson() passed, in place of args.
  std::optional<std::string> args_json;
};

// Answers a call (bridge.cc), now or later, on the UI thread.
void HandlePageCall(PageCall call);

// Bytes as {"$bytes": base64}, the form the application's JSON has them
// in, and back.
base::Value ToPageValue(base::Value value);
base::Value FromPageValue(base::Value value);

// Bytes in a call: a binary value from the application's script, {$bytes}
// from a page, or a string, taken as its UTF-8.
std::optional<std::string> GetBytes(const base::Value* value);

// zebkit.pdf (pdf_bridge.cc); and the documents a view's pages opened,
// closed with it.
void CallPdf(PageCall call);
void ClosePdfsOf(const zebkit_view* view);

// The view of a page, or null.
zebkit_view* ViewOf(content::WebContents* contents);

// A new window a page asks for: to its view's application when it said so
// (Window::SetNewWindowHandler()).
bool HandleNewWindow(content::WebContents* opener, const GURL& url);
// The window made for it, to the application as a view
// (Window::SetPopupHandler()).
void HandlePopup(content::WebContents* opener,
                 Window* popup,
                 const GURL& url,
                 WindowOpenDisposition disposition,
                 const blink::mojom::WindowFeatures& features,
                 bool user_gesture);
// A popup of an off-screen view off the screen too (offscreen.cc), the size
// window.open's features ask for when they do, else its opener's.
void MakeOffscreenLike(zebkit_view* opener, zebkit_view* popup,
                       const blink::mojom::WindowFeatures& features);
// Whether a window.open() waits, in the opener's script, for its popup's
// DevTools session (automation holds popups as Chromium's DevTools does with
// Target.setAutoAttach, so their first script has the context's scripts).
bool& PopupStartsHeld();
// Set by automation: asked with @opener and true as a window.open() is allowed
// (BrowserClient::CanCreateWindow(), before Chromium decides whether the new
// window waits), and with null and false once the popup has its view.
using PopupHoldFunc = void (*)(content::WebContents* opener, bool hold);
PopupHoldFunc& PopupHoldHook();

// The view delegate: the drop told to the application.
std::unique_ptr<content::WebContentsViewDelegate> WrapViewDelegate(
    content::WebContents* contents,
    std::unique_ptr<content::WebContentsViewDelegate> inner);

}  // namespace zebkit

// A view is the observer of its page, and a DevTools client of it: for
// zebkit_view_evaluate() (Runtime.evaluate, which waits for a promise and
// reports an exception as Chrome does) and for the scripts every new
// document starts with, zebkit.* among them.
struct zebkit_view : public content::WebContentsObserver,
                     public content::DevToolsAgentHostClient {
  using Reply = base::OnceCallback<void(const base::DictValue* result,
                                        const std::string& error)>;

  explicit zebkit_view(zebkit::Window* window);
  ~zebkit_view() override;

  static std::set<zebkit_view*>& Views();

  zebkit::Window* window();
  void Close();

  // A DevTools command to the page; @reply gets its result or an error.
  void Send(const std::string& method,
            base::DictValue params,
            Reply reply = Reply());

  // Loads @url, once the scripts for new documents that were just asked
  // for (zebkit.*, zebkit_view_add_script()) are in place.
  void LoadUrl(const GURL& url);

  // A script for every new document, before the page's own; with
  // @all_frames in other sites' frames too (their own targets), as
  // Playwright's init scripts are in each frame's session (crPage.ts).
  // @run_immediately: in the document there is too (Page.addScriptToEvaluate-
  // OnNewDocument's runImmediately); Playwright's init scripts are not.
  void AddScript(const std::string& source, Reply reply = Reply(), bool all_frames = false,
                 bool run_immediately = true);
  // Other sites' frames attached as targets of their own from now on, each
  // held until what its attachment event's handler sends it has been sent.
  void AttachFrames();
  // A DevTools command to the session @session (a frame's, from its
  // Target.attachedToTarget), at once.
  void SendToSession(const std::string& session, const std::string& method,
                     base::DictValue params, Reply reply = Reply());
  // The same in a world of its own name ("" for the page's), in every frame,
  // those of other sites' processes too, or only in the main frame.
  void AddWorldScript(const std::string& world, const std::string& source,
                      bool main_frame_only);

  void Evaluate(const char* script, zebkit_result_func func, void* data);
  void SetUserAgent(const char* user_agent);
  void SetUserAgentOverride(base::DictValue params, Reply reply = Reply());
  void Grant(const std::string& origin,
             unsigned allow,
             const std::string& files_root);
  void Bind(const std::string& name, zebkit_bind_func func, void* data);

  // A call from @frame of the page (call.cc): with the origin the browser
  // has for the frame and what the application granted it.
  void TakeCall(content::RenderFrameHost* frame, zebkit::PageCall call);

  // zebkit.app.<name>(): to the application's bound function.
  void CallApp(zebkit::PageCall call);

  // A popup is decided for by the view that opened it until it has a handler
  // of its own. The view that decides for this one; nullptr when that opener
  // has closed, which every hook treats as a refusal: a popup must not outlive
  // its opener's policy into having none.
  template <typename Handler>
  zebkit_view* Decider(Handler zebkit_view::*handler) {
    zebkit_view* view = this;
    while (view && !(view->*handler) && view->inherits_policy_)
      view = view->policy_opener_.get();
    return view;
  }
  base::WeakPtr<zebkit_view> policy_opener_;
  bool inherits_policy_ = false;
  bool navigation_fail_closed_ = false;

  base::WeakPtr<zebkit_view> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

  // content::DevToolsAgentHostClient
  void DispatchProtocolMessage(content::DevToolsAgentHost* agent_host,
                               base::span<const uint8_t> message) override;
  void AgentHostClosed(content::DevToolsAgentHost* agent_host) override;

  // content::WebContentsObserver
  void DidStartNavigation(content::NavigationHandle* navigation) override;
  void DidFinishNavigation(content::NavigationHandle* navigation) override;
  void RenderFrameDeleted(content::RenderFrameHost* frame) override;
  void DidFinishLoad(content::RenderFrameHost* frame, const GURL& url) override;
  void DidFailLoad(content::RenderFrameHost* frame,
                   const GURL& url,
                   int error_code) override;
  void TitleWasSet(content::NavigationEntry* entry) override;
  void OnDidAddMessageToConsole(content::RenderFrameHost* frame,
      blink::mojom::ConsoleMessageLevel level, const std::u16string& message,
      int32_t line, const std::u16string& source,
      const std::optional<std::u16string>& stack) override;
  void WebContentsDestroyed() override;

  zebkit_load_func on_load_ = nullptr;
  // Made without a URL: the about:blank it starts on is not reported.
  bool quiet_blank_ = false;
  void* on_load_data_ = nullptr;
  zebkit_title_func on_title_ = nullptr;
  void* on_title_data_ = nullptr;
  zebkit_view_func on_close_ = nullptr;
  void* on_close_data_ = nullptr;
  // The window's title, when the application gave one.
  std::optional<std::string> window_title_;
  zebkit_url_func on_new_window_ = nullptr;
  void* on_new_window_data_ = nullptr;
  zebkit_window_open_func on_window_open_ = nullptr;
  void* on_window_open_data_ = nullptr;
  struct PermissionRule { zebkit_permission_state state; uint64_t revision; };
  std::map<std::pair<std::string, int>, PermissionRule> permission_rules_;
  uint32_t permission_timeout_ms_ = 30000;
  zebkit_permission_func on_permission_ = nullptr;
  void* on_permission_data_ = nullptr;
  zebkit_permission_result_func on_permission_result_ = nullptr;
  void* on_permission_result_data_ = nullptr;
  uint32_t navigation_timeout_ms_ = 30000;
  zebkit_navigation_func on_navigation_ = nullptr;
  void* on_navigation_data_ = nullptr;
  zebkit_navigation_result_func on_navigation_result_ = nullptr;
  void* on_navigation_result_data_ = nullptr;
  base::DictValue settings_;
  std::optional<zebkit_popup_policy> popup_policy_;
  // "downloads" of the runtime settings: true denies when no function decides.
  std::optional<bool> deny_downloads_;
  base::DictValue window_open_info_;
  zebkit_window_open_event_func on_window_open_event_ = nullptr;
  void* on_window_open_event_data_ = nullptr;
  uint64_t window_open_event_generation_ = 0;
  zebkit_console_func on_console_ = nullptr;
  void* on_console_data_ = nullptr;
  uint64_t console_generation_ = 0;
  size_t console_pending_ = 0;
  uint64_t console_dropped_ = 0;
  zebkit_certificate_func on_certificate_ = nullptr;
  void* on_certificate_data_ = nullptr;
  zebkit_certificate_result_func on_certificate_result_ = nullptr;
  void* on_certificate_result_data_ = nullptr;
  uint64_t certificate_result_generation_ = 0;
  zebkit_http_auth_func on_http_auth_ = nullptr;
  void* on_http_auth_data_ = nullptr;
  zebkit_http_auth_result_func on_http_auth_result_ = nullptr;
  void* on_http_auth_result_data_ = nullptr;
  uint64_t http_auth_result_generation_ = 0;
  uint32_t http_auth_timeout_ms_ = 30000;
  uint32_t certificate_timeout_ms_ = 30000;
  // Hooks of page_hooks.cc, download_hooks.cc and client_certificate.cc.
  size_t told_pending_ = 0;
  std::shared_ptr<const zebkit::NetworkRules> network_rules_;
  bool network_ever_ = false;
  zebkit_network_func on_network_ = nullptr;
  void* on_network_data_ = nullptr;
  zebkit_network_event_func on_network_event_ = nullptr;
  void* on_network_event_data_ = nullptr;
  uint64_t network_generation_ = 0;
  uint32_t network_timeout_ms_ = 30000;
  size_t network_events_pending_ = 0;
  uint64_t network_events_dropped_ = 0;
  zebkit_network_body_func on_network_body_ = nullptr;
  void* on_network_body_data_ = nullptr;
  uint64_t network_body_generation_ = 0;
  zebkit_external_protocol_func on_external_protocol_ = nullptr;
  void* on_external_protocol_data_ = nullptr;
  uint64_t external_protocol_generation_ = 0;
  zebkit_download_func on_download_ = nullptr;
  void* on_download_data_ = nullptr;
  zebkit_download_progress_func on_download_progress_ = nullptr;
  void* on_download_progress_data_ = nullptr;
  uint64_t download_generation_ = 0;
  uint32_t download_timeout_ms_ = 30000;
  zebkit_client_certificate_func on_client_certificate_ = nullptr;
  void* on_client_certificate_data_ = nullptr;
  zebkit_client_certificate_result_func on_client_certificate_result_ = nullptr;
  void* on_client_certificate_result_data_ = nullptr;
  uint64_t client_certificate_generation_ = 0;
  uint32_t client_certificate_timeout_ms_ = 30000;
  zebkit_file_chooser_func on_file_chooser_ = nullptr;
  void* on_file_chooser_data_ = nullptr;
  zebkit_file_chooser_result_func on_file_chooser_result_ = nullptr;
  void* on_file_chooser_result_data_ = nullptr;
  uint64_t file_chooser_generation_ = 0;
  uint32_t file_chooser_timeout_ms_ = 30000;
  zebkit_media_access_func on_media_access_ = nullptr;
  void* on_media_access_data_ = nullptr;
  zebkit_media_access_result_func on_media_access_result_ = nullptr;
  void* on_media_access_result_data_ = nullptr;
  uint64_t media_access_generation_ = 0;
  uint32_t media_access_timeout_ms_ = 30000;
  std::optional<std::set<std::string>> reserved_keys_;
  zebkit_reserved_key_func on_reserved_key_ = nullptr;
  void* on_reserved_key_data_ = nullptr;
  uint64_t reserved_key_generation_ = 0;
  zebkit_context_menu_func on_context_menu_ = nullptr;
  void* on_context_menu_data_ = nullptr;
  uint64_t context_menu_generation_ = 0;
  std::optional<zebkit_fullscreen_policy> fullscreen_policy_;
  zebkit_fullscreen_func on_fullscreen_ = nullptr;
  void* on_fullscreen_data_ = nullptr;
  uint64_t fullscreen_generation_ = 0;
  zebkit_popup_blocked_func on_popup_blocked_ = nullptr;
  void* on_popup_blocked_data_ = nullptr;
  zebkit_drop_func on_drop_ = nullptr;
  void* on_drop_data_ = nullptr;
  // Normal views have a native authentication anchor. Off-screen views opt
  // in and have their foreground state explicitly maintained by the host.
  zebkit_authentication_func on_authentication_ = nullptr;
  void* on_authentication_data_ = nullptr;
  uint64_t authentication_handler_generation_ = 0;
  bool authentication_ui_enabled_ = true;
  bool offscreen_view_ = false;
  bool offscreen_focused_ = false;

  // A popup's first script waits until ReleaseStart(): its page target is
  // released (Runtime.runIfWaitingForDebugger) once what was sent meanwhile
  // has been, as Chromium's DevTools holds a new window for its client.
  void HoldStart();
  void ReleaseStart();

  // Automation's use of the view's own DevTools session's events; @session
  // is the session the event came on ("" for the tab's).
  using ProtocolEvents = base::RepeatingCallback<void(
      const std::string& method, const base::DictValue& params, const std::string& session)>;
  void SetProtocolEvents(ProtocolEvents events) {
    protocol_events_ = std::move(events);
  }

 private:
  struct GrantEntry {
    unsigned allow = 0;
    base::FilePath files_root;
  };
  struct Binding {
    zebkit_bind_func func = nullptr;
    void* data = nullptr;
  };

  bool EnsureSession();
  void MaybeRunDeferredLoad();
  void DispatchCommand(base::DictValue message);
  void FlushTargetCommands(const std::string& target);
  void SendTargetCommand(const std::string& target, const std::string& method,
                         base::DictValue params, Reply reply = Reply());
  void InitializeProtocolTarget(const std::string& target);
  void ProtocolInitializationReply(const std::string& target, const std::string& error);
  bool RecordProtocolScript(const std::string& target, size_t index,
                            const base::DictValue* result);
  void RemoveProtocolScript(size_t index, Reply reply);
  void RefreshOtherProtocolTargets(const std::string& current);
  void SendScript(const std::string& source, Reply reply = Reply(),
                  const std::string& world = std::string(), bool run_immediately = true);
  // Other sites' frames as targets of their own, for scripts in all frames.
  void AttachFrameTargets(const std::string& target);
  void InstallBridge();
  const GrantEntry* GrantFor(const url::Origin& origin) const;
  void Load(zebkit_load_event event, const GURL& url);
  void FailPending(const std::string& why);

  scoped_refptr<content::DevToolsAgentHost> devtools_;
  int next_id_ = 1;
  std::map<int, Reply> pending_;
  // A tab session survives primary-frame swaps. Commands are routed only to
  // the page target that was primary when the host issued them.
  std::map<std::string, std::string> protocol_sessions_;
  std::map<int, std::string> protocol_pending_targets_;
  std::vector<std::pair<std::string, base::DictValue>> protocol_queued_;
  std::set<std::string> protocol_ready_targets_;
  std::map<std::string, size_t> protocol_scripts_applied_;
  std::map<std::string, size_t> protocol_initializing_;
  std::vector<std::string> protocol_script_sources_;
  std::map<size_t, std::string> protocol_script_worlds_;
  std::set<size_t> protocol_all_frame_scripts_;
  std::set<std::string> protocol_frame_targets_;
  std::set<std::string> protocol_worker_targets_;
  bool protocol_attach_frames_ = false;
  std::set<size_t> protocol_removed_scripts_;
  std::map<std::string, std::map<size_t, std::string>> protocol_script_ids_;
  std::optional<base::DictValue> protocol_user_agent_;
  size_t protocol_user_agent_revision_ = 0;
  size_t protocol_user_agent_submission_ = 0;
  size_t protocol_user_agent_committed_ = 0;
  std::map<std::string, size_t> protocol_user_agent_applied_;
  size_t protocol_setups_pending_ = 0;
  int protocol_auto_attach_id_ = 0;
  bool protocol_attach_failed_ = false;
  bool protocol_closing_ = false;
  bool protocol_hold_start_ = false;
  bool protocol_flush_before_run_ = false;
  // The events of the view's own DevTools session, for automation's use
  // (Network's request types and the network's raw headers).
  ProtocolEvents protocol_events_;
  bool bridge_installed_ = false;
  // Until the browser has the scripts for new documents (zebkit.*, and the
  // application's), a load waits: a document committed first would be
  // without them.
  int scripts_pending_ = 0;
  std::optional<GURL> deferred_load_;
  std::map<std::string, GrantEntry> grants_;
  std::map<std::string, Binding> bindings_;
  base::WeakPtrFactory<zebkit_view> weak_factory_{this};
};

// A pending zebkit.app call, answered by zebkit_call_resolve() or
// zebkit_call_reject(); it survives its page, and then answers nobody.
struct zebkit_call {
  zebkit::CallReply reply;
  zebkit::TextReply text_reply;
};

#endif  // ZEBKIT_ENGINE_VIEW_H_
