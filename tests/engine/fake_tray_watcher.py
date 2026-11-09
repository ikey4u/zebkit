"""A fake StatusNotifierWatcher (org.kde.StatusNotifierWatcher) on a private
session bus, for the tray test: it says a host is registered, and writes each
item's service name that registers to the log.

    python3 tests/engine/fake_tray_watcher.py <log> <ready file>
"""

import ctypes
import sys
import time

import dbus
import dbus.mainloop.glib
import dbus.service

NAME = "org.kde.StatusNotifierWatcher"


class Watcher(dbus.service.Object):
    def __init__(self, bus, log):
        super().__init__(bus, "/StatusNotifierWatcher")
        self.log = log

    @dbus.service.method(NAME, in_signature="s", out_signature="", sender_keyword="sender")
    def RegisterStatusNotifierItem(self, service, sender=None):
        with open(self.log, "a") as out:
            out.write((service if service.startswith(":") or "." in service else sender) + "\n")

    @dbus.service.method(dbus.PROPERTIES_IFACE, in_signature="ss", out_signature="v")
    def Get(self, interface, name):
        if name == "IsStatusNotifierHostRegistered":
            return dbus.Boolean(True)
        if name == "ProtocolVersion":
            return dbus.Int32(0)
        raise dbus.exceptions.DBusException("no such property", name="org.freedesktop.DBus.Error.UnknownProperty")


def main():
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SessionBus()
    name = dbus.service.BusName(NAME, bus)
    Watcher(bus, sys.argv[1])
    with open(sys.argv[2], "w") as ready:
        ready.write("ready\n")
    glib = ctypes.CDLL("libglib-2.0.so.0")
    glib.g_main_context_iteration.argtypes = [ctypes.c_void_p, ctypes.c_int]
    while True:
        glib.g_main_context_iteration(None, 0)
        time.sleep(0.01)
    del name


if __name__ == "__main__":
    main()
