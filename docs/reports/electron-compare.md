# zebkit and Electron 45.2.1, measured the same way

`scripts/electron-compare.sh`, 2026-11-02, Linux x86-64: zebkit `slim` (Chromium 155.0.8059.84) and Electron 45.2.1, the latest stable release on npm, on one machine. Each cell is the median and, in parentheses, the lowest and highest run. A result is called within noise when those ranges overlap. `docs/SIZE_AND_SPEED.md` has what was held the same and what could not be.

## Size

MiB. As published is what each project hands out; as an application ships it is the runtime trimmed as far as it goes without building it, with the same thin host: Electron with one locale and no licence file, zebkit without the headers an application is built against.

| | zebkit | Electron | Smaller |
|---|---:|---:|---|
| As published, uncompressed | 160.8 | 273.4 | zebkit, 41% less |
| As published, `.tar.xz` | 54.9 | 88.7 | zebkit, 38% less |
| As published, `.zip` | 73.1 | 117.6 | zebkit, 38% less |
| As an application ships it, uncompressed | 152.3 | 260.5 | zebkit, 42% less |
| As an application ships it, `.tar.xz` | 53.5 | 87.1 | zebkit, 39% less |
| As an application ships it, `.zip` | 71.1 | 114.8 | zebkit, 38% less |

## Engines


### Startup, files not in cache, new profile

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| to the page's request, ms | 3742 (3666-3904) | 5775 (5518-5911) | zebkit, 35% less |
| to first paint (FCP), ms | 4720 (4478-4838) | 6337 (6196-6594) | zebkit, 26% less |
| to a frame after load, ms | 4691 (4493-4854) | 6347 (6176-6557) | zebkit, 26% less |
| CPU until then, all processes, s | 0.33 (0.29-0.4) | 0.56 (0.53-0.58) | zebkit, 41% less |

Load before a run: 3.5 to 4.5. Runs: 7 of each.

### Startup, files in cache, profile kept

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| to the page's request, ms | 53.3 (51.7-54.7) | 176 (173-186) | zebkit, 70% less |
| to first paint (FCP), ms | 120 (114-127) | 232 (225-252) | zebkit, 48% less |
| to a frame after load, ms | 119 (113-128) | 232 (226-250) | zebkit, 48% less |
| CPU until then, all processes, s | 0.22 (0.21-0.27) | 0.47 (0.46-0.48) | zebkit, 53% less |
| memory at that frame, MB | 175 (174-175) | 250 (248-250) | zebkit, 30% less |

Load before a run: 3.2 to 3.7. Runs: 7 of each.

### Text layout, a chapter

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| time per layout, the median of the last 50, ms | 9.2 (9-9.4) | 7.8 (7.7-8) | Electron, 15% less |
| renderer main thread's CPU, the mean of all 60 with the first 10, ms | 12.9 (12.6-13.0) | 10.7 (10.6-11.1) | Electron, 18% less |

Load before a run: 2.8 to 3.1. Runs: 7 of each.

### JavaScript and DOM, tests/bench/index.html

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| all, ms | 1300 (1267-1478) | 1196 (1170-1412) | Electron, 8% less, within noise |
| loop, ms | 39.0 (38.0-136) | 38.0 (38.0-168) | Electron, 3% less, within noise |
| objects, ms | 92.0 (86.0-102) | 91.0 (89.0-106) | Electron, 1% less, within noise |
| strings, ms | 26.0 (26.0-27.0) | 26.0 (26.0-29.0) | level |
| json, ms | 205 (203-211) | 223 (214-257) | zebkit, 8% less |
| regex, ms | 239 (236-243) | 217 (210-246) | Electron, 9% less, within noise |
| sort, ms | 91.0 (85.0-93.0) | 91.0 (88.0-97.0) | level |
| parse, ms | 48.0 (44.0-52.0) | 45.0 (42.0-67.0) | Electron, 6% less, within noise |
| gc, ms | 94.0 (86.0-128) | 122 (89.0-133) | zebkit, 23% less, within noise |
| jit, ms | 57.0 (56.0-62.0) | 58.0 (55.0-63.0) | zebkit, 2% less, within noise |
| dom, ms | 410 (384-440) | 281 (273-295) | Electron, 31% less |

