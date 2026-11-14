# Connection reuse: built, measured, removed

*A record of an experiment, 2026-11-13, on zebkit 0.5.0 and Chromium 157.0.8103.38 (profile slim). It was removed the same day; the engine's network is as in 0.5.0.*

## What was built

- An HTTP cache on disk under the profile (`<profile>/Cache`), with the
  servers' properties (HTTP/2, alternative services) and HSTS kept in
  `<profile>/Network`, as Chrome keeps them; a startup setting `httpCache`
  (disk, memory or off, a size) and `zebkit_clear_http_cache()`.
- `zebkit_preconnect()` and a startup setting `preconnect`: connections opened
  to an origin ahead of a load.
- `zebkit_fetch_in()`: `zebkit_fetch()` in a context's network, with its
  connections and cookies.
- Tried behind a switch and not kept: Chromium's preconnect at the start of
  each navigation, and the spare renderer on by default.

## How it was measured

A local Node.js HTTPS server (HTTP/2 and HTTP/1.1, 40 ms added to each
handshake and response) counted connections, TLS handshakes and requests for
a page of 20 subresources from one origin and one from another; a client
drove zebkit through each case, offline, with medians of 10 runs. Each change
was compared with 0.5.0 as released.

## Key numbers

| Case | 0.5.0 | With the change |
|---|---|---|
| Restart, same profile, HTTP/1.1 (disk cache) | 498 ms | 402 ms |
| Restart, same profile, HTTP/2 (disk cache) | 309 ms | 309 ms (no gain: the critical path is the connections, not the cached images) |
| Preconnect 1 s before a load in a new context | 304 / 498 ms (h2 / h1) | about 81 ms less |
| Preconnect 90 s before | | no gain (an unused socket is closed after 60 s) |
| `zebkit_fetch()` after a page loaded in a context, vs `zebkit_fetch_in()` | 124 ms | 42 ms |
| Navigation preconnect, spare renderer | | no gain (within 1 to 2 ms) |
| Partitioning (Chrome's split cache and connections), a page on another site using a host a first page used | 171 ms | 254 ms (+48 %) |

## Why it was removed

On 2026-11-13 the gains were judged too small for what they cost: four new
functions and two startup settings in every binding, and a cache on disk that
records which sites an application's pages visited. The preconnect gain only
appears when a page follows within about a minute; the restart gain only on
HTTP/1.1; `zebkit_fetch_in()` saves two round trips once per context.

## Findings worth keeping

- zebkit's HTTP cache is in memory (no `http_cache_directory`): at most 2 % of
  RAM, up to 50 MiB, empty after a restart. Cookies are the only network
  state on disk (`docs/notes/cookie-persistence.md`).
- Pages' own `<link rel=preconnect>` and `dns-prefetch` already work, without
  Chrome's LoadingPredictor (`network_hints` is installed).
- TLS sessions are not kept across restarts: net keeps session tickets in
  memory only, so a restart makes full handshakes.
- zebkit shares connections and cache entries across top-level sites; Chrome
  partitions them. Partitioning costs exactly the cross-site reuse (the +48 %
  above) and nothing to pages of one site.
- Each `zebkit_context` has its own connections and cache; Chromium cannot
  share them between profiles.
