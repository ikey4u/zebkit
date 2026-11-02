# zebkit and Electron 45.2.1 on macOS, measured the same way

`tests/bench/equal/compare.py` on a Mac (Apple silicon, 10 cores, macOS 27, a person's desktop, not otherwise busy: load 2 to 2.7), 2026-11-02: zebkit `slim-macos` (Chromium 155.0.8059.84, without PGO) and Electron 45.2.1 for arm64, each in a window on the real display, taking turns. Medians, lowest and highest run in parentheses. Memory here is the sum of each process's phys_footprint (`footprint`), which leaves shared code out: it is not PSS and is not comparable with the Linux report. No cold start: macOS cannot drop a file from its cache without root. Size: zebkit's framework with its headers 157.0 MiB (47.5 `.tar.xz`, 66.5 `.zip`), Electron.app 257.4 MiB (81.0, 114.9).

## Engines


### Startup, files in cache, profile kept

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| to the page's request, ms | 119 (112-125) | 170 (168-188) | zebkit, 30% less |
| to first paint (FCP), ms | 135 (132-149) | 229 (206-242) | zebkit, 41% less |
| to a frame after load, ms | 134 (128-142) | 203 (195-216) | zebkit, 34% less |
| CPU until then, all processes, s | 0.24 (0.22-0.25) | 0.33 (0.32-0.35) | zebkit, 27% less |
| memory at that frame, MB | 166 (166-167) | 184 (182-184) | zebkit, 10% less |

Load before a run: 2.3 to 2.4. Runs: 7 of each.

### Text layout, a chapter

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| time per layout, the median of the last 50, ms | 4.1 (4.1-4.1) | 3.8 (3.8-3.9) | Electron, 7% less |

Load before a run: 2.2 to 2.4. Runs: 7 of each.

### JavaScript and DOM, tests/bench/index.html

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| all, ms | 408 (404-417) | 347 (343-347) | Electron, 15% less |
| loop, ms | 19.0 (19.0-25.0) | 19.0 (19.0-19.0) | level |
| objects, ms | 20.0 (19.0-21.0) | 19.0 (19.0-19.0) | Electron, 5% less, within noise |
| strings, ms | 6 (6-6) | 5 (5-5) | Electron, 17% less |
| json, ms | 77.0 (77.0-77.0) | 68.0 (67.0-69.0) | Electron, 12% less |
| regex, ms | 95.0 (94.0-98.0) | 73.0 (73.0-75.0) | Electron, 23% less |
| sort, ms | 42.0 (41.0-43.0) | 40.0 (40.0-41.0) | Electron, 5% less, within noise |
| parse, ms | 20.0 (20.0-20.0) | 16.0 (16.0-16.0) | Electron, 20% less |
| gc, ms | 33.0 (32.0-34.0) | 32.0 (30.0-32.0) | Electron, 3% less, within noise |
| jit, ms | 21.0 (20.0-21.0) | 18.0 (18.0-18.0) | Electron, 14% less |
| dom, ms | 74.0 (73.0-77.0) | 55.0 (55.0-56.0) | Electron, 26% less |

Load before a run: 2.2 to 2.3. Runs: 5 of each.

### Calls from the page to the host and back

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| 64 bytes, ms | 0.1 (0.1-0.1) | 0 (0-0) | Electron, 100% less |
| 64 KiB, ms | 0.7 (0.7-0.7) | 0.1 (0.1-0.1) | Electron, 86% less |
| 1 MiB, ms | 9.6 (9.5-9.6) | 0.5 (0.5-0.5) | Electron, 95% less |
| 16 MiB, ms | 151 (150-152) | 6.7 (6.5-6.9) | Electron, 96% less |

Load before a run: 2.2 to 2.9. Runs: 5 of each.

### Long document (200,000 words)

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| to its first frame, ms | 59.0 (58.0-59.0) | 46.0 (46.0-51.0) | Electron, 22% less |
| longest task, ms | 68.0 (66.0-69.0) | 52.0 (51.0-52.0) | Electron, 24% less |
| scrolling frame, p50, ms | 16.7 (16.7-16.7) | 16.7 (16.7-16.7) | level |
| scrolling frame, p95, ms | 18.6 (18.5-18.6) | 18.6 (18.4-18.6) | level |
| scrolling frame, max, ms | 18.7 (18.7-18.8) | 18.7 (18.7-18.8) | level |
| CPU, all processes, s | 0.86 (0.84-0.88) | 0.85 (0.83-0.96) | Electron, 1% less, within noise |
| memory, MB | 277 (276-279) | 295 (295-296) | zebkit, 6% less |

Load before a run: 2.2 to 2.9. Runs: 5 of each.

## Local pages

### blank

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 0.23 (0.23-0.24) | 0.34 (0.34-0.34) | zebkit, 32% less |
| CPU at rest (20 s after), % of a core, % | 0 (0-0.05) | 0 (0-0) | level |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0 (0-0) | level |
| memory of all processes, MB | 57.1 (56.2-57.4) | 69.4 (69.3-69.5) | zebkit, 18% less |
| processes | 5 (5-5) | 4 (4-4) | Electron, 20% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 1.8 to 2.5. Runs: 5 of each.

Titles, zebkit: ['Blank (5)']; Electron: ['Blank (5)']. Processes in a middle run, zebkit: {'browser': 1, 'renderer': 1, 'utility:network': 1, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'renderer': 1, 'utility:network': 1, 'gpu-process': 1}.

### static

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 0.26 (0.25-0.26) | 0.36 (0.35-0.36) | zebkit, 28% less |
| CPU at rest (20 s after), % of a core, % | 0 (0-0.05) | 0 (0-0.05) | level |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0 (0-0) | level |
| memory of all processes, MB | 75.2 (75.1-75.2) | 88.4 (88.3-88.4) | zebkit, 15% less |
| processes | 5 (5-5) | 4 (4-4) | Electron, 20% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 1.8 to 2.3. Runs: 5 of each.

Titles, zebkit: ['Static article (5)']; Electron: ['Static article (5)']. Processes in a middle run, zebkit: {'browser': 1, 'renderer': 1, 'utility:network': 1, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'renderer': 1, 'utility:network': 1, 'gpu-process': 1}.

### heavy

| Measure | zebkit | Electron | Fewer or faster |
|---|---:|---:|---|
| CPU loading (first 10 s), s | 0.61 (0.61-0.64) | 0.68 (0.68-0.73) | zebkit, 10% less |
| CPU at rest (20 s after), % of a core, % | 0.72 (0.63-0.82) | 0.67 (0.62-0.72) | Electron, 7% less, within noise |
| X server's CPU at rest (Linux), % of a core, % | 0 (0-0) | 0 (0-0) | level |
| memory of all processes, MB | 227 (226-267) | 238 (226-276) | zebkit, 5% less, within noise |
| processes | 5 (5-5) | 4 (4-4) | Electron, 20% less |
| renderers | 1 (1-1) | 1 (1-1) | level |

Load before a run: 2.0 to 2.4. Runs: 5 of each.

Titles, zebkit: ['Heavy page (0) (5)']; Electron: ['Heavy page (0) (5)']. Processes in a middle run, zebkit: {'browser': 1, 'renderer': 1, 'utility:network': 1, 'utility:storage': 1, 'gpu-process': 1}; Electron: {'browser': 1, 'renderer': 1, 'utility:network': 1, 'gpu-process': 1}.

Sums of the medians over 3 pages, zebkit / Electron: CPU loading 1.1 / 1.4 s (zebkit less on 3), CPU at rest 0.7 / 0.7 % of a core (zebkit less on 0), memory 359 / 396 MB (zebkit less on 3).

Runs discarded for a machine that was not quiet: 0. Runs that failed: 0.
