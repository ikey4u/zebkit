# zebkit and Electron 45.2.1, measured the same way

`scripts/electron-compare.sh`, 2026-11-08, Linux x86-64: zebkit `slim` (Chromium 157.0.8103.38) and Electron 45.2.1, the latest stable release on npm, on one machine. Each cell is the median and, in parentheses, the lowest and highest run. A result is called within noise when those ranges overlap. `docs/SIZE_AND_SPEED.md` has what was held the same and what could not be.

## Size

MiB. As published is what each project hands out; as an application ships it is the runtime trimmed as far as it goes without building it, with the same thin host: Electron with one locale and no licence file, zebkit without the headers an application is built against.

| | zebkit | Electron | Smaller |
|---|---:|---:|---|
| As published, uncompressed | 182.6 | 273.4 | zebkit, 33% less |
| As published, `.tar.xz` | 61.9 | 88.7 | zebkit, 30% less |
| As published, `.zip` | 83.0 | 117.6 | zebkit, 29% less |
| As an application ships it, uncompressed | 174.1 | 260.5 | zebkit, 33% less |
| As an application ships it, `.tar.xz` | 60.5 | 87.1 | zebkit, 30% less |
| As an application ships it, `.zip` | 81.0 | 114.8 | zebkit, 29% less |

## Engines


### Startup, files not in cache, new profile

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| to the page's request, ms | 2282 (2243-2353) | 5367 (5068-5413) | zebkit, 57% less |
| to first paint (FCP), ms | 2329 (2287-2410) | 5769 (5453-5822) | zebkit, 60% less |
| to a frame after load, ms | 2327 (2289-2396) | 5785 (5468-5884) | zebkit, 60% less |
| CPU until then, all processes, s | 0.34 (0.3-0.44) | 0.58 (0.55-0.59) | zebkit, 41% less |

Load before a run: 4.0 to 4.8. Runs: 7 of each.

### Startup, files in cache, profile kept

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| to the page's request, ms | 56.8 (52.9-61.1) | 175 (172-182) | zebkit, 67% less |
| to first paint (FCP), ms | 124 (120-135) | 236 (230-249) | zebkit, 47% less |
| to a frame after load, ms | 124 (120-135) | 236 (222-251) | zebkit, 47% less |
| CPU until then, all processes, s | 0.23 (0.21-0.26) | 0.47 (0.46-0.48) | zebkit, 51% less |
| memory at that frame, MB | 183 (183-183) | 248 (247-250) | zebkit, 26% less |

Load before a run: 4.0 to 4.4. Runs: 7 of each.

### Text layout, a chapter

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| time per layout, the median of the last 50, ms | 7.5 (7.4-8.5) | 7.4 (7.1-8.1) | Electron, 1% less, within noise |
| renderer main thread's CPU, the mean of all 60 with the first 10, ms | 10.8 (10.3-12.0) | 10.2 (9.96-10.6) | Electron, 6% less, within noise |

Load before a run: 3.9 to 4.3. Runs: 7 of each.

### JavaScript and DOM, tests/bench/index.html

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| all, ms | 1086 (1064-1102) | 1161 (1114-1378) | zebkit, 6% less |
| loop, ms | 37.0 (36.0-39.0) | 38.0 (37.0-125) | zebkit, 3% less, within noise |
| objects, ms | 85.0 (81.0-90.0) | 91.0 (85.0-108) | zebkit, 7% less, within noise |
| strings, ms | 25.0 (25.0-26.0) | 26.0 (25.0-27.0) | zebkit, 4% less, within noise |
| json, ms | 188 (184-205) | 212 (195-246) | zebkit, 11% less, within noise |
| regex, ms | 222 (216-234) | 221 (209-245) | Electron, 0% less, within noise |
| sort, ms | 86.0 (82.0-91.0) | 90.0 (84.0-91.0) | zebkit, 4% less, within noise |
| parse, ms | 41.0 (40.0-46.0) | 46.0 (43.0-47.0) | zebkit, 11% less, within noise |
| gc, ms | 75.0 (72.0-82.0) | 118 (90.0-136) | zebkit, 36% less |
| jit, ms | 56.0 (54.0-63.0) | 57.0 (54.0-74.0) | zebkit, 2% less, within noise |
| dom, ms | 253 (247-280) | 276 (274-295) | zebkit, 8% less, within noise |

