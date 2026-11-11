// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZEBKIT_ENGINE_PROFILE_H_
#define ZEBKIT_ENGINE_PROFILE_H_

#include <memory>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "content/public/browser/browser_context.h"

class SimpleFactoryKey;

namespace content {
class BackgroundFetchDelegate;
class BackgroundSyncController;
class BrowserPluginGuestManager;
class BrowsingDataRemoverDelegate;
class ClientHintsControllerDelegate;
class ContentIndexProvider;
class DownloadManagerDelegate;
class OriginTrialsControllerDelegate;
class PermissionControllerDelegate;
class PlatformNotificationService;
class PushMessagingService;
class ReduceAcceptLanguageControllerDelegate;
class SSLHostStateDelegate;
class StorageNotificationService;
class ZoomLevelDelegate;
}  // namespace content

namespace zebkit {

class EngineDownloadManagerDelegate;
class FileSystemAccessContext;
class NotificationService;

// The path key of the directory --user-data-dir says (PathService).
inline constexpr int kUserDataPath = 18600;
inline constexpr char kUserDataDirSwitch[] = "user-data-dir";
void RegisterUserDataPath();
// Before the engine starts: the directory --user-data-dir says made, or, when
// there is no home to have given one, a new private one in the temporary
// directory, deleted at the end (DeleteTemporaryUserData()). False with
// @error when the one given cannot be made.
bool PrepareUserDataDirectory(std::vector<std::string>* args, std::string* error);
// Deletes that private directory, at once, on this thread: when the start
// fails after PrepareUserDataDirectory(), and once Chromium has shut down
// (its threads stopped and child processes gone), not before.
void DeleteTemporaryUserData();

// A browser context: a profile, on disk or, off the record, in memory.
class Profile : public content::BrowserContext {
 public:
  explicit Profile(bool off_the_record);

  Profile(const Profile&) = delete;
  Profile& operator=(const Profile&) = delete;

  ~Profile() override;

  void set_client_hints_controller_delegate(
      content::ClientHintsControllerDelegate* delegate) {
    client_hints_controller_delegate_ = delegate;
  }

  // BrowserContext implementation.
  base::FilePath GetPath() const override;
  std::unique_ptr<content::ZoomLevelDelegate> CreateZoomLevelDelegate(
      const base::FilePath& partition_path) override;
  bool IsOffTheRecord() override;
  content::DownloadManagerDelegate* GetDownloadManagerDelegate() override;
  content::BrowserPluginGuestManager* GetGuestManager() override;
  storage::SpecialStoragePolicy* GetSpecialStoragePolicy() override;
  content::PlatformNotificationService* GetPlatformNotificationService() override;
  content::FileSystemAccessPermissionContext* GetFileSystemAccessPermissionContext() override;
  content::PushMessagingService* GetPushMessagingService() override;
  content::StorageNotificationService* GetStorageNotificationService() override;
  content::SSLHostStateDelegate* GetSSLHostStateDelegate() override;
  content::PermissionControllerDelegate* GetPermissionControllerDelegate() override;
  content::BackgroundFetchDelegate* GetBackgroundFetchDelegate() override;
  content::BackgroundSyncController* GetBackgroundSyncController() override;
  content::BrowsingDataRemoverDelegate* GetBrowsingDataRemoverDelegate() override;
  content::ContentIndexProvider* GetContentIndexProvider() override;
  content::ClientHintsControllerDelegate* GetClientHintsControllerDelegate() override;
  content::ReduceAcceptLanguageControllerDelegate*
  GetReduceAcceptLanguageControllerDelegate() override;
  content::OriginTrialsControllerDelegate* GetOriginTrialsControllerDelegate() override;

 protected:
  std::unique_ptr<EngineDownloadManagerDelegate> download_manager_delegate_;
  std::unique_ptr<content::PermissionControllerDelegate> permission_manager_;
  std::unique_ptr<FileSystemAccessContext> file_system_access_;
  std::unique_ptr<NotificationService> notifications_;
  std::unique_ptr<content::BackgroundSyncController> background_sync_controller_;
  std::unique_ptr<content::ContentIndexProvider> content_index_provider_;
  std::unique_ptr<content::ReduceAcceptLanguageControllerDelegate>
      reduce_accept_lang_controller_delegate_;
  std::unique_ptr<content::OriginTrialsControllerDelegate>
      origin_trials_controller_delegate_;

 private:
  const bool off_the_record_;
  base::FilePath path_;
  std::unique_ptr<SimpleFactoryKey> key_;
  raw_ptr<content::ClientHintsControllerDelegate> client_hints_controller_delegate_ =
      nullptr;
};

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_PROFILE_H_
