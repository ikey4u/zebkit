# Size report: Electron

- Electron 45.0.0-beta.5 (Chromium 156.0.8078.36, Node.js included), Linux x86-64
- Application: `examples/electron-hello`, a window with one page, in `resources/app`
- Measured: 2026-10-19 by `scripts/electron-baseline.sh`, every file in the package, uncompressed
- **As Electron ships: 273.9 MiB**
- **Trimmed without rebuilding (only `en-US.pak` of 55 locales, no `LICENSES.chromium.html`): 261.0 MiB**

| | Uncompressed | `.tar.xz` (xz -6) | `.zip` (deflate -9) |
|---|---:|---:|---:|
| As Electron ships | 273.9 | 88.9 | 117.8 |
| Trimmed | 261.0 | 87.3 | 115.0 |

In MiB. The `.tar.xz` is compressed as zebkit's releases are (`scripts/release.sh`).

| File | MiB |
|---|---:|
| `locales/` (55 files) | 7.6 |
| `electron` | 224.1 |
| `resources.pak` | 12.9 |
| `icudtl.dat` | 10.3 |
| `LICENSES.chromium.html` | 5.3 |
| `libvk_swiftshader.so` | 4.4 |
| `libffmpeg.so` | 2.7 |
| `libvulkan.so.1` | 2.4 |
| `chrome_crashpad_handler` | 1.6 |
| `chrome_200_percent.pak` | 0.9 |
| `v8_context_snapshot.bin` | 0.7 |
| `chrome_100_percent.pak` | 0.5 |
| `snapshot_blob.bin` | 0.4 |
| 7 smaller files | 0.0 |
