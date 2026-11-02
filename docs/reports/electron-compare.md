# zebkit and Electron 45.2.1, measured the same way

`scripts/electron-compare.sh`, 2026-11-02, Linux x86-64: zebkit `slim` (Chromium 155.0.8059.84) and Electron 45.2.1, the latest stable release on npm, on one machine. Each cell is the median and, in parentheses, the lowest and highest run. A result is called within noise when those ranges overlap. `docs/SIZE_AND_SPEED.md` has what was held the same and what could not be.

## Size

MiB. As published is what each project hands out; as an application ships it is the runtime trimmed as far as it goes without building it, with the same thin host: Electron with one locale and no licence file, zebkit without the headers an application is built against.

| | zebkit | Electron | Smaller |
|---|---:|---:|---|
| As published, uncompressed | 170.1 | 273.4 | zebkit, 38% less |
| As published, `.tar.xz` | 57.7 | 88.7 | zebkit, 35% less |
| As published, `.zip` | 77.5 | 117.6 | zebkit, 34% less |
| As an application ships it, uncompressed | 161.7 | 260.5 | zebkit, 38% less |
| As an application ships it, `.tar.xz` | 56.4 | 87.1 | zebkit, 35% less |
| As an application ships it, `.zip` | 75.5 | 114.8 | zebkit, 34% less |

## Engines


### Startup, files not in cache, new profile

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| to the page's request, ms | 3464 (3190-3770) | 5651 (5404-5794) | zebkit, 39% less |
| to first paint (FCP), ms | 4619 (4387-4934) | 6243 (5889-6343) | zebkit, 26% less |
| to a frame after load, ms | 4604 (4347-4893) | 6282 (5902-6369) | zebkit, 27% less |
| CPU until then, all processes, s | 0.31 (0.29-0.32) | 0.58 (0.56-0.62) | zebkit, 47% less |

Load before a run: 3.8 to 4.7. Runs: 7 of each.

### Startup, files in cache, profile kept

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| to the page's request, ms | 53.7 (51.8-55.5) | 177 (172-184) | zebkit, 70% less |
| to first paint (FCP), ms | 117 (115-121) | 232 (222-237) | zebkit, 49% less |
| to a frame after load, ms | 116 (110-124) | 230 (226-239) | zebkit, 50% less |
| CPU until then, all processes, s | 0.22 (0.2-0.23) | 0.45 (0.45-0.48) | zebkit, 51% less |
| memory at that frame, MB | 169 (169-170) | 239 (238-240) | zebkit, 29% less |

Load before a run: 3.9 to 4.2. Runs: 7 of each.

### Text layout, a chapter

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| time per layout, the median of the last 50, ms | 8.6 (8.2-8.9) | 7.8 (7.2-8) | Electron, 9% less |
| renderer main thread's CPU, the mean of all 60 with the first 10, ms | 12.1 (11.6-12.3) | 10.6 (10.3-10.8) | Electron, 12% less |

Load before a run: 3.7 to 4.4. Runs: 7 of each.

### JavaScript and DOM, tests/bench/index.html

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| all, ms | 1278 (1196-1508) | 1166 (1154-1297) | Electron, 9% less, within noise |
| loop, ms | 38.0 (38.0-188) | 39.0 (38.0-162) | zebkit, 3% less, within noise |
| objects, ms | 91.0 (87.0-132) | 92.0 (89.0-104) | zebkit, 1% less, within noise |
| strings, ms | 26.0 (26.0-28.0) | 26.0 (26.0-27.0) | level |
| json, ms | 201 (196-208) | 209 (204-247) | zebkit, 4% less, within noise |
| regex, ms | 234 (227-237) | 221 (214-223) | Electron, 6% less |
| sort, ms | 87.0 (84.0-89.0) | 91.0 (86.0-111) | zebkit, 4% less, within noise |
| parse, ms | 44.0 (42.0-45.0) | 45.0 (43.0-50.0) | zebkit, 2% less, within noise |
| gc, ms | 122 (75.0-138) | 93.0 (79.0-127) | Electron, 24% less, within noise |
| jit, ms | 59.0 (58.0-63.0) | 58.0 (55.0-67.0) | Electron, 2% less, within noise |
| dom, ms | 377 (340-403) | 282 (255-284) | Electron, 25% less |

