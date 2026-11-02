// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// A window on macOS: an NSWindow, or for an off-screen view (and every view
// under --headless) a view in no window, drawn at the size it is given as
// Chromium's headless mode does.

#include "zebkit/engine/window_platform.h"

#import <Cocoa/Cocoa.h>

#import "base/apple/foundation_util.h"
#include "base/check_op.h"
#include "base/containers/flat_map.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/sys_string_conversions.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "ui/display/screen.h"
#include "ui/gfx/native_ui_types.h"
#include "zebkit/engine/include/zebkit/zebkit.h"
#include "zebkit/engine/window.h"

// Receives notification that the window is closing so that it can start the
// tear-down process.
@interface ZebkitWindowDelegate : NSObject <NSWindowDelegate>

@property(readonly) NSWindow* window;

- (id)initWithWindow:(zebkit::Window*)owner window:(NSWindow*)window;

@end

@implementation ZebkitWindowDelegate {
  raw_ptr<zebkit::Window> _owner;
  NSWindow* __strong _window;
}

@synthesize window = _window;

- (id)initWithWindow:(zebkit::Window*)owner window:(NSWindow*)window {
  if ((self = [super init])) {
    _owner = owner;
    _window = window;
    window.delegate = self;
  }
  return self;
}

// Called when the window is about to close. Gets rid of its Window. By
// returning YES, we allow the window to be removed from the screen.
- (BOOL)windowShouldClose:(id)sender {
  CHECK_EQ(base::apple::ObjCCastStrict<NSWindow>(sender), _window);
  // Don't leave a dangling pointer if the window lives beyond
  // this method. See crbug.com/719830.
  _window.delegate = nil;
  _window = nil;
  _owner.ClearAndDelete();
  return YES;
}

@end

namespace zebkit {

namespace {

class NativePlatform : public WindowPlatform {
 public:
  void Create(Window* window, const gfx::Size& initial_size) override {
    NSRect content_rect = NSMakeRect(0, 0, initial_size.width(), initial_size.height());
    NSUInteger style_mask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable |
                            NSWindowStyleMaskResizable;
    NSWindow* ns_window =
        [[NSWindow alloc] initWithContentRect:content_rect
                                    styleMask:style_mask
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    ns_window.title = @"";
    // The smallest it may be without the view bindings wrecked; in window
    // coordinates. zebkit_view_set_size_limits() has another.
    ns_window.contentMinSize =
        [ns_window.contentView convertSize:NSMakeSize(400, 300) toView:nil];
    ns_window.collectionBehavior |= NSWindowCollectionBehaviorFullScreenPrimary;
    // The delegate cleans up rather than the window as it closes, from the
    // autorelease pool, so that the Window is not on the stack during it (a
    // close from script). releasedWhenClosed == YES is incompatible with ARC.
    ns_window.releasedWhenClosed = NO;
    windows_[window] = [[ZebkitWindowDelegate alloc] initWithWindow:window
                                                             window:ns_window];
    [ns_window makeKeyAndOrderFront:nil];
  }

  void SetContents(Window* window) override {
    NSView* web_view = window->web_contents()->GetNativeView().GetNativeNSView();
    web_view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    NSWindow* ns_window = windows_[window].window;
    [ns_window.contentView addSubview:web_view];
    web_view.frame = ns_window.contentView.bounds;
    web_view.needsDisplay = YES;
  }

  void ResizeContents(Window* window, const gfx::Size& content_size) override {
    windows_[window].window.contentView.frame =
        NSMakeRect(0, 0, content_size.width(), content_size.height());
  }

  void SetTitle(Window* window, const std::u16string& title) override {
    windows_[window].window.title = base::SysUTF16ToNSString(title);
  }

  gfx::NativeWindow GetNativeWindow(Window* window) override {
    return gfx::NativeWindow(windows_[window].window);
  }

  void CleanUp(Window* window) override { windows_.erase(window); }

  bool Destroy(Window* window) override {
    [windows_[window].window performClose:nil];
    return true;  // performClose: destroys the Window.
  }

  void Activate(Window* window) override {
    // This focuses the main frame's widget in the window, but does not make
    // the window itself active.
    window->web_contents()->Focus();
    // The active window of the application, and with an active application the
    // window's widgets are active.
    [windows_[window].window makeKeyAndOrderFront:nil];
    // The application active, so that focus can move between windows and the
    // page gets focus and blur events.
    [NSApp activateIgnoringOtherApps:YES];
  }

  void SetSizeLimits(Window* window,
                     const gfx::Size& minimum,
                     bool resizable) override {
    NSWindow* ns_window = windows_[window].window;
    if (!ns_window)
      return;
    ns_window.contentMinSize = NSMakeSize(minimum.width(), minimum.height());
    if (resizable)
      ns_window.styleMask |= NSWindowStyleMaskResizable;
    else
      ns_window.styleMask &= ~NSWindowStyleMaskResizable;
  }

 private:
  base::flat_map<Window*, ZebkitWindowDelegate*> windows_;
};

class NoWindowPlatform : public WindowPlatform {
 public:
  void Create(Window* window, const gfx::Size& initial_size) override {
    sizes_[window] = initial_size;
  }
  void SetContents(Window* window) override {
    NSView* view = window->web_contents()->GetNativeView().GetNativeNSView();
    view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    Resize(window);
    window->web_contents()->WasShown();
  }
  void ResizeContents(Window* window, const gfx::Size& content_size) override {
    sizes_[window] = content_size;
    Resize(window);
  }
  void MainFrameCreated(Window* window) override { Resize(window); }
  void SetTitle(Window* window, const std::u16string& title) override {}
  gfx::NativeWindow GetNativeWindow(Window* window) override {
    return gfx::NativeWindow();
  }
  void CleanUp(Window* window) override { sizes_.erase(window); }
  bool Destroy(Window* window) override { return false; }

 private:
  void Resize(Window* window) {
    content::WebContents* contents = window->web_contents();
    if (!contents)
      return;
    const gfx::Size size = sizes_[window];
    NSView* view = contents->GetNativeView().GetNativeNSView();
    view.frame = NSMakeRect(0, 0, size.width(), size.height());
    if (content::RenderWidgetHostView* host = contents->GetRenderWidgetHostView())
      host->SetWindowFrameInScreen(gfx::Rect(size));
  }

  base::flat_map<Window*, gfx::Size> sizes_;
};

NativePlatform* g_native = nullptr;
NoWindowPlatform* g_no_window = nullptr;
display::ScopedNativeScreen* g_screen = nullptr;

}  // namespace

void InitializePlatforms() {
  g_screen = new display::ScopedNativeScreen();
  g_native = new NativePlatform();
  g_no_window = new NoWindowPlatform();
}

void ShutdownPlatforms() {
  delete g_no_window;
  g_no_window = nullptr;
  delete g_native;
  g_native = nullptr;
  delete g_screen;
  g_screen = nullptr;
}

WindowPlatform* PlatformFor(Window::Kind kind) {
  return kind == Window::Kind::kOffscreen || zebkit_is_headless()
             ? static_cast<WindowPlatform*>(g_no_window)
             : g_native;
}

}  // namespace zebkit
