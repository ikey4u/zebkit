# Connection reuse benchmark

Engine: zebkit 0.5.0 (Chromium 157.0.8103.38), profile slim, measured 2026-11-13.

`scripts/connection-bench.sh` (tests/bench/connections, docs/CONNECTION_REUSE.md): a local HTTPS
server behind a proxy adding 40 ms a round trip (one more for TCP's handshake), a page of 20
images from its origin (half `max-age=3600`, half `no-cache` with an ETag) and a script that adds
one from a second origin; HTTP/2 (ALPN h2) and HTTP/1.1 on ports of their own. Headless, offline.
Medians of 10 runs (F and G: of the idle runs). TCP: connections the server accepted; full and
resumed: TLS handshakes; requests and 304s: what reached it. Time: from zebkit_view_load_url() to
the load (C: the fetch's answer; I: the back navigation's commit).

## HTTP/2

| Case | What | Time ms | TCP | Full | Resumed | Requests | 304 | baseline ms |
|---|---|---|---|---|---|---|---|---|
| A | first load, new profile | 773.6 | 2 | 2 | 0 | 23 | 0 | 725.2 |
| B | a second view, same origin, after A | 134.4 | 0 | 0 | 0 | 13 | 12 | 133.5 |
| C | zebkit_fetch() to the origin after A | 41.7 | 0 | 0 | 0 | 1 | 0 | 41.6 |
| C2 | zebkit_fetch() after A in a zebkit_context | 123.5 | 1 | 1 | 0 | 1 | 0 | 123.6 |
| C3 | zebkit_fetch_in() the context, after A in it | 41.5 | 0 | 0 | 0 | 1 | 0 | |
| D | restart, same profile, load again | 310.3 | 2 | 2 | 0 | 13 | 12 | 309.1 |
| D-pre | D with the startup setting preconnect | 277.6 | 1 | 3 | 0 | 13 | 12 | |
| E | a new zebkit_context, same page | 304.2 | 2 | 2 | 0 | 23 | 0 | 304.0 |
| F | load again after 30 s idle | 133.6 | 0 | 0 | 0 | 13 | 12 | 130.7 |
| G | load again after 5 min idle | 133.7 | 0 | 0 | 0 | 13 | 12 | 131.6 |
| H | E with <link rel=preconnect> to b | 252.4 | 2 | 2 | 0 | 23 | 0 | 252.8 |
| I | back (back-forward cache) (restored 10 of 10) | 1.2 | 0 | 0 | 0 | 0 | 0 | 1.2 |
| P1 | E after zebkit_preconnect(), 1 s before | 223.8 | 1 | 1 | 0 | 23 | 0 | |
| X | a shared host's resources from another site | 170.5 | 1 | 1 | 0 | 2 | 1 | 170.9 |
| S-off | to another site, no spare renderer | 130.7 | 1 | 1 | 0 | 1 | 0 | 130.9 |
| S-on | to another site, spare renderer | 131.0 | 1 | 1 | 0 | 1 | 0 | 131.5 |

Spare renderer, memory of the engine's processes after the first load: off 156.8 MiB PSS in 7 processes, on 161.3 MiB PSS in 8 processes.

## HTTP/1.1

| Case | What | Time ms | TCP | Full | Resumed | Requests | 304 | baseline ms |
|---|---|---|---|---|---|---|---|---|
| A | first load, new profile | 499.0 | 7 | 5 | 2 | 23 | 0 | 498.8 |
| B | a second view, same origin, after A | 132.8 | 0 | 0 | 0 | 13 | 12 | 132.5 |
| C | zebkit_fetch() to the origin after A | 41.3 | 0 | 0 | 0 | 1 | 0 | 41.5 |
| C2 | zebkit_fetch() after A in a zebkit_context | 123.3 | 1 | 1 | 0 | 1 | 0 | 123.2 |
| C3 | zebkit_fetch_in() the context, after A in it | 41.4 | 0 | 0 | 0 | 1 | 0 | |
| D | restart, same profile, load again | 401.9 | 7 | 5 | 2 | 13 | 12 | 498.4 |
| D-pre | D with the startup setting preconnect | 299.8 | 6 | 4 | 2 | 13 | 12 | |
| E | a new zebkit_context, same page | 498.0 | 7 | 5 | 2 | 23 | 0 | 497.6 |
| F | load again after 30 s idle | 131.6 | 0 | 0 | 0 | 13 | 12 | 130.8 |
| G | load again after 5 min idle | 380.2 | 7 | 4 | 3 | 13 | 12 | 379.3 |
| H | E with <link rel=preconnect> to b | 417.0 | 7 | 5 | 2 | 23 | 0 | 415.9 |
| I | back (back-forward cache) (restored 10 of 10) | 1.2 | 0 | 0 | 0 | 0 | 0 | 1.2 |
| P1 | E after zebkit_preconnect(), 1 s before | 415.7 | 6 | 4 | 2 | 23 | 0 | |
| X | a shared host's resources from another site | 170.3 | 1 | 1 | 0 | 2 | 1 | 170.3 |
| S-off | to another site, no spare renderer | 130.2 | 1 | 1 | 0 | 1 | 0 | 130.0 |
| S-on | to another site, spare renderer | 130.7 | 1 | 1 | 0 | 1 | 0 | 130.1 |

Spare renderer, memory of the engine's processes after the first load: off 184.7 MiB PSS in 9 processes, on 189.1 MiB PSS in 10 processes.

