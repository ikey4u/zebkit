// The Dock (dock.h): its tile's badge and a progress bar over the icon, and
// the person's attention.

#import <Cocoa/Cocoa.h>

#include "base/strings/sys_string_conversions.h"
#include "zebkit/engine/dock.h"

@interface ZebkitDockTileView : NSView
@property(nonatomic) double progress;
@end

@implementation ZebkitDockTileView
@synthesize progress = _progress;
- (void)drawRect:(NSRect)dirty {
  [NSApp.applicationIconImage drawInRect:self.bounds];
  if (_progress < 0)
    return;
  // A bar along the icon's foot, as the Finder's own copies show one.
  const NSRect bar = NSMakeRect(NSWidth(self.bounds) * 0.1, NSHeight(self.bounds) * 0.08,
                                NSWidth(self.bounds) * 0.8, NSHeight(self.bounds) * 0.1);
  [[NSColor colorWithWhite:0 alpha:0.5] setFill];
  [[NSBezierPath bezierPathWithRoundedRect:bar xRadius:NSHeight(bar) / 2 yRadius:NSHeight(bar) / 2] fill];
  NSRect done = bar;
  done.size.width *= std::min(1.0, _progress);
  [NSColor.controlAccentColor setFill];
  [[NSBezierPath bezierPathWithRoundedRect:done xRadius:NSHeight(bar) / 2 yRadius:NSHeight(bar) / 2] fill];
}
@end

namespace zebkit {

void ApplyDockState(const DockState& state) {
  NSDockTile* tile = NSApp.dockTile;
  tile.badgeLabel = state.badge.empty() ? nil : base::SysUTF8ToNSString(state.badge);
  if (state.progress < 0) {
    tile.contentView = nil;
  } else {
    ZebkitDockTileView* view = [tile.contentView isKindOfClass:[ZebkitDockTileView class]]
                                   ? (ZebkitDockTileView*)tile.contentView
                                   : [[ZebkitDockTileView alloc] initWithFrame:NSMakeRect(0, 0, 128, 128)];
    view.progress = state.progress;
    tile.contentView = view;
  }
  [tile display];
}

void RequestDockAttention(bool critical) {
  [NSApp requestUserAttention:critical ? NSCriticalRequest : NSInformationalRequest];
}

bool DockMenuSupported() {
  return true;
}

}  // namespace zebkit
