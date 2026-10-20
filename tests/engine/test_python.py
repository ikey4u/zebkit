"""zebkit for Python (bindings/python): the services, then the engine with
a view, script and a function bound for the page. Run with the engine's
switches, as scripts/engine-test.sh does:

    ZEBKIT_DIR=<package> PYTHONPATH=bindings/python python3 tests/engine/test_python.py --no-sandbox --headless
"""

import sys

import zebkit

failures = []


def check(ok, what, detail=None):
    print(f"{'ok  ' if ok else 'FAIL'} {what}" + (f": {detail}" if detail is not None else ""), flush=True)
    if not ok:
        failures.append(what)


text = b"zebkit for Python " * 50
for name in ("gzip", "zlib", "deflate-raw", "brotli", "zstd"):
    packed = zebkit.compress(text, name)
    check(zebkit.decompress(packed, name) == text and len(packed) < len(text) // 5, f"compress, {name}")
try:
    zebkit.decompress(b"nonsense", "gzip")
    check(False, "decompress refuses what is not gzip")
except zebkit.Error as e:
    check(True, "decompress refuses what is not gzip", e)
check(zebkit.digest(b"abc").hex().startswith("ba7816bf8f01cfea"), "digest sha256")
check(zebkit.hmac(b"Jefe", b"what do ya want for nothing?").hex().startswith("5bdcc146"), "hmac sha256")
key, nonce = zebkit.random_bytes(32), zebkit.random_bytes(12)
sealed = zebkit.aes_gcm_seal(key, nonce, b"secret", b"ad")
check(zebkit.aes_gcm_open(key, nonce, sealed, b"ad") == b"secret", "AES-GCM seal and open")
check(zebkit.url_part("../c", "https://example.com/a/b/") == "https://example.com/a/c", "url_part, resolved")
check(zebkit.registrable_domain("x.y.example.co.uk") == "example.co.uk", "registrable_domain")
pixels = bytes([10, 20, 30, 255]) * 6
width, height, back = zebkit.image_decode(zebkit.image_encode(pixels, 3, 2, "png"))
check((width, height, back) == (3, 2, pixels), "image encode and decode, PNG")

PDF = b"""%PDF-1.7
1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj
2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj
3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >> endobj
4 0 obj << >> stream
BT /F1 18 Tf 10 40 Td (Hello Python) Tj ET
endstream endobj
5 0 obj << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> endobj
trailer << /Root 1 0 R >>
%%EOF
"""
with zebkit.Pdf(PDF) as pdf:
    width, height, rgba = pdf.render(0, scale=2)
    found = pdf.search("python")
    added = pdf.add_annotation(0, {"type": "ink", "rect": [5, 5, 95, 95], "border": 2, "ink": [[[10, 10], [90, 90]]]})
    appearance = pdf.render_annotation(0, added, scale=2)
    check(appearance is not None and appearance[0] > 100 and pdf.render_annotation(0, added, mode=2) is None,
          "Pdf: an annotation's appearance alone", appearance and appearance[:2])
    pdf.set_metadata(title="from Python")
    saved = pdf.save()
    check(pdf.page_count == 1 and (width, height, len(rgba)) == (400, 200, 400 * 200 * 4) and len(found) == 1
          and added == 0, "Pdf: render, search, annotate", found)
with zebkit.Pdf(saved) as again:
    check(again.info()["metadata"]["title"] == "from Python" and again.text(0) == "Hello Python"
          and again.annotations(0)[0]["type"] == "ink", "Pdf: saved and opened again")
try:
    zebkit.Pdf(b"not a pdf")
    check(False, "Pdf refuses what is not a PDF")
except zebkit.Error as e:
    check(True, "Pdf refuses what is not a PDF", e)

check(zebkit.scheme_register("py"), "scheme_register")
state = {}


def serve(request):
    if request.url == "py://test/index.html":
        request.respond(200, "<title>scheme</title><script>fetch('/data').then(r => r.text())"
                        ".then(t => document.title = t)</script>", {"Content-Type": "text/html"})
    else:
        request.respond(200, f"{request.method} from Python", {"Content-Type": "text/plain"})


def scheme_view():
    zebkit.scheme_handle("py", serve)
    view = zebkit.View("py://test/index.html", 320, 240)

    def titled(title):
        if title == "GET from Python":
            check(True, "a page from the application's scheme, and its fetch")
            view.close()
    view.on_title(titled)
    view.on_close(zebkit.quit)


def ready():
    check(zebkit.engine_version().startswith("Chromium "), "engine_version", zebkit.engine_version())
    view = zebkit.View("data:text/html,<title>python</title>", 640, 480)
    view.grant("*", zebkit.ALLOW_APP | zebkit.ALLOW_CRYPTO)
    view.bind("twice", lambda x, origin: x * 2)
    view.bind("broken", lambda origin: 1 / 0)

    def after_crypto(value, error):
        check(value == 32, "zebkit.crypto from the page", error)
        view.close()

    def after_broken(value, error):
        check(value is not None and "division" in value, "a Python exception rejects the page's promise", value)
        view.evaluate("zebkit.crypto.randomBytes(32).then(b => b.length)", after_crypto)

    def after_twice(value, error):
        check(value == 42, "zebkit.app.twice(21) from the page", error)
        view.evaluate("zebkit.app.broken().then(() => 'resolved', e => e.message)", after_broken)

    def loaded(event, url):
        if event == zebkit.LOAD_FINISHED and not state.get("loaded"):
            state["loaded"] = True
            check(view.title == "python", "the page's title", view.title)
            view.evaluate("zebkit.app.twice(21)", after_twice)

    view.on_load(loaded)
    view.on_close(lambda: (check(True, "on_close"), scheme_view()))


code = zebkit.main(ready)
check(code == 0, "main returned 0", code)
print(f"{'all' if not failures else len(failures)} {'passed' if not failures else 'failed'}")
sys.exit(1 if failures else 0)
