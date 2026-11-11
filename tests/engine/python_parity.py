"""The Python binding's functions that came with parity with Rust and Go
(scripts/check-bindings.py): contexts, the view's window and script calls,
script outside pages, off-screen views (frames, input, IME, caret, cursor),
dialogs, printing, closing, drag and drop, media capture, the rest of the PDF
API, the automation page id and the media probe.

    python3 tests/engine/python_parity.py [engine switches...]

PDFium's test documents come from the checkout's Chromium tree, or
ZEBKIT_PDF_RESOURCES. Exit 0 when every check passes.
"""

import http.server
import json
import os
import pathlib
import sys
import tempfile
import threading
import time

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(ROOT / "bindings" / "python"))

import zebkit  # noqa: E402
import zebkit.automation  # noqa: E402
import zebkit.media_tools  # noqa: E402

MEDIA = HERE / "pages" / "media"
RESOURCES = pathlib.Path(os.environ.get("ZEBKIT_PDF_RESOURCES") or
                         ROOT / "upstream" / "chromium" / "src" / "third_party" / "pdfium" / "testing" / "resources")

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
    view.evaluate(script, lambda value, error=None: box.append(value if error is None else ("error", error)))
    wait(lambda: box, seconds)
    return box[0] if box else None


def loaded(view, url):
    return wait(lambda: ev(view, "location.href + document.readyState") == url + "complete", 15)


PAGE = b"""<title>Parity</title>
<input id=field style="position:absolute;left:10px;top:10px;width:200px;height:30px">
<button id=button style="position:absolute;left:10px;top:60px;width:100px;height:30px"
        onclick="document.title='clicked'">Go</button>
<a href="#" style="position:absolute;left:10px;top:110px;cursor:pointer">link</a>
<div id=target style="position:absolute;left:10px;top:150px;width:200px;height:60px">drop here</div>
<script>
target.addEventListener('dragover', e => e.preventDefault());
target.addEventListener('drop', e => { e.preventDefault(); target.textContent = 'dropped ' + e.dataTransfer.getData('text/plain'); });
</script>"""
VIDEO = b'<title>Video</title><video id=v src="/media/clip.mp4" muted loop autoplay width=320></video>'


