# Site compatibility

The `slim` build removes a lot from WebKit. This document is the plan for
proving that the result still works on the web people actually use, and the
record of how that went.

**Goal:** every site in `assets/WEBSITE-TOP-100.json` passes when loaded in
`slim`.

## The test set

`assets/WEBSITE-TOP-100.json` holds the 100 most-visited websites worldwide.
It is compiled from published traffic rankings (Semrush first, then
Similarweb and Ahrefs to fill up to 100); the file records the source and
source rank of every entry. Adult sites are excluded, as are two entries
that are not sites of their own. The exclusions and reasons are in the file.

A domain ranking such as Tranco was not used: its top is dominated by CDN,
DNS and advertising domains that nobody opens in a browser.

## What "passes" means

We do not know what each of 100 changing sites is supposed to look like, so
the test is a comparison. Each site is loaded twice, on the same machine at
about the same time: once in `slim` and once in `baseline`, which is stock
WebKit built from the same source. Stock WebKit is the reference for "what
this engine does with this site"; anything `slim` does worse is something
our trimming caused.

`tools/pageprobe` loads one URL in a headless 1280x800 view, waits for the
load to finish, lets the page settle for 8 seconds, then records:

- whether the load finished or failed, and whether the web process crashed
- every uncaught script error and unhandled promise rejection
- DOM measurements: element count, visible text length, links, images
  (and how many are broken), scripts, stylesheets, frames
- how much of the viewport was painted, and a screenshot

A site **passes** in `slim` when all of these hold:

1. **It loads.** No web process crash, no network or TLS failure.
2. **It paints.** If the reference painted the viewport, `slim` did too.
3. **It builds the same page.** Element count and text length are at least
   60% of the reference's. The margin is there because two loads of a live
   site seconds apart differ: ads, rotating content, A/B tests.
4. **It runs the same script.** No uncaught script error that the reference
   does not also throw. Errors that both builds throw are the site's or
   WebKit's, not ours.

A site is **blocked** when the reference build could not load it from the
test machine (unreachable, geo-blocked, bot challenge with no page). That
says nothing about `slim`, so it is reported separately and retried.

### What this does not prove

- **Interaction.** Nothing is clicked, typed or logged in. The test covers
  the landing page a visitor gets without an account.
- **Visual equality.** The comparison is structural. Screenshots are saved
  for a person to look at, and failures are checked by eye, but a layout
  that is subtly off would not be caught automatically.
- **Media playback on the sites.** The probe does not press play. Playback
  is tested separately with local samples by `scripts/media.sh`.

## How to run it

```sh
scripts/compat.sh baseline          # reference results into out/compat/baseline/
scripts/compat.sh slim              # results for the build under test
scripts/compat-report.py slim       # compare, write docs/reports/compat.md
```

`scripts/compat.sh <profile> <domain>...` re-runs individual sites.
`compat-report.py` exits non-zero until every site passes.

## The loop

1. Run both profiles over all 100 sites and generate the report.
2. Re-run every non-passing site in both profiles once more. Live sites and
   a busy network produce one-off failures; only repeatable ones count.
3. For each repeatable failure, find the cause: read the script errors,
   compare the screenshots, and reduce it to the missing or broken piece.
4. Fix it in the profile, a dependency recipe or a patch. A fix that brings
   back a removed feature is weighed against its size, and a fix that costs
   more than a few hundred KiB is raised before it is made.
5. Rebuild, re-run the affected sites, then re-run all 100 before calling a
   round done, because a fix can break something else.

Every round is recorded below.

## Results

### Round 1: 35 pass, 62 fail, 3 blocked

Almost every failure was one bug. Scripts died with errors like
`(x ??= new Set).add is not a function`.

- **Cause:** the linker option `--icf=safe` (identical-code folding). Under
  ThinLTO it folded not only functions but 30 distinct `JSC::ClassInfo`
  objects into 3 addresses. JavaScriptCore identifies types by `ClassInfo`
  address, so the optimizing JIT took `new Set` for `new Map`. The bug only
  appeared after a function had run a few thousand times, which is why the
  smoke test never saw it.
- **Fix:** folding is removed from `profiles/slim.cmake` (2.2 MiB). LLVM's
  own function merging, which cannot touch data, replaces about half of it.
- **Regression test:** added to `scripts/smoke.sh`; see open items.

Test-setup fixes made in this round: the probe now counts elements and text
inside shadow DOM; `bet.br` (not a site) was replaced in the list; Amazon,
eBay and live.com got the URLs they redirect visitors to; a reference page
that is a bot challenge or an error stub now counts as blocked instead of
letting two copies of it pass.

### Round 2: 54 pass, 34 fail, 12 blocked

