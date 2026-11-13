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
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/strings/string_util.h"
#include "base/strings/sys_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "content/public/browser/context_factory.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_paths.h"
#include "zebkit/engine/application_mac.h"
#include "zebkit/engine/window.h"
#include "sandbox/mac/seatbelt_exec.h"
#include "services/device/public/cpp/geolocation/geolocation_system_permission_manager.h"
#include "services/device/public/cpp/geolocation/system_geolocation_source_apple.h"
#include "ui/views/layout/layout_provider.h"
#include "ui/views/views_delegate.h"
#include "zebkit/engine/app.h"
#include "zebkit/engine/dock.h"
#include "zebkit/engine/attached_views.h"
#include "zebkit/engine/view.h"

// Menu items of the application's (zebkit_app_set_menu()).
@interface ZebkitMenuTarget : NSObject
- (void)menuItem:(NSMenuItem*)item;
@end

@implementation ZebkitMenuTarget
- (void)menuItem:(NSMenuItem*)item {
  zebkit::RunMenuItem(base::SysNSStringToUTF8(item.representedObject));
}
// The application's own items as zebkit_app_update_menu_item() left them.
- (BOOL)validateMenuItem:(NSMenuItem*)item {
  return zebkit::MenuItemEnabled(base::SysNSStringToUTF8(item.representedObject));
}
@end

// What the system asks of the application (app.h).
@interface ZebkitAppDelegate : NSObject <NSApplicationDelegate>
@end

// The Dock's menu's items (dock.h).
@interface ZebkitDockMenuTarget : NSObject
- (void)dockItem:(NSMenuItem*)item;
@end

@implementation ZebkitDockMenuTarget
- (void)dockItem:(NSMenuItem*)item {
  zebkit::RunDockMenuItem(base::SysNSStringToUTF8(item.representedObject));
}
@end

static void AddDockItems(NSMenu* menu, const std::vector<zebkit::MenuItem>& items, ZebkitDockMenuTarget* target) {
  for (const zebkit::MenuItem& item : items) {
    if (item.separator) {
      [menu addItem:[NSMenuItem separatorItem]];
      continue;
    }
    NSMenuItem* entry = [[NSMenuItem alloc] initWithTitle:base::SysUTF8ToNSString(item.title)
                                                   action:@selector(dockItem:)
                                            keyEquivalent:@""];
    entry.target = target;
    entry.enabled = item.enabled;
    entry.state = item.checked ? NSControlStateValueOn : NSControlStateValueOff;
    entry.representedObject = base::SysUTF8ToNSString(item.id);
    if (!item.items.empty()) {
      entry.submenu = [[NSMenu alloc] initWithTitle:entry.title];
      entry.submenu.autoenablesItems = NO;
      AddDockItems(entry.submenu, item.items, target);
    }
    [menu addItem:entry];
  }
}

@implementation ZebkitAppDelegate
- (NSMenu*)applicationDockMenu:(NSApplication*)sender {
  if (zebkit::DockMenu().empty())
    return nil;
  static ZebkitDockMenuTarget* target = [[ZebkitDockMenuTarget alloc] init];
  NSMenu* menu = [[NSMenu alloc] initWithTitle:@""];
  menu.autoenablesItems = NO;
  AddDockItems(menu, zebkit::DockMenu(), target);
  return menu;
}
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
    entry.state = item.checked ? NSControlStateValueOn : NSControlStateValueOff;
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

