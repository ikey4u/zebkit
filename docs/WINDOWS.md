# Windows

zebkit for Windows x64: `zebkit.dll`, with the same functions as on Linux and
macOS. It is cross-built on Linux with Chromium's `clang-cl` and `lld-link`,
against MSVC's C++ library and the Windows SDK (`scripts/windows-toolchain.sh`).

## Build

```sh
scripts/windows-toolchain.sh       # once: lays out MSVC and the SDK ($SDK_DIR/windows)
scripts/build.sh slim-windows
scripts/build.sh slim-windows-sandbox zebkit/engine:zebkit_sandbox
scripts/package.sh slim-windows
```

## The package

| File | What it is |
|---|---|
| `zebkit.dll` | the engine |
| `zebkit.dll.lib` | its import library, to link an application with |
| `zebkit-helper.exe` | child processes of an application that runs with `--no-sandbox` |
| `zebkit_sandbox.lib` | for the application's executable, below |
| `zebkit.pak`, `icudtl.dat`, `snapshot_blob.bin` | resources, ICU's data, V8's snapshot |
| `libEGL.dll`, `libGLESv2.dll` | ANGLE |
| `vk_swiftshader.dll`, `vk_swiftshader_icd.json`, `vulkan-1.dll` | drawing without a GPU |
| `include/zebkit/` | the headers |

The engine links the static C runtime, and its import table names only what
Windows has: nothing from Microsoft ships with it. `d3dcompiler_47.dll` is left
out, as Windows has its own (checked on Windows 10 19045).

An allocation of the engine's is freed by `zebkit_free()`: the application's C
runtime has a heap of its own. `argv` is taken as UTF-8, or else in the system's
code page; paths in the API are UTF-8.

## The sandbox

Chromium's sandbox writes into a child at addresses from its own image, so what
starts the children and the children must be one executable. On Windows, unlike
Linux and macOS, the application takes part: it links `zebkit_sandbox.lib`, with
the static C runtime (`/MT`), and calls one function first (`<zebkit/win_sandbox.h>`):

```c
int main(int argc, char **argv) {
    int code;
    if (zebkit_win_startup(&code))
        return code;   /* this process was one of the engine's children */
    /* the application, as on every system */
}
```

This costs the executable 1.2 MiB. Without the call the engine does not start,
and says what to do; `--no-sandbox` runs without a sandbox and logs a warning,
with `zebkit-helper.exe` as the children.

As Chromium's defaults: renderers at untrusted integrity, the GPU process at low,
each in a job. The network service is not sandboxed: `kNetworkServiceSandbox` is
off upstream, and needs permissions set on the profile's files first.

`zebkit_sandbox.lib` is built in a profile of its own
(`profiles/slim-windows-sandbox.gn`), without link-time optimization, since an
application's linker need not read LLVM's bitcode; `src/engine/win/flatten_lib.py`
keeps only what a link loads. `//base`'s Rust is not in it: nothing in
`zebkit_win_startup()` reaches JSON or Rust's log, and the C++ names they would call are
empty there (`src/engine/win/sandbox_startup.cc`).

## What differs

- **Scale.** A scale for each off-screen view is Chromium's capture scale override
  (`SetScaleOverrideForCapture`), not a display as on Linux: displays on Windows' headless
  screen are laid out by Chromium itself, and a window takes a scale only when resized.

- **JavaScript dialogs.** A window's `alert()`, `confirm()` and beforeunload with no
  handler are a message box, and `prompt()` gives null. This has not been run yet.
- **Frames.** `zebkit_frame.handle` is a file mapping's `HANDLE`, and `fd` is -1.
  `ZEBKIT_BUFFERS_GPU` asks for `ZEBKIT_FRAME_D3D11`, an NT handle to a shared texture.
  That path has never run: the test machine's display adapter has no D3D shared images,
  so frames stay in shared memory there (without the check the GPU process dies).
- **A view in the application's window.** `zebkit_view_attach_native()` takes an
  `HWND`.
