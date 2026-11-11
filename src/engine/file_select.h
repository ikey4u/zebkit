// Copyright 2023 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZEBKIT_ENGINE_FILE_SELECT_H_
#define ZEBKIT_ENGINE_FILE_SELECT_H_

#include "zebkit/engine/content_names.h"
#include <memory>
#include <vector>

#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/file_select_listener.h"
#include "net/base/directory_lister.h"
#include "third_party/blink/public/mojom/choosers/file_chooser.mojom-forward.h"
#include "ui/shell_dialogs/select_file_dialog.h"

namespace content {
class RenderFrameHost;
class WebContents;
class FileSelectListener;
}  // namespace content

namespace ui {
struct SelectedFileInfo;
}

namespace zebkit {


// A page's file chooser, and a folder dropped on an <input webkitdirectory>
// listed: the system's dialog or the listing, then the page's answer. It
// lives on the UI thread, kept alive by itself until the answer.
class FileSelectHelper
    : public base::RefCountedThreadSafe<FileSelectHelper,
                                        BrowserThread::DeleteOnUIThread>,
      public ui::SelectFileDialog::Listener,
      private net::DirectoryLister::DirectoryListerDelegate {
 public:
  FileSelectHelper(const FileSelectHelper&) = delete;
  FileSelectHelper& operator=(const FileSelectHelper&) = delete;

  // Show the file chooser dialog.
  static void RunFileChooser(content::RenderFrameHost* render_frame_host,
                             scoped_refptr<FileSelectListener> listener,
                             const blink::mojom::FileChooserParams& params);

  // Lists the files under @path, a directory dropped on an <input type=file
  // webkitdirectory>, recursively, as a chosen folder.
  static void EnumerateDirectory(WebContents* web_contents,
                                 scoped_refptr<FileSelectListener> listener,
                                 const base::FilePath& path);

 private:
  friend class base::RefCountedThreadSafe<FileSelectHelper>;
  friend class base::DeleteHelper<FileSelectHelper>;
  friend struct content::BrowserThread::DeleteOnThread<BrowserThread::UI>;

  FileSelectHelper();
  ~FileSelectHelper() override;

  void RunFileChooser(content::RenderFrameHost* render_frame_host,
                      scoped_refptr<FileSelectListener> listener,
                      blink::mojom::FileChooserParamsPtr params);

  void EnumerateDirectoryImpl(WebContents* web_contents,
                              scoped_refptr<FileSelectListener> listener,
                              const base::FilePath& path);

  // Cleans up and releases this instance. This must be called after the last
  // callback is received from the file chooser dialog.
  void RunFileChooserEnd();

  // SelectFileDialog::Listener:
  void FileSelected(const ui::SelectedFileInfo& file, int index) override;
  void MultiFilesSelected(
      const std::vector<ui::SelectedFileInfo>& files) override;
  void FileSelectionCanceled() override;

  // Kicks off a new directory enumeration.
  void StartNewEnumeration(const base::FilePath& path);

  // net::DirectoryLister::DirectoryListerDelegate overrides.
  void OnListFile(
      const net::DirectoryLister::DirectoryListerData& data) override;
  void OnListDone(int error) override;

  // This method is called after the user has chosen the file(s) in the UI in
  // order to process and filter the list before returning the final result to
  // the caller.
  void ConvertToFileChooserFileInfoList(
      const std::vector<ui::SelectedFileInfo>& files);

  // The enumeration root directory for EnumerateDirectory() and
  // RunFileChooser with kUploadFolder.
  base::FilePath base_dir_;

  // Maintain an active directory enumeration.  These could come from the file
  // select dialog or from drag-and-drop of directories.  There could not be
  // more than one going on at a time.
  struct ActiveDirectoryEnumeration;
  std::unique_ptr<ActiveDirectoryEnumeration> directory_enumeration_;

  // A weak pointer to the WebContents of the RenderFrameHost, for life checks.
  base::WeakPtr<WebContents> web_contents_;

  // |listener_| receives the result of the FileSelectHelper.
  scoped_refptr<FileSelectListener> listener_;

  // Dialog box used for choosing files to upload from file form fields.
  scoped_refptr<ui::SelectFileDialog> select_file_dialog_;

  std::unique_ptr<ui::SelectFileDialog::FileTypeInfo> select_file_types_;

  // The type of file dialog last shown.
  ui::SelectFileDialog::Type dialog_type_ =
      ui::SelectFileDialog::SELECT_OPEN_FILE;

  // The mode of file dialog last shown.
  blink::mojom::FileChooserParams::Mode dialog_mode_ =
      blink::mojom::FileChooserParams::Mode::kOpen;
};

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_FILE_SELECT_H_
