# patches

Changes to Chromium, on top of the release `upstream.lock` pins.

- `series` lists the patches to apply, one file name per line, in order;
  `#` starts a comment. `scripts/fetch-upstream.sh` applies them with
  `patch -p1` from Chromium's `src/`, and again whenever the list or a patch
  changes.
- Name a patch `NNNN-short-description.patch`, and begin it with what it
  does and why, and what it saves if it is for size.
- `1xxx`: size and speed, the engine in the build, PDFium, the shell's
  hooks for an embedder, FFmpeg. `0013`: what macOS needs that upstream
  does not build (IOSurface without Dawn).
- The series is append-only, not in the order of the numbers: a patch added
  at its end is applied on top of the tree as it is, while one inserted or
  changed earlier undoes and re-applies every patch (each patched file then
  has a new time, and the build takes hours). `scripts/fetch-upstream.sh`
  records each applied patch with its SHA-256 for this. So `0013` comes
  after `1017`, `1040` before `1038`, and a patch's number is never reused
  or changed once it is in the series: two are `1031`
  (`1031-v8-inspector-stack-microtasks-scope` and `1031-media-demuxer-tap`),
  told apart by the rest of the name.
- Patches that came before the rule of a description at the top:
  `1053-embedder-widget-screen-info` adds a `screen_infos` hook to
  `EmbedderWidgetHooks`, through which the embedder overrides a widget's
  screen information before it is sent to the renderer (on macOS, in
  `RenderWidgetHostViewMac`), and `NotifyEmbedderWidgetScreenInfoChanged()`,
  which makes a widget read it again after the embedder changes the screen it
  gives the view. Editing the file to add this would re-apply the series.

Every patch has to be carried to each Chromium release, so a build switch
in `profiles/` is preferred whenever one exists.
