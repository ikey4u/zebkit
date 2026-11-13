"""Native media tap integration (zebkit.media_tap): encoded packets, RGBA,
PCM and files of what a page plays, its options, its lifetimes and its limits.

    python3 tests/engine/media_tap.py <base URL of tests/engine/pages> [engine switches...]

ZEBKIT_MEDIA_PROBE, when set, names the media_probe program each file written
is read back with. Exit 0 when every check passes.
"""
import array
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import check, finish, must_wait, part, skip, wait, zebkit  # noqa: E402
from zebkit.media_tap import MediaTap, list_media  # noqa: E402

base = sys.argv.pop(1)
view = None


def until(predicate, what='native media output', timeout=15):
    must_wait(predicate, what, timeout)


def evaluate(code):
    result = []
    view.evaluate(code, lambda value, error: result.append((value, error)))
    until(lambda: result, 'an answer to ' + code[:60])
    value, error = result[0]
    assert not error, error
    return json.loads(value) if isinstance(value, str) else value


def inventory():
    result = []
    list_media(view, lambda media, error: result.append((media, error)))
    until(lambda: result, 'the media list')
    assert not result[0][1], result
    return result[0][0]


def main():
    global view
    if zebkit.init() != 0:
        print('FAIL zebkit.init()')
        return 1
    try:
        view = zebkit.View()
        loaded = []
        view.on_load(lambda event, url: loaded.append(True) if event == zebkit.LOAD_FINISHED and url.endswith('/media.html') else None)
        view.load(base + '/media.html')
        if check(wait(lambda: loaded, 15), 'the media page loaded'):
            with tempfile.TemporaryDirectory(prefix='zebkit-media-tap-') as directory:
                taps(directory)
    finally:
        check(zebkit.shutdown(), 'shutdown')
    return finish()


