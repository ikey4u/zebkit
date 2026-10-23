# Roadmap: the UI layer

zebkit's own UI layer: applications written in React, Solid or Vue, run by
the engine's V8, laid out and drawn by the engine itself, with no DOM, no
CSS engine and no web view, fast enough for 60 frames a second everywhere
and 120 where the screen has them, and looking and behaving as each system's
own applications do on macOS, Windows and Linux, then iOS and Android. The
engine's roadmap is `docs/ROADMAP.md`; what `zebkit.ui` is today,
`docs/API.md` and `docs/ARCHITECTURE.md`.

## Requirements

| Requirement | From |
|---|---|
| Applications in React, Solid or Vue (TSX, Tailwind classes), drawn natively: no DOM, no CSS engine, no web view for the application's own interface | Owner, 2026-10-23 |
| The technique of PocketJS (components send changes to a tree; a native core lays it out and draws it) with the native look of Native Shell (nsh), on one layer | Owner, 2026-10-23 |
| The look and behaviour of each system: macOS, Windows and Linux first, iOS and Android after | Owner, 2026-10-23 |
| The same application code on every system | Owner |
| iOS through the App Store: V8 without its JIT, no Blink; web pages in the system's WKWebView | Owner, 2026-10-23 |
| Web pages as one element among the others: Blink on the desktop and Android, WKWebView on iOS | Owner, 2026-10-23 |
| Agents drive the interface as they drive pages: the tree read, clicked, typed into, captured | Owner (zebkit is for agents) |

## Where it comes from

The layer takes ideas, not code. Native Shell as a library was looked at on
2026-10-20 and not taken: it brings a second JavaScript engine, GTK 4, and
second copies of libraries the engine has.

| From | What it does | What zebkit takes |
|---|---|---|
| PocketJS | Solid, Vue Vapor or Octane components send changes to one native tree; a Rust core lays it out with flexbox, resolves Tailwind classes from a table made at build time and draws every pixel, in one thread, on QuickJS | The tree fed by changes, the class table made at build time, layout and drawing in native code, frames that can be replayed |
| Native Shell (nsh) | TSX and Solid's signals on QuickJS; each system's own widgets (AppKit, Win32, GTK 4, UIKit, Android views); flexbox and grid; system text, input and accessibility | The look and behaviour of each system, system text and input, the unstyled foundation (navigation, dialogs, safe areas) under an application's own components |
| Chromium, in the engine already | V8 with every JIT tier; Skia; the compositor (layers, tiling, animations off the main thread); HarfBuzz and ICU; IME (`ui/base/ime`); accessibility (`ui/accessibility`); views' text field | Everything below the tree: script, drawing, compositing, text, input, accessibility |

## Where it stands

`zebkit.ui` (2026-10-20) is the first step: windows of Chromium's views from
a script run by `zebkit-js`, with signals, `Show` and `For`, `h` for JSX, a
subset of Tailwind's classes read at run time, and `dump`, `click`, `type`
and `capture` for tests and agents. Its elements are `view`, `text`,
`button`, `input`, `image`, `scroll` and `web`; its layout is views' box
layout (one direction, gap, padding, alignment, grow). Its changes reach the
native side as one batch per turn of the loop. `tests/engine/ui/app.mjs`
checks it on Linux and macOS.

What it lacks for the goal: flexbox and grid, drawing of its own (views'
widgets look like Chrome's interface, not like the system's), animation,
lists that build only the rows in sight, React and Vue, the systems' looks,
iOS and Android.

## Design

```text
the application: React, Solid or Vue components (TSX, Tailwind classes)
    | an adapter per framework: React's reconciler, Solid's renderer, Vue Vapor's
    v
changes to the tree, one batch per turn, in a binary form          V8
------------------------------------------------------------------------
the core (C++, in the engine)
    the tree of nodes; styles from the class table made at build time
    layout: flexbox and grid
    text: HarfBuzz, ICU, Skia's paragraphs; the system's fonts
    a display list per node, drawn with Skia
    the compositor: layers, scrolling and animations off the script's thread
    hit testing, focus, keyboard, pointer, gestures
------------------------------------------------------------------------
the look, one per system: controls drawn to the system's measures
the system, one per system: windows, IME, menus, dialogs, clipboard,
    drag and drop, accessibility, the system's colours and settings
```

