"""JavaScript libraries as normal libraries (<zebkit/library.h>, docs/JS_LIBRARIES.md).

    import zebkit
    import zebkit.library

    md = zebkit.library.load("libs/markdown")          # on the engine's thread
    md.call("render", "# Title").add_done_callback(lambda f: print(f.result()))
    md.call("render", "# Title", function=lambda value, error: print(value, error))

A library is a folder with index.mjs, or one .mjs file, bundled (js-bundle.mjs);
it runs in a V8 isolate of its own, on a thread of its own. Values are JSON
with bytes in place: bytes, bytearray and memoryview go as parts (a Uint8Array
in the library), and the library's typed arrays come back as bytes.

load() and close() are for the engine's thread; call() and cancel() for any
thread. A call's outcome is a zebkit.automation.Future (awaitable in
zebkit.automation.run()), or function(value, error) when given, on the
engine's thread; error is a LibraryError.
"""

import ctypes
import itertools
import json
import threading

from . import Error, _EP, _P, _S, _Z, _declare, _guard, _require_thread, _string
from .automation import Future

__all__ = [
    "Library", "LibraryError", "load", "CODES",
    "OK", "THROWN", "LOAD_FAILED", "NO_FUNCTION", "BAD_ARGUMENTS", "BAD_RESULT", "TIMEOUT", "CANCELLED",
    "QUEUE_FULL", "OUT_OF_MEMORY", "GONE",
]

CODES = ("OK", "THROWN", "LOAD_FAILED", "NO_FUNCTION", "BAD_ARGUMENTS", "BAD_RESULT", "TIMEOUT", "CANCELLED",
         "QUEUE_FULL", "OUT_OF_MEMORY", "GONE")
OK, THROWN, LOAD_FAILED, NO_FUNCTION, BAD_ARGUMENTS, BAD_RESULT, TIMEOUT, CANCELLED, QUEUE_FULL, \
    OUT_OF_MEMORY, GONE = range(11)


class _Part(ctypes.Structure):
    _fields_ = [("data", ctypes.c_void_p), ("size", ctypes.c_size_t)]


_TOLD = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p, ctypes.POINTER(_Part),
                         ctypes.c_size_t, ctypes.c_void_p)
_library_load = _declare("zebkit_library_load", _P, _S, _S, _TOLD, _P, _EP)
_library_call = _declare("zebkit_library_call", ctypes.c_uint64, _P, _S, _S, ctypes.POINTER(_Part), _Z,
                         ctypes.c_int64, _TOLD, _P)
_library_cancel = _declare("zebkit_library_cancel", None, _P, ctypes.c_uint64)
_library_close = _declare("zebkit_library_close", None, _P)


class LibraryError(Error):
    """A call or load that failed: code (an int, CODES[code] its name), and
    what the library threw (name, stack) or what the engine says."""

    def __init__(self, code, message, name="", stack=""):
        super().__init__(message)
        self.code, self.message, self.name, self.stack = code, message, name, stack

    @property
    def code_name(self):
        return CODES[self.code] if 0 <= self.code < len(CODES) else str(self.code)

    def __str__(self):
        return "%s: %s%s" % (self.code_name, self.name + ": " if self.name else "", self.message)


# The outcomes waited for, by the key given as the callback's data.
_waiting = {}
_waiting_lock = threading.Lock()
_keys = itertools.count(1)


def _to_wire(args):
    """The arguments as JSON (bytes) and parts."""
    parts = []

    def walk(value):
        if isinstance(value, (bytes, bytearray, memoryview)):
            parts.append(bytes(value))
            return {"$part": len(parts) - 1}
        if isinstance(value, dict):
            return {key: walk(item) for key, item in value.items()}
        if isinstance(value, (list, tuple)):
            return [walk(item) for item in value]
        return value
    text = json.dumps(walk(list(args)), ensure_ascii=False, allow_nan=False, separators=(",", ":"))
    return text.encode("utf-8", "surrogatepass"), parts


