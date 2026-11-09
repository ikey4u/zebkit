#include "zebkit/engine/notifications.h"

#include <string.h>

#include <utility>

#include "base/functional/callback_helpers.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/notification_event_dispatcher.h"
#include "third_party/blink/public/common/notifications/notification_resources.h"
#include "third_party/blink/public/common/notifications/platform_notification_data.h"
#include "third_party/blink/public/mojom/notifications/notification.mojom.h"
#include "zebkit/engine/include/zebkit/zebkit.h"

namespace zebkit {

namespace {

struct Host {
  zebkit_notification_func func = nullptr;
  void* data = nullptr;
};

Host& TheHost() {
  static Host host;
  return host;
}

std::map<std::string, NotificationService*>& Owners() {
  static base::NoDestructor<std::map<std::string, NotificationService*>> owners;
  return *owners;
}

void Tell(base::DictValue event) {
  Host& host = TheHost();
  if (!host.func)
    return;
  const std::string json = base::WriteJson(event).value_or("{}");
  host.func(json.c_str(), host.data);
}

base::DictValue Describe(const std::string& id, bool persistent, const GURL& origin,
                         const blink::PlatformNotificationData& data) {
  base::DictValue value;
  value.Set("id", id);
  value.Set("persistent", persistent);
  value.Set("origin", origin.DeprecatedGetOriginAsURL().spec());
  value.Set("title", base::UTF16ToUTF8(data.title));
  value.Set("body", base::UTF16ToUTF8(data.body));
  value.Set("icon", data.icon.spec());
  value.Set("image", data.image.spec());
  value.Set("badge", data.badge.spec());
  value.Set("tag", data.tag);
  value.Set("lang", data.lang);
  value.Set("silent", data.silent);
  value.Set("requireInteraction", data.require_interaction);
  value.Set("renotify", data.renotify);
  base::ListValue actions;
  for (const auto& action : data.actions) {
    base::DictValue item;
    item.Set("action", action->action);
    item.Set("title", base::UTF16ToUTF8(action->title));
    actions.Append(std::move(item));
  }
  value.Set("actions", std::move(actions));
  return value;
}

}  // namespace

NotificationService::NotificationService(content::BrowserContext* context)
    : context_(context),
      // Persistent ids are kept by the profile's notification database across
      // restarts; new ones start past any of a run before.
      next_persistent_id_((base::Time::Now() - base::Time::UnixEpoch()).InMilliseconds() * 1000) {}

NotificationService::~NotificationService() {
  for (const auto& [id, shown] : shown_)
    Owners().erase(id);
}

// static
NotificationService* NotificationService::Owning(const std::string& id) {
  auto it = Owners().find(id);
  return it == Owners().end() ? nullptr : it->second;
}

void NotificationService::Display(const std::string& id, bool persistent, const GURL& origin,
                                  const blink::PlatformNotificationData& data) {
  shown_[id] = {persistent, origin};
  Owners()[id] = this;
  base::DictValue description = Describe(id, persistent, origin, data);
  // The application's to show, when it listens; the system's otherwise.
  if (TheHost().func) {
    description.Set("event", "show");
    Tell(std::move(description));
  } else {
    ShowSystemNotification(id, description);
  }
  if (!persistent)
    content::NotificationEventDispatcher::GetInstance()->DispatchNonPersistentShowEvent(id);
}

void NotificationService::DisplayNotification(const std::string& notification_id, const GURL& origin,
                                              const GURL& document_url,
                                              const blink::PlatformNotificationData& notification_data,
                                              const blink::NotificationResources& notification_resources) {
  Display(notification_id, false, origin, notification_data);
}

void NotificationService::DisplayPersistentNotification(
    const std::string& notification_id, const GURL& service_worker_origin, const GURL& origin,
    const blink::PlatformNotificationData& notification_data,
    const blink::NotificationResources& notification_resources) {
  Display(notification_id, true, origin, notification_data);
}

void NotificationService::Close(const std::string& id, bool by_page) {
  if (!shown_.erase(id))
    return;
  Owners().erase(id);
  if (TheHost().func) {
    base::DictValue event;
    event.Set("event", "close");
    event.Set("id", id);
    Tell(std::move(event));
  } else {
    CloseSystemNotification(id);
  }
}

void NotificationService::CloseNotification(const std::string& notification_id) {
  Close(notification_id, true);
}

void NotificationService::ClosePersistentNotification(const std::string& notification_id) {
  Close(notification_id, true);
}

void NotificationService::GetDisplayedNotifications(DisplayedNotificationsCallback callback) {
  std::set<std::string> ids;
  for (const auto& [id, shown] : shown_)
    ids.insert(id);
  std::move(callback).Run(std::move(ids), true);
}

void NotificationService::GetDisplayedNotificationsForOrigin(const GURL& origin,
                                                             DisplayedNotificationsCallback callback) {
  std::set<std::string> ids;
  for (const auto& [id, shown] : shown_)
    if (shown.origin.DeprecatedGetOriginAsURL() == origin.DeprecatedGetOriginAsURL())
      ids.insert(id);
  std::move(callback).Run(std::move(ids), true);
}

void NotificationService::ScheduleTrigger(base::Time timestamp) {}

base::Time NotificationService::ReadNextTriggerTimestamp() {
  return base::Time::Max();
}

int64_t NotificationService::ReadNextPersistentNotificationId() {
  return ++next_persistent_id_;
}

void NotificationService::RecordNotificationUkmEvent(const content::NotificationDatabaseData& data) {}

bool NotificationService::Clicked(const std::string& id, std::optional<int> action) {
  auto it = shown_.find(id);
  if (it == shown_.end())
    return false;
  auto* dispatcher = content::NotificationEventDispatcher::GetInstance();
  if (it->second.persistent) {
    dispatcher->DispatchNotificationClickEvent(context_, id, it->second.origin, action, std::nullopt,
                                               base::DoNothing());
  } else {
    dispatcher->DispatchNonPersistentClickEvent(id, base::DoNothing());
  }
  return true;
}

bool NotificationService::Closed(const std::string& id, bool by_user) {
  auto it = shown_.find(id);
  if (it == shown_.end())
    return false;
  const Shown shown = it->second;
  shown_.erase(it);
  Owners().erase(id);
  auto* dispatcher = content::NotificationEventDispatcher::GetInstance();
  if (shown.persistent)
    dispatcher->DispatchNotificationCloseEvent(context_, id, shown.origin, by_user, base::DoNothing());
  else
    dispatcher->DispatchNonPersistentCloseEvent(id, base::DoNothing());
  return true;
}

void SystemNotificationClicked(const std::string& id, std::optional<int> action) {
  if (NotificationService* service = NotificationService::Owning(id))
    service->Clicked(id, action);
}

void SystemNotificationClosed(const std::string& id, bool by_user) {
  if (NotificationService* service = NotificationService::Owning(id))
    service->Closed(id, by_user);
}

#if !BUILDFLAG(IS_MAC) && !BUILDFLAG(IS_WIN) && !(BUILDFLAG(IS_LINUX) && defined(ZEBKIT_COOKIES_USE_DBUS))
// No system presenter here yet: the application's function shows them.
bool ShowSystemNotification(const std::string& id, const base::DictValue& notification) {
  return false;
}
void CloseSystemNotification(const std::string& id) {}
#endif

}  // namespace zebkit

extern "C" {

void zebkit_on_notification(zebkit_notification_func func, void* data) {
  if (!content::BrowserThread::CurrentlyOn(content::BrowserThread::UI))
    return;
  zebkit::TheHost() = {func, data};
}

bool zebkit_notification_clicked(const char* id, int action) {
  if (!content::BrowserThread::CurrentlyOn(content::BrowserThread::UI) || !id)
    return false;
  zebkit::NotificationService* service = zebkit::NotificationService::Owning(id);
  return service && service->Clicked(id, action >= 0 ? std::make_optional(action) : std::nullopt);
}

bool zebkit_notification_closed(const char* id, bool by_user) {
  if (!content::BrowserThread::CurrentlyOn(content::BrowserThread::UI) || !id)
    return false;
  zebkit::NotificationService* service = zebkit::NotificationService::Owning(id);
  return service && service->Closed(id, by_user);
}

}  // extern "C"
