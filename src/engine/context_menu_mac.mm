// A page's context menu as an NSMenu (context_menu.h), on macOS: the system's
// own, as Chrome's is there.

#import <Cocoa/Cocoa.h>

#include <string>
#include <utility>
#include <vector>

#include "base/strings/sys_string_conversions.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "zebkit/engine/context_menu.h"
#include "zebkit/engine/view.h"

@interface ZebkitContextMenuTarget : NSObject
@property(nonatomic) std::string picked;
- (void)choose:(NSMenuItem*)item;
@end

@implementation ZebkitContextMenuTarget
@synthesize picked = _picked;
- (void)choose:(NSMenuItem*)item {
  _picked = base::SysNSStringToUTF8(item.representedObject);
}
@end

namespace zebkit {

bool PresentNativeMenu(zebkit_view* view, content::RenderFrameHost* frame, const gfx::Point& point,
                       std::vector<ContextMenuEntry> entries, base::OnceCallback<void(std::string)> chosen) {
  content::RenderWidgetHostView* host = frame->GetView();
  NSView* page = host ? host->GetNativeView().GetNativeNSView() : nil;
  if (!page || !page.window || entries.empty())
    return false;
  ZebkitContextMenuTarget* target = [[ZebkitContextMenuTarget alloc] init];
  NSMenu* menu = [[NSMenu alloc] initWithTitle:@""];
  menu.autoenablesItems = NO;
  for (const ContextMenuEntry& entry : entries) {
    if (entry.separator) {
      [menu addItem:[NSMenuItem separatorItem]];
      continue;
    }
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:base::SysUTF8ToNSString(entry.title)
                                                  action:@selector(choose:)
                                           keyEquivalent:@""];
    item.target = target;
    item.enabled = entry.enabled;
    item.representedObject = base::SysUTF8ToNSString(entry.id);
    [menu addItem:item];
  }
  // The point is in the page's view, top left the origin.
  const NSPoint at = NSMakePoint(point.x(), page.isFlipped ? point.y() : NSHeight(page.bounds) - point.y());
  // Modal while it is open, as the system's context menus are.
  [menu popUpMenuPositioningItem:nil atLocation:at inView:page];
  if (chosen)
    std::move(chosen).Run(target.picked);
  return true;
}

}  // namespace zebkit
