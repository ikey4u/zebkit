// JavaScript libraries as normal libraries, the backend "page"
// (library_page.h; docs/JS_LIBRARIES.md, "Backend page").
//
// A library's instance is a LibraryPage, the queue of its calls, and while it
// runs a LibraryPageHost: a profile of its own in memory (none of the
// application's cookies or storage), and in it a hidden window whose page is
// zebkit-library://<random id>/. The page's renderer is told it is visible
// (WebContents::IncrementCapturerCount(stay_hidden = false)), so that its
// timers and requestAnimationFrame run as a shown page's do.
//
// The runner in the page (library_page_runner.h) asks for work over the
// frame's PageCallHost channel (call.cc hands "library" calls of a library's
// page here): one request for the next call is out whenever the page is free.
// At a call's deadline a page with that request out was waiting: the call is
// TIMEOUT and the next one goes. A page without it is running script: after a
// short grace its renderer is stopped, the call is TIMEOUT and the library
// ends, as an isolate's TerminateExecution() ends it.
//
// What the page may load: its own files (the library's folder, links resolved
// as the isolate resolves imports), and the origins granted at load. A URL
// loader factory in front of the profile's network refuses the rest, whatever
// asks (documents, images, CSS, fonts, workers, fetch, navigations);
// WebSockets, network hints and new windows are refused in engine.cc and
// client_defaults.cc; the document's CSP (with a sandbox that allows no
// downloads, popups, modals or forms) says the same again in the renderer.

#ifdef UNSAFE_BUFFERS_BUILD
// The C API takes C arrays; parts are cut out of the channel's buffers.
#pragma allow_unsafe_buffers
#endif

#include "zebkit/engine/library_page.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <optional>
#include <set>
#include <utility>

#include "base/base64.h"
#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/process/kill.h"
#include "base/rand_util.h"
#include "base/strings/escape.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/supports_user_data.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/navigation_throttle.h"
#include "content/public/browser/navigation_throttle_registry.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/common/result_codes.h"
#include "crypto/sha2.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/base/mime_util.h"
#include "net/base/net_errors.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/self_deleting_url_loader_factory.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/cpp/url_loader_factory_builder.h"
#include "services/network/public/mojom/early_hints.mojom.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "third_party/blink/public/common/peerconnection/webrtc_ip_handling_policy.h"
#include "third_party/blink/public/common/renderer_preferences/renderer_preferences.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/web/web_security_policy.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "url/url_constants.h"
#include "zebkit/engine/call.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/library.h"
#include "zebkit/engine/library_page_runner.h"
#include "zebkit/engine/path_utf8.h"
#include "zebkit/engine/profile.h"
#include "zebkit/engine/scheme.h"
#include "zebkit/engine/window.h"

namespace zebkit {

namespace {

constexpr int kDefaultWidth = 800;
constexpr int kDefaultHeight = 600;
// How long after a call's deadline a page that is not running script has to
// ask for its next call before it is taken to be running it.
constexpr base::TimeDelta kGrace = base::Milliseconds(500);

const char* CodeName(zebkit_library_code code) {
  switch (code) {
    case ZEBKIT_LIBRARY_TIMEOUT: return "TimeoutError";
    case ZEBKIT_LIBRARY_CANCELLED: return "AbortError";
    default: return "Error";
  }
}

std::string CodeJson(zebkit_library_code code, const std::string& message) {
  return LibraryErrorJson(message, CodeName(code));
}

// What a library's page is: fixed at its start, read by its loaders on any
// thread.
struct PageFiles : public base::RefCountedThreadSafe<PageFiles> {
  std::string id;
  url::Origin origin;
  base::FilePath root, entry;
  bool single_file = false;
  std::string html, csp;
  std::vector<url::Origin> network;
  size_t heap_limit_mb = 128;
  std::string name;  // for messages: the library's path

 private:
  friend class base::RefCountedThreadSafe<PageFiles>;
  ~PageFiles() = default;
};

// A library's profile knows it is one: its fence, its renderer's switches
// and its loaders ask, whatever frame or process asks them.
constexpr char kProfileKey[] = "zebkit-library-page";
struct ProfileData : public base::SupportsUserData::Data {
  scoped_refptr<const PageFiles> files;
};

const PageFiles* FilesOf(content::BrowserContext* context) {
  auto* data = context ? static_cast<ProfileData*>(context->GetUserData(kProfileKey)) : nullptr;
  return data ? data->files.get() : nullptr;
}

std::string RandomId() {
  uint8_t bytes[16];
  base::RandBytes(bytes);
  return base::HexEncodeLower(bytes);
}

void Deliver(scoped_refptr<zebkit_library> library, zebkit_library_func func, void* data, zebkit_library_code code,
             std::string json, std::vector<std::string> parts) {
  std::vector<zebkit_part> list;
  list.reserve(parts.size());
  for (const std::string& part : parts)
    list.push_back({reinterpret_cast<const uint8_t*>(part.data()), part.size()});
  func(library.get(), code, json.c_str(), list.empty() ? nullptr : list.data(), list.size(), data);
}

mojo_base::BigBuffer Buffer(std::string_view bytes) {
  return mojo_base::BigBuffer(base::as_byte_span(bytes));
}

// ---- The page's own files ----

// "Name: value" lines of a file's answer: its type by its name, never
// sniffed, never cached.
std::string FileHeaders(const base::FilePath& path) {
  std::string mime;
  if (!net::GetMimeTypeFromFile(path, &mime))
    mime = "application/octet-stream";
  return "Content-Type: " + mime + "\r\nX-Content-Type-Options: nosniff\r\nCache-Control: no-store";
}

void Refuse(mojo::PendingRemote<network::mojom::URLLoaderClient> client, int error) {
  mojo::Remote<network::mojom::URLLoaderClient>(std::move(client))
      ->OnComplete(network::URLLoaderCompletionStatus(error));
}

class FilesFactory : public network::SelfDeletingURLLoaderFactory {
 public:
  FilesFactory(scoped_refptr<const PageFiles> files,
               mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver, base::SelfDeletingPassKey key)
      : network::SelfDeletingURLLoaderFactory(std::move(receiver), key), files_(std::move(files)) {}

