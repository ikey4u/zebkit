// Downloads (<zebkit/hooks.h>): the target of a download asked of the
// application, as Chrome's DownloadTargetDeterminer waits for its answers.

#include <stdlib.h>
#include <string.h>

#include <map>
#include <set>
#include <string>
#include <utility>

#include "base/files/file_path.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_util.h"
#include "components/download/public/common/download_item.h"
#include "components/download/public/common/download_target_info.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/devtools_agent_host_client_channel.h"
#include "content/public/browser/download_item_utils.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/web_contents.h"
#include "zebkit/engine/download_manager.h"
#include "net/base/filename_util.h"
#include "url/gurl.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/hook_requests.h"
#include "zebkit/engine/media_capture.h"
#include "zebkit/engine/page_hooks.h"
#include "zebkit/engine/page_internal.h"
#include "zebkit/engine/settings.h"

namespace zebkit {
namespace {

struct Traits {
  struct Decision {
    bool save = false;
    base::FilePath path;
  };
  using Request = zebkit_download_request;
  static constexpr auto kHandler = &zebkit_view::on_download_;
  static constexpr auto kData = &zebkit_view::on_download_data_;
  static constexpr auto kResult = &zebkit_view::on_download_progress_;
  static constexpr auto kResultData = &zebkit_view::on_download_progress_data_;
  static constexpr auto kGeneration = &zebkit_view::download_generation_;
  static constexpr auto kTimeout = &zebkit_view::download_timeout_ms_;
  static Decision Refusal() { return {}; }
  static Decision Unanswered() { return {}; }
  static const char* State(const Decision& decision) { return decision.save ? "save" : "cancelled"; }
};
using Requests = HookRequests<Traits>;

std::string Redacted(const GURL& url) {
  GURL::Replacements redact;
  redact.ClearUsername();
  redact.ClearPassword();
  return url.ReplaceComponents(redact).spec();
}

// What a download's interrupt reason is about, and Chrome's words for it
// (its download shelf's IDS_DOWNLOAD_INTERRUPTED_DESCRIPTION_*).
struct Failure {
  const char* category;
  const char* message;
};
Failure FailureOf(download::DownloadInterruptReason reason) {
  using R = download::DownloadInterruptReason;
  switch (reason) {
    case R::DOWNLOAD_INTERRUPT_REASON_FILE_ACCESS_DENIED:
      return {"file", "Insufficient permissions"};
    case R::DOWNLOAD_INTERRUPT_REASON_FILE_NO_SPACE:
      return {"file", "Disk full"};
    case R::DOWNLOAD_INTERRUPT_REASON_FILE_NAME_TOO_LONG:
      return {"file", "Path too long"};
    case R::DOWNLOAD_INTERRUPT_REASON_FILE_TOO_LARGE:
      return {"file", "File too large"};
    case R::DOWNLOAD_INTERRUPT_REASON_FILE_VIRUS_INFECTED:
      return {"blocked", "Virus detected"};
    case R::DOWNLOAD_INTERRUPT_REASON_FILE_TRANSIENT_ERROR:
      return {"file", "System busy"};
    case R::DOWNLOAD_INTERRUPT_REASON_FILE_BLOCKED:
      return {"blocked", "Blocked"};
    case R::DOWNLOAD_INTERRUPT_REASON_FILE_SECURITY_CHECK_FAILED:
      return {"blocked", "Virus scan failed"};
    case R::DOWNLOAD_INTERRUPT_REASON_FILE_TOO_SHORT:
      return {"file", "File incomplete"};
    case R::DOWNLOAD_INTERRUPT_REASON_FILE_HASH_MISMATCH:
      return {"file", "Hash mismatch"};
    case R::DOWNLOAD_INTERRUPT_REASON_FILE_SAME_AS_SOURCE:
      return {"file", "Already downloaded"};
    case R::DOWNLOAD_INTERRUPT_REASON_FILE_FAILED:
      return {"file", "Failed to save the file"};
    case R::DOWNLOAD_INTERRUPT_REASON_NETWORK_FAILED:
      return {"network", "Network error"};
    case R::DOWNLOAD_INTERRUPT_REASON_NETWORK_TIMEOUT:
      return {"network", "Network timeout"};
    case R::DOWNLOAD_INTERRUPT_REASON_NETWORK_DISCONNECTED:
      return {"network", "Network disconnected"};
    case R::DOWNLOAD_INTERRUPT_REASON_NETWORK_SERVER_DOWN:
      return {"network", "Server unavailable"};
    case R::DOWNLOAD_INTERRUPT_REASON_NETWORK_INVALID_REQUEST:
      return {"network", "Invalid request"};
    case R::DOWNLOAD_INTERRUPT_REASON_SERVER_FAILED:
      return {"server", "Server problem"};
    case R::DOWNLOAD_INTERRUPT_REASON_SERVER_NO_RANGE:
      return {"server", "Server does not support resuming"};
    case R::DOWNLOAD_INTERRUPT_REASON_SERVER_BAD_CONTENT:
      return {"server", "No file"};
    case R::DOWNLOAD_INTERRUPT_REASON_SERVER_UNAUTHORIZED:
      return {"server", "Needs authorization"};
    case R::DOWNLOAD_INTERRUPT_REASON_SERVER_CERT_PROBLEM:
      return {"server", "Bad certificate"};
    case R::DOWNLOAD_INTERRUPT_REASON_SERVER_FORBIDDEN:
      return {"server", "Forbidden"};
    case R::DOWNLOAD_INTERRUPT_REASON_SERVER_UNREACHABLE:
      return {"server", "Unreachable server"};
    case R::DOWNLOAD_INTERRUPT_REASON_SERVER_CONTENT_LENGTH_MISMATCH:
      return {"server", "File incomplete"};
    case R::DOWNLOAD_INTERRUPT_REASON_SERVER_CROSS_ORIGIN_REDIRECT:
      return {"server", "Cross-origin redirect"};
    case R::DOWNLOAD_INTERRUPT_REASON_USER_CANCELED:
      return {"cancelled", "Cancelled"};
    case R::DOWNLOAD_INTERRUPT_REASON_USER_SHUTDOWN:
      return {"shutdown", "Shut down"};
    case R::DOWNLOAD_INTERRUPT_REASON_CRASH:
      return {"crash", "Crashed"};
    case R::DOWNLOAD_INTERRUPT_REASON_LOCAL_DOWNLOAD_BLOCKED:
      return {"blocked", "Blocked"};
    default:
      return {"failed", "Failed"};
  }
}

// A download's report: zebkit_view_on_download_progress()'s JSON, and
// Zebkit.downloadProgress's.
base::DictValue Report(download::DownloadItem* item) {
  const auto state = item->GetState();
  const bool done = state != download::DownloadItem::IN_PROGRESS;
  const download::DownloadInterruptReason reason = item->GetLastReason();
  base::DictValue event;
  event.Set("downloadId", "d" + base::NumberToString(item->GetId()));
  // As CDP's Browser.downloadWillBegin and downloadProgress name it.
  event.Set("guid", item->GetGuid());
  event.Set("url", Redacted(item->GetURL()));
  event.Set("state", state == download::DownloadItem::COMPLETE    ? "complete"
                     : state == download::DownloadItem::CANCELLED ? "cancelled"
                     : done                                       ? "interrupted"
                                                                  : "in-progress");
  // Cancelled (by the application, a hook's answer, CDP or the user) or
  // failed (the network, the server, the file system, ...).
  const bool cancelled =
      state == download::DownloadItem::CANCELLED ||
      reason == download::DOWNLOAD_INTERRUPT_REASON_USER_CANCELED;
  event.Set("outcome", !done                                       ? "in-progress"
                       : state == download::DownloadItem::COMPLETE ? "complete"
                       : cancelled                                 ? "cancelled"
                                                                   : "failed");
  event.Set("path", item->GetTargetFilePath().AsUTF8Unsafe());
  if (state == download::DownloadItem::COMPLETE)
    event.Set("savedPath", item->GetFullPath().AsUTF8Unsafe());
  else
    event.Set("savedPath", base::Value());
  event.Set("receivedBytes", static_cast<double>(item->GetReceivedBytes()));
  event.Set("totalBytes", static_cast<double>(item->GetTotalBytes()));
  event.Set("mimeType", item->GetMimeType());
  event.Set("reason", download::DownloadInterruptReasonToString(reason));
  if (done && state != download::DownloadItem::COMPLETE) {
    // A cancellation without a reason of its own is the user's.
    const download::DownloadInterruptReason why =
        reason == download::DOWNLOAD_INTERRUPT_REASON_NONE
            ? download::DOWNLOAD_INTERRUPT_REASON_USER_CANCELED
            : reason;
    const Failure failure = FailureOf(why);
    base::DictValue error;
    error.Set("category", cancelled ? "cancelled" : failure.category);
    error.Set("code", static_cast<int>(why));
    error.Set("name", download::DownloadInterruptReasonToString(why));
    error.Set("message", cancelled ? "Cancelled" : failure.message);
    // Chrome may go on with it by itself (a network error, with a server
    // that resumes), when a report in progress follows.
    error.Set("resumable", !cancelled && item->CanResume());
    event.Set("error", std::move(error));
  } else {
    event.Set("error", base::Value());
  }
  event.Set("done", done);
  return event;
}

void Answer(download::DownloadTargetCallback callback, Traits::Decision decision) {
  download::DownloadTargetInfo target;
  if (decision.save) {
    // Windows' move of a download (SHFileOperation) takes no forward slashes.
    const base::FilePath path = decision.path.NormalizePathSeparators();
    target.target_path = path;
    target.intermediate_path = path.AddExtension(FILE_PATH_LITERAL(".crdownload"));
  } else {
    target.interrupt_reason = download::DOWNLOAD_INTERRUPT_REASON_USER_CANCELED;
  }
  std::move(callback).Run(std::move(target));
}

// The downloads of views, for progress, for a request whose download ended, and
// for zebkit_view_cancel_download().
class Watcher : public content::DownloadManager::Observer,
                public download::DownloadItem::Observer {
 public:
  static Watcher& Get() {
    static base::NoDestructor<Watcher> watcher;
    return *watcher;
  }

  void WatchProfileOf(zebkit_view* view) {
    used_ = true;
    content::DownloadManager* manager = view->web_contents()->GetBrowserContext()->GetDownloadManager();
    if (manager && managers_.insert(manager).second)
      manager->AddObserver(this);
  }
  bool used() const { return used_; }

  void Track(download::DownloadItem* item, zebkit_view* view) {
    auto [it, added] = items_.try_emplace(item);
    if (!added)
      return;
    it->second.item = item;
    it->second.view = view->GetWeakPtr();
    item->AddObserver(this);
    Tell(item, it->second);
  }
  void SetRequest(download::DownloadItem* item, uintptr_t request) {
    if (auto it = items_.find(item); it != items_.end())
      it->second.request = request;
  }
  bool Cancel(zebkit_view* view, const std::string& id) {
    for (auto& [key, entry] : items_) {
      if (entry.view.get() == view && "d" + base::NumberToString(entry.item->GetId()) == id &&
          entry.item->GetState() == download::DownloadItem::IN_PROGRESS) {
        entry.item->Cancel(true);
        return true;
      }
    }
    return false;
  }
  void Stop() {
    sessions_.clear();
    is_page_session_.clear();
    for (auto& [key, entry] : items_)
      entry.item->RemoveObserver(this);
    items_.clear();
    for (content::DownloadManager* manager : managers_)
      manager->RemoveObserver(this);
    managers_.clear();
  }

  // content::DownloadManager::Observer
  void OnDownloadCreated(content::DownloadManager*, download::DownloadItem* item) override {
    content::WebContents* contents = content::DownloadItemUtils::GetWebContents(item);
    if (zebkit_view* view = contents ? ViewOf(contents) : nullptr)
      Track(item, view);
  }

  // Zebkit.setDownloadEvents: a page's session hears of that page's
  // downloads, the browser's of all of the views'.
  void AddSession(content::DevToolsAgentHostClientChannel* channel,
                  content::WebContents* contents) {
    sessions_[channel] = contents ? contents->GetWeakPtr() : nullptr;
    is_page_session_[channel] = contents != nullptr;
    for (zebkit_view* view : zebkit_view::Views()) {
      if (view->web_contents() && (!contents || view->web_contents() == contents))
        WatchProfileOf(view);
    }
  }
  void RemoveSession(content::DevToolsAgentHostClientChannel* channel) {
    sessions_.erase(channel);
    is_page_session_.erase(channel);
  }
  bool has_sessions() const { return !sessions_.empty(); }
  void TellSessions(download::DownloadItem* item, const base::DictValue& event) {
    if (sessions_.empty())
      return;
    content::WebContents* contents = content::DownloadItemUtils::GetWebContents(item);
    for (auto it = sessions_.begin(); it != sessions_.end();) {
      const bool page = is_page_session_[it->first];
      if (page && (!it->second || it->second.get() != contents)) {
        ++it;
        continue;
      }
      base::DictValue message;
      message.Set("method", "Zebkit.downloadProgress");
      message.Set("params", event.Clone());
      if (SendToDevToolsClient(it->first, std::move(message))) {
        ++it;
      } else {
        is_page_session_.erase(it->first);
        it = sessions_.erase(it);
      }
    }
  }
  void ManagerGoingDown(content::DownloadManager* manager) override {
    manager->RemoveObserver(this);
    managers_.erase(manager);
  }

  // download::DownloadItem::Observer
  void OnDownloadUpdated(download::DownloadItem* item) override {
    auto it = items_.find(item);
    if (it == items_.end())
      return;
    const uintptr_t request = it->second.request;
    if (request && item->GetState() != download::DownloadItem::IN_PROGRESS) {
      it->second.request = 0;
      Requests::Get().Settle(request, Traits::Refusal(), "cancelled");
      it = items_.find(item);
      if (it == items_.end())
        return;
    }
    Tell(item, it->second);
  }
  void OnDownloadDestroyed(download::DownloadItem* item) override {
    auto it = items_.find(item);
    if (it == items_.end())
      return;
    Requests::Get().Abandon(it->second.request);
    item->RemoveObserver(this);
    items_.erase(it);
  }

 private:
  struct Entry {
    download::DownloadItem* item = nullptr;
    base::WeakPtr<zebkit_view> view;
    uintptr_t request = 0;
    int64_t received = -1;
    download::DownloadItem::DownloadState state = download::DownloadItem::MAX_DOWNLOAD_STATE;
  };

  void Tell(download::DownloadItem* item, Entry& entry) {
    const auto state = item->GetState();
    if (entry.state == state && entry.received == item->GetReceivedBytes())
      return;
    entry.state = state;
    entry.received = item->GetReceivedBytes();
    zebkit_view* view = entry.view.get();
    base::DictValue event = Report(item);
    TellSessions(item, event);
    // The view's own function, or that of the opener a popup inherits from.
    zebkit_view* teller = view ? view->Decider(&zebkit_view::on_download_progress_) : nullptr;
    if (!teller || !teller->on_download_progress_)
      return;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](base::WeakPtr<zebkit_view> view, base::WeakPtr<zebkit_view> teller, uint64_t generation,
               std::string json) {
              if (!view || !teller || teller->download_generation_ != generation || !teller->on_download_progress_)
                return;
              auto function = teller->on_download_progress_;
              function(view.get(), json.c_str(), teller->on_download_progress_data_);
            },
            entry.view, teller->GetWeakPtr(), teller->download_generation_, base::WriteJson(event).value_or("{}")));
  }

  bool used_ = false;
  std::set<content::DownloadManager*> managers_;
  // Zebkit.setDownloadEvents' sessions: a page's, or the browser's (null).
  std::map<content::DevToolsAgentHostClientChannel*, base::WeakPtr<content::WebContents>> sessions_;
  std::map<content::DevToolsAgentHostClientChannel*, bool> is_page_session_;
  // By item: each profile's manager counts its ids from where the delegate
  // started it, so two profiles' items can share one.
  std::map<download::DownloadItem*, Entry> items_;
};

