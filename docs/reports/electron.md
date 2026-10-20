# Size report: Electron

- Electron 44.9.0 (Chromium 152.0.7977.167, Node.js included), Linux x86-64
- Application: `examples/electron-hello`, a window with one page, in `resources/app`
- Measured: 2026-10-18 by `scripts/electron-baseline.sh`, every file in the package, uncompressed
- **As Electron ships: 283.3 MiB**
- **Trimmed without rebuilding (only `en-US.pak` of 55 locales, no `LICENSES.chromium.html`): 255.7 MiB**

| | Uncompressed | `.tar.xz` (xz -6) | `.zip` (deflate -9) |
|---|---:|---:|---:|
| As Electron ships | 283.3 | 87.1 | 117.2 |
| Trimmed | 255.7 | 85.3 | 112.6 |

In MiB. The `.tar.xz` is compressed as zebkit's releases are (`scripts/release.sh`).

| File | MiB |
|---|---:|
| `locales/` (55 files) | 8.6 |
| `electron` | 218.9 |
| `LICENSES.chromium.html` | 19.2 |
| `resources.pak` | 12.0 |
| `icudtl.dat` | 10.4 |
| `libvk_swiftshader.so` | 4.4 |
| `libffmpeg.so` | 2.7 |
| `libvulkan.so.1` | 2.4 |
| `chrome_crashpad_handler` | 1.8 |
| `chrome_200_percent.pak` | 1.2 |
| `v8_context_snapshot.bin` | 0.7 |
| `chrome_100_percent.pak` | 0.7 |
| `snapshot_blob.bin` | 0.4 |
| 7 smaller files | 0.0 |
