// A real input method in the engine's X11 windows, through IBus
// (ime_ibus_linux.cc). Linux with D-Bus only.

#ifndef ZEBKIT_ENGINE_IME_IBUS_LINUX_H_
#define ZEBKIT_ENGINE_IME_IBUS_LINUX_H_

namespace zebkit {

// Gives Chromium's windows IBus's input contexts where Ozone has none of its
// own (X11). Once, on the engine's thread, after Ozone has started.
void InstallIBusInputMethod();

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_IME_IBUS_LINUX_H_
