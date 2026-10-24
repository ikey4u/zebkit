// What the engine does on macOS alone: where the framework and its helper
// are, the helper's sandbox, the application's menu, and views that are
// not in a window when there is to be none (--headless).

#include "zebkit/engine/engine.h"
#include "zebkit/engine/page_internal.h"

#import <Cocoa/Cocoa.h>
#include <mach-o/dyld.h>

#include <map>
#include <string>
#include <vector>

#include "base/apple/bundle_locations.h"
#include "base/containers/flat_map.h"
#include "base/files/file_path.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/strings/string_util.h"
#include "base/strings/sys_string_conversions.h"
#include "content/public/browser/context_factory.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_paths.h"
#include "content/shell/browser/shell.h"
#include "sandbox/mac/seatbelt_exec.h"
#include "services/device/public/cpp/geolocation/geolocation_system_permission_manager.h"
#include "services/device/public/cpp/geolocation/system_geolocation_source_apple.h"
#include "ui/views/layout/layout_provider.h"
#include "ui/views/views_delegate.h"
#include "zebkit/engine/app.h"
#include "zebkit/engine/view.h"

// Menu items of the application's (zebkit_app_set_menu()).
@interface ZebkitMenuTarget : NSObject
- (void)menuItem:(NSMenuItem*)item;
@end

@implementation ZebkitMenuTarget
- (void)menuItem:(NSMenuItem*)item {
  zebkit::RunMenuItem(base::SysNSStringToUTF8(item.representedObject));
}
@end

// What the system asks of the application (app.h).
@interface ZebkitAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation ZebkitAppDelegate
- (void)application:(NSApplication*)application openURLs:(NSArray<NSURL*>*)urls {
  std::vector<std::string> paths;
  for (NSURL* url in urls) {
    if (url.isFileURL)
      paths.push_back(base::SysNSStringToUTF8(url.path));
  }
  if (!paths.empty())
    zebkit::OpenFiles(paths);
}

- (BOOL)applicationShouldHandleReopen:(NSApplication*)application
                    hasVisibleWindows:(BOOL)visible {
  if (!visible)
    zebkit::Reopen();
  return YES;
}

// The application quits through zebkit_quit(), which closes its views
// first, or not at all, as the application decides.
- (NSApplicationTerminateReply)applicationShouldTerminate:
    (NSApplication*)application {
  zebkit::QuitRequested();
  return NSTerminateCancel;
}
@end

