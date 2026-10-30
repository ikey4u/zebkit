"""Native media packets, RGBA frames, PCM and optional file recording.

All calls and callbacks run on the engine thread. Example::

    from zebkit.media_tap import MediaTap, list_media
    list_media(view, lambda media, error: start(media[0]['id']))
    tap = MediaTap(view, media_id, on_data, path='/tmp/movie.mp4', maxFps=5)
    # on_data(metadata: dict, payload: bytes); payload is an owned copy.
    tap.stop()  # Continue pumping until tap.done, then tap.close().

For MediaStream use packets=False (no path): its original demuxed packets
are unavailable. Video samples are displayed frames, not a decode-all API.
"""
import ctypes
import json
import weakref

from . import Error, _declare, _P, _S, _Z, _EP, _RESULT, _string

_EVENT = ctypes.CFUNCTYPE(None, _P, _S, _P, _Z, _P)
_start = _declare('zebkit_media_tap_start', _P, _P, _S, _S, _EVENT, _P, _EP)
_stop = _declare('zebkit_media_tap_stop', None, _P)
_free = _declare('zebkit_media_tap_free', None, _P)
_list = _declare('zebkit_view_media_list', None, _P, _RESULT, _P)
_active = set()
_pending_lists = {}


def list_media(view, callback):
    """Call callback(list_or_none, error_or_none) with every frame's media."""
    @_RESULT
    def result(_view, value, error, _data):
        _pending_lists.pop(id(result), None)
        callback(json.loads(value) if value else None,
                 error.decode() if error else None)
    handle = view._check()
    _pending_lists[id(result)] = result
    _list(handle, result, None)


class MediaTap:
    """Active taps stay alive until done or close; explicitly stop/close them.

    Callback exceptions are stored in callback_error and stop the tap. File
    finalization remains asynchronous. close() suppresses subsequent events.
    """
    def __init__(self, view, media_id, callback, **options):
        self._handle = None
        if not callable(callback):
            raise TypeError('callback must be callable')
        self._handle = None
        self._callback = callback
        self.done = None
        self.callback_error = None
        owner = weakref.ref(self)

        @_EVENT
        def event(_tap, text, pointer, size, _data):
            current = owner()
            if current is None:
                return
            metadata = json.loads(text)
            for key in ('ptsUs', 'dtsUs', 'durationUs'):
                if key in metadata:
                    metadata[key] = int(metadata[key])
            payload = ctypes.string_at(pointer, size) if size else b''
            terminal = metadata.get('type') == 'done'
            if terminal:
                current.done = metadata
            try:
                if current._callback:
                    current._callback(metadata, payload)
            except Exception as exc:
                current.callback_error = exc
                current.stop()
            finally:
                if terminal:
                    _active.discard(current)

        self._event = event
        error = ctypes.c_void_p()
        self._handle = _start(view._check(), str(media_id).encode(),
                              json.dumps(options, allow_nan=False).encode(),
                              event, None, ctypes.byref(error))
        if not self._handle:
            raise Error(_string(error.value) or 'media tap did not start')
        _active.add(self)

    def stop(self):
        """Stop receiving samples; done follows when any file is finalized."""
        if self._handle:
            _stop(self._handle)

    def close(self):
        """Release the handle and suppress callbacks; safe inside a callback."""
        handle, self._handle = getattr(self, "_handle", None), None
        if handle:
            _free(handle)
        _active.discard(self)
        self._callback = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def __del__(self):
        self.close()
