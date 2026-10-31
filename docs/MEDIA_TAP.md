# Native media output for applications

`<zebkit/media_tap.h>` provides a native, asynchronous session for encoded
packets, sampled video frames, PCM, and optional recording in one call. It
uses the media IDs returned by `<zebkit/media.h>`'s inventory, including
cross-site frames, shadow trees and detached audio elements. The inventory's
`tap` object reports `packets`, `recording`, `video`, `audio` and `active`.
Capabilities reflect the player's current state; starting can still fail if
the page navigates or changes its source.

## Playback and tap locations

* URL files use `FFmpegDemuxer`; MSE players (including script-driven DASH
  and HLS) use `ChunkDemuxer`; native HLS uses `ManifestDemuxer` with an
  internal chunk demuxer. Patch 1031's `DemuxerTap` supplies their original
  compressed buffers before decoding. Patch 1032 exposes the player to the
  embedder. The tap receives config before packets and on config changes.
* Decoded video uses `WebMediaPlayer::GetCurrentFrameThenUpdate()` and
  `Paint(..., force_pixel_readback=true)`. This accesses the media frame,
  independently of page overlays, element visibility, or CSS layout. It
  samples displayed frames up to `maxFps`, deduplicating unchanged frames;
  it does not guarantee every frame the decoder produces. Readback and RGBA
  conversion incur work; limits are applied before allocating the output.
* File/MSE/HLS PCM comes from an independent native observer on
  `WebAudioSourceProviderImpl::TeeFilter`. Patch 1047 keeps this separate
  from the page's `captureStream()` observer and WebAudio routing. The
  native observer receives the rendered PCM before page-facing CORS
  substitution and before element volume. It does not grant page script
  additional access to that data.
* MediaStream video uses the same player frame API. PCM attaches native
  `WebMediaStreamAudioSink`s to the element's current `srcObject` audio
  tracks. These are source-track samples, not mixed system/tab audio; the
  source can keep supplying them while its element is paused. The initial
  tracks are attached at start; stop/restart to pick up added/replaced tracks.
* Renderer data crosses Mojo to `media_tap.cc`. Application callbacks run
  on the engine thread. Recording forwards the same encoded packets to
  `CaptureWriter` on its own sequence. Existing file-only capture APIs and
  CDP `Zebkit.mediaCapture*` remain available.

| Source | Compressed packets | RGBA / PCM | Recording without re-encoding |
| --- | --- | --- | --- |
| URL file | Yes, for supported demuxers | Yes | Yes |
| MSE, including DASH/script HLS | Yes | Yes | Yes |
| Native HLS | Yes | Yes | Yes |
| MediaStream, including WebRTC/camera | Unavailable through this API | Yes, when the player exposes tracks/frames | Unavailable through this API |
| EME/protected media | Refused | Refused | Refused |

“Playable” does not imply an original demuxed packet stream is exposed:
MediaStream needs `packets:false` and no recording path. Applications can
encode its RGBA/PCM themselves if they need a file. The API does not add
codecs, fetch unplayed segments, extract keys, or bypass protected surfaces.

## C

```c
#include <zebkit/media.h>
#include <zebkit/media_tap.h>

static void output(zebkit_media_tap *tap, const char *json,
                   const uint8_t *bytes, size_t size, void *context) {
    /* Parse json; copy bytes before returning if processing asynchronously.
       A type:"done" event reports final files and any error. */
}

/* On the engine thread, after listing media and choosing its id: */
char *error = NULL;
zebkit_media_tap *tap = zebkit_media_tap_start(view, media_id,
    "{\"maxFps\":5,\"maxWidth\":640,\"maxHeight\":360,"
    "\"path\":\"/tmp/recording.mp4\"}", output, NULL, &error);
/* Check tap/error; release error with zebkit_free(error).
   Later: zebkit_media_tap_stop(tap), wait for done, then free(tap). */
```

`zebkit_media_tap_free()` stops and suppresses subsequent callbacks; it is
safe from inside a callback. A file still finalizing completes only while
the engine runs. `stop()` is idempotent and its `done` event is asynchronous.
A media ID must belong to the supplied view. No callback is called before
`start()` returns. Synchronous validation failures return NULL with an
allocated error; renderer failures arrive in `done.error`.

Only one tap per element is allowed. An encoded tap and an existing encoded
file-only capture conflict explicitly, without replacing one another.
Decoded-only taps can coexist with a file-only capture. Use `path` on a tap
to obtain packets, decoded output and a file together.

## Options

All options are strictly typed; unknown options are rejected.

| Name | Default | Meaning |
| --- | --- | --- |
| `packets`, `video`, `audio` | `true` | Enable callback outputs; absent tracks emit nothing |
| `maxFps` | 10 | Integer 1–60, maximum displayed-frame sampling rate |
| `maxWidth`, `maxHeight` | 1280, 720 | Integers 1–4096; fit within both bounds, preserve aspect ratio, never enlarge |
| `maxPendingBytes` | 16777216 | Integer 65536–67108864; shared outstanding packet/sample payload budget |
| `includeBuffered` | false | Include retained compressed history before the playhead |
| `path` | absent | Absolute recording path; enables internal compressed output even if `packets:false` |
| `recordingOptions` | `{}` | Existing capture options (`format`, `wallClock`, `closeGaps`, `maxBytes`, `maxDuration`, etc.); requires `path` |

