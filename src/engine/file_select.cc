// Copyright 2023 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "zebkit/engine/file_select.h"

#include "base/functional/bind.h"
#include "base/memory/scoped_refptr.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "net/base/filename_util.h"
#include "net/base/mime_util.h"
#include "third_party/blink/public/mojom/choosers/file_chooser.mojom.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/shell_dialogs/select_file_policy.h"
#include "ui/shell_dialogs/selected_file_info.h"

namespace zebkit {

namespace {

// Helper function to get allowed extensions for select file dialog from
// the specified accept types as defined in the spec:
//   http://whatwg.org/html/number-state.html#attr-input-accept
// |accept_types| contains only valid lowercased MIME types or file extensions
// beginning with a period (.).
std::unique_ptr<ui::SelectFileDialog::FileTypeInfo> GetFileTypesFromAcceptType(
    const std::vector<std::u16string>& accept_types) {
  auto base_file_type = std::make_unique<ui::SelectFileDialog::FileTypeInfo>();
  if (accept_types.empty()) {
    return base_file_type;
  }

  // Create FileTypeInfo and pre-allocate for the first extension list.
  auto file_type =
      std::make_unique<ui::SelectFileDialog::FileTypeInfo>(*base_file_type);
  file_type->extensions.resize(1);
  std::vector<base::FilePath::StringType>* extensions =
      &file_type->extensions.back();

  // Find the corresponding extensions.
  size_t valid_type_count = 0;
  for (const auto& accept_type : accept_types) {
    size_t old_extension_size = extensions->size();
    if (accept_type[0] == '.') {
      // If the type starts with a period it is assumed to be a file extension
      // so we just have to add it to the list.
      base::FilePath::StringType ext =
          base::FilePath::FromUTF16Unsafe(accept_type).value();
      extensions->push_back(ext.substr(1));
    } else {
      if (!base::IsStringASCII(accept_type)) {
        continue;
      }
      std::string ascii_type = base::UTF16ToASCII(accept_type);
      net::GetExtensionsForMimeType(ascii_type, extensions);
    }

    if (extensions->size() > old_extension_size) {
      valid_type_count++;
    }
  }

  // If no valid extension is added, bail out.
  if (valid_type_count == 0) {
    return base_file_type;
  }

  return file_type;
}

}  // namespace

struct FileSelectHelper::ActiveDirectoryEnumeration {
  explicit ActiveDirectoryEnumeration(const base::FilePath& path)
      : path_(path) {}

