# zebkit and Electron 45.2.1, measured the same way

`scripts/electron-compare.sh`, 2026-11-06 (startup and size again on 2026-11-07, with the cold-start read-ahead), Linux x86-64: zebkit `slim` (Chromium 155.0.8059.84) and Electron 45.2.1, the latest stable release on npm, on one machine. Each cell is the median and, in parentheses, the lowest and highest run. A result is called within noise when those ranges overlap. `docs/SIZE_AND_SPEED.md` has what was held the same and what could not be.

## Size

MiB. As published is what each project hands out; as an application ships it is the runtime trimmed as far as it goes without building it, with the same thin host: Electron with one locale and no licence file, zebkit without the headers an application is built against.

| | zebkit | Electron | Smaller |
|---|---:|---:|---|
| As published, uncompressed | 179.7 | 273.4 | zebkit, 34% less |
| As published, `.tar.xz` | 60.9 | 88.7 | zebkit, 31% less |
| As published, `.zip` | 81.5 | 117.6 | zebkit, 31% less |
| As an application ships it, uncompressed | 171.2 | 260.5 | zebkit, 34% less |
| As an application ships it, `.tar.xz` | 59.4 | 87.1 | zebkit, 32% less |
| As an application ships it, `.zip` | 79.5 | 114.8 | zebkit, 31% less |

## Engines


### Startup, files not in cache, new profile

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| to the page's request, ms | 2164 (1990-2248) | 5896 (5696-15570) | zebkit, 63% less |
| to first paint (FCP), ms | 2279 (2090-2354) | 6613 (6304-16896) | zebkit, 66% less |
| to a frame after load, ms | 2284 (2097-2358) | 6617 (6323-17267) | zebkit, 65% less |
| CPU until then, all processes, s | 0.31 (0.28-0.33) | 0.6 (0.57-0.69) | zebkit, 48% less |

Load before a run: 4.0 to 4.9. Runs: 9 of each. The disk was quiet (under 2 MB/s) before every run; Electron's slowest (16.9 s, the others 6.3 to 6.7) is most likely other work on the shared disk during it.

### Startup, files in cache, profile kept

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| to the page's request, ms | 52.9 (51.7-55.4) | 180 (170-188) | zebkit, 71% less |
| to first paint (FCP), ms | 116 (114-124) | 236 (224-243) | zebkit, 51% less |
| to a frame after load, ms | 116 (108-126) | 230 (223-242) | zebkit, 50% less |
| CPU until then, all processes, s | 0.21 (0.2-0.23) | 0.46 (0.45-0.48) | zebkit, 54% less |
| memory at that frame, MB | 180 (180-181) | 249 (247-251) | zebkit, 28% less |

Load before a run: 4.1 to 4.5. Runs: 9 of each.

### Text layout, a chapter

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| time per layout, the median of the last 50, ms | 8.1 (7.5-9) | 7.6 (7.1-8.4) | Electron, 6% less, within noise |
| renderer main thread's CPU, the mean of all 60 with the first 10, ms | 10.9 (10.4-12.0) | 10.3 (9.88-11.6) | Electron, 6% less, within noise |

Load before a run: 3.2 to 3.9. Runs: 7 of each.

### JavaScript and DOM, tests/bench/index.html

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| all, ms | 1139 (1105-1422) | 1193 (1122-1393) | zebkit, 5% less, within noise |
| loop, ms | 41.0 (37.0-199) | 38.0 (37.0-183) | Electron, 7% less, within noise |
| objects, ms | 92.0 (85.0-125) | 88.0 (86.0-132) | Electron, 4% less, within noise |
| strings, ms | 25.0 (24.0-28.0) | 26.0 (25.0-30.0) | zebkit, 4% less, within noise |
| json, ms | 178 (174-217) | 210 (202-233) | zebkit, 15% less, within noise |
| regex, ms | 230 (220-246) | 224 (212-243) | Electron, 3% less, within noise |
| sort, ms | 91.0 (82.0-106) | 99.0 (91.0-116) | zebkit, 8% less, within noise |
| parse, ms | 39.0 (37.0-51.0) | 54.0 (41.0-62.0) | zebkit, 28% less, within noise |
| gc, ms | 89.0 (75.0-149) | 87.0 (80.0-90.0) | Electron, 2% less, within noise |
| jit, ms | 56.0 (52.0-61.0) | 59.0 (55.0-72.0) | zebkit, 5% less, within noise |
| dom, ms | 280 (270-322) | 278 (261-296) | Electron, 1% less, within noise |

