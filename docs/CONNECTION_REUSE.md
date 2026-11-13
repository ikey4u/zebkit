# Connection reuse

Status: steps 0, 1, 2 and the `zebkit_fetch_in` step built and measured
(2026-11-13, zebkit 0.5.0 + these changes, Chromium 157.0.8103.38, profile
slim); what was kept and why is under [Results](#results). Goal: pages in
applications load faster because zebkit reuses connections and cached
responses. Each step stays only if the benchmark of step 0 shows it gains
more than it costs.

The facts below are as they were before these changes.

Paths are the repository's; Chromium's are under `upstream/chromium/src`.

## What zebkit does today

| Fact | Where |
|---|---|
| A profile has one network context, shared by all its views: content makes one per storage partition. | `content/browser/storage_partition_impl.cc:3540-3620` |
| `zebkit_fetch()` always uses the engine's own profile, through its browser-process URL loader factory, with credentials. It reuses the connections of views in that profile, never those of a `zebkit_context`. | `src/engine/services.cc:231-234`, `:706` |
| Each `zebkit_context` is a new in-memory profile, so its own network context: its own socket pools and host cache. Only the DNS resolver manager is shared. | `src/engine/view.cc:1569-1577`; `services/network/network_context.cc:1596-1597`, `:2939-2940` |
| zebkit sets the cookie database under the profile, and nothing else on disk for the network. | `src/engine/engine.cc:1022-1034`, `src/engine/cookies.cc:219-248` |
| Without `http_cache_directory` the HTTP cache is in memory, not off: at most 2% of RAM, up to 50 MiB. A restart starts with it empty. | `services/network/public/mojom/network_context.mojom:248-251`; `network_context.cc:646-652`, `:2998-3006`; `net/disk_cache/memory/mem_backend_impl.cc:36-47` |
| Without file names, HTTP server properties (HTTP/2, QUIC alternative services) and transport security state (HSTS) are in memory. Chrome sets both, and a disk cache. | `network_context.mojom:306-316`; `chrome/browser/net/profile_network_context_service.cc:1407-1466` |
| V8's code cache is already on disk, under the profile. | `src/engine/client_defaults.cc:191-194` |
| Cache splitting and connection partitioning by network isolation key are off by default in net. Chrome turns both on in code, not by field trial. zebkit's feature list is the command line's only, and neither call is in `src/` or `patches/`. So zebkit shares sockets and cache entries across top-level sites; Chrome does not. | `net/base/features.cc:213-220`; `chrome/app/chrome_main_delegate.cc:1102-1108`; `src/engine/client_defaults.cc:140-152` |
| A socket group is keyed by destination, privacy mode and, only when partitioning is on, the network anonymization key. | `net/socket/client_socket_pool.h:130-135`, `client_socket_pool.cc:132`, `:166` |
| `<link rel=preconnect>` and `dns-prefetch` work without Chrome's LoadingPredictor: zebkit installs `network_hints`' renderer side and `SimpleNetworkHintsHandlerImpl`, which calls `PreconnectSockets`. | `src/engine/client_defaults.cc:99-102`, `:225`, `:434-436`; `components/network_hints/browser/simple_network_hints_handler_impl.cc:142-167` |
| Preconnect at the start of a navigation is off: content asks `ShouldPreconnectNavigation()`, false by default; zebkit does not override it. Chrome returns true when preloading is enabled. | `content/browser/renderer_host/navigation_request.cc:2164-2180`; `content/public/browser/content_browser_client.cc:1730-1733`; `chrome/browser/chrome_content_browser_client.cc:8823-8841` |
| zebkit has no preconnect API. | `src/engine/include/zebkit/*.h` |
| Idle sockets: a used HTTP/1.1 socket is kept 300 s; the setter is process-wide and only tests call it. An unused (preconnected) one is kept 60 s, fixed. QUIC: 30 s, 300 s with `QuicLongerIdleConnectionTimeout`, or per context with `quic_idle_connection_timeout_seconds`. | `net/socket/client_socket_pool.cc:42`, `:180`; `net/socket/client_socket_pool_manager.cc:208-211`; `net/quic/quic_context.h:67-74`; `network_context.mojom:644`, `network_context.cc:3205-3208` |
| Spare renderer: off unless `--zebkit-spare-renderer`; `spareRenderer` engine setting exists. Back-forward cache: content's default, on; zebkit does not turn it off. | `src/engine/engine.cc:298-313`; `src/engine/include/zebkit/hooks.h:67-71`; `content/public/common/content_features.cc:182` |

## Step 0: the benchmark

`scripts/connection-bench.sh` (`mise run bench:connections`), with
`tests/bench/connections/` (`server.mjs`, `client.py`, `report.py`); the
report is `docs/reports/connection-bench.md` and `.json`:

- **Server** (`server.mjs`, Node.js at mise.toml's pin). One HTTPS port with
  ALPN `h2` and `http/1.1` (`http2.createSecureServer({allowHTTP1: true})`,
  as `tests/engine/certificate_h2_server.mjs` does), one HTTP/1.1-only port.
  It counts TCP connections (`connection`), full TLS handshakes
  (`secureConnection` with `isSessionReused()` false), resumed handshakes,
  HTTP/2 sessions, and requests per connection, and answers them on a third,
  plain port that is not counted. Keep-alive is 10 minutes, since Node's
  default (5 s) would close sockets before the client does. It adds a
  configurable round-trip delay (default 40 ms) before each handshake and
  each response, so that saved round trips show on loopback. HTTP/3: Node.js
  has no stable QUIC server; left out.
- **Certificate and names.** A certificate made with `openssl`, as
  `tests/engine/tls_fixture.py:36-45` makes one, trusted with
  `--ignore-certificate-errors-spki-list`, which needs no `--user-data-dir`
  here (`services/network/ignore_errors_cert_verifier.cc:33-46`, called at
  `network_context.cc:2907`). `--host-resolver-rules="MAP *.bench.test
  127.0.0.1"` gives two origins, as `tests/engine/certificate.py:56` maps a
  name. `--no-proxy-server`.
- **No external network.** The client has only those names; on Linux the run
  can also be in a network namespace with only loopback.
- **Page.** HTML with 20 subresources from `a.bench.test` (half
  `max-age=3600`, half `no-cache` with an `ETag`) and one from `b.bench.test`.
- **Driver** (`client.py`, the Python binding, as `tests/engine/*_python.py`).
  Time from `zebkit_view_load_url()` to `on_load`, and Navigation Timing
  (`connectStart`, `secureConnectionStart`, `connectEnd`, `responseStart`)
  through `zebkit_view_evaluate()`. Each case 10 times; medians.

| Case | What |
|---|---|
| A | first load, new profile |
| B | a second view on the same origin, after A |
| C | `zebkit_fetch()` to the origin after A; C2 the same when A's view is in a `zebkit_context` |
| D | restart with the same profile, load again |
| E | a new `zebkit_context`, same page |
| F, G | reload after 30 s and after 5 min idle |
| H | the page with `<link rel=preconnect>` to `b.bench.test` |
| I | back navigation (back-forward cache) |

As built, where it differs from the above: the delay is a proxy in front of
each counted port that passes every chunk on half a round trip later each
way, and the client's first chunk a round trip more (TCP's own handshake,
which loopback does not cost), so a new HTTPS connection costs two round
trips and a request one. Each host has a certificate of its own (one key,
trusted by its SPKI), so that HTTP/2 cannot pool `a` and `b` on one
connection. Names are `MAP *.test 127.0.0.1`; besides `a.bench.test` and
`b.bench.test` (one site) there are `c.other.test` and `cdn.shared.test`
(other sites, for X and S). The `b` image is added by a script (`late.js`,
`no-cache`) at the end of the page, so that it is found late, as a third-party
resource usually is. Each view is made at `about:blank` and then loaded, so a
time is the navigation's, not the view's making. Added cases: C3
(`zebkit_fetch_in()` after A in the context), D-pre (D with the startup
setting `preconnect`), P1 and P90 (`zebkit_preconnect()` 1 s and 90 s before
E), X (a page on `c.other.test` using the `cdn.shared.test` images a page on
`a.bench.test` used before: what partitioning costs), S (a navigation to
another site with the spare renderer off and on, and the processes' PSS). F
and G run 3 cycles, both protocols at once. The engine's own profile is used
first by a fetch to another host in the C2 session, so that C2 times the
connection, not the making of that profile's network context.

Reported per case: TCP connections, full and resumed handshakes, requests
that reached the server, 304s, load time. Report:
`docs/reports/connection-bench.md` and `.json`, with `scripts/lib.sh`'s
`report_engine` line. Expected today: B, C reuse; C2, D, E cold; F reuses on
HTTP/2 and on HTTP/1.1 (300 s); G reconnects on HTTP/1.1.

## Changes

### 1. Disk cache, server properties, transport security state

- **Chromium.** In zebkit's network context set-up, for a profile that is not
  in memory: `file_paths->http_cache_directory` = `<profile>/Cache`;
  `data_directory` = `<profile>/Network` even when cookies stay in memory
  (today it is set only with a cookie key: `cookies.cc:233-240`);
  `http_server_properties_file_name` = `Network Persistent State`,
  `transport_security_persister_file_name` = `TransportSecurity` (Chrome's
  names); `http_cache_max_size`. An out-of-process network service gets its
  cache file operations brokered by content itself
  (`content/browser/network_service_instance_impl.cc:1046-1060`).
- **API.** A startup setting of `zebkit_init_with_settings()`
  (`zebkit.h:100-113`): `"httpCache": {"storage": "disk" | "memory" | "off",
  "maxSize": bytes}`, default `disk` for a profile on disk, as the code cache
  is. Settings are JSON, so no binding needs a new function for it. Before
  this ships, one function to clear it, since an application must be able to
  forget what was visited: `bool zebkit_clear_http_cache(zebkit_context
  *context, zebkit_task_func done, void *data)`, over
  `NetworkContext::ClearHttpCache` (`network_context.mojom:1182`), in every
  binding.
- **Cost.** Disk up to `maxSize`; 0 is Chromium's choice from free space
  (80 MiB base, larger outside Windows: `net/disk_cache/cache_util.cc:145`,
  `:191-205`). No code size: `disk_cache` is linked already for the code
  cache. Memory: the cache's index.
- **Privacy.** The cache keeps responses of visited sites on disk, per
  profile; the application chooses. `zebkit_context`s stay in memory
  (`network_context.cc:646-652`).
- **Proof.** Case D: requests reaching the server fall by the cacheable
  share, load time falls. Connections and full handshakes on restart do not
  fall: as far as I found, net keeps TLS session tickets in memory only (to
  be confirmed by the benchmark). Server properties matter
  for QUIC (an alternative service known before the first request), which
  this benchmark does not cover; they stay if they cost nothing measurable.
- **Expected.** The largest gain of this plan for applications that restart.

### 2. Preconnect

- **API.** `ZEBKIT_API bool zebkit_preconnect(zebkit_context *context, const
  char *url, int count);` NULL for the engine's profile; http and https only;
  `count` 1 to 6; false off the engine's thread or for a closed context. In
  every binding. And a startup setting `"preconnect": ["https://...", ...]`,
  done for the engine's profile as it starts.
- **Chromium.** `NetworkContext::PreconnectSockets(count, url, kInclude,
  NetworkAnonymizationKey::CreateSameSite(SchemefulSite(url)), no-op
  restrictions id, annotation, nullopt, {})`
  (`network_context.mojom:1675-1697`). `kInclude` and that key are what a
  top-level navigation to `url` uses (`navigation_request.cc:2175-2180`), so
  the sockets match it also if partitioning is turned on later.
- **Also.** Override `ShouldPreconnectNavigation()` to return true, as Chrome
  does: the connection starts with the navigation instead of after it.
- **Cost.** Sockets the page may not use, and a server's resources. An unused
  socket is closed after 60 s, so a preconnect is for a load soon after.
- **Proof.** Case A preceded by `zebkit_preconnect()` 1 s before the load:
  handshakes before the load equal `count`, load time falls by the connection
  set-up (about 2 round trips). The same with the preconnect 90 s before: no
  gain. Navigation preconnect: case A with an existing view, with and without.

### 3. Idle connection lifetime

Little to gain from the client: a used HTTP/1.1 socket already lives 300 s,
the setter is process-wide in the network service and only tests use it, the
60 s for unused sockets is fixed, and no client idle timeout for HTTP/2
sessions was found (they end when the server closes them, on errors, or by
`CloseCurrentIdleSessions`, `net/spdy/spdy_session_pool.cc:415`). Servers usually close first. Only QUIC's timeout is a
parameter (`quic_idle_connection_timeout_seconds`). No change unless HTTP/3 is
added to the benchmark and case G shows a gain. No patch to net is proposed.

### 4. Contexts sharing connections

Chromium has no safe form: each network context builds its own HTTP session
and socket pools (`network_context.cc`, from about `:2900` to `:3220`); only
the DNS resolver manager is shared. No change. Document that a
`zebkit_context` starts cold, and that related views belong in one context.

What can help an application that uses contexts: `zebkit_fetch()` always uses
the engine's profile (`services.cc:231-234`). A `zebkit_fetch_in(context,
...)`, same arguments plus the context, reuses the context's connections and
sends its cookies, which is what an application using contexts expects.
Kept only if case C2 shows the gain.

### 5. Spare renderer and back-forward cache, as measurements

Neither is about connections; both make pages appear sooner. The spare
renderer has its setting; the benchmark measures a navigation to the second
origin with it on and off, and its memory (one renderer process). The
back-forward cache is on; case I checks that it is used
(`src/engine/navigation_hooks.cc:187` reports it). No default changes without
a measured gain larger than the memory.

## Results

Measured 2026-11-13 on this Linux machine, offline, with
`scripts/connection-bench.sh` (40 ms a round trip; medians of 10 runs, 3 for
P90 and for F and G): the baseline is zebkit 0.5.0 as released (Chromium
157.0.8103.38, profile slim); each step is the same engine with that step's
changes. Times in ms; TCP, full TLS handshakes and requests as the server
counted them. The last full run, of the final engine with every kept step
and the baseline beside it, is `docs/reports/connection-bench.md` (D h1 498
-> 402, D-pre 300, P1 224 / 416, C3 42; the other cases as before).

### Baseline (0.5.0)

| Case | h2 ms | h2 TCP / full / requests | h1 ms | h1 TCP / full / requests |
|---|---|---|---|---|
| A first load, new profile | 725 | 2 / 2 / 23 | 499 | 7 / 5 / 23 |
| B second view, after A | 134 | 0 / 0 / 13 (12 x 304) | 133 | 0 / 0 / 13 |
| C `zebkit_fetch()` after A | 42 | 0 / 0 / 1 | 42 | 0 / 0 / 1 |
| C2 `zebkit_fetch()` after A in a context | 124 | 1 / 1 / 1 | 123 | 1 / 1 / 1 |
| D restart, same profile | 309 | 2 / 2 / 23 | 498 | 7 / 5 / 23 |
| E new context | 304 | 2 / 2 / 23 | 498 | 7 / 5 / 23 |
| F after 30 s idle | 131 | 0 / 0 / 13 | 131 | 0 / 0 / 13 |
| G after 5 min idle | 132 | 0 / 0 / 13 | 379 | 7 / 4 (+3 resumed) / 13 |
| H E with `<link rel=preconnect>` | 253 | 2 / 2 / 23 | 416 | 7 / 5 / 23 |
| I back | 1.2, restored from the back-forward cache 10 of 10 | | 1.2, 10 of 10 | |
| X shared host from another site | 171 | 1 / 1 / 2 | 170 | 1 / 1 / 2 |

As the plan expected: B and C reuse; C2, D and E start cold; F reuses on both
protocols; G reconnects on HTTP/1.1 (the server's sockets live 10 minutes,
the client's idle ones 300 s) and not on HTTP/2. A restart (D) resumes no TLS
session: net keeps session tickets in memory only, confirmed. A's spread is
large (574 to 2180 ms over 30 runs on h2): it is the first navigation of a
new process with a new profile; the HTTP/1.1 A, the same process's second
first load, is the steady one.

### Step 1: disk cache, server properties, transport security state — kept

| Case | baseline | step 1 |
|---|---|---|
| D h2: ms, requests | 309, 23 | 309, 13 (12 x 304) |
| D h1: ms, requests | 498, 23 | 415, 13 (20 runs: 503 -> 409, -19%) |
| A h1 (a cold cache, in a warm process) | 499 / 500 | 501 / 501 |
| every other case | | within 1 % |

The restart sends only the revalidations: the 10 `max-age` images come from
the disk cache. On HTTP/1.1, with six connections, that is 19 % of the load;
on HTTP/2 the time is the critical path (connection, page, `late.js`, the
second origin's connection and image), which the cache does not shorten, so
it stays. No cost was measured: the cold-cache load (A on h1, and E) is
unchanged; A on h2 moved +30 ms in both comparisons (725 -> 764, 853 -> 881
over 20 runs), well inside its spread. Disk: 124 KiB for this page; the cap
is Chromium's (80 MiB base, more on a large disk) unless `maxSize` says.
`Network Persistent State` is written (`supports_spdy` of both origins);
neither it nor HSTS changed a measured case (they matter for QUIC and for
`http://` URLs of HSTS hosts, which the benchmark does not have), and they
cost two small files: kept, as Chrome has them. TLS handshakes on restart
did not fall, as expected.

Decision on the default: `disk` for a profile on disk, as Chrome and as the
code cache already is; the application chooses `memory` or `off`
(`httpCache`), and `zebkit_clear_http_cache()` forgets the cache and the
servers' properties and HSTS state. A `zebkit_context` stays in memory. The
temporary profile zebkit makes when there is no home is on disk and deleted
at the end, so its cache goes with it.

### Step 2: preconnect — `zebkit_preconnect()` and `preconnect` kept, the navigation preconnect dropped

| Case | h2 ms | h1 ms |
|---|---|---|
| E (no preconnect) | 307 / 303 | 499 / 499 |
| P1: `zebkit_preconnect(context, origin, 1)` 1 s before E | 225 / 221 (-81) | 418 / 418 (-81) |
| P90: the same 90 s before | 305 (no gain) | 498 (no gain) |
| D | 309 | 398 |
| D-pre: D with the startup setting `preconnect` | 279 (-30) | 333 (-66) |
| E with the navigation preconnect off / on | 305.3 / 303.6 | 498.5 / 496.2 |
| A with it off / on | 815 / 703 | 498 / 498 |
| H with it off / on | 253.0 / 250.7 | 419.1 / 417.7 |

`zebkit_preconnect()` saves the connection's two round trips when the load
follows within the unused-socket lifetime (60 s), and nothing after it (P90:
the socket was closed; one TLS session was resumed, which saved no measurable
time). The startup setting saves part of that on a restart (the view's own
start overlaps it). Both kept: their cost is a connection the application
asked for.

The navigation preconnect (`ShouldPreconnectNavigation()` true, as Chrome's,
measured behind a temporary switch, not opening a connection when the view
has a navigation policy or network rules) gained 1 to 2 ms (under 1 %, inside
the noise; A's 112 ms is A's spread) and opened no extra socket: content
starts the navigation's own request at once, so the preconnect races it and
wins nothing. It helps a browser whose navigations wait (beforeunload,
service workers, throttles); in zebkit the waits are the application's
policies, where a connection before the decision would reach a host the
application may refuse. Dropped, and the code removed.

### `zebkit_fetch_in` — kept

| Case | h2 ms | h1 ms |
|---|---|---|
| C2: `zebkit_fetch()` after A in a context (cold) | 123 (1 connection) | 123 (1 connection) |
| C3: `zebkit_fetch_in(context, ...)` | 42 (0 connections) | 42 (0 connections) |

It reuses the context's connections (-81 ms, two round trips) and sends the
context's cookies, which `zebkit_fetch()` cannot. Kept, in every binding.

### Step 5: spare renderer and back-forward cache — no change

The back-forward cache is used (I: 10 of 10 restored, 1.2 ms, nothing reaches
the server). The spare renderer gained nothing on a navigation to another
site (S: 130.9 off, 131.5 on, h2; 130.0 / 130.1 h1): the new renderer starts
while the navigation's request is on the network, which takes longer, and
costs 4.5 MiB PSS and one process. Its default (off) stays.

### What partitioning would cost (for the owner)

Run with
`--enable-features=SplitCacheByNetworkIsolationKey,PartitionConnectionsByNetworkIsolationKey`
on the 0.5.0 engine (`scripts/connection-bench.sh --partitioned`):

| Case | shared (zebkit today) | partitioned (Chrome) |
|---|---|---|
| X h2: a page on another site using a host a first page used | 171 ms, 1 TCP, 2 requests (1 x 304) | 254 ms (+48 %), 4 TCP, 4 requests (no 304) |
| X h1 | 170 ms, 1 TCP, 2 requests | 253 ms (+49 %), 4 TCP, 4 requests |
| A, B, C, C2, D, E, H, I (one site) | | unchanged (within 1 %) |

So partitioning costs exactly the cross-site reuse: a new connection (two
round trips) and the shared host's resources fetched again instead of read
from the cache; pages of one site lose nothing. An application whose views
show one site or its own pages loses nothing either; one that shows many
sites sharing a CDN pays the X row on each new site, and gains that no site
can learn through the cache or a connection what another showed. Steps 1 and
2 work in both modes: with the final engine partitioned, D still sends 13
requests (12 revalidations; 308 ms h2, 414 h1), D-pre 277 / 316, P1 226 /
416, C3 42, and X costs the same 254 / 253 ms. No partitioning was changed.

## A decision for the owner: partitioning

zebkit shares connections and cache across top-level sites; Chrome partitions
them. Following Chrome would be `HttpCache::SplitCacheFeatureEnableByDefault()`
and `NetworkAnonymizationKey::PartitionByDefault()` early in start-up, as
`chrome/app/chrome_main_delegate.cc:1102-1108` does. It is a privacy gain
(no cross-site tracking through the cache or connections) and costs reuse
between pages of different sites. The benchmark measures the cost with
`--enable-features=SplitCacheByNetworkIsolationKey,PartitionConnectionsByNetworkIsolationKey`.
Steps 1 and 2 work in both modes.

## Order

0, then 1, 2, the navigation preconnect, `zebkit_fetch_in`, the measurements
of 5, done in that order on 2026-11-13 ([Results](#results)). Step 3 only
with HTTP/3; step 4's sharing between contexts stays impossible
(`docs/API.md`, "Connection reuse": contexts start cold).
