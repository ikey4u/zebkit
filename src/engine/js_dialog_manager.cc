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
#else
  // The application's to show, through <zebkit/page.h> (dialog.cc).
  *did_suppress_message = true;
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
      u"Is it OK to leave/reload this page?", std::u16string(),
      std::move(callback));
#else
  std::move(callback).Run(true, std::u16string());
#endif
}

void JavaScriptDialogManagerImpl::CancelDialogs(WebContents* web_contents,
                                                bool reset_state) {
#if BUILDFLAG(IS_MAC)
  if (dialog_) {
    dialog_->Cancel();
    dialog_.reset();
  }
#endif
}

#if BUILDFLAG(IS_MAC)
void JavaScriptDialogManagerImpl::DialogClosed(JavaScriptDialog* dialog) {
  DCHECK_EQ(dialog, dialog_.get());
  dialog_.reset();
}
#endif

}  // namespace zebkit
