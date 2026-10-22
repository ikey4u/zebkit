# Size report: `baseline` on macOS

- Upstream: WebKit's Mac port at wpewebkit 2.54.1
- Measured: 2026-10-12 on Darwin arm64, from the package directory `scripts/package.sh` assembles
- **Total: 128.0 MiB**: 125.5 of code and 2.5 of resources
- Expects from the system: macOS 27.0 or newer

Headers are for building against the engine and are not counted.

| In the package | Code, MiB | Resources, MiB |
|---|---:|---:|
| `WebCore.framework` | 54.1 | 1.5 |
| `JavaScriptCore.framework` | 28.0 | 0.0 |
| `WebKit.framework` | 23.8 | 1.0 |
| `libwebrtc.dylib` | 10.2 | 0.0 |
| `libANGLE-shared.dylib` | 3.7 | 0.0 |
| `WebKitLegacy.framework` | 3.0 | 0.0 |
| `WebGPU.framework` | 2.4 | 0.0 |
| `com.apple.WebKit.WebContent.Development.xpc` | 0.0 | 0.0 |
| `com.apple.WebKit.WebContent.xpc` | 0.0 | 0.0 |
| `com.apple.WebKit.WebContent.EnhancedSecurity.xpc` | 0.0 | 0.0 |
| `com.apple.WebKit.WebContent.CaptivePortal.xpc` | 0.0 | 0.0 |
| `com.apple.WebKit.GPU.xpc` | 0.0 | 0.0 |
| `com.apple.WebKit.Networking.xpc` | 0.0 | 0.0 |
| `com.apple.WebKit.Model.xpc` | 0.0 | 0.0 |
