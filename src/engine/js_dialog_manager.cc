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

namespace zebkit {

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
  std::erase_if(dialogs_, [](const auto& entry) { return !entry.second.contents; });
  if (dialogs_.contains(web_contents)) {
    // One dialog of a page at a time, as Chrome's.
    *did_suppress_message = true;
    return;
  }
  Shown& shown = dialogs_[web_contents];
  shown.contents = web_contents->GetWeakPtr();
  shown.dialog = std::make_unique<JavaScriptDialog>(
      this, web_contents->GetTopLevelNativeWindow(), dialog_type,
      url_formatter::FormatUrl(render_frame_host->GetLastCommittedOrigin().GetURL()), message_text,
      default_prompt_text, std::move(callback));
#else
  // Chromium's views dialog where the page has a window, as Chrome's on
  // Linux and Windows; without one the application's to show, through
  // <zebkit/page.h> (dialog.cc), or suppressed.
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
  std::erase_if(dialogs_, [](const auto& entry) { return !entry.second.contents; });
  if (dialogs_.contains(web_contents)) {
    std::move(callback).Run(true, std::u16string());
    return;
  }
  Shown& shown = dialogs_[web_contents];
  shown.contents = web_contents->GetWeakPtr();
  shown.dialog = std::make_unique<JavaScriptDialog>(
      this, web_contents->GetTopLevelNativeWindow(), JAVASCRIPT_DIALOG_TYPE_CONFIRM, std::u16string(),
      is_reload ? u"Reload this page? Changes you made may not be saved."
                : u"Leave this page? Changes you made may not be saved.",
      std::u16string(), std::move(callback));
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
  auto it = dialogs_.find(web_contents);
  if (it != dialogs_.end()) {
    std::unique_ptr<JavaScriptDialog> dialog = std::move(it->second.dialog);
    dialogs_.erase(it);
    if (dialog)
      dialog->Cancel();
  }
#else
  CloseViewsJavaScriptDialogs(web_contents);
#endif
}

#if BUILDFLAG(IS_MAC)
void JavaScriptDialogManagerImpl::DialogClosed(JavaScriptDialog* dialog) {
  std::erase_if(dialogs_, [dialog](const auto& entry) { return entry.second.dialog.get() == dialog; });
}
#endif

}  // namespace zebkit
