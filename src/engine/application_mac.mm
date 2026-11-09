// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "zebkit/engine/application_mac.h"

#include "base/auto_reset.h"
#include "base/observer_list.h"
#include "content/public/browser/native_event_processor_mac.h"
#include "content/public/browser/native_event_processor_observer_mac.h"

#include <memory>

#include <objc/runtime.h>

#include "base/no_destructor.h"
#include "zebkit/engine/engine.h"

#include "content/public/browser/browser_accessibility_state.h"
#include "content/public/browser/scoped_accessibility_mode.h"
#include "ui/accessibility/ax_mode.h"

@interface ZebkitApplication () <NativeEventProcessor>
@end

@implementation ZebkitApplication {
  base::ObserverList<content::NativeEventProcessorObserver>::Unchecked
      _observers;

  BOOL _handlingSendEvent;
  // The pages' accessibility trees, while an assistive technology asks for
  // them (docs/SYSTEM_INTEGRATION.md, 11), as Chrome's application has them.
  std::unique_ptr<content::ScopedAccessibilityMode> _completeAccessibility;
  std::unique_ptr<content::ScopedAccessibilityMode> _basicAccessibility;
}

// VoiceOver sets AXEnhancedUserInterface; other clients, and Electron's
// applications, AXManualAccessibility: the complete tree while it is set.
- (void)accessibilitySetValue:(id)value forAttribute:(NSString*)attribute {
  if ([attribute isEqualToString:@"AXEnhancedUserInterface"] ||
      [attribute isEqualToString:@"AXManualAccessibility"]) {
    if ([value boolValue] && !_completeAccessibility) {
      _completeAccessibility =
          content::BrowserAccessibilityState::GetInstance()->CreateScopedModeForProcess(ui::kAXModeComplete);
    } else if (![value boolValue]) {
      _completeAccessibility.reset();
    }
    if ([attribute isEqualToString:@"AXManualAccessibility"])
      return;
  }
  [super accessibilitySetValue:value forAttribute:attribute];
}

// Voice Control and the like only read: the basic tree once they do, as Apple
// recommends and Chrome does.
- (NSAccessibilityRole)accessibilityRole {
  if (!_basicAccessibility && !_completeAccessibility) {
    _basicAccessibility = content::BrowserAccessibilityState::GetInstance()->CreateScopedModeForProcess(
        ui::kAXModeBasic | ui::AXMode::kNativeAPIs);
  }
  return [super accessibilityRole];
}

- (BOOL)isHandlingSendEvent {
  return _handlingSendEvent;
}

- (void)sendEvent:(NSEvent*)event {
  base::AutoReset<BOOL> scoper(&_handlingSendEvent, YES);

  content::ScopedNotifyNativeEventProcessorObserver scopedObserverNotifier(
      &_observers, event);
  [super sendEvent:event];
}

- (void)setHandlingSendEvent:(BOOL)handlingSendEvent {
  _handlingSendEvent = handlingSendEvent;
}

- (void)addNativeEventProcessorObserver:
    (content::NativeEventProcessorObserver*)observer {
  _observers.AddObserver(observer);
}

- (void)removeNativeEventProcessorObserver:
    (content::NativeEventProcessorObserver*)observer {
  _observers.RemoveObserver(observer);
}

@end

namespace zebkit {

namespace {

IMP g_host_set_value = nullptr;

std::unique_ptr<content::ScopedAccessibilityMode>& HostComplete() {
  static base::NoDestructor<std::unique_ptr<content::ScopedAccessibilityMode>> mode;
  return *mode;
}

// NSApplication's accessibilitySetValue:forAttribute:, while the engine runs
// in an application's own NSApplication: what ZebkitApplication does, then
// the system's own.
void HostSetValue(id self, SEL command, id value, NSString* attribute) {
  const bool manual = [attribute isEqualToString:@"AXManualAccessibility"];
  if (manual || [attribute isEqualToString:@"AXEnhancedUserInterface"]) {
    if ([value boolValue] && !HostComplete())
      HostComplete() = content::BrowserAccessibilityState::GetInstance()->CreateScopedModeForProcess(ui::kAXModeComplete);
    else if (![value boolValue])
      HostComplete().reset();
    if (manual)
      return;
  }
  reinterpret_cast<void (*)(id, SEL, id, NSString*)>(g_host_set_value)(self, command, value, attribute);
}

}  // namespace

void HookHostAccessibility(bool on) {
  if ([NSApp isKindOfClass:[ZebkitApplication class]])
    return;
  Method method = class_getInstanceMethod([NSApplication class], @selector(accessibilitySetValue:forAttribute:));
  if (!method)
    return;
  if (on && !g_host_set_value) {
    g_host_set_value = method_setImplementation(method, reinterpret_cast<IMP>(&HostSetValue));
  } else if (!on && g_host_set_value) {
    method_setImplementation(method, g_host_set_value);
    g_host_set_value = nullptr;
    HostComplete().reset();
  }
}

}  // namespace zebkit
