// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "zebkit/engine/js_dialog_manager.h"

#include <memory>

#include "base/command_line.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"
#include "components/url_formatter/url_formatter.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/web_contents.h"
#include "zebkit/engine/js_dialog.h"
#if !BUILDFLAG(IS_MAC)
#include "zebkit/engine/js_dialog_views.h"
#endif

#if BUILDFLAG(IS_WIN)
#include <windows.h>

#include "base/memory/weak_ptr.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/current_thread.h"
#include "base/task/single_thread_task_runner.h"
#include "content/public/browser/web_contents.h"
#include "ui/aura/window.h"
#include "ui/aura/window_tree_host.h"
#endif

namespace zebkit {

#if BUILDFLAG(IS_WIN)
namespace {

// A message box, which needs no resources in the application's executable. Out
// of the page's call, and with the engine's tasks running under it. One at a time.
bool g_message_box_open = false;

void ShowMessageBox(base::WeakPtr<content::WebContents> contents,
                    std::wstring text,
                    unsigned buttons,
                    JavaScriptDialogManager::DialogClosedCallback callback) {
  if (!contents) {
    std::move(callback).Run(false, std::u16string());
    return;
  }
  g_message_box_open = true;
  aura::Window* window = contents->GetTopLevelNativeWindow();
  HWND owner = window && window->GetHost() ? window->GetHost()->GetAcceleratedWidget()
                                           : nullptr;
  int answer;
  {
    base::CurrentThread::ScopedAllowApplicationTasksInNativeNestedLoop allow;
    answer = ::MessageBoxW(owner, text.c_str(), L"", buttons | MB_TASKMODAL);
  }
  g_message_box_open = false;
  std::move(callback).Run(answer == IDOK, std::u16string());
}

}  // namespace
#endif  // BUILDFLAG(IS_WIN)

JavaScriptDialogManagerImpl::JavaScriptDialogManagerImpl() = default;
JavaScriptDialogManagerImpl::~JavaScriptDialogManagerImpl() = default;

void JavaScriptDialogManagerImpl::RunJavaScriptDialog(
    WebContents* web_contents,
    RenderFrameHost* render_frame_host,
    JavaScriptDialogType dialog_type,
    const std::u16string& message_text,
    const std::u16string& default_prompt_text,
    DialogClosedCallback callback,
    bool* did_suppress_message) {
#if BUILDFLAG(IS_MAC)
  *did_suppress_message = false;
  if (dialog_) {
    // One dialog at a time, please.
    *did_suppress_message = true;
    return;
  }
  std::u16string new_message_text =
      url_formatter::FormatUrl(
          render_frame_host->GetLastCommittedOrigin().GetURL()) +
      u"\n\n" + message_text;
  gfx::NativeWindow parent_window = web_contents->GetTopLevelNativeWindow();
  dialog_ = std::make_unique<JavaScriptDialog>(
      this, parent_window, dialog_type, new_message_text, default_prompt_text,
      std::move(callback));
  dialog_contents_ = web_contents->GetWeakPtr();
#elif BUILDFLAG(IS_WIN)
  *did_suppress_message = g_message_box_open;
  if (g_message_box_open)
    return;
  if (dialog_type == JAVASCRIPT_DIALOG_TYPE_PROMPT) {
    // A message box has no text field: Chromium's views dialog, as on Linux;
    // without a window, null, as when it is cancelled.
    if (!ShowViewsJavaScriptDialog(web_contents,
                                   url_formatter::FormatUrl(render_frame_host->GetLastCommittedOrigin().GetURL()),
                                   dialog_type, message_text, default_prompt_text, callback))
      std::move(callback).Run(false, std::u16string());
    return;
  }
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&ShowMessageBox, web_contents->GetWeakPtr(),
                     base::UTF16ToWide(url_formatter::FormatUrl(
                                           render_frame_host->GetLastCommittedOrigin().GetURL()) +
                                       u"\n\n" + message_text),
                     dialog_type == JAVASCRIPT_DIALOG_TYPE_CONFIRM ? MB_OKCANCEL : MB_OK,
                     std::move(callback)));
#else
  // Chromium's views dialog where the page has a window, as Chrome's on
  // Linux; without one the application's to show, through <zebkit/page.h>
  // (dialog.cc), or suppressed.
  *did_suppress_message = !ShowViewsJavaScriptDialog(
      web_contents, url_formatter::FormatUrl(render_frame_host->GetLastCommittedOrigin().GetURL()), dialog_type,
      message_text, default_prompt_text, callback);
#endif
}

void JavaScriptDialogManagerImpl::RunBeforeUnloadDialog(
    WebContents* web_contents,
    RenderFrameHost* render_frame_host,
    bool is_reload,
    DialogClosedCallback callback) {
#if BUILDFLAG(IS_MAC)
  if (dialog_) {
    std::move(callback).Run(true, std::u16string());
    return;
  }
  gfx::NativeWindow parent_window = web_contents->GetTopLevelNativeWindow();
  dialog_ = std::make_unique<JavaScriptDialog>(
      this, parent_window, JAVASCRIPT_DIALOG_TYPE_CONFIRM,
      is_reload ? u"Reload this page? Changes you made may not be saved."
                : u"Leave this page? Changes you made may not be saved.",
      std::u16string(),
      std::move(callback));
  dialog_contents_ = web_contents->GetWeakPtr();
#elif BUILDFLAG(IS_WIN)
  if (g_message_box_open) {
    std::move(callback).Run(true, std::u16string());
    return;
  }
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&ShowMessageBox, web_contents->GetWeakPtr(),
                                is_reload ? L"Reload this page? Changes you made may not be saved."
                                          : L"Leave this page? Changes you made may not be saved.",
                                MB_OKCANCEL,
                                std::move(callback)));
#else
  // "Leave this page?" in Chromium's views dialog where there is a window.
  if (!ShowViewsJavaScriptDialog(web_contents, std::u16string(), JAVASCRIPT_DIALOG_TYPE_CONFIRM,
                                 is_reload ? u"Reload this page? Changes you made may not be saved."
                                           : u"Leave this page? Changes you made may not be saved.",
                                 std::u16string(), callback))
    std::move(callback).Run(true, std::u16string());
#endif
}

void JavaScriptDialogManagerImpl::CancelDialogs(WebContents* web_contents,
                                                bool reset_state) {
#if BUILDFLAG(IS_MAC)
  // Only the page's own: one view's navigation does not end another's alert.
  if (dialog_ && dialog_contents_.get() == web_contents) {
    dialog_->Cancel();
    dialog_.reset();
  }
#else
  CloseViewsJavaScriptDialogs(web_contents);
#endif
}

#if BUILDFLAG(IS_MAC)
void JavaScriptDialogManagerImpl::DialogClosed(JavaScriptDialog* dialog) {
  DCHECK_EQ(dialog, dialog_.get());
  dialog_.reset();
}
#endif

}  // namespace zebkit
