// Downloads (<zebkit/hooks.h>): the target of a download asked of the
// application, as Chrome's DownloadTargetDeterminer waits for its answers.

#include <stdlib.h>
#include <string.h>

#include <map>
#include <set>
#include <string>
#include <utility>

#include "base/files/file_path.h"
#include "base/strings/string_util.h"
#include "components/download/public/common/download_item.h"
#include "components/download/public/common/download_target_info.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/download_item_utils.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/web_contents.h"
#include "zebkit/engine/download_manager.h"
#include "net/base/filename_util.h"
#include "url/gurl.h"
#include "zebkit/engine/hook_requests.h"
#include "zebkit/engine/page_hooks.h"
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

void Answer(download::DownloadTargetCallback callback, Traits::Decision decision) {
  download::DownloadTargetInfo target;
  if (decision.save) {
    target.target_path = decision.path;
    target.intermediate_path = decision.path.AddExtension(FILE_PATH_LITERAL(".crdownload"));
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
    auto [it, added] = items_.try_emplace(item->GetId());
    if (!added)
      return;
    it->second.item = item;
    it->second.view = view->GetWeakPtr();
    item->AddObserver(this);
    Tell(item, it->second);
  }
  void SetRequest(download::DownloadItem* item, uintptr_t request) {
    if (auto it = items_.find(item->GetId()); it != items_.end())
      it->second.request = request;
  }
  bool Cancel(zebkit_view* view, const std::string& id) {
    for (auto& [key, entry] : items_) {
      if (entry.view.get() == view && "d" + base::NumberToString(key) == id &&
          entry.item->GetState() == download::DownloadItem::IN_PROGRESS) {
        entry.item->Cancel(true);
        return true;
      }
    }
    return false;
  }
  void Stop() {
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
  void ManagerGoingDown(content::DownloadManager* manager) override {
    manager->RemoveObserver(this);
    managers_.erase(manager);
  }

  // download::DownloadItem::Observer
  void OnDownloadUpdated(download::DownloadItem* item) override {
    auto it = items_.find(item->GetId());
    if (it == items_.end())
      return;
    const uintptr_t request = it->second.request;
    if (request && item->GetState() != download::DownloadItem::IN_PROGRESS) {
      it->second.request = 0;
      Requests::Get().Settle(request, Traits::Refusal(), "cancelled");
      it = items_.find(item->GetId());
      if (it == items_.end())
        return;
    }
    Tell(item, it->second);
  }
  void OnDownloadDestroyed(download::DownloadItem* item) override {
    auto it = items_.find(item->GetId());
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
    if (!view || !view->on_download_progress_)
      return;
    const bool done = state != download::DownloadItem::IN_PROGRESS;
    base::DictValue event;
    event.Set("downloadId", "d" + base::NumberToString(item->GetId()));
    event.Set("url", Redacted(item->GetURL()));
    event.Set("state", state == download::DownloadItem::COMPLETE    ? "complete"
                       : state == download::DownloadItem::CANCELLED ? "cancelled"
                       : done                                       ? "interrupted"
                                                                    : "in-progress");
    event.Set("path", item->GetTargetFilePath().AsUTF8Unsafe());
    event.Set("receivedBytes", static_cast<double>(item->GetReceivedBytes()));
    event.Set("totalBytes", static_cast<double>(item->GetTotalBytes()));
    event.Set("reason", download::DownloadInterruptReasonToString(item->GetLastReason()));
    event.Set("done", done);
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](base::WeakPtr<zebkit_view> view, uint64_t generation, std::string json) {
              if (!view || view->download_generation_ != generation || !view->on_download_progress_)
                return;
              auto function = view->on_download_progress_;
              function(view.get(), json.c_str(), view->on_download_progress_data_);
            },
            entry.view, view->download_generation_, base::WriteJson(event).value_or("{}")));
  }

  bool used_ = false;
  std::set<content::DownloadManager*> managers_;
  std::map<uint32_t, Entry> items_;
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
  auto& requests = zebkit::Requests::Get();
  if (function && requests.Ready() && view && view->web_contents())
    zebkit::Watcher::Get().WatchProfileOf(view);
  requests.Install(view, function, data);
}
void zebkit_view_on_download_progress(zebkit_view* view, zebkit_download_progress_func function, void* data) {
  auto& requests = zebkit::Requests::Get();
  if (function && requests.Ready() && view && view->web_contents())
    zebkit::Watcher::Get().WatchProfileOf(view);
  requests.Observe(view, function, data);
}
bool zebkit_view_set_download_timeout(zebkit_view* view, uint32_t milliseconds) {
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
  if (!zebkit::Requests::Get().Ready() || !view || !download_id) return false;
  return zebkit::Watcher::Get().Cancel(view, download_id);
}
}  // extern "C"