- **The host's loop.** `zebkit_set_wakeup()`, `zebkit_init()`, `zebkit_pump()` under the
  application's own message loop, which also runs Chromium's window messages.
- **Cookies.** The key is a file in the profile, protected with DPAPI for this user.
- **Vendor libraries.** The header maps names with macros, as MSVC's compiler has no
  `#pragma redefine_extname`; each entry point is the linker's export of the function.
  FreeType's BDF and slot functions and two BoringSSL functions that are not built on
  Windows are not there; PDFium's `FPDF_RenderPage`, SQLite's and zlib's Windows
  functions are there, on Windows alone.
- **Profiles.** `use_ml_inliner` is off (`patches/1008` is for ELF linkers); PGO uses
  Chromium's `win64` profile.

## Testing without building there

Programs are built on Linux and copied, as for a Mac, and run under Git Bash:

```sh
scripts/engine-test.sh slim-windows --build-only DIR   # on Linux; C, and Rust if the target is installed
tar -C DIR -cJf bundle.tar.xz .                        # copy it to Windows, unpack it as DIR there
ENGINE_TEST_WINDOWS=1 DIR/scripts/engine-test.sh slim-windows --prebuilt DIR/programs [--only REGEX]
```

`tests/engine/win_compat` is the POSIX the tests use. `ENGINE_TEST_WINDOWS=1` adds what
takes the mouse and the keyboard. Python comes from the machine; nothing is installed.
A sandboxed child has no PATH of the parent's: `zebkit.dll` has to be beside the executable.

## State

Run the suite on NTFS. A shared folder has no ACLs, and there file key storage rightly refuses
to start and copies can arrive with a wrong bit.

Windows 10 19045 with a zh-CN system locale, 2 CPUs, VMware's display adapter, on NTFS,
2026-11-02: 143 sections of the suite, none failing; 1,868 of the 1,868 checks in those that print a total passed. Node.js's tests (Playwright, MCP, certificates over
HTTP/2) did not run.

Pass, among others: `win_smoke`, `win_host` and `win_native_parent` (23 checks, real
`SendInput`), with the sandbox and without; the control of `zebkit_sandbox.lib`'s stubs; `canvas`
(57), `pdf` (99), `pdf-hooks` (286), `api` (28), `automation`, `vendor` (48); hooks in C, JavaScript,
Python and Rust, downloads too; cookies across a restart with each key; WebAuthn with the virtual
authenticator; Rust with the sandbox.

A spare renderer kept by `spareRenderer` is not taken by a navigation to another site, here as on
a Mac: it goes, and another process starts (16 runs of 16, with or without 3 s before the
navigation). Not understood; `runtime-settings-python` does not ask it of either.

Windows' WebAuthn route (webauthn.dll) has never been tried: the test machine's `WebAuthNGetApiVersionNumber()`
is 0, so Chromium does not offer it; `win-webauthn` says so and skips.

Copies to a shared folder are not always intact: two copies of `zebkit.dll` had one wrong
bit each, and one build of the `services` program never arrived intact under any name. Five
copies to NTFS, all verified, had none. Compare SHA-256 after every copy.

## The network service

It runs unsandboxed, as upstream's default. A page's renderer has to be broken first, but then
the network service, with the user's rights and every site's cookies, is the next step, and
a parser bug there (HTTP, DNS, certificates) needs nothing else. To turn on
`kNetworkServiceSandbox`: an AppContainer for the process, access granted on every profile's
`Network` and cache directories before use (`content/browser/network_sandbox.cc`), which a
disk without ACLs cannot take, and proxy resolution, system DNS and certificate paths tested
under it. Not tried here. Chrome decides it in its own code, not by the default.

## Left

- `zebkit.ui` beyond its script's test; printing through the system's dialog.
- The example of frames in another process.
- D3D11 frames on a real GPU. The size-trained inliner with `lld-link`.

`zebkit.dll` is 185.1 MiB, `zebkit_sandbox.lib` 29.4 MiB, the package 239.9 MiB; as `.tar.xz`
without that library, which an application does not ship, 68.2 MiB (Linux's: 54.9 MiB, 160.9 MiB unpacked).