void Cancel(download::DownloadTargetCallback* callback) {
  Answer(std::move(*callback), Traits::Decision());
}

bool TakeDownload(download::DownloadItem* item, download::DownloadTargetCallback* callback) {
  auto& requests = Requests::Get();
  if (!requests.Ready())
    return false;
  content::WebContents* contents = content::DownloadItemUtils::GetWebContents(item);
  zebkit_view* view = contents ? ViewOf(contents) : nullptr;
  if (!view) {
    // Its view closed before it was asked: not past a policy that was set.
    if (!Watcher::Get().used() && !EngineDeniesDownloads())
      return false;
    Cancel(callback);
    return true;
  }
  if (Requests::OrphanedPopup(view)) {
    Cancel(callback);
    return true;
  }
  if (!Requests::HasDecider(view)) {
    // "downloads": "deny" of the runtime settings, with no function to ask.
    if (!view->deny_downloads_.value_or(EngineDeniesDownloads()))
      return false;
    Cancel(callback);
    return true;
  }
  Watcher::Get().Track(item, view);
  const std::string name =
      net::GenerateFileName(item->GetURL(), item->GetContentDisposition(), std::string(),
                            item->GetSuggestedFilename(), item->GetMimeType(), "download")
          .AsUTF8Unsafe();
  base::DictValue metadata;
  metadata.Set("downloadId", "d" + base::NumberToString(item->GetId()));
  metadata.Set("url", Redacted(item->GetURL()));
  metadata.Set("suggestedName", name);
  metadata.Set("mimeType", item->GetMimeType());
  if (item->GetTotalBytes() > 0)
    metadata.Set("totalBytes", static_cast<double>(item->GetTotalBytes()));
  else
    metadata.Set("totalBytes", base::Value());
  metadata.Set("userGesture", item->HasUserGesture());
  metadata.Set("origin", url::Origin::Create(item->GetTabUrl()).Serialize());
  Requests::Reply reply = base::BindOnce(&Answer, std::move(*callback));
  if (!requests.Ask(view, std::move(metadata), reply)) {
    std::move(reply).Run(Traits::Decision());
    return true;
  }
  Watcher::Get().SetRequest(item, requests.last_id());
  return true;
}

}  // namespace

