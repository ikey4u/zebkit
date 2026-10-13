# Size report: `slim` on macOS

- Upstream: WebKit's Mac port at wpewebkit 2.54.1
- Measured: 2026-10-13 on Darwin arm64, from the package directory `scripts/package.sh` assembles
- **Total: 92.3 MiB**: 89.7 of code and 2.5 of resources
- Expects from the system: macOS 27.0 or newer

Headers are for building against the engine and are not counted.

| In `Zebkit.framework` | Code, MiB | Resources, MiB |
|---|---:|---:|
| `WebCore.framework` | 33.3 | 1.5 |
| `JavaScriptCore.framework` | 22.8 | 0.0 |
| `WebKit.framework` | 20.0 | 1.0 |
| `libwebrtc.dylib` | 6.3 | 0.0 |
| `libANGLE-shared.dylib` | 3.3 | 0.0 |
| `WebKitLegacy.framework` | 2.4 | 0.0 |
| `WebGPU.framework` | 1.4 | 0.0 |
| `com.apple.WebKit.WebContent.Development.xpc` | 0.0 | 0.0 |
| `com.apple.WebKit.WebContent.xpc` | 0.0 | 0.0 |
| `com.apple.WebKit.WebContent.EnhancedSecurity.xpc` | 0.0 | 0.0 |
| `com.apple.WebKit.WebContent.CaptivePortal.xpc` | 0.0 | 0.0 |
| `com.apple.WebKit.GPU.xpc` | 0.0 | 0.0 |
| `com.apple.WebKit.Networking.xpc` | 0.0 | 0.0 |
| `com.apple.WebKit.Model.xpc` | 0.0 | 0.0 |
| `Zebkit.framework` | 0.0 | 0.0 |