  void CreateLoaderAndStart(mojo::PendingReceiver<network::mojom::URLLoader> loader, int32_t request_id,
                            uint32_t options, const network::ResourceRequest& request,
                            mojo::PendingRemote<network::mojom::URLLoaderClient> client,
                            const net::MutableNetworkTrafficAnnotationTag& annotation) override {
    const GURL& url = request.url;
    if (!files_ || url::Origin::Create(url) != files_->origin) {
      Refuse(std::move(client), net::ERR_ACCESS_DENIED);
      return;
    }
    if (request.method != "GET" && request.method != "HEAD") {
      RespondToLoader(std::move(loader), std::move(client), 405, "Content-Type: text/plain", "not allowed");
      return;
    }
    const std::string path(url.path());
    if (path == "/") {
      RespondToLoader(std::move(loader), std::move(client), 200,
                      "Content-Type: text/html; charset=utf-8\r\nX-Content-Type-Options: nosniff\r\n"
                      "Cache-Control: no-store\r\nContent-Security-Policy: " + files_->csp,
                      files_->html);
      return;
    }
    // Each segment of the path, unescaped, as a name in the folder: no
    // ".", "..", separators or NULs that unescaping made.
    std::vector<std::string> names;
    for (const std::string& piece :
         base::SplitString(path.substr(1), "/", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL)) {
      std::string name;
      if (piece.empty() || !base::UnescapeBinaryURLComponentSafe(piece, /*fail_on_path_separators=*/true, &name) ||
          name.empty() || name == "." || name == ".." || name.find('\0') != std::string::npos) {
        RespondToLoader(std::move(loader), std::move(client), 404, "Content-Type: text/plain", "not found");
        return;
      }
      names.push_back(std::move(name));
    }
    base::ThreadPool::PostTask(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
        base::BindOnce(&FilesFactory::Read, files_, std::move(names), std::move(loader), std::move(client)));
  }

 private:
  ~FilesFactory() override = default;

  static void Read(scoped_refptr<const PageFiles> files, std::vector<std::string> names,
                   mojo::PendingReceiver<network::mojom::URLLoader> loader,
                   mojo::PendingRemote<network::mojom::URLLoaderClient> client) {
    base::FilePath path = files->root;
    for (const std::string& name : names)
      path = path.Append(PathFromUTF8(name));
    // Links resolved, then inside the folder: the import rule of the
    // isolate's (library.cc). A one-file library has that file alone.
    const base::FilePath real = LibraryRealPath(path);
    std::string body;
    if (real.empty() || (files->single_file ? real != files->entry : !files->root.IsParent(real)) ||
        base::DirectoryExists(real) || !base::ReadFileToString(real, &body)) {
      RespondToLoader(std::move(loader), std::move(client), 404, "Content-Type: text/plain", "not found");
      return;
    }
    RespondToLoader(std::move(loader), std::move(client), 200, FileHeaders(real), std::move(body));
  }

  const scoped_refptr<const PageFiles> files_;
};

mojo::PendingRemote<network::mojom::URLLoaderFactory> NewFilesFactory(scoped_refptr<const PageFiles> files) {
  mojo::PendingRemote<network::mojom::URLLoaderFactory> remote;
  base::MakeSelfDeleting<FilesFactory>(std::move(files), remote.InitWithNewPipeAndPassReceiver());
  return remote;
}

// ---- The fence in front of the profile's network ----

// What the fence lets through: the page's own files (their loader is behind
// it too), what never leaves the process (data:, blob:, about:), and the
// granted origins.
bool Granted(const PageFiles& files, const GURL& url) {
  if (url.SchemeIs(url::kDataScheme) || url.SchemeIs(url::kBlobScheme) || url.SchemeIs(url::kAboutScheme))
    return true;
  const url::Origin origin = url::Origin::Create(url);
  if (origin == files.origin)
    return true;
  return url.SchemeIsHTTPOrHTTPS() && std::ranges::find(files.network, origin) != files.network.end();
}

// One request to a granted origin; a redirect elsewhere ends it.
class FencedLoader : public network::mojom::URLLoader, public network::mojom::URLLoaderClient {
 public:
  FencedLoader(scoped_refptr<const PageFiles> files, mojo::PendingReceiver<network::mojom::URLLoader> loader,
               mojo::PendingRemote<network::mojom::URLLoaderClient> client)
      : files_(std::move(files)), receiver_(this, std::move(loader)), client_(std::move(client)) {
    receiver_.set_disconnect_handler(base::BindOnce(&FencedLoader::Gone, base::Unretained(this)));
    client_.set_disconnect_handler(base::BindOnce(&FencedLoader::Gone, base::Unretained(this)));
  }

  void Start(network::mojom::URLLoaderFactory* target, int32_t request_id, uint32_t options,
             const network::ResourceRequest& request, const net::MutableNetworkTrafficAnnotationTag& annotation) {
    target->CreateLoaderAndStart(target_.BindNewPipeAndPassReceiver(), request_id, options, request,
                                 client_receiver_.BindNewPipeAndPassRemote(), annotation);
    client_receiver_.set_disconnect_handler(base::BindOnce(&FencedLoader::Gone, base::Unretained(this)));
  }