- **One tree, drawn by zebkit.** Controls are drawn with Skia to each
  system's measures, colours, fonts and motion, not made of the system's
  widgets: one tree is laid out and composited in one pass, animates
  without crossing into a toolkit, and behaves the same under every
  framework. What only the system can do well stays the system's: text
  input and IME, menus and context menus, file and colour dialogs, and the
  window's frame and its buttons.
- **Changes in, nothing out per frame.** The script sends changes; it is
  not asked anything while a frame is made. Layout, drawing and animation
  run without it, so a busy script does not stop scrolling or an
  animation.
- **Classes resolved at build time.** An esbuild plugin turns the class
  literals of an application into a table of styles; at run time a class is
  a lookup. Classes made at run time are parsed when the application is run
  for development, and refused in a release build unless the application
  asks for the parser.
- **The same layer everywhere.** The core and the adapters are the same on
  every system; the look and the system part are one each. On iOS the core
  runs without Blink and with V8 without its JIT.

## Phases

Each phase ends with what it is checked by. Linux and macOS go together
from the first; Windows when the engine runs there (`docs/ROADMAP.md`).

### U0. Ground

- The measures, before any code: start to the first frame, memory, frames
  while scrolling 10,000 rows and while animating, CPU at rest. The same
  interface three ways: today's `zebkit.ui`, a page in zebkit, Electron.
- The decisions still open (below) taken.
- **Checked by:** the measures written down in `docs/SIZE_AND_SPEED.md`'s
  manner, with the programs that make them under `tests/bench/`.

### U1. The core

- The tree and its changes: create, set, insert, move, remove, in a binary
  batch from V8 without JSON.
- Layout: flexbox first, grid after, on the decided library.
- Styles: the class table (spacing, sizing, colours, borders, radii,
  shadows, typography, opacity, transforms, states such as `hover:` and
  `focus:`, dark mode), the esbuild plugin that makes it.
- Drawing: boxes, borders, radii, shadows, gradients, images, text with
  HarfBuzz and the system's fonts, clipping; a display list per node, drawn
  with Skia into the compositor's layers.
- Input: hit testing, pointer, wheel and touchpad, keyboard focus and
  order.
- Scrolling on the compositor, with each system's physics.
- `dump`, `click`, `type` and `capture` on the new tree.
- **Checked by:** today's `tests/engine/ui/app.mjs` passing on the new
  core; 10,000 rows scrolled at the screen's rate on Linux and macOS.

### U2. The frameworks

- Solid, from today's signals, with `Show`, `For`, `Switch`, `Index` and
  context.
- React through `react-reconciler`: components, hooks, context, Suspense;
  the host configuration sends U1's changes.
- Vue Vapor.
- TypeScript declarations for the elements, their properties and the
  classes; a project template; reloading a changed module while the
  application keeps its state.
- **Checked by:** one example application written in each framework,
  passing the same automated test.

### U3. Controls and the systems' looks

- The controls: button, toggle, checkbox, radio, slider, stepper, text
  field and text area, search field, select and combo box, menu button,
  segmented control, tabs, list and the virtual list (widgets only for the
  rows in sight), table, tree, progress and spinner, tooltip, popover,
  dialog and sheet, toolbar, sidebar, split view, scroll view.
- The unstyled foundation under them, as nsh's: navigation, focus scopes,
  dialogs, safe areas; an application can draw its own components on it.