def _from_wire(text, parts):
    if not parts:
        return json.loads(text)

    def hook(value):
        if len(value) == 1 and "$part" in value and isinstance(value["$part"], int) and \
                0 <= value["$part"] < len(parts):
            return parts[value["$part"]]
        return value
    return json.loads(text, object_hook=hook)


def _told(library, code, text, parts, count, data):
    with _waiting_lock:
        handler = _waiting.pop(data or 0, None)
    if handler is None:
        return
    text = text.decode("utf-8", "replace") if text is not None else "null"
    if code == OK:
        values = [ctypes.string_at(parts[i].data, parts[i].size) if parts[i].size else b"" for i in range(count)]
        handler(_from_wire(text, values), None)
    else:
        try:
            info = json.loads(text)
        except ValueError:
            info = {}
        info = info if isinstance(info, dict) else {}
        handler(None, LibraryError(code, str(info.get("message", CODES[code] if 0 <= code < len(CODES) else code)),
                                   str(info.get("name") or ""), str(info.get("stack") or "")))


_told_callback = _TOLD(_guard(_told))


def _expect(function):
    """A key for the outcome, and the Future (or None) it settles."""
    future = None
    if function is None:
        future = Future()

        def function(value, error):
            if error is not None:
                future.set_error(error)
            else:
                future.set_result(value)
    key = next(_keys)
    with _waiting_lock:
        _waiting[key] = function
    return key, future


def _forget(key):
    with _waiting_lock:
        _waiting.pop(key, None)


class Library:
    """A loaded JavaScript library (load())."""

    def __init__(self, path, options=None, ready=None):
        """Loads the library at path and returns at once; calls wait for the
        load. options: zebkit_library_load()'s ("timeoutMs", "queue",
        "heapLimitMB", "maxBytes", "network"). ready(value, error), if given,
        is told once: {"functions": [...]}, or a LibraryError. Raises Error
        for bad options or a path that is no library. The engine's thread."""
        _require_thread("zebkit.library.load()")
        self._lock = threading.RLock()
        self._closed = False
        key, self.ready = _expect(ready)
        error = ctypes.c_void_p()
        raw = _library_load(str(path).encode(), json.dumps(options).encode() if options else None, _told_callback,
                            ctypes.c_void_p(key), ctypes.byref(error))
        if not raw:
            _forget(key)
            raise Error(_string(error.value) or "the library was not loaded")
        self._raw = raw

    @classmethod
    def load(cls, path, options=None, ready=None):
        return cls(path, options, ready)

    def call(self, name, *args, timeout_ms=0, function=None):
        """Calls the exported function name with args (JSON values, bytes
        in place). timeout_ms: 0 the library's, < 0 none. A Future of its
        value; with function, function(value, error) is called instead, and
        the call's number returned. Any thread; Error once closed."""
        text, parts = _to_wire(args)
        array = (_Part * len(parts))()
        keep = []
        for i, part in enumerate(parts):
            buffer = ctypes.create_string_buffer(part, len(part)) if part else None
            keep.append(buffer)
            array[i].data = ctypes.cast(buffer, ctypes.c_void_p).value if buffer is not None else None
            array[i].size = len(part)
        key, future = _expect(function)
        with self._lock:
            number = 0 if self._closed else _library_call(self._raw, str(name).encode(), text, array, len(parts),
                                                          int(timeout_ms), _told_callback, ctypes.c_void_p(key))
        if not number:
            _forget(key)
            raise Error("the library is closed")
        if future is None:
            return number
        future.call_id = number
        future._on_cancel = lambda: self.cancel(number)
        return future

    def cancel(self, call):
        """The call (its number, or its Future) is told CANCELLED, unless
        told already. Any thread."""
        number = getattr(call, "call_id", call)
        with self._lock:
            if not self._closed and number:
                _library_cancel(self._raw, int(number))

    def close(self):
        """Ends the library: calls not told yet fail with GONE. The engine's
        thread."""
        _require_thread("Library.close()")
        with self._lock:
            if not self._closed:
                self._closed = True
                _library_close(self._raw)

    @property
    def closed(self):
        return self._closed


def load(path, options=None, ready=None):
    """Library(path, options, ready)."""
    return Library(path, options, ready)