  // network::mojom::URLLoader
  void FollowRedirect(network::HttpRequestHeadersUpdateParams params, const std::optional<GURL>& new_url) override {
    if (new_url && !Granted(*files_, *new_url)) {
      Fail();
      return;
    }
    target_->FollowRedirect(std::move(params), new_url);
  }
  void SetPriority(net::RequestPriority priority, int32_t value) override { target_->SetPriority(priority, value); }

  // network::mojom::URLLoaderClient
  void OnReceiveEarlyHints(network::mojom::EarlyHintsPtr hints) override {
    client_->OnReceiveEarlyHints(std::move(hints));
  }
  void OnReceiveResponse(network::mojom::URLResponseHeadPtr head, mojo::ScopedDataPipeConsumerHandle body,
                         std::optional<mojo_base::BigBuffer> metadata) override {
    client_->OnReceiveResponse(std::move(head), std::move(body), std::move(metadata));
  }
  void OnReceiveRedirect(const net::RedirectInfo& redirect, network::mojom::URLResponseHeadPtr head) override {
    if (!Granted(*files_, redirect.new_url)) {
      Fail();
      return;
    }
    client_->OnReceiveRedirect(redirect, std::move(head));
  }
  void OnUploadProgress(int64_t position, int64_t total, OnUploadProgressCallback callback) override {
    client_->OnUploadProgress(position, total, std::move(callback));
  }
  void OnTransferSizeUpdated(int32_t diff) override { client_->OnTransferSizeUpdated(diff); }
  void OnComplete(const network::URLLoaderCompletionStatus& status) override {
    client_->OnComplete(status);
    Gone();
  }

 private:
  ~FencedLoader() override = default;
  void Fail() {
    client_->OnComplete(network::URLLoaderCompletionStatus(net::ERR_BLOCKED_BY_CLIENT));
    Gone();
  }
  void Gone() { delete this; }

  const scoped_refptr<const PageFiles> files_;
  mojo::Receiver<network::mojom::URLLoader> receiver_;
  mojo::Remote<network::mojom::URLLoaderClient> client_;
  mojo::Remote<network::mojom::URLLoader> target_;
  mojo::Receiver<network::mojom::URLLoaderClient> client_receiver_{this};
};

class Fence : public network::mojom::URLLoaderFactory {
 public:
  Fence(scoped_refptr<const PageFiles> files, mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
        mojo::PendingRemote<network::mojom::URLLoaderFactory> target)
      : files_(std::move(files)), target_(std::move(target)) {
    receivers_.Add(this, std::move(receiver));
    receivers_.set_disconnect_handler(base::BindRepeating(&Fence::Disconnected, base::Unretained(this)));
    target_.set_disconnect_handler(base::BindOnce(&Fence::Gone, base::Unretained(this)));
  }

  void CreateLoaderAndStart(mojo::PendingReceiver<network::mojom::URLLoader> loader, int32_t request_id,
                            uint32_t options, const network::ResourceRequest& request,
                            mojo::PendingRemote<network::mojom::URLLoaderClient> client,
                            const net::MutableNetworkTrafficAnnotationTag& annotation) override {
    if (!Granted(*files_, request.url)) {
      Refuse(std::move(client), net::ERR_BLOCKED_BY_CLIENT);
      return;
    }
    // Deletes itself when either end goes.
    (new FencedLoader(files_, std::move(loader), std::move(client)))
        ->Start(target_.get(), request_id, options, request, annotation);
  }
  void Clone(mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver) override {
    receivers_.Add(this, std::move(receiver));
  }

 private:
  void Disconnected() {
    if (receivers_.empty())
      Gone();
  }
  void Gone() { delete this; }

  const scoped_refptr<const PageFiles> files_;
  mojo::Remote<network::mojom::URLLoaderFactory> target_;
  mojo::ReceiverSet<network::mojom::URLLoaderFactory> receivers_;
};

// ---- A library's page navigates once, to itself ----

class PageThrottle : public content::NavigationThrottle {
 public:
  PageThrottle(content::NavigationThrottleRegistry& registry, scoped_refptr<const PageFiles> files)
      : NavigationThrottle(registry), files_(std::move(files)) {}
  ThrottleCheckResult WillStartRequest() override { return Check(); }
  ThrottleCheckResult WillRedirectRequest() override { return Check(); }
  ThrottleCheckResult WillCommitWithoutUrlLoader() override { return Check(); }
  const char* GetNameForLogging() override { return "ZebkitLibraryPage"; }

 private:
  ThrottleCheckResult Check() {
    content::NavigationHandle* handle = navigation_handle();
    // Frames in the page navigate as the page's script has them; what they
    // load goes through the fence.
    if (!handle->IsInMainFrame())
      return PROCEED;
    // The main frame: the engine's load of the page, and nothing else.
    if (!handle->IsRendererInitiated() && url::Origin::Create(handle->GetURL()) == files_->origin &&
        handle->GetURL().path() == "/")
      return PROCEED;
    return CANCEL_AND_IGNORE;
  }

  const scoped_refptr<const PageFiles> files_;
};

}  // namespace

// ---- The host: the profile, the window, the renderer ----

class LibraryPageHost : public content::WebContentsObserver {
 public:
  LibraryPageHost(LibraryPage* page, scoped_refptr<const PageFiles> files) : page_(page), files_(std::move(files)) {
    Hosts().insert(this);
  }

  ~LibraryPageHost() override {
    Hosts().erase(this);
    // Not told any more, whatever goes now.
    Observe(nullptr);
    capture_.RunAndReset();
    if (Window* window = window_.get()) {
      window_ = nullptr;
      window->Close();
    }
    if (Profile* profile = profile_.get()) {
      profile_ = nullptr;
      DestroyProfileWhenUnused(profile);
    }
  }