void InstallDownloadHooks() {
  Requests::Get().Start();
  EngineDownloadManagerDelegate::SetTargetHandler(base::BindRepeating(&TakeDownload));
}
void StopDownloadHooks() {
  EngineDownloadManagerDelegate::SetTargetHandler({});
  Requests::Get().Stop();
  Watcher::Get().Stop();
}
void CancelDownloadRequests(zebkit_view* view) { Requests::Get().CancelAll(view, "cancelled"); }
void SetDownloadPolicy(zebkit_view* view, std::optional<bool> deny) {
  view->deny_downloads_ = deny;
  // A download whose view has closed by the time it is decided is not let past it.
  if (deny.value_or(false))
    Watcher::Get().WatchProfileOf(view);
}
bool IsDownloadCommand(std::string_view method) {
  return method == "Zebkit.setDownloadEvents";
}

void HandleDownloadCommand(content::DevToolsAgentHostClientChannel* channel,
                           int id,
                           std::string_view method,
                           const std::string& params_json) {
  std::optional<base::Value> parsed = base::JSONReader::Read(params_json, 0);
  const bool enabled = parsed && parsed->is_dict() &&
                       parsed->GetDict().FindBool("enabled").value_or(false);
  base::DictValue message;
  message.Set("id", id);
  if (!Requests::Get().Ready()) {
    base::DictValue error;
    error.Set("code", -32000);
    error.Set("message", "the engine is not running");
    message.Set("error", std::move(error));
    SendToDevToolsClient(channel, std::move(message));
    return;
  }
  content::DevToolsAgentHost* host = channel->GetAgentHost();
  content::WebContents* contents = nullptr;
  if (host && host->GetType() == content::DevToolsAgentHost::kTypePage)
    contents = host->GetWebContents();
  if (enabled)
    Watcher::Get().AddSession(channel, contents);
  else
    Watcher::Get().RemoveSession(channel);
  message.Set("result", base::DictValue());
  SendToDevToolsClient(channel, std::move(message));
}

