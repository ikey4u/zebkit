// The tray on Windows (tray.h): an icon in the notification area,
// Shell_NotifyIcon, with a message-only window for its clicks and menu.

#include <windows.h>

#include <shellapi.h>

#include <memory>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/current_thread.h"
#include "base/strings/utf_string_conversions.h"
#include "base/win/scoped_gdi_object.h"
#include "ui/gfx/win/icon_util.h"
#include "zebkit/engine/tray.h"

namespace zebkit {

namespace {

constexpr UINT kCallback = WM_APP + 41;

class WinTray : public TrayPlatform {
 public:
  explicit WinTray(Tray* tray) : tray_(tray) {
    static const ATOM atom = [] {
      WNDCLASSEXW type = {sizeof(type)};
      type.lpfnWndProc = &WinTray::Proc;
      type.hInstance = GetModuleHandleW(nullptr);
      type.lpszClassName = L"ZebkitTray";
      return RegisterClassExW(&type);
    }();
    window_ = CreateWindowExW(0, MAKEINTATOM(atom), L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
    SetWindowLongPtrW(window_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    static UINT next_id = 1;
    data_.cbSize = sizeof(data_);
    data_.hWnd = window_;
    data_.uID = next_id++;
    data_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data_.uCallbackMessage = kCallback;
    icon_ = IconUtil::CreateHICONFromSkBitmap(tray->image());
    data_.hIcon = icon_.get();
    Tip(tray->tooltip());
    Shell_NotifyIconW(NIM_ADD, &data_);
  }
  ~WinTray() override {
    Shell_NotifyIconW(NIM_DELETE, &data_);
    DestroyWindow(window_);
  }

  void SetImage(const SkBitmap& image) override {
    icon_ = IconUtil::CreateHICONFromSkBitmap(image);
    data_.hIcon = icon_.get();
    data_.uFlags = NIF_ICON;
    Shell_NotifyIconW(NIM_MODIFY, &data_);
  }
  void SetToolTip(const std::u16string& tooltip) override {
    Tip(tooltip);
    data_.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &data_);
  }
  void MenuChanged() override {}

 private:
  void Tip(const std::u16string& tooltip) {
    const std::wstring text = base::UTF16ToWide(tooltip).substr(0, std::size(data_.szTip) - 1);
    wcsncpy_s(data_.szTip, text.c_str(), _TRUNCATE);
  }

  // The menu where the person right-clicked, built from the items each time.
  void Menu() {
    if (tray_->menu().empty())
      return;
    HMENU menu = CreatePopupMenu();
    std::vector<std::string> ids;
    Add(menu, tray_->menu(), &ids);
    POINT at;
    GetCursorPos(&at);
    // The system closes a tray's menu on a click elsewhere only for a window in front.
    SetForegroundWindow(window_);
    // The engine's tasks go on while the menu is open; one of them may
    // close the tray, and this with it.
    base::WeakPtr<WinTray> alive = weak_factory_.GetWeakPtr();
    UINT chosen;
    {
      base::CurrentThread::ScopedAllowApplicationTasksInNativeNestedLoop allow;
      chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, at.x, at.y, 0, window_,
                              nullptr);
    }
    DestroyMenu(menu);
    if (alive && chosen > 0 && chosen <= ids.size())
      tray_->Chosen(ids[chosen - 1]);
  }
  static void Add(HMENU menu, const std::vector<MenuItem>& items, std::vector<std::string>* ids) {
    for (const MenuItem& item : items) {
      if (item.separator) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        continue;
      }
      const std::wstring title = base::UTF8ToWide(item.title);
      if (!item.items.empty()) {
        HMENU submenu = CreatePopupMenu();
        Add(submenu, item.items, ids);
        AppendMenuW(menu, MF_POPUP | (item.enabled ? 0 : MF_GRAYED), reinterpret_cast<UINT_PTR>(submenu),
                    title.c_str());
        continue;
      }
      ids->push_back(item.id);
      AppendMenuW(menu, MF_STRING | (item.enabled ? 0 : MF_GRAYED) | (item.checked ? MF_CHECKED : 0),
                  ids->size(), title.c_str());
    }
  }

  static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<WinTray*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self && message == kCallback) {
      if (LOWORD(l) == WM_LBUTTONUP)
        self->tray_->Clicked();
      else if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_CONTEXTMENU)
        self->Menu();
      return 0;
    }
    return DefWindowProcW(window, message, w, l);
  }

  raw_ptr<Tray> tray_;
  HWND window_ = nullptr;
  NOTIFYICONDATAW data_ = {};
  base::win::ScopedGDIObject<HICON> icon_;
  base::WeakPtrFactory<WinTray> weak_factory_{this};
};

}  // namespace

std::unique_ptr<TrayPlatform> CreateTrayPlatform(Tray* tray) {
  return std::make_unique<WinTray>(tray);
}

}  // namespace zebkit
