# Libraries inside the engine, for applications

zebkit has whole libraries linked into it. An application that needs one of
them, and ships its own, ships it twice. The ones listed here can be used
from the engine instead: the application includes one header, links against
`libzebkit.so.1` and nothing else, and calls the library's ordinary API.

| Library | Version | C++ namespace | Header |
|---|---|---|---|
| FFmpeg | 8.1.3 | `zebkit::vendor::ffmpeg` | `<zebkit/vendor/ffmpeg.h>` |

`docs/DEPS.md` lists everything else that is in the engine. Each of those
could be exposed the same way; FFmpeg is the first.

## Using it

```cpp
#include <zebkit/vendor/ffmpeg.h>

namespace ff = zebkit::vendor::ffmpeg;

const ff::AVCodec* codec = ff::avcodec_find_decoder(ff::AV_CODEC_ID_H264);
ff::AVCodecContext* context = ff::avcodec_alloc_context3(codec);
ff::avcodec_open2(context, codec, nullptr);
```

```sh
c++ app.cpp -I<package>/include -L<package> -l:libzebkit.so.1 -Wl,-rpath,'$ORIGIN'
```

- **C++:** types, functions and enumerators are in the namespace. The
  library's macros (`AVERROR`, `AV_NOPTS_VALUE`, `AV_VERSION_MAJOR`) are
  macros and so are global, as are the inline functions' names inside the
  namespace (`ff::av_q2d`).
- **C:** include the same header; the API has the names FFmpeg gives it
  (`avcodec_open2`).
- **The library's own headers** are in `include/zebkit/vendor/ffmpeg/`.
  The one header above includes the usual ones. A header it leaves out,
  such as one for a platform's hardware decoding, can be included from
  there, inside `namespace zebkit::vendor::ffmpeg { extern "C" { ... } }`
  in C++.

`examples/ffmpeg.cpp` is a complete program: it decodes an MP3 file to a
WAV file. `mise example:ffmpeg` builds and runs it.

## Why it cannot collide with another FFmpeg

The engine does not export `avcodec_open2`. It exports
`zebkit_vendor_ffmpeg_avcodec_open2`, and the header tells the compiler that
this is what `avcodec_open2` is called at link time. So:

- An application that also links a system FFmpeg, or loads a plugin that
  does, has two FFmpegs with different symbol names. Each caller gets the
  one it was compiled against.
- The engine's own use of FFmpeg, for `<video>` and `<audio>`, is not
  affected by anything an application does, and the other way round, with
  one exception: FFmpeg's process-wide settings, of which the log level and
  log callback (`av_log_set_level`, `av_log_set_callback`) are the ones
  that matter. They are shared with the engine.

Objects are not interchangeable between the two: an `AVFrame` from the
engine's FFmpeg must not be handed to another FFmpeg, whose version and
build options may differ.

## What is in it

Exactly what the engine was built with (`deps/ffmpeg/recipe.sh`): what
playing media in a web page needs, plus what is cheap to add for
applications and needs no other library.

| Part | What is there |
|---|---|
| libavcodec, decoders | H.264, VP8, VP9, AAC, MP3, Opus, Vorbis, FLAC, PCM, Motion JPEG, with their parsers |
| libavcodec, encoders | AAC, FLAC, PCM (16-bit) and Motion JPEG: the ones FFmpeg has of its own |
| libavformat, reading | MP4/MOV, WebM/Matroska, Ogg, WAV, bare MP3, AAC and FLAC streams, image files |
| libavformat, writing | MP4, MOV, WebM, Matroska, Ogg, WAV, ADTS (AAC), FLAC, image files |
| libavformat, protocols | Local files only. No network |
| libswscale | Complete: scaling and pixel format conversion |
| libswresample | Complete: sample format, rate and channel conversion |
| libavutil | Complete |
| libavfilter, libavdevice | Not built |

Not there, and not a line in the recipe away: **encoders for H.264, VP8,
VP9, Opus and MP3.** FFmpeg has none built in; they are the separate
libraries x264 (GPL), libvpx, libopus and LAME, each of which would have to
be built and linked in. An application can write video only as Motion JPEG
with this build.

Other decoders, demuxers and muxers that FFmpeg has built in are a line in
the recipe; the header and the entry points follow by themselves.

938 functions are exposed. Data symbols are not; FFmpeg's public API has
almost none.

## Cost

Exposing the API means every function of it is linked in, not only what the
engine calls, and the parts built for applications are extra code. For
FFmpeg that is 1.8 MiB in all: 0.3 MiB for exporting the API the engine
already had, and 1.5 MiB for file reading and writing, scaling and the
encoders (the package went from 81.3 to 83.1 MiB). A call through an entry
point costs one extra jump.

## How it works

`scripts/gen-vendor.py` runs at the end of `scripts/build-deps.sh` and
writes, into `out/deps/vendor/`:

- **`src/ZebkitVendorFfmpeg.cpp`**: for each function, an exported entry
  point `zebkit_vendor_ffmpeg_<function>` that jumps to the function. The
  build compiles it into the engine (`patches/2008`, `ZEBKIT_VENDOR_DIR` in
  the profile).
- **`include/zebkit/vendor/ffmpeg.h`**: a `#pragma redefine_extname` line
  per function, then FFmpeg's headers inside the namespace. GCC and Clang
  both have the pragma.
- **`include/zebkit/vendor/ffmpeg/`**: FFmpeg's headers as installed by its
  own build, with the includes between them made relative.

The list of functions is not written anywhere by hand: it is every global
function the library's static archives define, whose name matches the
pattern FFmpeg exports by, and which a public header declares.

To expose another library, add it to `VENDORED` in `scripts/gen-vendor.py`.

## Testing

```sh
scripts/vendor.sh slim
```

Builds `tests/vendor/` with the system's compiler against the package and
checks that the engine exports every entry point and none of FFmpeg's own
names, that an MP3 file decodes and converts through the C++ namespace,
that the C API works, and that the program needs no FFmpeg library.
