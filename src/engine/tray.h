// The application's icon in the system's tray or menu bar
// (docs/SYSTEM_INTEGRATION.md, 12): an icon, a tooltip, a menu of the
// application's items (app.h's), and its click. NSStatusItem on macOS,
// Shell_NotifyIcon on Windows, a StatusNotifierItem over D-Bus on Linux
// (tray_linux_dbus.h), where a desktop without a host for it shows nothing.

#ifndef ZEBKIT_ENGINE_TRAY_H_
#define ZEBKIT_ENGINE_TRAY_H_

#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "zebkit/engine/app.h"

namespace zebkit {

class TrayPlatform {
 public:
  virtual ~TrayPlatform() = default;
  virtual void SetImage(const SkBitmap& image) = 0;
  virtual void SetToolTip(const std::u16string& tooltip) = 0;
  // The menu changed (Tray::menu()).
  virtual void MenuChanged() = 0;
};

class Tray {
 public:
  Tray(SkBitmap image, std::u16string tooltip);
  ~Tray();

  void SetImage(SkBitmap image);
  void SetToolTip(std::u16string tooltip);
  void SetMenu(std::vector<MenuItem> menu, base::RepeatingCallback<void(const std::string&)> chosen);
  void SetClick(base::RepeatingClosure click) { click_ = std::move(click); }

  const SkBitmap& image() const { return image_; }
  const std::u16string& tooltip() const { return tooltip_; }
  const std::vector<MenuItem>& menu() const { return menu_; }
  // From the platform: the icon clicked, an item of its menu chosen.
  void Clicked();
  void Chosen(const std::string& id);

 private:
  SkBitmap image_;
  std::u16string tooltip_;
  std::vector<MenuItem> menu_;
  base::RepeatingCallback<void(const std::string&)> chosen_;
  base::RepeatingClosure click_;
  std::unique_ptr<TrayPlatform> platform_;
};

// tray_linux.cc, tray_mac.mm, tray_win.cc; null where there is none.
std::unique_ptr<TrayPlatform> CreateTrayPlatform(Tray* tray);

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_TRAY_H_
