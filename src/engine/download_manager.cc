// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "zebkit/engine/download_manager.h"

#include <algorithm>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/notimplemented.h"
#include "build/build_config.h"
#include "components/download/public/common/download_target_info.h"

#include "base/check_op.h"
#include "base/command_line.h"
#include "base/compiler_specific.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/notreached.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/download_item_utils.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/web_contents.h"
#include "net/base/filename_util.h"

namespace zebkit {

using namespace content;

namespace {
EngineDownloadManagerDelegate::TargetHandler& TargetHandlerForEmbedder() {
  static base::NoDestructor<EngineDownloadManagerDelegate::TargetHandler> handler;
  return *handler;
}
}  // namespace

void EngineDownloadManagerDelegate::SetTargetHandler(TargetHandler handler) {
  TargetHandlerForEmbedder() = std::move(handler);
}

EngineDownloadManagerDelegate::EngineDownloadManagerDelegate()
    // saved without asking, the same on every system: only Windows has
    // the prompt, and elsewhere its absence cancels the download.
    : download_manager_(nullptr) {}

EngineDownloadManagerDelegate::~EngineDownloadManagerDelegate() {
  if (download_manager_) {
    download_manager_->SetDelegate(nullptr);
    download_manager_ = nullptr;
  }
}


void EngineDownloadManagerDelegate::SetDownloadManager(
    DownloadManager* download_manager) {
  download_manager_ = download_manager;
}

void EngineDownloadManagerDelegate::Shutdown() {
  // Revoke any pending callbacks. download_manager_ et. al. are no longer safe
  // to access after this point.
  weak_ptr_factory_.InvalidateWeakPtrs();
  download_manager_ = nullptr;
}

bool EngineDownloadManagerDelegate::DetermineDownloadTarget(
    download::DownloadItem* download,
    download::DownloadTargetCallback* callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  // This assignment needs to be here because even at the call to
  // SetDownloadManager, the system is not fully initialized.
  if (default_download_path_.empty()) {
    default_download_path_ = download_manager_->GetBrowserContext()
                                 ->GetPath()
#if BUILDFLAG(IS_CHROMEOS)
                                 .Append(FILE_PATH_LITERAL("MyFiles"))
#endif
                                 .Append(FILE_PATH_LITERAL("Downloads"))
                                 // Windows' move of a download (SHFileOperation)
                                 // takes no forward slashes, as a profile may have.
                                 .NormalizePathSeparators();
  }

  if (!download->GetForcedFilePath().empty()) {
    download::DownloadTargetInfo target_info;
    target_info.target_path = download->GetForcedFilePath();
    target_info.intermediate_path = download->GetForcedFilePath();

    std::move(*callback).Run(std::move(target_info));
    return true;
  }

  if (const TargetHandler& handler = TargetHandlerForEmbedder();
      handler && handler.Run(download, callback)) {
    return true;
  }

  FilenameDeterminedCallback filename_determined_callback = base::BindOnce(
      &EngineDownloadManagerDelegate::OnDownloadPathGenerated,
      weak_ptr_factory_.GetWeakPtr(), download->GetId(), std::move(*callback));

  base::ThreadPool::PostTask(
      FROM_HERE,
      {base::MayBlock(), base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN,
       base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&EngineDownloadManagerDelegate::GenerateFilename,
                     download->GetURL(), download->GetContentDisposition(),
                     download->GetSuggestedFilename(), download->GetMimeType(),
                     default_download_path_,
                     std::move(filename_determined_callback)));
  return true;
}

bool EngineDownloadManagerDelegate::ShouldOpenDownload(
    download::DownloadItem* item,
    DownloadOpenDelayedCallback callback) {
  return true;
}

void EngineDownloadManagerDelegate::GetNextId(DownloadIdCallback callback) {
  static uint32_t next_id = download::DownloadItem::kInvalidId + 1;
  std::move(callback).Run(next_id++);
}

// static
void EngineDownloadManagerDelegate::GenerateFilename(
    const GURL& url,
    const std::string& content_disposition,
    const std::string& suggested_filename,
    const std::string& mime_type,
    const base::FilePath& suggested_directory,
    FilenameDeterminedCallback callback) {
  base::FilePath generated_name = net::GenerateFileName(url,
                                                        content_disposition,
                                                        std::string(),
                                                        suggested_filename,
                                                        mime_type,
                                                        "download");

  if (!base::PathExists(suggested_directory))
    base::CreateDirectory(suggested_directory);

  base::FilePath suggested_path(suggested_directory.Append(generated_name));
  // what is already there is kept, as Chrome keeps it.
  base::FilePath unique_path = base::GetUniquePath(suggested_path);
  if (!unique_path.empty()) {
    suggested_path = unique_path;
  }
  GetUIThreadTaskRunner({})->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), suggested_path));
}

void EngineDownloadManagerDelegate::OnDownloadPathGenerated(
    uint32_t download_id,
    download::DownloadTargetCallback callback,
    const base::FilePath& suggested_path) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  // Saved where it was generated: asking where is the application's
  // (<zebkit/page.h>, download_hooks.cc).
  download::DownloadTargetInfo target_info;
  target_info.target_path = suggested_path;
  target_info.intermediate_path =
      suggested_path.AddExtension(FILE_PATH_LITERAL(".crdownload"));
  std::move(callback).Run(std::move(target_info));
}

void EngineDownloadManagerDelegate::SetDownloadDirectory(
    const base::FilePath& default_download_path) {
  default_download_path_ = default_download_path;
}

}  // namespace zebkit