Load before a run: 3.1 to 3.5. Runs: 5 of each.

### Calls from the page to the host and back

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| 64 bytes, ms | 0.1 (0.1-0.1) | 0.1 (0.1-0.1) | level |
| 64 KiB, ms | 0.4 (0.4-0.4) | 0.4 (0.4-0.4) | level |
| 1 MiB, ms | 3.3 (3.2-3.6) | 4.3 (4.2-4.4) | zebkit, 23% less |
| 16 MiB, ms | 58.1 (52.2-60.9) | 66.3 (65.9-75.1) | zebkit, 12% less |

Load before a run: 2.9 to 4.1. Runs: 5 of each.

### Long document (200,000 words)

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| to its first frame, ms | 171 (166-195) | 170 (168-184) | Electron, 1% less, within noise |
| longest task, ms | 189 (178-210) | 184 (181-197) | Electron, 3% less, within noise |
| scrolling frame, p50, ms | 16.7 (16.7-16.7) | 16.7 (16.6-16.7) | level |
| scrolling frame, p95, ms | 17.3 (17.3-17.3) | 17.4 (17.3-17.4) | zebkit, 1% less, within noise |
| scrolling frame, max, ms | 17.7 (17.6-38.0) | 18.1 (17.8-29.2) | zebkit, 2% less, within noise |
| CPU, all processes, s | 3.46 (3.42-3.59) | 4.33 (4.06-4.51) | zebkit, 20% less |
| memory, MB | 260 (260-260) | 333 (330-334) | zebkit, 22% less |

Load before a run: 2.4 to 3.8. Runs: 5 of each.

## Local pages

### blank

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 0.23 (0.22-0.26) | 0.49 (0.48-0.51) | zebkit, 53% less |
| CPU at rest (20 s after), % of a core, % | 0 (0-0) | 0 (0-0.05) | level |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0 (0-0) | level |
| memory of all processes, MB | 179 (179-179) | 250 (250-251) | zebkit, 29% less |
| processes | 7 (7-7) | 6 (6-6) | Electron, 14% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 2.0 to 3.0. Runs: 5 of each.

