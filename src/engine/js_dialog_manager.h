// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZEBKIT_ENGINE_JS_DIALOG_MANAGER_H_
#define ZEBKIT_ENGINE_JS_DIALOG_MANAGER_H_

#include "zebkit/engine/content_names.h"
#include <memory>

#include "base/functional/callback.h"
#include "build/build_config.h"
#include "content/public/browser/javascript_dialog_manager.h"

namespace zebkit {

class JavaScriptDialog;

class JavaScriptDialogManagerImpl : public JavaScriptDialogManager {
 public:
  JavaScriptDialogManagerImpl();

  JavaScriptDialogManagerImpl(const JavaScriptDialogManagerImpl&) = delete;
  JavaScriptDialogManagerImpl& operator=(const JavaScriptDialogManagerImpl&) =
      delete;

  ~JavaScriptDialogManagerImpl() override;

  // JavaScriptDialogManager:
  void RunJavaScriptDialog(WebContents* web_contents,
                           RenderFrameHost* render_frame_host,
                           JavaScriptDialogType dialog_type,
                           const std::u16string& message_text,
                           const std::u16string& default_prompt_text,
                           DialogClosedCallback callback,
                           bool* did_suppress_message) override;

  void RunBeforeUnloadDialog(WebContents* web_contents,
                             RenderFrameHost* render_frame_host,
                             bool is_reload,
                             DialogClosedCallback callback) override;

  void CancelDialogs(WebContents* web_contents,
                     bool reset_state) override;

#if BUILDFLAG(IS_MAC)
  // Called by the JavaScriptDialog when it closes.
  void DialogClosed(JavaScriptDialog* dialog);

 private:
  std::unique_ptr<JavaScriptDialog> dialog_;
#endif
};

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_JS_DIALOG_MANAGER_H_