- The looks: macOS (AppKit's measures, the system font, the accent colour,
  vibrancy), Windows (Fluent's measures, Segoe UI Variable, Mica), Linux
  (Adwaita's measures, the desktop's font and colours through the portal).
  Each follows the system's light or dark appearance, contrast, reduced
  motion and text size, and changes when they change.
- **Checked by:** each control captured beside the system's own, in light
  and dark, reviewed by a person on each system.

### U4. The system

- Text input with every input method (Chinese, Japanese, Korean),
  through `ui/base/ime` on the desktop: composition drawn in place, the
  candidate window where the caret is.
- Menus and context menus, the menu bar on macOS, file dialogs, the
  clipboard, drag and drop, notifications, the tray.
- Windows: several, with the system's frame or the application's own
  title bar and the system's buttons; full screen, minimum sizes.
- Accessibility: the tree given to VoiceOver, Narrator and Orca through
  `ui/accessibility`.
- Text in both directions, and every script ICU and HarfBuzz shape.
- **Checked by:** a person typing Chinese and Japanese on each system, and
  each screen reader reading the example applications.

### U5. Motion and speed

- Animations and transitions of transform, opacity and colour on the
  compositor; springs; layout changes animated.
- Frames at the screen's rate (120 on ProMotion and on 120 Hz screens),
  paced to the display.
- A trace of every frame, which can be replayed without the application,
  for finding a slow frame afterwards (as PocketJS records its frames).
- **Checked by:** U0's measures again: better than the same interface in a
  page and in Electron on start, memory and frames, and no frame missed
  in the scrolling and animation tests.

### U6. Pages inside

- The `web` element: a page among the controls, Blink on the desktop and
  Android, WKWebView on iOS; the application's `zebkit.*` bridge into it.
- **Checked by:** an application with a page inside it, the same on every
  system.

### U7. Tools

- A command to create, run and package an application, for every system.
- The tree in DevTools: elements, styles and layout of the native tree,
  served over CDP so that agents and DevTools see it as they see a page.
- Documentation for people and for models (TypeScript declarations,
  every element and class listed).
- **Checked by:** a new application made, run and packaged from nothing
  with the command, on each system.

### U8. Windows

- The look and system parts for Windows, once the engine runs there.
- **Checked by:** U1 to U7's checks on Windows.

### U9. iOS

- An iOS build of the engine's parts without content and Blink: V8 without
  its JIT (`--jitless`), Skia on Metal, the core, the libraries
  applications use (PDFium, SQLite, BoringSSL, ICU).
- UIKit as the system part: the view an application embeds, touch and
  gestures, `UITextInput` for text, VoiceOver, safe areas, the keyboard.
- The iOS look; the `web` element on WKWebView.
- The App Store's rules: no JIT; no code downloaded after review run on
  V8 (code delivered later may run only on the system's JavaScriptCore or
  WebKit), so updates of an application's script go through the store
  unless an application runs them on JavaScriptCore.
- **Checked by:** an example application accepted by TestFlight's review,
  and U5's measures on an iPhone without the JIT.

### U10. Android

- Android views as the system part, the Material look, Blink for the `web`
  element, V8 with its JIT.
- **Checked by:** U1 to U7's checks on an Android phone.

## Decided

| Decision | When |
|---|---|
| Native UI on Chromium's views, not Native Shell as a library (a second JavaScript engine, GTK 4, duplicates of the engine's libraries) | 2026-10-20 |
| zebkit's own UI layer: PocketJS's technique with Native Shell's native look, on the engine's V8, Skia and compositor; it takes the place of `zebkit.ui` on Chromium's views | 2026-10-23 |
| iOS without the JIT and without Blink; web pages in WKWebView | 2026-10-23 |
| React, Solid and Vue, on one tree | 2026-10-23 |

## Open

| Question | Proposed |
|---|---|
| Layout library | Taffy (flexbox, grid and block, in Rust, which Chromium builds) over Yoga (flexbox only, C++) |
| Controls drawn or the system's own | Drawn to the system's measures, with the system's own for text input, menus and dialogs (Design, above); a control the system draws better can be the system's |
| JavaScript on iOS | V8 without its JIT, the same engine as everywhere; JavaScriptCore for applications that update their script after review |
| The changes' form | A binary batch per turn, written by the adapters into one buffer |
| Today's `zebkit.ui` | Kept until U3 gives the new layer its controls, then replaced; `h`, `signal`, `Show` and `For` keep their names |

## Costs to plan for

- **The looks are the most work.** Three desktop looks and two phone
  looks, each followed as the systems change every year. nsh's size is
  mostly this.
- **Text input and accessibility** cannot be left for later: they decide
  whether the layer is usable at all, and each system's is different.
- **The JIT on iOS.** Without it V8 runs script several times slower; the
  layer keeps script out of every frame so that this shows in start and in
  work, not in scrolling.
- **iOS is a second build of the engine**, without content: its own
  profile, its own tests, and Chromium's iOS build for what it shares.
