// What the engine does on macOS alone: where the framework and its helper
// are, the helper's sandbox, the application's menu, and views that are
// not in a window when there is to be none (--headless).

#include "zebkit/engine/engine.h"

#import <Cocoa/Cocoa.h>
#include <mach-o/dyld.h>

#include <string>

#include "base/apple/bundle_locations.h"
#include "base/containers/flat_map.h"
#include "base/files/file_path.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_paths.h"
#include "content/shell/browser/shell.h"
#include "sandbox/mac/seatbelt_exec.h"
#include "ui/views/layout/layout_provider.h"
#include "ui/views/views_delegate.h"

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

NSMenuItem* MenuItem(NSString* title, SEL action, NSString* key) {
  return [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key];
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

// The views delegate, which on macOS nothing else makes: zebkit.ui's
// windows are views widgets.
class ViewsDelegate : public views::ViewsDelegate {};

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
  // front unless it says it is a regular application; without windows it
  // stays out of the Dock.
  if (!headless)
    [app setActivationPolicy:NSApplicationActivationPolicyRegular];

  NSString* name = NSProcessInfo.processInfo.processName;
  NSMenu* main = [[NSMenu alloc] initWithTitle:@""];
  NSMenu* application = Submenu(main, name);
  [application addItem:MenuItem([@"Hide " stringByAppendingString:name],
                                @selector(hide:), @"h")];
  NSMenuItem* others =
      MenuItem(@"Hide Others", @selector(hideOtherApplications:), @"h");
  others.keyEquivalentModifierMask =
      NSEventModifierFlagOption | NSEventModifierFlagCommand;
  [application addItem:others];
  [application addItem:MenuItem(@"Show All",
                                @selector(unhideAllApplications:), @"")];
  [application addItem:[NSMenuItem separatorItem]];
  [application addItem:MenuItem([@"Quit " stringByAppendingString:name],
                                @selector(terminate:), @"q")];
  // Text fields in pages and in zebkit.ui take these from the menu.
  NSMenu* edit = Submenu(main, @"Edit");
  [edit addItem:MenuItem(@"Undo", @selector(undo:), @"z")];
  [edit addItem:MenuItem(@"Redo", @selector(redo:), @"Z")];
  [edit addItem:[NSMenuItem separatorItem]];
  [edit addItem:MenuItem(@"Cut", @selector(cut:), @"x")];
  [edit addItem:MenuItem(@"Copy", @selector(copy:), @"c")];
  [edit addItem:MenuItem(@"Paste", @selector(paste:), @"v")];
  [edit addItem:MenuItem(@"Select All", @selector(selectAll:), @"a")];
  NSMenu* window = Submenu(main, @"Window");
  [window addItem:MenuItem(@"Minimize", @selector(performMiniaturize:), @"m")];
  [window addItem:MenuItem(@"Zoom", @selector(performZoom:), @"")];
  [window addItem:MenuItem(@"Close", @selector(performClose:), @"w")];
  app.windowsMenu = window;
  app.mainMenu = main;
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
