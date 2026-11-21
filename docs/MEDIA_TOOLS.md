# Native FFmpeg / ffprobe convenience functions

`<zebkit/media_tools.h>` provides synchronous functions for local media files
and live streams (RTMP, RTSP),
using the FFmpeg already inside zebkit. No external executable, shell, browser
view, engine initialization, or main loop is required. Call long operations on
an application worker thread. Independent calls with distinct output paths may
run concurrently. The existing `zebkit/vendor/ffmpeg.h` and Rust `vendor` APIs
remain available for custom pipelines, packet processing and custom I/O.

These functions implement a **documented subset** of the familiar CLI syntax,
not upstream `fftools` embedded in the process. zebkit's FFmpeg encodes H.264
(the engine's OpenH264, BSD) and AAC (FFmpeg's own encoder), reads and writes
FLV, and opens RTMP, RTMPT, RTMPS, RTMPTS, RTSP and RTSPS, the TLS ones on the
engine's BoringSSL (`scripts/ffmpeg-config.sh`, `patches/1090`). It has no
libavfilter and no other encoder. Unsupported switches/codecs fail explicitly;
there is no fallback to a system executable.

## Available operations

| Function / output mode | Behavior |
| --- | --- |
| `zebkit_media_probe(path, &json, &error)` | Container and stream metadata as JSON |
| `zebkit_ffprobe(argc, argv, &json, &error)` | Probe with section selection |
| `zebkit_ffmpeg(..., "-c", "copy", ...)` | Remux selected encoded streams without decoding/re-encoding |
| `... "-c:v", "h264", "-c:a", "aac", ...` | Encode video to H.264 and/or audio to AAC; the kind not named is copied |
| `... rtmp://... or rtsp://...` as input or output | Record a live stream, or publish to one (with `-re` from a file) |
| `... "-c:a", "pcm_s16le", ...` | Decode audio, resample/remix and write WAV or raw signed PCM16 little-endian |
| `... "-c:v", "rawvideo", ...` | Decode video, optionally scale, write packed RGBA frames |

Input is one local regular file or one live stream's `rtmp://`, `rtmps://`,
`rtmpt://`, `rtmpts://`, `rtsp://` or `rtsps://` address; the output is a file
or such an address. Other URLs (HTTP, HLS), pipes, multiple inputs/outputs,
concatenation, filter graphs and hardware frame surfaces are outside this
interface. For a page's file/MSE/HLS/WebRTC playback, use
[MediaTap](MEDIA_TAP.md); it can record a supported playback stream to a local
file for subsequent processing. The low-level API also permits custom I/O.

## C API

```c
#include <zebkit/media_tools.h>

char *json = NULL, *error = NULL;
if (zebkit_media_probe("input.mp4", &json, &error) == 0) {
    /* consume JSON */
}
zebkit_free(json);
zebkit_free(error);

const char *args[] = {"-i", "input.mp4", "-c", "copy", "output.mkv"};
error = NULL;
int status = zebkit_ffmpeg(5, args, NULL, NULL, &error);
/* status: 0 completed, 1 cancelled, -1 failed. */
zebkit_free(error);
```

Argument arrays **exclude the executable name**. Each argument is literal;
filenames with spaces/metacharacters need no shell quoting. `--` allows an
output path beginning with a hyphen. `-i PATH` allows such an input path.
Returned strings must be freed with `zebkit_free`. Initialize no engine state.

The optional progress callback receives `(packets, time_us, user_data)` on the
calling thread. It is called initially, during processing and before publication.
Return nonzero to continue or zero to cancel. Timestamp is `INT64_MIN` when
unknown; source packet timestamps need not be monotonic. Packet count includes
packets of skipped streams and is not an output frame count or percentage.
Cancellation is cooperative between packets, not an interrupt of a blocking
filesystem read. While FFmpeg waits on the network (a connect, a read of a live
stream) the callback is still called, about ten times a second, with time
`INT64_MIN`, and returning zero ends the wait. Callbacks must not unwind across
the C boundary.

File outputs are written to a sibling temporary file (a live stream's address
is written to as it is; `-y` and `-n` do not apply). On success the file is
atomically published. Existing outputs are preserved by default (`-n`); `-y`
allows atomic replacement. On error/cancellation, the temporary file is removed
and an existing destination is preserved. Input/output inode aliases are rejected.
New files inherit the private permissions of `mkstemp` (0600). Atomic publication
does not imply crash durability (`fsync` is not performed).

