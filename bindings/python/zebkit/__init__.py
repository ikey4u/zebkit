"""zebkit for Python: the engine, and what is inside it, through ctypes.

    import zebkit

    def ready():
        view = zebkit.View("https://example.com/")
        view.on_load(lambda event, url: event == zebkit.LOAD_FINISHED and
                     view.evaluate("document.title", lambda title, error: print(title)))

    zebkit.main(ready)

The services work without the engine running:

    packed = zebkit.compress(b"..." * 100, "zstd")
    zebkit.digest(b"abc", "sha256").hex()
    zebkit.url_part("../b", "https://example.com/a/", "spec")

The library is libzebkit.so.1 (Zebkit.framework on macOS) in $ZEBKIT_DIR,
or beside this package, or in the zebkit repository's package of the slim
profile (slim-macos on macOS).

Threads: everything is for the thread that called main(), and callbacks
run on it; post() runs a function there from any thread.
"""

import ctypes
import json
import os
import pathlib
import sys

__all__ = [
    "View", "main", "quit", "post", "version", "engine_version", "is_headless",
    "cdp_listen", "cdp_port", "cdp_url", "cdp_stop", "fetch",
    "compress", "decompress", "digest", "hmac", "random_bytes", "aes_gcm_seal", "aes_gcm_open",
    "url_part", "registrable_domain", "image_decode", "image_encode", "icu_init", "Error",
    "LOAD_STARTED", "LOAD_COMMITTED", "LOAD_FINISHED", "LOAD_FAILED",
    "ALLOW_APP", "ALLOW_FETCH", "ALLOW_FILES", "ALLOW_COMPRESS", "ALLOW_CRYPTO", "ALLOW_IMAGE",
    "ALLOW_URL", "ALLOW_PDF", "ALLOW_ALL",
    "Pdf", "RENDER_ANNOTATIONS", "RENDER_FORMS", "RENDER_LCD_TEXT", "RENDER_GRAYSCALE", "RENDER_PRINTING",
    "RENDER_NO_SMOOTHING", "Request", "scheme_register", "scheme_handle",
    "SCHEME_STANDARD", "SCHEME_SECURE", "SCHEME_FETCH", "SCHEME_BYPASS_CSP",
    "app_set_menu", "app_on_open_files", "app_on_reopen", "app_on_quit_request",
]

LOAD_STARTED, LOAD_COMMITTED, LOAD_FINISHED, LOAD_FAILED = 0, 1, 2, 3
ALLOW_APP, ALLOW_FETCH, ALLOW_FILES, ALLOW_COMPRESS = 1, 2, 4, 8
ALLOW_CRYPTO, ALLOW_IMAGE, ALLOW_URL, ALLOW_PDF, ALLOW_ALL = 16, 32, 64, 128, 0xFF

_COMPRESSION = {"gzip": 1, "zlib": 2, "deflate": 2, "deflate-raw": 3, "brotli": 4, "br": 4, "zstd": 5}
_DIGEST = {"sha1": 1, "sha256": 2, "sha384": 3, "sha512": 4}
_IMAGE = {"png": 1, "jpeg": 2, "webp": 3}
_URL_PART = {"spec": 0, "scheme": 1, "username": 2, "password": 3, "host": 4, "port": 5, "path": 6,
             "query": 7, "fragment": 8, "origin": 9}


class Error(Exception):
    """What the engine reports when something it was asked to do fails."""


def _find_library():
    if sys.platform == "darwin":
        library, profile = "Zebkit.framework/Zebkit", "slim-macos"
    else:
        library, profile = "libzebkit.so.1", "slim"
    names = []
    if os.environ.get("ZEBKIT_DIR"):
        names.append(pathlib.Path(os.environ["ZEBKIT_DIR"]) / library)
    here = pathlib.Path(__file__).resolve().parent
    names.append(here / library)
    if sys.platform == "darwin":
        names.append(here.parents[2] / "dist" / "release" / library)
        names.append(here.parents[2] / "out" / "blink" / "slim-macos-native" / "package" / library)
    names.append(here.parents[2] / "out" / "blink" / profile / "package" / library)
    for name in names:
        if name.exists():
            return str(name)
    raise ImportError(f"{library} not found; set ZEBKIT_DIR to the engine's directory")


_lib = ctypes.CDLL(_find_library())

_c_char_p_p = ctypes.POINTER(ctypes.c_char_p)
_TASK = ctypes.CFUNCTYPE(None, ctypes.c_void_p)
_LOAD = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_void_p)
_TITLE = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p)
_VIEW = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p)
_RESULT = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p)
_BIND = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p)


