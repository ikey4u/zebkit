# Security

## Reporting a vulnerability

Report it privately, through GitHub's security advisories: on the
repository's **Security** tab, **Report a vulnerability**
(<https://github.com/ikey4u/zebkit/security/advisories/new>). Do not open a
public issue or pull request for it.

Say what is affected (the version or commit, the system), what an attacker
needs (a page they control, a file the application opens, a local user),
and how to reproduce it. A proof of concept as a page or a short program
helps most. The report stays private until a fix is released.

## What is zebkit's, and what is Chromium's

zebkit is Chromium's engine with a small patch set (`patches/`) and an
embedder of its own (`src/engine/`), the bindings (`bindings/`) and the
tools (`tools/`).

- A bug in zebkit's own code, its patches, its defaults (what a page is
  granted, the `zebkit.*` object, the hooks, the sandbox's setup) or the
  tools, such as `zebkit-mcp`'s bounds: report it here.
- A bug in Chromium itself (Blink, V8, the network stack, PDFium, ...) that
  Chrome has too: report it to Chromium
  (<https://www.chromium.org/Home/chromium-security/reporting-security-bugs/>).
  zebkit takes Chromium's fix with the release that carries it.

## Which versions get fixes

Fixes go into `main` and into the next release. Older releases are not
updated: use the latest one. Each release is pinned to one Chromium
release (`upstream.lock`) and takes Chromium's security fixes by moving to a
newer one; `docs/releases/` says which Chromium each release has.

## What an application decides

Much of what keeps a page contained is the application's choice: which
origins get `zebkit.*` and which capabilities (`zebkit_view_grant`), what
the hooks answer, whether `--no-sandbox` is passed. `docs/API.md`,
`docs/SANDBOX.md` and `docs/MCP.md` say what each default is.
