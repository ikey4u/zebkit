"""Synchronous media tools using zebkit's FFmpeg; no engine loop required.

Run long calls in an application worker thread. argv is a sequence of strings,
without an executable name or shell quoting. See docs/MEDIA_TOOLS.md for the
supported CLI subset. Unsupported codecs/options raise Error.
"""
import ctypes
import json
import os

from . import Error, _declare, _P, _S, _string

_PROGRESS = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_int64, ctypes.c_int64, _P)
_probe = _declare('zebkit_ffprobe', ctypes.c_int, ctypes.c_int,
                  ctypes.POINTER(_S), ctypes.POINTER(_P), ctypes.POINTER(_P))
_process = _declare('zebkit_ffmpeg', ctypes.c_int, ctypes.c_int,
                    ctypes.POINTER(_S), _PROGRESS, _P, ctypes.POINTER(_P))


class Cancelled(Error):
    """Processing was cancelled; the destination was not changed."""


def _args(argv):
    if isinstance(argv, (str, bytes)):
        raise TypeError('argv must be a sequence, not a shell command')
    values = [os.fsencode(arg) for arg in argv]
    if any(not arg or b'\0' in arg for arg in values):
        raise ValueError('arguments must be nonempty and contain no NUL')
    return (_S * len(values))(*values)


def ffprobe(argv):
    """Return format/streams as a dict (JSON-compatible ffprobe subset)."""
    args = _args(argv)
    result, error = _P(), _P()
    code = _probe(len(args), args, ctypes.byref(result), ctypes.byref(error))
    message = _string(error)
    value = _string(result)
    if code:
        raise Error(message or 'media probe failed')
    return json.loads(value)


def probe(path):
    """Inspect a local media file; return format and stream metadata."""
    return ffprobe(['-i', path])


def ffmpeg(argv, *, progress=None):
    """Process a local file. progress(packets, time_us_or_None) returns bool.

    Return False to cancel; callback exceptions are re-raised after native
    cleanup. A callback runs synchronously on the calling thread.
    """
    args = _args(argv)
    if progress is not None and not callable(progress):
        raise TypeError('progress must be callable')
    failure = []

    @_PROGRESS
    def callback(packets, time_us, _data):
        if progress is None:
            return 1
        try:
            return int(bool(progress(packets, None if time_us == -(1 << 63) else time_us)))
        except BaseException as exc:
            failure.append(exc)
            return 0

    error = _P()
    code = _process(len(args), args, callback, None, ctypes.byref(error))
    message = _string(error)
    if failure:
        raise failure[0]
    if code == 1:
        raise Cancelled(message or 'cancelled')
    if code:
        raise Error(message or 'media processing failed')


def remux(source, destination, *, streams=None, format=None, overwrite=False, progress=None):
    """Copy encoded streams without quality loss; default selects audio/video."""
    args = ['-y' if overwrite else '-n', '-i', source, '-c', 'copy']
    if streams is not None:
        streams = list(streams)
        if not streams:
            raise ValueError('streams must not be empty')
        for stream in streams:
            if type(stream) is not int or stream < 0:
                raise ValueError('stream indices must be nonnegative integers')
            args += ['-map', f'0:{stream}']
    if format:
        args += ['-f', format]
    ffmpeg(args + ['--', destination], progress=progress)


def extract_audio(source, destination, *, sample_rate=16000, channels=1,
                  stream=None, raw=False, overwrite=False, progress=None):
    """Decode to WAV or raw interleaved s16le, resampling as requested."""
    args = ['-y' if overwrite else '-n', '-i', source, '-c:a', 'pcm_s16le',
            '-ar', str(sample_rate), '-ac', str(channels), '-f', 's16le' if raw else 'wav']
    if stream is not None:
        args += ['-map', f'0:{stream}']
    ffmpeg(args + ['--', destination], progress=progress)


def extract_video(source, destination, *, size=None, frames=None, stream=None,
                  overwrite=False, progress=None):
    """Decode to packed RGBA frames. No headers; use probe() for source size.

    size=(width,height) scales every frame. frames limits the output count.
    Rotation/display matrices and HDR tone mapping are not applied.
    """
    args = ['-y' if overwrite else '-n', '-i', source, '-c:v', 'rawvideo', '-f', 'rawvideo']
    if size is not None:
        width, height = size
        args += ['-s', f'{width}x{height}']
    if frames is not None:
        args += ['-frames:v', str(frames)]
    if stream is not None:
        args += ['-map', f'0:{stream}']
    ffmpeg(args + ['--', destination], progress=progress)
