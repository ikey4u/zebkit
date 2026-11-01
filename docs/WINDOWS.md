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
| `content_shell.pak`, `icudtl.dat`, `snapshot_blob.bin` | resources, ICU's data, V8's snapshot |
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

- **Frames.** `zebkit_frame.handle` is a file mapping's `HANDLE`, and `fd` is -1.
  GPU textures (D3D11) are not done: an off-screen view's frames are shared memory.
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

Test programs are built on Linux and copied (`tests/engine/win_*.c` were written for
Windows; other tests build as they are or need POSIX parts ported). `scripts/engine-test.sh`
does not run on Windows yet.

## State

On Windows 10 19045, one CPU, VMware's display adapter, 2026-11-01:

| Test | Result |
|---|---|
| `win_smoke`: start, page, script, capture | the capture's pixels are the page's |
| `win_host`: own window and loop, view in its `HWND`, sandbox on and off | pixels read back from the screen; children's tokens checked |
| `win_offscreen`: `--headless`, frames | pixels through `frame->pixels` and the file mapping |
| `services` 25 of 25, `pdf` 99 of 99, `scheme` 11 of 11, `mode`, `edit_commands`, `automation_reentry` | pass, with `--no-sandbox` |
| `canvas` | 50 of 57, run without the Ahem font it is given by `engine-test.sh` |

## Left

- The rest of `scripts/engine-test.sh`: ported tests, Python and Rust bindings.
- D3D11 textures for off-screen frames; WebAuthn's and printing's system interfaces;
  `zebkit.ui` on Windows; Windows' own input in attached views.
- The size-trained inliner with `lld-link`; size against `slim`'s.