Titles, zebkit: ['Blank (5)']; Electron: ['Blank (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

### static

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 0.26 (0.25-0.28) | 0.54 (0.5-0.54) | zebkit, 52% less |
| CPU at rest (20 s after), % of a core, % | 0 (0-0) | 0 (0-0) | level |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0 (0-0) | level |
| memory of all processes, MB | 185 (185-185) | 257 (254-257) | zebkit, 28% less |
| processes | 7 (7-7) | 6 (6-6) | Electron, 14% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 1.8 to 2.6. Runs: 5 of each.

Titles, zebkit: ['Static article (5)']; Electron: ['Static article (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

### animation

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 0.74 (0.71-0.84) | 0.97 (0.96-1.02) | zebkit, 24% less |
| CPU at rest (20 s after), % of a core, % | 5.06 (4.91-6.1) | 5 (4.76-6.15) | Electron, 1% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0.3 (0.2-0.45) | 0.3 (0.3-0.45) | level |
| memory of all processes, MB | 192 (192-192) | 260 (258-261) | zebkit, 26% less |
| processes | 7 (7-7) | 6 (6-6) | Electron, 14% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 2.0 to 3.0. Runs: 5 of each.

Titles, zebkit: ['Animation (5)']; Electron: ['Animation (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

### heavy

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 1 (0.96-1.03) | 1.25 (1.22-1.35) | zebkit, 20% less |
| CPU at rest (20 s after), % of a core, % | 0.59 (0.5-0.6) | 0.64 (0.55-0.74) | zebkit, 8% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0 (0-0) | level |
| memory of all processes, MB | 339 (338-339) | 406 (405-407) | zebkit, 17% less |
| processes | 7 (7-7) | 6 (6-6) | Electron, 14% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 2.1 to 3.5. Runs: 5 of each.

Titles, zebkit: ['Heavy page (0) (5)']; Electron: ['Heavy page (0) (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

### long-pdf

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 0.75 (0.71-0.76) | 1.6 (1.6-1.66) | zebkit, 53% less |
| CPU at rest (20 s after), % of a core, % | 0 (0-0) | 0 (0-0.1) | level |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0 (0-0) | level |
| memory of all processes, MB | 261 (261-261) | 399 (398-402) | zebkit, 35% less |
| processes | 7 (7-7) | 8 (8-8) | zebkit, 12% less |
| renderers | 1 (1-1) | 3 (3-3) | zebkit, 67% less |

Load before a run: 1.9 to 2.6. Runs: 5 of each.

Titles, zebkit: ['127.0.0.1:46495/long.pdf (5)']; Electron: ['PDF32000.book (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 3, 'gpu-process': 1}.

Sums of the medians over 5 pages, zebkit / Electron: CPU loading 3.0 / 4.8 s (zebkit less on 5), CPU at rest 5.6 / 5.6 % of a core (zebkit less on 1), memory 1156 / 1572 MB (zebkit less on 5).

## Sites

### en.wikipedia.org/wiki/Charles_Darwin

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 1.18 (1.09-1.25) | 1.47 (1.42-1.67) | zebkit, 20% less |
| CPU at rest (20 s after), % of a core, % | 2.63 (2.53-2.73) | 2.58 (2.43-3.17) | Electron, 2% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0.15 (0.1-0.2) | 0.15 (0.1-0.2) | level |
| memory of all processes, MB | 295 (295-296) | 350 (347-378) | zebkit, 15% less |
| processes | 7 (7-7) | 6 (6-6) | Electron, 14% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 1.6 to 2.6. Runs: 5 of each.

Titles, zebkit: ['Charles Darwin - Wikipedia (5)']; Electron: ['Charles Darwin - Wikipedia (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

### www.google.com/

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 1.22 (1.13-1.33) | 1.48 (1.45-1.5) | zebkit, 18% less |
| CPU at rest (20 s after), % of a core, % | 0.3 (0.2-0.4) | 0.4 (0.3-0.45) | zebkit, 25% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0 (0-0.05) | level |
| memory of all processes, MB | 264 (261-265) | 319 (317-323) | zebkit, 17% less |
| processes | 7 (7-7) | 6 (6-6) | Electron, 14% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 2.1 to 2.8. Runs: 5 of each.

Titles, zebkit: ['Google (5)']; Electron: ['Google (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

### www.youtube.com/

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 4.75 (1.39-4.82) | 4.58 (2.21-6.98) | Electron, 4% less, within noise |
| CPU at rest (20 s after), % of a core, % | 1.98 (1.49-17.8) | 13.9 (2.53-25.6) | zebkit, 86% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0 (0-0.05) | level |
| memory of all processes, MB | 684 (678-690) | 583 (560-591) | Electron, 15% less |
| processes | 9 (9-9) | 7 (7-7) | Electron, 22% less |
| renderers | 2 (2-2) | 1 (1-1) | Electron, 50% less |

Load before a run: 2.1 to 2.8. Runs: 5 of each.

Titles, zebkit: ['YouTube (5)']; Electron: ['YouTube (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:audio': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 2, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:audio': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

### github.com/torvalds/linux

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 2.56 (0.55-2.58) | 2.83 (2.7-3.51) | zebkit, 10% less |
| CPU at rest (20 s after), % of a core, % | 0.15 (0-9.96) | 0.25 (0.1-0.3) | zebkit, 40% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0.15) | 0 (0-0) | level |
| memory of all processes, MB | 337 (328-342) | 402 (398-419) | zebkit, 16% less |
| processes | 7 (7-7) | 6 (6-6) | Electron, 14% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 2.2 to 3.2. Runs: 5 of each.

Titles, zebkit: ['GitHub - torvalds/linux: Linux kernel source tree  (5)']; Electron: ['GitHub - torvalds/linux: Linux kernel source tree  (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

### www.amazon.com/

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 0.83 (0.56-2.66) | 0.78 (0.74-1.56) | Electron, 6% less, within noise |
| CPU at rest (20 s after), % of a core, % | 40.8 (27.2-41.7) | 49.5 (39.7-52.5) | zebkit, 17% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0.54 (0.45-0.59) | 0.55 (0.5-0.59) | zebkit, 2% less, within noise |
| memory of all processes, MB | 525 (468-539) | 578 (548-584) | zebkit, 9% less |
| processes | 8 (8-8) | 7 (7-7) | Electron, 12% less |
| renderers | 2 (2-2) | 2 (2-2) | level |

Load before a run: 2.5 to 3.5. Runs: 5 of each.

Titles, zebkit: ['Amazon.com. Spend less. Smile more. (5)']; Electron: ['Amazon.com. Spend less. Smile more. (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 2, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 2, 'gpu-process': 1}.

### www.reddit.com/r/programming/

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 1.4 (0.2-1.59) | 2.8 (0.57-3.02) | zebkit, 50% less, within noise |
| CPU at rest (20 s after), % of a core, % | 1.04 (0.84-6.4) | 12.6 (8.38-29.6) | zebkit, 92% less |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0.05 (0-0.35) | zebkit, 100% less, within noise |
| memory of all processes, MB | 352 (349-353) | 530 (494-611) | zebkit, 34% less |
| processes | 8 (8-8) | 7 (7-8) | Electron, 12% less, within noise |
| renderers | 2 (2-2) | 2 (1-2) | level |

Load before a run: 2.0 to 2.9. Runs: 5 of each.

Titles, zebkit: ['Reddit - Prove your humanity (5)']; Electron: ['programming (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 2, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 2, 'gpu-process': 1}.

### www.nytimes.com/

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 3.51 (0.31-3.77) | 5.36 (0.61-6.23) | zebkit, 35% less, within noise |
| CPU at rest (20 s after), % of a core, % | 47.9 (46.7-58.2) | 46.0 (36.4-62.9) | Electron, 4% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0.2 (0.15-0.25) | 0.1 (0.05-0.15) | Electron, 50% less, within noise |
| memory of all processes, MB | 812 (779-826) | 931 (859-1007) | zebkit, 13% less |
| processes | 21.0 (21.0-21.0) | 24.0 (22.0-30.0) | zebkit, 12% less |
| renderers | 14.0 (14.0-14.0) | 18.0 (16.0-24.0) | zebkit, 22% less |

Load before a run: 2.3 to 3.0. Runs: 5 of each.

Titles, zebkit: ['The New York Times - Breaking News, US News, World (5)']; Electron: ['The New York Times - Breaking News, US News, World (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:audio': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 14, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:audio': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 18, 'gpu-process': 1}.

### www.bbc.com/news

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 1.91 (0.85-2.09) | 2.18 (1.97-2.41) | zebkit, 12% less, within noise |
| CPU at rest (20 s after), % of a core, % | 19.5 (14.8-20.7) | 19.0 (15.3-20.4) | Electron, 2% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0.05 (0-0.1) | 0 (0-0.05) | Electron, 100% less, within noise |
| memory of all processes, MB | 474 (417-501) | 536 (504-577) | zebkit, 12% less |
| processes | 12.0 (10.0-15.0) | 11.0 (10.0-13.0) | Electron, 8% less, within noise |
| renderers | 6 (4-9) | 6 (5-8) | level |

Load before a run: 2.1 to 2.8. Runs: 5 of each.

Titles, zebkit: ['BBC News - Breaking news, video and the latest top (5)']; Electron: ['BBC News - Breaking news, video and the latest top (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 8, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 5, 'gpu-process': 1}.

### www.theverge.com/

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 3.42 (2.96-3.86) | 3.67 (0.45-3.8) | zebkit, 7% less, within noise |
| CPU at rest (20 s after), % of a core, % | 37.0 (32.6-40.9) | 28.9 (17.7-37.1) | Electron, 22% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0.1 (0.05-0.15) | 0 (0-0.05) | Electron, 100% less, within noise |
| memory of all processes, MB | 801 (755-833) | 686 (611-816) | Electron, 14% less, within noise |
| processes | 16.0 (14.0-17.0) | 23.0 (22.0-24.0) | zebkit, 30% less |
| renderers | 10.0 (8-11.0) | 18.0 (16.0-19.0) | zebkit, 44% less |

Load before a run: 2.3 to 3.3. Runs: 5 of each.

Titles, zebkit: ['The Verge (5)']; Electron: ['The Verge (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 8, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 18, 'gpu-process': 1}.

### stackoverflow.com/questions/11227809

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 1.29 (0.23-1.62) | 1.69 (1.24-2.11) | zebkit, 24% less, within noise |
| CPU at rest (20 s after), % of a core, % | 34.8 (33.6-41.2) | 23.3 (20.9-25.8) | Electron, 33% less |
| X server's CPU at rest (Linux), % of a core, % | 0.1 (0.1-0.15) | 0.05 (0.05-0.1) | Electron, 50% less, within noise |
| memory of all processes, MB | 401 (347-714) | 566 (545-590) | zebkit, 29% less, within noise |
| processes | 7 (7-18.0) | 7 (7-16.0) | level |
| renderers | 1 (1-12.0) | 2 (2-11.0) | zebkit, 50% less, within noise |

Load before a run: 2.3 to 2.6. Runs: 5 of each.

Titles, zebkit: ['java - Why is conditional processing of a sorted a (5)']; Electron: ['Just a moment... (4)', 'java - Why is conditional processing of a sorted a (1)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 2, 'gpu-process': 1}.

### www.apple.com/

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 1.62 (1.56-1.65) | 1.8 (1.8-1.82) | zebkit, 10% less |
| CPU at rest (20 s after), % of a core, % | 0 (0-0.1) | 0.1 (0-0.15) | zebkit, 100% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0 (0-0) | level |
| memory of all processes, MB | 323 (321-330) | 363 (359-366) | zebkit, 11% less |
| processes | 7 (7-7) | 6 (6-6) | Electron, 14% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 2.1 to 2.5. Runs: 5 of each.

Titles, zebkit: ['Apple (5)']; Electron: ['Apple (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

### developer.mozilla.org/en-US/docs/Web/JavaScript

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 0.72 (0.7-0.82) | 1.01 (0.97-1.02) | zebkit, 29% less |
| CPU at rest (20 s after), % of a core, % | 0 (0-0.1) | 0.05 (0-0.05) | zebkit, 100% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0 (0-0) | level |
| memory of all processes, MB | 242 (239-244) | 313 (310-314) | zebkit, 23% less |
| processes | 7 (7-7) | 6 (6-6) | Electron, 14% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 1.8 to 3.0. Runs: 5 of each.

Titles, zebkit: ['JavaScript | MDN (5)']; Electron: ['JavaScript | MDN (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

Left out of the sums, for showing a different page in some run: www.reddit.com/r/programming/, stackoverflow.com/questions/11227809.

Sums of the medians over 10 pages, zebkit / Electron: CPU loading 21.7 / 25.2 s (zebkit less on 8), CPU at rest 150.3 / 160.7 % of a core (zebkit less on 6), memory 4756 / 5060 MB (zebkit less on 8).

Runs discarded for a machine that was not quiet: 0. Runs that failed: 0.
