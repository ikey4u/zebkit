"""The pages' accessibility trees turned on when an assistive technology asks
(docs/SYSTEM_INTEGRATION.md, 11), told in zebkit.environment()["accessibility"].

    python3 tests/engine/accessibility.py [engine switches...]

macOS: AXManualAccessibility set on the application, as an assistive client
sets it (and Electron's applications), through the Objective-C runtime. Other
systems: --force-renderer-accessibility, content's own switch, as their
detection of a screen reader cannot be faked here. What reads the trees
(AT-SPI, NSAccessibility, UI Automation clients) is not on the test machines.
Exit 0 when every check passes.
"""

import ctypes
import ctypes.util
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import check, finish, wait, zebkit  # noqa: E402


def ask_like_an_assistive_client():
    objc = ctypes.cdll.LoadLibrary(ctypes.util.find_library("objc"))
    objc.objc_getClass.restype = ctypes.c_void_p
    objc.sel_registerName.restype = ctypes.c_void_p
    send = objc.objc_msgSend
    send.restype = ctypes.c_void_p
    send.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    app = send(objc.objc_getClass(b"NSApplication"), objc.sel_registerName(b"sharedApplication"))
    number = ctypes.cast(objc.objc_msgSend, ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_bool))
    yes = number(objc.objc_getClass(b"NSNumber"), objc.sel_registerName(b"numberWithBool:"), True)
    string = ctypes.cast(objc.objc_msgSend, ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p))
    attribute = string(objc.objc_getClass(b"NSString"), objc.sel_registerName(b"stringWithUTF8String:"),
                       b"AXManualAccessibility")
    set_value = ctypes.cast(objc.objc_msgSend, ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p,
                                                                 ctypes.c_void_p, ctypes.c_void_p))
    set_value(app, objc.sel_registerName(b"accessibilitySetValue:forAttribute:"), yes, attribute)


def main():
    mac = sys.platform == "darwin"
    switches = sys.argv[1:] + ([] if mac else ["--force-renderer-accessibility"])
    if zebkit.init(["accessibility"] + switches) != 0:
        print("FAIL zebkit.init()")
        return 1
    try:
        view = zebkit.View("data:text/html,<title>a11y</title><button>Send the letter</button>")
        wait(lambda: zebkit.environment() is not None)
        if mac:
            check(zebkit.environment()["accessibility"] == "off", "no tree until something asks",
                  zebkit.environment()["accessibility"])
            ask_like_an_assistive_client()
        # The trees on: "complete", or "basic" where Chromium leaves the screen
        # reader's part to its own detection of one (macOS without VoiceOver).
        on = ("basic", "complete") if mac else ("complete",)
        wait(lambda: zebkit.environment()["accessibility"] in on, 5)
        check(zebkit.environment()["accessibility"] in on,
              "an assistive technology's request turns the pages' trees on", zebkit.environment()["accessibility"])
        view.close()
    except Exception as error:  # noqa: BLE001
        check(False, "the test ran to the end", repr(error))
    finally:
        zebkit.shutdown()
    return finish()


if __name__ == "__main__":
    sys.exit(main())
