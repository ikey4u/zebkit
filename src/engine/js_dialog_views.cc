// A page's alert(), confirm(), prompt() and "leave this page?" with Chromium's
// views (docs/SYSTEM_INTEGRATION.md, 9): the dialog Chrome shows for them,
// window-modal on the page's window, on Linux, and for prompt() on Windows,
// whose system message box has no text field.

#include "zebkit/engine/js_dialog_views.h"

#include <memory>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/base/mojom/ui_base_types.mojom-shared.h"
#include "ui/views/controls/message_box_view.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/widget/widget.h"
#include "ui/views/window/dialog_delegate.h"
#include "zebkit/engine/engine.h"

namespace zebkit {

namespace {

// Answers once, whichever way the dialog goes.
struct Answer {
  content::JavaScriptDialogManager::DialogClosedCallback callback;
  void Run(bool accepted, std::u16string text) {
    if (callback)
      std::move(callback).Run(accepted, std::move(text));
  }
};

}  // namespace

bool ShowViewsJavaScriptDialog(content::WebContents* contents, const std::u16string& title,
                               content::JavaScriptDialogType type, const std::u16string& message,
                               const std::u16string& default_prompt,
                               content::JavaScriptDialogManager::DialogClosedCallback& callback) {
  // Without a display (--headless, an off-screen view) a widget is no
  // window anyone sees: the dialog would wait for no one.
  if (HasNoDisplay(contents))
    return false;
  gfx::NativeWindow window = contents->GetTopLevelNativeWindow();
  if (!window || !views::Widget::GetWidgetForNativeWindow(window))
    return false;
  auto answer = std::make_shared<Answer>();
  answer->callback = std::move(callback);
  auto* box = new views::MessageBoxView(message);
  if (type == content::JAVASCRIPT_DIALOG_TYPE_PROMPT)
    box->SetPromptField(default_prompt);
  auto delegate = std::make_unique<views::DialogDelegate>();
  delegate->SetTitle(title);
  delegate->SetModalType(ui::mojom::ModalType::kWindow);
  delegate->SetShowCloseButton(false);
  delegate->SetButtons(type == content::JAVASCRIPT_DIALOG_TYPE_ALERT
                           ? static_cast<int>(ui::mojom::DialogButton::kOk)
                           : static_cast<int>(ui::mojom::DialogButton::kOk) |
                                 static_cast<int>(ui::mojom::DialogButton::kCancel));
  // The text field has the keyboard, as in Chrome's dialog for prompt().
  if (views::View* field = box->GetVisiblePromptField())
    delegate->SetInitiallyFocusedView(field);
  delegate->SetContentsView(base::WrapUnique(box));
  delegate->SetAcceptCallback(base::BindOnce(
      [](std::shared_ptr<Answer> answer, views::MessageBoxView* box, content::JavaScriptDialogType type) {
        answer->Run(true, type == content::JAVASCRIPT_DIALOG_TYPE_PROMPT
                              ? std::u16string(box->GetInputText())
                              : std::u16string());
      },
      answer, box, type));
  delegate->SetCancelCallback(base::BindOnce([](std::shared_ptr<Answer> answer) { answer->Run(false, {}); }, answer));
  delegate->SetCloseCallback(base::BindOnce([](std::shared_ptr<Answer> answer) { answer->Run(false, {}); }, answer));
  views::Widget* widget = views::DialogDelegate::CreateDialogWidget(std::move(delegate), gfx::NativeWindow(),
                                                                   window);
  widget->Show();
  return true;
}

}  // namespace zebkit