- **Cause of nearly all 34:** `slim` was built without media support, and on
  WebKit's GLib ports that removes the DOM interfaces themselves. Scripts
  that mention `HTMLVideoElement`, `HTMLMediaElement`, `MediaError` or
  `Audio`, or call `canPlayType()`, `play()`, `pause()` or `load()` on a
  `<video>` element, throw. Affected: youtube.com, amazon.com, reddit.com,
  tiktok.com, twitch.tv, microsoft.com, apple.com, spotify.com, discord.com,
  openai.com and others.
- **Fix:** build the media elements with no playback engine behind them
  (`patches/2002`). They now exist, `canPlayType()` answers `""` and
  `play()` rejects, which sites handle. Video and audio still do not play.
  Cost: 1.1 MiB.
- **Also changed:** geolocation is back on, because with it off
  `navigator.permissions.query({name: "geolocation"})` rejects.

### Round 3: 86 pass, 1 fail, 13 blocked

The first pass gave 81. The non-passing sites were then re-run in both
builds, some several times; most were differences between two loads of a
live site rather than differences between the builds.

| Site | Status | What is known |
|---|---|---|
| uol.com.br | **Fails, repeatable** | A video player on the page rejects with "Browser not supported!". It needs Media Source Extensions, which `slim` does not have. The page itself matches the reference (3688 vs 3693 elements). Fixing it means shipping a media backend. |
| msn.com | Passes | Element counts swing between 1500 and 4200 from run to run in both builds as its feed and ads load; loaded one at a time the two builds agree, and the screenshots match apart from ads. |
| mashable.com | Passes | The Firebase Messaging error also appears in stock WebKit on some loads; it is the site's. |
| espn.com | Blocked | The page's script never yields in either build, so neither can be measured. `pageprobe` now gives up after a deadline instead of hanging. |

### Round 4: 84 pass, 0 fail, 16 blocked

Run after two requirement changes: real media playback (GStreamer and
FFmpeg linked in) and a build for glibc 2.17 with a different compiler.
Both builds were re-run from scratch.

| What turned up | Cause | Fix |
|---|---|---|
| ikea.com crashed the web process on about half its loads | WebKit's software video path uploaded a frame using its presentation size while reading the decoded buffer, which is smaller for video with non-square pixels or cropping | `patches/0004`: upload the decoded size. Eight loads in a row, no crash. A non-square-pixel sample is now in `scripts/media.sh`. |
| baidu.com threw "WebGLRenderingContext Error" | WebGL was off | WebGL is on (3.7 MiB). The smoke test clears a WebGL canvas and reads it back. |
| uol.com.br's "Browser not supported!" from round 3 | Needed Media Source Extensions | Gone now that media is real. |

The first pass of the final run gave 80; msn.com, yandex.ru, adobe.com and
amazon.co.jp were then re-run in both builds and passed (bot challenges
and feed variation, as in earlier rounds).

`tools/crashsite` and `scripts/symbolize.py` were written for the ikea.com
crash: the engine has no unwind tables, so a preloaded handler records the
crash address and the stack's code pointers, and the script maps them to
functions using the unstripped build.

### Script-visible differences from stock WebKit

Comparing `window` in both builds, `slim` lacks 27 global interfaces that
stock WebKit defines (it was 84 before media and WebGL were turned on). A
script that feature-detects them works; one that assumes them throws.

| Group | Interfaces | Why absent |
|---|---|---|
| Capture and recording (15) | `MediaDevices`, `MediaStream`, `MediaStreamTrack`, `MediaRecorder` and related | Camera and microphone capture is off |
| WebCodecs (7) | `VideoDecoder`, `VideoEncoder`, `VideoFrame`, `AudioDecoder`, `AudioEncoder`, `AudioData`, encoded chunks | Off |
| Gamepad (4) | `Gamepad`, `GamepadButton`, `GamepadEvent`, `GamepadHapticActuator` | Off |
| XSLT (1) | `XSLTProcessor` | Off |

### Blocked sites

These could not be judged from the test machine because stock WebKit did not
get the real site either. The set shifts a little from run to run; in the
last run it was these 16:

| Site | What the reference build got |
|---|---|
| claude.ai, openai.com, quora.com, fandom.com, indeed.com, ecosia.org | Cloudflare "Just a moment..." challenge |
| nytimes.com, realtor.com, ebay.com, ozon.ru, vk.ru, vkvideo.ru | Bot wall, challenge or error page |
| bilibili.com | No response from the server at all |
| live.com, dzen.ru | A near-empty redirect target |
| espn.com | A page whose script never yields; stock WebKit cannot be measured on it either |

### Open items

- The 16 blocked sites need a run from a residential network or with a
  browser the sites do not challenge. Until then "all 100 pass" is not shown;
  what is shown is that none of the 84 testable sites fails.
- Media playback is tested with generated samples (`scripts/media.sh`), not
  on the sites themselves: nothing presses play on YouTube.
- Stock WebKit on the build host has no H.264 decoder, so for H.264 there is
  no reference to compare with; `slim` plays it.
