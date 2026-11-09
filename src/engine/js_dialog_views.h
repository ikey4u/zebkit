#ifndef ZEBKIT_ENGINE_JS_DIALOG_VIEWS_H_
#define ZEBKIT_ENGINE_JS_DIALOG_VIEWS_H_

#include <string>

#include "content/public/browser/javascript_dialog_manager.h"

namespace content {
class WebContents;
}

namespace zebkit {

// A JavaScript dialog with Chromium's views, modal on @contents' window; it
// takes @callback. False, and @callback left to the caller, when it has no
// window of the system's to be modal on.
bool ShowViewsJavaScriptDialog(content::WebContents* contents, const std::u16string& title,
                               content::JavaScriptDialogType type, const std::u16string& message,
                               const std::u16string& default_prompt,
                               content::JavaScriptDialogManager::DialogClosedCallback& callback);

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_JS_DIALOG_VIEWS_H_