namespace zebkit {

base::FilePath HelperPath(const base::FilePath& library) {
  return library.Append("Helpers")
      .Append("Zebkit Helper.app")
      .Append("Contents")
      .Append("MacOS")
      .Append("Zebkit Helper");
}

void SetUpBundle(const base::FilePath& library) {
  // Chromium finds its resources, ICU's data, V8's snapshot and ANGLE's
  // libraries in the framework bundle, which is zebkit's and not one
  // inside the application.
  base::apple::SetOverrideFrameworkBundlePath(library);
  base::PathService::OverrideAndCreateIfNeeded(
      content::CHILD_PROCESS_EXE, HelperPath(library), /*is_absolute=*/true,
      /*create=*/false);
}

bool EnterHelperSandbox(int argc, char** argv) {
  // The browser asks for the sandbox on the helper's command line; the
  // helper enters it before anything else runs, as Chrome's helpers do.
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string path(size, '\0');
  if (_NSGetExecutablePath(path.data(), &size) != 0)
    return false;
  sandbox::SeatbeltExecServer::CreateFromArgumentsResult seatbelt =
      sandbox::SeatbeltExecServer::CreateFromArguments(path.c_str(), argc,
                                                       argv);
  if (!seatbelt.sandbox_required)
    return true;
  return seatbelt.server && seatbelt.server->InitializeSandbox();
}

namespace {

NSMenuItem* NewMenuItem(NSString* title, SEL action, NSString* key) {
  return [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key];
}

// The system's Services menu, taken out of the item it is in: a menu bar
// set again still has it, and AppKit throws for a menu put in two items.
NSMenu* ServicesMenu() {
  NSMenu* services = NSApp.servicesMenu;
  if (!services) {
    services = [[NSMenu alloc] initWithTitle:@"Services"];
    NSApp.servicesMenu = services;
  }
  if (NSMenu* holder = services.supermenu) {
    NSInteger index = [holder indexOfItemWithSubmenu:services];
    if (index >= 0)
      [holder itemAtIndex:index].submenu = nil;
  }
  return services;
}

NSMenu* Submenu(NSMenu* main, NSString* title) {
  NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title
                                                action:nullptr
                                         keyEquivalent:@""];
  NSMenu* menu = [[NSMenu alloc] initWithTitle:title];
  item.submenu = menu;
  [main addItem:item];
  return menu;
}

// The selector of a standard command (zebkit_app_set_menu()'s roles).
SEL RoleAction(const std::string& role) {
  static const struct {
    const char* role;
    SEL action;
  } kRoles[] = {
      {"undo", @selector(undo:)},
      {"redo", @selector(redo:)},
      {"cut", @selector(cut:)},
      {"copy", @selector(copy:)},
      {"paste", @selector(paste:)},
      {"pasteAndMatchStyle", @selector(pasteAsPlainText:)},
      {"selectAll", @selector(selectAll:)},
      {"minimize", @selector(performMiniaturize:)},
      {"zoom", @selector(performZoom:)},
      {"close", @selector(performClose:)},
      {"fullscreen", @selector(toggleFullScreen:)},
      {"front", @selector(arrangeInFront:)},
      {"quit", @selector(terminate:)},
      {"hide", @selector(hide:)},
      {"hideOthers", @selector(hideOtherApplications:)},
      {"showAll", @selector(unhideAllApplications:)},
      {"about", @selector(orderFrontStandardAboutPanel:)},
  };
  for (const auto& entry : kRoles) {
    if (role == entry.role)
      return entry.action;
  }
  return nullptr;
}

// A menu item's key for Cocoa: a character, or a function key's.
NSString* KeyEquivalent(const std::string& key) {
  if (key.size() >= 2 && (key[0] == 'f' || key[0] == 'F')) {
    const int n = atoi(key.c_str() + 1);
    if (n >= 1 && n <= 24) {
      const unichar c = static_cast<unichar>(NSF1FunctionKey + n - 1);
      return [NSString stringWithCharacters:&c length:1];
    }
  }
  return base::SysUTF8ToNSString(base::ToLowerASCII(key));
}

void AddItems(NSMenu* menu, const std::vector<MenuItem>& items) {
  static ZebkitMenuTarget* target = [[ZebkitMenuTarget alloc] init];
  for (const MenuItem& item : items) {
    if (item.separator) {
      [menu addItem:[NSMenuItem separatorItem]];
      continue;
    }
    if (item.role == "services") {
      NSMenuItem* services = NewMenuItem(
          base::SysUTF8ToNSString(item.title.empty() ? "Services" : item.title),
          nullptr, @"");
      services.submenu = ServicesMenu();
      [menu addItem:services];
      continue;
    }
    std::string title = item.title;
    if (title.empty() && !item.role.empty()) {
      title = item.role;
      title[0] = base::ToUpperASCII(title[0]);
    }
    NSMenuItem* entry = NewMenuItem(base::SysUTF8ToNSString(title), nullptr,
                                 KeyEquivalent(item.key));
    if (!item.items.empty()) {
      entry.submenu =
          [[NSMenu alloc] initWithTitle:base::SysUTF8ToNSString(title)];
      AddItems(entry.submenu, item.items);
    } else if (!item.role.empty()) {
      entry.action = RoleAction(item.role);
    } else {
      entry.target = target;
      entry.action = @selector(menuItem:);
      entry.representedObject = base::SysUTF8ToNSString(item.id);
    }
    if (!item.key.empty()) {
      NSEventModifierFlags mask = NSEventModifierFlagCommand;
      if (item.modifiers & kMenuShift)
        mask |= NSEventModifierFlagShift;
      if (item.modifiers & kMenuAlt)
        mask |= NSEventModifierFlagOption;
      if (item.modifiers & kMenuCtrl)
        mask |= NSEventModifierFlagControl;
      entry.keyEquivalentModifierMask = mask;
    }
    [menu addItem:entry];
  }
}

// The views delegate, which on macOS nothing else makes: zebkit.ui's
// windows are views widgets, which draw with content's compositor.
class ViewsDelegate : public views::ViewsDelegate {
 public:
  ui::ContextFactory* GetContextFactory() override {
    return content::GetContextFactory();
  }
};

// Windows without a window: each view's page is laid out and drawn at the
// size it is given, as Chromium's headless mode does on macOS.
class HeadlessPlatformDelegate : public content::ShellPlatformDelegate {
 public:
  void CreatePlatformWindow(content::Shell* shell,
                            const gfx::Size& initial_size) override {
    sizes_[shell] = initial_size;
  }
  gfx::NativeWindow GetNativeWindow(content::Shell* shell) override {
    return gfx::NativeWindow();
  }
  void CleanUp(content::Shell* shell) override { sizes_.erase(shell); }
  void DidCloseLastWindow() override {}
  void SetContents(content::Shell* shell) override {
    NSView* view = shell->web_contents()->GetNativeView().GetNativeNSView();
    view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    Resize(shell);
    shell->web_contents()->WasShown();
  }
  void ResizeWebContent(content::Shell* shell,
                        const gfx::Size& content_size) override {
    sizes_[shell] = content_size;
    Resize(shell);
  }
  void MainFrameCreated(content::Shell* shell,
                        content::RenderFrameHost* main_frame) override {
    Resize(shell);
  }
  void EnableUIControl(content::Shell* shell,
                       UIControl control,
                       bool is_enabled) override {}
  void SetAddressBarURL(content::Shell* shell, const GURL& url) override {}
  void SetIsLoading(content::Shell* shell, bool loading) override {}
  std::unique_ptr<content::JavaScriptDialogManager>
  CreateJavaScriptDialogManager(content::Shell* shell) override {
    return CreateDialogManager(shell);
  }
  void SetTitle(content::Shell* shell, const std::u16string& title) override {}
  bool DestroyShell(content::Shell* shell) override { return false; }
  void ActivateContents(content::Shell* shell,
                        content::WebContents* contents) override {
    contents->Focus();
  }
  void DidNavigatePrimaryMainFramePostCommit(
      content::Shell* shell,
      content::WebContents* contents) override {}
  bool HandleKeyboardEvent(
      content::Shell* shell,
      content::WebContents* source,
      const input::NativeWebKeyboardEvent& event) override {
    return false;
  }

