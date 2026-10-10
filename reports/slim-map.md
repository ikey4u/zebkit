# Engine size by origin: `slim`

- Source: the linker map, so these are the bytes in the file's loaded sections.
- Total attributed: 54.5 MiB (40.1 code, 11.3 rodata, 2.8 relro, 0.3 data)
- `relro` is data that needs load-time relocation, mostly vtables.

## By component

| Component | Code | Read-only data | Relro | Data | Total | Share |
|---|---:|---:|---:|---:|---:|---:|
| WebCore | 17.9 | 0.5 | 1.9 | 0.1 | 20.4 | 37% |
| JavaScriptCore | 10.9 | 0.6 | 0.3 | 0.0 | 11.8 | 22% |
| Third-party: icudata | 0.0 | 7.6 | 0.0 | 0.0 | 7.6 | 14% |
| WebKit | 4.4 | 0.0 | 0.3 | 0.0 | 4.7 | 9% |
| Skia | 2.5 | 0.1 | 0.1 | 0.0 | 2.7 | 5% |
| Linker-generated | 0.0 | 1.5 | 0.0 | 0.0 | 1.5 | 3% |
| WTF + allocator | 0.8 | 0.1 | 0.0 | 0.1 | 1.0 | 2% |
| Third-party: icuuc | 0.5 | 0.3 | 0.1 | 0.0 | 0.8 | 2% |
| Third-party: icui18n | 0.7 | 0.0 | 0.0 | 0.0 | 0.7 | 1% |
| Third-party: gcrypt | 0.4 | 0.0 | 0.0 | 0.0 | 0.5 | 1% |
| Third-party: harfbuzz | 0.4 | 0.1 | 0.0 | 0.0 | 0.5 | 1% |
| Third-party: stdc++ | 0.4 | 0.0 | 0.0 | 0.0 | 0.5 | 1% |
| Third-party: jpeg | 0.4 | 0.1 | 0.0 | 0.0 | 0.5 | 1% |
| Third-party: freetype | 0.3 | 0.1 | 0.0 | 0.0 | 0.4 | 1% |
| Third-party: webp | 0.2 | 0.0 | 0.0 | 0.0 | 0.2 | 0% |
| Third-party: fontconfig | 0.1 | 0.1 | 0.0 | 0.0 | 0.1 | 0% |
| Third-party: xml2 | 0.1 | 0.0 | 0.0 | 0.0 | 0.1 | 0% |
| Third-party: epoxy | 0.0 | 0.1 | 0.0 | 0.0 | 0.1 | 0% |
| Third-party: expat | 0.1 | 0.0 | 0.0 | 0.0 | 0.1 | 0% |
| Third-party: png | 0.1 | 0.0 | 0.0 | 0.0 | 0.1 | 0% |
| Third-party: gpg-error | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0% |
| Third-party: gcc_eh | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0% |
| Third-party: tasn1 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0% |

## Inside WebCore

| Area | MiB |
|---|---:|
| `JSBindings` | 4.9 |
| `rendering` | 1.9 |
| `css` | 1.3 |
| `html` | 1.2 |
| `platform` | 1.2 |
| `root` | 1.1 |
| `Modules` | 1.1 |
| `page` | 1.0 |
| `dom` | 0.9 |
| `style` | 0.8 |
| `svg` | 0.8 |
| `editing` | 0.5 |
| `loader` | 0.5 |
| `accessibility` | 0.4 |
| `workers` | 0.4 |
| `layout` | 0.4 |
| `inspector` | 0.3 |
| `generated: JS bindings` | 0.2 |
| `animation` | 0.2 |
| `generated: UserAgentScriptsData` | 0.1 |
| `crypto` | 0.1 |
| `generated: NodeName` | 0.1 |
| `text` | 0.1 |
| everything smaller | 0.7 |

## Inside JavaScriptCore

| Area | MiB |
|---|---:|
| `runtime` | 4.4 |
| `dfg` | 1.9 |
| `jit` | 0.8 |
| `yarr` | 0.7 |
| `wasm` | 0.7 |
| `parser` | 0.7 |
| `llint` | 0.5 |
| `bytecode` | 0.5 |
| `bytecompiler` | 0.3 |
| `API/glib` | 0.3 |
| `heap` | 0.2 |
| `inspector` | 0.2 |
| `lol` | 0.1 |
| `generated: JS bindings` | 0.1 |
| everything smaller | 0.4 |

## Inside WebKit

| Area | MiB |
|---|---:|
| `UIProcess` | 1.2 |
| `NetworkProcess` | 1.0 |
| `WebProcess` | 1.0 |
| `generated: GeneratedSerializersShared` | 0.2 |
| `UIProcess/API/glib` | 0.2 |
| `generated: WebPreferencesFeatures` | 0.1 |
| everything smaller | 0.9 |

## Inside Skia

| Area | MiB |
|---|---:|
| everything smaller | 2.7 |