  static std::set<LibraryPageHost*>& Hosts() {
    static base::NoDestructor<std::set<LibraryPageHost*>> hosts;
    return *hosts;
  }

  static LibraryPageHost* Of(content::WebContents* contents) {
    if (!contents)
      return nullptr;
    for (LibraryPageHost* host : Hosts()) {
      if (host->web_contents() == contents)
        return host;
    }
    return nullptr;
  }

  bool Open(int width, int height, std::string* error) {
    if (Window::IsShutDown()) {
      *error = "the engine is stopping";
      return false;
    }
    // Its own profile, in memory: the engine's settings, none of the
    // application's cookies, storage or permissions. It knows whose it is
    // before anything of its network is made.
    profile_ = new Profile(/*off_the_record=*/true);
    auto data = std::make_unique<ProfileData>();
    data->files = files_;
    profile_->SetUserData(kProfileKey, std::move(data));
    ConfigureContext(profile_);
    // A window never shown: one exists for it on every system (Windows,
    // Wayland and --headless too), as an off-screen view's root does not.
    WindowOptions options;
    options.size = gfx::Size(width, height);
    options.show = false;
    Window* window = Window::Create(profile_, GURL(), nullptr, options.size, Window::Kind::kNative, &options);
    if (!window || !window->web_contents()) {
      *error = "the library's page could not be made";
      return false;
    }
    window_ = window;
    content::WebContents* contents = window->web_contents();
    Observe(contents);
    // Told it is visible, though no one sees it: its timers are not
    // throttled and requestAnimationFrame runs, as in a shown page.
    capture_ = contents->IncrementCapturerCount(gfx::Size(), /*stay_hidden=*/false, /*stay_awake=*/false,
                                                /*is_activity=*/false);
    // WebRTC's own sockets do not go through the fence: none but through a
    // proxy, and there is none.
    blink::RendererPreferences* prefs = contents->GetMutableRendererPrefs();
    prefs->webrtc_ip_handling_policy =
        blink::ToWebRTCIPHandlingPolicy(blink::kWebRTCIPHandlingDisableNonProxiedUdp);
    contents->SyncRendererPrefs();
    window->LoadURL(files_->origin.GetURL());
    return true;
  }

  // Its renderer stopped: the page is gone, and PrimaryMainFrameRenderProcessGone() says so.
  void Kill() {
    if (!web_contents())
      return;
    content::RenderProcessHost* process = web_contents()->GetPrimaryMainFrame()->GetProcess();
    if (process && process->IsInitializedAndNotDead())
      process->Shutdown(content::RESULT_CODE_KILLED);
  }

  const PageFiles& files() const { return *files_; }
  LibraryPage* page() const { return page_; }

  // content::WebContentsObserver
  void PrimaryMainFrameRenderProcessGone(base::TerminationStatus status) override {
    const bool oom = status == base::TERMINATION_STATUS_OOM;
    page_->End(oom ? ZEBKIT_LIBRARY_OUT_OF_MEMORY : ZEBKIT_LIBRARY_GONE,
               oom ? "the library's page ran out of memory (heapLimitMB); the library has ended"
                   : "the library's page went (its renderer crashed or was stopped); the library has ended");
  }
  void DidFinishNavigation(content::NavigationHandle* handle) override {
    if (!handle->IsInPrimaryMainFrame() || handle->IsSameDocument())
      return;
    if (!handle->HasCommitted() || handle->IsErrorPage())
      page_->Loaded(ZEBKIT_LIBRARY_LOAD_FAILED,
                    LibraryErrorJson("the library's page did not load: " + net::ErrorToString(handle->GetNetErrorCode()),
                                     "Error"));
  }
  void WebContentsDestroyed() override {
    // Closed under it, as every window is at the engine's end: not again here.
    std::ignore = capture_.Release();
    window_ = nullptr;
    Observe(nullptr);
    if (Profile* profile = profile_.get()) {
      profile_ = nullptr;
      DestroyProfileWhenUnused(profile);
    }
    page_->End(ZEBKIT_LIBRARY_GONE, "the engine closed the library's page; the library has ended");
  }

