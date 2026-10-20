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

The library is libzebkit.so.1 in $ZEBKIT_DIR, or beside this package, or in
the zebkit repository's out/blink/slim/package.

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
    "ALLOW_URL", "ALLOW_ALL",
]

LOAD_STARTED, LOAD_COMMITTED, LOAD_FINISHED, LOAD_FAILED = 0, 1, 2, 3
ALLOW_APP, ALLOW_FETCH, ALLOW_FILES, ALLOW_COMPRESS = 1, 2, 4, 8
ALLOW_CRYPTO, ALLOW_IMAGE, ALLOW_URL, ALLOW_ALL = 16, 32, 64, 0x7F

_COMPRESSION = {"gzip": 1, "zlib": 2, "deflate": 2, "deflate-raw": 3, "brotli": 4, "br": 4, "zstd": 5}
_DIGEST = {"sha1": 1, "sha256": 2, "sha384": 3, "sha512": 4}
_IMAGE = {"png": 1, "jpeg": 2, "webp": 3}
_URL_PART = {"spec": 0, "scheme": 1, "username": 2, "password": 3, "host": 4, "port": 5, "path": 6,
             "query": 7, "fragment": 8, "origin": 9}


class Error(Exception):
    """What the engine reports when something it was asked to do fails."""


def _find_library():
    names = []
    if os.environ.get("ZEBKIT_DIR"):
        names.append(pathlib.Path(os.environ["ZEBKIT_DIR"]) / "libzebkit.so.1")
    here = pathlib.Path(__file__).resolve().parent
    names.append(here / "libzebkit.so.1")
    names.append(here.parents[2] / "out" / "blink" / "slim" / "package" / "libzebkit.so.1")
    for name in names:
        if name.exists():
            return str(name)
    raise ImportError("libzebkit.so.1 not found; set ZEBKIT_DIR to the engine's directory")


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