class _Response(ctypes.Structure):
    _fields_ = [("status", ctypes.c_int), ("url", ctypes.c_char_p), ("headers", ctypes.c_char_p),
                ("body", ctypes.POINTER(ctypes.c_uint8)), ("body_size", ctypes.c_size_t),
                ("error", ctypes.c_char_p)]


_RESPONSE = ctypes.CFUNCTYPE(None, ctypes.POINTER(_Response), ctypes.c_void_p)


def _declare(name, result, *args):
    function = getattr(_lib, name)
    function.restype = result
    function.argtypes = list(args)
    return function


_V, _P, _S, _I, _U, _Z, _B = ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_uint, \
    ctypes.c_size_t, ctypes.c_bool
_ZP = ctypes.POINTER(ctypes.c_size_t)
_IP = ctypes.POINTER(ctypes.c_int)
_EP = ctypes.POINTER(ctypes.c_void_p)

_version = _declare("zebkit_version", _S)
_engine_version = _declare("zebkit_engine_version", _S)
_free = _declare("zebkit_free", None, _P)
_main = _declare("zebkit_main", _I, _I, ctypes.POINTER(ctypes.c_char_p), _TASK, _P)
_quit = _declare("zebkit_quit", None)
_post = _declare("zebkit_post_task", None, _TASK, _P)
_post_delayed = _declare("zebkit_post_delayed_task", None, _TASK, _P, ctypes.c_uint32)
_is_headless = _declare("zebkit_is_headless", _B)
_view_new = _declare("zebkit_view_new", _V, _S, _I, _I)
_view_load_url = _declare("zebkit_view_load_url", None, _V, _S)
_view_reload = _declare("zebkit_view_reload", None, _V)
_view_go_back = _declare("zebkit_view_go_back", None, _V)
_view_go_forward = _declare("zebkit_view_go_forward", None, _V)
_view_resize = _declare("zebkit_view_resize", None, _V, _I, _I)
_AUTHENTICATION = ctypes.CFUNCTYPE(None, _V, _S, _P)
_view_on_authentication = _declare("zebkit_view_on_authentication", _B, _V, _AUTHENTICATION, _P)
_view_authentication_action = _declare("zebkit_view_authentication_action", _B, _V, _S)
_view_authentication_ui = _declare("zebkit_view_set_authentication_ui", _B, _V, _B)
_view_close = _declare("zebkit_view_close", None, _V)
_view_get_url = _declare("zebkit_view_get_url", _P, _V)
_view_get_title = _declare("zebkit_view_get_title", _P, _V)
_view_on_load = _declare("zebkit_view_on_load", None, _V, _LOAD, _P)
_view_on_title = _declare("zebkit_view_on_title", None, _V, _TITLE, _P)
_view_on_close = _declare("zebkit_view_on_close", None, _V, _VIEW, _P)
_view_evaluate = _declare("zebkit_view_evaluate", None, _V, _S, _RESULT, _P)
_view_grant = _declare("zebkit_view_grant", None, _V, _S, _U, _S)
_view_bind = _declare("zebkit_view_bind", None, _V, _S, _BIND, _P)
_call_resolve = _declare("zebkit_call_resolve", None, _P, _S)
_call_reject = _declare("zebkit_call_reject", None, _P, _S)
_cdp_listen = _declare("zebkit_cdp_listen", _U, _S, _U, _EP)
_cdp_get_port = _declare("zebkit_cdp_get_port", _U)
_cdp_get_url = _declare("zebkit_cdp_get_url", _P)
_cdp_stop = _declare("zebkit_cdp_stop", None)
_compress = _declare("zebkit_compress", _P, _I, _P, _Z, _I, _ZP, _EP)
_decompress = _declare("zebkit_decompress", _P, _I, _P, _Z, _Z, _ZP, _EP)
_digest_size = _declare("zebkit_digest_size", _Z, _I)
_digest_compute = _declare("zebkit_digest_compute", _B, _I, _P, _Z, _P)
_hmac = _declare("zebkit_hmac", _B, _I, _P, _Z, _P, _Z, _P)
_random_bytes = _declare("zebkit_random_bytes", _B, _P, _Z)
_aes_gcm_seal = _declare("zebkit_aes_gcm_seal", _P, _P, _Z, _P, _Z, _P, _Z, _P, _Z, _ZP, _EP)
_aes_gcm_open = _declare("zebkit_aes_gcm_open", _P, _P, _Z, _P, _Z, _P, _Z, _P, _Z, _ZP, _EP)
_url_get = _declare("zebkit_url_get", _P, _S, _S, _I)
_url_registrable_domain = _declare("zebkit_url_registrable_domain", _P, _S)
_image_decode = _declare("zebkit_image_decode", _P, _P, _Z, _IP, _IP, _EP)
_image_encode = _declare("zebkit_image_encode", _P, _I, _P, _I, _I, _I, _ZP, _EP)
_icu_init = _declare("zebkit_icu_init", _B)
_fetch = _declare("zebkit_fetch", None, _S, _S, _S, _P, _Z, _RESPONSE, _P)

