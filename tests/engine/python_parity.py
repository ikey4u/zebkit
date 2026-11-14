"""The Python binding's functions that came with parity with Rust and Go
(scripts/check-bindings.py): contexts, the view's window and script calls,
script outside pages, off-screen views (frames, input, IME, caret, cursor),
dialogs, printing, closing, drag and drop, media capture, the rest of the PDF
API, the automation page id and the media probe, and the C API's newer
functions (check_new_api).

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
        elif self.path == "/automation":
            body, kind = AUTOMATION, "text/html"
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
    popups = []
    view.on_popup(popups.append)
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
    # A <select>'s list: on_popup(bounds), then None when it is gone (Rust's and Go's shape).
    ev(view, "document.body.insertAdjacentHTML('beforeend', '<select id=pick style=\"position:absolute;left:250px;"
             "top:10px\"><option>a</option><option>b</option></select>'); 1")
    view.send_mouse(zebkit.MOUSE_DOWN, 265, 20, zebkit.BUTTON_LEFT, 1)
    view.send_mouse(zebkit.MOUSE_UP, 265, 20, zebkit.BUTTON_LEFT, 1)
    shown = wait(lambda: any(isinstance(p, dict) and p.get("width", 0) > 0 for p in popups), 10)
    view.send_key(zebkit.KEY_DOWN, key_code=27, code="Escape", key="Escape")
    view.send_key(zebkit.KEY_UP, key_code=27, code="Escape", key="Escape")
    check(shown and wait(lambda: popups and popups[-1] is None, 10), "on_popup(bounds), None once gone", popups[-3:])
    view.on_popup(None)
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


AUTOMATION = b"""<title>Automation parity</title>
<iframe id=inner srcdoc="<button id=deep onclick=&quot;this.textContent='pressed'&quot;>Deep</button>"></iframe>
<p id=out>none</p>"""


def check_automation(base):
    """What Rust's automation has that Python got for parity: Locator.content_frame,
    BrowserContext.id, call(), on_event(), the command() methods,
    on_protocol_event(), a goto()'s Response and its body, and the requests that
    requestfinished names."""
    from zebkit.automation import Page, call, new_context, on_event, run
    results = {}

    async def test():
        page = Page(width=400, height=300)
        events, finished = [], []
        page.on_protocol_event(lambda name, params: events.append(name))
        page.on("requestfinished", finished.append)
        response = await page.goto(base + "/automation")
        results["response"] = (response.status, (await response.text())[:40])
        await page.locator("#inner").content_frame.locator("#deep").click()
        results["content_frame"] = await page.frame_locator("#inner").locator("#deep").text_content()
        results["command"] = (await page.command("Frame.title")).get("value")
        results["locator_command"] = (await page.locator("#out").command("Frame.textContent")).get("value")
        results["call"] = (await call(page.view, "Frame.evaluateExpression", {"expression": "1 + 2"})).get("value")
        results["events"] = "Page.request" in events
        results["finished"] = [r.url for r in finished]
        context = await new_context()
        results["context"] = context.id
        results["context_command"] = isinstance(await context.command("BrowserContext.cookies"), dict)
        await context.close()
        # (Before on_event(), which takes the page's events.)
        # The only dialog listener gone (wait_for_event()'s after its event,
        # one cancelled): a dialog no longer holds the page.
        waiting = page.wait_for_event("dialog")
        await page.evaluate("() => { setTimeout(() => alert('one'), 0); }")
        dialog = await waiting
        await dialog.accept()
        cancelled = page.wait_for_event("filechooser")
        results["cancel"] = cancelled.cancel() and not page._listeners.get("filechooser")
        try:
            await cancelled
        except zebkit.Error:
            results["cancel"] = results["cancel"] and True
        results["after"] = await page.evaluate("() => { alert('two'); return 'after ' + confirm('three'); }")
        await page.evaluate("() => { const i = document.createElement('input'); i.type = 'file'; i.id = 'f';"
                            " window.__closed = null; i.oncancel = () => window.__closed = 'cancel';"
                            " i.onchange = () => window.__closed = 'change'; document.body.append(i); }")
        await page.locator("#f").click()
        for _ in range(100):
            results["chooser"] = await page.evaluate("() => window.__closed")
            if results["chooser"]:
                break
            await page.wait_for_timeout(30)
        seen = []
        on_event(page.view, lambda name, params: seen.append(name))
        await call(page.view, "Frame.evaluateExpression", {"expression": "location.reload()"})
        for _ in range(100):
            if seen:
                break
            await page.wait_for_timeout(20)
        results["on_event"] = bool(seen)
        on_event(page.view, None)
        page.view.close()

    future = run(test())
    wait(future.done, 60)
    if future.done() and future._error is not None:
        check(False, "automation parity ran", repr(future._error))
    check(results.get("after") == "after false", "a dialog after the last listener went is dismissed",
          results.get("after"))
    check(results.get("cancel") is True, "wait_for_event(): Future.cancel() removes its listener",
          results.get("cancel"))
    check(bool(results.get("chooser")), "a file chooser after its listener was cancelled is not held",
          results.get("chooser"))
    check(results.get("response") == (200, "<title>Automation parity</title>\n<iframe"),
          "goto()'s Response and its body", results.get("response"))
    check(results.get("content_frame") == "pressed", "Locator.content_frame", results.get("content_frame"))
    check(results.get("command") == "Automation parity" and results.get("locator_command") == "none",
          "Page.command() and Locator.command()", (results.get("command"), results.get("locator_command")))
    check(results.get("call") == 3 and results.get("events"), "call() and on_protocol_event()",
          (results.get("call"), results.get("events")))
    check(any(url.endswith("/automation") for url in results.get("finished", [])),
          "requestfinished names the whole request", results.get("finished"))
    check(bool(results.get("context")) and results.get("context_command"), "BrowserContext.id and command()",
          results.get("context"))
    check(results.get("on_event"), "on_event()")


def check_new_api(url, base):
    """What came with the C API's new functions: bind_call() and Call (answered
    later, from another thread, once; dropped unanswered, zebkit_call_free), the
    _async forms of cdp_listen, open_external and show_in_folder, the off-screen
    constructor's error, on_process_gone, on_download_result,
    on_responsiveness, export_native_window's new callback, a refused user
    agent override, and Page.add_init_script()'s Disposable."""
    view = zebkit.View(url, 320, 240)
    loaded(view, url)
    view.grant("*", zebkit.ALLOW_APP)
    kept = []

    def later(call):
        kept.append(call)

    view.bind_call("later", later)
    view.bind_call("dropped", lambda call: None)
    view.bind_call("twice", lambda call: (call.resolve(call.args[0] * 2), kept.append(call)))
    view.bind_call("raises", lambda call: 1 / 0)
    view.reload()
    loaded(view, url)
    ev(view, "window.answer = zebkit.app.later(20, 1).then(v => 'later ' + v, e => 'rejected'); 1")
    check(wait(lambda: kept), "bind_call(): the function gets the call")
    call = kept.pop()
    check(call.args == [20, 1] and call.origin.startswith("http://127.0.0.1"), "Call.args and Call.origin",
          (call.args, call.origin))
    threading.Thread(target=lambda: call.resolve(call.args[0] + call.args[1])).start()
    check(ev(view, "window.answer") == "later 21", "a Call answered later, from another thread")
    try:
        call.reject("again")
        again = None
    except zebkit.Error as error:
        again = str(error)
    check(again is not None and call.answered, "a second answer raises, and frees nothing", again)
    answer = ev(view, "zebkit.app.dropped().then(() => 'resolved', e => 'rejected: ' + e.message)")
    check(answer == "rejected: the application dropped the call", "a Call let go unanswered: zebkit_call_free",
          answer)
    answer = ev(view, "zebkit.app.twice(4)")
    check(answer == 8 and kept and kept[-1].answered, "resolve() in the function, the call kept", answer)
    del kept[:]
    answer = ev(view, "zebkit.app.raises().then(() => 'resolved', e => 'rejected')")
    check(answer == "rejected", "an exception before the answer rejects", answer)

    # The _async forms: told later, on the engine's thread.
    told = {}
    zebkit.cdp_listen_async(None, 0, lambda port, ws, error: told.update(cdp=(port, ws, error)))
    check("cdp" not in told, "cdp_listen_async(): told after it returns")
    check(wait(lambda: "cdp" in told, 15), "cdp_listen_async(): told")
    port, ws, error = told.get("cdp", (0, None, "none"))
    check(port > 0 and ws and ws.startswith("ws://") and error is None and zebkit.cdp_port() == port,
          "cdp_listen_async(): the port and the browser's address", told.get("cdp"))
    zebkit.cdp_listen_async(None, port + 1, lambda port, ws, error: told.update(other=(port, ws, error)))
    check(wait(lambda: "other" in told) and told["other"][0] == 0 and told["other"][2],
          "cdp_listen_async(): another port while it listens is an error", told.get("other"))
    zebkit.cdp_stop()
    zebkit.open_external_async("javascript:alert(1)", lambda done, error: told.update(external=(done, error)))
    check(wait(lambda: "external" in told) and told["external"][0] is False and told["external"][1],
          "open_external_async(): a javascript: URL refused", told.get("external"))
    missing = os.path.join(tempfile.gettempdir(), "zebkit-parity-not-there", "nothing.txt")
    zebkit.show_in_folder_async(missing, lambda done, error: told.update(folder=(done, error)))
    check(wait(lambda: "folder" in told, 15) and told["folder"][0] is False and told["folder"][1],
          "show_in_folder_async(): a file that is not there", told.get("folder"))

    # The off-screen constructor's error is the engine's.
    context = zebkit.Context()
    context.close()
    try:
        zebkit.View.offscreen(url, 10, 10, 1.0, context=context)
        refused = None
    except zebkit.Error as error:
        refused = str(error)
    check(bool(refused), "View.offscreen(): the engine's error", refused)

    # export_native_window: (view, handle, data) now; an off-screen view has none.
    offscreen = zebkit.View.offscreen(url, 40, 40, 1.0)
    handles = []
    offscreen.export_native_window(handles.append)
    check(wait(lambda: handles) and handles == [None], "export_native_window(): an off-screen view has none",
          handles)
    offscreen.close()

    # The user agent override's rule, the engine's and every binding's:
    # invalid options are taken (True) and told once, after the call returns;
    # False, never called back, only for a closed view.
    answers = []
    queued = view.set_user_agent_override({"userAgent": 42}, lambda value, error: answers.append(error))
    nul = view.set_user_agent_override('{"userAgent":"a\0b"}', lambda value, error: answers.append(error))
    told_at_once = list(answers)
    not_json = view.set_user_agent_override({"userAgent": object()}, lambda value, error: answers.append(error))
    wait(lambda: len(answers) >= 3, 5)
    end = time.monotonic() + .3
    while time.monotonic() < end:
        zebkit.pump()
        time.sleep(.005)
    check(queued and nul and not_json and told_at_once == [] and len(answers) == 3 and all(answers),
          "set_user_agent_override(): invalid options taken, each told once after it returns",
          (queued, nul, not_json, told_at_once, answers))
    closed = zebkit.View(None, 20, 20)
    closed.close()
    wait(lambda: closed._closed, 5)
    late = []
    refused = closed.set_user_agent_override({"userAgent": "x"}, lambda value, error: late.append(error))
    wait(lambda: late, .3)
    check(refused is False and late == [], "set_user_agent_override(): a closed view is False, never told",
          (refused, late))

    # Call.close(): drops the call now, as Rust's drop(call) and Go's Close().
    view.bind_call("closed", lambda call: (kept.append(call), call.close()))
    answer = ev(view, "zebkit.app.closed().then(() => 'resolved', e => 'rejected: ' + e.message)")
    check(answer == "rejected: the application dropped the call" and kept and kept[-1].answered,
          "Call.close() drops it unanswered", answer)
    kept[-1].close()
    try:
        kept[-1].resolve(1)
        again = None
    except zebkit.Error as error:
        again = str(error)
    check(again is not None, "an answer after Call.close() raises; a second close() does nothing", again)
    del kept[:]

    # find(): a NUL or text that is not UTF-8 is refused, as in Go and Rust;
    # the result is a FindResult.
    refusals = []
    for text in ("app\0le", "\ud83d"):
        try:
            view.find(text)
        except zebkit.Error as error:
            refusals.append(str(error))
    check(len(refusals) == 2, "find(): a NUL or a lone surrogate is an Error", refusals)
    results = []
    view.on_find(results.append)
    ev(view, "document.body.innerHTML = '<p>pear</p><p>pear</p>'; 1")
    request = view.find("pear")
    wait(lambda: any(r.request_id == request and r.final_update for r in results), 10)
    last = view.find_result()
    check(isinstance(last, zebkit.FindResult) and last.request_id == request and last.matches == 2
          and last.active_match == 1 and last["matches"] == 2 and last.text == "pear",
          "find_result() and on_find(): a FindResult", last)
    view.on_find(None)
    view.stop_finding()

    # The page's process gone: told, the view stays and loads again.
    gone = []
    view.on_process_gone(gone.append)
    view.on_responsiveness(lambda event: None)
    view.on_download_result(lambda event: None)
    view.load("chrome://crash")
    check(wait(lambda: gone, 20) and gone[0].get("reason") == "crashed" and "exitCode" in gone[0],
          "on_process_gone(): crashed", gone)
    view.load(url)
    check(loaded(view, url), "the view loads again after its process went")
    view.on_process_gone(None)
    view.on_responsiveness(None)
    view.on_download_result(None)

    # Page.add_init_script(): a Disposable that takes it away.
    from zebkit.automation import Page, run
    results = {}

    async def test():
        page = Page(width=200, height=100)
        disposable = await page.add_init_script("window.initialized = (window.initialized || 0) + 1")
        await page.goto(base + "/automation")
        results["with"] = await page.evaluate("window.initialized")
        await disposable.dispose()
        await page.reload()
        results["without"] = await page.evaluate("window.initialized === undefined")
        page.view.close()

    future = run(test())
    wait(future.done, 60)
    check(results == {"with": 1, "without": True}, "add_init_script(): a Disposable whose dispose() removes it",
          (results, future._error if future.done() else "not done"))
    view.close()


