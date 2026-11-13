// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZEBKIT_ENGINE_DOWNLOAD_MANAGER_H_
#define ZEBKIT_ENGINE_DOWNLOAD_MANAGER_H_

#include "zebkit/engine/content_names.h"
#include <stdint.h>

#include "base/functional/callback_forward.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "components/download/public/common/download_item.h"
#include "components/download/public/common/download_target_info.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/download_manager_delegate.h"

namespace zebkit {

class EngineDownloadManagerDelegate : public content::DownloadManagerDelegate {
 public:
  EngineDownloadManagerDelegate();

  EngineDownloadManagerDelegate(const EngineDownloadManagerDelegate&) = delete;
  EngineDownloadManagerDelegate& operator=(const EngineDownloadManagerDelegate&) =
      delete;

  ~EngineDownloadManagerDelegate() override;

  void SetDownloadManager(content::DownloadManager* manager);

  void Shutdown() override;
  bool DetermineDownloadTarget(
      download::DownloadItem* download,
      download::DownloadTargetCallback* callback) override;
  bool ShouldOpenDownload(download::DownloadItem* item,
                          content::DownloadOpenDelayedCallback callback) override;
  void GetNextId(content::DownloadIdCallback callback) override;

  // zebkit: true when the embedder takes the download and runs the callback.
  using TargetHandler =
      base::RepeatingCallback<bool(download::DownloadItem*,
                                   download::DownloadTargetCallback*)>;
  static void SetTargetHandler(TargetHandler handler);

  // The directory downloads go to.
  void SetDownloadDirectory(const base::FilePath& default_download_path);

 private:
  using FilenameDeterminedCallback =
      base::OnceCallback<void(const base::FilePath&)>;

  static void GenerateFilename(const GURL& url,
                               const std::string& content_disposition,
                               const std::string& suggested_filename,
                               const std::string& mime_type,
                               const base::FilePath& suggested_directory,
                               FilenameDeterminedCallback callback);
  void OnDownloadPathGenerated(download::DownloadTargetCallback callback,
                               const base::FilePath& suggested_path);

  raw_ptr<content::DownloadManager> download_manager_;
  base::FilePath default_download_path_;
  base::WeakPtrFactory<EngineDownloadManagerDelegate> weak_ptr_factory_{this};
};

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_DOWNLOAD_MANAGER_H_
