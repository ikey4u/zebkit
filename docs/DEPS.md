# Dependencies

What zebkit is made of: the engine, the libraries built from source and
linked into it, the few that ship beside it as separate files, and what it
uses from the system when it is there.

Versions are pinned, with SHA-256 checksums, in `upstream.lock`,
`toolchain.lock` and `deps/<name>/recipe.sh`; this page is a summary of
those files and must be updated with them. `deps/README.md` says why each
library is built the way it is.

## The engine

| Component | Version | Notes |
|---|---|---|
| WebKit (WPE port) | 2.54.1 | `upstream.lock`, plus the patches in `patches/` |
| JavaScriptCore, WebCore, WTF, bmalloc | 2.54.1 | Part of WebKit |

Libraries marked *exposed* can be used by an application through the
engine, without a copy of its own; see `docs/VENDOR.md`.

## Bundled in the WebKit source and linked statically

These come inside the WebKit release, not from `deps/`.

| Library | Version | Used for | Licence |
|---|---|---|---|
| Skia | milestone 154 | 2D graphics, text rendering, image encoding | BSD-3-Clause |
| ANGLE | commit `e3fdc27d77e3` (2026-09-07) | WebGL on top of the system's OpenGL ES | BSD-3-Clause |
| xdgmime | WebKit's copy | MIME types from file names | LGPL-2.1-or-later or AFL-2.0 |

WebKit's source also carries pdf.js 6.1.200 and libsysprof-capture; both
are switched off in zebkit and not in the binary.

## Built from source and linked statically into `libzebkit.so.1`

| Library | Version | Used for | Licence |
|---|---|---|---|
| ICU | 76.1 | Unicode, text segmentation, `Intl`; data cut down to English and CJK | Unicode-3.0 |
| FFmpeg (*exposed* as `zebkit::vendor::ffmpeg`) | 8.1.3 | Decoders: H.264, VP8, VP9, AAC, MP3, Opus, Vorbis, FLAC, PCM, Motion JPEG. For applications also: reading and writing MP4, WebM, Ogg and WAV files, scaling, and encoders for AAC, FLAC, PCM and Motion JPEG | LGPL-2.1-or-later (built without the GPL and non-free parts) |
| GStreamer | 1.28.8 | Media pipeline: the core, and a fixed set of plugins for MP4, WebM, audio streams, ALSA output and the FFmpeg bridge (listed in the recipe) | LGPL-2.1-or-later |
| HarfBuzz | 10.2.0 | Text shaping | MIT |
| FreeType | 2.13.3 | Font rasterization, WOFF2 fonts (with Brotli) | FTL or GPL-2.0 |
| Fontconfig | 2.15.0 | Finding the system's fonts | MIT-style |
| libxml2 | 2.13.8 | XML parsing | MIT |
| libpng | 1.6.48 | PNG images | libpng-2.0 |
| libjpeg-turbo | 3.1.0 | JPEG images | IJG and BSD-3-Clause |
| libwebp | 1.5.0 | WebP images | BSD-3-Clause |
| zlib | 1.3.1 | Deflate | Zlib |
| Brotli | 1.1.0 | Brotli content encoding, WOFF2 | MIT |
| libgcrypt | 1.11.0 | Web Crypto algorithms | LGPL-2.1-or-later |
| libgpg-error | 1.51 | Needed by libgcrypt | LGPL-2.1-or-later |
| libtasn1 | 4.20.0 | ASN.1 for Web Crypto keys | LGPL-2.1-or-later |
| libepoxy | 1.5.10 | Loading the system's EGL and OpenGL ES | MIT |
| libxkbcommon | 1.7.0 | Keyboard layouts | MIT |
| Expat | 2.7.1 | XML for Fontconfig's configuration | MIT |
| libffi | 3.4.8 | Needed by GObject | MIT |
| nghttp2 | 1.64.0 | HTTP/2 (inside libsoup) | MIT |
| libpsl | 0.21.5 | Public suffix list for cookies (inside libsoup) | MIT |
| PCRE2 | 10.45 | Regular expressions (inside GLib) | BSD-3-Clause |
| libc++, libc++abi, compiler-rt | LLVM 21, from Zig 0.16.0 | The C++ standard library | Apache-2.0 with LLVM exception |

## Shipped beside the engine as separate files

Shared, because something outside the engine binary has to use the same
copy (see `deps/README.md`).

| Library | Version | File | Licence |
|---|---|---|---|
| GLib (GLib, GObject, GIO, GModule) | 2.84.4 | `libglib-2.0.so.0`, `libgobject-2.0.so.0`, `libgio-2.0.so.0`, `libgmodule-2.0.so.0` | LGPL-2.1-or-later |
| libsoup | 3.6.5 | `libsoup-3.0.so.0` | LGPL-2.0-or-later |
| SQLite | 3.46.1 | `libsqlite3.so.0` | Public domain |
| glib-networking | 2.80.1 | `gio/modules/libgiognutls.so` | LGPL-2.1-or-later |
| GnuTLS | 3.8.9 | Static, inside `libgiognutls.so` | LGPL-2.1-or-later |
| Nettle | 3.10.1 | Static, inside `libgiognutls.so` | LGPL-3.0-or-later or GPL-2.0-or-later |

## Used from the system, never bundled

| Library | When |
|---|---|
| glibc 2.17 or newer (`libc`, `libm`, `libdl`, `libpthread`, `librt`, `libresolv`) | Always; the only hard requirement |
| `libwayland-client`, `libwayland-egl`, `libwayland-cursor`, `libwayland-server` | Loaded on first use, when the session is Wayland |
| `libX11` | Loaded on first use, when there is an X server and no Wayland |
| `libgbm`, `libdrm` | Loaded on first use, for GPU buffers |
| `libEGL`, `libGLESv2` | Loaded by libepoxy, for accelerated rendering and WebGL |
| `libasound` | Loaded on first use, for sound |

The engine starts without any of the optional ones. Their stand-ins are
built by `deps/syslibs`.

## Not included

Off in the `slim` profile, so their libraries are not linked: AVIF
(libavif), JPEG XL (libjxl), colour management (lcms2), XSLT (libxslt),
WebRTC, Web Codecs, Encrypted Media, speech synthesis (Flite), hyphenation
(libhyphen), gamepads (libmanette), the Bubblewrap sandbox, systemd
journal logging, and the accessibility bridge (ATK).

## About the licences

The licence column is what each project states for the version used; it
is a guide, not legal advice. Two points an application that ships zebkit
has to deal with:

- **LGPL libraries are linked statically into the engine** (FFmpeg,
  GStreamer, libgcrypt, libgpg-error, libtasn1, and WebKit itself is
  LGPL-2.1 and BSD). zebkit as a whole is a shared library an application
  links dynamically, which is the usual way to meet the LGPL, and everything
  needed to rebuild it is in this repository.
- **H.264 and AAC are covered by patents** in some countries regardless of
  FFmpeg's licence. Whether to ship them is an open question in
  `docs/PROGRESS.md`.