## Supported arguments

Probe:

```
[-show_format] [-show_streams] [-of json | -print_format json] [-i] INPUT
```

Without either `show` switch both sections are returned. This is a subset of
ffprobe's JSON schema: format filename/name/duration/start_time/size/bit_rate,
stream codec/type/time_base/duration/start_time/bit_rate, video dimensions,
pixel format and frame rate, audio sample rate/channel count, and metadata tags.
Times and potentially large values are strings; unknown times are JSON null.
This does not scan/count every decoded frame. Unsupported output formats or
`-show_packets`/`-show_frames`/`-show_entries` are rejected. Global log-level
switches are not accepted; the wrapper does not change FFmpeg process-wide logs.

Processing:

```
[-y | -n] [-re] [-rtsp_transport tcp|udp] [-rw_timeout MICROSECONDS] [-listen 0|1]
[-tls_verify 0|1] [-ca_file FILE] [-cert_file FILE] [-key_file FILE]
-i INPUT [-map 0:INDEX]... [-vn] [-an]
(-c copy | [-c:v copy|h264|libopenh264] [-c:a copy|aac] | -c:a pcm_s16le | -c:v rawvideo)
[-f FORMAT] [-ss SECONDS] [-t SECONDS] [-b:v BITS] [-b:a BITS] [-g FRAMES]
[-ar RATE] [-ac CHANNELS] [-s WIDTHxHEIGHT] [-frames:v COUNT] [-pix_fmt rgba] OUTPUT
```

Examples:

```
-i in.webm -c:v h264 -c:a aac out.mp4                     encode both
-i in.mp4 -c copy out.flv                                 H.264/AAC into FLV
-re -i in.mp4 -c copy -f flv rtmp://host/live/key         publish a file
-re -i in.webm -c:v h264 -b:v 2500k -c:a aac rtmps://host/live/key
-rtsp_transport tcp -i rtsp://camera/stream -t 60 -c copy clip.mp4
-i rtmp://host/live/key -c copy recording.flv             record until it ends
-re -i in.mp4 -c copy -rtsp_transport tcp rtsp://server:8554/live
-listen 1 -i rtmp://0.0.0.0:1935/live/key -c copy received.flv   be the server
-tls_verify 1 -ca_file ca.pem -re -i in.mp4 -c copy rtmps://host/live/key
```

- Encoding: `-c:v h264` (or `libopenh264`) and `-c:a aac`; the other kind, unnamed,
  is copied, and `-c copy` copies both. H.264 is YUV 4:2:0 at the source's size,
  or `-s` (even, 2..8192; an odd source loses its last row or column), 2 Mbit/s
  unless `-b:v`, a key frame every two seconds unless `-g`, no B-frames. AAC is
  64 kbit/s a channel (two at most) unless `-b:a`, at the source's rate rounded up
  to one AAC has (8000..96000) or `-ar`, with the source's channels (1..8) or `-ac`.
  `-b:v`/`-b:a` take bits a second, with `k` or `M`. A damaged packet of a live
  stream is passed over. Encoders and decoders are flushed at the end.
- Formats: `-f`, or from the output: `rtmp*://` is FLV, `rtsp*://` is RTSP, and
  for a file its extension (`.flv`, `.aac` for ADTS, `.mp4`, `.mov`, `.mkv`, `.webm`,
  `.m4a`). FLV carries H.264 and AAC; ADTS one AAC stream.
- Live streams: `-re` reads the input at its own pace, as a publisher sends a
  live stream; without it a file is sent as fast as it is read. A live input
  runs until its publisher stops, `-t`, or cancellation. `-rtsp_transport`
  chooses RTP over TCP or UDP for RTSP; `-rw_timeout` bounds each network wait
  (RTSP's own `timeout` too, never RTMP's, which would make it a server).
  `-listen 1` makes the input a server that one publisher connects to:
  `rtmp://` or `rtsp://` only, as FFmpeg serves no RTMPS or RTSPS (put a TLS
  proxy in front). Timestamps a live stream lacks or sends out of order are
  repaired when copying, as the ffmpeg command does. TLS: as FFmpeg's TLS by
  default, a server's certificate is not verified; `-tls_verify 1` verifies it
  and its name against `-ca_file` (PEM). `-cert_file` and `-key_file` (PEM)
  are a client certificate, for servers that ask for one. The callback can end
  any wait.

