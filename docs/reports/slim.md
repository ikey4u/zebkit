# Size report: `slim`

- Upstream: wpewebkit 2.54.1
- Measured: 2026-10-12 on Linux x86_64, from the package directory `scripts/package.sh` assembles
- **Total: 83.1 MiB** (over the 60.0 MiB budget)
- Oldest glibc it can run on: 2.17
- Expects from the system: ld-linux-x86-64.so.2 libc.so.6 libdl.so.2 libm.so.6 libpthread.so.0 libresolv.so.2 librt.so.1

## Engine: 76.7 MiB

| File | MiB |
|---|---:|
| `libzebkit-bundle.so` | 0.0 |
| `libzebkit.so.1` | 76.7 |
| `zebkit-network` | 0.0 |
| `zebkit-web` | 0.0 |

## Bundled libraries: 6.4 MiB in 7 files

Files under 0.2 MiB are summed into the last row. Libraries linked
statically are inside the engine and counted there.

| File | MiB |
|---|---:|
| `gio/modules/libgiognutls.so` | 1.9 |
| `libgio-2.0.so.0` | 1.4 |
| `libglib-2.0.so.0` | 1.4 |
| `libsoup-3.0.so.0` | 0.8 |
| `libsqlite3.so.0` | 0.6 |
| `libgobject-2.0.so.0` | 0.3 |
| 1 smaller files | 0.0 |
