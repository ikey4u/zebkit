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
#include "skia/ext/skia_utils_mac.h"
#include "zebkit/engine/include/zebkit/zebkit.h"
#include "zebkit/engine/window.h"
#include "zebkit/engine/window_options.h"

// A window that is dragged from where the page's or the application's drag
// regions say (<zebkit/window.h>): a press there is the system's drag, with
// its snapping and Stage Manager, and a double click there what the person
// set for a title bar's (System Settings, "double-click a window's title bar").
@interface ZebkitNSWindow : NSWindow
@property(nonatomic) zebkit::Window* owner;
@end

@implementation ZebkitNSWindow
@synthesize owner = _owner;

- (BOOL)inDragRegion:(NSEvent*)event {
  if (!_owner || _owner->drag_regions().empty())
    return NO;
  NSView* content = self.contentView;
  NSPoint point = [content convertPoint:event.locationInWindow fromView:nil];
  // The content area's top left is the origin, as the regions have it.
  const gfx::Point at(point.x, content.isFlipped ? point.y : NSHeight(content.bounds) - point.y);
  return _owner->InDragRegion(at);
}

- (void)sendEvent:(NSEvent*)event {
  if (event.type == NSEventTypeLeftMouseDown && [self inDragRegion:event]) {
    if (event.clickCount == 2) {
      NSString* action = [NSUserDefaults.standardUserDefaults stringForKey:@"AppleActionOnDoubleClick"];
      if ([action isEqualToString:@"Minimize"])
        [self performMiniaturize:nil];
      else if (![action isEqualToString:@"None"])
        [self performZoom:nil];
      return;
    }
    [self performWindowDragWithEvent:event];
    return;
  }
  [super sendEvent:event];
}
@end

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

