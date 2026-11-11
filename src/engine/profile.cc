// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "zebkit/engine/profile.h"
#include "zebkit/engine/file_system_access.h"
#include "zebkit/engine/notifications.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include <utility>

#include "base/command_line.h"
#include "base/environment.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/no_destructor.h"
#include "base/task/thread_pool.h"
#include "base/path_service.h"
#include "base/logging.h"
#include "base/threading/scoped_blocking_call.h"
#include "base/threading/platform_thread.h"
#include "base/threading/thread_restrictions.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "components/keyed_service/content/browser_context_dependency_manager.h"
#include "components/keyed_service/core/simple_dependency_manager.h"
#include "components/keyed_service/core/simple_factory_key.h"
#include "components/keyed_service/core/simple_key_map.h"
#include "components/network_session_configurator/common/network_switches.h"
#include "components/origin_trials/browser/leveldb_persistence_provider.h"
#include "components/origin_trials/browser/origin_trials.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/origin_trials_controller_delegate.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/common/content_switches.h"
#include "zebkit/engine/content_index_provider.h"
#include "zebkit/engine/download_manager.h"
#include "zebkit/engine/permission_manager.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/background_sync_controller.h"
#include "zebkit/engine/accept_language_delegate.h"
#include "third_party/blink/public/common/origin_trials/trial_token_validator.h"

namespace zebkit {

using namespace content;

Profile::Profile(bool off_the_record) : off_the_record_(off_the_record) {
  // As Chrome's Profile: a context closed before may have had this address, which
  // the dependency manager remembers as dead.
  BrowserContextDependencyManager::GetInstance()->MarkBrowserContextLive(this);
  CHECK(base::PathService::Get(kUserDataPath, &path_));
  key_ = std::make_unique<SimpleFactoryKey>(path_, off_the_record_);
  SimpleDependencyManager::GetInstance()->MarkContextLive(key_.get());
  SimpleKeyMap::GetInstance()->Associate(this, key_.get());
#if BUILDFLAG(IS_WIN)
  base::SetExtraNoExecuteAllowedPath(kUserDataPath);
#endif  // BUILDFLAG(IS_WIN)
  BrowserContextDependencyManager::GetInstance()->CreateBrowserContextServices(this);
}

Profile::~Profile() {
  NotifyWillBeDestroyed();

  // The SimpleDependencyManager should always be passed after the
  // BrowserContextDependencyManager. This is because the KeyedService instances
  // in the BrowserContextDependencyManager's dependency graph can depend on the
  // ones in the SimpleDependencyManager's graph.
  DependencyManager::PerformInterlockedTwoPhaseShutdown(
      BrowserContextDependencyManager::GetInstance(), this,
      SimpleDependencyManager::GetInstance(), key_.get());

  SimpleKeyMap::GetInstance()->Dissociate(this);

  ShutdownStoragePartitions();
}

std::unique_ptr<ZoomLevelDelegate> Profile::CreateZoomLevelDelegate(
    const base::FilePath&) {
  return nullptr;
}

base::FilePath Profile::GetPath() const {
  return path_;
}

bool Profile::IsOffTheRecord() {
  return off_the_record_;
}

DownloadManagerDelegate* Profile::GetDownloadManagerDelegate()  {
  if (!download_manager_delegate_.get()) {
    download_manager_delegate_ =
        std::make_unique<EngineDownloadManagerDelegate>();
    download_manager_delegate_->SetDownloadManager(GetDownloadManager());
  }

  return download_manager_delegate_.get();
}

BrowserPluginGuestManager* Profile::GetGuestManager() {
  return nullptr;
}

storage::SpecialStoragePolicy* Profile::GetSpecialStoragePolicy() {
  return nullptr;
}

PlatformNotificationService*
Profile::GetPlatformNotificationService() {
  // Web notifications, once permitted (notifications.h).
  if (!notifications_)
    notifications_ = std::make_unique<NotificationService>(this);
  return notifications_.get();
}

// What File System Access handles may do (file_system_access.h).
content::FileSystemAccessPermissionContext* Profile::GetFileSystemAccessPermissionContext() {
  if (!file_system_access_)
    file_system_access_ = std::make_unique<FileSystemAccessContext>();
  return file_system_access_.get();
}

PushMessagingService* Profile::GetPushMessagingService() {
  return nullptr;
}

StorageNotificationService*
Profile::GetStorageNotificationService() {
  return nullptr;
}

SSLHostStateDelegate* Profile::GetSSLHostStateDelegate() {
  return nullptr;
}

PermissionControllerDelegate*
Profile::GetPermissionControllerDelegate() {
  if (!permission_manager_.get())
    permission_manager_ = std::make_unique<PermissionManager>();
  return permission_manager_.get();
}

ClientHintsControllerDelegate*
Profile::GetClientHintsControllerDelegate() {
  return client_hints_controller_delegate_;
}

BackgroundFetchDelegate* Profile::GetBackgroundFetchDelegate() {
  return nullptr;
}

BackgroundSyncController* Profile::GetBackgroundSyncController() {
  if (!background_sync_controller_) {
    background_sync_controller_ =
        std::make_unique<BackgroundSyncControllerImpl>();
  }
  return background_sync_controller_.get();
}

BrowsingDataRemoverDelegate*
Profile::GetBrowsingDataRemoverDelegate() {
  return nullptr;
}

ContentIndexProvider* Profile::GetContentIndexProvider() {
  if (!content_index_provider_)
    content_index_provider_ = std::make_unique<ContentIndexProviderImpl>();
  return content_index_provider_.get();
}

ReduceAcceptLanguageControllerDelegate*
Profile::GetReduceAcceptLanguageControllerDelegate() {
  if (!reduce_accept_lang_controller_delegate_) {
    reduce_accept_lang_controller_delegate_ =
        std::make_unique<AcceptLanguageDelegate>(
            AcceptLanguages());
  }
  return reduce_accept_lang_controller_delegate_.get();
}

OriginTrialsControllerDelegate*
Profile::GetOriginTrialsControllerDelegate() {
  if (!origin_trials_controller_delegate_) {
    origin_trials_controller_delegate_ =
        std::make_unique<origin_trials::OriginTrials>(
            std::make_unique<origin_trials::LevelDbPersistenceProvider>(
                GetPath(),
                GetDefaultStoragePartition()->GetProtoDatabaseProvider()),
            std::make_unique<blink::TrialTokenValidator>());
  }
  return origin_trials_controller_delegate_.get();
}

}  // namespace zebkit

