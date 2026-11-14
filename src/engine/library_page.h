// JavaScript libraries as normal libraries, the backend "page"
// (docs/JS_LIBRARIES.md, "Backend page"): a library run in a page of its own,
// in a renderer of its own, for what needs a DOM, a canvas or fonts.
//
// Each instance is a hidden window ({"show": false}, as zebkit_view_new_with_
// options() makes it) of a profile of its own in memory, whose page is
// zebkit-library://<random id>/: the engine's document with the runner
// (library_page_runner.h), which imports the library's entry. The runner pulls
// work over the frame's PageCallHost channel (call.cc): nothing is evaluated
// in the page, so it never has a user gesture. Its requests other than its own
// files and the granted origins fail in the browser.
//
// Everything here is the engine's thread's but LibraryPage::Call() and
// Cancel(), which any thread may call: the queue is under a lock.

#ifndef ZEBKIT_ENGINE_LIBRARY_PAGE_H_
#define ZEBKIT_ENGINE_LIBRARY_PAGE_H_

#include <stddef.h>
#include <stdint.h>

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/memory/ref_counted.h"
#include "base/memory/raw_ptr.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "content/public/browser/content_browser_client.h"
#include "content/public/browser/frame_tree_node_id.h"
#include "content/public/common/content_client.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "services/network/public/mojom/url_loader_factory.mojom-forward.h"
#include "zebkit/engine/include/zebkit/library.h"
#include "zebkit/engine/page_call.mojom.h"

namespace base {
class CommandLine;
}
namespace content {
class BrowserContext;
class NavigationThrottleRegistry;
class RenderFrame;
class RenderFrameHost;
}  // namespace content
namespace network {
class URLLoaderFactoryBuilder;
}

namespace zebkit {

// The scheme of libraries' pages: standard and secure, the engine's own.
inline constexpr char kLibraryScheme[] = "zebkit-library";

struct LibraryCall;
class LibraryPageHost;

class LibraryPage : public base::RefCountedThreadSafe<LibraryPage> {
 public:
  // Makes the page for @handle (its options, root, entry and ready set);
  // null with @error when it cannot. The engine's thread.
  static scoped_refptr<LibraryPage> Start(zebkit_library* handle, std::string* error);

  // As zebkit_library_call() and _cancel(): any thread.
  uint64_t Call(const char* function, const char* args_json, const zebkit_part* parts, size_t part_count,
                int64_t timeout_ms, zebkit_library_func func, void* data);
  void Cancel(uint64_t id);
  // Untold calls GONE, the page and its profile torn down. True the first
  // time: the application's reference is then let go.
  bool Close();

  // ---- The engine's thread: the host (library_page.cc) ----
  enum class State { kLoading, kReady, kFailed, kEnded, kClosed };
  // The runner's messages.
  void Loaded(zebkit_library_code code, std::string json);
  void Next(mojom::PageCallHost::CallCallback reply);
  void Done(uint64_t id, zebkit_library_code code, std::string json, std::vector<std::string> parts);
  // The page cannot go on (its renderer gone, killed, or the window closed
  // under it): pending calls fail, @code for the one running.
  void End(zebkit_library_code code, const std::string& why);
  size_t max_bytes() const;

 private:
  friend class base::RefCountedThreadSafe<LibraryPage>;
  explicit LibraryPage(zebkit_library* handle);
  ~LibraryPage();

  using Posts = std::vector<base::OnceClosure>;
  void TellLocked(LibraryCall* call, zebkit_library_code code, std::string json, std::vector<std::string> parts,
                  Posts* posts) EXCLUSIVE_LOCKS_REQUIRED(lock_);
  void TellReadyLocked(zebkit_library_code code, std::string json, Posts* posts) EXCLUSIVE_LOCKS_REQUIRED(lock_);
  void FailAllLocked(zebkit_library_code code, const std::string& json, Posts* posts) EXCLUSIVE_LOCKS_REQUIRED(lock_);
  static void Post(Posts posts);
  // The next queued call to the runner, when it waits for one.
  void Pump();
  void PumpSoon() EXCLUSIVE_LOCKS_REQUIRED(lock_);
  // A call's deadline (0: the load's), and the grace after it in which a
  // page that only waits asks for its next call.
  void Deadline(uint64_t id);
  void DeadlineGrace(uint64_t id);
  // The host goes, as a task.
  void TearDownSoon();

  // The application's handle: kept alive by each callback's reference.
  const raw_ptr<zebkit_library, DisableDanglingPtrDetection> handle_;

  base::Lock lock_;
  State state_ GUARDED_BY(lock_) = State::kLoading;
  bool ready_told_ GUARDED_BY(lock_) = false;
  std::deque<uint64_t> queue_ GUARDED_BY(lock_);
  std::map<uint64_t, std::unique_ptr<LibraryCall>> calls_ GUARDED_BY(lock_);
  uint64_t current_ GUARDED_BY(lock_) = 0;
  uint64_t next_id_ GUARDED_BY(lock_) = 1;
  bool pump_posted_ GUARDED_BY(lock_) = false;

  // ---- The engine's thread ----
  std::unique_ptr<LibraryPageHost> host_;
  // The runner's outstanding request for its next call.
  mojom::PageCallHost::CallCallback next_;
  // Why the renderer was stopped, told the running call when it goes.
  zebkit_library_code killed_for_ = ZEBKIT_LIBRARY_OK;

 public:
  // Its renderer is stopped: told @code (TIMEOUT, OUT_OF_MEMORY) for the call
  // running, and the library ends.
  void Kill(zebkit_library_code code);
};

// ---- What the engine asks (engine.cc, client_defaults.cc, call.cc) ----

// The scheme, in every process (ContentClient::AddAdditionalSchemes()), and
// what Blink is told of it in a renderer.
void AddLibraryPageScheme(content::ContentClient::Schemes* schemes);
void RegisterLibraryPageSchemeInRenderer();
// Whether @frame is of a library's page, or @context a library's profile.
bool IsLibraryPageFrame(content::RenderFrameHost* frame);
bool IsLibraryPageContext(content::BrowserContext* context);
// The loaders of a page's files: its navigation, and its subresources.
mojo::PendingRemote<network::mojom::URLLoaderFactory> CreateLibraryPageNavigationFactory(
    content::FrameTreeNodeId frame_tree_node_id);
void AddLibraryPageSubresourceFactory(int render_process_id, int render_frame_id,
                                      content::ContentBrowserClient::NonNetworkURLLoaderFactoryMap* factories);
// In front of a library profile's network: only the granted origins.
void FenceLibraryPageNetwork(content::BrowserContext* context, network::URLLoaderFactoryBuilder& builder);
// A library's page navigates once, to itself.
void AddLibraryPageThrottle(content::NavigationThrottleRegistry& registry);
// A library's renderer: V8's heap limit (heapLimitMB).
void AppendLibraryPageSwitches(base::CommandLine* command_line, int child_process_id);
// The runner's calls over PageCallHost ("library".<method>): true when taken,
// @reply then the page's to answer.
bool TakeLibraryPageCall(content::RenderFrameHost* frame, const std::string& module, const std::string& method,
                         mojo_base::BigBuffer& json, mojo_base::BigBuffer& parts,
                         mojom::PageCallHost::CallCallback& reply);
// The renderer's side, in each frame (library_page_renderer.cc): a library
// page's heap limit told as it is reached.
void InstallLibraryPageAgent(content::RenderFrame* render_frame);

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_LIBRARY_PAGE_H_
