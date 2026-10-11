# deps

Third-party libraries built from source by `scripts/build-deps.sh`, so they
are compiled for size and carry only the features the engine uses. Profiles
opt in with `zeb_use_deps()` (see `profiles/_common.cmake`).

- One directory per library; `recipe.sh` pins the version and source hashes
  and says how to build it. `order` lists them in dependency order.
- Everything installs into `out/deps/prefix`. Changing a recipe (or the
  shared compiler flags in `build-deps.sh`) rebuilds that library; after
  adding or removing a library, reconfigure the engine with
  `scripts/build.sh <profile> --fresh`.

`devtools-protocol/` is not a library and has no recipe: it is Chrome's
definition of the DevTools Protocol, copied unchanged. The CDP endpoint's
parameter checks are generated from it and its tests compare every message
with it (see the README in that directory).

## Static or shared

Static is the default: the engine link then keeps only the functions it
references, and nothing extra ships. A library is built shared only when
something outside the engine binary must use the same copy.

| Library | Linkage | Why it is built here |
|---|---|---|
| `zlib`, `brotli`, `expat`, `libffi`, `libtasn1`, `nghttp2`, `xkbcommon` | static | So they need not exist on the target |
| `icu` | static | Data filtered from 30 MiB to under 8 MiB |
| `libpng`, `freetype`, `harfbuzz`, `fontconfig` | static | One FreeType; HarfBuzz must link our ICU |
| `libjpeg-turbo`, `libwebp`, `libepoxy` | static | Unused code dropped at link |
| `libxml2` | static | Parser only; no schemas, XPath, HTML, compression |
| `libgpg-error`, `libgcrypt` | static | Only the algorithms WebCrypto calls |
| `pcre2` | static (into GLib) | GLib's regular expressions |
| `glib` | shared | Its types cross WebKit's GLib API; no SELinux or libmount |
| `sqlite` | shared | Used by both the engine and libsoup |
| `libpsl` | static (into libsoup) | Built without an IDN library, which drops libunistring |
| `libsoup` | shared | Its objects cross WebKit's GLib API; built without Kerberos |
| `nettle`, `gnutls` | static (into the TLS plugin) | No p11-kit, IDN or GMP |
| `glib-networking` | shared plugin | GIO's TLS backend, one file |
| `ffmpeg` | static | The engine's decoders (H.264, VP8, VP9, AAC, MP3, Opus, Vorbis, FLAC); for applications also file reading and writing, scaling, and FFmpeg's own encoders |
| `gstreamer` | static | Core, helper libraries and 21 plugins as one fixed set |

Everything is compiled with the cross toolchain from `toolchain.lock`, so no
library can import a symbol newer than the glibc floor.

## ICU data

`icu/filter.json` decides what goes into the data file. It currently keeps
locale data for English, Chinese, Japanese and Korean, every break rule and
dictionary (including the CJK word dictionary), root and CJK collation, and
the legacy converters WebKit asks ICU for by name.

To support more languages in `Intl` (dates, numbers, plurals, collation),
add their codes to `localeFilter.includelist`. Text in any language still
renders, breaks and normalizes correctly without this; the filter only
affects locale-specific formatting, which falls back to the root locale.

Left out: transliteration, spelled-out numbers, character names, and
converters WebKit never requests (`EUC-TW` among them; it is not in the
Encoding Standard).

## From the target system

The engine has no hard dependency beyond glibc. `syslibs/` builds stand-ins
for the libraries that belong to the system and cannot be bundled:
`libwayland-client`, `libX11`, `libgbm`, `libdrm` and `libasound`. A stand-in
has the library's functions, each of which loads the real library on first
use (`tools/syslib-stubs`). If the library is not installed, the call
returns a failure value and the feature is unavailable: no Wayland or X11
window, no GPU buffer sharing, no sound.

`ZEBKIT_SYSLIB_LOG=1` prints which of them a machine provides;
`ZEBKIT_WITHOUT_LIBRARIES=<soname>:<soname>` makes the loader ignore some.

Fonts, CA certificates and keyboard layout data are read from the usual
system locations.
