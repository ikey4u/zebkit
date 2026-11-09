// The tray on macOS (tray.h): an NSStatusItem in the menu bar.

#import <Cocoa/Cocoa.h>

#include <memory>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/strings/sys_string_conversions.h"
#include "skia/ext/skia_utils_mac.h"
#include "zebkit/engine/tray.h"

@interface ZebkitTrayTarget : NSObject
@property(nonatomic) zebkit::Tray* tray;
- (void)clicked:(id)sender;
- (void)chose:(NSMenuItem*)item;
@end

@implementation ZebkitTrayTarget
@synthesize tray = _tray;
- (void)clicked:(id)sender {
  if (_tray)
    _tray->Clicked();
}
- (void)chose:(NSMenuItem*)item {
  if (_tray)
    _tray->Chosen(base::SysNSStringToUTF8(item.representedObject));
}
@end

namespace zebkit {

namespace {

void AddItems(NSMenu* menu, const std::vector<MenuItem>& items, ZebkitTrayTarget* target) {
  for (const MenuItem& item : items) {
    if (item.separator) {
      [menu addItem:[NSMenuItem separatorItem]];
      continue;
    }
    NSMenuItem* entry = [[NSMenuItem alloc] initWithTitle:base::SysUTF8ToNSString(item.title)
                                                   action:@selector(chose:)
                                            keyEquivalent:@""];
    entry.target = target;
    entry.enabled = item.enabled;
    entry.state = item.checked ? NSControlStateValueOn : NSControlStateValueOff;
    entry.representedObject = base::SysUTF8ToNSString(item.id);
    if (!item.items.empty()) {
      entry.submenu = [[NSMenu alloc] initWithTitle:entry.title];
      entry.submenu.autoenablesItems = NO;
      AddItems(entry.submenu, item.items, target);
    }
    [menu addItem:entry];
  }
}

class MacTray : public TrayPlatform {
 public:
  explicit MacTray(Tray* tray) : tray_(tray) {
    item_ = [NSStatusBar.systemStatusBar statusItemWithLength:NSSquareStatusItemLength];
    target_ = [[ZebkitTrayTarget alloc] init];
    target_.tray = tray;
    item_.button.target = target_;
    item_.button.action = @selector(clicked:);
    SetImage(tray->image());
    SetToolTip(tray->tooltip());
    MenuChanged();
  }
  ~MacTray() override {
    target_.tray = nullptr;
    [NSStatusBar.systemStatusBar removeStatusItem:item_];
  }
  void SetImage(const SkBitmap& image) override {
    NSImage* picture = skia::SkBitmapToNSImage(image);
    // The menu bar's height: a picture of its own size, as the system scales it.
    picture.size = NSMakeSize(18, 18);
    [picture setTemplate:YES];
    item_.button.image = picture;
  }
  void SetToolTip(const std::u16string& tooltip) override {
    item_.button.toolTip = base::SysUTF16ToNSString(tooltip);
  }
  void MenuChanged() override {
    // With a menu, a click opens it, as the system's own items do; without one
    // a click is the application's.
    if (tray_->menu().empty()) {
      item_.menu = nil;
      return;
    }
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@""];
    menu.autoenablesItems = NO;
    AddItems(menu, tray_->menu(), target_);
    item_.menu = menu;
  }

 private:
  raw_ptr<Tray> tray_;
  NSStatusItem* __strong item_;
  ZebkitTrayTarget* __strong target_;
};

}  // namespace

std::unique_ptr<TrayPlatform> CreateTrayPlatform(Tray* tray) {
  return std::make_unique<MacTray>(tray);
}

}  // namespace zebkit
