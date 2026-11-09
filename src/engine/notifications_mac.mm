// Web notifications shown by macOS's UserNotifications framework
// (notifications.h). Only for an application with a bundle identifier: a
// command-line program has none, and the framework refuses it.

#import <Foundation/Foundation.h>
#import <UserNotifications/UserNotifications.h>

#include <optional>
#include <string>

#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/sys_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "zebkit/engine/notifications.h"

@interface ZebkitNotificationDelegate : NSObject <UNUserNotificationCenterDelegate>
@property(nonatomic) scoped_refptr<base::SingleThreadTaskRunner> runner;
@end

@implementation ZebkitNotificationDelegate
@synthesize runner = _runner;
// Shown while the application is in front too, as a page's are in Chrome.
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))handler {
  handler(UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionList);
}
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
    didReceiveNotificationResponse:(UNNotificationResponse*)response
             withCompletionHandler:(void (^)(void))handler {
  const std::string id = base::SysNSStringToUTF8(response.notification.request.identifier);
  const std::string action = base::SysNSStringToUTF8(response.actionIdentifier);
  const bool dismissed = [response.actionIdentifier isEqualToString:UNNotificationDismissActionIdentifier];
  _runner->PostTask(FROM_HERE, base::BindOnce(
                                   [](std::string id, std::string action, bool dismissed) {
                                     if (dismissed) {
                                       zebkit::SystemNotificationClosed(id, true);
                                       return;
                                     }
                                     int index = -1;
                                     std::optional<int> chosen;
                                     if (action.starts_with("action-") &&
                                         base::StringToInt(action.substr(7), &index))
                                       chosen = index;
                                     zebkit::SystemNotificationClicked(id, chosen);
                                   },
                                   id, action, dismissed));
  handler();
}
@end

namespace zebkit {

namespace {

UNUserNotificationCenter* Center() {
  if (!NSBundle.mainBundle.bundleIdentifier)
    return nil;
  static ZebkitNotificationDelegate* delegate = nil;
  UNUserNotificationCenter* center = UNUserNotificationCenter.currentNotificationCenter;
  if (!delegate) {
    delegate = [[ZebkitNotificationDelegate alloc] init];
    delegate.runner = base::SingleThreadTaskRunner::GetCurrentDefault();
    center.delegate = delegate;
    [center requestAuthorizationWithOptions:UNAuthorizationOptionAlert | UNAuthorizationOptionSound
                          completionHandler:^(BOOL granted, NSError* error){
                          }];
  }
  return center;
}

}  // namespace

bool ShowSystemNotification(const std::string& id, const base::DictValue& notification) {
  UNUserNotificationCenter* center = Center();
  if (!center)
    return false;
  UNMutableNotificationContent* content = [[UNMutableNotificationContent alloc] init];
  if (const std::string* title = notification.FindString("title"))
    content.title = base::SysUTF8ToNSString(*title);
  if (const std::string* body = notification.FindString("body"))
    content.body = base::SysUTF8ToNSString(*body);
  if (const std::string* origin = notification.FindString("origin"))
    content.subtitle = base::SysUTF8ToNSString(*origin);
  if (!notification.FindBool("silent").value_or(false))
    content.sound = UNNotificationSound.defaultSound;
  if (const base::ListValue* actions = notification.FindList("actions"); actions && !actions->empty()) {
    NSMutableArray* buttons = [NSMutableArray array];
    for (size_t i = 0; i < actions->size(); ++i) {
      const std::string* title = (*actions)[i].GetDict().FindString("title");
      [buttons addObject:[UNNotificationAction
                             actionWithIdentifier:[NSString stringWithFormat:@"action-%zu", i]
                                            title:base::SysUTF8ToNSString(title ? *title : "")
                                          options:UNNotificationActionOptionNone]];
    }
    NSString* category = [@"zebkit-" stringByAppendingString:base::SysUTF8ToNSString(id)];
    [center setNotificationCategories:[NSSet setWithObject:[UNNotificationCategory
                                                               categoryWithIdentifier:category
                                                                              actions:buttons
                                                                    intentIdentifiers:@[]
                                                                              options:UNNotificationCategoryOptionCustomDismissAction]]];
    content.categoryIdentifier = category;
  }
  UNNotificationRequest* request = [UNNotificationRequest requestWithIdentifier:base::SysUTF8ToNSString(id)
                                                                        content:content
                                                                        trigger:nil];
  [center addNotificationRequest:request withCompletionHandler:^(NSError* error){
         }];
  return true;
}

void CloseSystemNotification(const std::string& id) {
  if (UNUserNotificationCenter* center = Center()) {
    NSString* identifier = base::SysUTF8ToNSString(id);
    [center removeDeliveredNotificationsWithIdentifiers:@[ identifier ]];
    [center removePendingNotificationRequestsWithIdentifiers:@[ identifier ]];
  }
}

}  // namespace zebkit