namespace {

struct Attached {
  base::WeakPtr<zebkit_view> parent;
  NSView* __strong view = nil;
  // In an NSView of the application's, not another view's window.
  bool native = false;
};

std::map<const zebkit_view*, Attached>& AttachedViews() {
  return AttachedViewsOf<Attached>();
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
  if (!view || !parent || view == parent || !view->window() || !parent->window())
    return false;
  NSWindow* parent_window = parent->window()->native_window().GetNativeNSWindow();
  if (!parent_window) {
    // Without windows (--headless) there is nothing to be inside; the view
    // takes the size it is given, as it does inside a window.
    if (!zebkit_is_headless())
      return false;
    DetachView(view, /*restore=*/false);
    AttachedViews()[view] = {parent->GetWeakPtr(), nil, false};
    view->window()->ResizeContents(bounds.size());
    return true;
  }
  DetachView(view, /*restore=*/false);
  NSView* page = view->web_contents()->GetNativeView().GetNativeNSView();
  NSWindow* own_window = page.window;
  NSView* container = parent_window.contentView;
  page.autoresizingMask = NSViewNotSizable;
  [container addSubview:page positioned:NSWindowAbove relativeTo:nil];
  page.frame = Frame(container, bounds);
  if (own_window && own_window != parent_window)
    [own_window orderOut:nil];
  AttachedViews()[view] = {parent->GetWeakPtr(), page, false};
  return true;
}

void SetAttachedBounds(zebkit_view* view, const gfx::Rect& bounds) {
  auto it = AttachedViews().find(view);
  if (it == AttachedViews().end() || !it->second.parent)
    return;
  if (!it->second.view) {
    view->window()->ResizeContents(bounds.size());
    return;
  }
  if (it->second.view.superview)
    it->second.view.frame = Frame(it->second.view.superview, bounds);
}

void ExportNativeWindowForView(zebkit_view* view,
                               base::OnceCallback<void(std::string)> done) {
  // macOS has no portals: zebkit_view_get_native_window() is its handle.
  std::move(done).Run(std::string());
}

zebkit_native_window NativeWindowForView(zebkit_view* view) {
  NSView* page = view->web_contents()->GetNativeView().GetNativeNSView();
  NSWindow* window = page.window;
  return window ? zebkit_native_window{ZEBKIT_NATIVE_WINDOW_APPKIT,
                                      reinterpret_cast<uintptr_t>((__bridge void*)window)}
                : zebkit_native_window{};
}

WindowOwner ViewWindowOwner(const zebkit_view* view) {
  auto it = AttachedViews().find(view);
  if (it == AttachedViews().end())
    return WindowOwner::kEngine;
  return it->second.native ? WindowOwner::kApplication : WindowOwner::kView;
}

bool AttachViewNative(zebkit_view* view,
                      uintptr_t native_parent,
                      const gfx::Rect& bounds,
                      std::string* error) {
  if (zebkit_is_headless()) {
    *error =
        "the engine runs headless and the view has no window to parent; draw "
        "the page in your view with off-screen views (<zebkit/offscreen.h>)";
    return false;
  }
  if (bounds.width() <= 0 || bounds.height() <= 0) {
    *error = "the bounds are empty";
    return false;
  }
  NSView* container = (__bridge NSView*)reinterpret_cast<void*>(native_parent);
  NSView* page = view->web_contents()->GetNativeView().GetNativeNSView();
  if ([container isDescendantOf:page]) {
    *error = "the view's own page cannot be its parent";
    return false;
  }
  NSWindow* own_window = view->window()->native_window().GetNativeNSWindow();
  DetachView(view, /*restore=*/false);
  page.autoresizingMask = NSViewNotSizable;
  [container addSubview:page positioned:NSWindowAbove relativeTo:nil];
  page.frame = Frame(container, bounds);
  if (own_window && own_window != container.window)
    [own_window orderOut:nil];
  Attached attached;
  attached.view = page;
  attached.native = true;
  AttachedViews()[view] = attached;
  return true;
}

bool SetAttachedNativeBounds(zebkit_view* view, const gfx::Rect& bounds) {
  auto it = AttachedViews().find(view);
  if (it == AttachedViews().end() || !it->second.native)
    return false;
  if (it->second.view.superview)
    it->second.view.frame = Frame(it->second.view.superview, bounds);
  return true;
}

bool SetAttachedNativeSize(zebkit_view* view, const gfx::Size& size) {
  auto it = AttachedViews().find(view);
  if (it == AttachedViews().end() || !it->second.native)
    return false;
  NSView* page = it->second.view;
  if (page.superview) {
    const NSRect frame = page.frame;
    page.frame = NSMakeRect(
        frame.origin.x,
        page.superview.isFlipped
            ? frame.origin.y
            : NSMaxY(frame) - size.height(),
        size.width(), size.height());
  }
  return true;
}

bool SetAttachedNativeVisible(zebkit_view* view, bool visible) {
  auto it = AttachedViews().find(view);
  if (it == AttachedViews().end() || !it->second.native)
    return false;
  it->second.view.hidden = !visible;
  return true;
}

bool FocusAttachedNative(zebkit_view* view) {
  auto it = AttachedViews().find(view);
  if (it == AttachedViews().end() || !it->second.native)
    return false;
  [it->second.view.window makeFirstResponder:it->second.view];
  return true;
}

bool DetachViewNative(zebkit_view* view) {
  auto it = AttachedViews().find(view);
  if (it == AttachedViews().end() || !it->second.native)
    return false;
  NSView* page = it->second.view;
  [page removeFromSuperview];
  AttachedViews().erase(it);
  page.hidden = NO;
  NSWindow* own_window = view->window()->native_window().GetNativeNSWindow();
  if (own_window) {
    [own_window.contentView addSubview:page];
    page.frame = own_window.contentView.bounds;
    page.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [own_window makeKeyAndOrderFront:nil];
  }
  return true;
}

void ForgetAttachedNative(zebkit_view* view) {
  // DetachView(), which the closing view calls too, forgets it here.
}

void SetAttachedVisible(zebkit_view* view, bool visible) {
  auto it = AttachedViews().find(view);
  if (it != AttachedViews().end() && it->second.parent)
    it->second.view.hidden = !visible;
}

// @restore is not needed here: what moves is the page's NSView itself, and
// every caller puts it where it goes next at once (AttachView(),
// AttachViewNative()) or is the closing view. On Linux and Windows it is the
// page's own window that an application's window takes, so the page has to
// be back in it first.
void DetachView(zebkit_view* view, bool /*restore*/) {
  auto it = AttachedViews().find(view);
  if (it == AttachedViews().end())
    return;
  NSView* page = it->second.view;
  [page removeFromSuperview];
  AttachedViews().erase(it);
}

void CloseViewsAttachedTo(zebkit_view* parent) {
  // A view in an NSView of the application's stays (zebkit.h).
  CloseAttachedViewsOf<Attached>(parent, [](const Attached& attached) { return !attached.native; });
}

void RegisterApplication() {
  // Before anything has made another NSApplication.
  [ZebkitApplication sharedApplication];
  CHECK([NSApp isKindOfClass:[ZebkitApplication class]]);
}

base::FilePath ResourcePackPath() {
  NSString* path = [base::apple::FrameworkBundle() pathForResource:@"zebkit"
                                                            ofType:@"pak"];
  return base::FilePath(path.fileSystemRepresentation);
}

void SetUpViews() {
  if (!views::ViewsDelegate::GetInstance())
    static base::NoDestructor<ViewsDelegate> delegate;
  if (!views::LayoutProvider::Get())
    static base::NoDestructor<views::LayoutProvider> layout;
}

}  // namespace zebkit
