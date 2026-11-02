# zebkit-mcp: the engine as MCP tools

`tools/zebkit-mcp` gives an AI agent the engine's capabilities as
[MCP](https://modelcontextprotocol.io) tools, over standard input and over HTTP,
within bounds whoever runs it sets. CDP stays for clients that speak it; MCP is
what agents are given.

It is written in Rust, over the `zebkit` crate: parsing untrusted JSON and HTTP
next to the engine wants a memory-safe language, the crate already has the C API,
and `std` gives the same sockets and paths on Linux, macOS and later Windows,
without an interpreter to install.

## Running it

```sh
ZEBKIT_DIR=<package> cargo build --release --manifest-path tools/zebkit-mcp/Cargo.toml
```

The executable looks for the engine beside itself: copy it into the package, or
beside the `zebkit` directory's contents.

An MCP client starts it and talks on standard input and output:

```json
{"mcpServers": {"zebkit": {"command": "/path/to/zebkit-mcp",
  "args": ["--allow-origin", "https://example.com", "--allow-write", "/home/me/agent-files"]}}}
```

Over HTTP, on loopback, with a token:

```sh
zebkit-mcp --http 8931 --http-token-file ~/.config/zebkit-mcp-token --allow-origin '*'
# zebkit-mcp: listening on http://127.0.0.1:8931/mcp
```

The client sends `Authorization: Bearer <the file's content>`. Without
`--http-token-file` a token is generated and printed once on standard error.

Chromium's switches go after `--`, as `-- --no-sandbox` where the machine cannot
give the engine a sandbox (`docs/SANDBOX.md`; never by default, and said at the start). Those that would pass the bounds are refused, with one dash
or two: `--remote-debugging-*`, `--host-resolver-rules`, `--host-rules`, `--proxy-*`,
`--user-data-dir` (use `--profile`), `--disable-web-security`, `--disable-features`,
`--enable-features`, WebRTC's policy, extensions, and programs for the engine to start.

## Bounds

Set on the command line, read once, never changed: nothing an agent or a page says
can widen them. What is not given is denied.

| Bound | Option | Default |
|---|---|---|
| Tools | `--tools pages,pdf_open,...`, `--deny-tools page_evaluate,...` (groups or names) | All. A tool not allowed is not listed, and a call to it is refused |
| Origins | `--allow-origin https://example.com`, `https://*.example.com`, `*` | None: no page loads anything, and `net_fetch` reaches nothing |
| Directories | `--allow-read DIR`, `--allow-write DIR` | None: no file is read or written |
| Windows | `--windows` | None: pages and `zebkit.ui` are drawn off-screen |
| Settings | `--allow-settings KEY,KEY` | None: `settings_set` refuses every key |

`*` means every public origin, not this machine or private addresses: not
`localhost`, `*.localhost`, `*.local`, `*.internal`, `*.localdomain`, `*.lan`, `*.home`,
`*.corp`, `*.intranet`, `*.home.arpa`, a name with no dot, 0/8, 10/8, 100.64/10, 127/8,
169.254/16, 172.16/12, 192.0.0/24, 192.168/16, 198.18/15, 224/3, `::1`, `fc00::/7`,
`fe80::/9`, `fec0::/10`, `ff00::/8`, nor those inside an IPv6 address (`::ffff:a.b.c.d`,
`::a.b.c.d`, `64:ff9b::a.b.c.d`, `2002:a.b:c.d::`), in any way a URL can write them
(`http://2130706433/`), since the browser's own parser reads them. Name a private origin
exactly to allow it.

Every tool's arguments are checked against the schema it publishes before it runs:
types, ranges, lengths, no property it does not name. A failure comes back as a tool
error the agent can read; a schema using a keyword the checker does not enforce
stops the server at its start.

### What holds the origins

An origin is not checked once: everything of a page goes through it, whoever asked.

- **Documents**, of any frame, redirects too: `zebkit_view_on_navigation`, failing
  closed if not answered. `about:blank` and `blob:` of an allowed origin pass; `about:`
  and `data:` only in child frames; `file:` and every other scheme never.
- **Requests** of frames and dedicated workers: `zebkit_view_set_network_filter`, each
  held and answered.
- **WebSockets**: the same hook takes a page's handshakes (2026-10-31); before it, as well: a
  `connect-src` policy added to the response of every document and of every script (a worker's policy is its script's,
  not its document's), and put as an element into a blank page, which has no response;
  a page cannot undo either. Under `*` there is no such policy, which cannot say
  "not private": the next but one line holds it.
- **Service and shared workers**, which have no view for the hook: they are removed
  from each page before its scripts, as Playwright's `serviceWorkers: "block"` does.
- **The whole network stack** resolves only the allowed names; under `*`, resolves no
  private name or address of those above (`--host-resolver-rules`): the last line for
  anything above missed. WebRTC is told not to use its own UDP sockets, which the tests
  check against a local STUN server (without the switch, that server is reached).

Popups are blocked and reported, with their address, for the agent to go to itself.
Downloads, a page's file choosers, camera and microphone, permissions, HTTP
authentication prompts, full screen and links to other programs are refused. A page
never gets `zebkit.*`: nothing here grants it. CDP and Playwright's endpoint are not
served. What was refused is in `refusedByBounds` of `page_network` and in the error of
the navigation it stopped.

What is known not to be held, on purpose or not, is under [What is weak](#what-is-weak).

### What holds the directories

Paths must be absolute; `..` is refused outright. A path is resolved with its links,
and the real path must be inside a grant. A file to read must be a regular file (a
pipe would hang the server, a device worse). It is then opened without following a
last link, and checked again after opening against what the name resolves to now, so
a link swapped in after the check is caught. To write: the real parent directory must
be inside a write grant, and the file must not be a link, or have other hard links; it
is written to a new private file beside it and renamed, so nothing is ever written
through a link. A file there is replaced only with `overwrite`. The engine gets paths
only where it has to (`media_*`), always the real ones; `page_upload` gives it the
file's contents, up to 64 MiB each.

### HTTP

A local server that drives a browser is what any web page would like to reach. Each
request passes, before anything of it is parsed as MCP:

1. **Host**: exactly one, a loopback name at this port, or `--http-allow-host`
   (against DNS rebinding). 403.
2. **Origin**: absent (not a browser), `--http-allow-origin`, or this server's own
   loopback address; `null` never. 403, before the token is looked at.
3. **Path**: only `/mcp`. 404.
4. **Token**: `Authorization: Bearer`, compared in constant time; never read from the
   URL. 401.
5. **Method**: POST, and DELETE to end a session; GET 405, as this server sends no
   streams. There is no CORS: a preflight gets a refusal and no header.
6. **Body**: `application/json` (not a form a page may send freely), an `Accept` as MCP
   says, one `Content-Length` up to 128 MiB (no chunked bodies). Headers up to 16 KiB;
   64 connections at once, 30 seconds for a whole request, however slowly it comes.

`--http-bind` to anything but a loopback address needs `--http-insecure-remote`: there
is no TLS, so the token would cross the network in the clear. Put a proxy in front.

## The protocol

The current revision, [2026-07-28](https://modelcontextprotocol.io/specification/2026-07-28):
stateless, the version in each request's `_meta`, `server/discover`, the
`MCP-Protocol-Version`, `Mcp-Method` and `Mcp-Name` headers checked against the body
(`HeaderMismatch`), `UnsupportedProtocolVersionError`, `resultType` and caching hints.
And, as a dual-era server, the handshake revisions (2025-11-25, 2025-06-18,
2025-03-26: `initialize`, sessions over HTTP), which is what today's SDKs speak.
Pages and documents are referred to by ids a client passes, as the revision asks, and
live as long as the server: one principal, one token, however many connections.
No resources, prompts, subscriptions or streams; `notifications/cancelled` drops the
answer on standard input, and the work already begun finishes.

Standard output is the protocol's alone: at the start the server takes the descriptor
for itself and everything else in the process, and the engine's child processes, sees
standard error at 1 and nothing at 0.

## The tools

`zebkit-mcp --list-tools` prints them with their schemas.

| Group | Tools |
|---|---|
| `pages` | `page_open`, `page_navigate`, `page_history`, `page_wait`, `page_read` (`aria` with references, `text`, `markdown`, in pieces), `page_click`, `page_type`, `page_press`, `page_select`, `page_check`, `page_hover`, `page_scroll`, `page_mouse`, `page_keyboard`, `page_upload`, `page_evaluate`, `page_capture`, `page_network`, `page_console`, `page_dialog`, `page_list`, `page_close` |
| `pdf` | `pdf_open`, `pdf_close`, `pdf_info`, `pdf_read` (text, lines, characters), `pdf_search`, `pdf_render`, `pdf_outline`, `pdf_links`, `pdf_form_fields`, `pdf_form_set`, `pdf_annotations`, `pdf_annotate`, `pdf_redact`, `pdf_edit_pages`, `pdf_signatures`, `pdf_save`, `page_pdf` |
| `files` | `file_read`, `file_write`, `file_list`, `file_stat`, `file_mkdir`, `net_fetch`, `data_compress`, `data_decompress`, `data_hash`, `image_decode`, `image_encode`, `url_parse` |
| `media` | `media_probe`, `media_convert`, `media_cut`, `media_extract_audio`, `media_extract_frames` |
| `ui` | `ui_open`, `ui_tree`, `ui_click`, `ui_type`, `ui_submit`, `ui_events`, `ui_capture`, `ui_close` |
| `auth` | `auth_device_start`, `auth_device_poll`, `auth_status`, `auth_forget` |
| `settings` | `settings_get`, `settings_set` |

`settings` is the runtime settings (`docs/API.md`): `settings_get`, read-only, and
`settings_set`, of a page or, with no `page`, the whole engine. Nothing can be set unless
named in the bound `--allow-settings KEY,KEY`, which is empty by default, and a page never
reaches either. `downloads` takes only `deny` and `newWindows` only `block`: they cannot widen
what the hooks here refuse. `viewport` is `{width, height}`, of a page.

**Page content is data.** What comes from a page, a PDF or a response is marked so in
results (`untrusted`) and in `instructions`. A server cannot stop an agent believing a
page; what it can do is that the bounds hold when the agent does.

**A reference** such as `e12` comes from `page_read` in `aria` format, Playwright's own
snapshot for AI, and the action tools take it as `ref`, or a Playwright `selector`.
Frames are not expanded in the snapshot: reach into one with a selector
(`iframe >> internal:control=enter-frame >> button`).

**Pages drawn on a canvas** (Google Docs and Sheets, maps, editors, games) have
nothing an element reference can name: their content is pixels. For them, as a
computer-use agent works, there are coordinates:

- `page_capture` of the viewport is at one pixel per CSS pixel of the viewport
  (`scale: "css"`), and its result says `scale: 1`, the viewport's size and its
  scroll: the picture's x and y are what `page_mouse` takes. `grid: true` lays
  numbered lines every 100 pixels over it, `cursor: true` marks where `page_mouse`
  left the mouse; both are off by default and taken out of the page after the
  picture.
- `page_mouse`: `move`, `click` (`button`, `count: 2` for a double click), `down`,
  `up`, `drag` from `x,y` to `toX,toY` (moved in steps, as a hand drags), `wheel`
  (`deltaX`, `deltaY`) at a point, with `modifiers` held meanwhile. These are the
  automation's `Page.mouse*` commands: trusted events from the browser process,
  through Blink's hit testing.
- `page_keyboard`: to whatever has the focus, no element named: `type` key by key,
  `insertText` at once as an input method commits, `press` a key or chord, `down`
  and `up`; `modifiers` held around it. A canvas editor takes keys in an element
  of its own it focuses on a click (Docs: a hidden, contenteditable iframe), so a
  click into the canvas first, then the keyboard.
- `page_read` still has such an app's menus, toolbars and dialogs, which are DOM;
  its descriptions say that what is drawn needs `page_capture` and coordinates.

`tests/engine/mcp_server.mjs` has a canvas page of its own: a click draws where it
was asked (checked in the canvas's pixels, in the page's state and in the
captured PNG at the same x and y), modifiers, a double click, a drag, the wheel,
keys into a hidden sink the click focused, and the overlay gone after.

**Windows.** A process has windows or has none: `--windows` decides, and `page_open`'s
`window` refuses what the server was not started for. `ui_open` builds a window of
`zebkit.ui` from data, with an allow-list of elements and properties; no `web` element,
as that would be a page outside the hooks. `ui_events` gives what a person did.

**net_fetch** is a request through the engine's network stack, hop by hop. A plain
loader of the engine follows redirects itself and takes `Authorization` along, so this
is a `fetch()` in a page of no one's, whose document the server answers at the target's
origin without sending anything: a redirect to another origin stops there, is checked
like a first request, and goes on as a GET with nothing of the first request's: no
header given for the first goes along. A response over 256 MiB fails.

## Sign-in without a screen

OAuth's device flow (RFC 8628) is a tool of this server, not an API of the engine:
`auth_device_start` gives a code and an address to show the person, who approves on any
device; `auth_device_poll` asks no sooner than the provider allows and follows
`authorization_pending`, `slow_down`, `access_denied` and `expired_token`. `--oauth-config`
is a file only its owner can read:

```json
{
  "github": {"clientId": "...", "scopes": ["repo"]},
  "google": {"clientId": "...", "clientSecret": "...", "scopes": ["https://www.googleapis.com/auth/drive.readonly"]},
  "microsoft": {"clientId": "..."},
  "mine": {"clientId": "...", "deviceEndpoint": "https://auth.example/device", "tokenEndpoint": "https://auth.example/token", "apiOrigins": ["https://api.example"]}
}
```

`github`, `google` and `microsoft` have their endpoints built in; `apiOrigins` says where
a token may go, and `scopes`, when given, what an agent may ask for. Endpoints must be
https, but on this machine.

The token never leaves the server: not in a result, a log, a file or a page. An agent
holds the flow's id and passes it as `net_fetch`'s `auth`; the header is added in the
browser process to requests to that provider's `apiOrigins` only (which `--allow-origin`
must allow too), a redirect elsewhere does not carry it, and a response that echoes it
as it is, in its body, headers or address, has it replaced before anything is shown or
written. An API that gives it back encoded some other way is not caught. Tokens are in memory, and gone with the process. An expired one means a
new flow: refresh tokens are kept but not used yet.

## How it is checked

`scripts/engine-test.sh` builds it against the package and runs three programs, with the
official client (`@modelcontextprotocol/sdk`) and the project's conformance suite as
development dependencies, pinned in `upstream.lock`, shipped nowhere:

- `tests/engine/mcp_server.mjs`: every tool, called through the SDK, over both transports,
  and failing if one is not called; for every tool an unknown argument, a missing one, a
  wrong type; symbolic links, `..`, pipes, hard limits and writes out of the grants; what
  a page tries (fetch, images, frames, WebSockets, workers, navigation, popups, downloads,
  file choosers, the camera); private addresses under `*`; missing and wrong tokens, bad
  Origin and Host, wrong headers; refusals to start; a window under Xvfb; and that no token,
  device code or secret is in any log or result. The SDK does not speak 2026-07-28 yet, so
  that revision is checked with raw requests on both transports.
- `tests/engine/mcp_conformance.mjs`: the conformance suite's scenarios that apply to any
  server (`server-initialize`, `ping`, `tools-list`, `dns-rebinding-protection`), over HTTP.
- `tests/engine/mcp_agent_run.mjs`: research across a local site, a form of two steps and a
  320-page PDF, using only the tools. **It is a scripted run**, written by those who wrote the
  server: no model decides anything in it. It shows the tools are enough for the task, not
  that an outside agent does well with them; that is still to do (`docs/ROADMAP.md`).

## Engine fixes made for it

What this server found and worked around was fixed in the engine on 2026-10-31:
`Frame.setInputFiles` with `localPaths` grants the renderer the files, `APIRequestContext.fetch`
stops at a redirect and has a size limit, neither it nor `zebkit_fetch()` takes `Authorization`
to another origin, `Page.setViewportSize` lasts on an off-screen view, a command from a failed
navigation's callback no longer crashes, input after a load is not lost, and `media_tools` has
`-ss` and `-t`. So `media_cut.c`, the wait for frames and the viewport's workaround are gone.
`page_upload` stays by contents and `net_fetch` on its page, on purpose: each keeps the path from
the renderer and checks each hop for itself, which the fixes do not replace.

## An outside agent on the open web

On 2026-11-02 Claude Code, headless (`claude -p`), with this server as its only
tools (`--strict-mcp-config`, built-in tools off, `--allowedTools mcp__zebkit`) and
`--allow-origin '*'`, did five real tasks; no sign-in anywhere, nothing in the way
of a site's protections attempted. Each run is a transcript kept outside the
repository; what they showed:

| Task | Outcome | What got in the way, and what was done |
|---|---|---|
| Fill and submit httpbin.org's order form | Done, 13 turns: every field echoed back | The accessibility tree names radios and check boxes by their labels, not the values they submit, and shows a time input as a text box: Playwright's snapshot, kept as it is |
| Research three facts on rust-lang.org, nodejs.org and Wikipedia | Done, 16 turns, each with its URL | No way to find a passage on a long page but script (`page_evaluate` with `indexOf`): `page_read` now takes `find`. Wikipedia answered 429 once, shown as the status; a retry passed |
| Read a 15-page arXiv PDF, four questions with quotes | Done, 8 turns: `net_fetch` into a file, `pdf_open`, `pdf_read` | Writing into a directory that did not exist said "entity not found": it now says to make it with `file_mkdir` |
| Search Wikipedia with its own search box | Done, 11 turns | Typing re-rendered the box, so the reference went stale and the next press waited its whole 30 s: a reference no longer on the page now fails at once, saying to read the page again. A press that submitted answered with the page it left: actions now answer after the navigation they started has loaded. Run again after both: 10 turns instead of 11 and 42 s instead of 78; the stale reference failed at once, the press answered with the article's URL, and `find` found both facts in a 222,000-character tree |
| Read a public Google Doc and scroll it | Done, 8 turns | The text is on a canvas: `page_capture` read it, `page_mouse`'s wheel scrolled it; `page_read` had only the menus. `page_wait` for `networkidle` never came (Docs keeps connections open), as Playwright's would not |

**Google Docs, as seen from here** (a public, view-only document): the text is
drawn on `<canvas>` tiles (three on the first screen); the keys go to a hidden
`docs-texteventtarget-iframe` whose body is `contenteditable`, `role=presentation`,
which takes the focus on a click into the page; the two other iframes are
`aria-hidden`. Docs' own **screen reader support** (`Control+Alt+Z`, or the
Accessibility menu) puts the document's text into the accessibility tree: after
`page_keyboard` presses it, `page_read` has the body as well as the menus, which is
the cheaper way to read a long document. Without it: `page_capture` and `page_mouse`.

## What is weak

Plainly, what these bounds do not do:

- **DNS rebinding of a page's own requests under `*`.** A public name that resolves to a
  private address is not seen: the check is on names. Naming origins exactly holds here, as
  only those names resolve. Asking the resolver first would not close it: whoever serves the
  name's DNS can answer differently the second time.
- **Under `*`, what the hooks do not see can reach a name with no dot.** The resolver's rules
  have no way to say it, and they know no digits: a public name shaped like `10.a.b.c` does not
  resolve under `*`, the price of holding `10.0.0.1`.
- **A preconnect** (`<link rel=preconnect>`) opens a TCP connection, and sends nothing, to another
  port of an allowed host, or under `*` to any public name: seen in the tests. Enough to tell a
  listening port by the far end's log, not to carry data. `<a ping>`, beacons, WebSockets of
  workers and frames and WebRTC's UDP are checked in the tests not to get out.
- **A server killed with SIGKILL** leaves its temporary directory, what pages stored in it, until
  the next server starts on that machine as that user and removes it. The SDK's client does this
  to a server not gone within 4 seconds of closing.
- **Any local process can take the 64 connections**, for 30 seconds at a time each, before it
  shows a token: other clients get a 503 meanwhile.
- **Hard links** into a grant are refused on writing only: one already there, made by someone
  with access to the disk, reads through. Between the checks and the engine's open of a path
  (`media_*`) a link swapped in by another local user would be followed; `file_*`, `page_upload`
  and `pdf_open` do not have this gap. An attacker who can change a granted directory is not what
  this is built against.
- **PDF and media are parsed in this process**, outside the renderer's sandbox, as the
  engine's `zebkit.pdf` is (`docs/API.md`): a sandboxed utility process is on the roadmap. Limits
  on sizes and pixels are there (a decoding media tool: 4 GiB of output, 240 seconds, stopped
  between packets; an image: 64 million pixels by its header); a bug in PDFium or FFmpeg would
  be the server's. A file that lies in its header about its streams can pass the size check of
  `media_extract_frames`; the time limit still holds. A tool on PDF has no time limit.
- **`page_evaluate` runs what an agent writes** in the page: it has what the page has, inside the
  origins. Take it away with `--deny-tools page_evaluate` where an agent should not.
- **An allowed origin is trusted with the data sent to it**: the server cannot tell an agent
  posting a file there from one misled into it. Grant directories and origins together with care.
- **Sessions are not principals.** Any holder of the token sees every page and document; HTTP
  sessions exist for the handshake's sake only.
- **Uploads are by contents**, so the file's name reaches the page and its path does not, and a
  page sees them as in memory, up to 64 MiB. The engine's local paths are not used, on purpose.
- **Cancelling** drops the answer; it does not stop the work.
- **Windows:** the standard-input transport is Unix for now. The directory checks have not
  been looked at for that system's paths (alternate streams, device names, `\\?\`), and a
  file opened there can follow a last link: do not rely on them there yet.
- **Device flow against real providers is untested**: the tests use a local provider. Google and
  Microsoft may treat the request's `Origin` header differently.
- **Not run on macOS.**