 private:
  const raw_ptr<LibraryPage> page_;
  const scoped_refptr<const PageFiles> files_;
  raw_ptr<Profile> profile_ = nullptr;
  raw_ptr<Window> window_ = nullptr;
  base::ScopedClosureRunner capture_;
};

// ---- LibraryPage ----

LibraryPage::LibraryPage(zebkit_library* handle) : handle_(handle) {}
LibraryPage::~LibraryPage() = default;

size_t LibraryPage::max_bytes() const {
  return handle_->options.max_bytes;
}

scoped_refptr<LibraryPage> LibraryPage::Start(zebkit_library* handle, std::string* error) {
  const LibraryOptions& options = handle->options;
  // The isolate's own: a page is one instance, and calls nothing of the
  // application's (yet).
  if (options.instances > 1) {
    *error = "instances is the backend \"isolate\"'s: a page is one instance";
    return nullptr;
  }
  if (!options.host.empty()) {
    *error = "host is the backend \"isolate\"'s: a page calls no function of the application's";
    return nullptr;
  }
  auto files = base::MakeRefCounted<PageFiles>();
  files->id = RandomId();
  files->origin = url::Origin::Create(GURL(std::string(kLibraryScheme) + "://" + files->id + "/"));
  files->root = handle->root;
  files->entry = handle->entry;
  files->single_file = handle->single_file;
  files->network = options.network;
  files->heap_limit_mb = std::max<size_t>(16, options.heap_limit >> 20);
  files->name = PathToUTF8(handle->entry);
  // The document: the runner, given the entry, and nothing else of its own.
  const std::string entry =
      "./" + (handle->single_file ? base::EscapePath(PathToUTF8(handle->entry.BaseName())) : std::string("index.mjs"));
  files->html = base::StrCat({"<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"zebkit-library-entry\" "
                              "content=\"",
                              base::EscapeForHTML(entry), "\"></head><body><script>", kLibraryPageRunner,
                              "</script></body></html>"});
  std::string granted;
  for (const url::Origin& origin : options.network)
    granted += " " + origin.Serialize();
  const std::string runner_hash = base::Base64Encode(crypto::SHA256HashString(kLibraryPageRunner));
  // What the renderer enforces too: its own files and the granted origins;
  // no workers, plugins, forms, popups, downloads or modals (sandbox).
  files->csp = base::StrCat({"default-src 'none'; script-src 'self' 'sha256-", runner_hash,
                             "' 'wasm-unsafe-eval'; style-src 'self' 'unsafe-inline'", granted,
                             "; img-src 'self' data: blob:", granted, "; font-src 'self' data:", granted,
                             "; media-src 'self' data: blob:", granted, "; connect-src 'self' data: blob:", granted,
                             "; frame-src 'self' data: blob:; worker-src 'none'; object-src 'none'; "
                             "base-uri 'none'; form-action 'none'; manifest-src 'none'; "
                             "sandbox allow-scripts allow-same-origin"});

  scoped_refptr<LibraryPage> page = base::WrapRefCounted(new LibraryPage(handle));
  page->host_ = std::make_unique<LibraryPageHost>(page.get(), files);
  const int width = options.width ? options.width : kDefaultWidth;
  const int height = options.height ? options.height : kDefaultHeight;
  if (!page->host_->Open(width, height, error)) {
    page->host_.reset();
    return nullptr;
  }
  PostEngineTask(base::BindOnce(&LibraryPage::Deadline, page, 0), base::Milliseconds(options.timeout_ms));
  return page;
}

void LibraryPage::Post(Posts posts) {
  for (base::OnceClosure& post : posts)
    PostEngineTask(std::move(post), base::TimeDelta());
}

void LibraryPage::TellLocked(LibraryCall* call, zebkit_library_code code, std::string json,
                             std::vector<std::string> parts, Posts* posts) {
  if (call->told)
    return;
  call->told = true;
  if (call->func) {
    posts->push_back(base::BindOnce(&Deliver, base::WrapRefCounted(handle_.get()), call->func, call->data.get(), code,
                                    std::move(json), std::move(parts)));
  }
}

void LibraryPage::TellReadyLocked(zebkit_library_code code, std::string json, Posts* posts) {
  if (ready_told_)
    return;
  ready_told_ = true;
  if (handle_->ready_func) {
    posts->push_back(base::BindOnce(&Deliver, base::WrapRefCounted(handle_.get()), handle_->ready_func,
                                    handle_->ready_data.get(), code, std::move(json), std::vector<std::string>()));
  }
}

void LibraryPage::FailAllLocked(zebkit_library_code code, const std::string& json, Posts* posts) {
  for (auto& [id, call] : calls_)
    TellLocked(call.get(), code, json, {}, posts);
  calls_.clear();
  queue_.clear();
  current_ = 0;
}

void LibraryPage::PumpSoon() {
  if (state_ != State::kReady || current_ || queue_.empty() || pump_posted_)
    return;
  pump_posted_ = true;
  PostEngineTask(base::BindOnce(&LibraryPage::Pump, base::WrapRefCounted(this)), base::TimeDelta());
}

uint64_t LibraryPage::Call(const char* function, const char* args_json, const zebkit_part* parts, size_t part_count,
                           int64_t timeout_ms, zebkit_library_func func, void* data) {
  if (!function || !*function || (part_count && !parts))
    return 0;
  const LibraryOptions& options = handle_->options;
  auto call = std::make_unique<LibraryCall>();
  call->function = function;
  call->args = args_json ? args_json : "[]";
  size_t total = call->args.size();
  for (size_t i = 0; i < part_count; ++i)
    total += parts[i].size;
  const bool too_large = total > options.max_bytes;
  if (!too_large) {
    call->parts.reserve(part_count);
    for (size_t i = 0; i < part_count; ++i)
      call->parts.emplace_back(reinterpret_cast<const char*>(parts[i].data), parts[i].data ? parts[i].size : 0);
  }
  call->timeout_ms = timeout_ms == 0 ? options.timeout_ms : timeout_ms;
  call->func = func;
  call->data = data;
  Posts posts;
  uint64_t id;
  {
    base::AutoLock lock(lock_);
    if (state_ == State::kClosed)
      return 0;
    id = call->id = next_id_++;
    LibraryCall* raw = call.get();
    if (state_ == State::kEnded) {
      TellLocked(raw, ZEBKIT_LIBRARY_GONE, CodeJson(ZEBKIT_LIBRARY_GONE, "the library has ended"), {}, &posts);
    } else if (state_ == State::kFailed) {
      TellLocked(raw, ZEBKIT_LIBRARY_LOAD_FAILED, CodeJson(ZEBKIT_LIBRARY_LOAD_FAILED, "the library did not load"), {},
                 &posts);
    } else if (too_large) {
      TellLocked(raw, ZEBKIT_LIBRARY_BAD_ARGUMENTS,
                 LibraryErrorJson("the arguments are larger than maxBytes (" +
                                      base::NumberToString(options.max_bytes) + " bytes)",
                                  "RangeError"),
                 {}, &posts);
    } else if (queue_.size() >= options.queue) {
      TellLocked(raw, ZEBKIT_LIBRARY_QUEUE_FULL,
                 CodeJson(ZEBKIT_LIBRARY_QUEUE_FULL, base::NumberToString(options.queue) + " calls wait already"), {},
                 &posts);
    } else {
      calls_[id] = std::move(call);
      queue_.push_back(id);
      PumpSoon();
    }
  }
  Post(std::move(posts));
  return id;
}

void LibraryPage::Cancel(uint64_t id) {
  Posts posts;
  {
    base::AutoLock lock(lock_);
    auto it = calls_.find(id);
    if (it == calls_.end() || it->second->told)
      return;
    TellLocked(it->second.get(), ZEBKIT_LIBRARY_CANCELLED, CodeJson(ZEBKIT_LIBRARY_CANCELLED, "the call was cancelled"),
               {}, &posts);
    // A running call stays the page's until it settles.
    if (current_ != id) {
      std::erase(queue_, id);
      calls_.erase(it);
    }
  }
  Post(std::move(posts));
}

bool LibraryPage::Close() {
  Posts posts;
  {
    base::AutoLock lock(lock_);
    if (state_ == State::kClosed)
      return false;
    state_ = State::kClosed;
    const std::string json = CodeJson(ZEBKIT_LIBRARY_GONE, "the library was closed");
    TellReadyLocked(ZEBKIT_LIBRARY_GONE, json, &posts);
    FailAllLocked(ZEBKIT_LIBRARY_GONE, json, &posts);
  }
  Post(std::move(posts));
  TearDownSoon();
  return true;
}

void LibraryPage::TearDownSoon() {
  // Not inside what the page's WebContents is telling, nor the caller's.
  PostEngineTask(base::BindOnce(
                     [](scoped_refptr<LibraryPage> page) {
                       if (page->next_)
                         std::move(page->next_).Run(false, Buffer("the library has ended"), mojo_base::BigBuffer());
                       page->host_.reset();
                     },
                     base::WrapRefCounted(this)),
                 base::TimeDelta());
}

void LibraryPage::Loaded(zebkit_library_code code, std::string json) {
  Posts posts;
  bool failed = false;
  {
    base::AutoLock lock(lock_);
    if (state_ != State::kLoading)
      return;
    if (code == ZEBKIT_LIBRARY_OK) {
      state_ = State::kReady;
      TellReadyLocked(code, std::move(json), &posts);
      PumpSoon();
    } else {
      state_ = State::kFailed;
      TellReadyLocked(ZEBKIT_LIBRARY_LOAD_FAILED, json, &posts);
      FailAllLocked(ZEBKIT_LIBRARY_LOAD_FAILED, json, &posts);
      failed = true;
    }
  }
  Post(std::move(posts));
  if (failed)
    TearDownSoon();
}

void LibraryPage::Next(mojom::PageCallHost::CallCallback reply) {
  {
    base::AutoLock lock(lock_);
    if (state_ != State::kReady && state_ != State::kLoading) {
      std::move(reply).Run(false, Buffer("the library has ended"), mojo_base::BigBuffer());
      return;
    }
  }
  if (next_)
    std::move(next_).Run(false, Buffer("asked twice"), mojo_base::BigBuffer());
  next_ = std::move(reply);
  Pump();
}

void LibraryPage::Pump() {
  uint64_t id;
  std::string message;
  mojo_base::BigBuffer parts;
  int64_t timeout_ms;
  Posts posts;
  {
    base::AutoLock lock(lock_);
    pump_posted_ = false;
    if (state_ != State::kReady || current_ || queue_.empty() || !next_)
      return;
    id = queue_.front();
    queue_.pop_front();
    LibraryCall* call = calls_[id].get();
    std::optional<std::string> name = base::WriteJson(base::Value(call->function));
    if (!name) {
      TellLocked(call, ZEBKIT_LIBRARY_NO_FUNCTION,
                 LibraryErrorJson("the function's name is not UTF-8", "ReferenceError"), {}, &posts);
      calls_.erase(id);
      PumpSoon();
    } else {
      current_ = id;
      // The parts' sizes: "" for none, "0" for one of no bytes.
      std::string sizes;
      size_t total = 0;
      for (size_t i = 0; i < call->parts.size(); ++i) {
        if (i)
          sizes += ",";
        sizes += base::NumberToString(call->parts[i].size());
        total += call->parts[i].size();
      }
      // Written once, into the buffer that goes (shared memory past 64 KiB).
      parts = mojo_base::BigBuffer(total);
      size_t at = 0;
      for (const std::string& part : call->parts) {
        if (!part.empty())
          memcpy(parts.data() + at, part.data(), part.size());
        at += part.size();
      }
      message = base::StrCat({base::NumberToString(id), "\n", sizes, "\n", *name, "\n", call->args});
      call->parts.clear();
      call->args.clear();
      timeout_ms = call->timeout_ms;
    }
  }
  Post(std::move(posts));
  if (message.empty())
    return;
  std::move(next_).Run(true, Buffer(message), std::move(parts));
  if (timeout_ms > 0) {
    PostEngineTask(base::BindOnce(&LibraryPage::Deadline, base::WrapRefCounted(this), id),
                   base::Milliseconds(timeout_ms));
  }
}

void LibraryPage::Done(uint64_t id, zebkit_library_code code, std::string json, std::vector<std::string> parts) {
  Posts posts;
  {
    base::AutoLock lock(lock_);
    if (current_ != id || id == 0)
      return;  // past its deadline, or the library ended: dropped
    current_ = 0;
    auto it = calls_.find(id);
    if (it != calls_.end()) {
      TellLocked(it->second.get(), code, std::move(json), std::move(parts), &posts);
      calls_.erase(it);
    }
    PumpSoon();
  }
  Post(std::move(posts));
}

void LibraryPage::Deadline(uint64_t id) {
  Posts posts;
  bool failed = false;
  {
    base::AutoLock lock(lock_);
    if (id == 0) {
      if (state_ != State::kLoading)
        return;
      // A load that has not ended, waiting or running: it fails, and the
      // page goes (its renderer with it, if it spins).
      state_ = State::kFailed;
      const std::string json = CodeJson(ZEBKIT_LIBRARY_LOAD_FAILED, "the load passed its deadline (timeoutMs)");
      TellReadyLocked(ZEBKIT_LIBRARY_LOAD_FAILED, json, &posts);
      FailAllLocked(ZEBKIT_LIBRARY_LOAD_FAILED, json, &posts);
      failed = true;
    } else {
      if (current_ != id || state_ != State::kReady)
        return;
      if (!next_) {
        // No request for the next call out: the page is running script,
        // unless that request is on its way. A moment for it to come.
        PostEngineTask(base::BindOnce(&LibraryPage::DeadlineGrace, base::WrapRefCounted(this), id), kGrace);
        return;
      }
      // Waiting: TIMEOUT, and the queue moves on; what it does later is dropped.
      auto it = calls_.find(id);
      if (it != calls_.end()) {
        TellLocked(it->second.get(), ZEBKIT_LIBRARY_TIMEOUT,
                   CodeJson(ZEBKIT_LIBRARY_TIMEOUT, "the call passed its deadline while it waited"), {}, &posts);
        calls_.erase(it);
      }
      current_ = 0;
      PumpSoon();
    }
  }
  Post(std::move(posts));
  if (failed) {
    if (host_)
      host_->Kill();
    TearDownSoon();
  }
}

void LibraryPage::DeadlineGrace(uint64_t id) {
  {
    base::AutoLock lock(lock_);
    if (current_ != id || state_ != State::kReady)
      return;
  }
  if (next_) {
    // It was waiting after all.
    Deadline(id);
    return;
  }
  // Running past its deadline: its renderer is stopped, and the library ends.
  Kill(ZEBKIT_LIBRARY_TIMEOUT);
}

void LibraryPage::Kill(zebkit_library_code code) {
  if (killed_for_ == ZEBKIT_LIBRARY_OK)
    killed_for_ = code;
  if (host_)
    host_->Kill();
}

void LibraryPage::End(zebkit_library_code code, const std::string& why) {
  if (killed_for_ != ZEBKIT_LIBRARY_OK) {
    code = killed_for_;
  }
  const std::string message =
      code == ZEBKIT_LIBRARY_TIMEOUT
          ? "the script ran past the call's deadline and its page was stopped; the library has ended"
          : why;
  Posts posts;
  {
    base::AutoLock lock(lock_);
    if (state_ == State::kLoading) {
      state_ = State::kFailed;
      const std::string json = LibraryErrorJson(message, "Error");
      TellReadyLocked(ZEBKIT_LIBRARY_LOAD_FAILED, json, &posts);
      FailAllLocked(ZEBKIT_LIBRARY_LOAD_FAILED, json, &posts);
    } else if (state_ == State::kReady) {
      state_ = State::kEnded;
      auto it = calls_.find(current_);
      if (it != calls_.end())
        TellLocked(it->second.get(), code, CodeJson(code, message), {}, &posts);
      FailAllLocked(ZEBKIT_LIBRARY_GONE, CodeJson(ZEBKIT_LIBRARY_GONE, message), &posts);
    }
  }
  Post(std::move(posts));
  TearDownSoon();
}

// ---- What the engine asks ----

void AddLibraryPageScheme(content::ContentClient::Schemes* schemes) {
  schemes->standard_schemes.push_back(kLibraryScheme);
  schemes->secure_schemes.push_back(kLibraryScheme);
  // Module scripts and fetch() of the page's own files.
  schemes->cors_enabled_schemes.push_back(kLibraryScheme);
}

void RegisterLibraryPageSchemeInRenderer() {
  blink::WebSecurityPolicy::RegisterURLSchemeAsSupportingFetchAPI(blink::WebString::FromLatin1(kLibraryScheme));
}

bool IsLibraryPageContext(content::BrowserContext* context) {
  return FilesOf(context) != nullptr;
}

bool IsLibraryPageFrame(content::RenderFrameHost* frame) {
  return frame && IsLibraryPageContext(frame->GetBrowserContext());
}

mojo::PendingRemote<network::mojom::URLLoaderFactory> CreateLibraryPageNavigationFactory(
    content::FrameTreeNodeId frame_tree_node_id) {
  content::WebContents* contents = content::WebContents::FromFrameTreeNodeId(frame_tree_node_id);
  const PageFiles* files = contents ? FilesOf(contents->GetBrowserContext()) : nullptr;
  // Another page's navigation to a library's URL finds nothing there.
  return NewFilesFactory(base::WrapRefCounted(files));
}

void AddLibraryPageSubresourceFactory(int render_process_id, int render_frame_id,
                                      content::ContentBrowserClient::NonNetworkURLLoaderFactoryMap* factories) {
  content::RenderFrameHost* frame = content::RenderFrameHost::FromID(render_process_id, render_frame_id);
  const PageFiles* files = frame ? FilesOf(frame->GetBrowserContext()) : nullptr;
  if (files)
    factories->emplace(kLibraryScheme, NewFilesFactory(base::WrapRefCounted(files)));
}

void FenceLibraryPageNetwork(content::BrowserContext* context, network::URLLoaderFactoryBuilder& builder) {
  const PageFiles* files = FilesOf(context);
  if (!files)
    return;
  auto [receiver, target] = builder.Append();
  // Deletes itself when the page's factory and its clones go.
  new Fence(base::WrapRefCounted(files), std::move(receiver), std::move(target));
}

void AddLibraryPageThrottle(content::NavigationThrottleRegistry& registry) {
  content::WebContents* contents = registry.GetNavigationHandle().GetWebContents();
  const PageFiles* files = contents ? FilesOf(contents->GetBrowserContext()) : nullptr;
  if (files)
    registry.AddThrottle(std::make_unique<PageThrottle>(registry, base::WrapRefCounted(files)));
}

void AppendLibraryPageSwitches(base::CommandLine* command_line, int child_process_id) {
  // Renderers are started from the engine's thread; other children (the GPU,
  // utilities) from others, and are not a library's.
  if (!CurrentlyOnEngineThread() || command_line->GetSwitchValueASCII("type") != "renderer")
    return;
  content::RenderProcessHost* process = content::RenderProcessHost::FromID(child_process_id);
  const PageFiles* files = process ? FilesOf(process->GetBrowserContext()) : nullptr;
  if (!files)
    return;
  // V8's heap in the page's renderer: past it the renderer stops (its
  // out-of-memory end), and the library ends with OUT_OF_MEMORY.
  std::string flags = command_line->GetSwitchValueASCII("js-flags");
  if (!flags.empty())
    flags += " ";
  flags += "--max-old-space-size=" + base::NumberToString(files->heap_limit_mb);
  command_line->AppendSwitchASCII("js-flags", flags);
}

bool TakeLibraryPageCall(content::RenderFrameHost* frame, const std::string& module, const std::string& method,
                         mojo_base::BigBuffer& json, mojo_base::BigBuffer& parts,
                         mojom::PageCallHost::CallCallback& reply) {
  if (!frame || !IsLibraryPageFrame(frame))
    return false;
  content::WebContents* contents = content::WebContents::FromRenderFrameHost(frame);
  LibraryPageHost* host = LibraryPageHost::Of(contents);
  auto refuse = [&](const char* why) {
    std::move(reply).Run(false, Buffer(why), mojo_base::BigBuffer());
    return true;
  };
  // Only the runner of the page itself: its main frame, of its origin.
  if (module != "library" || !host || frame != contents->GetPrimaryMainFrame() ||
      frame->GetLastCommittedOrigin() != host->files().origin) {
    return refuse("zebkit is not available in this frame");
  }
  LibraryPage* page = host->page();
  // Copied out of the memory the renderer shares before it is read.
  const std::string text(reinterpret_cast<const char*>(json.data()), json.size());
  if (method == "next") {
    page->Next(std::move(reply));
    return true;
  }
  if (method == "oom") {
    // From the renderer itself (library_page_renderer.cc): its heap is full.
    page->Kill(ZEBKIT_LIBRARY_OUT_OF_MEMORY);
    std::move(reply).Run(true, mojo_base::BigBuffer(), mojo_base::BigBuffer());
    return true;
  }
  if (method == "log") {
    fprintf(stderr, "zebkit library %s: %s\n", host->files().name.c_str(), text.c_str());
    fflush(stderr);
    std::move(reply).Run(true, mojo_base::BigBuffer(), mojo_base::BigBuffer());
    return true;
  }
  // "<code>\n<json>" (loaded) or "<id>\n<code>\n<sizes>\n<json>" (done).
  const bool done = method == "done";
  if (!done && method != "loaded")
    return refuse("not a call");
  std::vector<std::string_view> lines;
  size_t at = 0;
  for (int i = 0; i < (done ? 3 : 1); ++i) {
    const size_t end = text.find('\n', at);
    if (end == std::string::npos)
      return refuse("not a call");
    lines.push_back(std::string_view(text).substr(at, end - at));
    at = end + 1;
  }
  std::string value = text.substr(at);
  uint64_t id = 0;
  int code = -1;
  if ((done && !base::StringToUint64(lines[0], &id)) || !base::StringToInt(lines[done ? 1 : 0], &code) ||
      code < ZEBKIT_LIBRARY_OK || code > ZEBKIT_LIBRARY_BAD_RESULT) {
    return refuse("not a call");
  }
  std::vector<std::string> list;
  const size_t max_bytes = page->max_bytes();
  if (done && !lines[2].empty()) {
    size_t offset = 0;
    for (std::string_view size_text : base::SplitStringPiece(lines[2], ",", base::KEEP_WHITESPACE,
                                                             base::SPLIT_WANT_ALL)) {
      size_t size = 0;
      if (!base::StringToSizeT(size_text, &size) || size > parts.size() - offset)
        return refuse("not a call");
      list.emplace_back(reinterpret_cast<const char*>(parts.data()) + offset, size);
      offset += size;
    }
    if (offset != parts.size())
      return refuse("not a call");
  }
  std::move(reply).Run(true, mojo_base::BigBuffer(), mojo_base::BigBuffer());
  size_t total = value.size();
  for (const std::string& part : list)
    total += part.size();
  auto result = static_cast<zebkit_library_code>(code);
  if (!IsPlainJson(value, false)) {
    // The page's renderer is not trusted to write JSON the application reads.
    result = done ? ZEBKIT_LIBRARY_BAD_RESULT : ZEBKIT_LIBRARY_LOAD_FAILED;
    value = LibraryErrorJson("the library's page answered with what is not JSON", "TypeError");
    list.clear();
  } else if (total > max_bytes) {
    result = ZEBKIT_LIBRARY_BAD_RESULT;
    value = LibraryErrorJson("the value is larger than maxBytes (" + base::NumberToString(max_bytes) + " bytes)",
                             "RangeError");
    list.clear();
  }
  if (done)
    page->Done(id, result, std::move(value), std::move(list));
  else
    page->Loaded(result == ZEBKIT_LIBRARY_OK ? ZEBKIT_LIBRARY_OK : ZEBKIT_LIBRARY_LOAD_FAILED, std::move(value));
  return true;
}

}  // namespace zebkit
