"""The page's device choosers (docs/SYSTEM_INTEGRATION.md, 10): WebUSB, WebHID
and Web Serial. Without a function of the application's the page gets no
device; with one, a request of the kind with the system's matching devices,
answered with none or, where the machine has a device, with one, which the
page then has (getDevices) until it forgets it. Machines without devices
report that part as skipped.

    python3 tests/engine/device_chooser.py [engine switches...]

Exit 0 when every check passes.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import Server, activated, check, ev, finish, knob, skip, wait, zebkit  # noqa: E402

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


def main():
    server = Server({"/": b"<title>Devices</title>"})
    url = server.base + "/"
    if zebkit.init(["device-chooser"] + sys.argv[1:]) != 0:
        print("FAIL zebkit.init()")
        return 1
    try:
        view = zebkit.View(url, 400, 300)
        wait(lambda: ev(view, "location.href + document.readyState") == url + "complete")
        view.focus()
        only = knob("DEVICE_KINDS")
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
                skip("%s: a device chosen" % kind, "no device on this machine")
                continue

            answers = []

            def choose(request):
                answers.append(request.respond(request.metadata["devices"][0]["id"]))
            view.on_device_chooser(choose)
            chosen = ev(view, ask)
            check(isinstance(chosen, str) and chosen.startswith("device:"),
                  "%s: a device chosen, the page has it" % kind, (chosen, devices[0].get("name"), answers))
            kept = ev(view, count)
            check(isinstance(kept, int) and kept >= 1, "%s: and keeps it (getDevices)" % kind, kept)
            ev(view, forget)
            left = ev(view, count)
            check(left == 0, "%s: until it forgets it" % kind, left)
        view.close()
    except Exception as error:  # noqa: BLE001
        check(False, "the test ran to the end", repr(error))
    finally:
        zebkit.shutdown()
        server.close()
    return finish()


if __name__ == "__main__":
    sys.exit(main())
