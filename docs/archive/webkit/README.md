# The WebKit engine

Until 2026-10-17 zebkit was a trimmed build of WebKit: WPE WebKit on Linux,
WebKit's Mac port on macOS, and a start on its Windows port, with a Chrome
DevTools Protocol endpoint of its own. It was 83.1 MiB on Linux with media
playback. It was left for Blink because WebKit is three separate ports with
three APIs, where Chromium is one engine on every system.

These documents are the record of that engine, as they stood then:
`PROGRESS.md` (the project), `LINUX.md`, `MACOS.md`, `WINDOWS.md` (each
port), `CDP.md` (its endpoint), `COMPAT.md` (the site comparison),
`DEPS.md` and `VENDOR.md` (its libraries), and `reports/`. Paths in them are
the paths of that time; the engine they describe is no longer built.
