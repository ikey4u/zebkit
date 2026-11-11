// Web notifications shown by the desktop's notification service
// (notifications.h): org.freedesktop.Notifications, the Desktop
// Notifications Specification, as Chrome's Linux bridge uses it.

#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "components/dbus/thread_linux/dbus_thread_linux.h"
#include "components/dbus/utils/call_method.h"
#include "components/dbus/utils/connect_to_signal.h"
#include "components/dbus/utils/variant.h"
#include "dbus/bus.h"
#include "dbus/object_proxy.h"
#include "zebkit/engine/notifications.h"

namespace zebkit {

namespace {

constexpr char kInterface[] = "org.freedesktop.Notifications";

class Desktop {
 public:
  Desktop()
      : bus_(dbus_thread_linux::GetSharedSessionBus()),
        proxy_(bus_->GetObjectProxy(kInterface, dbus::ObjectPath("/org/freedesktop/Notifications"))) {
    dbus_utils::ConnectToSignal<"us">(proxy_, kInterface, "ActionInvoked",
                                      base::BindRepeating(&Desktop::Action, base::Unretained(this)),
                                      base::BindOnce([](const std::string&, const std::string&, bool) {}));
    dbus_utils::ConnectToSignal<"uu">(proxy_, kInterface, "NotificationClosed",
                                      base::BindRepeating(&Desktop::ClosedSignal, base::Unretained(this)),
                                      base::BindOnce([](const std::string&, const std::string&, bool) {}));
  }

  void Show(const std::string& id, const base::DictValue& notification) {
    // Actions as the specification has them: an id then its label, "default"
    // the click on the notification itself.
    std::vector<std::string> actions = {"default", ""};
    if (const base::ListValue* list = notification.FindList("actions")) {
      for (size_t i = 0; i < list->size(); ++i) {
        const std::string* title = (*list)[i].GetDict().FindString("title");
        actions.push_back(base::NumberToString(i));
        actions.push_back(title ? *title : std::string());
      }
    }
    std::map<std::string, dbus_utils::Variant> hints;
    if (notification.FindBool("requireInteraction").value_or(false))
      hints["urgency"] = dbus_utils::Variant::Wrap<"y">(uint8_t{2});
    const std::string* title = notification.FindString("title");
    const std::string* body = notification.FindString("body");
    const std::string* origin = notification.FindString("origin");
    const uint32_t replaces = system_ids_.contains(id) ? system_ids_[id] : 0;
    dbus_utils::CallMethod<"susssasa{sv}i", "u">(
        proxy_, kInterface, "Notify",
        base::BindOnce(&Desktop::Shown, base::Unretained(this), id), std::string(origin ? *origin : ""), replaces,
        std::string(), std::string(title ? *title : ""), std::string(body ? *body : ""), std::move(actions),
        std::move(hints), int32_t{-1});
  }

  void Close(const std::string& id) {
    auto it = system_ids_.find(id);
    if (it == system_ids_.end()) {
      // Before Notify has answered: closed once it does.
      closed_early_.insert(id);
      return;
    }
    dbus_utils::CallMethod<"u", "">(proxy_, kInterface, "CloseNotification",
                                    base::BindOnce([](dbus_utils::CallMethodResult<>) {}), it->second);
    ids_.erase(it->second);
    system_ids_.erase(it);
  }

 private:
  void Shown(std::string id, dbus_utils::CallMethodResultSig<"u"> result) {
    const bool closed = closed_early_.erase(id);
    if (!result.has_value()) {
      // Nothing shows it: closed, not shown for ever.
      if (!closed)
        SystemNotificationClosed(id, false);
      return;
    }
    const uint32_t system_id = std::get<0>(result.value());
    system_ids_[id] = system_id;
    ids_[system_id] = id;
    if (closed)
      Close(id);
  }

  void Action(dbus_utils::ConnectToSignalResultSig<"us"> result) {
    if (!result.has_value())
      return;
    auto& [system_id, key] = result.value();
    auto it = ids_.find(system_id);
    if (it == ids_.end())
      return;
    int index = -1;
    std::optional<int> action;
    if (key != "default" && base::StringToInt(key, &index))
      action = index;
    SystemNotificationClicked(it->second, action);
  }

  void ClosedSignal(dbus_utils::ConnectToSignalResultSig<"uu"> result) {
    if (!result.has_value())
      return;
    auto& [system_id, reason] = result.value();
    auto it = ids_.find(system_id);
    if (it == ids_.end())
      return;
    const std::string id = it->second;
    ids_.erase(it);
    system_ids_.erase(id);
    // 2: dismissed by the person (the specification's reasons).
    SystemNotificationClosed(id, reason == 2);
  }

  scoped_refptr<dbus::Bus> bus_;
  raw_ptr<dbus::ObjectProxy> proxy_;
  std::map<std::string, uint32_t> system_ids_;
  std::map<uint32_t, std::string> ids_;
  std::set<std::string> closed_early_;
};

Desktop& TheDesktop() {
  static base::NoDestructor<Desktop> desktop;
  return *desktop;
}

}  // namespace

bool ShowSystemNotification(const std::string& id, const base::DictValue& notification) {
  TheDesktop().Show(id, notification);
  return true;
}

void CloseSystemNotification(const std::string& id) {
  TheDesktop().Close(id);
}

}  // namespace zebkit
