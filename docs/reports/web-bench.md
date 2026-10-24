# Web pages: zebkit and Electron 44.9.0

scripts/web-bench.sh, 2026-10-24. Each cell is zebkit / Electron, the median of 3 runs; bold where zebkit costs less.
Loading: CPU seconds in the first 10 s. At rest: share of one core in the 20 s after. Memory: PSS of every process.

| Page | Loaded (title) | Loading, s | At rest, % | Memory, MB | Renderers |
|---|---|---:|---:|---:|---:|
| local:static | Static article | **0.3** / 0.7 | 0.0 / 0.0 | **179** / 262 | 1 / 1 |
| local:animation | Animation | **1.1** / 1.4 | 8.4 / 7.1 | **188** / 263 | 1 / 1 |
| local:heavy | Heavy page (0) | **1.2** / 1.6 | 0.8 / 0.7 | **325** / 410 | 1 / 1 |
| en.wikipedia.org/wiki/Charles_Darwin | Charles Darwin - Wikipedia | **1.2** / 1.6 | 0.1 / 0.1 | **279** / 352 | 1 / 1 |
| www.google.com/ | Google | **1.5** / 1.9 | 0.1 / 0.1 | **253** / 330 | 1 / 1 |
| www.youtube.com/ | YouTube | **5.5** / 7.6 | **2.2** / 4.3 | 680 / 596 | 2 / 1 |
| github.com/torvalds/linux | GitHub - torvalds/linux: Linux kernel so | **3.0** / 3.8 | 0.1 / 0.1 | **324** / 415 | 1 / 1 |
| www.amazon.com/ | Amazon.com. Spend less. Smile more. | 1.7 / 1.0 | **44.0** / 67.3 | **466** / 571 | 2 / 2 |
| www.reddit.com/r/programming/ | programming | **2.7** / 3.5 | **11.9** / 19.7 | **444** / 549 | 2 / 1 |
| www.nytimes.com/ | The New York Times - Breaking News, US N | **0.5** / 6.3 | 65.5 / 52.6 | **769** / 906 | 14 / 27 |
| www.bbc.com/news | BBC News - Breaking news, video and the  | **2.5** / 2.8 | 21.8 / 20.5 | **458** / 553 | 5 / 6 |
| www.theverge.com/ | The Verge | **0.2** / 4.8 | 33.4 / 32.6 | **572** / 798 | 3 / 16 |
| stackoverflow.com/questions/11227809 | java - Why is conditional processing of  | **0.7** / 1.1 | 47.7 / 36.4 | 672 / 650 | 11 / 9 |
| www.apple.com/ | Apple | **2.6** / 3.8 | **0.1** / 2.1 | **303** / 398 | 1 / 1 |
| developer.mozilla.org/en-US/docs/Web/JavaScript | JavaScript | MDN | **0.8** / 1.4 | 0.0 / 0.0 | **223** / 321 | 1 / 1 |
| **All** | | 25.7 / 43.3 | 236.1 / 243.6 | 6135 / 7374 | |