def check_library(base):
    """zebkit.library (<zebkit/library.h>): what Rust, Go and host JavaScript
    check too, through Futures, a coroutine, function= and another thread;
    tests/engine/library.py has the C API's every case."""
    import zebkit.library as L
    fixture = HERE / "library_fixture"

    def done(*futures, seconds=20):
        return wait(lambda: all(f.done() for f in futures), seconds)

    def code(future):
        if not done(future):
            return "pending"
        try:
            future.result()
            return "OK"
        except L.LibraryError as error:
            return error.code_name

    try:
        L.load(HERE / "no-such-library")
        check(False, "library: a bad path raises")
    except zebkit.Error:
        check(True, "library: a bad path raises")
    lib = L.load(fixture)
    check(done(lib.ready) and "echo" in lib.ready.result()["functions"], "library: ready, the exported functions")
    value = [{"a": [1, None, True], "b": b"\x01\x02\xff"}, "日本語 🦓"]
    echoed = lib.call("echo", *value)
    check(done(echoed) and echoed.result() == value, "library: JSON, bytes in place and non-ASCII text echoed",
          echoed.done() and echoed.result())
    got = lib.call("bytes", 300)
    check(done(got) and got.result()["b"] == bytes(i & 255 for i in range(300)), "library: a Uint8Array returned is bytes")

    async def coroutine():
        return await lib.call("double", 21)
    ran = zebkit.automation.run(coroutine())
    check(done(ran) and ran.result() == 42, "library: a call awaited in a coroutine")
    thrown = lib.call("thrower")
    check(code(thrown) == "THROWN" and thrown._error.name == "RangeError" and "thrower" in thrown._error.stack,
          "library: THROWN, the library's name and stack")
    for function, expect in (("rejecter", "THROWN"), ("missing", "NO_FUNCTION"), ("badResult", "BAD_RESULT")):
        check(code(lib.call(function)) == expect, "library: %s is %s" % (function, expect))
    first, queued = lib.call("later", 1, 200), lib.call("double", 1)
    lib.cancel(queued)
    check(code(queued) == "CANCELLED" and code(first) == "OK", "library: cancel of a waiting call")
    slow, nxt = lib.call("later", 1, 2000, timeout_ms=100), lib.call("double", 2)
    check(code(slow) == "TIMEOUT" and code(nxt) == "OK" and nxt.result() == 4,
          "library: a waiting call past its deadline is TIMEOUT, the next runs")
    check(code(lib.call("get", base + "/")) == "THROWN", "library: fetch() refused without a grant")
    net = L.load(fixture, {"network": [base]})
    fetched = net.call("get", base + "/")
    check(code(fetched) == "OK" and fetched.result()["status"] == 200, "library: fetch() to a granted origin")
    told = []
    threading.Thread(target=lambda: lib.call("double", 5, function=lambda v, e: told.append((v, e)))).start()
    check(wait(lambda: told) and told[0] == (10, None), "library: a call from another thread, function=")
    small = L.load(fixture, {"queue": 1})
    done(small.ready)
    running = small.call("later", 0, 300)
    wait(lambda: False, 0.1)
    waiting, full = small.call("double", 1), small.call("double", 1)
    check(code(full) == "QUEUE_FULL" and code(running) == "OK" and code(waiting) == "OK", "library: QUEUE_FULL")
    doomed = L.load(fixture)
    spinning, after = doomed.call("spin", timeout_ms=200), doomed.call("double", 1)
    check(code(spinning) == "TIMEOUT" and code(after) == "GONE",
          "library: a running script past its deadline is TIMEOUT, then GONE")
    hungry = L.load(fixture, {"heapLimitMB": 32})
    check(code(hungry.call("grow", timeout_ms=-1)) == "OUT_OF_MEMORY", "library: heap exhaustion is OUT_OF_MEMORY")
    forbidden = L.load(HERE / "library_forbidden")
    check(code(forbidden.ready) == "LOAD_FAILED", "library: an import outside the folder fails the load")
    closing = L.load(fixture)
    done(closing.ready)
    pending = [closing.call("later", 1, 1000), closing.call("double", 1)]
    closing.close()
    check([code(f) for f in pending] == ["GONE", "GONE"], "library: close with calls pending: GONE")
    for library in (lib, net, small, doomed, hungry, forbidden):
        library.close()


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
        check_automation(base)
        check_new_api(url, base)
        check_library(base)
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