void OnDownloadClientDetached(content::DevToolsAgentHostClientChannel* channel) {
  Watcher::Get().RemoveSession(channel);
}

const char* EffectiveDownloads(zebkit_view* view) {
  if (Requests::OrphanedPopup(view))
    return "deny";
  if (Requests::HasDecider(view))
    return "ask";
  return view->deny_downloads_.value_or(EngineDeniesDownloads()) ? "deny" : "save";
}

}  // namespace zebkit

extern "C" {
void zebkit_view_on_download(zebkit_view* view, zebkit_download_func function, void* data) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view);
  auto& requests = zebkit::Requests::Get();
  if (function && requests.Ready() && view->web_contents())
    zebkit::Watcher::Get().WatchProfileOf(view);
  requests.Install(view, function, data);
}
void zebkit_view_on_download_progress(zebkit_view* view, zebkit_download_progress_func function, void* data) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view);
  auto& requests = zebkit::Requests::Get();
  if (function && requests.Ready() && view->web_contents())
    zebkit::Watcher::Get().WatchProfileOf(view);
  requests.Observe(view, function, data);
}
bool zebkit_view_set_download_timeout(zebkit_view* view, uint32_t milliseconds) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view, false);
  return zebkit::Requests::Get().SetTimeout(view, milliseconds);
}
char* zebkit_download_request_info(zebkit_download_request* request) {
  return zebkit::Requests::Get().Info(reinterpret_cast<uintptr_t>(request));
}
bool zebkit_download_respond(zebkit_download_request* request, zebkit_download_action action,
                             const char* path, char** error) {
  auto fail = [&](const char* message) { if (error) *error = strdup(message); return false; };
  auto& requests = zebkit::Requests::Get();
  if (!requests.Ready()) return fail("download replies need the engine thread");
  const auto id = reinterpret_cast<uintptr_t>(request);
  if (!requests.Find(id)) return fail("the request is no longer pending");
  zebkit::Traits::Decision decision;
  if (action == ZEBKIT_DOWNLOAD_SAVE) {
    if (!path || !base::IsStringUTF8(path)) return fail("saving needs a path");
    decision.path = base::FilePath::FromUTF8Unsafe(path);
    if (!decision.path.IsAbsolute() || decision.path.ReferencesParent())
      return fail("the path must be absolute");
    decision.save = true;
  } else if (action != ZEBKIT_DOWNLOAD_CANCEL) {
    return fail("unknown action");
  } else if (path) {
    return fail("only saving takes a path");
  }
  requests.Settle(id, std::move(decision), "answered");
  return true;
}
bool zebkit_view_cancel_download(zebkit_view* view, const char* download_id) {
  if (!zebkit::Requests::Get().Ready() || !zebkit::LiveView(view) || !download_id) return false;
  return zebkit::Watcher::Get().Cancel(view, download_id);
}
}  // extern "C"
