// Default presentation on desktop platforms using Chromium views. Hosts that
// install the public handler skip this UI entirely, including headless hosts.
#include <algorithm>
#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/utf_string_conversions.h"
#include "components/qr_code_generator/qr_code_generator.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/base/ime/text_input_type.h"
#include "ui/base/models/image_model.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/controls/button/md_text_button.h"
#include "ui/views/controls/image_view.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/scroll_view.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_delegate.h"
#include "ui/views/widget/widget_observer.h"
#include "zebkit/engine/view.h"
#include "zebkit/engine/webauthn_ui.h"

namespace zebkit {
namespace {
std::string Text(const base::DictValue& event, std::string_view name) {
  const auto* value = event.FindString(name);
  return value ? *value : "";
}
class EscapeView : public views::View {
 public:
  explicit EscapeView(base::RepeatingClosure cancel)
      : cancel_(std::move(cancel)) {}
  bool OnKeyPressed(const ui::KeyEvent& event) override {
    if (event.key_code() != ui::VKEY_ESCAPE)
      return false;
    cancel_.Run();
    return true;
  }

 private:
  base::RepeatingClosure cancel_;
};
class ViewsAuthenticationUI : public AuthenticationUI,
                              public views::WidgetObserver {
 public:
  ViewsAuthenticationUI(content::RenderFrameHost* frame,
                        bool host_ui,
                        AuthenticationResponse response)
      : response_(std::move(response)) {
    auto* contents = content::WebContents::FromRenderFrameHost(frame);
    context_ = contents->GetTopLevelNativeWindow();
    auto* view = ViewOf(contents);
    // Generic headless rendering has no native desktop surface. The public
    // callback remains usable there; don't instantiate a hidden default widget.
    available_ = !host_ui && view && !view->offscreen_view_;
  }
  ~ViewsAuthenticationUI() override { Dismiss(); }
  bool Available() const override { return available_; }
  bool Focused() const override { return widget_ && widget_->IsActive(); }
  void Configure(device::FidoDiscoveryFactory*) override {}
  void PrepareSystem() override { Dismiss(); }
  void Dismiss() override {
    if (widget_) {
      widget_->RemoveObserver(this);
      widget_->CloseNow();
      widget_.reset();
    }
    delegate_.reset();
    pin_ = nullptr;
    confirmation_ = nullptr;
  }
  void OnWidgetClosing(views::Widget*) override { Submit("cancel"); }
  void Submit(std::string action, std::string account = "") {
    base::DictValue reply;
    reply.Set("request", Text(event_, "request"));
    reply.Set("revision", event_.FindInt("revision").value_or(0));
    reply.Set("action", action);
    if (action == "pin" && pin_) {
      reply.Set("pin", base::UTF16ToUTF8(pin_->GetText()));
      pin_->SetText(u"");
      if (confirmation_) {
        reply.Set("confirmation", base::UTF16ToUTF8(confirmation_->GetText()));
        confirmation_->SetText(u"");
      }
    }
    if (!account.empty())
      reply.Set("account", account);
    auto callback = response_;
    callback.Run(std::move(reply));
  }
  void Show(const base::DictValue& event) override {
    Dismiss();
    event_ = event.Clone();
    if (!available_) {
      Submit("cancel");
      return;
    }
    auto root = std::make_unique<EscapeView>(
        base::BindRepeating(&ViewsAuthenticationUI::Submit,
                            weak_factory_.GetWeakPtr(), "cancel", ""));
    root->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical, gfx::Insets(20), 12));
    auto label = [&](std::string value) {
      auto* field = root->AddChildView(
          std::make_unique<views::Label>(base::UTF8ToUTF16(value)));
      field->SetMultiLine(true);
      field->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    };
    label("Verify your identity for " + Text(event, "origin"));
    if (event.FindBool("phone").value_or(false))
      label(
          "Scan with a nearby phone that has this account's passkey. Keep "
          "Bluetooth on.");
    if (event.FindBool("security_key").value_or(false))
      label("You can also insert and touch a USB security key.");
    if (const auto* qr = event.FindString("qr")) {
      auto code = qr_code_generator::GenerateCode(base::as_byte_span(*qr));
      if (code.has_value()) {
        const int scale = 4, margin = 4,
                  edge = (code->qr_size + 2 * margin) * scale;
        SkBitmap bitmap;
        bitmap.allocN32Pixels(edge, edge);
        bitmap.eraseColor(SK_ColorWHITE);
        for (int y = 0; y < code->qr_size; ++y) {
          for (int x = 0; x < code->qr_size; ++x) {
            if (!(code->data[y * code->qr_size + x] & 1))
              continue;
            bitmap.eraseArea(
                SkIRect::MakeXYWH((x + margin) * scale, (y + margin) * scale,
                                  scale, scale),
                SK_ColorBLACK);
          }
        }
        auto* image = root->AddChildView(std::make_unique<views::ImageView>());
        image->SetImage(ui::ImageModel::FromImageSkia(
            gfx::ImageSkia::CreateFrom1xBitmap(bitmap)));
        image->GetViewAccessibility().SetName(
            u"Passkey QR code. Scan with your phone camera.");
      }
    }
    for (std::string_view key : {"message", "error"})
      if (const auto* text = event.FindString(key))
        label(*text);
    if (Text(event, "bluetooth") == "off")
      label("Turn on Bluetooth to use your phone.");
    if (Text(event, "bluetooth") == "denied")
      label(
          "Allow Bluetooth access in your system settings to use your phone.");
    if (Text(event, "state") == "pin") {
      const bool setting = event.FindBool("setting").value_or(false);
      const auto add_pin = [&](std::u16string name) {
        label(base::UTF16ToUTF8(name));
        auto* field = root->AddChildView(std::make_unique<views::Textfield>());
        field->SetTextInputType(ui::TEXT_INPUT_TYPE_PASSWORD);
        field->GetViewAccessibility().SetName(name);
        return field;
      };
      pin_ = add_pin(setting ? u"New security-key PIN" : u"Security-key PIN");
      if (setting)
        confirmation_ = add_pin(u"Confirm new PIN");
    }
    auto button = [&](std::string action, std::string name,
                      std::string account = "") {
      root->AddChildView(std::make_unique<views::MdTextButton>(
          base::BindRepeating(&ViewsAuthenticationUI::Submit,
                              weak_factory_.GetWeakPtr(), action, account),
          base::UTF8ToUTF16(name)));
    };
    if (const auto* accounts = event.FindList("accounts")) {
      for (const auto& account : *accounts) {
        const auto& data = account.GetDict();
        button("account", Text(data, "display_name") + " " + Text(data, "name"),
               Text(data, "id"));
      }
    }
    const auto* actions = event.FindList("actions");
    for (const auto& [action, name] :
         std::initializer_list<std::pair<const char*, const char*>>{
             {"external", "Use a phone or security key"},
             {"bluetooth", "Enable Bluetooth"},
             {"pin", "Continue"},
             {"cancel", "Cancel"}}) {
      if (actions &&
          std::ranges::any_of(*actions, [&](const base::Value& value) {
            return value == base::Value(action);
          }))
        button(action, name);
    }
    auto scroll = std::make_unique<views::ScrollView>();
    scroll->SetContents(std::move(root));
    scroll->SetPreferredSize(gfx::Size(440, 600));
    delegate_ = std::make_unique<views::WidgetDelegate>();
    delegate_->SetTitle(base::UTF8ToUTF16(Text(event, "title")));
    delegate_->SetContentsView(std::move(scroll));
    widget_ = std::make_unique<views::Widget>();
    views::Widget::InitParams params(
        views::Widget::InitParams::CLIENT_OWNS_WIDGET,
        views::Widget::InitParams::TYPE_WINDOW);
    params.context = context_;
    params.bounds = gfx::Rect(460, 640);
    params.delegate = delegate_.get();
    widget_->Init(std::move(params));
    widget_->AddObserver(this);
    widget_->Show();
    if (pin_)
      pin_->RequestFocus();
  }

 private:
  AuthenticationResponse response_;
  base::DictValue event_;
  gfx::NativeWindow context_;
  bool available_ = false;
  std::unique_ptr<views::WidgetDelegate> delegate_;
  std::unique_ptr<views::Widget> widget_;
  raw_ptr<views::Textfield> pin_ = nullptr, confirmation_ = nullptr;
  base::WeakPtrFactory<ViewsAuthenticationUI> weak_factory_{this};
};
}  // namespace
bool SystemPasskeysAvailable() {
  return false;
}
std::unique_ptr<AuthenticationUI> CreateAuthenticationUI(
    content::RenderFrameHost* frame,
    bool host_ui,
    AuthenticationResponse response) {
  return std::make_unique<ViewsAuthenticationUI>(frame, host_ui,
                                                 std::move(response));
}
}  // namespace zebkit
