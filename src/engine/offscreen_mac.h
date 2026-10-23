// macOS's part of off-screen views (offscreen.cc, offscreen_mac.mm).

#ifndef ZEBKIT_ENGINE_OFFSCREEN_MAC_H_
#define ZEBKIT_ENGINE_OFFSCREEN_MAC_H_

#include "ui/gfx/native_ui_types.h"

namespace zebkit {

// Keeps a popup widget's own window off the screen's sight, still drawing.
void HideOffscreenPopupWindow(gfx::NativeView view);

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_OFFSCREEN_MAC_H_
