// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZEBKIT_ENGINE_JS_DIALOG_H_
#define ZEBKIT_ENGINE_JS_DIALOG_H_

#include "zebkit/engine/content_names.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "build/build_config.h"
#include "content/public/browser/javascript_dialog_manager.h"
#include "ui/gfx/native_ui_types.h"

#if BUILDFLAG(IS_MAC)
#if __OBJC__
@class ZebkitJavaScriptDialogHelper;
#else
class ZebkitJavaScriptDialogHelper;
#endif  // __OBJC__
#endif  // BUILDFLAG(IS_MAC)

namespace zebkit {

class JavaScriptDialogManagerImpl;

class JavaScriptDialog {
 public:
  // A sheet on @parent_window (app-modal without one), titled @title (the
  // page's origin, as the views dialog's title; empty for "leave this page?").
  JavaScriptDialog(JavaScriptDialogManagerImpl* manager,
                        gfx::NativeWindow parent_window,
                        JavaScriptDialogType dialog_type,
                        const std::u16string& title,
                        const std::u16string& message_text,
                        const std::u16string& default_prompt_text,
                        JavaScriptDialogManager::DialogClosedCallback callback);

  JavaScriptDialog(const JavaScriptDialog&) = delete;
  JavaScriptDialog& operator=(const JavaScriptDialog&) = delete;

  ~JavaScriptDialog();

  // Called to cancel a dialog mid-flight.
  void Cancel();

 private:
#if BUILDFLAG(IS_MAC)
  // This field is not a raw_ptr<> because it is a pointer to Objective-C
  // object.
  RAW_PTR_EXCLUSION ZebkitJavaScriptDialogHelper* helper_;  // owned
#endif
};

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_JS_DIALOG_H_
