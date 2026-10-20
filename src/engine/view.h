// zebkit_view and the calls a page makes to zebkit.* (view.cc, bridge.cc).

#ifndef ZEBKIT_ENGINE_VIEW_H_
#define ZEBKIT_ENGINE_VIEW_H_

#include <map>
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
#include "zebkit/engine/include/zebkit/zebkit.h"

namespace content {
class RenderFrameHost;
class Shell;
}  // namespace content

namespace url {
class Origin;
}

namespace zebkit {

// A call from a page's script, after the browser has decided which origin
// made it and what that origin may use.
struct PageCall {
  PageCall();
  PageCall(PageCall&&);
  PageCall& operator=(PageCall&&);
  ~PageCall();

  zebkit_view* view = nullptr;
  int context_id = 0;
  int call_id = 0;
  std::string origin;
  unsigned allow = 0;
  base::FilePath files_root;
  std::string module;
  std::string method;
  base::ListValue args;
};

// Answers a call (bridge.cc), now or later, on the UI thread.
void HandlePageCall(PageCall call);

}  // namespace zebkit

// A view is the observer of its page, and a DevTools client of it: for
// zebkit_view_evaluate() (Runtime.evaluate, which waits for a promise and
// reports an exception as Chrome does) and for zebkit.* in the page.
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

  void Evaluate(const char* script, zebkit_result_func func, void* data);
  void Grant(const std::string& origin,
             unsigned allow,
             const std::string& files_root);
  void Bind(const std::string& name, zebkit_bind_func func, void* data);

  // Settles the promise of call @call_id in the page's context @context_id
  // with @value, or rejects it with @value as the message.
  void ReplyToPage(int context_id, int call_id, bool ok, base::Value value);

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
  void* on_load_data_ = nullptr;
  zebkit_title_func on_title_ = nullptr;
  void* on_title_data_ = nullptr;
  zebkit_view_func on_close_ = nullptr;
  void* on_close_data_ = nullptr;

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
  void InstallBridge();
  const GrantEntry* GrantFor(const url::Origin& origin) const;
  content::RenderFrameHost* FrameForContext(int context_id);
  void OnBindingCalled(const base::DictValue& params);
  void Load(zebkit_load_event event, const GURL& url);
  void FailPending(const std::string& why);

  scoped_refptr<content::DevToolsAgentHost> devtools_;
  int next_id_ = 1;
  std::map<int, Reply> pending_;
  bool bridge_installed_ = false;
  // The main-world execution contexts of the page's frames, by id, with
  // the DevTools id of their frame.
  std::map<int, std::string> contexts_;
  std::map<std::string, GrantEntry> grants_;
  std::map<std::string, Binding> bindings_;
  base::WeakPtrFactory<zebkit_view> weak_factory_{this};
};

// A pending zebkit.app call, answered by zebkit_call_resolve() or
// zebkit_call_reject(); it survives its view, and then answers nobody.
struct zebkit_call {
  base::WeakPtr<zebkit_view> view;
  int context_id = 0;
  int call_id = 0;
};

#endif  // ZEBKIT_ENGINE_VIEW_H_
