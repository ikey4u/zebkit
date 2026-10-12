/*
 * Views with a window of their own, on macOS: zebkit-view.h, which says why.
 * Built into WebKit.framework (patches/2010).
 */

#import "config.h"
#import "zebkit-view.h"

#import "WKWebViewPrivate.h"
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <wtf/HashMap.h>
#import <wtf/NeverDestroyed.h>
#import <wtf/RetainPtr.h>

// The window of a view that is not shown. A page lays out, paints and takes
// input only in a window, and runs at full speed only in the key window, so
// this one is in the window list, off every screen, and says it is key.
@interface ZebkitHiddenWindow : NSWindow
@end

@implementation ZebkitHiddenWindow

- (BOOL)isKeyWindow
{
    return YES;
}

- (BOOL)canBecomeKeyWindow
{
    return YES;
}

@end

static constexpr int defaultViewWidth = 1024;
static constexpr int defaultViewHeight = 768;

static HashMap<WKWebView *, RetainPtr<NSWindow>>& windows()
{
    static NeverDestroyed<HashMap<WKWebView *, RetainPtr<NSWindow>>> windows;
    return windows;
}

// The menus a Mac application is expected to have, and without which
// Command-Q and copying and pasting in a page do nothing.
static void addMenus()
{
    auto bar = adoptNS([[NSMenu alloc] init]);
    auto application = adoptNS([[NSMenu alloc] init]);
    [application addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];
    [[bar addItemWithTitle:@"" action:nullptr keyEquivalent:@""] setSubmenu:application.get()];
    auto edit = adoptNS([[NSMenu alloc] initWithTitle:@"Edit"]);
    [edit addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
    [edit addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
    [edit addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
    [edit addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
    [[bar addItemWithTitle:@"" action:nullptr keyEquivalent:@""] setSubmenu:edit.get()];
    [NSApp setMainMenu:bar.get()];
}

static WKWebView *createView(WKWebViewConfiguration *configuration, bool shown)
{
    [NSApplication sharedApplication];
    // An application with windows the first time one is shown, and one
    // without a Dock icon until then, unless the application decided.
    if (shown && [NSApp activationPolicy] != NSApplicationActivationPolicyRegular) {
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        if (![NSApp mainMenu])
            addMenus();
    } else if (!shown && [NSApp activationPolicy] == NSApplicationActivationPolicyProhibited)
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

    auto ownConfiguration = configuration ? RetainPtr { configuration } : adoptNS([[WKWebViewConfiguration alloc] init]);
    auto frame = NSMakeRect(0, 0, defaultViewWidth, defaultViewHeight);
    auto view = adoptNS([[WKWebView alloc] initWithFrame:frame configuration:ownConfiguration.get()]);
    if (!view)
        return nil;

    RetainPtr<NSWindow> window;
    if (shown) {
        window = adoptNS([[NSWindow alloc] initWithContentRect:frame
            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
            backing:NSBackingStoreBuffered defer:NO]);
        [window setContentView:view.get()];
        [window center];
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    } else {
        window = adoptNS([[ZebkitHiddenWindow alloc] initWithContentRect:NSOffsetRect(frame, -20000, -20000)
            styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO]);
        [window setExcludedFromWindowsMenu:YES];
        // What is covered or off screen is otherwise not painted, and its
        // timers are slowed down.
        [view _setWindowOcclusionDetectionEnabled:NO];
        [window setContentView:view.get()];
        [window orderBack:nil];
    }
    // The window is this file's to let go of, not AppKit's on closing.
    [window setReleasedWhenClosed:NO];
    [window makeFirstResponder:view.get()];
    windows().set(view.get(), WTF::move(window));
    // The window holds the view from here on.
    return view.autorelease();
}

extern "C" {

__attribute__((visibility("default"))) bool zebkit_display_is_available(void)
{
    // No session dictionary: the process is outside a login session (ssh,
    // a daemon), where no window server answers.
    auto session = adoptCF(CGSessionCopyCurrentDictionary());
    if (!session)
        return false;
    [NSApplication sharedApplication];
    return [[NSScreen screens] count];
}

__attribute__((visibility("default"))) WKWebView *zebkit_view_new(WKWebViewConfiguration *configuration)
{
    return createView(configuration, zebkit_display_is_available());
}

__attribute__((visibility("default"))) WKWebView *zebkit_view_new_headless(WKWebViewConfiguration *configuration)
{
    return createView(configuration, false);
}

__attribute__((visibility("default"))) bool zebkit_view_is_shown(WKWebView *view)
{
    auto window = windows().get(view);
    return window && ![window isKindOfClass:[ZebkitHiddenWindow class]];
}

__attribute__((visibility("default"))) void zebkit_view_destroy(WKWebView *view)
{
    if (auto window = windows().take(view)) {
        [window setDelegate:nil];
        [window orderOut:nil];
        [window setContentView:nil];
    }
}

}
