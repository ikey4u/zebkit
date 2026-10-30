"""Source UTC dates: HLS, buffered discontinuities, WebM DateUTC and opt-out."""
import ctypes
import datetime
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import zebkit

base, mode = sys.argv[1:3]
assert zebkit.init([sys.argv[0], *sys.argv[3:]], settings={"logging": {"destination": "none"}}) == 0
keep = zebkit._declare('zebkit_media_keep_history', None, ctypes.c_bool)
listing = zebkit._declare('zebkit_view_media_list', None, zebkit._V, zebkit._RESULT, zebkit._P)
start = zebkit._declare('zebkit_media_capture_start', zebkit._P, zebkit._V, ctypes.c_char_p,
                       ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(zebkit._P))
status = zebkit._declare('zebkit_media_capture_status', zebkit._P, zebkit._P)
stop = zebkit._declare('zebkit_media_capture_stop', None, zebkit._P)
free = zebkit._declare('zebkit_media_capture_free', None, zebkit._P)
callback = ctypes.CFUNCTYPE(None, zebkit._P, ctypes.c_char_p, zebkit._P)
on_done = zebkit._declare('zebkit_media_capture_on_done', None, zebkit._P, callback, zebkit._P)
keep(True)
view = zebkit.View()
state = {'loaded': False, 'done': None}
view.on_load(lambda event, url: state.update(loaded=True) if event == zebkit.LOAD_FINISHED and url.endswith('/media.html') else None)
view.load(base + '/media.html')


def wait(condition, timeout=20):
    deadline = time.monotonic() + timeout
    while not condition() and time.monotonic() < deadline:
        assert zebkit.pump()
        time.sleep(.001)
    assert condition(), state


def evaluate(script):
    result = []
    view.evaluate(script, lambda value, error: result.append((value, error)))
    wait(lambda: bool(result))
    assert result[0][1] is None, result
    return result[0][0]


def utc(value):
    return datetime.datetime.fromisoformat(value.replace('Z', '+00:00')).timestamp()


capture = None
try:
    with tempfile.TemporaryDirectory(prefix='zebkit-clock-') as work:
        wait(lambda: state['loaded'])
        hls = mode.startswith('hls')
        source = 'media/clock/discontinuity.m3u8' if mode == 'hls-discontinuity' else 'media/clock/program.m3u8' if hls else 'media/tone.webm' if mode == 'unknown' else 'media/clock/date.webm'
        assert evaluate(f'file({json.dumps("video" if hls else "audio")},{json.dumps(source)})') is True
        wait(lambda: evaluate('ready() || !!state.error'))
        assert not evaluate('state.error'), evaluate('state.error')
        if mode == 'hls-discontinuity':
            assert evaluate('play(2)') is True
            wait(lambda: evaluate('media.currentTime>=5 || !!state.error'))
            assert evaluate('media.pause();true') is True
        elements = []

        @zebkit._RESULT
        def listed(handle, value, error, data):
            if error:
                state['error'] = error.decode()
            else:
                elements.extend(json.loads(value))

        listing(view._check(), listed, None)
        wait(lambda: bool(elements) or 'error' in state)
        assert 'error' not in state, state
        assert elements[0]['kind'] == ('hls' if hls else 'file'), elements
        options = {'includeBuffered': True}
        if mode == 'off':
            options['wallClock'] = False
        for invalid in ['now', {}, {'mediaTime': 0}, {'mediaTime': 0, 'unixTime': 1e100},
                        {'mediaTime': 0, 'unixTime': 1700000000, 'extra': True}]:
            rejected = zebkit._P()
            candidate = start(view._check(), elements[0]['id'].encode(), os.fsencode(Path(work) / 'invalid.mp4'),
                              json.dumps({'wallClock': invalid}).encode(), ctypes.byref(rejected))
            assert not candidate and rejected.value, invalid
            zebkit._string(rejected.value)
            assert not (Path(work) / 'invalid.mp4').exists()
        error = zebkit._P()
        capture = start(view._check(), elements[0]['id'].encode(), os.fsencode(Path(work) / ('clock.mp4' if hls else 'clock.webm')),
                        json.dumps(options).encode(), ctypes.byref(error))
        assert capture, zebkit._string(error.value) if error.value else 'capture rejected'

        @callback
        def done(handle, value, data):
            state['done'] = json.loads(value)

        on_done(capture, done, None)
        wait(lambda: json.loads(zebkit._string(status(capture)))['state'] == 'capturing')
        assert evaluate('play(2)') is True
        wait(lambda: evaluate('state.ended || !!state.error'))
        assert not evaluate('state.error'), evaluate('state.error')
        stop(capture)
        wait(lambda: state['done'] is not None)
        result = state['done']
        assert not result.get('error'), result
        assert len(result['files']) == 1, result
        file = result['files'][0]
        probe = json.loads(subprocess.check_output([os.environ['ZEBKIT_MEDIA_PROBE'], file['path']], text=True))
        assert probe['ok'] and probe['errors'] == 0, probe
        assert probe['videoFrames'] >= 180 if hls else probe['audioFrames'] >= 390, probe
        if mode in ('off', 'unknown'):
            assert 'wallClockStart' not in file and 'wallClockMapping' not in file, file
            assert not probe['wallClockMetadata'] and not probe['creationTime'], probe
        else:
            assert 'wallClockStart' in file and file['wallClockMapping']['source'] == 'player', file
            assert abs(utc(file['wallClockStart']) - 1700000000.25) < .05, file
            assert json.loads(probe['wallClockMetadata']) == file['wallClockMapping'], probe
            assert utc(probe['creationTime']) == int(utc(file['wallClockStart'])) if hls else abs(utc(probe['creationTime']) - utc(file['wallClockStart'])) < .00001, probe
            segments = file['wallClockMapping']['segments']
            assert abs(segments[0]['unixTime'] - 1700000000.25) < .05, segments
            if mode == 'hls-discontinuity':
                assert len(segments) >= 2, segments
                late = next(s for s in segments if s['mediaTime'] >= 3.9)
                assert abs(late['unixTime'] - (1700000010.25 + late['mediaTime'])) < .05, segments
                assert abs(late['fileTime'] - late['mediaTime']) < .1, segments
        print('ok source wall clock mode=' + mode + ': source UTC, container metadata, buffered history and decoded content', flush=True)
finally:
    if capture:
        free(capture)
    assert zebkit.shutdown()
