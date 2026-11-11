// The system's share sheet for navigator.share() (share.h), on macOS:
// NSSharingServicePicker at the page's window.

#import <Cocoa/Cocoa.h>

#include <memory>
#include <utility>

#include "base/strings/sys_string_conversions.h"
#include "content/public/browser/web_contents.h"
#include "zebkit/engine/share.h"

@interface ZebkitShareDelegate : NSObject <NSSharingServicePickerDelegate, NSSharingServiceDelegate>
@property(nonatomic) std::shared_ptr<base::OnceCallback<void(bool)>> done;
@end

@implementation ZebkitShareDelegate
@synthesize done = _done;
- (void)answer:(bool)shared {
  if (_done && *_done)
    std::move(*_done).Run(shared);
}
- (void)sharingServicePicker:(NSSharingServicePicker*)picker didChooseSharingService:(NSSharingService*)service {
  if (!service) {
    [self answer:false];
    return;
  }
  service.delegate = self;
}
- (void)sharingService:(NSSharingService*)service didShareItems:(NSArray*)items {
  [self answer:true];
}
- (void)sharingService:(NSSharingService*)service didFailToShareItems:(NSArray*)items error:(NSError*)error {
  [self answer:false];
}
@end

namespace zebkit {

bool ShowSystemShareSheet(content::WebContents* contents, const std::string& title, const std::string& text,
                          const std::string& url, base::OnceCallback<void(bool)>& done) {
  NSView* view = contents->GetNativeView().GetNativeNSView();
  if (!view || !view.window)
    return false;
  NSMutableArray* items = [NSMutableArray array];
  if (!text.empty())
    [items addObject:base::SysUTF8ToNSString(text)];
  if (!url.empty())
    if (NSURL* link = [NSURL URLWithString:base::SysUTF8ToNSString(url)])
      [items addObject:link];
  if (!items.count && !title.empty())
    [items addObject:base::SysUTF8ToNSString(title)];
  if (!items.count)
    return false;
  static ZebkitShareDelegate* delegate = nil;
  // One sheet's delegate at a time: the share before this one, still
  // unanswered, is answered as not shared rather than never.
  if (delegate)
    [delegate answer:false];
  delegate = [[ZebkitShareDelegate alloc] init];
  delegate.done = std::make_shared<base::OnceCallback<void(bool)>>(std::move(done));
  NSSharingServicePicker* picker = [[NSSharingServicePicker alloc] initWithItems:items];
  picker.delegate = delegate;
  [picker showRelativeToRect:NSMakeRect(NSMidX(view.bounds), NSMidY(view.bounds), 1, 1)
                      ofView:view
               preferredEdge:NSRectEdgeMinY];
  return true;
}

}  // namespace zebkit