# Callbacks and views that C still holds, by id: kept here so that Python
# does not free them. ctypes' function objects cannot be hashed.
class _Alive(dict):
    def add(self, item):
        self[id(item)] = item

    def discard(self, item):
        self.pop(id(item), None)


_alive = _Alive()


def _bytes(data):
    if isinstance(data, str):
        data = data.encode()
    return bytes(data)


def _string(pointer):
    """A string the engine returned, freed."""
    if not pointer:
        return None
    try:
        return ctypes.string_at(pointer).decode()
    finally:
        _free(pointer)


def _buffer(pointer, size):
    """A buffer the engine returned, freed."""
    try:
        return ctypes.string_at(pointer, size)
    finally:
        _free(pointer)


def _raise(error, fallback):
    message = _string(error.value) if error.value else fallback
    raise Error(message)


def _guard(function):
    """Exceptions do not cross into C: report them and carry on."""
    def call(*args):
        try:
            return function(*args)
        except Exception:  # noqa: BLE001
            sys.excepthook(*sys.exc_info())
    return call


# ---- the engine ----

def version():
    return _version().decode()


def engine_version():
    return _engine_version().decode()


def is_headless():
    return bool(_is_headless())


def main(ready, argv=None):
    """Start the engine, call ready() once views can be made, run until
    quit(); returns the exit code. Chromium's switches in argv (by default
    sys.argv) are honoured, such as --remote-debugging-port=9222."""
    argv = list(sys.argv if argv is None else argv) or ["python"]
    array = (ctypes.c_char_p * (len(argv) + 1))(*[a.encode() for a in argv], None)
    callback = _TASK(_guard(lambda data: ready()))
    _alive.add(callback)
    return _main(len(argv), array, callback, None)


def quit():  # noqa: A001
    _quit()


def post(function, delay_ms=0):
    """Run function() on the engine's thread; any thread may call this."""
    def run(data):
        _alive.discard(callback)
        function()
    callback = _TASK(_guard(run))
    _alive.add(callback)
    if delay_ms:
        _post_delayed(callback, None, int(delay_ms))
    else:
        _post(callback, None)


