// The tray on Linux (tray.h): a StatusNotifierItem (tray_linux_dbus.cc),
// shown by the desktop's host for them, with the menu over
// com.canonical.dbusmenu.

#include <memory>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/strings/utf_string_conversions.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/linux/status_icon_linux.h"
#include "ui/menus/simple_menu_model.h"
#include "zebkit/engine/tray.h"
#include "zebkit/engine/tray_linux_dbus.h"

namespace zebkit {

namespace {

class LinuxTray : public TrayPlatform,
                  public ui::StatusIconLinux::Delegate,
                  public ui::SimpleMenuModel::Delegate {
 public:
  explicit LinuxTray(Tray* tray) : tray_(tray), image_(gfx::ImageSkia::CreateFrom1xBitmap(tray->image())) {
    Rebuild();
    icon_ = base::MakeRefCounted<TrayIconDbus>();
    icon_->SetDelegate(this);
  }
  ~LinuxTray() override {
    if (icon_)
      icon_->SetDelegate(nullptr);
  }

  // TrayPlatform:
  void SetImage(const SkBitmap& image) override {
    image_ = gfx::ImageSkia::CreateFrom1xBitmap(image);
    if (icon_)
      icon_->SetImage(image_);
  }
  void SetToolTip(const std::u16string& tooltip) override {
    if (icon_)
      icon_->SetToolTip(tooltip);
  }
  void MenuChanged() override {
    Rebuild();
    if (icon_)
      icon_->UpdatePlatformContextMenu(model_.get());
  }

  // ui::StatusIconLinux::Delegate:
  void OnClick() override { tray_->Clicked(); }
  bool HasClickAction() override { return true; }
  const gfx::ImageSkia& GetImage() const override { return image_; }
  const gfx::VectorIcon* GetIcon() const override { return nullptr; }
  const std::u16string& GetToolTip() const override { return tray_->tooltip(); }
  ui::MenuModel* GetMenuModel() const override { return model_.get(); }
  void OnImplInitializationFailed() override { icon_ = nullptr; }

  // ui::SimpleMenuModel::Delegate:
  bool IsCommandIdEnabled(int command_id) const override {
    return command_id >= 0 && static_cast<size_t>(command_id) < items_.size() && items_[command_id]->enabled;
  }
  bool IsCommandIdChecked(int command_id) const override {
    return command_id >= 0 && static_cast<size_t>(command_id) < items_.size() && items_[command_id]->checked;
  }
  void ExecuteCommand(int command_id, int event_flags) override {
    if (IsCommandIdEnabled(command_id))
      tray_->Chosen(items_[command_id]->id);
  }

 private:
  void Rebuild() {
    items_.clear();
    submenus_.clear();
    model_ = std::make_unique<ui::SimpleMenuModel>(this);
    Add(model_.get(), tray_->menu());
  }
  void Add(ui::SimpleMenuModel* model, const std::vector<MenuItem>& items) {
    for (const MenuItem& item : items) {
      if (item.separator) {
        model->AddSeparator(ui::NORMAL_SEPARATOR);
        continue;
      }
      const std::u16string title = base::UTF8ToUTF16(item.title);
      if (!item.items.empty()) {
        auto submenu = std::make_unique<ui::SimpleMenuModel>(this);
        Add(submenu.get(), item.items);
        model->AddSubMenu(-1, title, submenu.get());
        submenus_.push_back(std::move(submenu));
        continue;
      }
      const int command = static_cast<int>(items_.size());
      items_.push_back(&item);
      if (!item.radio_group.empty())
        model->AddRadioItem(command, title, std::hash<std::string>()(item.radio_group) & 0x7fffffff);
      else if (item.checked)
        model->AddCheckItem(command, title);
      else
        model->AddItem(command, title);
    }
  }

  raw_ptr<Tray> tray_;
  gfx::ImageSkia image_;
  std::unique_ptr<ui::SimpleMenuModel> model_;
  std::vector<std::unique_ptr<ui::SimpleMenuModel>> submenus_;
  std::vector<raw_ptr<const MenuItem>> items_;
  scoped_refptr<TrayIconDbus> icon_;
};

}  // namespace

std::unique_ptr<TrayPlatform> CreateTrayPlatform(Tray* tray) {
  return std::make_unique<LinuxTray>(tray);
}

}  // namespace zebkit
