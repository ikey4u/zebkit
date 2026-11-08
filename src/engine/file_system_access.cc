#include "zebkit/engine/file_system_access.h"

#include <utility>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/path_service.h"
#include "base/values.h"
#include "content/public/browser/file_system_access_permission_grant.h"
#include "content/public/browser/file_system_access_write_item.h"
#include "content/public/browser/render_frame_host.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_manager.mojom.h"
#include "zebkit/engine/permissions.h"

namespace zebkit {

using Status = blink::mojom::PermissionStatus;
using Outcome = content::FileSystemAccessPermissionGrant::PermissionRequestOutcome;

class FileSystemAccessContext::Grant : public content::FileSystemAccessPermissionGrant {
 public:
  Grant(base::WeakPtr<FileSystemAccessContext> context, Key key, content::PathInfo path_info,
        HandleType handle_type, Status status)
      : context_(std::move(context)), key_(std::move(key)), path_info_(std::move(path_info)),
        handle_type_(handle_type), status_(status) {}

  Status GetStatus() override { return status_; }
  base::FilePath GetPath() override { return path_info_.path; }
  std::string GetDisplayName() override { return path_info_.display_name; }

  void RequestPermission(content::GlobalRenderFrameHostId frame_id, UserActivationState activation,
                         base::OnceCallback<void(Outcome)> callback) override {
    if (status_ == Status::GRANTED) {
      std::move(callback).Run(Outcome::kGrantedByContentSetting);
      return;
    }
    content::RenderFrameHost* frame = content::RenderFrameHost::FromID(frame_id);
    if (!frame || !frame->IsActive()) {
      std::move(callback).Run(Outcome::kInvalidFrame);
      return;
    }
    const bool gesture = frame->HasTransientUserActivation();
    if (activation == UserActivationState::kRequired && !gesture) {
      std::move(callback).Run(Outcome::kNoUserActivation);
      return;
    }
    base::DictValue details;
    details.Set("path", path_info_.path.AsUTF8Unsafe());
    details.Set("name", path_info_.display_name.empty() ? path_info_.path.BaseName().AsUTF8Unsafe()
                                                        : path_info_.display_name);
    details.Set("kind", handle_type_ == HandleType::kDirectory ? "directory" : "file");
    RequestNamedPermission(
        frame, std::get<2>(key_) ? "file-system-write" : "file-system-read", std::move(details), gesture,
        base::BindOnce(
            [](scoped_refptr<Grant> grant, base::OnceCallback<void(Outcome)> callback, NamedAnswer answer) {
              // Undecided (no hook, no answer in time) is a dismissed prompt:
              // refused now, asked again next time.
              if (answer == NamedAnswer::kUndecided) {
                std::move(callback).Run(Outcome::kUserDismissed);
                return;
              }
              const bool granted = answer == NamedAnswer::kGranted;
              grant->Set(granted ? Status::GRANTED : Status::DENIED);
              std::move(callback).Run(granted ? Outcome::kUserGranted : Outcome::kUserDenied);
            },
            scoped_refptr<Grant>(this), std::move(callback)));
  }

  void Set(Status status) {
    if (status_ == status)
      return;
    status_ = status;
    NotifyPermissionStatusChanged();
  }
  void Move(Key key, const content::PathInfo& path_info) {
    key_ = std::move(key);
    path_info_ = path_info;
  }

 private:
  ~Grant() override {
    if (!context_)
      return;
    auto it = context_->grants_.find(key_);
    if (it != context_->grants_.end() && it->second == this)
      context_->grants_.erase(it);
  }