class View:
    """A page, in a window when the machine has a display."""

    def __init__(self, url=None, width=0, height=0):
        self._handle = _view_new(url.encode() if url else None, width, height)
        if not self._handle:
            raise Error("cannot create a view")
        self._callbacks = {}
        self._on_close_user = None
        self._closed = False
        self._set("close", _VIEW(_guard(self._closed_by_engine)), _view_on_close)
        _alive.add(self)

    def _set(self, kind, callback, setter):
        self._callbacks[kind] = callback
        setter(self._handle, callback, None)

    def _closed_by_engine(self, handle, data):
        self._closed = True
        _alive.discard(self)
        if self._on_close_user:
            self._on_close_user()

    def _check(self):
        if self._closed:
            raise Error("the view is closed")
        return self._handle

    def load(self, url):
        _view_load_url(self._check(), url.encode())

    def reload(self):
        _view_reload(self._check())

    def back(self):
        _view_go_back(self._check())

    def forward(self):
        _view_go_forward(self._check())

    def resize(self, width, height):
        _view_resize(self._check(), width, height)

    def on_authentication(self, function):
        """Receive host-owned Passkey events as dictionaries; None restores native UI.

        Never log/persist these events or PIN actions. Off-screen hosts must
        opt into authentication UI. See docs/PASSKEYS.md for the schema.
        """
        handle = self._check()
        _view_on_authentication(handle, _AUTHENTICATION(), None)
        previous = self._callbacks.pop("authentication", None)
        if previous is not None:
            # A handler may unregister itself; keep the ctypes trampoline
            # alive until its native invocation has returned.
            post(lambda previous=previous: None)
        if function is None:
            return True
        callback = _AUTHENTICATION(_guard(lambda h, event, data: function(json.loads(event))))
        self._callbacks["authentication"] = callback
        return bool(_view_on_authentication(handle, callback, None))

    def authentication_action(self, action):
        """Queue a current request/revision action; True is not authentication success."""
        return bool(_view_authentication_action(self._check(), json.dumps(action).encode()))

    def set_authentication_ui(self, enabled):
        """Allow Passkey UI for this view."""
        return bool(_view_authentication_ui(self._check(), bool(enabled)))

    def close(self):
        if not self._closed:
            _view_close(self._handle)

    @property
    def url(self):
        return _string(_view_get_url(self._check()))

    @property
    def title(self):
        return _string(_view_get_title(self._check()))

    def on_load(self, function):
        """function(event, url), event one of LOAD_STARTED, LOAD_COMMITTED,
        LOAD_FINISHED and LOAD_FAILED."""
        self._set("load", _LOAD(_guard(lambda h, e, u, d: function(e, u.decode()))), _view_on_load)

    def on_title(self, function):
        self._set("title", _TITLE(_guard(lambda h, t, d: function(t.decode()))), _view_on_title)

    def on_close(self, function):
        self._on_close_user = function

    def evaluate(self, script, function=None):
        """Run script in the page; function(value, error), value as from JSON."""
        def done(handle, result, error, data):
            _alive.discard(callback)
            if function:
                if error is not None:
                    function(None, error.decode())
                else:
                    function(json.loads(result.decode()) if result else None, None)
        callback = _RESULT(_guard(done))
        _alive.add(callback)
        _view_evaluate(self._check(), script.encode(), callback, None)

    def grant(self, origin, allow, files_root=None):
        """Give pages of origin the zebkit.* capabilities in allow (ALLOW_*)."""
        _view_grant(self._check(), origin.encode(), allow,
                    str(files_root).encode() if files_root else None)

    def bind(self, name, function):
        """Make zebkit.app.<name>(...args) in the page call
        function(*args, origin=...), whose return value (JSON-able) the
        page's promise resolves with; an exception rejects it."""
        def called(handle, origin, args, call, data):
            try:
                result = function(*json.loads(args.decode()), origin=origin.decode())
                _call_resolve(call, json.dumps(result).encode())
            except Exception as e:  # noqa: BLE001
                _call_reject(call, str(e).encode())
        if function is None:
            self._callbacks.pop("bind:" + name, None)
            _view_bind(self._check(), name.encode(), _BIND(), None)
            return
        callback = _BIND(called)
        self._callbacks["bind:" + name] = callback
        _view_bind(self._check(), name.encode(), callback, None)


def cdp_listen(address=None, port=0):
    error = ctypes.c_void_p()
    bound = _cdp_listen(address.encode() if address else None, port, ctypes.byref(error))
    if not bound:
        _raise(error, "cannot listen")
    return bound


def cdp_port():
    return _cdp_get_port()


def cdp_url():
    return _string(_cdp_get_url())


def cdp_stop():
    _cdp_stop()


def fetch(url, function, method="GET", headers=None, body=None):
    """Request url through the engine's network stack; function(response)
    gets a dict: status, url, headers (a dict), body (bytes), error."""
    def done(response, data):
        _alive.discard(callback)
        r = response.contents
        headers_out = {}
        for line in (r.headers or b"").decode().split("\r\n"):
            if ": " in line:
                name, value = line.split(": ", 1)
                headers_out[name] = value
        function({
            "status": r.status,
            "url": (r.url or b"").decode(),
            "headers": headers_out,
            "body": ctypes.string_at(r.body, r.body_size) if r.body_size else b"",
            "error": r.error.decode() if r.error else None,
        })
    callback = _RESPONSE(_guard(done))
    _alive.add(callback)
    lines = "\r\n".join(f"{k}: {v}" for k, v in (headers or {}).items())
    data = _bytes(body) if body is not None else b""
    _fetch(method.encode(), url.encode(), lines.encode() if lines else None, data or None, len(data), callback, None)


# ---- the services ----

def compress(data, format="gzip", level=-1):
    data = _bytes(data)
    size, error = ctypes.c_size_t(), ctypes.c_void_p()
    out = _compress(_COMPRESSION[format], data, len(data), level, ctypes.byref(size), ctypes.byref(error))
    if not out:
        _raise(error, "compression failed")
    return _buffer(out, size.value)


