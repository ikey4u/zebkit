"""Run with ZEBKIT_DIR, PYTHONPATH=bindings/python; no browser/HTTP server."""
import concurrent.futures
import ctypes
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import wave

from zebkit import Error
from zebkit.media_tools import Cancelled, extract_audio, extract_video, ffmpeg, ffprobe, probe, remux

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
        info = probe(source)
        assert info['format']['nb_streams'] == 2
        assert {s['codec_name'] for s in info['streams']} == {'h264', 'aac'}
        assert float(info['format']['duration']) == 8
        assert set(ffprobe(['-show_streams', '-of', 'json', '-i', source])) == {'streams'}
        assert set(ffprobe(['-show_format', '-i', source])) == {'format'}
        # Actual Unicode and shell metacharacters are filename bytes, never code.
        special = root / '媒体 "quoted";$(touch unexpected).mp4'
        special.write_bytes(source.read_bytes())
        assert probe(special)['format']['filename'] == str(special)
        print('ok probe JSON, section selection and literal Unicode paths')

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
        print('ok MP4/WebM stream copy, audio mapping and decoded output')

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
        print('ok PCM resampling, WAV/raw identity, RGBA scaling and decoder flush')

        original = digest(copied)
        fails(lambda: remux(source, copied))
        assert digest(copied) == original
        fails(lambda: remux(source, source, overwrite=True))
        alias = root / 'alias.mp4'
        os.link(special, alias)
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
            ['-i', source, '-c:a', 'copy', fresh],
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
        print('ok strict options, overwrite safety, cancellation and temporary cleanup')

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
        print('ok -ss and -t')

        def parallel(i):
            out = root / f'parallel-{i}.wav'
            extract_audio(source, out, sample_rate=8000 + i * 8000)
            return probe(source)['format']['nb_streams'], out.stat().st_size
        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
            results = list(pool.map(parallel, range(3)))
        assert all(streams == 2 and size > 1000 for streams, size in results)
        print('ok concurrent independent processing without engine initialization')
    print('media tools checks passed')


if __name__ == '__main__':
    main()