- (void)changed {
  if (_owner)
    _owner->PlatformChanged();
}
- (void)windowDidResize:(NSNotification*)notification { [self changed]; }
- (void)windowDidMove:(NSNotification*)notification { [self changed]; }
- (void)windowDidMiniaturize:(NSNotification*)notification { [self changed]; }
- (void)windowDidDeminiaturize:(NSNotification*)notification { [self changed]; }
- (void)windowDidEnterFullScreen:(NSNotification*)notification { [self changed]; }
- (void)windowDidExitFullScreen:(NSNotification*)notification { [self changed]; }
- (void)windowDidBecomeKey:(NSNotification*)notification { [self changed]; }
- (void)windowDidResignKey:(NSNotification*)notification { [self changed]; }
- (void)windowDidChangeOcclusionState:(NSNotification*)notification { [self changed]; }

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
  if ([_window isKindOfClass:[ZebkitNSWindow class]])
    static_cast<ZebkitNSWindow*>(_window).owner = nullptr;
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
    const WindowOptions& options = window->options();
    // "overlay" and "none": the page from the window's top, under the
    // system's title bar made transparent; a titled window still, with its
    // key-window behaviour, the Window menu's commands, resizing, fullscreen
    // and tiling. Borderless would lose them.
    const bool custom = options.frame != WindowOptions::Frame::kSystem;
    if (custom)
      style_mask |= NSWindowStyleMaskFullSizeContentView;
    ZebkitNSWindow* ns_window =
        [[ZebkitNSWindow alloc] initWithContentRect:content_rect
                                          styleMask:style_mask
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    ns_window.owner = window;
    ns_window.title = @"";
    if (custom) {
      ns_window.titlebarAppearsTransparent = YES;
      ns_window.titleVisibility = NSWindowTitleHidden;
      // A taller title bar as the system makes one, its buttons placed by it:
      // the unified toolbar styles.
      if (options.controls_height > 28) {
        ns_window.toolbar = [[NSToolbar alloc] initWithIdentifier:@"zebkit"];
        ns_window.toolbarStyle = options.controls_height >= 46 ? NSWindowToolbarStyleUnified
                                                               : NSWindowToolbarStyleUnifiedCompact;
      }
      if (options.frame == WindowOptions::Frame::kNone) {
        for (NSWindowButton kind : {NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton})
          [ns_window standardWindowButton:kind].hidden = YES;
      }
    }
    // No minimum of the engine's, as on the other systems:
    // zebkit_view_set_size_limits() and minWidth/minHeight give one.
    ns_window.collectionBehavior |= NSWindowCollectionBehaviorFullScreenPrimary;
    // The delegate cleans up rather than the window as it closes, from the
    // autorelease pool, so that the Window is not on the stack during it (a
    // close from script). releasedWhenClosed == YES is incompatible with ARC.
    ns_window.releasedWhenClosed = NO;
    windows_[window] = [[ZebkitWindowDelegate alloc] initWithWindow:window
                                                             window:ns_window];
    ApplyOptions(window, ns_window);
    if (options.position)
      [ns_window setFrameTopLeftPoint:ScreenPoint(*options.position)];
    else
      [ns_window center];
    // Shown once its page is in it (SetContents()), not before: the first
    // frame the person sees is the window as configured.
  }

  void SetContents(Window* window) override {
    NSView* web_view = window->web_contents()->GetNativeView().GetNativeNSView();
    web_view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    NSWindow* ns_window = windows_[window].window;
    [ns_window.contentView addSubview:web_view];
    web_view.frame = ns_window.contentView.bounds;
    web_view.needsDisplay = YES;
    if (window->options().show)
      [ns_window makeKeyAndOrderFront:nil];
    window->PlatformChanged();
  }

  void ResizeContents(Window* window, const gfx::Size& content_size) override {
    // The window's content area that size, its frame around it: the content
    // view's frame alone left the window as it was.
    [windows_[window].window setContentSize:NSMakeSize(content_size.width(), content_size.height())];
  }

  void SetTitle(Window* window, const std::u16string& title) override {
    windows_[window].window.title = base::SysUTF16ToNSString(title);
  }

  gfx::NativeWindow GetNativeWindow(Window* window) override {
    return gfx::NativeWindow(windows_[window].window);
  }

  void CleanUp(Window* window) override { windows_.erase(window); }

  bool Destroy(Window* window) override {
    auto it = windows_.find(window);
    if (it == windows_.end())
      return false;
    ZebkitWindowDelegate* delegate = it->second;
    NSWindow* ns_window = delegate.window;
    if (!ns_window)
      return false;
    // Not performClose:, which only beeps when a sheet (a file panel, a
    // dialog) is attached, and leaves the Window: its sheets ended, then
    // what the close button does, which destroys the Window.
    // A few passes, for sheets on sheets: one that does not end (its owner
    // refuses) does not hold the close up for ever.
    for (int pass = 0; pass < 8; ++pass) {
      NSWindow* sheet = ns_window.attachedSheet;
      if (!sheet)
        break;
      [ns_window endSheet:sheet];
    }
    [delegate windowShouldClose:ns_window];
    [ns_window close];
    return true;
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

  WindowCapabilities Capabilities(Window* window) override {
    WindowCapabilities capabilities;
    capabilities.overlay = true;
    capabilities.none = true;
    capabilities.position = true;
    capabilities.actions = true;
    capabilities.controls_drawn_by_engine = false;
    capabilities.controls_height = true;
    capabilities.appearance = true;
    return capabilities;
  }

  WindowGeometry Geometry(Window* window) override {
    WindowGeometry geometry;
    NSWindow* ns_window = windows_.contains(window) ? windows_[window].window : nil;
    if (!ns_window)
      return WindowPlatform::Geometry(window);
    const bool fullscreen = ns_window.styleMask & NSWindowStyleMaskFullScreen;
    geometry.state = fullscreen                ? WindowGeometry::State::kFullscreen
                     : ns_window.miniaturized ? WindowGeometry::State::kMinimized
                     : ns_window.zoomed       ? WindowGeometry::State::kMaximized
                                              : WindowGeometry::State::kNormal;
    geometry.visible = ns_window.visible;
    geometry.focused = ns_window.keyWindow;
    const NSRect frame = ns_window.frame;
    const CGFloat top = NSMaxY(NSScreen.screens.firstObject.frame);
    geometry.bounds = gfx::Rect(NSMinX(frame), top - NSMaxY(frame), NSWidth(frame), NSHeight(frame));
    NSView* content = ns_window.contentView;
    geometry.content = gfx::Size(NSWidth(content.bounds), NSHeight(content.bounds));
    geometry.scale = ns_window.backingScaleFactor;
    if (window->options().frame == WindowOptions::Frame::kOverlay && !fullscreen) {
      // The title bar is what the content layout leaves at the top; the
      // buttons are the system's, where it put them.
      const CGFloat height = NSHeight(content.bounds) - NSHeight(ns_window.contentLayoutRect);
      int right = 0;
      for (NSWindowButton kind : {NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton}) {
        NSButton* button = [ns_window standardWindowButton:kind];
        if (!button || button.hidden)
          continue;
        NSRect rect = [content convertRect:button.bounds fromView:button];
        const gfx::Rect converted(NSMinX(rect), content.isFlipped ? NSMinY(rect)
                                                                  : NSHeight(content.bounds) - NSMaxY(rect),
                                  NSWidth(rect), NSHeight(rect));
        geometry.controls.push_back(converted);
        right = std::max(right, converted.right());
      }
      // The free area starts past the buttons, with the system's own gap.
      const int start = right ? right + 8 : 0;
      geometry.titlebar = gfx::Rect(start, 0, std::max(0, geometry.content.width() - start), height);
    }
    return geometry;
  }

  bool PerformAction(Window* window, WindowAction action, std::string* error) override {
    NSWindow* ns_window = windows_.contains(window) ? windows_[window].window : nil;
    if (!ns_window)
      return WindowPlatform::PerformAction(window, action, error);
    const bool fullscreen = ns_window.styleMask & NSWindowStyleMaskFullScreen;
    switch (action) {
      case WindowAction::kShow: [ns_window makeKeyAndOrderFront:nil]; break;
      case WindowAction::kHide: [ns_window orderOut:nil]; break;
      case WindowAction::kMinimize: [ns_window miniaturize:nil]; break;
      case WindowAction::kMaximize:
        if (!(ns_window.styleMask & NSWindowStyleMaskResizable)) {
          *error = "unsupported: the window is not resizable";
          return false;
        }
        if (!ns_window.zoomed)
          [ns_window zoom:nil];
        break;
      case WindowAction::kRestore:
        if (ns_window.miniaturized)
          [ns_window deminiaturize:nil];
        else if (fullscreen)
          [ns_window toggleFullScreen:nil];
        else if (ns_window.zoomed)
          [ns_window zoom:nil];
        break;
      case WindowAction::kFullscreen:
        if (!fullscreen)
          [ns_window toggleFullScreen:nil];
        break;
      case WindowAction::kExitFullscreen:
        if (fullscreen)
          [ns_window toggleFullScreen:nil];
        break;
      case WindowAction::kClose: break;  // zebkit_view_request_close(), in window_api.cc.
    }
    // The outcome comes as the delegate's notifications; miniaturize: and the
    // like may tell nothing when nothing changes.
    window->PlatformChanged();
    return true;
  }

  void OptionsChanged(Window* window) override {
    NSWindow* ns_window = windows_.contains(window) ? windows_[window].window : nil;
    if (!ns_window)
      return;
    ApplyOptions(window, ns_window);
    if (window->options().position)
      [ns_window setFrameTopLeftPoint:ScreenPoint(*window->options().position)];
  }

  void SetFullscreen(Window* window, bool fullscreen) override {
    NSWindow* ns_window = windows_.contains(window) ? windows_[window].window : nil;
    if (!ns_window || static_cast<bool>(ns_window.styleMask & NSWindowStyleMaskFullScreen) == fullscreen)
      return;
    [ns_window toggleFullScreen:nil];
  }

 private:
  // A point on the screen, top left the origin, in AppKit's coordinates.
  static NSPoint ScreenPoint(const gfx::Point& point) {
    const CGFloat top = NSMaxY(NSScreen.screens.firstObject.frame);
    return NSMakePoint(point.x(), top - point.y());
  }

  // What can change while the window is open: its colour and its look.
  static void ApplyOptions(Window* window, NSWindow* ns_window) {
    const WindowOptions& options = window->options();
    if (options.background) {
      ns_window.backgroundColor = skia::SkColorToSRGBNSColor(*options.background);
      if (content::RenderWidgetHostView* view = window->web_contents()->GetRenderWidgetHostView())
        view->SetBackgroundColor(*options.background);
    } else {
      // null: the system's colour again, as on views.
      ns_window.backgroundColor = NSColor.windowBackgroundColor;
    }
    switch (options.appearance) {
      case WindowOptions::Appearance::kSystem: ns_window.appearance = nil; break;
      case WindowOptions::Appearance::kLight:
        ns_window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];
        break;
      case WindowOptions::Appearance::kDark:
        ns_window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
        break;
    }
  }

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
