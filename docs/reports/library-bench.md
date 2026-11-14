# JavaScript libraries: what a library costs

Engine: zebkit 0.5.0 (Chromium 157.0.8103.38), profile slim, measured 2026-11-14.

`scripts/library-bench.sh`, 20 fresh processes per backend, each followed by another on the same profile ("disk cache": what the first left there); medians. tests/bench/library/bench.c, driven by the program's own loop; the bundles of examples/js-libraries (markdown-it 14.1.0, ECharts 5.6.0, minified). docs/JS_LIBRARIES.md says what each measure is.

| Measure | target (isolate) | isolate |
|---|---|---|
| Load to ready: an empty library (ms) |  | 10.47 |
| Load to ready: markdown-it, first in the process (ms) | 15 | 11.09 |
| Load to ready: markdown-it, again in the process (ms) |  | 10.52 |
| Load to ready: markdown-it, a new process with the disk cache (ms) |  | 7.53 |
| Load to ready: ECharts, first in the process (ms) | 60 | 46.78 |
| Load to ready: ECharts, again in the process (ms) |  | 18.77 |
| Load to ready: ECharts, a new process with the disk cache (ms) |  | 17.27 |
| ECharts' first render: first load / again / disk cache (ms) |  | 26.41 / 11.39 / 10.02 |
| ECharts' next renders, each (ms) |  | 1.89 |
| ECharts' first render after 3 s idle / the next (ms) |  | 8.18 / 5.21 |
| Call, 100 B JSON each way: median (µs) | 50 | 29.75 |
| Call, 100 B JSON each way: p99 (µs) |  | 76.30 |
| 1 MiB part there and back (ms) | 1 | 1.06 |
| host.call() answered at once: median (µs) |  | 45.75 |
| Memory: an empty instance (MiB) | 3 | 1.79 |
| Memory: an empty instance, with the processes it made (MiB, a renderer's PSS) |  | 1.79 |
| Memory: ECharts after 100 renders (MiB) | 30 | 28.83 |
| Memory: ECharts after 100 renders, with the processes it made (MiB) |  | 28.68 |
| Memory: ECharts after 100 renders and 3 s idle (MiB) | 30 | 11.91 |
| 40 ECharts renders at once: 1 instance (ms, wall) |  | 69.79 |
| 40 ECharts renders at once: 4 instances (ms, wall) |  | 30.84 |
