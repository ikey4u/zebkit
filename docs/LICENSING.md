# Licensing

What zebkit is under, what it contains, and what an application that ships
it has to do. This is a summary for developers, not legal advice; the
licenses themselves are what count.

## zebkit

zebkit's own code (the engine's embedder in `src/`, the bindings in
`bindings/`, the tools in `tools/`, the scripts) is under the **GNU Lesser
General Public License, version 2.1 or later** (`LGPL-2.1-or-later`), in
`LICENSE`. The Rust crates (`zebkit`, `zebkit-rs`, `zebkit-build`), the
Python package and the Go module carry the same `LICENSE`, and so does every
release archive from the release after 0.4.0 on (0.4.0's archives were made
before `LICENSE` was added).

## What the engine contains

The engine is Chromium (the version in `upstream.lock`) with the patches in
`patches/`, built by the profiles in `profiles/`. It contains the libraries
Chromium uses, each under its own license: BSD-style for most of Chromium,
V8 and Skia; the GNU Library General Public License, version 2 or later, for
Blink's code that came from WebKit and KDE; LGPL 2.1 or later
for FFmpeg; BSD-style, MIT or similar permissive licenses for PDFium,
ICU, HarfBuzz, FreeType, zlib, Zstandard, Brotli, libjpeg-turbo, libwebp,
libvpx, dav1d, Opus, BoringSSL (OpenSSL and ISC) and others; SQLite is in
the public domain.

From the release after 0.4.0 on, every release archive has three files at
its top:

| File | What it is |
|---|---|
| `LICENSE` | zebkit's license, the LGPL 2.1 |
| `NOTICE` | what the archive is, its license, and where its exact source is: the repository at the tag `v<version>` |
| `THIRD_PARTY_NOTICES.txt` | every third-party component in the engine with its license text, then the Rust crates compiled into `zebkit-mcp` |

`THIRD_PARTY_NOTICES.txt` is made at packaging time by
`scripts/third-party-notices.py`: for the engine, Chromium's own tool
(`tools/licenses/licenses.py`, what Chrome's `about:credits` page comes
from) over the GN dependencies of `//zebkit/engine:engine` for that
system, so that it lists what is linked and nothing else; for
`zebkit-mcp`, the crates `cargo metadata` resolves for the target, normal
dependencies only. Each system's archive has its own list.

**FFmpeg** is configured with its LGPL parts only: `CONFIG_GPL`,
`CONFIG_GPLV3` and `CONFIG_NONFREE` are 0 in every configuration
(`src/ffmpeg/config`), so no part of the engine is under the GPL.

## Shipping zebkit with an application

The application's own code can be under any license, open or closed: it
uses the engine as a library. What the LGPL and the third-party licenses ask
of it, in short:

1. **Ship the notices.** Put `LICENSE`, `NOTICE` and `THIRD_PARTY_NOTICES.txt`
   in what you distribute, beside the engine, or show their text in the
   application (an "About" or "Open source licenses" screen), as Chrome does
   with `about:credits`.
2. **Keep the engine replaceable.** Ship it as the shared library it is
   (`libzebkit.so.1`, `Zebkit.framework`, `zebkit.dll`), so that a user can
   put another build of the same version in its place. Do not prevent that
   (by obfuscation or by a check that refuses a rebuilt engine), and allow
   the reverse engineering needed to debug such a replacement, as the LGPL's
   section 6 says.
3. **Say where the source is.** For an unmodified engine, point to the
   source of the version you ship: `https://github.com/ikey4u/zebkit/tree/v<version>`,
   which names Chromium's version and carries the patches (the `NOTICE` file
   does this). If you change zebkit or the engine, you must offer the source of
   your changes under the LGPL as well.
4. **Keep the license notices** in the files you received.

The bindings (Rust, Python, Go) and `zebkit-js` are under the same license
and are compiled or loaded into your program; the same four points apply.

## Codecs

The engine decodes H.264 and AAC, as Chrome and Electron do
(`proprietary_codecs = true`, `ffmpeg_branding = "Chrome"`), besides VP8,
VP9, AV1, Opus and others. These codecs are covered by patents in some
countries; their licensing, where it applies, is the distributor's to
consider. zebkit adds no license for them.

## Signing on macOS and Windows

Not a licensing matter, but part of shipping: on macOS the engine's helper,
`Zebkit.framework/Versions/A/Helpers/Zebkit Helper.app`, is signed and
notarized with the application, as Chrome's and CEF's helpers are. The
release archives are signed ad hoc only; signing with a Developer ID and
notarization are the application's (`docs/MACOS.md`).