 private:
  void Resize(content::Shell* shell) {
    content::WebContents* contents = shell->web_contents();
    if (!contents)
      return;
    const gfx::Size size = sizes_[shell];
    NSView* view = contents->GetNativeView().GetNativeNSView();
    view.frame = NSMakeRect(0, 0, size.width(), size.height());
    if (content::RenderWidgetHostView* host =
            contents->GetRenderWidgetHostView()) {
      host->SetWindowFrameInScreen(gfx::Rect(size));
    }
  }

  base::flat_map<content::Shell*, gfx::Size> sizes_;
};

}  // namespace

void SetUpApplication(bool headless) {
  NSApplication* app = NSApplication.sharedApplication;
  // A program that is not in an application bundle may not come to the
  // front unless it says it is a regular application. Without windows it
  // has no Dock icon and no menu bar, whatever its bundle says, as Chromium's
  // headless shell does (headless_content_main_delegate_mac.mm).
  [app setActivationPolicy:headless ? NSApplicationActivationPolicyAccessory
                                    : NSApplicationActivationPolicyRegular];
  // Files from the Finder, the Dock's reopen and quit (app.h). Content
  // sets no delegate of its own.
  static ZebkitAppDelegate* delegate = [[ZebkitAppDelegate alloc] init];
  app.delegate = delegate;
  SetUpMenuBar();
  // Location permission is the system's, as in Chrome (patches/0014).
  if (!device::GeolocationSystemPermissionManager::GetInstance()) {
    device::GeolocationSystemPermissionManager::SetInstance(
        device::SystemGeolocationSourceApple::
            CreateGeolocationSystemPermissionManager());
  }
}

