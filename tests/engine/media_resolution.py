"""Real MSE H.264 resolution switch: one avc3 file, decoded at both sizes. A
capture of Media Source Extensions across a change of resolution
(ZEBKIT_ADAPTIVE_MODE: down, up, audio with an audio track, mkv in Matroska,
clock with a wall clock given), read back with the engine's FFmpeg (the
media_probe program ZEBKIT_MEDIA_PROBE names) and played again in a page.

    ZEBKIT_ADAPTIVE_MODE=<mode> python3 tests/engine/media_resolution.py <base URL> [engine switches...]

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

from harness import check, ev, finish, must_wait, part, zebkit  # noqa: E402


def evaluate(view, script):
    """ev(), raising when the script threw or did not answer."""
    value = ev(view, script)
    assert not (isinstance(value, tuple) and value[:1] in (('error',), ('timeout',))), value
    return value


def probe(path):
    return json.loads(subprocess.check_output([os.environ['ZEBKIT_MEDIA_PROBE'], path], text=True))


def run(view, base, mode, work):
    state = {'loaded': False, 'done': None}
    view.on_load(lambda event, url: state.update(loaded=True)
                 if event == zebkit.LOAD_FINISHED and url.endswith('/media.html') else None)
    view.load(base + '/media.html')
    with part('the first representation buffered: ' + mode, fatal=True):
        must_wait(lambda: state['loaded'], 'media.html')
        if mode == 'up':
            assert evaluate(view, "(async()=>{const video=make('video');source=new MediaSource();video.src=URL.createObjectURL(source);await new Promise(r=>source.addEventListener('sourceopen',r,{once:true}));buffers=[source.addSourceBuffer(dash.reps[0].type)];await appendTo(buffers[0],'media/adaptive-small.mp4');return true})()") is True
        else:
            assert evaluate(view, ('mse(8)' if mode == 'audio' else 'switched()') + '.then(()=>true)') is True
        must_wait(lambda: evaluate(view, 'ready()'), 'ready()')
    with part('the capture across the switch runs to its end', fatal=True):
        box = []
        view.media_list(lambda media, error: box.append((media, error)))
        must_wait(lambda: box, 'the media list')
        assert box[0][1] is None and box[0][0], box
        options = {'includeBuffered': True, 'format': 'mkv' if mode == 'mkv' else 'mp4'}
        if mode == 'clock':
            options['wallClock'] = {'mediaTime': 8.0, 'unixTime': 1700000008.25}
        capture = view.media_capture(box[0][0][0]['id'], Path(work) / 'adaptive.mp4', **options)
        try:
            capture.on_done(lambda result: state.update(done=result))
            must_wait(lambda: (capture.status() or {}).get('state') == 'capturing', 'capturing')
            if mode == 'up':
                assert evaluate(view, "(async()=>{buffers[0].timestampOffset=buffers[0].buffered.end(0);await appendTo(buffers[0],dash.dir+'init-0.m4s');for(let n=1;n<=8;n++)await appendTo(buffers[0],dash.dir+'chunk-0-'+String(n).padStart(3,'0')+'.m4s');source.endOfStream();return true})()") is True
            else:
                audio = "buffers[1].timestampOffset=8;for(let n=1;n<=4;n++)await appendTo(buffers[1],dash.dir+'chunk-1-'+String(n).padStart(3,'0')+'.m4s');" if mode == 'audio' else ''
                assert evaluate(view, "(async()=>{buffers[0].timestampOffset=8;await appendTo(buffers[0],'media/adaptive-small.mp4');" + audio + "source.endOfStream();return true})()") is True
            assert evaluate(view, 'play(4)') is True
            must_wait(lambda: evaluate(view, 'state.ended'), 'the end', 20)
            assert not evaluate(view, 'state.error'), evaluate(view, 'state.error')
            capture.stop()
            must_wait(lambda: state['done'] is not None, 'the capture done')
        finally:
            capture.close()
        result = state['done']
        assert not result.get('error'), result
    files = result['files']
    print('adaptive capture files:', json.dumps(files), flush=True)
    if mode == 'mkv':
        check(len(files) == 2, 'Matroska keeps incompatible sample configurations apart: two files', len(files))
        with part('adaptive explicit Matroska: preserves separate configurations and all frames'):
            probes = [probe(f['path']) for f in files]
            assert all(p['ok'] and p['errors'] == 0 for p in probes), probes
            assert sum(p['videoFrames'] for p in probes) >= 280, probes
            assert sorted(p['width'] for p in probes) == [160, 320], probes
        return
    check(len(files) == 1, 'a resolution change keeps one MP4', len(files))
    if len(files) != 1:
        return
    start_time = None
    if mode == 'clock':
        with part('the capture keeps the live stream\'s time anchor given'):
            assert 'wallClockStart' in files[0] and 'wallClockMapping' in files[0], files[0]
            start_time = datetime.datetime.fromisoformat(files[0]['wallClockStart'].replace('Z', '+00:00')).timestamp()
            assert abs(start_time - (1700000000.25 + files[0]['start'])) < .00001, files[0]
    with part('one avc3 MP4, both resolutions decoded, all frames', fatal=True):
        read = probe(files[0]['path'])
        assert read['ok'] and read['errors'] == 0 and read['videoFrames'] >= 280, read
        assert (read['minWidth'], read['maxWidth'], read['minHeight'], read['maxHeight']) == (160, 320, 90, 180), read
        assert read['videoTag'] == int.from_bytes(b'avc3', 'little'), read
    if mode == 'clock':
        with part('the anchor in the container\'s metadata'):
            assert json.loads(read['wallClockMetadata']) == files[0]['wallClockMapping'], read
            created = datetime.datetime.fromisoformat(read['creationTime'].replace('Z', '+00:00')).timestamp()
            assert start_time is not None and created == int(start_time), read
    if mode == 'audio':
        check(read['streams'] == 2 and read['audio'] == 'aac' and read['audioFrames'] >= 550,
              'and the audio track, all of it', read)
    with part('adaptive H.264 mode=' + mode + ': the file replayed in a page, every frame'):
        player = Path(work) / 'readback.html'
        player.write_text('<video muted src="' + Path(files[0]['path']).name + '"></video>')
        view.load(player.as_uri())
        must_wait(lambda: evaluate(view, "location.protocol==='file:' && !!document.querySelector('video') && document.querySelector('video').readyState>=1"), 'the file in a page')
        assert evaluate(view, "(()=>{const v=document.querySelector('video');v.playbackRate=4;return v.play().then(()=>true)})()") is True
        must_wait(lambda: evaluate(view, "document.querySelector('video').ended"), 'its end', 20)
        quality = evaluate(view, "(()=>{const v=document.querySelector('video');return [v.videoWidth,v.videoHeight,v.getVideoPlaybackQuality().totalVideoFrames,!!v.error]})()")
        assert quality[:2] == ([320, 180] if mode == 'up' else [160, 90]) and quality[2] >= 280 and not quality[3], quality


def main():
    base = sys.argv[1]
    mode = os.environ.get('ZEBKIT_ADAPTIVE_MODE', 'down')
    if zebkit.init([sys.argv[0], *sys.argv[2:]], settings={'logging': {'destination': 'none'}}) != 0:
        print('FAIL zebkit.init()')
        return 1
    try:
        zebkit.media_keep_history(True)
        view = zebkit.View()
        with tempfile.TemporaryDirectory(prefix='zebkit-resolution-') as work:
            with part('the adaptive capture, mode=' + mode):
                run(view, base, mode, work)
    finally:
        check(zebkit.shutdown(), 'zebkit.shutdown()')
    return finish()


if __name__ == '__main__':
    sys.exit(main())
