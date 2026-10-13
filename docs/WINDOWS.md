# Windows

zebkit on Windows is not started. Nothing here has been built or tried;
this is what is known from the other two platforms and what has to be
decided first. `docs/PROGRESS.md` has the project as a whole.

## What the other platforms say about it

- **The engine would be a port WebKit has itself**, built from the same
  release as the others, as on macOS. WebKit's source tree has a Windows
  port; which state it is in at this release, what it uses for
  networking, graphics and media, and whether it builds from the release
  as tagged has not been looked at. The release tarball Linux builds from
  holds the GLib ports only, so the source would come from WebKit's git
  repository at the pinned commit, as on macOS.
- **The CDP endpoint needs a third layer.** `src/cdp` is portable code
  (`CDPView.cpp`, the WebSocket server, the protocol tables) over a layer
  for each engine API: GLib on Linux, Cocoa on macOS. Windows needs its
  own, on whatever API that port gives a view.
- **The same things decide when it is done:** the smoke test, the media
  test, the raw CDP checks with Puppeteer and Playwright, the top-100
  comparison against the port's own unslimmed build, and the benchmark
  beside the size (`scripts/bench.sh`).
- **The same pieces would have to exist:** a `slim` profile, a package
  that runs from any directory, the release archive with its checksum in
  `rust/zebkit-rs/engines.txt`, FFmpeg for applications (`docs/VENDOR.md`),
  the window example, and the Rust crates with a third platform layer.

## To decide

| Question | Why it matters |
|---|---|
| Which port, and which of its backends | Sets what comes from the system and what has to be built and shipped, and with that the size |
| Architectures: x86_64, ARM64 or both | One archive for each |
| Oldest Windows to support | Which system APIs the engine may use |
| Where it is built | Linux builds on Linux and macOS on a Mac; whether Windows can be cross-built or needs a Windows machine is not known |
| The API applications see | Linux has WebKit's GLib C API and macOS its Objective-C API; a third API for Windows makes the common C layer (`docs/MACOS.md`, "Not decided") more pressing |
| Media | macOS uses the system's decoders, Linux ships its own. Which of the two Windows can do decides whether the codec licensing question (`docs/PROGRESS.md`) applies there |

## First steps

1. Find out what the Windows port is at this release: how it is built, on
   what, and with which libraries.
2. Build it unchanged, as `baseline`, and load a page with it.
3. Measure it, and write down here what it is made of, before taking
   anything out.