Load before a run: 3.4 to 5.0. Runs: 5 of each.

### Calls from the page to the host and back

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| 64 bytes, ms | 0.1 (0.1-0.1) | 0.1 (0.1-0.1) | level |
| 64 KiB, ms | 0.4 (0.4-0.4) | 0.4 (0.4-0.4) | level |
| 1 MiB, ms | 3.4 (3.3-3.5) | 4.4 (4.3-4.5) | zebkit, 23% less |
| 16 MiB, ms | 55.6 (54.6-57.8) | 71.3 (70.0-72.1) | zebkit, 22% less |

Load before a run: 3.3 to 3.8. Runs: 5 of each.

### Long document (200,000 words)

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| to its first frame, ms | 196 (190-201) | 159 (157-170) | Electron, 19% less |
| longest task, ms | 209 (208-215) | 176 (174-184) | Electron, 16% less |
| scrolling frame, p50, ms | 16.7 (16.6-16.8) | 16.7 (16.7-16.7) | level |
| scrolling frame, p95, ms | 17.3 (17.2-17.5) | 17.3 (17.2-17.4) | level |
| scrolling frame, max, ms | 17.6 (17.6-33.9) | 17.7 (17.7-21.2) | zebkit, 1% less, within noise |
| CPU, all processes, s | 3.45 (3.4-3.52) | 4.27 (3.96-4.39) | zebkit, 19% less |
| memory, MB | 248 (247-248) | 322 (321-323) | zebkit, 23% less |

Load before a run: 2.0 to 3.3. Runs: 5 of each.

## Local pages

### blank

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 0.25 (0.23-0.26) | 0.5 (0.49-0.51) | zebkit, 50% less |
| CPU at rest (20 s after), % of a core, % | 0 (0-0) | 0 (0-0.1) | level |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0 (0-0) | level |
| memory of all processes, MB | 167 (167-167) | 239 (239-240) | zebkit, 30% less |
| processes | 7 (7-7) | 6 (6-6) | Electron, 14% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 1.8 to 3.1. Runs: 5 of each.

