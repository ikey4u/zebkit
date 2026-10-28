# Web pages: zebkit and Electron 44.9.0

scripts/web-bench.sh, 2026-10-27. Each cell is zebkit / Electron, the median of 3 runs; bold where zebkit costs less.
Loading: CPU seconds in the first 10 s. At rest: share of one core in the 20 s after. Memory: PSS of every process.

| Page | Loaded (title) | Loading, s | At rest, % | Memory, MB | Renderers |
|---|---|---:|---:|---:|---:|
| local:static | Static article | **0.2** / 0.6 | 0.0 / 0.0 | **172** / 262 | 1 / 1 |
| local:animation | Animation | **0.7** / 1.0 | 5.4 / 4.5 | **179** / 267 | 1 / 1 |
| local:heavy | Heavy page (0) | **1.0** / 1.4 | 0.6 / 0.6 | **321** / 387 | 1 / 1 |
| en.wikipedia.org/wiki/Charles_Darwin | Charles Darwin - Wikipedia | **1.0** / 1.3 | 0.1 / 0.1 | **279** / 351 | 1 / 1 |
| www.google.com/ | Google | **1.2** / 1.5 | **0.1** / 0.3 | **246** / 323 | 1 / 1 |
| www.youtube.com/ | YouTube | **3.3** / 5.1 | 2.0 / 1.9 | **479** / 576 | 2 / 2 |
| github.com/torvalds/linux | GitHub - torvalds/linux: Linux kernel so | **2.7** / 3.2 | 0.1 / 0.1 | **326** / 413 | 1 / 1 |
| www.amazon.com/ | Amazon.com. Spend less. Smile more. | **0.8** / 1.1 | **39.6** / 49.3 | **516** / 550 | 2 / 2 |
| www.reddit.com/r/programming/ | Reddit - Prove your humanity (Electron: programming) | **0.8** / 3.0 | **4.0** / 30.3 | **332** / 617 | 2 / 2 |
| www.nytimes.com/ | The New York Times - Breaking News, US N | **0.3** / 6.5 | 85.9 / 72.7 | **842** / 955 | 14 / 16 |
| www.bbc.com/news | BBC News - Breaking news, video and the  | **2.0** / 2.2 | 20.6 / 18.6 | **498** / 542 | 7 / 8 |
| www.theverge.com/ | The Verge | **0.2** / 4.0 | 43.2 / 35.8 | **750** / 853 | 9 / 17 |
| stackoverflow.com/questions/11227809 | java - Why is conditional processing of  | **0.6** / 0.7 | 42.3 / 41.2 | **697** / 813 | 12 / 11 |
| www.apple.com/ | Apple | **1.2** / 1.5 | 0.1 / 0.1 | **302** / 365 | 1 / 1 |
| developer.mozilla.org/en-US/docs/Web/JavaScript | JavaScript | MDN | **0.8** / 1.1 | 0.0 / 0.0 | **224** / 322 | 1 / 1 |
| **All** | | 16.8 / 34.3 | 244.0 / 255.5 | 6163 / 7596 | |