- `-ss` and `-t` are seconds. A copied stream begins at the key frame at or before the
  start, its times from zero, as `ffmpeg -ss -t -c copy`; decoded or encoded, exactly those
  frames are written. Nothing between those times fails.
- Copy defaults to all audio/video streams. Explicit maps select stream indices;
  duplicate or missing indices fail. Streams retain source order. Container and
  stream tags, codec parameters, dispositions and timestamps are copied.
  Chapters/programs are not copied. Muxer compatibility is enforced; copying
  H.264/AAC into WebM fails. Container support: MP4, MOV, Matroska, WebM, iPod/M4A,
  FLV, ADTS, and RTSP for an `rtsp://` output.
- Audio selects the first matching audio stream unless `-map` selects one.
  WAV is the default; `-f s16le` writes headerless PCM. `-ar` is 1..384000 Hz;
  `-ac` is 1..32 channels. Unspecified values use the source values. Output
  layout uses FFmpeg's default channel layout for that count. Changes to the
  decoded audio format fail explicitly. WAV is limited to the RIFF 4 GiB size;
  choose raw PCM for longer output. Decoder and resampler are flushed at EOF.
- Video selects the first matching video stream unless explicitly mapped.
  Output is RGBA8, top-down rows, `width * 4` bytes per row, no headers or padding.
  `-s` fixes/scales dimensions (1..8192 each); otherwise dimensions are fixed
  from the first decoded frame. Later source size changes are scaled to that
  output size. `-frames:v N` stops after at most N frames; EOF may arrive earlier.
  Source display rotation and HDR tone mapping are not applied. This extracts
  decoded frames, unlike MediaTap's sampled displayed frames.

## Python

```python
from zebkit.media_tools import probe, remux, extract_audio, extract_video, ffmpeg

info = probe("input.mp4")
remux("input.mp4", "output.mkv")
extract_audio("input.mp4", "speech.wav", sample_rate=16000, channels=1)
extract_video("input.mp4", "frames.rgba", size=(320, 180), frames=24)
ffmpeg(["-i", "input.mp4", "-map", "0:1", "-c", "copy", "audio.m4a"])
```

Set `overwrite=True` explicitly to replace an output. `remux(streams=[...])`
selects tracks, `extract_audio(raw=True)` writes raw PCM. All processing functions
accept `progress=lambda packets, time_us: True`; return False to raise `Cancelled`.
Other failures raise `zebkit.Error`. Callback exceptions are re-raised after native
cleanup. `ffprobe([...])` returns a dict; `probe(path)` returns both sections.

## Rust

```rust,no_run
use zebkit::media_tools;
let metadata_json = media_tools::probe("input.mp4")?;
media_tools::remux("input.mp4", "output.mkv", false)?;
media_tools::extract_audio("input.mp4", "speech.wav", 16000, 1, false)?;
media_tools::extract_video("input.mp4", "frames.rgba", 320, 180, 24, false)?;
media_tools::ffmpeg_with_progress(
    &["-i", "input.mp4", "-c", "copy", "another.mkv"],
    |packets, timestamp_us| packets < 1000,
)?;
# Ok::<(), zebkit::Error>(())
```

Processing returns `Result<Completion, Error>`; cancellation is
`Completion::Cancelled`. Callback panics are caught at the FFI boundary and
resumed after native cleanup. Probe returns JSON text without requiring a JSON
crate. The same API is available through `zebkit_rs` for raw FFI consumers.

## Verification

`tests/engine/media_tools.py` needs no HTTP server or engine loop:

```sh
ZEBKIT_DIR=out/blink/slim/package PYTHONPATH=bindings/python \
  python3 tests/engine/media_tools.py
```

Set `ZEBKIT_MEDIA_PROBE` to the built `tests/engine/media_probe.c` executable
for an independent decode check of remuxed output. The test covers real MP4/WebM,
stream selection, full decoder/resampler flushing, exact PCM samples, RGBA frame
sizes, Unicode paths, invalid parameters, cancellation, output preservation and
concurrent jobs. `scripts/engine-test.sh` includes this test.

Validated on Linux: the media-tools integration test, independent decode of
remuxed output, Rust example execution, Rust cancellation/panic-cleanup test,
and C header compilation all passed. The macOS media-tools object also
cross-compiled, and the integration test passes on a Mac (2026-10-31, `docs/MACOS.md`).

A runnable Rust entry-point example is
[`bindings/rust/zebkit/examples/media_tools.rs`](../bindings/rust/zebkit/examples/media_tools.rs).
