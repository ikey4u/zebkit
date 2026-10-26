# Google sign-in: isolating the slim library's rejection

On macOS arm64, the slim C host rejected email/Next with “This browser or
app may not be secure” (`/v3/signin/rejected`, `rrk=46`), while a full
Chromium 155.0.8059.84 build on the same Mac reached the password page.
The full build was a control, not a replacement for the library.

## Official source and the limits of this evidence

Chromium's native bindings explicitly state that `chrome.app` is exposed
["to all webpages"](https://chromium.googlesource.com/chromium/src/+/main/extensions/renderer/native_extension_bindings_system.cc).
The [API feature definition](https://chromium.googlesource.com/chromium/src/+/main/chrome/common/extensions/api/_api_features.json)
includes `web_page`, matches `<all_urls>`, and says any webpage can use the
API. The [schema](https://chromium.googlesource.com/chromium/src/+/main/chrome/common/extensions/api/app.json)
and [native hooks](https://chromium.googlesource.com/chromium/src/+/main/chrome/renderer/extensions/api/app_hooks_delegate.cc)
define the hosted-app queries. These rules were also checked in the
project's pinned Chromium 155.0.8059.84 source.

Google's [browser guidance](https://developers.googleblog.com/en/guidance-to-developers-affected-by-our-effort-to-block-less-secure-browsers-and-applications/)
requires reasonably complete browser features, clear browser identity and
other security properties; it also restricts embedded frameworks and
automation. Its [supported-browser help](https://support.google.com/accounts/answer/7675428?hl=en)
does not publish a `chrome.app` requirement. No official Google document
establishing that specific prerequisite was found. The experiment below
identifies a trigger in the tested environment, not Google's private
implementation or an official promise of embedded sign-in support.

## Cause and controlled comparison

The slim renderer installed `chrome.loadTimes` and `chrome.csi`, but not
`chrome.app`. The missing API was already recorded as a TODO in the
roadmap. Chromium exposes these legacy hosted-app queries to ordinary
web pages even when no app is installed. Its contract is in the pinned
upstream `chrome/common/extensions/api/app.json` and
`chrome/renderer/extensions/api/app_hooks_delegate.cc`.

On 2026-10-26, a diagnostic switch changed only installation of that API.
The same signed C host and slim library binary were used throughout,
with the same machine, network, account, sandbox, UA/client hints and
accessibility setting. Each attempt used a fresh profile. No CDP client
was attached to Google, and no page script was injected.

| Legacy `chrome.app` queries | Email + Next |
|---|---|
| Enabled | Password page |
| Disabled, same binary | Unsafe-browser rejection, `rrk=46` |
| Enabled again, new profile | Password page |

The diagnostic library's SHA-256 was
`1083e2741466d8fae74243b50a4942addd9fe61f23ee419b6c14914b91188b55`.
A localhost fixture separately confirmed that the switch changed the
presence of `chrome.app`, while PDF interfaces stayed absent and
`navigator.webdriver` stayed false. The diagnostic switch is not needed
by the final implementation: ordinary page contexts receive the API.

This isolates the missing API as a reproducible trigger for the observed
rejection in these controlled tests. Google's private decision logic is not
available; the tests do not identify a particular member or internal score.
Only email/Next was exercised. No password was entered, and full login,
MFA and token persistence were not tested.

## Differences that did not require restoring browser subsystems

- Restoring FedCM alone in the slim host still produced the rejection.
- Full Chromium still reached the password page with PDF viewing disabled.
  The same test window reported `pdfViewerEnabled=false` and an empty
  `navigator.plugins` on localhost. Its PDF setting was restored afterward.
- The clean C host had no toolbar, JavaScript bridge or attached DevTools
  session. Those were not needed to reproduce the original failure.

## Library patch

`src/engine/chrome_object.cc` implements the legacy query contract in the
renderer. With no hosted-app registry, the actual state is:

- `isInstalled` and `getIsInstalled()`: `false`.
- `getDetails()`: `null`.
- `runningState()`: `cannot_run`.
- `installState(callback)`: asynchronous `not_installed`.

The state enums follow Chromium's API. Invalid arguments raise `TypeError`;
assignment does not change `isInstalled`. Removed or navigated documents
receive no stale callback, and Blink handles callback exceptions.

No extension framework, browser UI, new process or background timer is
introduced. The C ABI and historical slim optimization profiles are
unchanged. `macos-basic` continues to build the C embedding sample linked
to `Zebkit.framework`.

Regression coverage lives in `tests/engine/pages/browser.html` and
`tests/engine/cdp_browser.py`, both with and without Runtime observation.
The live Google check is separate from those localhost tests.

## Native build and final verification

The final library was rebuilt and copied to `dist/release` and the default
C embedding example. Their framework binary SHA-256 is identical:
`cb01de73eff590d64aa73675d91073c5aca8209aa0c53e57abc41be7a6a1bbf2`.
A fresh-profile minimal C host using that exact framework reached Google's
password page again, with only `--user-data-dir` and the page URL on its
command line. No diagnostic, automation, accessibility or sandbox-disabling
switch was used. No password was entered.

The complete engine suite passed in the local native Mac validation
configuration, including all 31 browser checks. That configuration and
its unrelated native bring-up changes are separate from this focused
hosted-app commit. The committed browser regression can be run against
an already packaged engine through `scripts/engine-test.sh <profile>`. The default C
example also passed the signed, sandbox-enabled localhost smoke check.
The focused browser regression in this commit was separately exercised
from its staged snapshot: 24 of 24 checks passed.

The UI tool denied access to the separately named embedding example, so
its Google interaction was not tested directly; the live check used the
minimal C host with the byte-identical framework.

## Size and performance

These measurements compare the native Mac framework immediately before
and after the hosted-app patch, using the same package/strip/sign process.
They are not a comparison with a differently configured full Chromium
build or with the historical Linux benchmarks.

| Artifact | Before, bytes | After, bytes | Change |
|---|---:|---:|---:|
| Stripped framework binary | 226,273,392 | 226,273,408 | +16 |
| Framework, all regular files | 237,736,658 | 237,736,674 | +16 |

All Mach-O segment sizes remained identical. The added code fits existing
segment alignment; the 16-byte artifact change should not be generalized
to other platforms or link configurations. The existing optimization
profiles (`slim.gn`, `slim-macos.gn`, `_common.gn`) were unchanged.

[Raw samples and artifact sizes](reports/google-signin-macos.json) are
recorded beside this report. Five paired runs alternated the before/after
order, with fresh profiles, the same C host executable, sandbox enabled, and the repository's unchanged
`tests/bench/index.html` workload. Medians on this Mac:

| Metric | Before | After |
|---|---:|---:|
| Process start through workload result | 2.511 s | 2.523 s |
| Sum of the JS/DOM workload timings | 2,287 ms | 2,300 ms |
| DOM workload | 487 ms | 496 ms |
| Aggregate process RSS after settling | 616.44 MiB | 616.36 MiB |
| Idle CPU, two-second sample | 0.0% | 0.0% |
| Processes | 5 | 5 |

RSS sums the process tree and can count shared pages more than once.
Idle CPU values are limited by the sampler's resolution. One first-after
sample was an outlier (4,205 ms of workload time, versus 2,257–2,324 ms
in its other four runs); it remains included in the medians. These local
results show no sustained large regression, not a guarantee for every
workload. The workload runs no hosted-app query; it measures the cost of
adding the interface to normal page contexts.
