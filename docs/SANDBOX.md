# The sandbox on Linux

Pages run in sandboxed processes. Chromium's Linux sandbox needs one of two things
from the system: unprivileged user namespaces, or a set-user-ID helper. Containers
and build machines often have neither. The answer (owner, 2026-10-31) is both of the
following, and running without a sandbox is never silent.

macOS has its own sandbox (`docs/MACOS.md`), which needs nothing installed. Windows has
Chromium's, with a call in the application's executable (`docs/WINDOWS.md`).

## What the engine does

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

## The choices

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

## The tools

`scripts/engine-test.sh` passes `--no-sandbox` by name to each program, for the machines it
runs on, and `tests/engine/sandbox_startup.py` runs without it to check the refusal.
The MCP server, `zebkit-run`, `zebkit-js` and the examples start with the sandbox unless told
otherwise.

## What has been tested

The refusal, its message and `--no-sandbox` run in the suite. The helper itself cannot run on
the build machine, where no root is to be had; **it has not been run from this package**. See
`docs/ROADMAP.md`.
