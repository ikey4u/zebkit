"""What reached the system without the application's decision, now decided by it
(docs/SYSTEM_INTEGRATION.md, phase 2):

- File System Access's pickers go through the file chooser hook, and what a page
  may write beyond where it saves is the "file-system-write" permission;
- registerProtocolHandler() is the "protocol-handler" permission, denied by
  default, and a handler accepted handles its scheme's links;
- pointer lock and keyboard lock are permissions: pointer lock granted by
  default, keyboard lock denied.

    python3 tests/engine/system_holes.py [engine switches...]

Exit 0 when every check passes.
"""

import http.server
import os
import pathlib
import sys
import tempfile
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


class Pages(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b"<!DOCTYPE html><title>holes</title><body><input id=file type=file>"
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def wait(condition, seconds=15):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        zebkit.pump()
        if condition():
            return True
        time.sleep(.002)
    return False


def ev(view, script, seconds=15):
    box = []
    view.evaluate(script, lambda value, error=None: box.append((value, error)))
    if not wait(lambda: box, seconds):
        return ("timeout", script)
    return box[0][0] if box[0][1] is None else ("error", box[0][1])


def opened(url):
    view = zebkit.View()
    loads = []
    view.on_load(lambda phase, u: loads.append((phase, u)))
    view.load(url)
    wait(lambda: (zebkit.LOAD_FINISHED, url) in loads)
    wait(lambda: ev(view, "location.href + ' ' + document.readyState") == url + " complete")
    return view


def file_system_access(base, files):
    view = opened(base + "/page")
    alpha = files / "alpha.txt"
    alpha.write_text("alpha")
    asked = []

    def chooser(request):
        asked.append(request.metadata)
        mode = request.metadata["mode"]
        if mode == "open":
            request.respond([str(alpha)])
        elif mode == "save":
            request.respond([str(files / "saved.txt")])
        elif mode == "directory":
            request.respond([str(files)])
        else:
            request.respond([])
    view.on_file_chooser(chooser)

    text = ev(view, "showOpenFilePicker().then(([h]) => (window.h = h).getFile()).then(f => f.text())")
    check(text == "alpha", "showOpenFilePicker() goes through the file chooser hook, and the page reads what was picked",
          text)
    check(asked and asked[-1]["mode"] == "open" and asked[-1]["api"] == "file-system-access",
          "the hook is told the File System Access picker and its mode", asked[-1:] or None)

    write = "h.createWritable().then(w => w.write('beta').then(() => w.close())).then(() => 'wrote', e => e.name)"
    denied = ev(view, write)
    check(denied == "NotAllowedError", "writing to an opened file is refused without the application's decision", denied)
    check(alpha.read_text() == "alpha", "and the file is unchanged")

    permissions = []

    def permission(request):
        permissions.append(request.metadata)
        request.respond("granted" if request.metadata["permission"] == "file-system-write" else "denied")
    view.on_permission(permission)
    wrote = ev(view, "h.requestPermission({mode: 'readwrite'}).then(s => s === 'granted' ? " + write + " : s)")
    check(wrote == "wrote", "with the permission hook granting file-system-write, the page writes", wrote)
    check(alpha.read_text() == "beta", "and the file has what it wrote", alpha.read_text())
    asked_write = [p for p in permissions if p["permission"] == "file-system-write"]
    check(asked_write and asked_write[0]["path"] == str(alpha) and asked_write[0]["kind"] == "file",
          "the permission request names the file", asked_write[:1] or None)

    saved = ev(view, "showSaveFilePicker({suggestedName: 'saved.txt'}).then(h => h.createWritable())"
                     ".then(w => w.write('gamma').then(() => w.close())).then(() => 'saved', e => e.name)")
    check(saved == "saved" and (files / "saved.txt").read_text() == "gamma",
          "where the person saves, the page writes, without another question", saved)
    check(asked[-1]["mode"] == "save", "showSaveFilePicker() is the hook's save mode", asked[-1])

    names = ev(view, "showDirectoryPicker().then(async d => { const n = []; for await (const k of d.keys()) n.push(k);"
                     " return n.sort().join(','); })")
    check(isinstance(names, str) and "alpha.txt" in names.split(","), "showDirectoryPicker() picks a directory", names)
    check(asked[-1]["mode"] == "directory", "the hook's directory mode", asked[-1])

    ev(view, "document.getElementById('file').click(); 1")
    wait(lambda: asked and asked[-1].get("api") == "input", 5)
    check(asked[-1].get("api") == "input", "<input type=file> is the hook's input", asked[-1])
    view.close()


def protocol_handlers(base):
    view = opened(base + "/page")
    told = []
    view.on_external_protocol(lambda event: told.append(event))
    register = "navigator.registerProtocolHandler('web+zebkit', '/handler?u=%s'); 1"
    ev(view, register)
    ev(view, "location.href = 'web+zebkit:one'; 1")
    wait(lambda: told, 5)
    check(told and view.url.startswith(base + "/page"),
          "without a decision, a protocol handler is not registered: its links stay the system's", (told, view.url))

    asked = []

    def permission(request):
        asked.append(request.metadata)
        request.respond("granted")
    view.on_permission(permission)
    ev(view, register)
    wait(lambda: asked, 5)
    check(asked and asked[0]["permission"] == "protocol-handler" and asked[0]["scheme"] == "web+zebkit"
          and asked[0]["url"].startswith(base + "/handler"),
          "registerProtocolHandler() asks the application, with the scheme and the handler", asked[:1] or None)
    ev(view, "location.href = 'web+zebkit:two'; 1")
    wait(lambda: "/handler" in view.url, 10)
    check(view.url.startswith(base + "/handler?u=web%2Bzebkit%3Atwo"), "a handler granted handles its links", view.url)
    view.close()


def locks(base):
    view = opened(base + "/page")
    view.focus()
    pointer = "document.body.requestPointerLock().then(() => 'locked', e => e.name)"
    settled = lambda value: value is not None and not (isinstance(value, tuple) and value[0] == "timeout")
    if zebkit.is_headless():
        refused = ev(view, pointer)
        check(settled(refused) and refused != "locked",
              "without a window there is no pointer to lock: refused, and the engine goes on", refused)
    elif not (view.window or {}).get("focused", True):
        print("ok   pointer lock: not run, the window cannot have focus (a locked screen?)", flush=True)
    else:
        locked = ev(view, pointer)
        check(locked == "locked", "pointer lock is granted by default, as before", locked)
        ev(view, "document.exitPointerLock(); 1")
        view.set_permission("pointer-lock", "denied", "*")
        refused = ev(view, pointer)
        check(settled(refused) and refused != "locked", "and refused when the application denies it", refused)

    keyboard = "navigator.keyboard.lock().then(() => 'locked', e => e.name)"
    denied = ev(view, keyboard)
    check(settled(denied) and denied != "locked", "keyboard lock is denied by default", denied)
    view.set_permission("keyboard-lock", "granted", "*")
    granted = ev(view, keyboard)
    check(granted == "locked", "and granted when the application grants it", granted)
    view.close()


def main():
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Pages)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = "http://127.0.0.1:%d" % server.server_address[1]
    with tempfile.TemporaryDirectory(prefix="zebkit-holes-") as directory:
        files = pathlib.Path(directory) / "files"
        files.mkdir()
        if zebkit.init(["system-holes"] + sys.argv[1:]) != 0:
            print("FAIL zebkit.init()")
            return 1
        try:
            for test in (lambda: file_system_access(base, files), lambda: protocol_handlers(base),
                         lambda: locks(base)):
                try:
                    test()
                except Exception as error:  # noqa: BLE001
                    check(False, "the test ran to the end", repr(error))
        finally:
            zebkit.shutdown()
            server.shutdown()
    print("%d of %d checks passed" % (checks - len(failures), checks), flush=True)
    return 1 if failures or not checks else 0


if __name__ == "__main__":
    sys.exit(main())
