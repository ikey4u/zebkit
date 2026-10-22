# Engine size by origin: `slim`

- Source: the linker map of `libzebkit.so.1`, so these are the bytes of its loaded sections.
- Total attributed: 135.5 MiB (118.9 code, 13.8 rodata, 2.3 relro, 0.5 data, 0.0 relocs)
- `relro` is data that needs load-time relocation, mostly vtables; `relocs` is
  the relocations themselves, which the linker makes and charges to no source.

| Component | Code | Read-only data | Relro | Data | Relocs | Total | Share |
|---|---:|---:|---:|---:|---:|---:|---:|
| `third_party/blink/renderer/core` | 18.3 | 0.8 | 0.2 | 0.0 | 0.0 | 19.4 | 14.3% |
| `v8/v8_base_without_compiler` | 12.1 | 0.3 | 0.2 | 0.1 | 0.0 | 12.7 | 9.4% |
| `content/browser` | 6.8 | 0.1 | 0.1 | 0.0 | 0.0 | 7.0 | 5.1% |
| `third_party/blink/renderer/bindings` | 5.7 | 0.1 | 0.5 | 0.0 | 0.0 | 6.2 | 4.6% |
| `v8/v8_compiler` | 5.8 | 0.1 | 0.0 | 0.0 | 0.0 | 5.9 | 4.4% |
| `skia` | 4.7 | 0.1 | 0.0 | 0.0 | 0.0 | 4.9 | 3.6% |
| `linker-generated` | 0.0 | 4.2 | 0.0 | 0.0 | 0.0 | 4.3 | 3.1% |
| `third_party/angle` | 3.7 | 0.3 | 0.1 | 0.2 | 0.0 | 4.3 | 3.1% |
| `C runtime and other` | 2.3 | 1.6 | 0.0 | 0.0 | 0.0 | 3.9 | 2.9% |
| `third_party/blink/renderer/platform` | 3.2 | 0.2 | 0.0 | 0.0 | 0.0 | 3.4 | 2.5% |
| `third_party/webrtc` | 3.2 | 0.1 | 0.0 | 0.0 | 0.0 | 3.3 | 2.5% |
| `third_party/pdfium` | 1.5 | 1.4 | 0.0 | 0.0 | 0.0 | 2.9 | 2.1% |
| `net` | 2.3 | 0.4 | 0.0 | 0.0 | 0.0 | 2.7 | 2.0% |
| `v8/v8_snapshot` | 2.2 | 0.1 | 0.0 | 0.0 | 0.0 | 2.3 | 1.7% |
| `third_party/ffmpeg` | 2.0 | 0.2 | 0.2 | 0.0 | 0.0 | 2.3 | 1.7% |
| `third_party/rust` | 1.9 | 0.4 | 0.0 | 0.0 | 0.0 | 2.3 | 1.7% |
| `third_party/icu` | 1.6 | 0.4 | 0.1 | 0.0 | 0.0 | 2.1 | 1.5% |
| `third_party/blink/public` | 1.9 | 0.0 | 0.0 | 0.0 | 0.0 | 2.0 | 1.5% |
| `services/network` | 1.7 | 0.0 | 0.0 | 0.0 | 0.0 | 1.7 | 1.3% |
| `cc` | 1.6 | 0.0 | 0.0 | 0.0 | 0.0 | 1.6 | 1.2% |
| `third_party/boringssl` | 1.4 | 0.1 | 0.1 | 0.0 | 0.0 | 1.5 | 1.1% |
| `gpu/command_buffer` | 1.2 | 0.0 | 0.0 | 0.0 | 0.0 | 1.2 | 0.9% |
| `third_party/tflite` | 1.2 | 0.0 | 0.0 | 0.0 | 0.0 | 1.2 | 0.9% |
| `base` | 1.1 | 0.0 | 0.0 | 0.1 | 0.0 | 1.2 | 0.9% |
| `third_party/perfetto` | 1.0 | 0.1 | 0.0 | 0.0 | 0.0 | 1.2 | 0.9% |
| `third_party/libvpx` | 1.1 | 0.1 | 0.0 | 0.0 | 0.0 | 1.2 | 0.9% |
| `third_party/harfbuzz` | 1.1 | 0.1 | 0.0 | 0.0 | 0.0 | 1.1 | 0.8% |
| `third_party/xnnpack` | 1.1 | 0.0 | 0.0 | 0.0 | 0.0 | 1.1 | 0.8% |
| `components/viz` | 1.0 | 0.0 | 0.0 | 0.0 | 0.0 | 1.0 | 0.7% |
| `net/third_party` | 0.8 | 0.0 | 0.0 | 0.0 | 0.0 | 0.8 | 0.6% |
| `third_party/brotli` | 0.3 | 0.4 | 0.0 | 0.0 | 0.0 | 0.8 | 0.6% |
| `net/http` | 0.0 | 0.7 | 0.0 | 0.0 | 0.0 | 0.7 | 0.6% |
| `ui/views` | 0.7 | 0.0 | 0.0 | 0.0 | 0.0 | 0.7 | 0.5% |
| `device` | 0.7 | 0.0 | 0.0 | 0.0 | 0.0 | 0.7 | 0.5% |
| `third_party/sqlite` | 0.6 | 0.0 | 0.0 | 0.0 | 0.0 | 0.6 | 0.4% |
| `ui/gfx` | 0.5 | 0.0 | 0.0 | 0.0 | 0.0 | 0.5 | 0.4% |
| `components/services` | 0.5 | 0.0 | 0.0 | 0.0 | 0.0 | 0.5 | 0.4% |
| everything under 0.5 MiB | 22.2 | 1.6 | 0.5 | 0.1 | 0.0 | 24.3 | 18.0% |
