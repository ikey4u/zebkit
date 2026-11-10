"""A fake IBus on a bus of its own, for the input method test: what
ibus-daemon serves to a client (org.freedesktop.IBus's CreateInputContext, and
an input context's ProcessKeyEvent, FocusIn, FocusOut, Reset, SetCapabilities,
SetCursorLocation), with an engine that composes Latin letters and commits
them on space as Chinese: "nihao" then space gives "你好", any other word
its letters in full-width. Enter commits the letters as they are, Escape drops
them; keys outside a composition are not taken.

    python3 tests/engine/fake_ibus.py <ready file> <log file>

Run under the bus it is to own (DBUS_SESSION_BUS_ADDRESS); the test gives the
engine that address as IBUS_ADDRESS.
"""

import ctypes
import sys
import time

import dbus
import dbus.mainloop.glib
import dbus.service

IBUS = "org.freedesktop.IBus"
CONTEXT = "org.freedesktop.IBus.InputContext"
RELEASE = 1 << 30
WORDS = {"nihao": "你好", "zhongwen": "中文"}


def text(value):
    """An IBusText: a variant of (s a{sv} s v), its attributes an empty list."""
    attributes = dbus.Struct((dbus.String("IBusAttrList"), dbus.Dictionary({}, signature="sv"),
                              dbus.Array([], signature="v")), signature="sa{sv}av", variant_level=1)
    return dbus.Struct((dbus.String("IBusText"), dbus.Dictionary({}, signature="sv"), dbus.String(value), attributes),
                       signature="sa{sv}sv", variant_level=1)


class Context(dbus.service.Object):
    def __init__(self, bus, path, log):
        super().__init__(bus, path)
        self.letters = ""
        self.log = log

    def note(self, line):
        with open(self.log, "a") as out:
            out.write(line + "\n")

    @dbus.service.method(CONTEXT, in_signature="uuu", out_signature="b")
    def ProcessKeyEvent(self, keyval, keycode, state):
        if state & RELEASE:
            return False
        if 0x61 <= keyval <= 0x7a and not state & 0xc:  # a letter, without Control or Alt
            self.letters += chr(keyval)
            self.UpdatePreeditText(text(self.letters), dbus.UInt32(len(self.letters)), True)
            return True
        if not self.letters:
            return False
        if keyval == 0x20:  # space: the word
            word = WORDS.get(self.letters) or "".join(chr(ord(c) + 0xfee0) for c in self.letters)
            self.finish(word)
        elif keyval == 0xff0d:  # Return: the letters
            self.finish(self.letters)
        elif keyval == 0xff1b:  # Escape
            self.finish(None)
        elif keyval == 0xff08:  # BackSpace
            self.letters = self.letters[:-1]
            self.UpdatePreeditText(text(self.letters), dbus.UInt32(len(self.letters)), bool(self.letters))
        else:
            self.note("unknown key 0x%x" % keyval)
        return True

    def finish(self, committed):
        self.letters = ""
        self.UpdatePreeditText(text(""), dbus.UInt32(0), False)
        if committed:
            self.CommitText(text(committed))

    @dbus.service.method(CONTEXT, in_signature="", out_signature="")
    def FocusIn(self):
        self.note("FocusIn")

    @dbus.service.method(CONTEXT, in_signature="", out_signature="")
    def FocusOut(self):
        self.note("FocusOut")
        self.letters = ""

    @dbus.service.method(CONTEXT, in_signature="", out_signature="")
    def Reset(self):
        self.letters = ""

    @dbus.service.method(CONTEXT, in_signature="u", out_signature="")
    def SetCapabilities(self, capabilities):
        self.note("SetCapabilities %d" % capabilities)

    @dbus.service.method(CONTEXT, in_signature="iiii", out_signature="")
    def SetCursorLocation(self, x, y, width, height):
        self.note("SetCursorLocation %d %d %d %d" % (x, y, width, height))

    @dbus.service.method(CONTEXT, in_signature="", out_signature="")
    def Destroy(self):
        self.note("Destroy")

    @dbus.service.signal(CONTEXT, signature="v")
    def CommitText(self, value):
        pass

    @dbus.service.signal(CONTEXT, signature="vub")
    def UpdatePreeditText(self, value, cursor, visible):
        pass


class Bus(dbus.service.Object):
    def __init__(self, bus, log):
        super().__init__(bus, "/org/freedesktop/IBus")
        self.bus = bus
        self.log = log
        self.contexts = []

    @dbus.service.method(IBUS, in_signature="s", out_signature="o")
    def CreateInputContext(self, client):
        path = "/org/freedesktop/IBus/InputContext_%d" % (len(self.contexts) + 1)
        self.contexts.append(Context(self.bus, path, self.log))
        with open(self.log, "a") as out:
            out.write("CreateInputContext %s\n" % client)
        return dbus.ObjectPath(path)


def main():
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SessionBus()
    name = dbus.service.BusName(IBUS, bus)
    service = Bus(bus, sys.argv[2])
    with open(sys.argv[1], "w") as ready:
        ready.write("ready\n")
    # GLib's own loop, through ctypes: python's gi is not needed.
    glib = ctypes.CDLL("libglib-2.0.so.0")
    glib.g_main_context_iteration.argtypes = [ctypes.c_void_p, ctypes.c_int]
    while True:
        glib.g_main_context_iteration(None, 0)
        time.sleep(0.005)
    del name, service


if __name__ == "__main__":
    main()