  std::unique_ptr<net::DirectoryLister> lister_;
  const base::FilePath path_;
  std::vector<base::FilePath> results_;
  // More files than kMaxDirectoryFiles: the choice is cancelled at the end.
  bool too_many_ = false;
};

namespace {
// More files than this in a dropped or chosen folder: the choice is cancelled
// rather than handed to the page. It does not bound the listing's own memory:
// net::DirectoryLister reads the whole folder on a worker thread and then
// delivers every entry, and the end, in one task, so there is no stopping it
// part way; what it bounds is the copy kept here and what the page is sent.
constexpr size_t kMaxDirectoryFiles = 100000;
}  // namespace

// static
void FileSelectHelper::RunFileChooser(
    content::RenderFrameHost* render_frame_host,
    scoped_refptr<FileSelectListener> listener,
    const blink::mojom::FileChooserParams& params) {
  // FileSelectHelper will keep itself alive until it sends the result
  // message.
  scoped_refptr<FileSelectHelper> file_select_helper(
      new FileSelectHelper());
  file_select_helper->RunFileChooser(render_frame_host, std::move(listener),
                                     params.Clone());
}

// static
void FileSelectHelper::EnumerateDirectory(
    WebContents* web_contents,
    scoped_refptr<FileSelectListener> listener,
    const base::FilePath& path) {
  scoped_refptr<FileSelectHelper> file_select_helper(
      new FileSelectHelper());
  file_select_helper->EnumerateDirectoryImpl(web_contents, std::move(listener),
                                             path);
}

FileSelectHelper::FileSelectHelper() = default;

FileSelectHelper::~FileSelectHelper() {
  // There may be pending file dialogs, we need to tell them that we've gone
  // away so they don't try and call back to us.
  if (select_file_dialog_) {
    select_file_dialog_->ListenerDestroyed();
  }
}

void FileSelectHelper::RunFileChooser(
    content::RenderFrameHost* render_frame_host,
    scoped_refptr<FileSelectListener> listener,
    blink::mojom::FileChooserParamsPtr params) {
  DCHECK(!web_contents_);
  DCHECK(listener);
  DCHECK(!listener_);
  DCHECK(!select_file_dialog_);

  select_file_dialog_ = ui::SelectFileDialog::Create(this, nullptr);
  if (!select_file_dialog_) {
    listener->FileSelectionCanceled();
    return;
  }

  listener_ = std::move(listener);
  web_contents_ = content::WebContents::FromRenderFrameHost(render_frame_host)
                      ->GetWeakPtr();

  select_file_types_ = GetFileTypesFromAcceptType(params->accept_types);
  select_file_types_->allowed_paths =
      params->need_local_path ? ui::SelectFileDialog::FileTypeInfo::NATIVE_PATH
                              : ui::SelectFileDialog::FileTypeInfo::ANY_PATH;
  // 1-based index of default extension to show.
  int file_type_index =
      select_file_types_ && !select_file_types_->extensions.empty() ? 1 : 0;

  dialog_mode_ = params->mode;
  switch (params->mode) {
    case blink::mojom::FileChooserParams::Mode::kOpen:
      dialog_type_ = ui::SelectFileDialog::SELECT_OPEN_FILE;
      break;
    case blink::mojom::FileChooserParams::Mode::kOpenMultiple:
      dialog_type_ = ui::SelectFileDialog::SELECT_OPEN_MULTI_FILE;
      break;
    case blink::mojom::FileChooserParams::Mode::kUploadFolder:
      dialog_type_ = ui::SelectFileDialog::SELECT_UPLOAD_FOLDER;
      break;
    case blink::mojom::FileChooserParams::Mode::kSave:
      dialog_type_ = ui::SelectFileDialog::SELECT_SAVEAS_FILE;
      break;
    // showDirectoryPicker() (File System Access through the file chooser,
    // Window::UseFileChooserForFileSystemAccess): the directory itself.
    case blink::mojom::FileChooserParams::Mode::kOpenDirectory:
      dialog_type_ = ui::SelectFileDialog::SELECT_FOLDER;
      break;
    default:
      // Prevent warning.
      dialog_type_ = ui::SelectFileDialog::SELECT_OPEN_FILE;
      NOTREACHED();
  }

  gfx::NativeWindow owning_window = web_contents_->GetTopLevelNativeWindow();
  select_file_dialog_->SelectFile(dialog_type_, params->title,
                                  params->default_file_name, select_file_types_.get(),
                                  file_type_index, base::FilePath::StringType(),
                                  owning_window, nullptr);

  // Because this class returns notifications to the RenderViewHost, it is
  // difficult for callers to know how long to keep a reference to this
  // instance. We AddRef() here to keep the instance alive after we return
  // to the caller, until the last callback is received from the file dialog.
  // At that point, we must call RunFileChooserEnd().
  AddRef();
}

void FileSelectHelper::EnumerateDirectoryImpl(
    WebContents* web_contents,
    scoped_refptr<FileSelectListener> listener,
    const base::FilePath& path) {
  DCHECK(listener);
  DCHECK(!listener_);
  listener_ = std::move(listener);
  web_contents_ = web_contents->GetWeakPtr();
  // Kept alive until the listing is done, as for a dialog; released by
  // RunFileChooserEnd().
  AddRef();
  StartNewEnumeration(path);
}

void FileSelectHelper::RunFileChooserEnd() {
  if (listener_) {
    listener_->FileSelectionCanceled();
  }

  // A directory listed for a drop had no dialog.
  if (select_file_dialog_) {
    select_file_dialog_->ListenerDestroyed();
    select_file_dialog_.reset();
  }
  Release();
}

void FileSelectHelper::FileSelected(const ui::SelectedFileInfo& file,
                                         int index) {
  if (dialog_type_ == ui::SelectFileDialog::SELECT_UPLOAD_FOLDER) {
    StartNewEnumeration(file.local_path);
    return;
  }
  ConvertToFileChooserFileInfoList({file});
}

void FileSelectHelper::MultiFilesSelected(
    const std::vector<ui::SelectedFileInfo>& files) {
  ConvertToFileChooserFileInfoList(files);
}

void FileSelectHelper::FileSelectionCanceled() {
  RunFileChooserEnd();
}

void FileSelectHelper::StartNewEnumeration(const base::FilePath& path) {
  base_dir_ = path;
  auto entry = std::make_unique<ActiveDirectoryEnumeration>(path);
  entry->lister_ = base::WrapUnique(new net::DirectoryLister(
      path, net::DirectoryLister::NO_SORT_RECURSIVE, this));
  entry->lister_->Start();
  directory_enumeration_ = std::move(entry);
}

void FileSelectHelper::OnListFile(
    const net::DirectoryLister::DirectoryListerData& data) {
  // The page gone, or more files than a page is given: the rest is not kept,
  // and OnListDone(), which comes in the same task, cancels the choice.
  if (!directory_enumeration_ || directory_enumeration_->too_many_ || !web_contents_)
    return;
  if (directory_enumeration_->results_.size() >= kMaxDirectoryFiles) {
    directory_enumeration_->too_many_ = true;
    directory_enumeration_->results_.clear();
    return;
  }
  // Directory upload only cares about files.
  if (data.info.IsDirectory()) {
    return;
  }

  directory_enumeration_->results_.push_back(data.path);
}

void FileSelectHelper::OnListDone(int error) {
  if (!web_contents_) {
    // Web contents was destroyed under us (probably by closing the tab). We
    // must notify |listener_| and release our reference to
    // ourself. RunFileChooserEnd() performs this.
    RunFileChooserEnd();
    return;
  }

  // This entry needs to be cleaned up when this function is done.
  std::unique_ptr<ActiveDirectoryEnumeration> entry =
      std::move(directory_enumeration_);
  if (error || !entry || entry->too_many_) {
    FileSelectionCanceled();
    return;
  }

  std::vector<blink::mojom::FileChooserFileInfoPtr> chooser_files;
  for (const auto& file_path : entry->results_) {
    chooser_files.push_back(blink::mojom::FileChooserFileInfo::NewNativeFile(
        blink::mojom::NativeFileInfo::New(file_path, std::u16string(),
                                          std::vector<std::u16string>())));
  }

  listener_->FileSelected(std::move(chooser_files), base_dir_,
                          blink::mojom::FileChooserParams::Mode::kUploadFolder);
  listener_.reset();
  // No members should be accessed from here on.
  RunFileChooserEnd();
}

void FileSelectHelper::ConvertToFileChooserFileInfoList(
    const std::vector<ui::SelectedFileInfo>& files) {
  if (!web_contents_) {
    RunFileChooserEnd();
    return;
  }

  std::vector<blink::mojom::FileChooserFileInfoPtr> chooser_files;
  for (const auto& file : files) {
    chooser_files.push_back(blink::mojom::FileChooserFileInfo::NewNativeFile(
        blink::mojom::NativeFileInfo::New(
            file.local_path, base::FilePath(file.display_name).AsUTF16Unsafe(),
            std::vector<std::u16string>())));
  }

  listener_->FileSelected(std::move(chooser_files), base::FilePath(),
                          dialog_mode_);
  listener_ = nullptr;

  // No members should be accessed from here on.
  RunFileChooserEnd();
}

}  // namespace zebkit