namespace zebkit {

namespace {

// The private directory made when there was no home to give one.
base::FilePath& TemporaryUserData() {
  static base::NoDestructor<base::FilePath> path;
  return *path;
}

}  // namespace

bool PrepareUserDataDirectory(std::vector<std::string>* args, std::string* error) {
  // The last --user-data-dir before "--" is the one Chromium takes.
  const std::string prefix = std::string("--") + kUserDataDirSwitch + "=";
  std::string given;
  bool found = false;
  for (const std::string& arg : *args) {
    if (arg == "--")
      break;
    if (arg.starts_with(prefix)) {
      given = arg.substr(prefix.size());
      found = true;
    }
  }
  if (found && !given.empty()) {
    const base::FilePath path = PathFromUTF8(given);
    if (!base::DirectoryExists(path) && !base::CreateDirectory(path)) {
      *error = "The profile directory could not be created: " + given;
      return false;
    }
    return true;
  }
  // No home to find one in: for a run of its own, not kept. A new directory
  // only the user can read, not a fixed name another user could make first.
  base::FilePath path;
  if (!base::CreateNewTempDirectory(FILE_PATH_LITERAL("zebkit-profile-"), &path)) {
    *error = "No profile directory was given and none could be made";
    return false;
  }
  TemporaryUserData() = path;
  auto at = std::find(args->begin(), args->end(), "--");
  args->insert(at, prefix + PathToUTF8(path));
  return true;
}

void DeleteTemporaryUserData() {
  if (TemporaryUserData().empty())
    return;
  const base::FilePath path = TemporaryUserData();
  TemporaryUserData().clear();
  // Best effort, on the calling thread: the thread pool has stopped. A child
  // process still exiting may hold a file a little longer (on Windows that
  // refuses the delete), so a few more tries, a second in all.
  base::ScopedAllowBlockingForTesting allow_blocking;
  for (int attempt = 0; !base::DeletePathRecursively(path) && attempt < 10; ++attempt)
    base::PlatformThread::Sleep(base::Milliseconds(100));
}

void RegisterUserDataPath() {
  base::PathService::RegisterProvider(
      [](int key, base::FilePath* result) {
        if (key != kUserDataPath)
          return false;
        const base::CommandLine& command = *base::CommandLine::ForCurrentProcess();
        // --user-data-dir, which EngineArguments() always gives when it can.
        base::FilePath path = command.GetSwitchValuePath(kUserDataDirSwitch);
        if (!path.empty()) {
          if (base::DirectoryExists(path) || base::CreateDirectory(path)) {
            if (!path.IsAbsolute())
              path = base::MakeAbsoluteFilePath(path);
            if (!path.empty()) {
              *result = path;
              return true;
            }
          }
        }
        // PrepareUserDataDirectory() gave one, or the engine does not start.
        LOG(ERROR) << "The profile directory could not be made: " << path.value();
        return false;
      },
      kUserDataPath, kUserDataPath + 1);
}

}  // namespace zebkit
