# Passkeys in library hosts

Zebkit reuses Chromium's WebAuthn/FIDO implementation. Its browser adapter
owns request state, authenticator discovery and a versioned host UI protocol.
The host can render authentication inside its own window; a default desktop
UI is used when no host handler was registered before the request.
No Chrome browser UI, extension runtime, password manager or Google account
service is linked in.

## Why the verification page stalled

Blink exposing `navigator.credentials` does not provide a browser's request
UI. Zebkit previously inherited content embedder defaults: no real resident
keys, no macOS window supplied to iCloud discovery, and no phone/PIN/account
selection UI. Host renders through a separate off-screen helper, so attaching
a sheet only to the C example's window could not fix its browser panel.
This is separate from the earlier Google unsafe-browser check and cookie
persistence.

Google decides when its page requests a passkey. Zebkit responds to the
WebAuthn request; it does not trigger authentication just because a Google
page loaded. Cancel returns control to the website, where another offered
verification method can be selected.

## Platform boundaries

| Component | macOS arm64 | Linux x86-64 |
| --- | --- | --- |
| C/Rust/Python events and actions | Shared implementation | Shared implementation |
| Chromium FIDO, USB PIN/account selection, phone hybrid transport | Shared implementation | Shared implementation |
| Host embedded panel and private transport | Shared implementation | Shared implementation |
| Default presentation | Cocoa sheets | Chromium views for visible desktop views |
| OS platform passkeys | iCloud, subject to Apple's entitlement/permission | No built-in platform credential store integration |

These are the engine platforms currently provided by this project. Windows
engine support is not added by this change. Linux runtime/device validation
must be distinguished from compilation checks performed on a Mac.

`src/engine/webauthn.cc` contains the shared controller. `webauthn_ui.h`
is the presentation boundary; `webauthn_mac.mm` and `webauthn_views.cc` only
implement default presentation and necessary OS hooks. Phone/USB protocol,
PIN validation, request ownership and host events are not reimplemented per
platform.

## Host-owned UI

Register the handler before a request:

```c
#include <zebkit/zebkit.h>
#include <zebkit/offscreen.h>

zebkit_view_on_authentication(view, authentication_event, host_data);
zebkit_view_set_authentication_ui(view, true);
zebkit_view_set_focus(view, true); /* synchronize with the active tab/window */
```

`authentication_event` has type:

```c
void authentication_event(zebkit_view *view, const char *event_json, void *data);
```

Callbacks are queued on the engine thread, including closure after a host
changes policy from inside a callback. The APIs run on that thread. JSON strings are UTF-8 and
valid only during the callback: copy them before passing to another thread.
A host rendering elsewhere posts operations back using `zebkit_post_task()`.
A request uses the handler installed at its start; registering one afterward
does not take over a default popup already being shown.

Normal views enable authentication UI by default. Off-screen views start
with it disabled. A human-facing off-screen host must opt in, mark the
view focused, and deliver genuine user input to the page. Recent user
activation is required to start an off-screen request. An installed handler
does not bypass these checks. The host keeps focus synchronized while its
panel is active, including tab switches and application deactivation.

With a host handler, phone, PIN and account-selection states do not create
a default native window. OS-owned permission, biometric and authenticator
prompts may still appear. A separate macOS browser helper may need a temporary
native anchor when the user explicitly selects the iCloud route.

### Event schema (version 1)

Every event includes `version: 1`, a process-local decimal-string `request`
ID, increasing integer `revision`, and `state`. Non-closed events include
the browser-validated `origin`, `rp_id`, a descriptive `title`, and an
`actions` array naming the operations currently allowed.

| State | Additional fields | Host presentation |
| --- | --- | --- |
| `choose` | actions include `system`, optionally `external`, and `cancel` | Choose an available provider |
| `external` | `phone`, `security_key`, `bluetooth`, optionally `qr` | Phone QR and/or USB instructions |
| `pin` | `setting`, `min_pin_length`, `attempts`, optional `error` | Masked PIN, plus confirmation when setting/changing |
| `account` | `accounts`: objects with opaque `id`, `name`, `display_name` | Explicit account selection |
| `waiting` | title, optionally `message`/`attempts` | Progress with Cancel |
| `unavailable` | `message` | Return to website / another verification method |
| `closed` | `outcome` | Dismiss and clear request-specific state |

`bluetooth` is `required`, `ready`, `off` or `denied`. `required` offers the
`bluetooth` action. `off` without that action requires manual system controls;
`denied` requires changing system privacy settings. USB may remain usable.

