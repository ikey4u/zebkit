// A page's web notifications (docs/SYSTEM_INTEGRATION.md, section 12): new
// Notification() and a service worker's showNotification(), once the
// "notifications" permission is granted (denied by default, as every
// permission is). The application shows them (zebkit_on_notification), or
// the system does: org.freedesktop.Notifications on Linux, the
// UserNotifications framework on macOS. Clicks and closes go back to the
// page's handlers and its service worker's notificationclick.

#ifndef ZEBKIT_ENGINE_NOTIFICATIONS_H_
#define ZEBKIT_ENGINE_NOTIFICATIONS_H_

#include <map>
#include <optional>
#include <set>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "content/public/browser/platform_notification_service.h"
#include "url/gurl.h"

namespace content {
class BrowserContext;
}

namespace zebkit {

class NotificationService : public content::PlatformNotificationService {
 public:
  explicit NotificationService(content::BrowserContext* context);
  ~NotificationService() override;

  void DisplayNotification(const std::string& notification_id, const GURL& origin, const GURL& document_url,
                           const blink::PlatformNotificationData& notification_data,
                           const blink::NotificationResources& notification_resources) override;
  void DisplayPersistentNotification(const std::string& notification_id, const GURL& service_worker_origin,
                                     const GURL& origin, const blink::PlatformNotificationData& notification_data,
                                     const blink::NotificationResources& notification_resources) override;
  void CloseNotification(const std::string& notification_id) override;
  void ClosePersistentNotification(const std::string& notification_id) override;
  void GetDisplayedNotifications(DisplayedNotificationsCallback callback) override;
  void GetDisplayedNotificationsForOrigin(const GURL& origin, DisplayedNotificationsCallback callback) override;
  void ScheduleTrigger(base::Time timestamp) override;
  base::Time ReadNextTriggerTimestamp() override;
  int64_t ReadNextPersistentNotificationId() override;
  void RecordNotificationUkmEvent(const content::NotificationDatabaseData& data) override;

  // What the person did with notification @id, from the application or the system.
  bool Clicked(const std::string& id, std::optional<int> action);
  bool Closed(const std::string& id, bool by_user);

  static NotificationService* Owning(const std::string& id);

 private:
  struct Shown {
    bool persistent = false;
    GURL origin;
  };
  void Display(const std::string& id, bool persistent, const GURL& origin,
               const blink::PlatformNotificationData& data);
  void Close(const std::string& id, bool by_page);

  raw_ptr<content::BrowserContext> context_;
  std::map<std::string, Shown> shown_;
  int64_t next_persistent_id_;
  base::WeakPtrFactory<NotificationService> weak_factory_{this};
};

// The system's notifications (notifications_linux.cc, notifications_mac.mm,
// notifications_win.cc): false when the system has no service to show it.
bool ShowSystemNotification(const std::string& id, const base::DictValue& notification);
void CloseSystemNotification(const std::string& id);
// Told by the system: clicked (with an action's index or none), closed.
void SystemNotificationClicked(const std::string& id, std::optional<int> action);
void SystemNotificationClosed(const std::string& id, bool by_user);

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_NOTIFICATIONS_H_