def decompress(data, format="gzip", max_size=0):
    data = _bytes(data)
    size, error = ctypes.c_size_t(), ctypes.c_void_p()
    out = _decompress(_COMPRESSION[format], data, len(data), max_size, ctypes.byref(size), ctypes.byref(error))
    if not out:
        _raise(error, "decompression failed")
    return _buffer(out, size.value)


def digest(data, algorithm="sha256"):
    data = _bytes(data)
    kind = _DIGEST[algorithm]
    out = ctypes.create_string_buffer(_digest_size(kind))
    if not _digest_compute(kind, data, len(data), out):
        raise Error("digest failed")
    return out.raw


def hmac(key, data, algorithm="sha256"):
    key, data = _bytes(key), _bytes(data)
    kind = _DIGEST[algorithm]
    out = ctypes.create_string_buffer(_digest_size(kind))
    if not _hmac(kind, key, len(key), data, len(data), out):
        raise Error("hmac failed")
    return out.raw


def random_bytes(count):
    out = ctypes.create_string_buffer(count)
    if not _random_bytes(out, count):
        raise Error("no random bytes")
    return out.raw


def aes_gcm_seal(key, nonce, plaintext, ad=b""):
    key, nonce, plaintext, ad = _bytes(key), _bytes(nonce), _bytes(plaintext), _bytes(ad)
    size, error = ctypes.c_size_t(), ctypes.c_void_p()
    out = _aes_gcm_seal(key, len(key), nonce, len(nonce), ad or None, len(ad), plaintext, len(plaintext),
                        ctypes.byref(size), ctypes.byref(error))
    if not out:
        _raise(error, "sealing failed")
    return _buffer(out, size.value)


def aes_gcm_open(key, nonce, sealed, ad=b""):
    key, nonce, sealed, ad = _bytes(key), _bytes(nonce), _bytes(sealed), _bytes(ad)
    size, error = ctypes.c_size_t(), ctypes.c_void_p()
    out = _aes_gcm_open(key, len(key), nonce, len(nonce), ad or None, len(ad), sealed, len(sealed),
                        ctypes.byref(size), ctypes.byref(error))
    if not out:
        _raise(error, "opening failed")
    return _buffer(out, size.value)


def url_part(url, base=None, part="spec"):
    """A part of url, resolved against base, as a browser parses it; None
    when it is not a valid URL."""
    return _string(_url_get(url.encode(), base.encode() if base else None, _URL_PART[part]))


def registrable_domain(host_or_url):
    return _string(_url_registrable_domain(host_or_url.encode()))


def image_decode(data):
    """(width, height, RGBA bytes) of a PNG, JPEG or WebP file."""
    data = _bytes(data)
    width, height, error = ctypes.c_int(), ctypes.c_int(), ctypes.c_void_p()
    out = _image_decode(data, len(data), ctypes.byref(width), ctypes.byref(height), ctypes.byref(error))
    if not out:
        _raise(error, "decoding failed")
    return width.value, height.value, _buffer(out, width.value * height.value * 4)


def image_encode(rgba, width, height, format="png", quality=-1):
    rgba = _bytes(rgba)
    if len(rgba) != width * height * 4:
        raise Error("the pixels must be width * height * 4 bytes of RGBA")
    size, error = ctypes.c_size_t(), ctypes.c_void_p()
    out = _image_encode(_IMAGE[format], rgba, width, height, quality, ctypes.byref(size), ctypes.byref(error))
    if not out:
        _raise(error, "encoding failed")
    return _buffer(out, size.value)


def icu_init():
    return bool(_icu_init())


# ---- PDF: <zebkit/pdf.h> ----

class _RenderOptions(ctypes.Structure):
    _fields_ = [("scale", ctypes.c_float), ("rotation", ctypes.c_int), ("flags", ctypes.c_uint),
                ("background", ctypes.c_uint32), ("x", ctypes.c_int), ("y", ctypes.c_int),
                ("width", ctypes.c_int), ("height", ctypes.c_int)]


