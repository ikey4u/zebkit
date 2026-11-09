// The client certificate picker with Chromium's views (certificate_picker.h).

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/models/combobox_model.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/base/mojom/ui_base_types.mojom-shared.h"
#include "ui/views/controls/combobox/combobox.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/controls/label.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/widget/widget.h"
#include "ui/views/window/dialog_delegate.h"
#include "zebkit/engine/certificate_picker.h"
#include "zebkit/engine/engine.h"

namespace zebkit {

namespace {

class Names : public ui::ComboboxModel {
 public:
  explicit Names(std::vector<std::u16string> names) : names_(std::move(names)) {}
  size_t GetItemCount() const override { return names_.size(); }
  std::u16string GetItemAt(size_t index) const override { return names_[index]; }

 private:
  std::vector<std::u16string> names_;
};

}  // namespace

bool ShowCertificatePicker(content::WebContents* contents, const std::u16string& host,
                           const std::vector<std::u16string>& names, CertificateAnswer& answer) {
  if (HasNoDisplay(contents) || names.empty())
    return false;
  gfx::NativeWindow window = contents->GetTopLevelNativeWindow();
  if (!window || !views::Widget::GetWidgetForNativeWindow(window))
    return false;
  auto form = std::make_unique<views::View>();
  form->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical,
                                                            gfx::Insets(12), 8));
  auto* text = form->AddChildView(std::make_unique<views::Label>(host + u" asks for a certificate."));
  text->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  auto* list = form->AddChildView(std::make_unique<views::Combobox>(std::make_unique<Names>(names)));
  list->GetViewAccessibility().SetName(u"Certificate");
  auto shared = std::make_shared<CertificateAnswer>(std::move(answer));
  auto delegate = std::make_unique<views::DialogDelegate>();
  delegate->SetTitle(u"Choose a certificate");
  delegate->SetModalType(ui::mojom::ModalType::kWindow);
  delegate->SetShowCloseButton(false);
  delegate->SetButtons(static_cast<int>(ui::mojom::DialogButton::kOk) |
                       static_cast<int>(ui::mojom::DialogButton::kCancel));
  delegate->SetInitiallyFocusedView(list);
  delegate->SetAcceptCallback(base::BindOnce(
      [](std::shared_ptr<CertificateAnswer> answer, views::Combobox* list) {
        if (*answer)
          std::move(*answer).Run(list->GetSelectedIndex());
      },
      shared, list));
  auto cancel = [](std::shared_ptr<CertificateAnswer> answer) {
    if (*answer)
      std::move(*answer).Run(std::nullopt);
  };
  delegate->SetCancelCallback(base::BindOnce(cancel, shared));
  delegate->SetCloseCallback(base::BindOnce(cancel, shared));
  delegate->SetContentsView(std::move(form));
  views::DialogDelegate::CreateDialogWidget(std::move(delegate), gfx::NativeWindow(), window)->Show();
  return true;
}

}  // namespace zebkit
