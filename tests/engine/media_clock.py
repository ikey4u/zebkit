"""Source UTC dates: HLS, buffered discontinuities, WebM DateUTC and opt-out. A
capture's wall clock from the player's source (an HLS program date, a WebM
DateUTC), with the buffered history; written into the file's metadata, read
back with the engine's FFmpeg (the media_probe program ZEBKIT_MEDIA_PROBE
names); none with wallClock off or where the source has no date.

    python3 tests/engine/media_clock.py <base URL> <hls|hls-discontinuity|webm|off|unknown> [engine switches...]

Exit 0 when every check passes.
"""
import datetime
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import check, ev, finish, must_wait, part, refused, zebkit  # noqa: E402


def utc(value):
    return datetime.datetime.fromisoformat(value.replace('Z', '+00:00')).timestamp()


def evaluate(view, script):
    """ev(), raising when the script threw or did not answer."""
    value = ev(view, script)
    assert not (isinstance(value, tuple) and value[:1] in (('error',), ('timeout',))), value
    return value


def listed(view):
    box = []
    view.media_list(lambda media, error: box.append((media, error)))
    must_wait(lambda: box, 'the media list')
    assert box[0][1] is None, box[0][1]
    return box[0][0] or []


def run(view, base, mode, work):
    hls = mode.startswith('hls')
    state = {'loaded': False, 'done': None}
    view.on_load(lambda event, url: state.update(loaded=True)
                 if event == zebkit.LOAD_FINISHED and url.endswith('/media.html') else None)
    view.load(base + '/media.html')
    with part('the source plays: ' + mode, fatal=True):
        must_wait(lambda: state['loaded'], 'media.html', 20)
        source = ('media/clock/discontinuity.m3u8' if mode == 'hls-discontinuity' else 'media/clock/program.m3u8'
                  if hls else 'media/tone.webm' if mode == 'unknown' else 'media/clock/date.webm')
        assert evaluate(view, f'file({json.dumps("video" if hls else "audio")},{json.dumps(source)})') is True
        must_wait(lambda: evaluate(view, 'ready() || !!state.error'), 'the source ready', 20)
        assert not evaluate(view, 'state.error'), evaluate(view, 'state.error')
        if mode == 'hls-discontinuity':
            assert evaluate(view, 'play(2)') is True
            must_wait(lambda: evaluate(view, 'media.currentTime>=5 || !!state.error'), 'past the discontinuity', 20)
            assert evaluate(view, 'media.pause();true') is True
    with part('the media list has it, as ' + ('hls' if hls else 'file'), fatal=True):
        elements = listed(view)
        assert elements and elements[0]['kind'] == ('hls' if hls else 'file'), elements
    media = elements[0]['id']
    invalid_path = Path(work) / 'invalid.mp4'
    for invalid in ['now', {}, {'mediaTime': 0}, {'mediaTime': 0, 'unixTime': 1e100},
                    {'mediaTime': 0, 'unixTime': 1700000000, 'extra': True}]:
        message = refused(lambda: view.media_capture(media, invalid_path, wallClock=invalid).close())
        check(message is not None and not invalid_path.exists(), 'an invalid wallClock is refused: %s' % json.dumps(invalid),
              message)
    options = {'includeBuffered': True}
    if mode == 'off':
        options['wallClock'] = False
    capture = None
    try:
        with part('the capture runs to its end, one file', fatal=True):
            capture = view.media_capture(media, Path(work) / ('clock.mp4' if hls else 'clock.webm'), **options)
            capture.on_done(lambda result: state.update(done=result))
            must_wait(lambda: (capture.status() or {}).get('state') == 'capturing', 'capturing', 20)
            assert evaluate(view, 'play(2)') is True
            must_wait(lambda: evaluate(view, 'state.ended || !!state.error'), 'the end', 20)
            assert not evaluate(view, 'state.error'), evaluate(view, 'state.error')
            capture.stop()
            must_wait(lambda: state['done'] is not None, 'the capture done', 20)
            result = state['done']
            assert not result.get('error'), result
            assert len(result['files']) == 1, result
    finally:
        if capture:
            capture.close()
    file = result['files'][0]
    with part('the file decodes, all of it', fatal=True):
        probe = json.loads(subprocess.check_output([os.environ['ZEBKIT_MEDIA_PROBE'], file['path']], text=True))
        assert probe['ok'] and probe['errors'] == 0, probe
        assert probe['videoFrames'] >= 180 if hls else probe['audioFrames'] >= 390, probe
    if mode in ('off', 'unknown'):
        check('wallClockStart' not in file and 'wallClockMapping' not in file, 'no wall clock told', file)
        check(not probe['wallClockMetadata'] and not probe['creationTime'], 'none in the file', probe)
        return
    with part('the source\'s UTC told, from the player'):
        assert 'wallClockStart' in file and file['wallClockMapping']['source'] == 'player', file
        assert abs(utc(file['wallClockStart']) - 1700000000.25) < .05, file
    with part('and in the container\'s metadata'):
        assert json.loads(probe['wallClockMetadata']) == file['wallClockMapping'], probe
        assert (utc(probe['creationTime']) == int(utc(file['wallClockStart'])) if hls
                else abs(utc(probe['creationTime']) - utc(file['wallClockStart'])) < .00001), probe
    with part('the buffered history\'s segments'):
        segments = file['wallClockMapping']['segments']
        assert abs(segments[0]['unixTime'] - 1700000000.25) < .05, segments
        if mode == 'hls-discontinuity':
            assert len(segments) >= 2, segments
            late = next(s for s in segments if s['mediaTime'] >= 3.9)
            assert abs(late['unixTime'] - (1700000010.25 + late['mediaTime'])) < .05, segments
            assert abs(late['fileTime'] - late['mediaTime']) < .1, segments


def main():
    base, mode = sys.argv[1:3]
    if zebkit.init([sys.argv[0], *sys.argv[3:]], settings={'logging': {'destination': 'none'}}) != 0:
        print('FAIL zebkit.init()')
        return 1
    try:
        zebkit.media_keep_history(True)
        view = zebkit.View()
        with tempfile.TemporaryDirectory(prefix='zebkit-clock-') as work:
            with part('source wall clock mode=' + mode):
                run(view, base, mode, work)
    finally:
        check(zebkit.shutdown(), 'zebkit.shutdown()')
    return finish()


if __name__ == '__main__':
    sys.exit(main())
