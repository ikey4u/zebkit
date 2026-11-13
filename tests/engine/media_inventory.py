"""Media outside the light DOM, stable IDs, and no accidental GC roots: what
View.media_list() lists of a page's audio and video elements, connected,
detached, in open, closed and nested shadow roots.

    python3 tests/engine/media_inventory.py [engine switches...]  (--js-flags=--expose-gc)

Exit 0 when every check passes.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import check, ev, finish, wait, zebkit  # noqa: E402

PAGE = """(() => {
  const keep = window.mediaInventory = {};
  const connected = document.createElement('audio');
  connected.id = 'connected'; document.body.append(connected);
  keep.detached = new Audio(); keep.detached.id = 'detached';
  for (const mode of ['open', 'closed']) {
    const host = document.createElement('div'); document.body.append(host);
    const root = host.attachShadow({mode});
    const video = document.createElement('video'); video.id = mode; root.append(video);
    if (mode === 'closed') {
      const nested = document.createElement('div'); root.append(nested);
      const audio = document.createElement('audio'); audio.id = 'nested';
      nested.attachShadow({mode: 'closed'}).append(audio);
    }
  }
  return true;
})()"""


def inventory(view):
    """path -> id of what media_list() gives; None when it gave none."""
    box = []
    view.media_list(lambda media, error: box.append((media, error)))
    if not wait(lambda: box) or box[0][1] is not None:
        return None
    return {entry["path"]: entry["id"] for entry in box[0][0] or []}


def main():
    if zebkit.init(["media-inventory"] + sys.argv[1:]) != 0:
        print("FAIL zebkit.init()")
        return 1
    try:
        view = zebkit.View()
        made = ev(view, PAGE)
        check(made is True, "the page's media made", made)
        first = inventory(view)
        check(first is not None and set(first) == {"audio#connected", "audio#detached", "video#open",
                                                   "video#closed", "audio#nested"},
              "media inventory includes detached Audio, open/closed and nested shadow roots", first)
        again = inventory(view)
        check(first is not None and again == first, "its IDs stay the same from list to list", again)
        collected = ev(view, "mediaInventory.detached = null; gc(); gc(); true")
        check(collected is True, "the detached Audio dropped and collected (--js-flags=--expose-gc)", collected)
        after = inventory(view)
        check(after is not None and "audio#detached" not in after,
              "media inventory does not keep unreachable detached elements alive", after)
        check(first is not None and after == {k: v for k, v in first.items() if k != "audio#detached"},
              "and the rest keep their IDs", after)
        view.close()
    finally:
        check(zebkit.shutdown(), "zebkit.shutdown()")
    return finish()


if __name__ == "__main__":
    sys.exit(main())