void SetUpMenuBar() {
  NSApplication* app = NSApplication.sharedApplication;
  NSString* name = NSProcessInfo.processInfo.processName;
  NSMenu* main = [[NSMenu alloc] initWithTitle:@""];
  NSMenu* application = Submenu(main, name);
  const std::vector<MenuItem>& menu = AppMenu();
  // The application's own application menu ("appMenu"), or zebkit's.
  const bool own = !menu.empty() && menu[0].role == "appMenu";
  if (own) {
    AddItems(application, menu[0].items);
  } else {
    [application addItem:NewMenuItem([@"About " stringByAppendingString:name],
                                     @selector(orderFrontStandardAboutPanel:),
                                     @"")];
    [application addItem:[NSMenuItem separatorItem]];
    NSMenuItem* services = NewMenuItem(@"Services", nullptr, @"");
    services.submenu = ServicesMenu();
    [application addItem:services];
    [application addItem:[NSMenuItem separatorItem]];
    [application addItem:NewMenuItem([@"Hide " stringByAppendingString:name],
                                     @selector(hide:), @"h")];
    NSMenuItem* others =
        NewMenuItem(@"Hide Others", @selector(hideOtherApplications:), @"h");
    others.keyEquivalentModifierMask =
        NSEventModifierFlagOption | NSEventModifierFlagCommand;
    [application addItem:others];
    [application addItem:NewMenuItem(@"Show All",
                                     @selector(unhideAllApplications:), @"")];
    [application addItem:[NSMenuItem separatorItem]];
    [application addItem:NewMenuItem([@"Quit " stringByAppendingString:name],
                                     @selector(terminate:), @"q")];
  }

  if (menu.size() == (own ? 1u : 0u)) {
    // Text fields in pages and in zebkit.ui take these from the menu.
    NSMenu* edit = Submenu(main, @"Edit");
    [edit addItem:NewMenuItem(@"Undo", @selector(undo:), @"z")];
    [edit addItem:NewMenuItem(@"Redo", @selector(redo:), @"Z")];
    [edit addItem:[NSMenuItem separatorItem]];
    [edit addItem:NewMenuItem(@"Cut", @selector(cut:), @"x")];
    [edit addItem:NewMenuItem(@"Copy", @selector(copy:), @"c")];
    [edit addItem:NewMenuItem(@"Paste", @selector(paste:), @"v")];
    [edit addItem:NewMenuItem(@"Select All", @selector(selectAll:), @"a")];
    NSMenu* window = Submenu(main, @"Window");
    [window addItem:NewMenuItem(@"Minimize", @selector(performMiniaturize:), @"m")];
    [window addItem:NewMenuItem(@"Zoom", @selector(performZoom:), @"")];
    [window addItem:NewMenuItem(@"Close", @selector(performClose:), @"w")];
    app.windowsMenu = window;
  } else {
    for (size_t i = own ? 1 : 0; i < menu.size(); ++i) {
      const MenuItem& item = menu[i];
      NSMenu* submenu = Submenu(main, base::SysUTF8ToNSString(item.title));
      AddItems(submenu, item.items);
      if (item.title == "Window")
        app.windowsMenu = submenu;
    }
  }
  app.mainMenu = main;
}