At least one output is required. The default packet setting must be
explicitly disabled for MediaStream. File names, replacement semantics,
configuration changes, UTC mapping and capture ranges follow `media.h`.

## Callback events

The metadata is JSON, and the payload is a separate borrowed byte buffer.
There is no base64 conversion. Copy data you retain past the callback.

| `type` | Metadata | Payload |
| --- | --- | --- |
| `audioConfig` | `track`, `codec`, `sampleRate`, `channels`, Chromium `sampleFormat` | Codec extra data |
| `videoConfig` | `track`, `codec`, Chromium `profile`, coded `width`/`height`, `displayWidth`/`displayHeight` | Codec extra data |
| `packet` | `track`, `ptsUs`, `dtsUs`, `durationUs`, `keyframe` | Original compressed access unit |
| `video` | `format:"rgba8"`, `width`, `height`, `stride`, `ptsUs`, `captureTimeUs`, `droppedSamples` | Premultiplied RGBA8 rows |
| `audio` | `format:"f32-planar"`, `track`, `sampleRate`, `channels`, `frames`, `framesDelayed`, `captureTimeUs`, `droppedSamples` | Native-endian float32, channel planes concatenated |
| `timeline` | `unixZeroUs`, `fromMediaUs` | Empty |
| `progress` | Existing recording progress fields | Empty |
| `done` | `reason`, optional `error`, recording `files` when requested | Empty |

Packet timestamps are decimal strings so int64 values survive JSON exactly;
unknown is `-9223372036854775807`. They refer to the media timeline, may jump
on seek, and are not recording elapsed time. PCM track numbers identify the source audio tracks independently of the
encoded demuxer track IDs. PCM `captureTimeUs` is monotonic
sampling/playout time with an arbitrary origin, not UTC or media PTS.
`framesDelayed` is the renderer's playback delay for file audio (zero for
MediaStream). Video `ptsUs` comes from its frame; `captureTimeUs` is the
monotonic sampling time. Use timeline events or recording wall-clock metadata
only when the source actually provides a UTC mapping.

A stalled consumer never blocks playback waiting for a reply. Payload bytes
are reserved before copying/posting and released after the callback (and any
queued recording write). Decoded output is skipped under pressure and later
samples carry a cumulative `droppedSamples` count. Encoded overflow terminates
with an error rather than silently corrupting a compressed stream. Config
messages are limited to 256 per tap, each with at most 64 KiB extra data.
Processing inside an engine-thread callback should be short; transfer copied
data to a worker for model inference or other expensive work.

## Python and Rust

```python
from zebkit.media_tap import MediaTap, list_media

# list_media(view, callback): callback(media_list, error_or_none)
def consume(metadata, payload):
    # payload is an owned bytes copy; queue it for background processing.
    print(metadata["type"], len(payload))

tap = MediaTap(view, media_id, consume, maxFps=5,
               path="/tmp/recording.mp4")
tap.stop()
# Continue pumping until tap.done, then tap.close().
```

Python converts packet timestamp strings to integers. An exception in the
callback is kept as `tap.callback_error` and stops the session. Active taps
are retained until done or close so native callbacks cannot outlive them.

```rust
let tap = view.media_tap_start(media_id,
    r#"{"maxFps":5,"maxWidth":640,"maxHeight":360}"#,
    |json, bytes| {
        // Borrowed for this callback; bytes.to_vec() for a worker queue.
        println!("{json}: {} bytes", bytes.len());
    })?;
// Later: tap.stop(); retain tap until its done callback.
```

The Rust `MediaTap` handle is engine-thread-bound and RAII; dropping it
suppresses callbacks. The new API is native C/Rust/Python, not a new CDP or
page-JavaScript command. Existing CDP file recording is unchanged.

## Verification

`tests/engine/media_tap.py` exercises real MP4, WebM, MSE and native HLS
playback; config-before-packet ordering; RGBA/PCM data; file finalization;
invalid options; close inside a callback; view isolation and duplicate sessions;
bounded buffering; page captureStream coexistence; synthetic MediaStream and
real RTCPeerConnection loopback audio/video; and EME refusal. Run against a newly built package with a local HTTP server
serving `tests/engine/pages`, e.g.:

```sh
ZEBKIT_DIR=/path/to/package PYTHONPATH=bindings/python \
  python3 tests/engine/media_tap.py http://127.0.0.1:8000 \
  --no-sandbox --headless --autoplay-policy=no-user-gesture-required
```

Set `ZEBKIT_MEDIA_PROBE` to the built `tests/engine/media_probe.c` executable
to decode-check recordings as well. GPU readback and real-device WebRTC
need platform/device coverage beyond this synthetic integration test.

Linux verification passed for this integration test (including decode checks of
recorded files) and all 123 existing C media checks. Rust and example type checks
also passed. On a Mac (2026-10-31, `docs/MACOS.md`) it passes too, frames on the GPU read back,
but for WebRTC's loopback, skipped where the peers do not connect.
