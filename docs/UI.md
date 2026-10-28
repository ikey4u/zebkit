# zebkit.ui: the engine's own small UI

What `zebkit.ui` is for, what it promises, and what it does not. The
plan it replaces, a UI layer of zebkit's own (PocketJS's technique with
Native Shell's native look, applications in React, Solid or Vue drawn by
the engine on every system, iOS through it), was retired on 2026-10-28:
zebkit is a library applications integrate, and a host application has a
toolkit of its own.

## What it is

The window around a page, scriptable, and the default face of what the
engine has to ask a person.

- **For programs without a host UI:** `zebkit-js` scripts, `zebkit-run`,
  examples, tests and agents. An agent that opens a page for a person
  needs a window, a title, a prompt, a sign-in popup; that is
  `zebkit.ui`'s job.
- **For the engine itself:** JavaScript dialogs, permission prompts,
  popups, file and colour choosers, context menus. They are the default
  implementations of the decisions the hooks let a host take over
  (`docs/ROADMAP_HOOKS.md`): a host with a toolkit presents them
  natively, a script or an agent gets them as they are.

## What it promises

- A few stable elements: `window`, `view`, `text`, `button`, `input`,
  `image`, `scroll` and `web`, with signals, `Show` and `For`, `h` for
  JSX, a subset of Tailwind's classes, and `dump`, `click`, `type` and
  `capture` so that tests and agents see it and drive it, headless too.
- Chromium's own looks (its views toolkit), the same on Linux, macOS and
  Windows. No native look of the system's, no React or Vue, no parity
  with a UI framework.
- Its size: about what it is today, 1,200 lines. A widget is added when
  an engine feature needs one, a chooser or a prompt, not for building
  applications.

## Where rich UI goes

Into a page. HTML and CSS in a `web` element is the retained, animated
and accessible UI layer the engine ships, with every framework working,
text input and accessibility solved by Blink. `zebkit.ui` is the chrome
around that page, not an alternative to it. The rule for every later
"should `zebkit.ui` have X": layout, styling, animation and controls
belong in a page; a window, a system dialog and a prompt belong in
`zebkit.ui`.

## Where it sits

- C API: the windows, views attached to a window and their visibility in
  `<zebkit/zebkit.h>`; a host uses those to put views in windows of its
  own and ignores the rest.
- JavaScript: `zebkit.ui` in `zebkit-js` (`docs/API.md`), and through the
  MCP server for agents when it comes.
- Hooks: every dialog `zebkit.ui` shows is a hook the host can answer
  instead.

## What was weighed, and declined

Native Shell (nsh, v0.3.0, measured 2026-10-28): 619,000 lines of Rust
with tests; its Linux release runner is 32.3 MiB unstripped (about 26.5
stripped), linking GTK 4, Pango, Cairo, HarfBuzz and WebKitGTK from the
system besides. Of it, the core (tree, styles, flexbox and grid, text,
mount batches) is 3.3 MiB and 136,000 lines, the unstyled controls 2.8
MiB and 129,000 lines, a desktop backend 0.5 MiB and 23,000 to 39,000
lines each; the rest is what zebkit's engine already has (a JavaScript
engine, text, drawing, networking, codecs). The same capability in
zebkit, drawn with Skia on V8, HarfBuzz, ICU and cc, was estimated at 6
to 10 MiB and 150,000 to 200,000 lines, the looks, text input and
accessibility the larger part, followed as the systems change every
year: a product in itself, for applications zebkit is not meant to be
the base of. Its ideas, a tree changed in batches and signals, are in
`zebkit.ui` already; its code was not taken (2026-10-20: a second
JavaScript engine, GTK 4, duplicates of the engine's libraries).