void FocusNativeWindow(gfx::NativeWindow window) {
  NSWindow* ns_window = window.GetNativeNSWindow();
  if (!ns_window)
    return;
  [ns_window makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];
}

void SetNativeWindowSizeLimits(gfx::NativeWindow window,
                               const gfx::Size& minimum,
                               bool resizable) {
  NSWindow* ns_window = window.GetNativeNSWindow();
  if (!ns_window)
    return;
  ns_window.contentMinSize = NSMakeSize(minimum.width(), minimum.height());
  if (resizable)
    ns_window.styleMask |= NSWindowStyleMaskResizable;
  else
    ns_window.styleMask &= ~NSWindowStyleMaskResizable;
}

namespace {

struct Attached {
  base::WeakPtr<zebkit_view> parent;
  NSView* __strong view = nil;
};

std::map<const zebkit_view*, Attached>& AttachedViews() {
  static base::NoDestructor<std::map<const zebkit_view*, Attached>> attached;
  return *attached;
}

// @bounds, from the top left of @container, in Cocoa's coordinates, which
// start at the bottom left.
NSRect Frame(NSView* container, const gfx::Rect& bounds) {
  const CGFloat height = container.bounds.size.height;
  return NSMakeRect(bounds.x(),
                    container.isFlipped ? bounds.y()
                                        : height - bounds.y() - bounds.height(),
                    bounds.width(), bounds.height());
}

}  // namespace

bool AttachView(zebkit_view* view, zebkit_view* parent, const gfx::Rect& bounds) {
  if (!view || !parent || view == parent || !view->shell() || !parent->shell())
    return false;
  NSWindow* parent_window = parent->shell()->window().GetNativeNSWindow();
  if (!parent_window) {
    // Without windows (--headless) there is nothing to be inside; the view
    // takes the size it is given, as it does inside a window.
    if (!zebkit_is_headless())
      return false;
    DetachView(view);
    AttachedViews()[view] = {parent->GetWeakPtr(), nil};
    view->shell()->ResizeWebContentForTests(bounds.size());
    return true;
  }
  DetachView(view);
  NSView* page = view->web_contents()->GetNativeView().GetNativeNSView();
  NSWindow* own_window = page.window;
  NSView* container = parent_window.contentView;
  page.autoresizingMask = NSViewNotSizable;
  [container addSubview:page positioned:NSWindowAbove relativeTo:nil];
  page.frame = Frame(container, bounds);
  if (own_window && own_window != parent_window)
    [own_window orderOut:nil];
  AttachedViews()[view] = {parent->GetWeakPtr(), page};
  return true;
}

void SetAttachedBounds(zebkit_view* view, const gfx::Rect& bounds) {
  auto it = AttachedViews().find(view);
  if (it == AttachedViews().end() || !it->second.parent)
    return;
  if (!it->second.view) {
    view->shell()->ResizeWebContentForTests(bounds.size());
    return;
  }
  if (it->second.view.superview)
    it->second.view.frame = Frame(it->second.view.superview, bounds);
}

void SetAttachedVisible(zebkit_view* view, bool visible) {
  auto it = AttachedViews().find(view);
  if (it != AttachedViews().end() && it->second.parent)
    it->second.view.hidden = !visible;
}

void DetachView(zebkit_view* view) {
  auto it = AttachedViews().find(view);
  if (it == AttachedViews().end())
    return;
  [it->second.view removeFromSuperview];
  AttachedViews().erase(it);
}

void SetUpViews() {
  if (!views::ViewsDelegate::GetInstance())
    static base::NoDestructor<ViewsDelegate> delegate;
  if (!views::LayoutProvider::Get())
    static base::NoDestructor<views::LayoutProvider> layout;
}

std::unique_ptr<content::ShellPlatformDelegate>
CreateHeadlessPlatformDelegate() {
  return std::make_unique<HeadlessPlatformDelegate>();
}

}  // namespace zebkit