Load before a run: 3.0 to 4.2. Runs: 5 of each.

### Calls from the page to the host and back

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| 64 bytes, ms | 0.4 (0.4-0.4) | 0.1 (0.1-0.1) | Electron, 75% less |
| 64 KiB, ms | 1.8 (1.8-1.9) | 0.4 (0.4-0.4) | Electron, 78% less |
| 1 MiB, ms | 32.1 (31.8-32.4) | 4.3 (4.2-4.4) | Electron, 87% less |
| 16 MiB, ms | 540 (528-550) | 69.2 (68.8-72.5) | Electron, 87% less |

Load before a run: 3.2 to 4.2. Runs: 5 of each.

### Long document (200,000 words)

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| to its first frame, ms | 210 (198-212) | 158 (155-281) | Electron, 25% less, within noise |
| longest task, ms | 228 (215-229) | 173 (171-190) | Electron, 24% less |
| scrolling frame, p50, ms | 16.7 (16.6-16.7) | 16.7 (16.6-16.7) | level |
| scrolling frame, p95, ms | 17.4 (17.3-17.5) | 17.3 (17.3-17.4) | Electron, 1% less, within noise |
| scrolling frame, max, ms | 17.8 (17.7-20.3) | 17.8 (17.7-30.6) | level |
| CPU, all processes, s | 3.7 (3.62-4.06) | 4.32 (4.11-4.43) | zebkit, 14% less |
| memory, MB | 253 (253-253) | 333 (332-359) | zebkit, 24% less |

Load before a run: 2.1 to 3.4. Runs: 5 of each.

## Local pages

### blank

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 0.25 (0.22-0.25) | 0.51 (0.46-0.51) | zebkit, 51% less |
| CPU at rest (20 s after), % of a core, % | 0 (0-0) | 0 (0-0) | level |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0 (0-0) | level |
| memory of all processes, MB | 173 (172-173) | 249 (247-252) | zebkit, 31% less |
| processes | 7 (7-7) | 6 (6-6) | Electron, 14% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 1.7 to 2.4. Runs: 5 of each.

