# zebkit 0.6.1 (draft)

**A draft.** The notes of 0.6.1, written before its build; the archives'
sizes and the runs are filled at release.

PDFs open on iPhones again: iOS's PDFium allocates with the system's malloc,
where PartitionAlloc's address-space reservation failed and stopped the
application on every PDF. iOS is now built on Linux like the other systems,
with the iPhone SDK copied from a Mac once. FFmpeg encodes H.264 and AAC and
streams over RTMP and RTSP, with TLS; pages can be kept off mDNS.

| Archive | System | Unpacked | `.tar.xz` |
|---|---|---:|---:|
| `zebkit-0.6.1-x86_64-unknown-linux-gnu.tar.xz` | Linux x86-64, glibc 2.17 or newer | To fill at release | To fill at release |
| `zebkit-0.6.1-aarch64-apple-darwin.tar.xz` | macOS, Apple silicon | To fill at release | To fill at release |
| `zebkit-0.6.1-x86_64-apple-darwin.tar.xz` | macOS, Intel | To fill at release | To fill at release |
| `zebkit-0.6.1-x86_64-pc-windows-msvc.tar.xz` | Windows 10 or newer, x64 | To fill at release | To fill at release |
| `zebkit-0.6.1-android.tar.xz` | Android 10 (API 29) or newer: arm64 and x86-64 (the emulator), without the engine | To fill at release | To fill at release |
| `zebkit-0.6.1-ios.tar.xz` | iOS 15 or newer: devices and the simulator (arm64), without the engine | To fill at release | To fill at release |

The Rust crates `zebkit`, `zebkit-rs` and `zebkit-build` 0.6.1 download these
archives, each checked against its SHA-256 in `bindings/rust/zebkit-rs/engines.txt`.
The Python binding `zebkit` 0.6.1 is the binding alone; its engine is this
release's archive. The Go module is tagged `bindings/golang/v0.6.1`.

## What changed since 0.6.0

**PDFs on iOS.** `zebkit.pdf.open` stopped the application on every PDF on
iPhones (iOS 26): PDFium allocated in PartitionAlloc's partitions, whose
pools reserve more address space than iOS gives an application without the
extended virtual addressing entitlement, and PartitionAlloc ends the process
when the reservation fails (`HandlePoolAllocFailure`). The iOS profiles set
`pdf_use_partition_alloc = false`: PDFium allocates with the system's
malloc, and libzebkit on iOS no longer reserves those pools. The Swift
package's tests, `testPdf` among them, pass on the simulator.

**iOS built on Linux** (`docs/MOBILE.md`). Like macOS: Chromium's clang and
`ld64.lld` compile and link iOS on any host, and what only a Mac has is
Apple's SDKs. `scripts/ios-toolchain.sh` lays out the iPhone and simulator
SDKs and Xcode's Swift libraries, made on a Mac once (about 40 MB);
`scripts/build.sh` gives an iOS profile's build their paths and versions
(patches/1091) and builds it without dSYMs (patches/1092, as there is no
`dsymutil` on Linux); `scripts/package-mobile.sh` writes
`Zebkit.xcframework` without Xcode; `scripts/release-ios.sh` runs on Linux.
Only testing needs a Mac. The simulator's framework now says
`iPhoneSimulator` in its Info.plist.

**FFmpeg: encoding and live streams** (patches/1090,
`scripts/ffmpeg-config.sh`). H.264 (the engine's OpenH264) and AAC
encoders, FLV in and out, ADTS, RTMP, RTMPT and RTSP, and over TLS on the
engine's BoringSSL RTMPS, RTMPTS and RTSPS: `zebkit_ffmpeg` encodes,
publishes and records live streams (`-c:v h264`, `-c:a aac`, `-re`,
`-listen`, `-rtsp_transport`, `-rw_timeout`, TLS verification),
cancellable inside network waits. Capture documents opt-in software WebGL
(SwiftShader) for machines without a GPU. Built and tested on Linux; the
macOS and Windows configurations are regenerated.

**WebRTC and mDNS.** The runtime setting `webrtcIpHandlingPolicy`, for the
engine and per view, with Chromium's four policies (a view's before the
engine's, then `--force-webrtc-ip-handling-policy`, then the default): sent
to open pages without a reload, kept across navigations, a popup's from its
opener. With `disable_non_proxied_udp` a page gets no host candidate and
nothing goes to mDNS, the multicast behind macOS's local network prompt; the
library's pages stay at it.

**macOS windows.** An "overlay" title bar's unified toolbar is set aside
while the window is fullscreen, so the page fills the screen instead of
sitting below a blank band, and the title bar slides in with the menu bar.

**File choosers.** On Linux the engine says on stderr why a page's chooser
was cancelled when no file chooser portal answers, rather than cancelling
without a word. The GPUI browser opens a page's chooser as it asks (several
files, a folder, a directory, a place to save), with five minutes to choose.

**Scripts.** `fetch-upstream.sh` no longer exits silently when the checkout
has no system other than its own.

## Tested

To fill at release: the suite's counts, and the runs on the test machines.