def taps(directory):
    for mode, setup in [
        ('mp4', 'file("video", "media/clip.mp4")'),
        ('webm', 'file("video", "media/clip.webm")'),
        ('mse', 'mse(3)'),
        ('hls', 'file("video", "media/tap/master.m3u8")'),
    ]:
        with part(mode + ': encoded packets, RGBA, nonzero PCM and file'):
            evaluate(setup)
            until(lambda: evaluate('ready()'))
            media_id = inventory()[0]['id']
            events, configs = [], set()
            nonzero_pcm = [False]
            frames = []

            def received(meta, data):
                kind = meta['type']
                events.append(meta)
                if kind in ('audioConfig', 'videoConfig'):
                    configs.add(meta['track'])
                if kind == 'packet':
                    assert meta['track'] in configs, 'config must precede packet'
                    assert data
                    assert isinstance(meta['ptsUs'], int)
                elif kind == 'audio':
                    assert len(data) == meta['channels'] * meta['frames'] * 4
                    samples = array.array('f'); samples.frombytes(data)
                    nonzero_pcm[0] |= any(abs(x) > .0001 for x in samples)
                    assert meta['sampleRate'] > 0
                elif kind == 'video':
                    assert meta['width'] <= 160 and meta['height'] <= 100
                    assert len(data) == meta['stride'] * meta['height']
                    assert any(data[3::4]), 'frame must contain painted pixels'
                    frames.append(data)

            tap = MediaTap(view, media_id, received, maxFps=12, maxWidth=160, maxHeight=100,
                           path=str(Path(directory) / (mode + '.mp4')))
            if mode == 'mse':
                evaluate('more()')
            assert evaluate('play()') is True
            try:
                until(lambda: tap.done or (len(frames) >= 3 and nonzero_pcm[0] and any(e['type'] == 'packet' for e in events)))
            except AssertionError:
                from collections import Counter
                print('diagnostic', mode, Counter(e['type'] for e in events),
                      'frames', len(frames), 'pcm', nonzero_pcm, 'callback', repr(tap.callback_error),
                      'done', tap.done, 'player', inventory(), flush=True)
                raise
            assert tap.callback_error is None, tap.callback_error
            assert not tap.done, tap.done
            tap.stop()
            until(lambda: tap.done)
            assert 'error' not in tap.done, tap.done
            assert tap.done['files'], tap.done
            for output in tap.done['files']:
                assert Path(output['path']).stat().st_size > 0
                if os.environ.get('ZEBKIT_MEDIA_PROBE'):
                    result = subprocess.run([os.environ['ZEBKIT_MEDIA_PROBE'], output['path']], capture_output=True, text=True)
                    assert result.returncode == 0, result.stderr
                    probe = json.loads(result.stdout)
                    assert probe['errors'] == 0 and probe['videoFrames'] > 0, probe
                    assert probe['audioFrames'] > 0, probe
            tap.close()

    with part('strict options'):
        evaluate('file("video", "media/clip.mp4")')
        until(lambda: evaluate('ready()'))
        media_id = inventory()[0]['id']
        for bad in ({'maxFps': 0}, {'maxWidth': 99999}, {'audio': 1}, {'maxPendingBytes': 1},
                    {'packets': False, 'video': False, 'audio': False}, {'path': 'relative.mp4'}, {'path': '/tmp/tap\0.mp4'}, {'typo': True}):
            try:
                MediaTap(view, media_id, lambda *_: None, **bad)
            except zebkit.Error:
                pass
            else:
                raise AssertionError(('invalid options accepted', bad))

    with part('close inside callback'):
        # A tap may close itself inside a data callback; no later callbacks occur.
        callbacks = []
        holder = []
        def close_inside(meta, data):
            callbacks.append(meta['type'])
            holder[0].close()
        holder.append(MediaTap(view, media_id, close_inside))
        evaluate('play()')
        until(lambda: callbacks)
        count = len(callbacks)
        deadline = time.monotonic() + .2
        until(lambda: time.monotonic() > deadline)
        assert len(callbacks) == count == 1, callbacks

    with part('immediate stop and free inside done callback'):
        # Stopping immediately and freeing from the done callback must not double-delete.
        holder = []
        def free_when_done(meta, data):
            if meta['type'] == 'done':
                holder[0].close()
        holder.append(MediaTap(view, media_id, free_when_done, packets=False, audio=False))
        holder[0].stop()
        until(lambda: holder[0].done)
        assert holder[0].callback_error is None

    with part('view isolation and conflicting tap leaves first intact'):
        # IDs are scoped to their owning view.
        other = zebkit.View()
        try:
            MediaTap(other, media_id, lambda *_: None)
        except zebkit.Error:
            pass
        else:
            raise AssertionError('cross-view media id accepted')
        other.close()

        evaluate('file("video", "media/clip.mp4")')
        until(lambda: evaluate('ready()'))
        media_id = inventory()[0]['id']
        first = MediaTap(view, media_id, lambda *_: None)
        duplicate = MediaTap(view, media_id, lambda *_: None)
        until(lambda: duplicate.done)
        assert 'conflicting' in duplicate.done.get('error', ''), duplicate.done
        duplicate.close()
        assert not first.done, first.done
        first.stop(); until(lambda: first.done); first.close()

    with part('decoded backpressure skips frames and retains PCM'):
        # Oversized decoded frames are skipped, while small PCM keeps flowing.
        evaluate('file("video", "media/clip.mp4")')
        until(lambda: evaluate('ready()'))
        events = []
        tap = MediaTap(view, inventory()[0]['id'], lambda m, d: events.append(m),
                       packets=False, maxPendingBytes=65536)
        evaluate('play()')
        until(lambda: tap.done or any(m.get('droppedSamples', 0) > 0 for m in events))
        assert not tap.done, tap.done
        assert any(m['type'] == 'audio' for m in events)
        tap.stop(); until(lambda: tap.done); tap.close()

    with part('encoded overflow ends only the tap, playback continues'):
        # Encoded overflow must be explicit, never silently drop compressed data.
        evaluate('mse(8)')
        until(lambda: evaluate('ready()'))
        tap = MediaTap(view, inventory()[0]['id'], lambda *_: None,
                       video=False, audio=False, includeBuffered=True, maxPendingBytes=65536)
        until(lambda: tap.done)
        assert 'maxPendingBytes' in tap.done.get('error', ''), tap.done
        tap.close()
        assert evaluate('play()') is True
        until(lambda: evaluate('time() > .1'))

    with part('source replacement terminates tap'):
        evaluate('file("video", "media/clip.mp4")')
        until(lambda: evaluate('ready()'))
        tap = MediaTap(view, inventory()[0]['id'], lambda *_: None, packets=False)
        evaluate('file("video", "media/clip.webm")')
        until(lambda: tap.done)
        assert tap.done['reason'] == 'the element loaded other media', tap.done
        tap.close()

    with part('native PCM coexists with page captureStream and preserves it after stop'):
        # Native PCM must coexist with the page's captureStream and leave its
        # render-muted request active after the native tap stops.
        evaluate('file("video", "media/clip.mp4")')
        until(lambda: evaluate('ready()'))
        evaluate('''(async () => {
          window.pageCaptureContext = new AudioContext(); await pageCaptureContext.resume();
          window.pageCapture = media.captureStream();
          window.pageAnalyser = pageCaptureContext.createAnalyser();
          window.pageSource = pageCaptureContext.createMediaStreamSource(pageCapture);
          const quiet = pageCaptureContext.createGain(); quiet.gain.value = 0;
          pageSource.connect(pageAnalyser); pageAnalyser.connect(quiet); quiet.connect(pageCaptureContext.destination);
          return await play();
        })()''')
        events = []
        tap = MediaTap(view, inventory()[0]['id'], lambda m, d: events.append(m), packets=False, video=False)
        until(lambda: tap.done or any(m['type'] == 'audio' for m in events))
        assert not tap.done, tap.done
        tap.stop(); until(lambda: tap.done); tap.close()
        until(lambda: evaluate('(() => { const b=new Uint8Array(pageAnalyser.fftSize); pageAnalyser.getByteTimeDomainData(b); return b.some(v=>v!==128); })()'))
        evaluate('pageCapture.getTracks().forEach(t=>t.stop()); pageCaptureContext.close(); true')

    with part('MediaStream RGBA and PCM, explicit encoded-output refusal'):
        # Canvas video and WebAudio PCM delivered through a srcObject stream.
        evaluate('''(async () => {
          reset();
          const canvas = document.createElement('canvas'); canvas.width=64; canvas.height=48;
          const ctx = canvas.getContext('2d'); let n=0;
          window.paintTimer = setInterval(() => {ctx.fillStyle=++n%2?'red':'blue'; ctx.fillRect(0,0,64,48)}, 30);
          window.ac = new AudioContext(); await ac.resume();
          const oscillator = ac.createOscillator(); const destination = ac.createMediaStreamDestination();
          oscillator.connect(destination); oscillator.start();
          const stream = window.nativeTestStream = canvas.captureStream(20);
          stream.addTrack(destination.stream.getAudioTracks()[0]);
          make('video').srcObject = stream;
          return await play();
        })()''')
        until(lambda: evaluate('ready()'))
        media_id = inventory()[0]['id']
        refused = MediaTap(view, media_id, lambda *_: None)
        until(lambda: refused.done)
        assert 'demuxed packets' in refused.done.get('error', ''), refused.done
        refused.close()
        events = []
        tap = MediaTap(view, media_id, lambda meta, data: events.append((meta, data)), packets=False)
        until(lambda: tap.done or {'video', 'audio'} <= {m['type'] for m, _ in events})
        assert not tap.done, tap.done
        assert any(any(payload) for meta, payload in events if meta['type'] == 'audio')
        tap.stop(); until(lambda: tap.done); tap.close()
        # Send the same tracks through a real RTCPeerConnection pair and tap
        # the receiver's decoded stream; no external network or devices are used.
        evaluate('''(async () => {
          window.pcA = new RTCPeerConnection({iceServers:[]});
          window.pcB = new RTCPeerConnection({iceServers:[]});
          make('video');
          pcB.ontrack = e => { media.srcObject = e.streams[0]; media.play(); };
          nativeTestStream.getTracks().forEach(t => pcA.addTrack(t, nativeTestStream));
          const gather = pc => pc.iceGatheringState === 'complete' ? Promise.resolve() : new Promise(resolve => {
            pc.addEventListener('icegatheringstatechange', () => { if(pc.iceGatheringState==='complete') resolve(); });
          });
          await pcA.setLocalDescription(await pcA.createOffer()); await gather(pcA);
          await pcB.setRemoteDescription(pcA.localDescription);
          await pcB.setLocalDescription(await pcB.createAnswer()); await gather(pcB);
          await pcA.setRemoteDescription(pcB.localDescription);
          return true;
        })()''')
        # On a Mac, peers in an unbundled program without the Local Network grant may never
        # connect (mDNS names unresolved, checks unanswered), which a person has to grant.
        connected = False
        try:
            until(lambda: evaluate("pcA.iceConnectionState === 'connected' || pcA.iceConnectionState === 'completed'"))
            connected = True
        except AssertionError:
            if sys.platform != 'darwin':
                raise
            skip('WebRTC loopback', 'the peers did not connect, state '
                 + evaluate('JSON.stringify(pcA.iceConnectionState)') + ' (macOS Local Network permission?)')
        if connected:
            with part('WebRTC loopback decoded RGBA and PCM'):
                until(lambda: evaluate('ready()'))
                events = []
                tap = MediaTap(view, inventory()[0]['id'], lambda m, d: events.append((m, d)), packets=False)
                until(lambda: tap.done or {'video', 'audio'} <= {m['type'] for m, _ in events})
                assert not tap.done, tap.done
                assert any(any(payload) for meta, payload in events if meta['type'] == 'audio')
                tap.stop(); until(lambda: tap.done); tap.close()
        evaluate('pcA.close(); pcB.close(); nativeTestStream.getTracks().forEach(t=>t.stop()); clearInterval(paintTimer); ac.close(); reset(); true')

    with part('EME rejected without data'):
        evaluate('encrypted().then(() => ({ok:true}), e => ({error:String(e), appended:state.appended, encrypted:state.encrypted}))')
        # Missing keys may reject append, but the player must still expose its encrypted state.
        until(lambda: any(entry.get('encrypted') for entry in inventory()))
        media_id = next(entry['id'] for entry in inventory() if entry.get('encrypted'))
        events = []
        tap = MediaTap(view, media_id, lambda m, d: events.append(m), packets=False)
        until(lambda: tap.done)
        assert 'encrypted' in tap.done.get('error', ''), tap.done
        assert not any(m['type'] in ('audio', 'video', 'packet') for m in events)
        tap.close()


if __name__ == '__main__':
    sys.exit(main())
