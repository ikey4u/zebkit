// File System Access for pages (docs/SYSTEM_INTEGRATION.md, 9): what a page
// may read and write through the handles its pickers and drops give it.
//
// The person's choice is the grant, as in Chrome: what was picked or dropped
// can be read, and where a page saves (showSaveFilePicker) can be written.
// Anything beyond that, writing to a file the page opened, or reading or
// writing a handle restored from storage, is the application's decision:
// the "file-system-write" and "file-system-read" permissions of the view's
// permission hook (permissions.h), denied without one. Grants last as long
// as the profile is loaded; nothing is kept on disk.

#ifndef ZEBKIT_ENGINE_FILE_SYSTEM_ACCESS_H_
#define ZEBKIT_ENGINE_FILE_SYSTEM_ACCESS_H_

#include <map>
#include <string>
#include <tuple>

#include "base/memory/weak_ptr.h"
#include "content/public/browser/file_system_access_permission_context.h"

namespace zebkit {

class FileSystemAccessContext : public content::FileSystemAccessPermissionContext {
 public:
  FileSystemAccessContext();
  ~FileSystemAccessContext() override;

  scoped_refptr<content::FileSystemAccessPermissionGrant> GetReadPermissionGrant(
      const url::Origin& origin, const content::PathInfo& path_info, HandleType handle_type,
      AccessTrigger access_trigger) override;
  scoped_refptr<content::FileSystemAccessPermissionGrant> GetWritePermissionGrant(
      const url::Origin& origin, const content::PathInfo& path_info, HandleType handle_type,
      AccessTrigger access_trigger) override;
  void ConfirmSensitiveEntryAccess(const url::Origin& origin, const content::PathInfo& path_info,
                                   HandleType handle_type, AccessTrigger access_trigger,
                                   content::GlobalRenderFrameHostId frame_id,
                                   base::OnceCallback<void(SensitiveEntryResult)> callback) override;
  void PerformAfterWriteChecks(std::unique_ptr<content::FileSystemAccessWriteItem> item,
                               content::GlobalRenderFrameHostId frame_id,
                               base::OnceCallback<void(AfterWriteCheckResult)> callback) override;
  bool IsFileTypeDangerous(const base::FilePath& path) override;
  base::expected<void, std::string> CanShowFilePicker(content::RenderFrameHost* rfh) override;
  bool CanObtainReadPermission(const url::Origin& origin) override;
  bool CanObtainWritePermission(const url::Origin& origin) override;
  void SetLastPickedDirectory(const url::Origin& origin, const std::string& id,
                              const content::PathInfo& path_info) override;
  content::PathInfo GetLastPickedDirectory(const url::Origin& origin, const std::string& id) override;
  base::FilePath GetWellKnownDirectoryPath(blink::mojom::WellKnownDirectory directory,
                                           const url::Origin& origin) override;
  std::u16string GetPickerTitle(const blink::mojom::FilePickerOptionsPtr& options) override;
  void NotifyEntryMoved(const url::Origin& origin, const content::PathInfo& old_path,
                        const content::PathInfo& new_path) override;
  void NotifyEntryModified(const url::Origin& origin, const content::PathInfo& path) override;
  void NotifyEntryRemoved(const url::Origin& origin, const content::PathInfo& path) override;
  void OnFileCreatedFromShowSaveFilePicker(const GURL& file_picker_binding_context,
                                           const storage::FileSystemURL& url) override;
  void CheckPathsAgainstEnterprisePolicy(std::vector<content::PathInfo> entries,
                                         content::GlobalRenderFrameHostId frame_id,
                                         EntriesAllowedByEnterprisePolicyCallback callback) override;

 private:
  class Grant;
  // origin, path, write: one grant each, shared by the handles that use it.
  using Key = std::tuple<url::Origin, base::FilePath, bool>;

  scoped_refptr<Grant> GrantFor(const url::Origin& origin, const content::PathInfo& path_info,
                                HandleType handle_type, bool write);

  std::map<Key, Grant*> grants_;
  std::map<std::pair<url::Origin, std::string>, content::PathInfo> last_picked_;
  base::WeakPtrFactory<FileSystemAccessContext> weak_factory_{this};
};

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_FILE_SYSTEM_ACCESS_H_