Load before a run: 3.7 to 4.0. Runs: 5 of each.

### Calls from the page to the host and back

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| 64 bytes, ms | 0.1 (0.1-0.1) | 0.1 (0.1-0.1) | level |
| 64 KiB, ms | 0.4 (0.4-0.4) | 0.4 (0.4-0.4) | level |
| 1 MiB, ms | 3.3 (3.3-3.4) | 4.2 (4.2-4.3) | zebkit, 21% less |
| 16 MiB, ms | 56.5 (55.8-57.8) | 71.6 (70.8-75.1) | zebkit, 21% less |

Load before a run: 3.7 to 4.3. Runs: 5 of each.

### Long document (200,000 words)

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| to its first frame, ms | 165 (158-171) | 155 (151-173) | Electron, 6% less, within noise |
| longest task, ms | 179 (169-187) | 170 (168-189) | Electron, 5% less, within noise |
| scrolling frame, p50, ms | 16.7 (16.7-16.7) | 16.6 (16.6-16.7) | Electron, 1% less, within noise |
| scrolling frame, p95, ms | 17.3 (17.2-17.3) | 17.5 (17.3-17.5) | zebkit, 1% less, within noise |
| scrolling frame, max, ms | 17.7 (17.6-22.1) | 18.1 (17.7-20.8) | zebkit, 2% less, within noise |
| CPU, all processes, s | 3.47 (3.37-3.72) | 4.13 (4.05-4.28) | zebkit, 16% less |
| memory, MB | 264 (264-264) | 333 (331-334) | zebkit, 21% less |

Load before a run: 3.5 to 4.4. Runs: 5 of each.

## Local pages

### blank

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 0.25 (0.24-0.27) | 0.49 (0.48-0.52) | zebkit, 49% less |
| CPU at rest (20 s after), % of a core, % | 0 (0-0) | 0 (0-0.05) | level |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0 (0-0) | level |
| memory of all processes, MB | 181 (181-182) | 250 (249-251) | zebkit, 27% less |
| processes | 7 (7-7) | 6 (6-6) | Electron, 14% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 3.0 to 4.2. Runs: 5 of each.

