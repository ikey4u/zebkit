"""A fake org.freedesktop.ScreenSaver on a private session bus, for the lock
test: GetActive, and ActiveChanged whenever it changes. SIGUSR1 locks (the
screen saver active), SIGUSR2 unlocks.

    python3 tests/engine/fake_screensaver.py <ready file> [<asked file>]

The ready file is written once the name is owned; the asked file, when given,
once something has read the state (GetActive). Run under dbus-run-session.
"""

import ctypes
import signal
import sys
import time

import dbus
import dbus.mainloop.glib
import dbus.service

NAME = "org.freedesktop.ScreenSaver"
PATH = "/org/freedesktop/ScreenSaver"


class ScreenSaver(dbus.service.Object):
    def __init__(self, bus, asked):
        super().__init__(bus, PATH)
        self.active = False
        self.asked = asked

    @dbus.service.method(NAME, in_signature="", out_signature="b")
    def GetActive(self):
        if self.asked:
            with open(self.asked, "a") as asked:
                asked.write("GetActive\n")
        return self.active

    @dbus.service.signal(NAME, signature="b")
    def ActiveChanged(self, active):
        pass

    def set(self, active):
        if active != self.active:
            self.active = active
            self.ActiveChanged(active)


def main():
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SessionBus()
    name = dbus.service.BusName(NAME, bus)
    saver = ScreenSaver(bus, sys.argv[2] if len(sys.argv) > 2 else None)
    wanted = []
    signal.signal(signal.SIGUSR1, lambda *args: wanted.append(True))
    signal.signal(signal.SIGUSR2, lambda *args: wanted.append(False))
    with open(sys.argv[1], "w") as ready:
        ready.write("ready\n")
    # GLib's own loop, through ctypes: python's gi is not needed.
    glib = ctypes.CDLL("libglib-2.0.so.0")
    glib.g_main_context_iteration.argtypes = [ctypes.c_void_p, ctypes.c_int]
    while True:
        glib.g_main_context_iteration(None, 0)
        while wanted:
            saver.set(wanted.pop(0))
        time.sleep(0.01)
    del name


if __name__ == "__main__":
    main()
