// What a Window (window.h) is on a system: a window of the system's, or none.
// Windows of both kinds are in one process (docs/ROADMAP.md).

#ifndef ZEBKIT_ENGINE_WINDOW_PLATFORM_H_
#define ZEBKIT_ENGINE_WINDOW_PLATFORM_H_

#include <string>

#include "ui/gfx/geometry/size.h"
#include "ui/gfx/native_ui_types.h"
#include "zebkit/engine/window.h"
#include "zebkit/engine/window_options.h"

namespace zebkit {

class WindowPlatform {
 public:
  virtual ~WindowPlatform() = default;

  // Called once the window has its page, with the size of the page in it.
  // The window's options (Window::options()) are those it is made with: its
  // frame, whether it starts shown, its place and colours.
  virtual void Create(Window* window, const gfx::Size& initial_size) = 0;
  // Links the page into the window made by Create(), and shows it unless the
  // options say it starts hidden.
  virtual void SetContents(Window* window) = 0;
  virtual void ResizeContents(Window* window, const gfx::Size& content_size) = 0;
  virtual void SetTitle(Window* window, const std::u16string& title) = 0;
  // Null when it has no window of the system's.
  virtual gfx::NativeWindow GetNativeWindow(Window* window) = 0;
  // The window is being destroyed.
  virtual void CleanUp(Window* window) = 0;
  // True: the platform's window closes, and destroys the Window with it.
  virtual bool Destroy(Window* window) = 0;
  // The page's frame was created in its renderer: more than once, if a
  // navigation changes its process.
  virtual void MainFrameCreated(Window* window) {}
  // The page asked to be brought to the front.
  virtual void Activate(Window* window);
  // The application asked: zebkit_view_focus().
  virtual void BringToFront(Window* window) { Activate(window); }
  // The smallest the window may be (empty for no limit), and whether it resizes.
  virtual void SetSizeLimits(Window* window,
                             const gfx::Size& minimum,
                             bool resizable) {}

  // <zebkit/window.h>. A platform without windows of the system's keeps these
  // defaults: no frames but the system's, no actions, a state from the size.
  //
  // What this platform can do for @window (for nullptr: for windows it would make).
  virtual WindowCapabilities Capabilities(Window* window) { return {}; }
  // The window as the system has it now.
  virtual WindowGeometry Geometry(Window* window);
  // Asks the system; the outcome comes as Window::PlatformChanged(). False with
  // @error for what this window cannot do.
  virtual bool PerformAction(Window* window, WindowAction action, std::string* error);
  // Runtime options of Window::options() changed: place, colours, appearance,
  // the title bar area's height. The size limits and the title have their own.
  virtual void OptionsChanged(Window* window) {}
  // Window::drag_regions() changed: what the hit test reads.
  virtual void DragRegionsChanged(Window* window) {}
  // A page's fullscreen, with pageFullscreen "window": the window's too.
  virtual void SetFullscreen(Window* window, bool fullscreen) {}
};

// Once, as the browser starts: what windows need of the system.
void InitializePlatforms();
// What the windows' platforms hold, once they are all closed.
void ShutdownPlatforms();
WindowPlatform* PlatformFor(Window::Kind kind);

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_WINDOW_PLATFORM_H_