_pdf_open = _declare("zebkit_pdf_open", _P, _P, _Z, _S, _EP)
_pdf_open_file = _declare("zebkit_pdf_open_file", _P, _S, _S, _EP)
_pdf_close = _declare("zebkit_pdf_close", None, _P)
_pdf_save = _declare("zebkit_pdf_save", _P, _P, _U, _ZP, _EP)
_pdf_info = _declare("zebkit_pdf_info", _P, _P)
_pdf_set_metadata = _declare("zebkit_pdf_set_metadata", _B, _P, _S, _EP)
_pdf_page_count = _declare("zebkit_pdf_page_count", _I, _P)
_pdf_page = _declare("zebkit_pdf_page", _P, _P, _I, _EP)
_pdf_render = _declare("zebkit_pdf_render", _P, _P, _I, ctypes.POINTER(_RenderOptions), _IP, _IP, _EP)
_pdf_render_annotation = _declare("zebkit_pdf_render_annotation", _P, _P, _I, _I, _I,
                                  ctypes.POINTER(_RenderOptions), _IP, _IP, _EP)
_pdf_text = _declare("zebkit_pdf_text", _P, _P, _I, _EP)
_pdf_text_layout = _declare("zebkit_pdf_text_layout", _P, _P, _I, _EP)
_pdf_text_rects = _declare("zebkit_pdf_text_rects", _P, _P, _I, _I, _I, _EP)
_pdf_char_at = _declare("zebkit_pdf_char_at", _I, _P, _I, ctypes.c_double, ctypes.c_double, ctypes.c_double)
_pdf_search = _declare("zebkit_pdf_search", _P, _P, _S, _U, _I, _I, _EP)
_pdf_outline = _declare("zebkit_pdf_outline", _P, _P, _EP)
_pdf_links = _declare("zebkit_pdf_links", _P, _P, _I, _EP)
_pdf_annotations = _declare("zebkit_pdf_annotations", _P, _P, _I, _EP)
_pdf_annotation_add = _declare("zebkit_pdf_annotation_add", _I, _P, _I, _S, _EP)
_pdf_annotation_update = _declare("zebkit_pdf_annotation_update", _B, _P, _I, _I, _S, _EP)
_pdf_annotation_remove = _declare("zebkit_pdf_annotation_remove", _B, _P, _I, _I, _EP)
_pdf_flatten = _declare("zebkit_pdf_flatten", _B, _P, _I, _B, _EP)
_pdf_attachments = _declare("zebkit_pdf_attachments", _P, _P, _EP)
_pdf_attachment = _declare("zebkit_pdf_attachment", _P, _P, _I, _ZP, _EP)
_pdf_attach = _declare("zebkit_pdf_attach", _B, _P, _S, _P, _Z, _EP)
_pdf_detach = _declare("zebkit_pdf_detach", _B, _P, _I, _EP)

RENDER_ANNOTATIONS, RENDER_FORMS, RENDER_LCD_TEXT, RENDER_GRAYSCALE, RENDER_PRINTING, RENDER_NO_SMOOTHING = \
    1, 2, 4, 8, 16, 32


