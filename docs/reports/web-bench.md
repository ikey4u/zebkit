# Web pages: zebkit and Electron 44.9.0

scripts/web-bench.sh, 2026-10-24. Each cell is zebkit / Electron; bold where zebkit costs less.
Loading: CPU seconds in the first 10 s. At rest: share of one core in the 20 s after. Memory: PSS of every process.

| Page | Loaded (title) | Loading, s | At rest, % | Memory, MB | Renderers |
|---|---|---:|---:|---:|---:|
| local:static | Static article | **0.3** / 0.8 | **0.0** / 0.1 | **179** / 265 | 1 / 1 |
| local:animation | Animation | **1.4** / 1.4 | 10.1 / 7.6 | **186** / 271 | 1 / 1 |
| local:heavy | Heavy page (0) | **1.2** / 1.5 | 0.8 / 0.8 | **326** / 412 | 1 / 1 |
| en.wikipedia.org/wiki/Charles_Darwin | Charles Darwin - Wikipedia | **1.4** / 1.6 | **0.0** / 0.1 | **282** / 353 | 1 / 1 |
| www.google.com/ | Google | **1.6** / 2.0 | 0.1 / 0.1 | **259** / 335 | 1 / 1 |
| www.youtube.com/ | YouTube | **5.3** / 7.3 | **2.7** / 4.8 | 697 / 599 | 2 / 1 |
| github.com/torvalds/linux | GitHub - torvalds/linux: Linux kernel so | **3.1** / 4.1 | 53.3 / 0.2 | **270** / 422 | 1 / 1 |
| www.amazon.com/ | Amazon.com. Spend less. Smile more. | **0.6** / 0.9 | **46.2** / 61.4 | **494** / 636 | 2 / 2 |
| www.reddit.com/r/programming/ | programming | **2.1** / 2.6 | **16.6** / 18.2 | **435** / 554 | 2 / 2 |
| www.nytimes.com/ | The New York Times - Breaking News, US N | **2.4** / 4.8 | 54.8 / 45.9 | **671** / 692 | 13 / 5 |
| www.bbc.com/news | BBC News - Breaking news, video and the  | **1.4** / 2.0 | 17.6 / 9.2 | **376** / 429 | 3 / 3 |
| www.theverge.com/ | The Verge | 2.7 / 0.6 | **2.4** / 25.7 | **387** / 501 | 1 / 1 |
| stackoverflow.com/questions/11227809 | java - Why is conditional processing of  | 1.1 / 0.9 | **11.1** / 15.6 | **345** / 444 | 1 / 1 |
| www.apple.com/ | Apple | **2.6** / 4.3 | 0.0 / 0.0 | **298** / 396 | 1 / 1 |
| developer.mozilla.org/en-US/docs/Web/JavaScript | JavaScript | MDN | **0.9** / 1.4 | 0.0 / 0.0 | **223** / 321 | 1 / 1 |
| **All** | | 28.0 / 36.2 | 215.7 / 189.7 | 5428 / 6630 | |
