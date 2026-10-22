# patches

Changes to Chromium, on top of the release `upstream.lock` pins.

- `series` lists the patches to apply, one file name per line, in order;
  `#` starts a comment. `scripts/fetch-upstream.sh` applies them with
  `patch -p1` from Chromium's `src/`, and again whenever the list or a patch
  changes.
- Name a patch `NNNN-short-description.patch`, and begin it with what it
  does and why, and what it saves if it is for size.
- `1xxx` are the Blink engine's: size and speed, the engine in the build,
  PDFium, the shell's hooks for an embedder, FFmpeg. `0013` and `0014`
  are for macOS. The other `0xxx` and the `2xxx`, with `series.macos` and
  `series.windows`, are the WebKit engine's and are not applied
  (`docs/ARCHITECTURE.md`, "What is left of WebKit").

Every patch has to be carried to each Chromium release, so a build switch
in `profiles/` is preferred whenever one exists.