class Server(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith("/media/"):
            body, kind = (MEDIA / self.path[len("/media/"):]).read_bytes(), "video/mp4"
        elif self.path == "/video":
            body, kind = VIDEO, "text/html"
        elif self.path == "/cookies":
            # A header given twice (the engine keeps Set-Cookie to itself).
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("X-Twice", "a=1")
            self.send_header("X-Twice", "b=2")
            self.send_header("Content-Length", "2")
            self.end_headers()
            self.wfile.write(b"ok")
            return
        else:
            body, kind = PAGE, "text/html"
        self.send_response(200)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def check_core(url):
    # Contexts: cookies of their own.
    shared = zebkit.View(url, 400, 300)
    context = zebkit.Context()
    isolated = zebkit.View(url, 400, 300, context=context)
    check(loaded(shared, url) and loaded(isolated, url), "a view in a Context loads")
    ev(isolated, "document.cookie = 'only=here; path=/'; 1")
    check(ev(isolated, "document.cookie") == "only=here" and ev(shared, "document.cookie") == "",
          "a Context's cookies are its own")
    closed = []
    isolated.on_close(lambda: closed.append(1))
    context.close()
    check(wait(lambda: closed, 10), "closing the Context closes its views")

    # The view's window and calls.
    view = shared
    view.set_title("A title of the application's")
    view.set_title(None)
    view.set_size_limits(200, 100, True)
    view.set_visible(True)
    view.add_script("window.added = 'before the page'")
    view.set_user_agent("ParityAgent/1.0")
    view.load(url + "?again")
    check(loaded(view, url + "?again") and ev(view, "window.added") == "before the page",
          "add_script runs before the page")
    check(ev(view, "navigator.userAgent") == "ParityAgent/1.0", "set_user_agent")
    view.set_user_agent(None)
    check(view.is_loading is False, "is_loading")
    view.stop()
    shots = []
    view.capture(lambda png, error: shots.append((png, error)))
    check(wait(lambda: shots, 15) and shots[0][0][:8] == b"\x89PNG\r\n\x1a\n", "capture",
          shots[0][1] if shots else None)
    child = zebkit.View(None, 100, 100)
    # In the parent's window (headless too, where views have windows of
    # their own); refused, with an Error, only for a parent without one.
    try:
        child.attach(view, 0, 0, 100, 100)
    except zebkit.Error:
        pass
    try:
        child.attach(child, 0, 0, 10, 10)
        check(False, "attach refuses the view as its own parent")
    except zebkit.Error:
        check(True, "attach refuses the view as its own parent")
    child.close()
    view.on_drop(lambda paths: None)
    view.on_drop(None)
    opened = []
    view.on_new_window(lambda target: opened.append(target))
    ev(view, "new Promise(r => setTimeout(r, 200)).then(() => { window.open('%sopened', '_blank'); return 1 })" % url)
    wait(lambda: opened, 5)
    check(opened == [url + "opened"], "on_new_window", opened)
    view.on_new_window(None)
    check(zebkit.automation.page_id(view) not in (None, ""), "automation.page_id", zebkit.automation.page_id(view))
    return view


def check_script():
    script = zebkit.Script(["an", "argument"])
    results, idle = [], []
    script.evaluate("1 + 2", lambda value, error: results.append((value, error)))
    check(wait(lambda: results, 10) and results[0] == (3, None), "Script.evaluate", results)
    with tempfile.TemporaryDirectory(prefix="zebkit-script-") as directory:
        module = pathlib.Path(directory) / "module.mjs"
        module.write_text("globalThis.ran = 6 * 7;\n")
        modules = []
        script.run_module(module, lambda value, error: modules.append(error))
        check(wait(lambda: modules, 10) and modules[0] is None, "Script.run_module", modules)
    after = []
    script.evaluate("globalThis.ran", lambda value, error: after.append(value))
    check(wait(lambda: after, 10) and after[0] == 42, "a module's work stays in the runtime", after)
    script.on_idle(lambda: idle.append(1))
    check(wait(lambda: idle, 10), "Script.on_idle")
    script.close()


def check_offscreen(url):
    view = zebkit.View.offscreen(url, 320, 240)
    check(view.is_offscreen, "View.offscreen")
    frames = []

    def frame(given):
        frames.append({"width": given.width, "height": given.height, "layer": given.layer,
                       "pixels": given.pixels(), "stride": given.stride, "damage": given.damage, "kind": given.kind})
    view.on_frame(frame)
    view.set_max_frames_in_flight(2)
    view.set_frame_rate(60)
    view.set_frame_buffers(zebkit.BUFFERS_SHARED_MEMORY)
    check(loaded(view, url), "an off-screen view loads")
    view.request_frame()
    check(wait(lambda: frames, 10), "on_frame")
    if frames:
        last = frames[-1]
        check(last["width"] == 320 and last["height"] == 240 and last["kind"] == zebkit.FRAME_SHARED_MEMORY and
              last["pixels"] is not None and len(last["pixels"]) >= 320 * 4 and last["damage"],
              "a frame's size, pixels and damage", {k: v for k, v in last.items() if k != "pixels"})
    view.set_focus(True)
    carets, cursors = [], []
    view.on_caret(lambda rect: carets.append(rect))
    view.on_cursor(lambda name, image: cursors.append(name))
    view.on_popup(lambda shown, bounds: None)
    view.send_mouse(zebkit.MOUSE_MOVE, 60, 75)
    view.send_mouse(zebkit.MOUSE_DOWN, 60, 75, zebkit.BUTTON_LEFT, 1)
    view.send_mouse(zebkit.MOUSE_UP, 60, 75, zebkit.BUTTON_LEFT, 1)
    check(wait(lambda: view.title == "clicked", 10), "send_mouse clicks the button", view.title)
    view.send_mouse(zebkit.MOUSE_MOVE, 30, 115)
    wait(lambda: "pointer" in cursors or "hand" in cursors, 5)
    check(cursors, "on_cursor", cursors[-3:])
    view.send_wheel(100, 100, 0, -40)
    view.send_mouse(zebkit.MOUSE_DOWN, 50, 25, zebkit.BUTTON_LEFT, 1)
    view.send_mouse(zebkit.MOUSE_UP, 50, 25, zebkit.BUTTON_LEFT, 1)
    wait(lambda: ev(view, "document.activeElement.id") == "field", 5)
    for char in "hi":
        view.send_key(zebkit.KEY_DOWN, key_code=ord(char.upper()), code="Key" + char.upper(), key=char)
        view.send_key(zebkit.KEY_CHAR, text=char, unmodified_text=char)
        view.send_key(zebkit.KEY_UP, key_code=ord(char.upper()), code="Key" + char.upper(), key=char)
    check(wait(lambda: ev(view, "field.value") == "hi", 5), "send_key types", ev(view, "field.value"))
    check(wait(lambda: carets and carets[-1] is not None, 5), "on_caret", carets[-1:])
    view.ime_set_composition("にほ", 0, 2)
    view.ime_commit("日本")
    check(wait(lambda: ev(view, "field.value") == "hi日本", 5), "ime_set_composition and ime_commit",
          ev(view, "field.value"))
    view.ime_set_composition("x", 0, 1)
    view.ime_cancel()
    ok = view.send_key_with_commands([("selectAll", None)], zebkit.KEY_DOWN, key_code=0x41, code="KeyA", key="a",
                                     modifiers=zebkit.MODIFIER_CONTROL)
    check(ok and wait(lambda: ev(view, "field.selectionEnd - field.selectionStart") == 4, 5),
          "send_key_with_commands", ev(view, "field.selectionEnd - field.selectionStart"))
    frames.clear()
    view.set_size(320, 240, 2.0)
    view.request_frame()
    check(wait(lambda: any(f["width"] == 640 and f["height"] == 480 for f in frames), 10), "set_size at scale 2",
          [(f["width"], f["height"]) for f in frames[-3:]])
    view.on_frame(None)
    return view


def check_page(url, view):
    # Dialogs held for the application.
    dialogs, closed = [], []
    view.set_dialog_policy(zebkit.DIALOG_POLICY_HOLD)
    check(view.dialog_policy == zebkit.DIALOG_POLICY_HOLD, "set_dialog_policy and dialog_policy")
    view.on_dialog(lambda dialog: dialogs.append(dialog))
    view.on_dialog_closed(lambda dialog_id, accepted, text: closed.append((dialog_id, accepted, text)))
    ev(view, "setTimeout(() => window.answer = prompt('Your name?', 'nobody'), 0); 1")
    check(wait(lambda: dialogs, 10) and dialogs[0]["type"] == zebkit.DIALOG_PROMPT and
          dialogs[0]["defaultPrompt"] == "nobody" and dialogs[0]["held"], "on_dialog: a held prompt", dialogs[:1])
    check(any("Your name?" in json.dumps(d) for d in view.dialogs()), "dialogs()", view.dialogs())
    if dialogs:
        check(view.respond_dialog(dialogs[0]["id"], True, "Ada"), "respond_dialog")
    check(wait(lambda: closed, 10) and closed[0][1:] == (True, "Ada") and ev(view, "window.answer") == "Ada",
          "on_dialog_closed, and the page's answer", closed)
    view.set_dialog_policy(zebkit.DIALOG_POLICY_ACCEPT)
    check(ev(view, "confirm('Sure?')") is True, "DIALOG_POLICY_ACCEPT answers confirm()")
    view.on_dialog(None)
    view.on_dialog_closed(None)
    view.set_dialog_policy(zebkit.DIALOG_POLICY_DEFAULT)

    # Printing.
    pdfs = []
    view.print_to_pdf(lambda pdf, error: pdfs.append((pdf, error)), {"printBackground": True})
    check(wait(lambda: pdfs, 30) and pdfs[0][0][:4] == b"%PDF", "print_to_pdf", pdfs[0][1] if pdfs else None)

    # Drag and drop into the page, from outside.
    starts, ends, over, dropped = [], [], [], []
    view.on_drag_start(lambda drag: starts.append(drag) or False)
    view.on_drag_end(lambda operation: ends.append(operation))
    view.drag_enter({"text": "a note"}, 50, 170)
    view.drag_over(60, 175, function=lambda operation: over.append(operation))
    wait(lambda: over, 5)
    view.drop(60, 175, function=lambda operation: dropped.append(operation))
    check(wait(lambda: dropped, 5) and wait(lambda: "dropped a note" in (ev(view, "target.textContent") or ""), 5),
          "drag_enter, drag_over and drop", (over, dropped, ev(view, "target.textContent")))
    check(view.is_dragging is False, "is_dragging")
    view.drag_enter({"text": "x"}, 50, 170)
    view.drag_leave()
    view.drag_source_end(0, 0, zebkit.DRAG_NONE)
    view.on_drag_start(None)
    view.on_drag_end(None)

    # Closing as a person would.
    closing = zebkit.View(url, 200, 100)
    gone = []
    closing.on_close(lambda: gone.append(1))
    loaded(closing, url)
    closing.request_close()
    check(wait(lambda: gone, 10), "request_close")


def check_media(base):
    zebkit.media_keep_history(True)
    view = zebkit.View(base + "/video", 400, 300)
    loaded(view, base + "/video")
    media = []

    def listed():
        box = []
        view.media_list(lambda value, error: box.append(value))
        wait(lambda: box, 5)
        media[:] = box[0] or [] if box else []
        return len(media) == 1 and media[0].get("playing") is True
    wait(listed, 20)
    check(len(media) == 1 and media[0].get("capturable") is True, "View.media_list", media)
    at = []
    view.media_at(20, 20, lambda value, error: at.append((value, error)))
    check(wait(lambda: at, 10) and at[0][1] is None and at[0][0] and "id" in at[0][0], "View.media_at", at)
    if media:
        with tempfile.TemporaryDirectory(prefix="zebkit-capture-") as directory:
            out = pathlib.Path(directory) / "captured.mp4"
            capture = view.media_capture(media[0]["id"], out)
            progress, done = [], []
            capture.on_progress(lambda value: progress.append(value))
            capture.on_done(lambda value: done.append(value))
            wait(lambda: progress, 10)
            wait(lambda: False, 1.5)
            status = capture.status()
            check(status and "state" in status, "MediaCapture.status", status)
            capture.stop()
            check(wait(lambda: done, 20) and progress, "MediaCapture: on_progress, stop, on_done", done)
            check(out.exists() and out.stat().st_size > 1000, "the captured file")
            capture.close()
    probed = zebkit.media_tools.probe(str(MEDIA / "clip.mp4"))
    check(probed.get("streams") and probed.get("format"), "media_tools.probe (zebkit_media_probe)",
          list(probed)[:4])
    view.close()


def check_pdf():
    hello = (RESOURCES / "hello_world.pdf").read_bytes()
    with zebkit.Pdf(hello) as pdf:
        lines = pdf.text_lines(0)
        check(lines and "Hello" in lines[0]["text"], "Pdf.text_lines", lines[:1])
        check(pdf.images(0) == [], "Pdf.images")
        check(pdf.signatures() == [], "Pdf.signatures")
        try:
            pdf.image_data(0, 0)
            check(False, "Pdf.image_data refuses an image the page has not")
        except zebkit.Error:
            check(True, "Pdf.image_data refuses an image the page has not")
        try:
            pdf.signature_contents(0)
            check(False, "Pdf.signature_contents refuses a signature it has not")
        except zebkit.Error:
            check(True, "Pdf.signature_contents refuses a signature it has not")
        stamp = pdf.add_annotation(0, {"type": "stamp", "rect": [100, 100, 140, 140]})
        pdf.set_annotation_image(0, stamp, bytes([0, 0, 255, 255]), 1, 1)
        shown = pdf.render_annotation(0, stamp)
        middle = None
        if shown:
            width, height, rgba = shown
            at = ((height // 2) * width + width // 2) * 4
            middle = tuple(rgba[at:at + 4])
        check(middle is not None and middle[2] > 200 and middle[0] < 60 and middle[1] < 60,
              "Pdf.set_annotation_image: the stamp shows the image", middle)
        pdf.rotate_page(0, 90)
        check(pdf.page(0).get("rotation") == 90, "Pdf.rotate_page", pdf.page(0))
        pdf.insert_page(1, 300, 400)
        check(pdf.page_count == 2, "Pdf.insert_page")
        pdf.move_pages([1], 0)
        check(round(pdf.page(0)["width"]) == 300, "Pdf.move_pages", pdf.page(0))
        pdf.delete_pages([0])
        check(pdf.page_count == 1, "Pdf.delete_pages")
        with zebkit.Pdf(hello) as other:
            pdf.import_pages(other)
        check(pdf.page_count == 2, "Pdf.import_pages")
        count = pdf.redact(1, [[0, 0, 600, 800]], 0x000000FF)
        check(count == 1 and "Hello" not in pdf.text(1), "Pdf.redact", count)
    blank = zebkit.Pdf.new()
    check(blank.page_count == 0, "Pdf.new")
    blank.insert_page(0, 100, 100)
    check(blank.page_count == 1, "a new document's page")
    blank.close()
    with zebkit.Pdf((RESOURCES / "text_form.pdf").read_bytes()) as form:
        fields = form.form_fields()
        check(fields and fields[0]["type"] == "text", "Pdf.form_fields", fields[:1])
        if fields:
            name = fields[0]["name"]
            form.set_field(name, "from Python")
            check(form.form_fields()[0]["value"] == "from Python", "Pdf.set_field")
            form.reset_form()
            check(form.form_fields()[0]["value"] != "from Python", "Pdf.reset_form")


def check_review_fixes(url):
    """What the review of 2026-11-10 found in the binding, fixed."""
    view = zebkit.View(url, 400, 300)
    check(loaded(view, url), "a view for the fixes loads")

    # The engine's thread only: another thread gets an Error, not a crash.
    caught = []

    def elsewhere():
        try:
            view.load(url)
        except zebkit.Error as error:
            caught.append(str(error))
    worker = threading.Thread(target=elsewhere)
    worker.start()
    worker.join()
    check(caught and "engine's thread" in caught[0], "a view used from another thread raises Error", caught)

    # A title function that replaces itself, then one that removes itself.
    titles = []

    def second(title):
        titles.append(("second", title))
        view.on_title(None)

    def first(title):
        titles.append(("first", title))
        view.on_title(second)
    view.on_title(first)
    for name in ("one", "two", "three"):
        ev(view, "document.title = %r; 1" % name)
        wait(lambda: False, .3)
    check(titles[:2] == [("first", "one"), ("second", "two")] and len(titles) == 2,
          "on_title replaced and removed from inside itself", titles)
    seen_loads = []
    view.on_load(lambda event, at: seen_loads.append(event))
    view.on_load(None)
    view.load(url + "?unseen")
    wait(lambda: ev(view, "location.search + document.readyState") == "?unseencomplete", 15)
    check(not seen_loads and ev(view, "location.search") == "?unseen", "on_load(None) stops on_load", seen_loads)

    # What is for the engine's thread raises Error on another (pump, CDP,
    # is_headless, set_wakeup, ...), rather than run there.
    refused = []

    def off_thread():
        for name, call in (("pump", zebkit.pump), ("is_headless", zebkit.is_headless),
                           ("cdp_listen", lambda: zebkit.cdp_listen(None, 0)), ("cdp_stop", zebkit.cdp_stop),
                           ("set_wakeup", lambda: zebkit.set_wakeup(None))):
            try:
                call()
            except zebkit.Error:
                refused.append(name)
    worker = threading.Thread(target=off_thread)
    worker.start()
    worker.join()
    check(len(refused) == 5, "engine-thread functions raise Error on another thread", refused)

    # The three hooks the binding had declared and not given.
    results = []
    view.on_eye_dropper_result(results.append)
    view.set_eye_dropper_timeout(1000)
    view.on_share_result(results.append)
    view.on_eye_dropper_result(None)
    view.on_share_result(None)
    try:
        view.set_eye_dropper_timeout(0)
        check(False, "set_eye_dropper_timeout refuses 0")
    except ValueError:
        check(True, "on_eye_dropper_result, set_eye_dropper_timeout, on_share_result")

    # The engine's own reason for a refused filter.
    try:
        view.set_network_filter([{"mode": "sometimes"}])
        check(False, "a bad network filter raises")
    except zebkit.Error as error:
        check(str(error) and str(error) != "network filter rejected", "a refused network filter says why", str(error))

    # A stream let go open ends as failed, as Rust's does when dropped.
    streamed = []

    def asked(request):
        stream = request.respond_stream(status=200, headers=[{"name": "Content-Type", "value": "text/plain"}])
        streamed.append(bool(stream and stream.write(b"part")))
        # Not ended, and not kept: collected here, it ends as failed.
    view.set_network_filter([{"url": "**/streamed"}])
    view.on_network(asked)
    got = ev(view, "fetch('/streamed').then(r => r.text()).then(t => 'read ' + t, e => 'failed')")
    view.on_network(None)
    view.set_network_filter([])
    check(streamed == [True] and got == "failed", "a NetworkStream let go open ends as failed", (streamed, got))

    # Every header of a response, repeated ones too.
    responses = []
    zebkit.fetch(url + "cookies", responses.append)
    wait(lambda: responses, 15)
    response = responses[0] if responses else {}
    cookies = [value for name, value in response.get("header_list", []) if name.lower() == "x-twice"]
    check(cookies == ["a=1", "b=2"] and response["headers"], "fetch's header_list keeps repeated headers",
          response.get("header_list"))

    # A bound function that answers undefined.
    view.grant("*", zebkit.ALLOW_APP)
    view.bind("nothing", lambda origin=None: zebkit.UNDEFINED)
    view.reload()
    loaded(view, url)
    # zebkit_call_resolve(NULL): undefined, as <zebkit/zebkit.h> says (the
    # engine gives null so far, as it does for Rust's Call::resolve_undefined).
    answer = ev(view, "zebkit.app.nothing().then(v => v == null)")
    check(answer is True, "UNDEFINED resolves with nothing", answer)
    view.bind("nothing", None)

    # An automation page's event function is let go after its view closes.
    page = zebkit.automation.Page(view)
    callback = page._event_callback
    check(id(callback) in zebkit._alive, "an automation page's events are kept while its view is open")
    closed = []
    view.on_close(lambda: closed.append(True))
    view.close()
    wait(lambda: closed, 10)
    wait(lambda: False, .3)
    check(id(callback) not in zebkit._alive and page._event_callback is None,
          "an automation page's events are let go after its view closes")
    try:
        view.on_load(lambda event, url: None)
        check(False, "on_load on a closed view raises")
    except zebkit.Error:
        check(True, "on_load on a closed view raises Error")

    # A frame's pixels without a copy.
    offscreen = zebkit.View.offscreen(url, 120, 80, 1.0)
    offscreen.set_frame_buffers(zebkit.BUFFERS_SHARED_MEMORY)
    seen = []

    def frame(given):
        memory = given.memory()
        if memory is not None and not seen:
            seen.append((bytes(memory) == given.pixels(), memory.readonly))
        given.release()
        if seen:
            check(given.memory() is None, "Frame.memory() is None once released")
            offscreen.on_frame(None)
    offscreen.on_frame(frame)
    loaded(offscreen, url)
    offscreen.request_frame()
    check(wait(lambda: seen, 10) and seen[0] == (True, True), "Frame.memory(): the pixels, read-only", seen)
    offscreen.close()

    # A closed document raises Error.
    document = zebkit.Pdf.new()
    document.close()
    try:
        document.info()
        check(False, "a closed Pdf raises")
    except zebkit.Error:
        check(True, "a closed Pdf raises Error")

    # What the module says it has, it has.
    check(all(hasattr(zebkit, name) for name in zebkit.__all__) and "Tray" in zebkit.__all__,
          "__all__ names what the module has", [n for n in zebkit.__all__ if not hasattr(zebkit, n)])


def main():
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Server)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = "http://127.0.0.1:%d" % server.server_address[1]
    url = base + "/"
    if zebkit.init(["python-parity"] + sys.argv[1:]) != 0:
        print("FAIL zebkit.init()")
        return 1
    try:
        view = check_core(url)
        check_script()
        offscreen = check_offscreen(url)
        check_page(url, offscreen)
        offscreen.close()
        view.close()
        check_media(base)
        check_pdf()
        check_review_fixes(url)
    except Exception as error:  # noqa: BLE001
        import traceback
        traceback.print_exc()
        check(False, "the test ran to the end", repr(error))
    finally:
        zebkit.shutdown()
        server.shutdown()
    print("%d of %d checks passed" % (checks - len(failures), checks), flush=True)
    return 1 if failures or not checks else 0


if __name__ == "__main__":
    sys.exit(main())
