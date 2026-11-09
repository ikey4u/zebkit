"""A fake desktop notification service (org.freedesktop.Notifications) on a
private session bus, for the notifications test: each Notify is written as a
line of JSON to the log, and SIGUSR1 clicks the last one (ActionInvoked
"default").

    python3 tests/engine/fake_notifications.py <log> <ready file>
"""

import ctypes
import json
import signal
import sys
import time

import dbus
import dbus.mainloop.glib
import dbus.service

NAME = "org.freedesktop.Notifications"


class Service(dbus.service.Object):
    def __init__(self, bus, log):
        super().__init__(bus, "/org/freedesktop/Notifications")
        self.log = log
        self.last = 0

    @dbus.service.method(NAME, in_signature="susssasa{sv}i", out_signature="u")
    def Notify(self, app, replaces, icon, summary, body, actions, hints, timeout):
        self.last += 1
        with open(self.log, "a") as out:
            out.write(json.dumps({"id": self.last, "app": str(app), "summary": str(summary), "body": str(body),
                                  "actions": [str(a) for a in actions]}) + "\n")
        return dbus.UInt32(self.last)

    @dbus.service.method(NAME, in_signature="u", out_signature="")
    def CloseNotification(self, notification):
        with open(self.log, "a") as out:
            out.write(json.dumps({"closed": int(notification)}) + "\n")

    @dbus.service.method(NAME, in_signature="", out_signature="as")
    def GetCapabilities(self):
        return ["actions", "body"]

    @dbus.service.signal(NAME, signature="us")
    def ActionInvoked(self, notification, key):
        pass


def main():
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SessionBus()
    name = dbus.service.BusName(NAME, bus)
    service = Service(bus, sys.argv[1])
    clicks = []
    signal.signal(signal.SIGUSR1, lambda *args: clicks.append(1))
    with open(sys.argv[2], "w") as ready:
        ready.write("ready\n")
    glib = ctypes.CDLL("libglib-2.0.so.0")
    glib.g_main_context_iteration.argtypes = [ctypes.c_void_p, ctypes.c_int]
    while True:
        glib.g_main_context_iteration(None, 0)
        if clicks:
            clicks.clear()
            service.ActionInvoked(dbus.UInt32(service.last), "default")
        time.sleep(0.01)
    del name


if __name__ == "__main__":
    main()
