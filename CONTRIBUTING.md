# Contributing

Contributions are welcome as pull requests on GitHub. By sending one you
agree that it is licensed under the GNU LGPL 2.1 or later, as the rest of
zebkit (`LICENSE`, `docs/LICENSING.md`).

## Building

`docs/BUILD.md` has every step. On Linux x86-64, with about 120 GB of disk:

```sh
scripts/fetch-upstream.sh       # Chromium into upstream/chromium/src, patches applied
scripts/build.sh slim           # libzebkit.so.1 and zebkit-helper
scripts/package.sh slim         # out/blink/slim/package
```

macOS and Windows are built on Linux too (`docs/MACOS.md`, `docs/WINDOWS.md`);
a macOS build can also be made on an Apple silicon Mac
(`scripts/dist.sh release`). A change in zebkit's own code rebuilds in
minutes; a change of compiler flags rebuilds everything.

## Testing

```sh
mise run check:static           # scripts/check.sh --static: bash -n, Python syntax, check-bindings.py; no build
scripts/engine-test.sh slim     # every layer of the API, from programs built against the package
scripts/glibc-test.sh slim      # the C tests on glibc 2.17
scripts/check-bindings.py       # every binding has every function of the C API
scripts/check-surfaces.py       # src/engine/surfaces.json agrees with the code
mise run check                  # scripts/check.sh: all of it, strict, as before a release
```

`scripts/engine-test.sh <profile> --only REGEX` runs some tests;
`--fetch` gets what some of them take from the network (into `.cache`);
`--strict` (`ENGINE_TEST_STRICT=1`) fails on a skipped test or part of one,
except those `ENGINE_TEST_EXPECTED_SKIPS` names. `mise run check` is strict
with the short list of skips a Linux build machine cannot avoid, written out
in `scripts/check.sh` (`docs/BUILD.md`, "Testing"); a release is published
only after it has passed on the same sources. For macOS
and Windows the suite is built on Linux (`--build-only`) and run on the
machine (`--prebuilt`; `docs/MACOS.md`, `docs/WINDOWS.md`). The Rust crates
have `cargo test` in `bindings/rust`, the Go binding the checks in
`bindings/golang/internal/` (`bindings/golang/README.md`).

## The rules a change is checked against

- **Every binding has the whole C API.** A function added to a header in
  `src/engine/include/zebkit/` is added to Rust, Python, Go and host
  JavaScript (`tools/zebkit-js`) in the same change;
  `scripts/check-bindings.py` fails otherwise. A binding never gives more
  than C does.
- **Every surface is in the registry.** What the engine shows the person
  or asks of the system is listed in `src/engine/surfaces.json` with its
  default, its status and its tests (`docs/SYSTEM_INTEGRATION.md`);
  `scripts/check-surfaces.py` fails otherwise.
- **The same functions on Linux, macOS and Windows** (`docs/ROADMAP.md`,
  "Requirements").
- **Patch Chromium rather than work around it**, and prefer a build switch
  in `profiles/` to a patch. A patch is named
  `NNNN-short-description.patch`, begins with what it does and why (and what
  it saves, if it is for size), and is listed in `patches/series`
  (`patches/README.md`).
- **Size against speed.** A change that costs more in size than it gains in
  speed is not taken; measure both (`docs/SIZE_AND_SPEED.md`).
- **Defaults are safe.** A page gets nothing a careful browser would ask
  about unless the application decides.

## Documentation

A change that changes behaviour changes the document that describes it
(`README.md`, `docs/`), and adds a line to `docs/releases/next.md`.
Numbers in the documents come from the scripts that measure them, with
their date and versions.

## Security

Vulnerabilities are reported privately: `SECURITY.md`.