Titles, zebkit: ['Blank (5)']; Electron: ['Blank (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

### static

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 0.28 (0.26-0.31) | 0.52 (0.5-0.54) | zebkit, 46% less |
| CPU at rest (20 s after), % of a core, % | 0 (0-0) | 0 (0-0.05) | level |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0 (0-0) | level |
| memory of all processes, MB | 187 (187-187) | 256 (254-258) | zebkit, 27% less |
| processes | 7 (7-7) | 6 (6-6) | Electron, 14% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 2.9 to 3.8. Runs: 5 of each.

Titles, zebkit: ['Static article (5)']; Electron: ['Static article (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

### animation

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 0.76 (0.74-0.81) | 1.02 (0.91-1.06) | zebkit, 25% less |
| CPU at rest (20 s after), % of a core, % | 5.71 (4.91-5.95) | 4.96 (4.51-5.35) | Electron, 13% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0.35 (0.35-0.4) | 0.35 (0.25-0.35) | level |
| memory of all processes, MB | 195 (195-196) | 259 (259-261) | zebkit, 25% less |
| processes | 7 (7-7) | 6 (6-6) | Electron, 14% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 2.9 to 3.8. Runs: 5 of each.

Titles, zebkit: ['Animation (5)']; Electron: ['Animation (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

### heavy

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 1.02 (0.97-1.04) | 1.27 (1.25-1.29) | zebkit, 20% less |
| CPU at rest (20 s after), % of a core, % | 0.65 (0.5-0.74) | 0.6 (0.5-0.65) | Electron, 8% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0.05) | 0 (0-0.05) | level |
| memory of all processes, MB | 343 (343-344) | 405 (404-407) | zebkit, 15% less |
| processes | 7 (7-7) | 6 (6-6) | Electron, 14% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 2.9 to 3.7. Runs: 5 of each.

Titles, zebkit: ['Heavy page (0) (5)']; Electron: ['Heavy page (0) (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

### long-pdf

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 0.73 (0.72-0.73) | 1.59 (1.57-1.66) | zebkit, 54% less |
| CPU at rest (20 s after), % of a core, % | 0 (0-0.05) | 0 (0-0) | level |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0 (0-0) | level |
| memory of all processes, MB | 264 (264-264) | 400 (398-402) | zebkit, 34% less |
| processes | 7 (7-7) | 8 (8-8) | zebkit, 12% less |
| renderers | 1 (1-1) | 3 (3-3) | zebkit, 67% less |

Load before a run: 2.7 to 3.2. Runs: 5 of each.

Titles, zebkit: ['127.0.0.1:42307/long.pdf (5)']; Electron: ['PDF32000.book (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 3, 'gpu-process': 1}.

Sums of the medians over 5 pages, zebkit / Electron: CPU loading 3.0 / 4.9 s (zebkit less on 5), CPU at rest 6.4 / 5.6 % of a core (zebkit less on 0), memory 1171 / 1571 MB (zebkit less on 5).

## Sites

### en.wikipedia.org/wiki/Charles_Darwin

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 1.15 (1.07-1.18) | 1.36 (1.33-1.47) | zebkit, 15% less |
| CPU at rest (20 s after), % of a core, % | 2.53 (2.38-2.73) | 2.63 (2.58-2.68) | zebkit, 4% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0.1 (0.1-0.15) | 0.1 (0.1-0.15) | level |
| memory of all processes, MB | 302 (300-313) | 348 (348-350) | zebkit, 13% less |
| processes | 7 (7-7) | 6 (6-6) | Electron, 14% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 3.1 to 3.4. Runs: 5 of each.

Titles, zebkit: ['Charles Darwin - Wikipedia (5)']; Electron: ['Charles Darwin - Wikipedia (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

### www.google.com/

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 1.17 (1.13-1.21) | 1.5 (1.26-1.57) | zebkit, 22% less |
| CPU at rest (20 s after), % of a core, % | 0.4 (0.3-0.4) | 0.45 (0.3-1.34) | zebkit, 11% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0 (0-0.05) | level |
| memory of all processes, MB | 266 (264-270) | 318 (316-319) | zebkit, 16% less |
| processes | 7 (7-7) | 6 (6-6) | Electron, 14% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 3.0 to 3.5. Runs: 5 of each.

Titles, zebkit: ['Google (5)']; Electron: ['Google (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

### www.youtube.com/

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 4.37 (4.2-4.55) | 6.27 (1.13-6.65) | zebkit, 30% less, within noise |
| CPU at rest (20 s after), % of a core, % | 3.37 (2.88-4.07) | 5.06 (4.32-32.0) | zebkit, 33% less |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0 (0-0.05) | level |
| memory of all processes, MB | 678 (674-685) | 574 (550-587) | Electron, 15% less |
| processes | 9 (9-9) | 7 (7-7) | Electron, 22% less |
| renderers | 2 (2-2) | 1 (1-1) | Electron, 50% less |

Load before a run: 3.2 to 4.3. Runs: 5 of each.

Titles, zebkit: ['YouTube (5)']; Electron: ['YouTube (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:audio': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 2, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:audio': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

### github.com/torvalds/linux

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 2.21 (0.55-2.48) | 2.93 (2.7-2.99) | zebkit, 25% less |
| CPU at rest (20 s after), % of a core, % | 1.24 (0.15-10.3) | 0.1 (0.05-0.2) | Electron, 92% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0.1) | 0 (0-0) | level |
| memory of all processes, MB | 345 (342-352) | 401 (395-410) | zebkit, 14% less |
| processes | 7 (7-7) | 6 (6-6) | Electron, 14% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 3.2 to 5.0. Runs: 5 of each.

Titles, zebkit: ['GitHub - torvalds/linux: Linux kernel source tree  (5)']; Electron: ['GitHub - torvalds/linux: Linux kernel source tree  (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

### www.amazon.com/

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 0.46 (0.44-2.38) | 0.73 (0.72-0.98) | zebkit, 37% less, within noise |
| CPU at rest (20 s after), % of a core, % | 31.5 (28.6-34.1) | 41.4 (40.3-42.3) | zebkit, 24% less |
| X server's CPU at rest (Linux), % of a core, % | 0.5 (0.45-0.5) | 0.55 (0.5-0.55) | zebkit, 9% less, within noise |
| memory of all processes, MB | 481 (459-510) | 526 (514-544) | zebkit, 8% less |
| processes | 8 (8-8) | 7 (7-7) | Electron, 12% less |
| renderers | 2 (2-2) | 2 (2-2) | level |

Load before a run: 3.0 to 3.7. Runs: 5 of each.

Titles, zebkit: ['Amazon.com. Spend less. Smile more. (5)']; Electron: ['Amazon.com. Spend less. Smile more. (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 2, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 2, 'gpu-process': 1}.

### www.reddit.com/r/programming/

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 0.54 (0.34-0.59) | 2.09 (1.38-2.92) | zebkit, 74% less |
| CPU at rest (20 s after), % of a core, % | 5.46 (5.06-6.2) | 11.3 (10.3-28.3) | zebkit, 52% less |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0 (0-0.3) | level |
| memory of all processes, MB | 355 (352-356) | 536 (507-592) | zebkit, 34% less |
| processes | 8 (8-8) | 8 (7-8) | level |
| renderers | 2 (2-2) | 2 (2-2) | level |

Load before a run: 2.8 to 3.3. Runs: 5 of each.

Titles, zebkit: ['Reddit - Prove your humanity (5)']; Electron: ['programming (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 2, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 2, 'gpu-process': 1}.

### www.nytimes.com/

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 2.55 (2.3-3.53) | 0.69 (0.65-4.29) | Electron, 73% less, within noise |
| CPU at rest (20 s after), % of a core, % | 45.1 (42.8-45.8) | 45.5 (0.05-61.6) | zebkit, 1% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0.1 (0.05-0.15) | 0.1 (0-0.2) | level |
| memory of all processes, MB | 828 (789-849) | 810 (247-882) | Electron, 2% less, within noise |
| processes | 21.0 (20.0-21.0) | 12.0 (6-18.0) | Electron, 43% less |
| renderers | 14.0 (13.0-14.0) | 6 (1-12.0) | Electron, 57% less |

Load before a run: 2.5 to 4.5. Runs: 5 of each.

Titles, zebkit: ['The New York Times - Breaking News, US News, World (5)']; Electron: ['The New York Times - Breaking News, US News, World (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:audio': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 13, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

### www.bbc.com/news

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 1.31 (1.1-1.76) | 1.51 (0.43-2.03) | zebkit, 13% less, within noise |
| CPU at rest (20 s after), % of a core, % | 14.9 (13.1-17.9) | 14.2 (8.06-16.6) | Electron, 5% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0.05) | 0 (0-0.1) | level |
| memory of all processes, MB | 426 (419-434) | 452 (341-462) | zebkit, 6% less, within noise |
| processes | 11.0 (10.0-11.0) | 9 (6-9) | Electron, 18% less |
| renderers | 5 (4-5) | 4 (1-4) | Electron, 20% less, within noise |

Load before a run: 1.8 to 3.0. Runs: 5 of each.

Titles, zebkit: ['BBC News - Breaking news, video and the latest top (5)']; Electron: ['BBC News - Breaking news, video and the latest top (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 4, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 4, 'gpu-process': 1}.

### www.theverge.com/

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 2.96 (2.23-3.53) | 2.15 (0.42-3.98) | Electron, 27% less, within noise |
| CPU at rest (20 s after), % of a core, % | 20.4 (17.9-21.6) | 21.1 (15.9-25.2) | zebkit, 3% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0.05) | 0.05 (0-0.05) | zebkit, 100% less, within noise |
| memory of all processes, MB | 609 (535-645) | 603 (451-664) | Electron, 1% less, within noise |
| processes | 11.0 (11.0-11.0) | 22.0 (7-24.0) | zebkit, 50% less, within noise |
| renderers | 5 (5-5) | 17.0 (2-19.0) | zebkit, 71% less, within noise |

Load before a run: 1.2 to 3.0. Runs: 5 of each.

Titles, zebkit: ['The Verge (5)']; Electron: ['The Verge (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 5, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 17, 'gpu-process': 1}.

### stackoverflow.com/questions/11227809

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 1.27 (0.22-1.3) | 1.69 (1.2-1.79) | zebkit, 25% less, within noise |
| CPU at rest (20 s after), % of a core, % | 25.7 (9.8-27.2) | 23.4 (21.6-26.5) | Electron, 9% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0.1 (0.05-0.15) | 0.05 (0.05-0.05) | Electron, 50% less, within noise |
| memory of all processes, MB | 288 (282-290) | 530 (471-571) | zebkit, 46% less |
| processes | 7 (7-7) | 7 (7-9) | level |
| renderers | 1 (1-1) | 2 (2-4) | zebkit, 50% less |

Load before a run: 1.4 to 4.3. Runs: 5 of each.

Titles, zebkit: ['java - Why is conditional processing of a sorted a (5)']; Electron: ['Just a moment... (4)', 'java - Why is conditional processing of a sorted a (1)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 2, 'gpu-process': 1}.

### www.apple.com/

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 1.47 (1.44-1.54) | 1.71 (1.7-1.82) | zebkit, 14% less |
| CPU at rest (20 s after), % of a core, % | 0.05 (0-0.2) | 0.05 (0-0.1) | level |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0 (0-0) | level |
| memory of all processes, MB | 321 (320-321) | 342 (339-350) | zebkit, 6% less |
| processes | 7 (7-7) | 6 (6-6) | Electron, 14% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 1.6 to 2.7. Runs: 5 of each.

Titles, zebkit: ['Apple (5)']; Electron: ['Apple (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

### developer.mozilla.org/en-US/docs/Web/JavaScript

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 0.66 (0.66-0.71) | 0.98 (0.89-0.99) | zebkit, 33% less |
| CPU at rest (20 s after), % of a core, % | 0.05 (0-0.1) | 0 (0-0.15) | Electron, 100% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0 (0-0) | level |
| memory of all processes, MB | 238 (236-238) | 288 (285-290) | zebkit, 18% less |
| processes | 7 (7-7) | 6 (6-6) | Electron, 14% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 1.3 to 2.2. Runs: 5 of each.

Titles, zebkit: ['JavaScript | MDN (5)']; Electron: ['JavaScript | MDN (5)']. Processes in a middle run, zebkit: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'utility:network': 1, 'zygote': 2, 'renderer': 1, 'gpu-process': 1}.

Left out of the sums, for showing a different page in some run: www.reddit.com/r/programming/, stackoverflow.com/questions/11227809.

Sums of the medians over 10 pages, zebkit / Electron: CPU loading 18.3 / 19.8 s (zebkit less on 8), CPU at rest 119.6 / 130.6 % of a core (zebkit less on 6), memory 4495 / 4661 MB (zebkit less on 7).

Runs discarded for a machine that was not quiet: 0. Runs that failed: 0.
