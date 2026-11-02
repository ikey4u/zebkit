// What a Window (window.h) is on a system: a window of the system's, or none.
// Windows of both kinds are in one process (docs/ROADMAP.md).

#ifndef ZEBKIT_ENGINE_WINDOW_PLATFORM_H_
#define ZEBKIT_ENGINE_WINDOW_PLATFORM_H_

#include <string>

#include "ui/gfx/geometry/size.h"
#include "ui/gfx/native_ui_types.h"
#include "zebkit/engine/window.h"

namespace zebkit {

class WindowPlatform {
 public:
  virtual ~WindowPlatform() = default;

  // Called once the window has its page, with the size of the page in it.
  virtual void Create(Window* window, const gfx::Size& initial_size) = 0;
  // Links the page into the window made by Create(), and shows it.
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
};

// Once, as the browser starts: what windows need of the system.
void InitializePlatforms();
// What the windows' platforms hold, once they are all closed.
void ShutdownPlatforms();
WindowPlatform* PlatformFor(Window::Kind kind);

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_WINDOW_PLATFORM_H_
