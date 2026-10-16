# patches

Source changes on top of the pinned upstream release (see `upstream.lock`).

- `series` lists the patches to apply, one filename per line, in order.
  `#` starts a comment. `scripts/fetch-upstream.sh` applies them with
  `patch -p1` and rebuilds the tree whenever the list or a patch changes.
- `series.macos` is the list for macOS, where the engine is WebKit's Mac
  port, built from the same release in git. `series.windows` is the list
  for Windows, where it is WebKit's Windows port, from git as well. A patch
  can be in more than one list.
- Name patches `NNNN-short-description.patch`. Group by purpose:
  `0xxx` fixes: for flag combinations upstream does not test, and for
  upstream bugs,
  `1xxx` size reductions, `2xxx` platform ports and what the engine
  needs for zebkit's own features (the CDP endpoint).
- Each patch starts with a short header saying what it does, why, and how
  many bytes it saves if it is a size patch.

Keep patches small and independent. Every one of them has to be rebased on
each upstream bump, so prefer a build flag in `profiles/` whenever one exists.
