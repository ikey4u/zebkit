"""The page's device choosers (docs/SYSTEM_INTEGRATION.md, 10): WebUSB, WebHID
and Web Serial. Without a function of the application's the page gets no
device; with one, a request of the kind with the system's matching devices,
answered with none or, where the machine has a device, with one, which the
page then has (getDevices) until it forgets it. Machines without devices
report that part "not run".

    python3 tests/engine/device_chooser.py [engine switches...]

Exit 0 when every check passes.
"""

import http.server
import json
import os
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "bindings", "python"))

import zebkit  # noqa: E402

failures = []
checks = 0


def check(ok, what, detail=None):
    global checks
    checks += 1
    print(("ok   " if ok else "FAIL ") + what + (": %r" % (detail,) if detail is not None else ""), flush=True)
    if not ok:
        failures.append(what)


def wait(condition, seconds=10):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        zebkit.pump()
        if condition():
            return True
        time.sleep(.005)
    return False


def ev(view, script, seconds=15):
    box = []
    view.evaluate(script, lambda value, error=None: box.append(value if error is None else "error: %s" % error))
    wait(lambda: box, seconds)
    return box[0] if box else None


class Page(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b"<title>Devices</title>"
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


# Each kind: how the page asks, what "none" is, and what it has afterwards.
KINDS = {
    "usb": ("navigator.usb.requestDevice({filters: []}).then(d => 'device:' + d.productId, e => e.name)",
            "NotFoundError",
            "navigator.usb.getDevices().then(l => l.length)",
            "navigator.usb.getDevices().then(async l => { for (const d of l) await d.forget(); return 'forgot'; })"),
    "hid": ("navigator.hid.requestDevice({filters: []}).then(l => l.length ? 'device:' + l[0].productId : 'none', "
            "e => e.name)",
            "none",
            "navigator.hid.getDevices().then(l => l.length)",
            "navigator.hid.getDevices().then(async l => { for (const d of l) await d.forget(); return 'forgot'; })"),
    "serial": ("navigator.serial.requestPort().then(p => 'device:' + (p.getInfo().usbProductId ?? 0), e => e.name)",
               "NotFoundError",
               "navigator.serial.getPorts().then(l => l.length)",
               "navigator.serial.getPorts().then(async l => { for (const p of l) await p.forget(); return 'forgot'; })"),
}


def activated(script):
    """@script once the browser has the evaluation's user activation: it reaches
    the browser on a pipe of its own, and these requests consume it there."""
    return "new Promise(r => setTimeout(r, 200)).then(() => %s)" % script


def main():
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Page)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = "http://127.0.0.1:%d/" % server.server_address[1]
    if zebkit.init(["device-chooser"] + sys.argv[1:]) != 0:
        print("FAIL zebkit.init()")
        return 1
    try:
        view = zebkit.View(url, 400, 300)
        wait(lambda: ev(view, "location.href + document.readyState") == url + "complete")
        view.focus()
        only = os.environ.get("DEVICE_KINDS")
        for kind, (ask, none, count, forget) in KINDS.items():
            if only and kind not in only.split(","):
                continue
            present = ev(view, "typeof navigator.%s" % kind)
            check(present == "object", "%s: the page has navigator.%s" % (kind, kind), present)
            if present != "object":
                continue
            view.on_device_chooser(None)
            ask = activated(ask)
            alone = ev(view, ask)
            check(alone == none, "%s: without a function the page gets no device" % kind, alone)
            asked = []

            def refuse(request):
                asked.append(request.metadata)
                check(not request.respond("no-such-device"), "%s: a device not listed is refused" % kind)
                request.respond()
            view.on_device_chooser(refuse)
            refused = ev(view, ask)
            meta = asked[0] if asked else {}
            check(meta.get("kind") == kind and isinstance(meta.get("devices"), list) and
                  meta.get("origin") == url.rstrip("/"), "%s: the application is asked, with the devices" % kind,
                  {k: meta.get(k) for k in ("kind", "origin", "filters")})
            check(refused == none, "%s: answered with none, the page gets none" % kind, refused)
            devices = meta.get("devices") or []
            print("     %s: %d device(s) here" % (kind, len(devices)), flush=True)
            if not devices:
                print("not run %s: a device chosen (no device on this machine)" % kind, flush=True)
                continue

            answers = []

            def choose(request):
                answers.append(request.respond(request.metadata["devices"][0]["id"]))
            view.on_device_chooser(choose)
            chosen = ev(view, ask)
            check(isinstance(chosen, str) and chosen.startswith("device:"),
                  "%s: a device chosen, the page has it" % kind, (chosen, devices[0].get("name"), answers))
            check(ev(view, count) >= 1, "%s: and keeps it (getDevices)" % kind)
            ev(view, forget)
            check(ev(view, count) == 0, "%s: until it forgets it" % kind)
        view.close()
    except Exception as error:  # noqa: BLE001
        check(False, "the test ran to the end", repr(error))
    finally:
        zebkit.shutdown()
        server.shutdown()
    print("%d of %d checks passed" % (checks - len(failures), checks), flush=True)
    return 1 if failures or not checks else 0


if __name__ == "__main__":
    sys.exit(main())
