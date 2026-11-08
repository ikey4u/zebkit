"""A fake XDG Desktop Portal on a private session bus, for the appearance test:
org.freedesktop.portal.Settings with org.freedesktop.appearance's color-scheme
(1 dark, 2 light), what Chromium's portal registration asks for, and a
SettingChanged signal whenever the scheme changes. SIGUSR1 flips the scheme.

    python3 tests/engine/fake_portal.py <initial scheme: 1 or 2> <ready file>

The ready file is written once the name is owned. Run under dbus-run-session.
"""

import ctypes
import signal
import sys
import time

import dbus
import dbus.mainloop.glib
import dbus.service

NAME = "org.freedesktop.portal.Desktop"
PATH = "/org/freedesktop/portal/desktop"
SETTINGS = "org.freedesktop.portal.Settings"
REGISTRY = "org.freedesktop.host.portal.Registry"


class Portal(dbus.service.Object):
    def __init__(self, bus, scheme):
        super().__init__(bus, PATH)
        self.scheme = scheme

    @dbus.service.method(REGISTRY, in_signature="sa{sv}", out_signature="")
    def Register(self, app_id, options):
        pass

    @dbus.service.method(dbus.PROPERTIES_IFACE, in_signature="ss", out_signature="v")
    def Get(self, interface, name):
        if interface == SETTINGS and name == "version":
            return dbus.UInt32(2)
        raise dbus.exceptions.DBusException("no such property", name="org.freedesktop.DBus.Error.UnknownProperty")

    @dbus.service.method(SETTINGS, in_signature="ss", out_signature="v")
    def Read(self, namespace, key):
        if namespace == "org.freedesktop.appearance" and key == "color-scheme":
            # A variant inside the variant, as the portal's Read answers.
            return dbus.UInt32(self.scheme, variant_level=2)
        raise dbus.exceptions.DBusException("no such setting", name="org.freedesktop.portal.Error.NotFound")

    @dbus.service.signal(SETTINGS, signature="ssv")
    def SettingChanged(self, namespace, key, value):
        pass

    def flip(self):
        self.scheme = 2 if self.scheme == 1 else 1
        self.SettingChanged("org.freedesktop.appearance", "color-scheme", dbus.UInt32(self.scheme, variant_level=1))


def main():
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SessionBus()
    name = dbus.service.BusName(NAME, bus)
    portal = Portal(bus, int(sys.argv[1]))
    flips = []
    signal.signal(signal.SIGUSR1, lambda *args: flips.append(1))
    with open(sys.argv[2], "w") as ready:
        ready.write("ready\n")
    # GLib's own loop, through ctypes: python's gi is not needed.
    glib = ctypes.CDLL("libglib-2.0.so.0")
    glib.g_main_context_iteration.argtypes = [ctypes.c_void_p, ctypes.c_int]
    while True:
        glib.g_main_context_iteration(None, 0)
        if flips:
            flips.clear()
            portal.flip()
        time.sleep(0.01)
    del name


if __name__ == "__main__":
    main()