Titles, zebkit: ['Blank (5)']; Electron: ['Blank (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

### static

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 0.27 (0.27-0.28) | 0.51 (0.5-0.53) | zebkit, 47% less |
| CPU at rest (20 s after), % of a core, % | 0 (0-0) | 0.05 (0-0.1) | zebkit, 100% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0 (0-0) | level |
| memory of all processes, MB | 173 (173-174) | 245 (245-247) | zebkit, 29% less |
| processes | 7 (7-7) | 6 (6-6) | Electron, 14% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 1.9 to 2.4. Runs: 5 of each.

Titles, zebkit: ['Static article (5)']; Electron: ['Static article (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

### animation

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 0.75 (0.71-0.87) | 1.01 (0.93-1.03) | zebkit, 26% less |
| CPU at rest (20 s after), % of a core, % | 5.75 (4.91-6.59) | 4.81 (4.21-5.25) | Electron, 16% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0.35 (0.3-0.5) | 0.3 (0.25-0.3) | Electron, 14% less, within noise |
| memory of all processes, MB | 180 (180-180) | 250 (248-251) | zebkit, 28% less |
| processes | 7 (7-7) | 6 (6-6) | Electron, 14% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 2.1 to 2.7. Runs: 5 of each.

Titles, zebkit: ['Animation (5)']; Electron: ['Animation (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

### heavy

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 1.04 (1.02-1.08) | 1.28 (1.25-1.43) | zebkit, 19% less |
| CPU at rest (20 s after), % of a core, % | 0.59 (0.5-0.69) | 0.5 (0.5-0.64) | Electron, 15% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0.05 (0-0.05) | zebkit, 100% less, within noise |
| memory of all processes, MB | 324 (324-327) | 396 (374-398) | zebkit, 18% less |
| processes | 7 (7-7) | 6 (6-6) | Electron, 14% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 2.0 to 2.5. Runs: 5 of each.

Titles, zebkit: ['Heavy page (0) (5)']; Electron: ['Heavy page (0) (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

### long-pdf

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 0.73 (0.7-0.75) | 1.62 (1.59-1.65) | zebkit, 55% less |
| CPU at rest (20 s after), % of a core, % | 0 (0-0) | 0 (0-0.05) | level |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0 (0-0) | level |
| memory of all processes, MB | 248 (248-248) | 390 (389-393) | zebkit, 36% less |
| processes | 7 (7-7) | 8 (8-8) | zebkit, 12% less |
| renderers | 1 (1-1) | 3 (3-3) | zebkit, 67% less |

Load before a run: 2.2 to 3.1. Runs: 5 of each.

Titles, zebkit: ['127.0.0.1:39853/long.pdf (5)']; Electron: ['PDF32000.book (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 3, 'gpu-process': 1}.

Sums of the medians over 5 pages, zebkit / Electron: CPU loading 3.0 / 4.9 s (zebkit less on 5), CPU at rest 6.3 / 5.4 % of a core (zebkit less on 1), memory 1092 / 1520 MB (zebkit less on 5).

## Sites

### en.wikipedia.org/wiki/Charles_Darwin

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 1.08 (1.01-1.13) | 1.28 (1.23-1.36) | zebkit, 16% less |
| CPU at rest (20 s after), % of a core, % | 0.05 (0-0.1) | 0 (0-0.05) | Electron, 100% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0 (0-0) | level |
| memory of all processes, MB | 280 (278-282) | 336 (335-339) | zebkit, 17% less |
| processes | 7 (7-7) | 6 (6-6) | Electron, 14% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 2.0 to 2.8. Runs: 5 of each.

Titles, zebkit: ['Charles Darwin - Wikipedia (5)']; Electron: ['Charles Darwin - Wikipedia (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

### www.google.com/

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 1.26 (1.25-1.31) | 1.51 (1.42-1.58) | zebkit, 17% less |
| CPU at rest (20 s after), % of a core, % | 0.4 (0.35-0.45) | 0.4 (0.35-0.45) | level |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0.05) | 0 (0-0.05) | level |
| memory of all processes, MB | 245 (244-247) | 309 (308-312) | zebkit, 21% less |
| processes | 7 (7-7) | 6 (6-6) | Electron, 14% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 2.0 to 2.6. Runs: 5 of each.

Titles, zebkit: ['Google (5)']; Electron: ['Google (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

### www.youtube.com/

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 3.48 (3.09-3.57) | 5.2 (4.77-5.46) | zebkit, 33% less |
| CPU at rest (20 s after), % of a core, % | 1.54 (0.84-4.71) | 3.27 (1.98-5.35) | zebkit, 53% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0 (0-0) | level |
| memory of all processes, MB | 484 (468-490) | 548 (536-553) | zebkit, 12% less |
| processes | 9 (9-9) | 8 (8-8) | Electron, 11% less |
| renderers | 2 (2-2) | 2 (2-2) | level |

Load before a run: 1.9 to 2.2. Runs: 5 of each.

Titles, zebkit: ['YouTube (5)']; Electron: ['YouTube (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:audio': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 2, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:audio': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 2, 'gpu-process': 1}.

### github.com/torvalds/linux

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 2.73 (2.25-2.96) | 2.96 (2.84-3) | zebkit, 8% less, within noise |
| CPU at rest (20 s after), % of a core, % | 0.05 (0.05-2.18) | 0.15 (0.05-0.2) | zebkit, 67% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0 (0-0) | level |
| memory of all processes, MB | 332 (324-334) | 392 (386-397) | zebkit, 15% less |
| processes | 7 (7-7) | 6 (6-6) | Electron, 14% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 1.8 to 3.1. Runs: 5 of each.

Titles, zebkit: ['GitHub - torvalds/linux: Linux kernel source tree  (5)']; Electron: ['GitHub - torvalds/linux: Linux kernel source tree  (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

### www.amazon.com/

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 0.92 (0.39-1.18) | 1.21 (0.75-1.84) | zebkit, 24% less, within noise |
| CPU at rest (20 s after), % of a core, % | 35.7 (34.7-36.9) | 46.4 (45.4-47.7) | zebkit, 23% less |
| X server's CPU at rest (Linux), % of a core, % | 0.64 (0.59-0.69) | 0.59 (0.59-0.64) | Electron, 8% less, within noise |
| memory of all processes, MB | 472 (456-484) | 528 (516-534) | zebkit, 11% less |
| processes | 9 (9-9) | 8 (8-8) | Electron, 11% less |
| renderers | 2 (2-2) | 2 (2-2) | level |

Load before a run: 1.9 to 2.6. Runs: 5 of each.

Titles, zebkit: ['Amazon.com. Spend less. Smile more. (5)']; Electron: ['Amazon.com. Spend less. Smile more. (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:audio': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 2, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:audio': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 2, 'gpu-process': 1}.

### www.reddit.com/r/programming/

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 1.49 (1.28-1.51) | 2.72 (1.11-3.23) | zebkit, 45% less, within noise |
| CPU at rest (20 s after), % of a core, % | 0.74 (0.69-1.83) | 27.2 (10.6-34.5) | zebkit, 97% less |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0.3 (0-0.35) | zebkit, 100% less, within noise |
| memory of all processes, MB | 334 (332-346) | 603 (528-618) | zebkit, 45% less |
| processes | 8 (8-8) | 8 (8-8) | level |
| renderers | 2 (2-2) | 2 (2-2) | level |

Load before a run: 1.9 to 3.2. Runs: 5 of each.

Titles, zebkit: ['Reddit - Prove your humanity (5)']; Electron: ['programming (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 2, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:audio': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 2, 'gpu-process': 1}.

### www.nytimes.com/

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 4.26 (3.87-5.75) | 5.42 (0.6-7.07) | zebkit, 21% less, within noise |
| CPU at rest (20 s after), % of a core, % | 47.8 (40.0-48.6) | 39.2 (36.9-62.7) | Electron, 18% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0.25 (0.2-0.25) | 0.1 (0.05-0.2) | Electron, 60% less, within noise |
| memory of all processes, MB | 809 (806-828) | 869 (855-933) | zebkit, 7% less |
| processes | 21.0 (21.0-21.0) | 21.0 (21.0-26.0) | level |
| renderers | 14.0 (14.0-14.0) | 15.0 (15.0-20.0) | zebkit, 7% less |

Load before a run: 2.1 to 2.7. Runs: 5 of each.

Titles, zebkit: ['The New York Times - Breaking News, US News, World (5)']; Electron: ['The New York Times - Breaking News, US News, World (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:audio': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 14, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:audio': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 15, 'gpu-process': 1}.

### www.bbc.com/news

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 1.95 (1.91-2.11) | 2 (1.96-2.21) | zebkit, 3% less, within noise |
| CPU at rest (20 s after), % of a core, % | 21.5 (19.8-22.5) | 20.1 (15.7-23.9) | Electron, 7% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0.05 (0-0.05) | 0.05 (0-0.05) | level |
| memory of all processes, MB | 515 (487-535) | 552 (492-604) | zebkit, 7% less, within noise |
| processes | 15.0 (14.0-16.0) | 13.0 (9-15.0) | Electron, 13% less, within noise |
| renderers | 9 (8-10.0) | 8 (4-10.0) | Electron, 11% less, within noise |

Load before a run: 2.0 to 2.6. Runs: 5 of each.

Titles, zebkit: ['BBC News - Breaking news, video and the latest top (5)']; Electron: ['BBC News - Breaking news, video and the latest top (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 9, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 6, 'gpu-process': 1}.

### www.theverge.com/

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 3.44 (2.64-3.84) | 3.73 (0.44-4.05) | zebkit, 8% less, within noise |
| CPU at rest (20 s after), % of a core, % | 36.5 (32.2-45.5) | 34.5 (24.1-50.3) | Electron, 6% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0.05 (0-0.05) | 0 (0-0.1) | Electron, 100% less, within noise |
| memory of all processes, MB | 762 (752-848) | 836 (762-860) | zebkit, 9% less, within noise |
| processes | 16.0 (15.0-18.0) | 25.0 (23.0-27.0) | zebkit, 36% less |
| renderers | 9 (8-11.0) | 20.0 (18.0-20.0) | zebkit, 55% less |

Load before a run: 1.7 to 3.5. Runs: 5 of each.

Titles, zebkit: ['The Verge (5)']; Electron: ['The Verge (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:audio': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 9, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 18, 'gpu-process': 1}.

### stackoverflow.com/questions/11227809

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 1.27 (0.46-1.54) | 1.83 (1.17-2) | zebkit, 31% less, within noise |
| CPU at rest (20 s after), % of a core, % | 39.7 (36.0-45.0) | 25.2 (24.6-44.9) | Electron, 36% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0.1 (0.1-0.15) | 0.05 (0.05-0.1) | Electron, 50% less, within noise |
| memory of all processes, MB | 689 (392-718) | 606 (558-804) | Electron, 12% less, within noise |
| processes | 20.0 (8-20.0) | 7 (7-19.0) | Electron, 65% less, within noise |
| renderers | 14.0 (2-14.0) | 2 (2-14.0) | Electron, 86% less, within noise |

Load before a run: 1.9 to 2.9. Runs: 5 of each.

Titles, zebkit: ['java - Why is conditional processing of a sorted a (5)']; Electron: ['Just a moment... (3)', 'java - Why is conditional processing of a sorted a (2)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 14, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 2, 'gpu-process': 1}.

### www.apple.com/

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 1.3 (1.24-1.32) | 1.49 (1.45-1.53) | zebkit, 13% less |
| CPU at rest (20 s after), % of a core, % | 0.05 (0-0.1) | 0.05 (0.05-0.1) | level |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0 (0-0) | level |
| memory of all processes, MB | 304 (295-312) | 347 (345-358) | zebkit, 12% less |
| processes | 7 (7-7) | 6 (6-6) | Electron, 14% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 1.6 to 2.1. Runs: 5 of each.

Titles, zebkit: ['Apple (5)']; Electron: ['Apple (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

### developer.mozilla.org/en-US/docs/Web/JavaScript

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 0.76 (0.74-0.78) | 1 (0.92-1.05) | zebkit, 24% less |
| CPU at rest (20 s after), % of a core, % | 0 (0-0) | 0.05 (0-0.2) | zebkit, 100% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0 (0-0) | level |
| memory of all processes, MB | 226 (226-226) | 301 (299-303) | zebkit, 25% less |
| processes | 7 (7-7) | 6 (6-6) | Electron, 14% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 1.8 to 3.0. Runs: 5 of each.

Titles, zebkit: ['JavaScript | MDN (5)']; Electron: ['JavaScript | MDN (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

Left out of the sums, for showing a different page in some run: www.reddit.com/r/programming/, stackoverflow.com/questions/11227809.

Sums of the medians over 10 pages, zebkit / Electron: CPU loading 21.2 / 25.8 s (zebkit less on 10), CPU at rest 143.6 / 144.0 % of a core (zebkit less on 4), memory 4430 / 5021 MB (zebkit less on 10).

Runs discarded for a machine that was not quiet: 0. Runs that failed: 0.
