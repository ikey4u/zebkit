"""Real MSE H.264 resolution switch: one avc3 file, decoded at both sizes."""
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

base = sys.argv[1]
mode = os.environ.get("ZEBKIT_ADAPTIVE_MODE", "down")
assert zebkit.init([sys.argv[0], *sys.argv[2:]], settings={"logging": {"destination": "none"}}) == 0
keep_history = zebkit._declare("zebkit_media_keep_history", None, ctypes.c_bool)
list_media = zebkit._declare("zebkit_view_media_list", None, zebkit._V, zebkit._RESULT, zebkit._P)
start = zebkit._declare("zebkit_media_capture_start", zebkit._P, zebkit._V, ctypes.c_char_p,
                       ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(zebkit._P))
stop = zebkit._declare("zebkit_media_capture_stop", None, zebkit._P)
free = zebkit._declare("zebkit_media_capture_free", None, zebkit._P)
status = zebkit._declare("zebkit_media_capture_status", zebkit._P, zebkit._P)
callback_type = ctypes.CFUNCTYPE(None, zebkit._P, ctypes.c_char_p, zebkit._P)
on_done = zebkit._declare("zebkit_media_capture_on_done", None, zebkit._P, callback_type, zebkit._P)
keep_history(True)
view = zebkit.View()
state = {"loaded": False, "done": None}
view.on_load(lambda event, url: state.update(loaded=True) if event == zebkit.LOAD_FINISHED and url.endswith('/media.html') else None)
view.load(base + '/media.html')


def wait(condition, timeout=15):
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


capture = None
try:
    with tempfile.TemporaryDirectory(prefix='zebkit-resolution-') as work:
        wait(lambda: state['loaded'])
        if mode == 'up':
            assert evaluate("(async()=>{const video=make('video');source=new MediaSource();video.src=URL.createObjectURL(source);await new Promise(r=>source.addEventListener('sourceopen',r,{once:true}));buffers=[source.addSourceBuffer(dash.reps[0].type)];await appendTo(buffers[0],'media/adaptive-small.mp4');return true})()") is True
        else:
            assert evaluate(('mse(8)' if mode == 'audio' else 'switched()') + '.then(()=>true)') is True
        wait(lambda: evaluate('ready()'))
        listed = []

        @zebkit._RESULT
        def media_list(handle, result, error, data):
            assert not error, error
            listed.extend(json.loads(result))

        list_media(view._check(), media_list, None)
        wait(lambda: bool(listed))
        error = zebkit._P()
        options = {'includeBuffered': True, 'format': 'mkv' if mode == 'mkv' else 'mp4'}
        if mode == 'clock':
            options['wallClock'] = {'mediaTime': 8.0, 'unixTime': 1700000008.25}
        capture = start(view._check(), listed[0]['id'].encode(), os.fsencode(Path(work) / 'adaptive.mp4'),
                        json.dumps(options).encode(), ctypes.byref(error))
        assert capture, zebkit._string(error.value) if error.value else 'capture rejected'

        @callback_type
        def done(handle, value, data):
            state['done'] = json.loads(value)

        on_done(capture, done, None)
        wait(lambda: json.loads(zebkit._string(status(capture)))['state'] == 'capturing')
        if mode == 'up':
            assert evaluate("(async()=>{buffers[0].timestampOffset=buffers[0].buffered.end(0);await appendTo(buffers[0],dash.dir+'init-0.m4s');for(let n=1;n<=8;n++)await appendTo(buffers[0],dash.dir+'chunk-0-'+String(n).padStart(3,'0')+'.m4s');source.endOfStream();return true})()") is True
        else:
            audio = "buffers[1].timestampOffset=8;for(let n=1;n<=4;n++)await appendTo(buffers[1],dash.dir+'chunk-1-'+String(n).padStart(3,'0')+'.m4s');" if mode == 'audio' else ''
            assert evaluate("(async()=>{buffers[0].timestampOffset=8;await appendTo(buffers[0],'media/adaptive-small.mp4');" + audio + "source.endOfStream();return true})()") is True
        assert evaluate('play(4)') is True
        wait(lambda: evaluate('state.ended'), timeout=20)
        assert not evaluate('state.error'), evaluate('state.error')
        stop(capture)
        wait(lambda: state['done'] is not None)
        result = state['done']
        assert not result.get('error'), result
        files = result['files']
        print('adaptive capture files:', json.dumps(files), flush=True)
        if mode == 'mkv':
            assert len(files) == 2, 'Matroska keeps incompatible sample configurations separate'
            probes = [json.loads(subprocess.check_output([os.environ['ZEBKIT_MEDIA_PROBE'], f['path']], text=True)) for f in files]
            assert all(p['ok'] and p['errors'] == 0 for p in probes), probes
            assert sum(p['videoFrames'] for p in probes) >= 280, probes
            assert sorted(p['width'] for p in probes) == [160, 320], probes
            print('ok adaptive explicit Matroska: preserves separate configurations and all frames', flush=True)
        else:
            assert len(files) == 1, 'a resolution change must keep one MP4'
            if mode == 'clock':
                assert 'wallClockStart' in files[0] and 'wallClockMapping' in files[0], 'capture must preserve the supplied live-stream time anchor'
                start_time = datetime.datetime.fromisoformat(files[0]['wallClockStart'].replace('Z', '+00:00')).timestamp()
                assert abs(start_time - (1700000000.25 + files[0]['start'])) < .00001, files[0]

            probe = json.loads(subprocess.check_output([os.environ['ZEBKIT_MEDIA_PROBE'], files[0]['path']], text=True))
            assert probe['ok'] and probe['errors'] == 0 and probe['videoFrames'] >= 280, probe
            assert (probe['minWidth'], probe['maxWidth'], probe['minHeight'], probe['maxHeight']) == (160, 320, 90, 180), probe
            assert probe['videoTag'] == int.from_bytes(b'avc3', 'little'), probe
            if mode == 'clock':
                assert json.loads(probe['wallClockMetadata']) == files[0]['wallClockMapping'], probe
                created = datetime.datetime.fromisoformat(probe['creationTime'].replace('Z', '+00:00')).timestamp()
                assert created == int(start_time), probe

            if mode == 'audio':
                assert probe['streams'] == 2 and probe['audio'] == 'aac' and probe['audioFrames'] >= 550, probe
            player = Path(work) / 'readback.html'
            player.write_text('<video muted src="' + Path(files[0]['path']).name + '"></video>')
            view.load(player.as_uri())
            wait(lambda: evaluate("location.protocol==='file:' && !!document.querySelector('video') && document.querySelector('video').readyState>=1"))
            assert evaluate("(()=>{const v=document.querySelector('video');v.playbackRate=4;return v.play().then(()=>true)})()") is True
            wait(lambda: evaluate("document.querySelector('video').ended"), timeout=20)
            quality = evaluate("(()=>{const v=document.querySelector('video');return [v.videoWidth,v.videoHeight,v.getVideoPlaybackQuality().totalVideoFrames,!!v.error]})()")
            assert quality[:2] == ([320, 180] if mode == 'up' else [160, 90]) and quality[2] >= 280 and not quality[3], quality
            print('ok adaptive H.264 mode=' + mode + ': one avc3 MP4, both resolutions decoded and replayed, all frames preserved', flush=True)

finally:
    if capture:
        free(capture)
    assert zebkit.shutdown()