`qr` is Chromium's opaque FIDO hybrid pairing payload. Render it as a QR
code with sharp square modules and at least four modules of white quiet
zone. It is short-lived secret pairing material, not a URL to navigate to.
Zebkit does not force a platform image format or QR library on hosts. Host
uses its existing QR renderer; the macOS default uses Core Image.

### Actions

Send an operation with the latest event's request and revision:

```c
bool queued = zebkit_view_authentication_action(view,
    "{\"request\":\"42\",\"revision\":3,\"action\":\"cancel\"}");
```

| Action | Extra fields |
| --- | --- |
| `cancel`, `system`, `external`, `bluetooth` | None |
| `pin` | `pin`, and `confirmation` when `setting` is true |
| `account` | `account`: an ID from the latest account list |

Only actions advertised by the current event are accepted. Malformed,
unknown, wrong-view, expired, stale-revision and duplicate pending replies
return false. True means the operation was queued, not that authentication
succeeded. The operation is dispatched asynchronously, so responding from
inside a callback is supported. Non-cancel operations also require the request to remain focused. Events
are bounded to 28 KiB; oversized authenticator metadata ends the request
instead of producing truncated JSON. A newer state can invalidate a queued reply;
the host renders the new state instead of retrying an old operation.

The FIDO implementation validates the PIN and the authenticator performs
verification. A host cannot assert success, manufacture a credential or
bypass origin/RP/user-verification checks. The website consumes the actual
WebAuthn result. `closed.outcome` describes UI teardown (`success`, `failed`,
`cancelled`, `superseded`, `disabled` or `ended`); `ended` includes abort,
timeout or document destruction and is not a successful sign-in.

### Lifecycle and sensitive data

Only one modal request owns a view's UI at a time; a new authorized request
cancels an older request in that view. Always match the request ID when
closing a panel, and reject older revisions. Navigation, abort and timeout
end delivery with `closed` while the view remains alive. A closing view ends
delivery; its normal close event is authoritative if there is no final
`closed` event.

Passing NULL to `zebkit_view_on_authentication()` cancels outstanding requests
and ends delivery before returning, then restores default presentation for
future requests. Handler replacement behaves similarly. The host may then
free the old callback data. Disabling the UI policy cancels current requests
without removing the installed handler. Callback replacement/removal and
closing a view from a callback are supported by the Rust wrapper.

Never persist or log QR pairing payloads, account lists or PIN operations.
Clear PIN/confirmation fields on submission and on any request/state change.
Keep these APIs in trusted application code, not bindings exposed to arbitrary
websites. The localhost bridge in `tests/engine/webauthn_host.c` is explicitly
a test fixture and is never installed in production browsers.

Rust exposes `view.on_authentication()`, `view.authentication_action()` and
`view.clear_authentication_handler()`; JSON parsing is the host's choice.
Python exposes equivalent methods using dictionaries.

## Host integration

Host's helper sends events and receives operations on the inherited private
frames socket, negotiated as wire version 3. PIN/QR material is not sent over
CDP, webpage JavaScript, the agent API or a remote Worker. The desktop stores
request state only in its live tab model, renders a panel over the page,
keeps the site visible, supports Escape/Cancel and masked PIN entry, and
clears inputs on submission/state teardown. Requests are not saved with tabs.

The helper installs a handler only after a version-3 client negotiates it.
Older clients retain default presentation. The helper's `webauthn-host`
feature is enabled only when both the Rust API and package C header expose
the new symbols; older engines remain buildable. The panel and transport
have no macOS-only conditionals.

A generic headless host stays windowless by default. Generic default
Chromium views presentation needs a visible desktop host; an off-screen
Linux application should supply the public handler. The macOS agent helper
can use the default temporary panel when no handler is installed.

## Native providers and system permissions

A nearby phone/tablet uses Chromium's hybrid/caBLE transport, with Bluetooth
and network connectivity. The phone must have a usable credential for the
account. Pairings are not retained. USB security keys use Chromium's HID/FIDO
stack; new biometric enrollment on a security key is not provided.

