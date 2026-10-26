// zebkit_view and the calls a page makes to zebkit.* (view.cc, bridge.cc).

#ifndef ZEBKIT_ENGINE_VIEW_H_
#define ZEBKIT_ENGINE_VIEW_H_

#include <map>
#include <optional>
#include <set>
#include <string>

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
#include "zebkit/engine/include/zebkit/zebkit.h"

namespace content {
class RenderFrameHost;
class Shell;
class WebContents;
}  // namespace content

namespace url {
class Origin;
}

namespace zebkit {

// How a call is answered: true with the result, or false with the message.
// Bytes are binary values (base::Value::BlobStorage).
using CallReply = base::OnceCallback<void(bool ok, base::Value value)>;

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
  std::string origin;
  unsigned allow = 0;
  base::FilePath files_root;
  std::string module;
  std::string method;
  base::ListValue args;
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
// (content::Shell::SetNewWindowHandler()).
bool HandleNewWindow(content::WebContents* opener, const GURL& url);
// The window the shell made for it, to the application as a view
// (content::Shell::SetPopupHandler()).
void HandlePopup(content::WebContents* opener,
                 content::Shell* popup,
                 const GURL& url);
// A popup of an off-screen view off the screen too (offscreen.cc).
void MakeOffscreenLike(zebkit_view* opener, zebkit_view* popup);

// The content shell's view delegate, with the drop told to the application.
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

  explicit zebkit_view(content::Shell* shell);
  ~zebkit_view() override;

  static std::set<zebkit_view*>& Views();

  content::Shell* shell();
  void Close();

  // A DevTools command to the page; @reply gets its result or an error.
  void Send(const std::string& method,
            base::DictValue params,
            Reply reply = Reply());

  // Loads @url, once the scripts for new documents that were just asked
  // for (zebkit.*, zebkit_view_add_script()) are in place.
  void LoadUrl(const GURL& url);

  // A script for every new document, before the page's own.
  void AddScript(const std::string& source);

  void Evaluate(const char* script, zebkit_result_func func, void* data);
  void Grant(const std::string& origin,
             unsigned allow,
             const std::string& files_root);
  void Bind(const std::string& name, zebkit_bind_func func, void* data);

  // A call from @frame of the page (call.cc): with the origin the browser
  // has for the frame and what the application granted it.
  void TakeCall(content::RenderFrameHost* frame, zebkit::PageCall call);

  // zebkit.app.<name>(): to the application's bound function.
  void CallApp(zebkit::PageCall call);

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
  void DidFinishLoad(content::RenderFrameHost* frame, const GURL& url) override;
  void DidFailLoad(content::RenderFrameHost* frame,
                   const GURL& url,
                   int error_code) override;
  void TitleWasSet(content::NavigationEntry* entry) override;
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
  // The URL of the window a page is opening, between the shell asking
  // whether to make it and making it (which starts it on about:blank).
  GURL opening_url_;
  zebkit_drop_func on_drop_ = nullptr;
  void* on_drop_data_ = nullptr;
  // Normal views have a native authentication anchor. Off-screen views opt
  // in and have their foreground state explicitly maintained by the host.
  bool authentication_ui_enabled_ = true;
  bool offscreen_view_ = false;
  bool offscreen_focused_ = false;

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
  void SendScript(const std::string& source);
  void InstallBridge();
  const GrantEntry* GrantFor(const url::Origin& origin) const;
  void Load(zebkit_load_event event, const GURL& url);
  void FailPending(const std::string& why);

  scoped_refptr<content::DevToolsAgentHost> devtools_;
  int next_id_ = 1;
  std::map<int, Reply> pending_;
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
};

#endif  // ZEBKIT_ENGINE_VIEW_H_
