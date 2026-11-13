// The sign-in dialog with Chromium's views (sign_in_dialog.h), on Linux and
// Windows: a username and a password, as Chrome's on those systems.

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/base/mojom/ui_base_types.mojom-shared.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"
#include "ui/views/window/dialog_delegate.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/sign_in_dialog.h"

namespace zebkit {

base::OnceClosure ShowSignInDialog(content::WebContents* contents, const std::u16string& title,
                                   const std::u16string& message, SignInAnswer& answer) {
  if (HasNoDisplay(contents))
    return base::OnceClosure();
  gfx::NativeWindow window = contents->GetTopLevelNativeWindow();
  if (!window || !views::Widget::GetWidgetForNativeWindow(window))
    return base::OnceClosure();
  auto form = std::make_unique<views::View>();
  form->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical,
                                                            gfx::Insets(12), 6));
  auto* text = form->AddChildView(std::make_unique<views::Label>(message));
  text->SetMultiLine(true);
  text->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  form->AddChildView(std::make_unique<views::Label>(u"Username"))->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  auto* username = form->AddChildView(std::make_unique<views::Textfield>());
  username->GetViewAccessibility().SetName(u"Username");
  form->AddChildView(std::make_unique<views::Label>(u"Password"))->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  auto* password = form->AddChildView(std::make_unique<views::Textfield>());
  password->SetTextInputType(ui::TEXT_INPUT_TYPE_PASSWORD);
  password->GetViewAccessibility().SetName(u"Password");

  auto shared = std::make_shared<SignInAnswer>(std::move(answer));
  auto delegate = std::make_unique<views::DialogDelegate>();
  delegate->SetTitle(title);
  delegate->SetModalType(ui::mojom::ModalType::kWindow);
  delegate->SetShowCloseButton(false);
  delegate->SetButtons(static_cast<int>(ui::mojom::DialogButton::kOk) |
                       static_cast<int>(ui::mojom::DialogButton::kCancel));
  delegate->SetButtonLabel(ui::mojom::DialogButton::kOk, u"Sign in");
  delegate->SetInitiallyFocusedView(username);
  delegate->SetAcceptCallback(base::BindOnce(
      [](std::shared_ptr<SignInAnswer> answer, views::Textfield* username, views::Textfield* password) {
        if (*answer)
          std::move(*answer).Run(std::make_pair(std::u16string(username->GetText()),
                                                std::u16string(password->GetText())));
      },
      shared, username, password));
  auto cancel = [](std::shared_ptr<SignInAnswer> answer) {
    if (*answer)
      std::move(*answer).Run(std::nullopt);
  };
  delegate->SetCancelCallback(base::BindOnce(cancel, shared));
  delegate->SetCloseCallback(base::BindOnce(cancel, shared));
  delegate->SetContentsView(std::move(form));
  views::Widget* widget = views::DialogDelegate::CreateDialogWidget(std::move(delegate), gfx::NativeWindow(), window);
  widget->Show();
  // Closed unanswered: the answer is dropped first, so that closing runs nothing.
  return base::BindOnce(
      [](base::WeakPtr<views::Widget> widget, std::shared_ptr<SignInAnswer> answer) {
        answer->Reset();
        if (widget)
          widget->CloseWithReason(views::Widget::ClosedReason::kUnspecified);
      },
      widget->GetWeakPtr(), shared);
}

}  // namespace zebkit