class Pdf:
    """A PDF document on the engine's PDFium (<zebkit/pdf.h>), from bytes or
    a path. Structured answers are Python values; coordinates are PDF page
    space. Needs no main(); any thread may use it, one at a time."""

    def __init__(self, source, password=None):
        error = ctypes.c_void_p()
        password = password.encode() if password else None
        if isinstance(source, (bytes, bytearray, memoryview)):
            data = bytes(source)
            self._handle = _pdf_open(data, len(data), password, ctypes.byref(error))
        else:
            self._handle = _pdf_open_file(os.fspath(source).encode(), password, ctypes.byref(error))
        if not self._handle:
            _raise(error, "the document cannot be opened")

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def __del__(self):
        self.close()

    def close(self):
        if getattr(self, "_handle", None):
            _pdf_close(self._handle)
            self._handle = None

    def _json(self, function, *args, fallback="failed"):
        error = ctypes.c_void_p()
        out = function(self._handle, *args, ctypes.byref(error))
        if not out:
            _raise(error, fallback)
        return json.loads(_string(out))

    def _ok(self, function, *args, fallback="failed"):
        error = ctypes.c_void_p()
        if not function(self._handle, *args, ctypes.byref(error)):
            _raise(error, fallback)

    def info(self):
        return json.loads(_string(_pdf_info(self._handle)))

    def set_metadata(self, **entries):
        self._ok(_pdf_set_metadata, json.dumps(entries).encode())

    @property
    def page_count(self):
        return _pdf_page_count(self._handle)

    def page(self, index):
        return self._json(_pdf_page, index, fallback="no page")

    def render(self, index, scale=1.0, rotation=0, flags=RENDER_ANNOTATIONS, background=0xFFFFFFFF,
               x=0, y=0, width=0, height=0):
        """(width, height, RGBA bytes) of a page, or of a tile of it."""
        options = _RenderOptions(scale, rotation, flags, background, x, y, width, height)
        out_width, out_height, error = ctypes.c_int(), ctypes.c_int(), ctypes.c_void_p()
        out = _pdf_render(self._handle, index, ctypes.byref(options), ctypes.byref(out_width),
                          ctypes.byref(out_height), ctypes.byref(error))
        if not out:
            _raise(error, "the page cannot be rendered")
        return out_width.value, out_height.value, _buffer(out, out_width.value * out_height.value * 4)

    def render_annotation(self, index, annotation, mode=0, scale=1.0, rotation=0, background=0):
        """(width, height, RGBA bytes) of an annotation's appearance alone (mode 0
        normal, 1 rollover, 2 down), or None when it has no such appearance."""
        options = _RenderOptions(scale, rotation, 0, background, 0, 0, 0, 0)
        out_width, out_height, error = ctypes.c_int(), ctypes.c_int(), ctypes.c_void_p()
        out = _pdf_render_annotation(self._handle, index, annotation, mode, ctypes.byref(options),
                                     ctypes.byref(out_width), ctypes.byref(out_height), ctypes.byref(error))
        if not out:
            message = _string(error.value) if error.value else "the annotation cannot be rendered"
            if message == "no appearance":
                return None
            raise Error(message)
        return out_width.value, out_height.value, _buffer(out, out_width.value * out_height.value * 4)

    def text(self, index):
        error = ctypes.c_void_p()
        out = _pdf_text(self._handle, index, ctypes.byref(error))
        if not out:
            _raise(error, "no text")
        return _string(out)

    def text_layout(self, index):
        return self._json(_pdf_text_layout, index)

    def text_rects(self, index, start, count):
        return self._json(_pdf_text_rects, index, start, count)

    def char_at(self, index, x, y, tolerance=2.0):
        found = _pdf_char_at(self._handle, index, x, y, tolerance)
        return None if found < 0 else found

    def search(self, query, match_case=False, whole_word=False, first=0, limit=0):
        flags = (1 if match_case else 0) | (2 if whole_word else 0)
        return self._json(_pdf_search, query.encode(), flags, first, limit)

    def outline(self):
        return self._json(_pdf_outline)

    def links(self, index):
        return self._json(_pdf_links, index)

    def annotations(self, index):
        return self._json(_pdf_annotations, index)

    def add_annotation(self, index, annotation):
        error = ctypes.c_void_p()
        added = _pdf_annotation_add(self._handle, index, json.dumps(annotation).encode(), ctypes.byref(error))
        if added < 0:
            _raise(error, "the annotation cannot be made")
        return added

    def update_annotation(self, index, annotation, changes):
        self._ok(_pdf_annotation_update, index, annotation, json.dumps(changes).encode())

    def remove_annotation(self, index, annotation):
        self._ok(_pdf_annotation_remove, index, annotation)

    def flatten(self, index, for_print=False):
        self._ok(_pdf_flatten, index, for_print)

    def attachments(self):
        return self._json(_pdf_attachments)

    def attachment(self, index):
        size, error = ctypes.c_size_t(), ctypes.c_void_p()
        out = _pdf_attachment(self._handle, index, ctypes.byref(size), ctypes.byref(error))
        if not out:
            _raise(error, "no attachment")
        return _buffer(out, size.value)

    def attach(self, name, data):
        data = _bytes(data)
        self._ok(_pdf_attach, name.encode(), data, len(data))

    def detach(self, index):
        self._ok(_pdf_detach, index)

    def save(self, incremental=False, no_security=False):
        size, error = ctypes.c_size_t(), ctypes.c_void_p()
        out = _pdf_save(self._handle, (1 if incremental else 0) | (2 if no_security else 0), ctypes.byref(size),
                        ctypes.byref(error))
        if not out:
            _raise(error, "the document cannot be saved")
        return _buffer(out, size.value)


# ---- URL schemes of the application's own ----

SCHEME_STANDARD, SCHEME_SECURE, SCHEME_FETCH, SCHEME_BYPASS_CSP = 1, 2, 4, 8

