"""zebkit.media_tools, the engine's FFmpeg as a library, without the engine
started: probe, remux, extraction, H.264/AAC encoding, FLV, RTMP, RTMPS and
RTSP over loopback, options, cancellation, concurrency.

    ZEBKIT_DIR=<package> ZEBKIT_MEDIA_PROBE=<media-probe> python3 tests/engine/media_tools.py
"""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import socket
import ssl
import struct
import subprocess
import tempfile
import threading
import time
import wave

import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import finish, part, zebkit  # noqa: E402,F401
from zebkit import Error  # noqa: E402
from zebkit.media_tools import Cancelled, extract_audio, extract_video, ffmpeg, ffprobe, probe, remux  # noqa: E402

FIXTURES = Path(__file__).resolve().parent / 'pages/media'


def fails(fn, kind=Error):
    try:
        fn()
    except kind:
        return
    raise AssertionError('expected failure')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    with tempfile.TemporaryDirectory(prefix='zebkit-media-tools-') as tmp:
        root = Path(tmp)
        source = FIXTURES / 'clip.mp4'
        with part('probe JSON, section selection and literal Unicode paths'):
            info = probe(source)
            assert info['format']['nb_streams'] == 2
            assert {s['codec_name'] for s in info['streams']} == {'h264', 'aac'}
            assert float(info['format']['duration']) == 8
            assert set(ffprobe(['-show_streams', '-of', 'json', '-i', source])) == {'streams'}
            assert set(ffprobe(['-show_format', '-i', source])) == {'format'}
            # Actual Unicode and shell metacharacters are filename bytes, never code.
            # What a shell would take apart; Windows' file names have no double quote.
            special = root / ("媒体 'quoted';$(touch unexpected).mp4" if os.name == 'nt' else '媒体 "quoted";$(touch unexpected).mp4')
            special.write_bytes(source.read_bytes())
            assert probe(special)['format']['filename'] == str(special)

        with part('MP4/WebM stream copy, audio mapping and decoded output'):
            copied = root / 'copy.mkv'
            progress = []
            remux(source, copied, progress=lambda packets, timestamp: progress.append((packets, timestamp)) or True)
            assert progress[0][0] == 0 and progress[-1][0] > 192
            assert probe(copied)['format']['nb_streams'] == 2
            if os.environ.get('ZEBKIT_MEDIA_PROBE'):
                decoded = json.loads(subprocess.check_output([os.environ['ZEBKIT_MEDIA_PROBE'], str(copied)]))
                assert decoded['errors'] == 0 and decoded['videoFrames'] == 192 and decoded['audioFrames'] > 0, decoded
            for name in ['clip.webm', 'clip-video.webm']:
                target = root / name
                remux(FIXTURES / name, target)
                assert probe(target)['format']['nb_streams'] >= 1
            mapped = root / 'audio.m4a'
            remux(source, mapped, streams=[1])
            assert [s['codec_type'] for s in probe(mapped)['streams']] == ['audio']

        with part('PCM resampling, WAV/raw identity, RGBA scaling and decoder flush'):
            wav = root / 'audio.wav'
            extract_audio(source, wav)
            with wave.open(str(wav)) as audio:
                assert (audio.getframerate(), audio.getnchannels(), audio.getsampwidth()) == (16000, 1, 2)
                assert audio.getnframes() == 128000, audio.getnframes()  # includes resampler flush
                data = audio.readframes(audio.getnframes())
                assert max(abs(v[0]) for v in struct.iter_unpack('<h', data)) > 100
            raw = root / 'audio.s16le'
            extract_audio(source, raw, raw=True)
            assert raw.read_bytes() == data
            video = root / 'two.rgba'
            extract_video(source, video, size=(80, 45), frames=2)
            pixels = video.read_bytes()
            assert len(pixels) == 80 * 45 * 4 * 2
            assert all(x == 255 for x in pixels[3::4]) and len(set(pixels[::4])) > 10
            full_video = root / 'all.rgba'
            extract_video(source, full_video, size=(16, 9))
            assert full_video.stat().st_size == 192 * 16 * 9 * 4  # decoder delayed-frame flush

        with part('strict options, overwrite safety, cancellation and temporary cleanup'):
            original = digest(copied)
            fails(lambda: remux(source, copied))
            assert digest(copied) == original
            fails(lambda: remux(source, source, overwrite=True))
            alias = root / 'alias.mp4'
            try:
                os.link(special, alias)
            except OSError:
                pass  # A file system without hard links: a VM's shared folder.
            else:
                fails(lambda: remux(special, alias, overwrite=True))
            fails(lambda: remux(source, copied, overwrite=True, progress=lambda n, t: n < 8), Cancelled)
            assert digest(copied) == original
            fresh = root / 'cancelled.mkv'
            fails(lambda: remux(source, fresh, progress=lambda n, t: n < 8), Cancelled)
            assert not fresh.exists()
            def bad_callback(*_):
                raise RuntimeError('callback failed')
            fails(lambda: remux(source, fresh, progress=bad_callback), RuntimeError)
            assert not fresh.exists()
            for args in [
                ['-i', source, '-c', 'libx264', fresh],
                ['-i', source, '-c', 'copy', '-vf', 'scale=80:45', fresh],
                ['-i', source, '-c', 'copy', '-pix_fmt', 'rgba', fresh],
                ['-i', source, '-c', 'copy', '-map', '0:999', fresh],
                ['-i', source, '-c', 'copy', '-map', '0:1', '-map', '0:1', fresh],
                ['-i', source, '-c:a', 'pcm_s16le', '-ac', '0', fresh],
                ['-i', source, '-c:v', 'rawvideo', '-s', '99999x2', fresh],
                ['-i', source, '-c', 'copy', '-f', 'webm', fresh],  # incompatible codecs
                ['-i', source, '-c:a', 'mp3', fresh],
                ['-i', source, '-c:v', 'h264', '-s', '81x45', fresh],  # odd width
                ['-i', source, '-c', 'copy', '-b:v', '1M', fresh],  # bit rate without an encoder
                ['-i', source, '-c', 'copy', 'rtmp://127.0.0.1:1/live/x.mp4', '-f', 'mp4'],
                ['-i'],
            ]:
                fails(lambda args=args: ffmpeg(args))
                assert not fresh.exists()
            fails(lambda: ffmpeg(['-i', 'x\0y', '-c', 'copy', fresh]), ValueError)
            fails(lambda: ffmpeg('-i input -c copy output'), TypeError)
            fails(lambda: ffprobe(['-show_packets', str(source)]))
            invalid = root / 'invalid.mp4'
            invalid.write_bytes(b'not media')
            fails(lambda: probe(invalid))
            fails(lambda: probe(root / 'missing.mp4'))
            assert not list(root.glob('*.zebkit-*'))
            remux(source, copied, overwrite=True)

        with part('-ss and -t'):
            # -ss and -t: the copy from the key frame at or before the start, its times
            # from zero; decoded audio exactly the part asked for.
            whole = float(probe(source)['format']['duration'])
            cut = root / 'cut.mp4'
            ffmpeg(['-i', str(source), '-ss', '%.3f' % (whole / 3), '-t', '%.3f' % (whole / 3), '-c', 'copy', str(cut)])
            duration = float(probe(cut)['format']['duration'])
            assert 0 < duration < whole * 0.85, (duration, whole)
            audio_whole, audio_part = root / 'all.wav', root / 'part.wav'
            ffmpeg(['-i', str(source), '-c:a', 'pcm_s16le', str(audio_whole)])
            ffmpeg(['-i', str(source), '-ss', '%.3f' % (whole / 2), '-t', '%.3f' % (whole / 4), '-c:a', 'pcm_s16le', str(audio_part)])
            assert audio_part.stat().st_size < audio_whole.stat().st_size * 0.4, (audio_part.stat().st_size, audio_whole.stat().st_size)
            for bad in (['-ss', 'x'], ['-t', '0'], ['-t', '-1'], ['-ss', '1e400']):
                fails(lambda bad=bad: ffmpeg(['-i', str(source), *bad, '-c', 'copy', str(root / 'never.mp4')]))
            fails(lambda: ffmpeg(['-i', str(source), '-ss', '%.3f' % (whole * 100), '-t', '1', '-c', 'copy', str(root / 'none.mp4')]))
            assert not (root / 'none.mp4').exists()

        with part('H.264 and AAC encoding, FLV and ADTS'):
            encoded = root / 'encoded.mp4'
            ffmpeg(['-i', str(FIXTURES / 'clip.webm'), '-c:v', 'h264', '-b:v', '500k', '-g', '24', '-c:a', 'aac', str(encoded)])
            streams = {s['codec_type']: s for s in probe(encoded)['streams']}
            assert streams['video']['codec_name'] == 'h264' and streams['audio']['codec_name'] == 'aac', streams
            frames = root / 'encoded.rgba'
            extract_video(encoded, frames, size=(16, 9))
            source_frames = root / 'source.rgba'
            extract_video(FIXTURES / 'clip.webm', source_frames, size=(16, 9))
            assert frames.stat().st_size == source_frames.stat().st_size, (frames.stat().st_size, source_frames.stat().st_size)
            # One kind encoded, the other copied; a size of its own.
            small = root / 'small.mkv'
            ffmpeg(['-i', str(source), '-c:v', 'h264', '-s', '160x90', str(small)])
            video = next(s for s in probe(small)['streams'] if s['codec_type'] == 'video')
            assert (video['width'], video['height'], video['codec_name']) == (160, 90, 'h264'), video
            flv = root / 'copy.flv'
            ffmpeg(['-i', str(source), '-c', 'copy', str(flv)])
            info = probe(flv)
            assert info['format']['format_name'] == 'flv' and {s['codec_name'] for s in info['streams']} == {'h264', 'aac'}
            adts = root / 'speech.aac'
            ffmpeg(['-i', str(FIXTURES / 'clip.webm'), '-vn', '-c:a', 'aac', '-ar', '16000', '-ac', '1', str(adts)])
            audio = probe(adts)['streams'][0]
            assert (audio['codec_name'], audio['sample_rate'], audio['channels']) == ('aac', '16000', 1), audio

        with part('RTMP, RTMPS and RTSP: a stream published and received over loopback'):
            def port():
                with socket.socket() as s:
                    s.bind(('127.0.0.1', 0))
                    return s.getsockname()[1]

            def relay(listen, publish, received):
                """Receives with -listen 1 on one thread while another publishes."""
                outcome = {}
                def receive():
                    try:
                        ffmpeg(['-listen', '1', '-rw_timeout', '30000000', *listen, '-c', 'copy', str(received)])
                        outcome['ok'] = True
                    except Exception as e:  # noqa: BLE001
                        outcome['error'] = e
                thread = threading.Thread(target=receive)
                thread.start()
                # The listener binds after its thread starts; a publisher before
                # it is refused, and tries again.
                for attempt in range(50):
                    try:
                        ffmpeg(['-re', '-t', '3', '-i', str(source), '-c', 'copy', *publish])
                        break
                    except Error:
                        if attempt == 49 or not thread.is_alive():
                            raise
                        time.sleep(0.2)
                thread.join(60)
                assert not thread.is_alive() and outcome.get('ok'), outcome
                info = probe(received)
                assert {s['codec_name'] for s in info['streams']} == {'h264', 'aac'}, info
                assert 1.5 < float(info['format']['duration']) < 4.5, info['format']

            rtmp = port()
            relay(['-i', f'rtmp://127.0.0.1:{rtmp}/live/test'], [f'rtmp://127.0.0.1:{rtmp}/live/test'], root / 'rtmp.flv')
            # RTMPS: FFmpeg serves plain RTMP only, so a TLS proxy of Python's
            # in front of it; the publisher speaks TLS on BoringSSL and
            # verifies the proxy's certificate.
            key, cert = root / 'key.pem', root / 'cert.pem'
            stranger_key, stranger = root / 'stranger-key.pem', root / 'stranger.pem'
            for k, c in ((key, cert), (stranger_key, stranger)):
                subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1', '-subj', '/CN=127.0.0.1',
                                '-addext', 'subjectAltName=IP:127.0.0.1', '-keyout', str(k), '-out', str(c)],
                               check=True, capture_output=True)

            def tls_proxy(backend):
                context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                context.load_cert_chain(str(cert), str(key))
                server = socket.socket()
                server.bind(('127.0.0.1', 0))
                server.listen()

                def pump(a, b):
                    try:
                        while data := a.recv(65536):
                            b.sendall(data)
                    except OSError:
                        pass
                    for s in (a, b):
                        try:
                            s.shutdown(socket.SHUT_RDWR)
                        except OSError:
                            pass

                def serve():
                    while True:
                        try:
                            raw, _ = server.accept()
                        except OSError:
                            return
                        try:
                            client = context.wrap_socket(raw, server_side=True)
                        except (ssl.SSLError, OSError):
                            raw.close()  # a client that refused the certificate
                            continue
                        try:
                            upstream = socket.create_connection(('127.0.0.1', backend))
                        except OSError:
                            client.close()
                            continue
                        for x, y in ((client, upstream), (upstream, client)):
                            threading.Thread(target=pump, args=(x, y), daemon=True).start()
                threading.Thread(target=serve, daemon=True).start()
                return server

            backend = port()
            proxy = tls_proxy(backend)
            front = proxy.getsockname()[1]
            relay(['-i', f'rtmp://127.0.0.1:{backend}/live/test'],
                  ['-tls_verify', '1', '-ca_file', str(cert), f'rtmps://127.0.0.1:{front}/live/test'], root / 'rtmps.flv')
            # A certificate it cannot verify: refused before any RTMP.
            fails(lambda: ffmpeg(['-tls_verify', '1', '-ca_file', str(stranger), '-rw_timeout', '5000000', '-t', '1',
                                  '-i', str(source), '-c', 'copy', f'rtmps://127.0.0.1:{front}/live/test']))
            proxy.close()
            # FFmpeg has no RTMPS or RTSPS server.
            fails(lambda: ffmpeg(['-listen', '1', '-i', f'rtmps://127.0.0.1:{port()}/live/x', '-c', 'copy', str(root / 'never.flv')]))
            rtsp = port()
            relay(['-rtsp_transport', 'tcp', '-i', f'rtsp://127.0.0.1:{rtsp}/live'],
                  ['-rtsp_transport', 'tcp', f'rtsp://127.0.0.1:{rtsp}/live'], root / 'rtsp.mkv')
            # A listener waiting for a publisher who never comes is cancelled
            # through FFmpeg's interrupt callback, inside its wait.
            started = time.monotonic()
            fails(lambda: ffmpeg(['-listen', '1', '-i', f'rtmp://127.0.0.1:{port()}/live/none', '-c', 'copy', str(root / 'never.flv')],
                                 progress=lambda n, t: time.monotonic() - started < 1), Cancelled)
            assert time.monotonic() - started < 10
            assert not (root / 'never.flv').exists()
            # Nobody listening: a publisher fails rather than waits.
            fails(lambda: ffmpeg(['-rw_timeout', '2000000', '-i', str(source), '-c', 'copy', f'rtmp://127.0.0.1:{port()}/live/x']))

        with part('concurrent independent processing without engine initialization'):
            def parallel(i):
                out = root / f'parallel-{i}.wav'
                extract_audio(source, out, sample_rate=8000 + i * 8000)
                return probe(source)['format']['nb_streams'], out.stat().st_size
            with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
                results = list(pool.map(parallel, range(3)))
            assert all(streams == 2 and size > 1000 for streams, size in results)
    return finish()


if __name__ == '__main__':
    sys.exit(main())
