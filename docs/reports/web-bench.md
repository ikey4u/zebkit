# Web pages: zebkit and Electron 45.2.1

Engine: zebkit 0.5.0 (Chromium 157.0.8103.38), profile slim, measured 2026-11-13; Electron 45.2.1.

scripts/web-bench.sh, 2026-11-13. Each cell is zebkit / Electron, the median of 3 runs; bold where zebkit costs less.
Loading: CPU seconds in the first 10 s. At rest: share of one core in the 20 s after. Memory: PSS of every process.

| Page | Loaded (title) | Loading, s | At rest, % | Memory, MB | Renderers |
|---|---|---:|---:|---:|---:|
| local:static | Static article | **0.2** / 0.5 | **0.0** / 0.1 | **169** / 235 | 1 / 1 |
| local:animation | Animation | **0.8** / 1.0 | 6.3 / 5.9 | **179** / 241 | 1 / 1 |
| local:heavy | Heavy page (0) | **1.0** / 1.3 | 0.5 / 0.5 | **323** / 382 | 1 / 1 |
| **All** | | 2.1 / 2.8 | 6.8 / 6.5 | 671 / 858 | |
