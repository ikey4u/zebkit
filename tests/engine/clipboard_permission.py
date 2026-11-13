"""The clipboard's permissions (docs/SYSTEM_INTEGRATION.md, the clipboard
surface): a page writes to the clipboard after a click, as Chrome lets it
(clipboard-write, granted by default); reading it is the application's to
decide (clipboard-read): denied without a decision, read once granted.

The clipboard is written and read only where it is the engine's own, in
memory: Linux under --headless (as pdf_in_page.py). On a Mac, on Windows and
with a window it is the person's, and is not touched: there the read is only
refused, without and with the application's decision, and the rest is a skip.

    python3 tests/engine/clipboard_permission.py [engine switches...]

Exit 0 when every check passes.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import Server, activated, check, ev, finish, skip, wait, zebkit  # noqa: E402

WRITE = "navigator.clipboard.writeText('zebkit %s').then(() => 'written', e => e.name)"
READ = "navigator.clipboard.readText().then(text => text, e => e.name)"


def main():
    # A secure context's (http://127.0.0.1): navigator.clipboard is not elsewhere.
    server = Server({"/": b"<title>clipboard</title><p>a page</p>"})
    url = server.base + "/"
    if zebkit.init(["clipboard-permission"] + sys.argv[1:]) != 0:
        print("FAIL zebkit.init()")
        return 1
    try:
        view = zebkit.View(url, 400, 300)
        wait(lambda: ev(view, "location.href + document.readyState") == url + "complete")
        view.focus()
        own = sys.platform.startswith("linux") and "--headless" in sys.argv[1:]
        if own:
            written = ev(view, activated(WRITE % "one"))
            check(written == "written", "writing after a click: granted by default, as in Chrome", written)
        else:
            skip("writing and reading the clipboard",
                 "the clipboard here is the person's (only Linux under --headless has its own)")
        read = ev(view, activated(READ))
        check(read == "NotAllowedError", "reading without the application's decision: denied", read)
        asked = []

        def decide(request):
            asked.append(request.metadata.get("permission"))
            request.respond("granted" if own else "denied", False)
        view.on_permission(decide)
        read = ev(view, activated(READ))
        check("clipboard-read" in asked, "reading is asked of the application", asked)
        if own:
            check(read == "zebkit one", "granted: the page reads what was written", read)
        else:
            check(read == "NotAllowedError", "denied by the application: refused, the clipboard not read", read)
        view.on_permission(None)
        view.close()
    except Exception as error:  # noqa: BLE001
        check(False, "the test ran to the end", repr(error))
    finally:
        zebkit.shutdown()
        server.close()
    return finish()


if __name__ == "__main__":
    sys.exit(main())
