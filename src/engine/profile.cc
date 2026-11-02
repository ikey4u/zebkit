// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "zebkit/engine/profile.h"

#include <memory>
#include <utility>

#include "base/command_line.h"
#include "base/environment.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/path_service.h"
#include "base/logging.h"
#include "base/threading/scoped_blocking_call.h"
#include "base/threading/thread_restrictions.h"
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

Profile::Profile(bool off_the_record,
                                         bool delay_services_creation)
    : off_the_record_(off_the_record) {
  // As Chrome's Profile: a context closed before may have had this address, which
  // the dependency manager remembers as dead.
  BrowserContextDependencyManager::GetInstance()->MarkBrowserContextLive(this);
  InitWhileIOAllowed();
#if BUILDFLAG(IS_WIN)
  base::SetExtraNoExecuteAllowedPath(kUserDataPath);
#endif  // BUILDFLAG(IS_WIN)

  if (!delay_services_creation) {
    BrowserContextDependencyManager::GetInstance()
        ->CreateBrowserContextServices(this);
  }
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

void Profile::InitWhileIOAllowed() {
  base::CommandLine* cmd_line = base::CommandLine::ForCurrentProcess();
  if (cmd_line->HasSwitch(switches::kIgnoreCertificateErrors))
    ignore_certificate_errors_ = true;

  CHECK(base::PathService::Get(kUserDataPath, &path_));

  FinishInitWhileIOAllowed();
}

void Profile::FinishInitWhileIOAllowed() {
  key_ = std::make_unique<SimpleFactoryKey>(path_, off_the_record_);
  SimpleDependencyManager::GetInstance()->MarkContextLive(key_.get());
  SimpleKeyMap::GetInstance()->Associate(this, key_.get());
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
  return nullptr;
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
          } else {
            LOG(WARNING) << "Unable to create the user data directory: "
                         << path.value();
          }
        }
        // No home to find one in: for a run of its own, not kept.
        if (!base::GetTempDir(result))
          return false;
        *result = result->AppendASCII("zebkit-profile");
        return base::CreateDirectory(*result);
      },
      kUserDataPath, kUserDataPath + 1);
}

}  // namespace zebkit