_SCHEME = ctypes.CFUNCTYPE(None, _P, _P)
_scheme_register = _declare("zebkit_scheme_register", _B, _S, _U)
_scheme_handle = _declare("zebkit_scheme_handle", None, _S, _SCHEME, _P)
_request_url = _declare("zebkit_request_url", _S, _P)
_request_method = _declare("zebkit_request_method", _S, _P)
_request_headers = _declare("zebkit_request_headers", _S, _P)
_request_body = _declare("zebkit_request_body", ctypes.POINTER(ctypes.c_uint8), _P, _ZP)
_request_respond = _declare("zebkit_request_respond", None, _P, _I, _S, _P, _Z)
_request_respond_file = _declare("zebkit_request_respond_file", None, _P, _S, _S)


class Request:
    """A request for a URL of the application's scheme. Answer it once, with
    respond() or respond_file(), from any thread."""

    def __init__(self, handle):
        self._handle = handle
        self.url = _request_url(handle).decode()
        self.method = _request_method(handle).decode()
        self.headers = {}
        for line in _request_headers(handle).decode().split("\r\n"):
            name, _, value = line.partition(":")
            if name:
                self.headers[name.strip()] = value.strip()
        size = ctypes.c_size_t()
        body = _request_body(handle, ctypes.byref(size))
        self.body = ctypes.string_at(body, size.value) if body else None

    def _take(self):
        handle, self._handle = self._handle, None
        if not handle:
            raise Error("the request is answered already")
        return handle

    def respond(self, status=200, body=b"", headers=None):
        body = _bytes(body)
        lines = "\r\n".join(f"{k}: {v}" for k, v in (headers or {}).items())
        _request_respond(self._take(), status, lines.encode(), body, len(body))

    def respond_file(self, path, headers=None):
        lines = "\r\n".join(f"{k}: {v}" for k, v in (headers or {}).items())
        _request_respond_file(self._take(), os.fspath(path).encode(), lines.encode())


def scheme_register(scheme, flags=SCHEME_STANDARD | SCHEME_SECURE | SCHEME_FETCH):
    """Makes scheme one of the application's: before main()."""
    return bool(_scheme_register(scheme.encode(), flags))


def scheme_handle(scheme, function):
    """function(Request) answers the scheme's requests, on the engine's thread."""
    callback = _SCHEME(_guard(lambda handle, data: function(Request(handle))))
    _alive.add(callback)
    _scheme_handle(scheme.encode(), callback, None)


# ---- The application ---------------------------------------------------

_MENU = ctypes.CFUNCTYPE(None, ctypes.c_char_p, _P)
_PATHS = ctypes.CFUNCTYPE(None, ctypes.c_char_p, _P)
_APP = ctypes.CFUNCTYPE(None, _P)
_app_set_menu = _declare("zebkit_app_set_menu", _B, _S, _MENU, _P, _EP)
_app_on_open_files = _declare("zebkit_app_on_open_files", None, _PATHS, _P)
_app_on_reopen = _declare("zebkit_app_on_reopen", None, _APP, _P)
_app_on_quit_request = _declare("zebkit_app_on_quit_request", None, _APP, _P)
# The application's callbacks, one of each, as the engine holds them.
_app_callbacks = {}


def app_set_menu(menu, function):
    """The application's menu: a list of {"title", "items"} (or its JSON),
    where an item is {"title", "id", "key", "modifiers"}, {"role"},
    {"title", "items"} or {"separator": True}; function(id) is called when
    an item with an id is chosen. The menu bar on macOS; elsewhere the
    items' keys work in the application's windows. On the engine's thread."""
    text = menu if isinstance(menu, str) else json.dumps(menu)
    callback = _MENU(_guard(lambda item, data: function(item.decode())))
    error = ctypes.c_void_p()
    if not _app_set_menu(text.encode(), callback, None, ctypes.byref(error)):
        _raise(error, "the menu was refused")
    _app_callbacks["menu"] = callback


def app_on_open_files(function):
    """function(paths) for files the system asks the running application to
    open (macOS: the Finder and the Dock); elsewhere they are arguments."""
    callback = _PATHS(_guard(lambda paths, data: function(json.loads(paths.decode()))))
    _app_callbacks["open"] = callback
    _app_on_open_files(callback, None)


def app_on_reopen(function):
    """function() when the application's icon in the Dock is clicked with no window shown."""
    callback = _APP(_guard(lambda data: function()))
    _app_callbacks["reopen"] = callback
    _app_on_reopen(callback, None)


def app_on_quit_request(function):
    """function() when the user asks the application to quit; it calls quit() or not."""
    callback = _APP(_guard(lambda data: function()))
    _app_callbacks["quit"] = callback
    _app_on_quit_request(callback, None)
