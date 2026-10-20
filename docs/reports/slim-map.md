# Engine size by origin: `slim`

- Source: the linker map of `libzebkit.so.1`, so these are the bytes of its loaded sections.
- Total attributed: 121.9 MiB (106.4 code, 12.8 rodata, 2.3 relro, 0.5 data, 0.0 relocs)
- `relro` is data that needs load-time relocation, mostly vtables; `relocs` is
  the relocations themselves, which the linker makes and charges to no source.

| Component | Code | Read-only data | Relro | Data | Relocs | Total | Share |
|---|---:|---:|---:|---:|---:|---:|---:|
| `third_party/blink/renderer/core` | 14.3 | 0.8 | 0.2 | 0.0 | 0.0 | 15.3 | 12.6% |
| `v8/v8_base_without_compiler` | 9.7 | 0.4 | 0.2 | 0.1 | 0.0 | 10.3 | 8.4% |
| `content/browser` | 6.7 | 0.1 | 0.1 | 0.0 | 0.0 | 6.8 | 5.6% |
| `v8/v8_compiler` | 5.2 | 0.1 | 0.0 | 0.0 | 0.0 | 5.4 | 4.4% |
| `third_party/blink/renderer/bindings` | 4.7 | 0.1 | 0.5 | 0.0 | 0.0 | 5.3 | 4.3% |
| `skia` | 5.0 | 0.2 | 0.0 | 0.0 | 0.0 | 5.2 | 4.3% |
| `third_party/angle` | 4.2 | 0.3 | 0.1 | 0.2 | 0.0 | 4.8 | 3.9% |
| `linker-generated` | 0.0 | 4.4 | 0.0 | 0.0 | 0.0 | 4.4 | 3.6% |
| `C runtime and other` | 2.1 | 1.6 | 0.0 | 0.0 | 0.0 | 3.7 | 3.0% |
| `third_party/webrtc` | 3.0 | 0.1 | 0.0 | 0.0 | 0.0 | 3.1 | 2.5% |
| `third_party/blink/renderer/platform` | 2.4 | 0.2 | 0.1 | 0.0 | 0.0 | 2.7 | 2.2% |
| `net` | 2.2 | 0.4 | 0.0 | 0.0 | 0.0 | 2.6 | 2.1% |
| `third_party/rust` | 2.2 | 0.4 | 0.0 | 0.0 | 0.0 | 2.6 | 2.1% |
| `v8/v8_snapshot` | 2.2 | 0.1 | 0.0 | 0.0 | 0.0 | 2.3 | 1.9% |
| `third_party/icu` | 1.5 | 0.4 | 0.1 | 0.0 | 0.0 | 1.9 | 1.6% |
| `third_party/blink/public` | 1.9 | 0.0 | 0.0 | 0.0 | 0.0 | 1.9 | 1.6% |
| `cc` | 1.7 | 0.0 | 0.0 | 0.0 | 0.0 | 1.7 | 1.4% |
| `third_party/ffmpeg` | 1.4 | 0.1 | 0.1 | 0.0 | 0.0 | 1.7 | 1.4% |
| `services/network` | 1.6 | 0.0 | 0.0 | 0.0 | 0.0 | 1.7 | 1.4% |
| `third_party/boringssl` | 1.4 | 0.1 | 0.1 | 0.0 | 0.0 | 1.6 | 1.3% |
| `third_party/harfbuzz` | 1.2 | 0.1 | 0.0 | 0.0 | 0.0 | 1.3 | 1.0% |
| `base` | 1.1 | 0.0 | 0.0 | 0.1 | 0.0 | 1.2 | 1.0% |
| `gpu/command_buffer` | 1.1 | 0.0 | 0.0 | 0.0 | 0.0 | 1.2 | 1.0% |
| `third_party/perfetto` | 1.0 | 0.1 | 0.0 | 0.0 | 0.0 | 1.2 | 1.0% |
| `third_party/tflite` | 1.1 | 0.0 | 0.0 | 0.0 | 0.0 | 1.2 | 0.9% |
| `components/viz` | 1.1 | 0.0 | 0.0 | 0.0 | 0.0 | 1.1 | 0.9% |
| `third_party/libvpx` | 1.0 | 0.1 | 0.0 | 0.0 | 0.0 | 1.1 | 0.9% |
| `third_party/xnnpack` | 1.0 | 0.0 | 0.0 | 0.0 | 0.0 | 1.1 | 0.9% |
| `third_party/brotli` | 0.3 | 0.4 | 0.0 | 0.0 | 0.0 | 0.8 | 0.6% |
| `net/third_party` | 0.8 | 0.0 | 0.0 | 0.0 | 0.0 | 0.8 | 0.6% |
| `net/http` | 0.0 | 0.7 | 0.0 | 0.0 | 0.0 | 0.8 | 0.6% |
| `ui/views` | 0.6 | 0.0 | 0.0 | 0.0 | 0.0 | 0.6 | 0.5% |
| `device` | 0.6 | 0.0 | 0.0 | 0.0 | 0.0 | 0.6 | 0.5% |
| `third_party/sqlite` | 0.5 | 0.0 | 0.0 | 0.0 | 0.0 | 0.5 | 0.4% |
| `components/services` | 0.5 | 0.0 | 0.0 | 0.0 | 0.0 | 0.5 | 0.4% |
| everything under 0.5 MiB | 21.0 | 1.6 | 0.5 | 0.1 | 0.0 | 23.2 | 19.0% |
