// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZEBKIT_ENGINE_PROFILE_H_
#define ZEBKIT_ENGINE_PROFILE_H_

#include <memory>

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

// A browser context: a profile, on disk or, off the record, in memory.
class Profile : public content::BrowserContext {
 public:
  // If |delay_services_creation| is true, the owner is responsible for calling
  // CreateBrowserContextServices() for this BrowserContext.
  Profile(bool off_the_record,
                      bool delay_services_creation = false);

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
  bool ignore_certificate_errors() const { return ignore_certificate_errors_; }

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
  // Performs initialization of the Profile while IO is still
  // allowed on the current thread.
  void InitWhileIOAllowed();
  void FinishInitWhileIOAllowed();

  const bool off_the_record_;
  bool ignore_certificate_errors_ = false;
  base::FilePath path_;
  std::unique_ptr<SimpleFactoryKey> key_;
  raw_ptr<content::ClientHintsControllerDelegate> client_hints_controller_delegate_ =
      nullptr;
};

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_PROFILE_H_