Apple's iCloud browser API requires macOS 13.5+ and the approved
`com.apple.developer.web-browser.public-key-credential` entitlement on the
executable running the engine, including a separate browser helper. An ad hoc
signature does not grant it. OS and entitlement checks remain intact. See
[Apple's entitlement documentation](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.web-browser.public-key-credential)
and [browser Passkey API](https://developer.apple.com/documentation/authenticationservices/passkey-use-in-web-browsers).

A macOS application using phones must declare
`NSBluetoothAlwaysUsageDescription` in its responsible app's Info.plist.
A helper needing native authentication UI must be an agent (`LSUIElement`),
rather than `LSBackgroundOnly`. The C example supports an approved signing
setup via `EXAMPLE_CODESIGN_IDENTITY`, `EXAMPLE_ENTITLEMENTS` and
`EXAMPLE_PROVISIONING_PROFILE`, and launches through LaunchServices.

A credential stored only in Chrome's Google Password Manager is not imported
into Zebkit. Platform credentials belong to the provider, independently of
Zebkit's cookie profiles. Cookie encryption/persistence and its Keychain
access prompt are separate from authentication UI; see
[Cookie persistence](COOKIE_PERSISTENCE.md). Installing a host handler does
not suppress a system security/permission dialog.

Conditional Passkey autofill, ambient UI and conditional credential upgrades
are not provided and are reported unavailable. Virtual authenticators remain
available for isolated protocol testing.

## Source basis and validation

The implementation follows Chromium's
[embedder delegate](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/content/public/browser/web_authentication_delegate.cc),
[request delegate](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/content/public/browser/authenticator_request_client_delegate.h),
[FIDO discovery factory](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/device/fido/fido_discovery_factory.cc)
and [Chrome's discovery/UI integration](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/chrome/browser/webauthn/chrome_authenticator_request_delegate.cc).
The shared controller preserves those protocol/security checks while replacing
only the browser presentation responsibilities.

`tests/engine/webauthn.py` verifies synthetic resident registration/sign-in,
origin/challenge/RP hash, presence, user verification and ECDSA signatures.
`webauthn_ui.py` tests actual host events, malformed/stale/duplicate replies,
cancel, abort, navigation and handler removal on an owned localhost page.
Host adds private-channel integration, state-ordering/redaction tests and
native panel rendering checks. Virtual tests do not establish successful
physical-device authentication or real Google sign-in.

### Verified on 2026-10-27

The macOS arm64 engine and optimized Host desktop/helper builds passed.
The full engine regression suite passed, including rendering, idle frames,
page/media/browser behavior and encrypted cookie persistence. Rust workspace
and documentation tests passed. Final focused tests also covered an ordinary
headless view with default UI enabled: it rejected real requests without
opening authentication UI, while virtual credentials continued to work.

The owned localhost host-API tests verified cancel, abort, navigation,
callback removal and rejection of malformed/stale/duplicate operations.
The Rust integration also disables UI from inside its callback, receives the
deferred close event, and closes the view from the callback. The C host-API
lifecycle tests also passed in a LaunchServices-launched app with Chromium's
sandbox enabled. Host's real helper/private-channel integration passed in a separate
no-sandbox fixture. Its 43 browser-client unit tests and the authentication
state/QR tests passed, including fresh requests after a helper restart.

The actual Host component rendered five states at four window sizes in both
themes (40 checks). Tests decoded the rendered QR screenshots, and checked
that Cancel remained visible in short panes. Its action footer stays outside
the scrolling body. PIN fields have explicit names and password semantics;
submission and state changes clear the fields and their undo history.

The default macOS QR sheet was also observed in the owned sandboxed app.
Aborting removed the old sheet; another request displayed a fresh sheet.
No Bluetooth permission, real credential, biometric or physical-device
approval was performed during these checks.

Two existing fixture issues were isolated in a separate test-only commit.
The unchanged off-screen executable reproduced the first-resized-frame pixel
race with the previous framework (29/30 checks); the new framework passed its
paired run (30/30). The fixture now waits for the completed scaled content.
The Rust integration executable now uses and removes a per-run Cookie-key
identity, so rebuilding it does not require authorization for a previous
binary's test key. Production Cookie-key behavior and existing keys are
unchanged.

The shared/default-views adapter passed a syntax compilation using the engine
toolchain. **Linux runtime/hardware validation was not performed on this Mac**;
Windows engine support remains outside this change. Successful physical-phone,
USB-key, entitled iCloud/Touch ID authentication and final Google account
sign-in remain unverified. This machine has no approved Apple browser Passkey
provisioning profile.

The slim optimization profiles are unchanged; no full Chrome layer or idle
polling was added. The packaged engine remains about 216 MiB. This is not a
new comparative performance benchmark.

The optimized local test application is `dist/authentication/Host.app`.
Quit the running Host before opening it because it uses the normal Host
profile. The installed application was not overwritten. The native profile
package and `dist/release` contain the final engine. See the
[validation record](reports/passkeys-host-ui.json) for artifact hashes,
platform boundaries and test scope.