Titles, zebkit: ['Blank (5)']; Electron: ['Blank (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:mojom': 2, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:mojom': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

### static

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 0.28 (0.27-0.29) | 0.53 (0.5-0.54) | zebkit, 47% less |
| CPU at rest (20 s after), % of a core, % | 0 (0-0) | 0 (0-0.05) | level |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0 (0-0) | level |
| memory of all processes, MB | 178 (178-179) | 258 (256-258) | zebkit, 31% less |
| processes | 7 (7-7) | 6 (6-6) | Electron, 14% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 1.7 to 2.3. Runs: 5 of each.

Titles, zebkit: ['Static article (5)']; Electron: ['Static article (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:mojom': 2, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:mojom': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

### animation

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 0.84 (0.76-0.91) | 0.96 (0.94-1.11) | zebkit, 12% less |
| CPU at rest (20 s after), % of a core, % | 5.9 (5.26-6.3) | 4.96 (4.81-5.45) | Electron, 16% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0.3 (0.25-0.35) | 0.3 (0.25-0.35) | level |
| memory of all processes, MB | 186 (186-186) | 261 (259-263) | zebkit, 29% less |
| processes | 7 (7-7) | 6 (6-6) | Electron, 14% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 1.6 to 3.1. Runs: 5 of each.

Titles, zebkit: ['Animation (5)']; Electron: ['Animation (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:mojom': 2, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:mojom': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

### heavy

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 1.08 (1.07-1.1) | 1.3 (1.25-1.3) | zebkit, 17% less |
| CPU at rest (20 s after), % of a core, % | 0.59 (0.55-0.74) | 0.64 (0.5-0.69) | zebkit, 8% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0 (0-0.05) | level |
| memory of all processes, MB | 328 (328-328) | 405 (404-408) | zebkit, 19% less |
| processes | 7 (7-7) | 6 (6-6) | Electron, 14% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 1.9 to 2.7. Runs: 5 of each.

Titles, zebkit: ['Heavy page (0) (5)']; Electron: ['Heavy page (0) (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:mojom': 2, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:mojom': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

### long-pdf

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 0.73 (0.73-0.77) | 1.59 (1.56-1.61) | zebkit, 54% less |
| CPU at rest (20 s after), % of a core, % | 0 (0-0) | 0 (0-0.05) | level |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0 (0-0) | level |
| memory of all processes, MB | 253 (253-253) | 400 (398-401) | zebkit, 37% less |
| processes | 7 (7-7) | 8 (8-8) | zebkit, 12% less |
| renderers | 1 (1-1) | 3 (3-3) | zebkit, 67% less |

Load before a run: 1.8 to 3.1. Runs: 5 of each.

Titles, zebkit: ['127.0.0.1:38723/long.pdf (5)']; Electron: ['PDF32000.book (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:mojom': 2, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:mojom': 1, 'zygote': 2, 'renderer': 3, 'gpu-process': 1}.

Sums of the medians over 5 pages, zebkit / Electron: CPU loading 3.2 / 4.9 s (zebkit less on 5), CPU at rest 6.5 / 5.6 % of a core (zebkit less on 1), memory 1118 / 1573 MB (zebkit less on 5).

## Sites

### en.wikipedia.org/wiki/Charles_Darwin

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 1.08 (1.05-1.41) | 1.3 (1.26-1.52) | zebkit, 17% less, within noise |
| CPU at rest (20 s after), % of a core, % | 0.05 (0-0.2) | 0.1 (0-0.2) | zebkit, 50% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0 (0-0) | level |
| memory of all processes, MB | 286 (283-314) | 348 (345-376) | zebkit, 18% less |
| processes | 7 (7-7) | 6 (6-6) | Electron, 14% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 1.8 to 2.5. Runs: 5 of each.

Titles, zebkit: ['Charles Darwin - Wikipedia (5)']; Electron: ['Charles Darwin - Wikipedia (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:mojom': 2, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:mojom': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

### www.google.com/

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 1.23 (0.79-1.29) | 1.51 (0.99-1.59) | zebkit, 19% less, within noise |
| CPU at rest (20 s after), % of a core, % | 0.35 (0.3-3.12) | 0.3 (0.25-3.08) | Electron, 14% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0.05) | 0.05 (0-0.05) | zebkit, 100% less, within noise |
| memory of all processes, MB | 251 (247-256) | 323 (320-328) | zebkit, 22% less |
| processes | 7 (7-7) | 6 (6-6) | Electron, 14% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 1.8 to 2.2. Runs: 5 of each.

Titles, zebkit: ['Google (5)']; Electron: ['Google (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:mojom': 2, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:mojom': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

### www.youtube.com/

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 3.15 (0.89-3.62) | 5.17 (0.54-5.3) | zebkit, 39% less, within noise |
| CPU at rest (20 s after), % of a core, % | 4.61 (1.88-15.5) | 3.32 (2.13-25.7) | Electron, 28% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0.05) | 0 (0-0.15) | level |
| memory of all processes, MB | 497 (482-503) | 554 (528-561) | zebkit, 10% less |
| processes | 9 (9-9) | 8 (8-8) | Electron, 11% less |
| renderers | 2 (2-2) | 2 (2-2) | level |

Load before a run: 1.8 to 2.5. Runs: 5 of each.

Titles, zebkit: ['YouTube (5)']; Electron: ['YouTube (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:mojom': 3, 'zygote': 2, 'renderer': 2, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:mojom': 2, 'zygote': 2, 'renderer': 2, 'gpu-process': 1}.

### github.com/torvalds/linux

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 2.84 (2.62-2.87) | 2.88 (2.42-3.13) | zebkit, 1% less, within noise |
| CPU at rest (20 s after), % of a core, % | 0.15 (0.05-0.55) | 0.1 (0.1-2.88) | Electron, 33% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0 (0-0.05) | level |
| memory of all processes, MB | 330 (322-338) | 408 (394-414) | zebkit, 19% less |
| processes | 7 (7-7) | 6 (6-6) | Electron, 14% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 1.9 to 2.7. Runs: 5 of each.

Titles, zebkit: ['GitHub - torvalds/linux: Linux kernel source tree  (5)']; Electron: ['GitHub - torvalds/linux: Linux kernel source tree  (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:mojom': 2, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:mojom': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

### www.amazon.com/

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 0.5 (0.47-1.03) | 0.98 (0.71-1.43) | zebkit, 49% less, within noise |
| CPU at rest (20 s after), % of a core, % | 57.0 (55.8-59.9) | 68.2 (58.9-70.2) | zebkit, 16% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0.94 (0.79-1.04) | 0.99 (0.79-1.24) | zebkit, 5% less, within noise |
| memory of all processes, MB | 555 (536-566) | 577 (532-583) | zebkit, 4% less, within noise |
| processes | 9 (9-9) | 8 (6-8) | Electron, 11% less |
| renderers | 2 (2-2) | 2 (1-2) | level |

Load before a run: 2.0 to 3.0. Runs: 5 of each.

Titles, zebkit: ['Amazon.com. Spend less. Smile more. (5)']; Electron: ['Amazon.com. Spend less. Smile more. (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:mojom': 3, 'zygote': 2, 'renderer': 2, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:mojom': 2, 'zygote': 2, 'renderer': 2, 'gpu-process': 1}.

### www.reddit.com/r/programming/

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 0.57 (0.21-1.52) | 2.5 (0.59-2.7) | zebkit, 77% less, within noise |
| CPU at rest (20 s after), % of a core, % | 5.65 (0.79-6.99) | 11.8 (10.3-14.2) | zebkit, 52% less |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0.05) | 0 (0-0.05) | level |
| memory of all processes, MB | 338 (330-344) | 511 (411-547) | zebkit, 34% less |
| processes | 8 (8-8) | 7 (6-8) | Electron, 12% less, within noise |
| renderers | 2 (2-2) | 2 (1-2) | level |

Load before a run: 2.0 to 2.8. Runs: 5 of each.

Titles, zebkit: ['Reddit - Prove your humanity (5)']; Electron: ['programming (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:mojom': 2, 'zygote': 2, 'renderer': 2, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:mojom': 2, 'zygote': 2, 'renderer': 2, 'gpu-process': 1}.

### www.nytimes.com/

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 2.48 (0.38-3.71) | 4.58 (0.64-5.92) | zebkit, 46% less, within noise |
| CPU at rest (20 s after), % of a core, % | 67.3 (59.0-70.7) | 57.4 (44.1-72.2) | Electron, 15% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0.45 (0.25-0.55) | 0.3 (0.25-0.4) | Electron, 33% less, within noise |
| memory of all processes, MB | 811 (799-842) | 898 (873-967) | zebkit, 10% less |
| processes | 21.0 (21.0-22.0) | 20.0 (20.0-30.0) | Electron, 5% less, within noise |
| renderers | 14.0 (14.0-15.0) | 14.0 (14.0-24.0) | level |

Load before a run: 2.3 to 3.8. Runs: 5 of each.

Titles, zebkit: ['The New York Times - Breaking News, US News, World (5)']; Electron: ['The New York Times - Breaking News, US News, World (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:mojom': 3, 'zygote': 2, 'renderer': 14, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:mojom': 2, 'zygote': 2, 'renderer': 14, 'gpu-process': 1}.

### www.bbc.com/news

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 1.84 (1.58-2.18) | 2.13 (2.01-2.29) | zebkit, 14% less, within noise |
| CPU at rest (20 s after), % of a core, % | 19.1 (13.9-20.7) | 16.1 (13.9-21.5) | Electron, 16% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0.05 (0-0.05) | 0 (0-0.05) | Electron, 100% less, within noise |
| memory of all processes, MB | 459 (431-506) | 524 (494-565) | zebkit, 12% less, within noise |
| processes | 11.0 (10.0-14.0) | 10.0 (9-12.0) | Electron, 9% less, within noise |
| renderers | 5 (4-8) | 5 (4-7) | level |

Load before a run: 1.8 to 2.7. Runs: 5 of each.

Titles, zebkit: ['BBC News - Breaking news, video and the latest top (5)']; Electron: ['BBC News - Breaking news, video and the latest top (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:mojom': 2, 'zygote': 2, 'renderer': 5, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:mojom': 1, 'zygote': 2, 'renderer': 5, 'gpu-process': 1}.

### www.theverge.com/

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 3.11 (0.2-3.78) | 3.73 (3.3-4.13) | zebkit, 17% less, within noise |
| CPU at rest (20 s after), % of a core, % | 24.4 (17.3-45.2) | 24.1 (9.66-32.4) | Electron, 1% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0.1) | 0 (0-0) | level |
| memory of all processes, MB | 630 (559-718) | 738 (570-829) | zebkit, 15% less, within noise |
| processes | 11.0 (11.0-15.0) | 23.0 (8-25.0) | zebkit, 52% less, within noise |
| renderers | 5 (5-9) | 18.0 (3-20.0) | zebkit, 72% less, within noise |

Load before a run: 1.9 to 4.3. Runs: 5 of each.

Titles, zebkit: ['The Verge (5)']; Electron: ['The Verge (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:mojom': 2, 'zygote': 2, 'renderer': 6, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:mojom': 1, 'zygote': 2, 'renderer': 3, 'gpu-process': 1}.

### stackoverflow.com/questions/11227809

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 0.87 (0.2-1.33) | 1.02 (0.45-1.4) | zebkit, 15% less, within noise |
| CPU at rest (20 s after), % of a core, % | 46.2 (0.3-47.5) | 27.2 (3.12-41.8) | Electron, 41% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0.1 (0.05-0.1) | 0.1 (0-0.1) | level |
| memory of all processes, MB | 725 (172-737) | 558 (271-785) | Electron, 23% less, within noise |
| processes | 20.0 (7-21.0) | 10.0 (6-17.0) | Electron, 50% less, within noise |
| renderers | 14.0 (1-15.0) | 5 (1-12.0) | Electron, 64% less, within noise |

Load before a run: 2.2 to 3.0. Runs: 5 of each.

Titles, zebkit: ['Error (1)', 'java - Why is conditional processing of a sorted a (4)']; Electron: ['Just a moment... (1)', 'java - Why is conditional processing of a sorted a (4)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:mojom': 2, 'zygote': 2, 'renderer': 14, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:mojom': 1, 'zygote': 2, 'renderer': 12, 'gpu-process': 1}.

### www.apple.com/

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 1.34 (1.27-1.36) | 1.47 (1.46-1.52) | zebkit, 9% less |
| CPU at rest (20 s after), % of a core, % | 0.1 (0.05-0.15) | 0 (0-0.05) | Electron, 100% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0 (0-0) | level |
| memory of all processes, MB | 310 (305-318) | 360 (359-362) | zebkit, 14% less |
| processes | 7 (7-7) | 6 (6-6) | Electron, 14% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 1.7 to 2.2. Runs: 5 of each.

Titles, zebkit: ['Apple (5)']; Electron: ['Apple (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:mojom': 2, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:mojom': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

### developer.mozilla.org/en-US/docs/Web/JavaScript

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 0.78 (0.77-0.83) | 1.01 (0.98-1.02) | zebkit, 23% less |
| CPU at rest (20 s after), % of a core, % | 0 (0-0.05) | 0 (0-0.1) | level |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0 (0-0) | level |
| memory of all processes, MB | 232 (231-233) | 314 (312-316) | zebkit, 26% less |
| processes | 7 (7-7) | 6 (6-6) | Electron, 14% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 1.7 to 2.6. Runs: 5 of each.

Titles, zebkit: ['JavaScript | MDN (5)']; Electron: ['JavaScript | MDN (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:mojom': 2, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:mojom': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

Left out of the sums, for showing a different page in some run: www.reddit.com/r/programming/, stackoverflow.com/questions/11227809.

Sums of the medians over 10 pages, zebkit / Electron: CPU loading 18.4 / 24.8 s (zebkit less on 10), CPU at rest 173.1 / 169.7 % of a core (zebkit less on 2), memory 4362 / 5044 MB (zebkit less on 10).

Runs discarded for a machine that was not quiet: 0. Runs that failed: 0.