  base::WeakPtr<FileSystemAccessContext> context_;
  Key key_;
  content::PathInfo path_info_;
  HandleType handle_type_;
  Status status_;
};

FileSystemAccessContext::FileSystemAccessContext() = default;
FileSystemAccessContext::~FileSystemAccessContext() = default;

scoped_refptr<FileSystemAccessContext::Grant> FileSystemAccessContext::GrantFor(
    const url::Origin& origin, const content::PathInfo& path_info, HandleType handle_type, bool write) {
  Key key{origin, path_info.path, write};
  if (auto it = grants_.find(key); it != grants_.end())
    return scoped_refptr<Grant>(it->second);
  // A directory granted covers what is inside it, as Chrome's does.
  for (auto& [other, grant] : grants_) {
    if (std::get<0>(other) == origin && std::get<2>(other) == write &&
        grant->GetStatus() == Status::GRANTED && std::get<1>(other).IsParent(path_info.path)) {
      auto inherited = base::MakeRefCounted<Grant>(weak_factory_.GetWeakPtr(), key, path_info, handle_type,
                                                   Status::GRANTED);
      grants_[key] = inherited.get();
      return inherited;
    }
  }
  auto grant = base::MakeRefCounted<Grant>(weak_factory_.GetWeakPtr(), key, path_info, handle_type, Status::ASK);
  grants_[key] = grant.get();
  return grant;
}

scoped_refptr<content::FileSystemAccessPermissionGrant> FileSystemAccessContext::GetReadPermissionGrant(
    const url::Origin& origin, const content::PathInfo& path_info, HandleType handle_type,
    AccessTrigger access_trigger) {
  auto grant = GrantFor(origin, path_info, handle_type, false);
  // Picked or dropped: the person chose it for this page.
  if (access_trigger == AccessTrigger::kOpen || access_trigger == AccessTrigger::kSave ||
      access_trigger == AccessTrigger::kDragAndDrop)
    grant->Set(Status::GRANTED);
  return grant;
}

scoped_refptr<content::FileSystemAccessPermissionGrant> FileSystemAccessContext::GetWritePermissionGrant(
    const url::Origin& origin, const content::PathInfo& path_info, HandleType handle_type,
    AccessTrigger access_trigger) {
  auto grant = GrantFor(origin, path_info, handle_type, true);
  // Where the person chose to save is where the page writes.
  if (access_trigger == AccessTrigger::kSave)
    grant->Set(Status::GRANTED);
  return grant;
}

void FileSystemAccessContext::ConfirmSensitiveEntryAccess(
    const url::Origin& origin, const content::PathInfo& path_info, HandleType handle_type,
    AccessTrigger access_trigger, content::GlobalRenderFrameHostId frame_id,
    base::OnceCallback<void(SensitiveEntryResult)> callback) {
  // What the person picked in a picker the application's hook decided, or in
  // the system's dialog: the application is the one to refuse paths.
  std::move(callback).Run(SensitiveEntryResult::kAllowed);
}

void FileSystemAccessContext::PerformAfterWriteChecks(
    std::unique_ptr<content::FileSystemAccessWriteItem> item, content::GlobalRenderFrameHostId frame_id,
    base::OnceCallback<void(AfterWriteCheckResult)> callback) {
  std::move(callback).Run(AfterWriteCheckResult::kAllow);
}

bool FileSystemAccessContext::IsFileTypeDangerous(const base::FilePath& path) {
  return false;
}

base::expected<void, std::string> FileSystemAccessContext::CanShowFilePicker(content::RenderFrameHost* rfh) {
  return base::ok();
}

bool FileSystemAccessContext::CanObtainReadPermission(const url::Origin& origin) {
  return true;
}

bool FileSystemAccessContext::CanObtainWritePermission(const url::Origin& origin) {
  return true;
}

void FileSystemAccessContext::SetLastPickedDirectory(const url::Origin& origin, const std::string& id,
                                                     const content::PathInfo& path_info) {
  if (last_picked_.size() < 1024 || last_picked_.contains({origin, id}))
    last_picked_[{origin, id}] = path_info;
}

content::PathInfo FileSystemAccessContext::GetLastPickedDirectory(const url::Origin& origin,
                                                                  const std::string& id) {
  auto it = last_picked_.find({origin, id});
  return it == last_picked_.end() ? content::PathInfo() : it->second;
}

base::FilePath FileSystemAccessContext::GetWellKnownDirectoryPath(blink::mojom::WellKnownDirectory directory,
                                                                  const url::Origin& origin) {
  // base knows the desktop on every system; the others are Chrome's own path
  // keys, so the picker starts in the home directory for them.
  const int key = directory == blink::mojom::WellKnownDirectory::kDirDesktop ? base::DIR_USER_DESKTOP
                                                                              : base::DIR_HOME;
  base::FilePath path;
  base::PathService::Get(key, &path);
  return path;
}

std::u16string FileSystemAccessContext::GetPickerTitle(const blink::mojom::FilePickerOptionsPtr& options) {
  return std::u16string();
}

void FileSystemAccessContext::NotifyEntryMoved(const url::Origin& origin, const content::PathInfo& old_path,
                                               const content::PathInfo& new_path) {
  for (bool write : {false, true}) {
    auto it = grants_.find({origin, old_path.path, write});
    if (it == grants_.end())
      continue;
    Grant* grant = it->second;
    grants_.erase(it);
    Key moved{origin, new_path.path, write};
    // A grant already there for the new path keeps it; this one keeps its own
    // entry out of the table.
    if (grants_.contains(moved))
      continue;
    grant->Move(moved, new_path);
    grants_[moved] = grant;
  }
}

void FileSystemAccessContext::NotifyEntryModified(const url::Origin& origin, const content::PathInfo& path) {}

void FileSystemAccessContext::NotifyEntryRemoved(const url::Origin& origin, const content::PathInfo& path) {}

void FileSystemAccessContext::OnFileCreatedFromShowSaveFilePicker(const GURL& file_picker_binding_context,
                                                                  const storage::FileSystemURL& url) {}

void FileSystemAccessContext::CheckPathsAgainstEnterprisePolicy(
    std::vector<content::PathInfo> entries, content::GlobalRenderFrameHostId frame_id,
    EntriesAllowedByEnterprisePolicyCallback callback) {
  std::move(callback).Run(std::move(entries));
}

}  // namespace zebkit
