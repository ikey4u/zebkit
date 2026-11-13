// A page's context menu as an NSMenu (context_menu.h), on macOS: the system's
// own, as Chrome's is there.

#import <Cocoa/Cocoa.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/strings/sys_string_conversions.h"
#include "base/task/current_thread.h"
#include "base/task/single_thread_task_runner.h"
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
  // Shown after this returns, as on Linux and Windows: the menu's modal loop
  // is not inside the call that asked for it, and the item comes after it too.
  __weak NSView* weak_page = page;
  auto pending = std::make_shared<base::OnceCallback<void(std::string)>>(std::move(chosen));
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE, base::BindOnce(^{
    NSView* shown_in = weak_page;
    if (shown_in && shown_in.window) {
      // Modal while it is open, as the system's context menus are; the
      // engine's tasks run meanwhile.
      base::CurrentThread::ScopedAllowApplicationTasksInNativeNestedLoop allow;
      [menu popUpMenuPositioningItem:nil atLocation:at inView:shown_in];
    }
    if (*pending) {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, base::BindOnce(std::move(*pending), shown_in ? target.picked : std::string()));
    }
  }));
  return true;
}

}  // namespace zebkit
