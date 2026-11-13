# The sandbox

Pages run in sandboxed processes, on every system, as in Chrome. What each
system needs differs: on Linux something of the system's, on macOS nothing,
on Windows a call in the application's executable. `--no-sandbox` turns it
off on each, and is never silent.

## Linux

Chromium's Linux sandbox needs one of two things
from the system: unprivileged user namespaces, or a set-user-ID helper. Containers
and build machines often have neither. The answer (owner, 2026-10-31) is both of the
following, and running without a sandbox is never silent.

### What the engine does

At the start of the browser process, in this order:

1. `--no-sandbox`: it starts, and says on standard error
   `zebkit: running pages without the sandbox (--no-sandbox)`.
2. Unprivileged user namespaces work: they are used, and nothing is said.
3. The helper `chrome-sandbox` beside the engine is installed: it is used.
4. None of these: the engine refuses to start and says what to do. `zebkit_main()`
   returns non-zero and `zebkit_init()` false; there is no crash, and pages never run
   unsandboxed by default. A helper that is there but not root's and 4755, or that is on a
   file system mounted `nosuid`, counts as none, and the message says which.

As root Chromium supports no sandbox: the engine refuses and names `--no-sandbox`.

### The choices

**Allow user namespaces.** Nothing of zebkit's to install. On Ubuntu 23.10 and later an
AppArmor profile for the application, or `sysctl
kernel.apparmor_restrict_unprivileged_userns=0`; on older Debian `sysctl
kernel.unprivileged_userns_clone=1`; elsewhere `user.max_user_namespaces`.

**Install the helper.** The package has `chrome-sandbox`, Chromium's
`sandbox/linux:chrome_sandbox`, about 60 KiB, not set-user-ID: a tarball unpacked by
a user cannot make a file root's. Installing it is a step of its own, run by root:

```sh
sudo <package>/zebkit-sandbox-install
```

It makes the helper root's, mode 4755, and refuses a package in a directory that someone
else can write to or that belongs to another user, since they could replace what root
is about to bless, and tells a `nosuid` mount. The helper does one thing, starting the
page processes in new namespaces; it is what Chrome ships on the same systems. Removing it:
delete the file. To package zebkit for a distribution, do the same in the post-install step.

**`--no-sandbox`.** For a container that is itself the isolation, and only there: pages
then run with everything the process has. In a container that runs as root it is the only
choice Chromium has. `tools/zebkit-mcp` takes it after `--`, stated by whoever runs the
server: `zebkit-mcp ... -- --no-sandbox`. It is never its default.

### The tools

`scripts/engine-test.sh` passes `--no-sandbox` by name to each program, for the machines it
runs on, and `tests/engine/sandbox_startup.py` runs without it to check the refusal.
The MCP server, `zebkit-run`, `zebkit-js` and the examples start with the sandbox unless told
otherwise.

### What has been tested

The refusal, its message and `--no-sandbox` run in the suite. The helper itself cannot run on
the build machine, where no root is to be had; **it has not been run from this package**. See
`docs/ROADMAP.md`.

`scripts/sandbox-helper-test.sh <release .tar.xz>`, run as root on a test machine, is a step of
every release: it installs the helper as above and checks that the page processes get a PID
namespace of their own from it. Without root it checks the package's side and skips the rest;
the suite runs it so, as `sandbox-helper`.

## macOS

The helper, `Zebkit Helper.app` inside the framework, enters the seatbelt sandbox
itself, as Chrome's helpers do; nothing is installed and the application does
nothing for it (`docs/MACOS.md`). With the release archive the renderer, GPU and
utility processes run sandboxed (checked on 2026-11-09). The test suite runs with
`--no-sandbox` on a Mac: in its test bundle, signed ad hoc, the helpers crashed at
their start on 2026-11-01, for a cause not found (`docs/MACOS.md`, "Not run").

## Windows

Chromium's sandbox writes into a child at addresses from its own image, so the
process that starts the children and the children must be one executable: the
application links `zebkit_sandbox.lib`, with the static C runtime (`/MT`), and calls
`zebkit_win_startup()` first in `main()` (`<zebkit/win_sandbox.h>`). Without the call
the engine does not start, and says what to do; with `--no-sandbox` the children are
`zebkit-helper.exe`. Renderers run at untrusted integrity and the GPU process at low,
each in a job; the network service is not sandboxed, as upstream's default. The
release archive's tools are built this way. The details: `docs/WINDOWS.md`, "The
sandbox" and "The network service".
