// macOS's part of off-screen views (offscreen.cc).

#include "zebkit/engine/offscreen_mac.h"

#import <AppKit/AppKit.h>

namespace zebkit {

namespace {

// Not to be seen on the screen, nor to take the mouse there, nor to be in
// the window lists; but on the screen, which is what makes it draw.
void MakeUnseen(NSWindow* window) {
  window.alphaValue = 0;
  window.opaque = NO;
  window.backgroundColor = NSColor.clearColor;
  window.ignoresMouseEvents = YES;
  window.hasShadow = NO;
  window.excludedFromWindowsMenu = YES;
  window.collectionBehavior = NSWindowCollectionBehaviorTransient |
                              NSWindowCollectionBehaviorIgnoresCycle |
                              NSWindowCollectionBehaviorFullScreenAuxiliary;
}

}  // namespace

void HideOffscreenPopupWindow(gfx::NativeView view) {
  // A popup widget on macOS is a borderless window of its own, which the
  // engine draws for the view's frames. Transparent rather than hidden or
  // moved away, which would stop its drawing.
  NSWindow* window = view.GetNativeNSView().window;
  if (window)
    MakeUnseen(window);
}

}  // namespace zebkit
