// Web notifications on Windows (notifications.h): a balloon of a notification
// area icon of the engine's (Shell_NotifyIcon with NIF_INFO), which Windows 10
// and later show as a toast in the Action Center under the program's name. One
// at a time: a new one replaces the one shown, which is closed. Its click is
// the notification's click; its timing out, or the person dismissing it, its
// close. The application's function (zebkit_on_notification) takes them all
// instead when it is set, as on the other systems.

#include <windows.h>

#include <shellapi.h>

#include <string>

#include "base/no_destructor.h"
#include "base/strings/utf_string_conversions.h"
#include "base/win/scoped_gdi_object.h"
#include "zebkit/engine/notifications.h"

namespace zebkit {

namespace {

constexpr UINT kCallback = WM_APP + 42;

class Balloon {
 public:
  Balloon() {
    static const ATOM atom = [] {
      WNDCLASSEXW type = {sizeof(type)};
      type.lpfnWndProc = &Balloon::Proc;
      type.hInstance = GetModuleHandleW(nullptr);
      type.lpszClassName = L"ZebkitNotifications";
      return RegisterClassExW(&type);
    }();
    window_ = CreateWindowExW(0, MAKEINTATOM(atom), L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
  }

  bool Show(const std::string& id, const base::DictValue& notification) {
    if (!window_)
      return false;
    if (!current_.empty() && current_ != id)
      Finish(false);
    NOTIFYICONDATAW data = Base();
    if (!added_) {
      // The program's own icon, or Windows' default for one; taken once, and
      // destroyed with this.
      if (!icon_taken_) {
        icon_taken_ = true;
        HICON icon = nullptr;
        wchar_t path[MAX_PATH];
        if (GetModuleFileNameW(nullptr, path, MAX_PATH) && ExtractIconExW(path, 0, nullptr, &icon, 1) && icon)
          icon_ = base::win::ScopedGDIObject<HICON>(icon);
      }
      data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
      data.uCallbackMessage = kCallback;
      data.hIcon = icon_.is_valid() ? icon_.get() : LoadIconW(nullptr, IDI_APPLICATION);
      wcsncpy_s(data.szTip, L"Notifications", _TRUNCATE);
      if (!Shell_NotifyIconW(NIM_ADD, &data))
        return false;
      data.uVersion = NOTIFYICON_VERSION_4;
      Shell_NotifyIconW(NIM_SETVERSION, &data);
      added_ = true;
      data = Base();
    }
    const std::string* title = notification.FindString("title");
    const std::string* body = notification.FindString("body");
    const std::string* origin = notification.FindString("origin");
    std::string text = body ? *body : std::string();
    if (origin && !origin->empty())
      text += (text.empty() ? "" : "\n") + *origin;
    data.uFlags = NIF_INFO;
    wcsncpy_s(data.szInfoTitle, base::UTF8ToWide(title ? *title : "").c_str(), _TRUNCATE);
    // An empty text would remove the balloon instead.
    wcsncpy_s(data.szInfo, base::UTF8ToWide(text.empty() ? " " : text).c_str(), _TRUNCATE);
    data.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON |
                       (notification.FindBool("silent").value_or(false) ? NIIF_NOSOUND : 0);
    if (!Shell_NotifyIconW(NIM_MODIFY, &data))
      return false;
    current_ = id;
    return true;
  }

  void Close(const std::string& id) {
    if (id != current_)
      return;
    current_.clear();
    Hide();
  }

 private:
  NOTIFYICONDATAW Base() const {
    NOTIFYICONDATAW data = {sizeof(data)};
    data.hWnd = window_;
    data.uID = 1;
    return data;
  }

  // The balloon gone: the icon too, so that nothing stays in the tray.
  void Hide() {
    if (!added_)
      return;
    NOTIFYICONDATAW data = Base();
    Shell_NotifyIconW(NIM_DELETE, &data);
    added_ = false;
  }

  void Finish(bool clicked, bool by_user = false) {
    const std::string id = current_;
    current_.clear();
    if (id.empty())
      return;
    if (clicked)
      SystemNotificationClicked(id, std::nullopt);
    SystemNotificationClosed(id, clicked || by_user);
  }

  static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

  HWND window_ = nullptr;
  bool added_ = false;
  bool icon_taken_ = false;
  base::win::ScopedGDIObject<HICON> icon_;
  std::string current_;
};

Balloon& TheBalloon() {
  static base::NoDestructor<Balloon> balloon;
  return *balloon;
}

LRESULT CALLBACK Balloon::Proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == kCallback) {
    Balloon& balloon = TheBalloon();
    switch (LOWORD(lparam)) {
      case NIN_BALLOONUSERCLICK:
      case WM_LBUTTONUP:
        balloon.Finish(true);
        balloon.Hide();
        return 0;
      case NIN_BALLOONTIMEOUT:
        balloon.Finish(false, false);
        balloon.Hide();
        return 0;
      // Sent when the icon goes, not when the person dismisses it (which is
      // NIN_BALLOONTIMEOUT too): not by the person.
      case NIN_BALLOONHIDE:
        balloon.Finish(false, false);
        balloon.Hide();
        return 0;
    }
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

bool ShowSystemNotification(const std::string& id, const base::DictValue& notification) {
  return TheBalloon().Show(id, notification);
}

void CloseSystemNotification(const std::string& id) {
  TheBalloon().Close(id);
}

}  // namespace zebkit
