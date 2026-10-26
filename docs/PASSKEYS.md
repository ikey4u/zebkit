# Passkeys in a macOS library host

Zebkit's macOS browser client supplies WebAuthn request UI to Chromium's
existing FIDO implementation. No Chrome browser UI, extension runtime,
password manager or Google account service is linked in. A normal visible
view uses the native window
that contains it, including a view attached inside its host's window. The
C/Rust APIs also expose an explicit policy for off-screen views.

## Why the verification page stalled

The page was requesting WebAuthn authentication, which needs browser-side
credential discovery and a request delegate as well as Blink's JavaScript
API. Zebkit previously inherited the content embedder defaults: no real
resident-key support, no native macOS window supplied to iCloud discovery,
and no phone/PIN/account-selection UI. Host also runs its engine in an
off-screen background helper, so adding a sheet only to the example's
visible window would not fix the application. This is separate from the
earlier Google unsafe-browser check and from cookie persistence.

This change implements those embedder responsibilities and gives human-facing
off-screen hosts an explicit UI policy. It preserves Chromium's origin,
relying-party, user-verification and authenticator checks.

## Available authentication routes

- **This Mac** uses Chromium's iCloud Keychain discovery and Apple's
  AuthenticationServices UI, when the signed host is entitled and the user
  has not denied access. Apple handles credential storage, selection and
  fingerprint/device verification.
- **A nearby phone or tablet** uses Chromium's standard FIDO hybrid/caBLE
  transport. The browser shows a fresh QR code, rendered using Core Image.
  The phone performs the authentication. Bluetooth and a network connection
  are required; phone pairings are not retained by this implementation.
- **A USB security key** uses Chromium's HID/FIDO implementation. Native
  sheets collect a required security-key PIN or let the user select a
  returned account. Setting/changing a PIN includes confirmation and FIDO's
  input validation. New biometric enrollment on a key is not supported; a
  request requiring it is canceled rather than left pending.

The relying party controls which transports and credentials it accepts.
A passkey held only in Chrome's Google Password Manager is not imported
into Zebkit. A phone that can use that credential may offer the hybrid
route. Authentication still requires the user's actual credential and
approval; this code never synthesizes successful authentication.

## Apple signing and host configuration

The native iCloud browser API requires macOS 13.5 or later and the
`com.apple.developer.web-browser.public-key-credential` entitlement. Apple
requires an approved organization developer account to obtain this
entitlement. The executable **running the engine**, not only `Zebkit.framework`, must
carry its legitimate entitlement and any required provisioning profile.
This includes a separately launched browser helper in an off-screen host. See Apple's [entitlement documentation](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.web-browser.public-key-credential)
and [passkeys in web browsers](https://developer.apple.com/documentation/authenticationservices/passkey-use-in-web-browsers).

An ad hoc test signature does not grant this approval. Zebkit preserves
Chromium's entitlement and OS-availability checks; it does not replace
them or add a purported entitlement to an ad hoc build. The phone and USB
routes remain usable without the iCloud browser entitlement.

A host that uses phones must put a meaningful
`NSBluetoothAlwaysUsageDescription` in its main Info.plist and launch as a
normal application. Chromium deliberately avoids Bluetooth when macOS's
responsible application lacks this privacy declaration. The C example
includes it and launches through LaunchServices:

```sh
scripts/example.sh macos-basic https://example.com/
```

For an already approved signing setup, the example accepts:

```sh
EXAMPLE_CODESIGN_IDENTITY='your signing identity' \
EXAMPLE_ENTITLEMENTS=/absolute/path/to/approved-entitlements.plist \
EXAMPLE_PROVISIONING_PROFILE=/absolute/path/to/approved.provisionprofile \
scripts/example.sh macos-basic
```

The provisioning-profile variable is optional when the host's signing
configuration does not require one. The entitlements file is supplied by
the host developer; the example refuses to apply it with an ad hoc
identity. Signing the helper/framework does not add the browser privilege
to those executables. The actual application asks the user for Bluetooth
and passkey access through macOS when necessary.

## Request lifetime and scope

Authentication UI is asynchronous and attached to the requesting view's
native window. It identifies the actual calling origin. Canceling the
sheet returns a failed WebAuthn request to the site, so another login
method can be selected. Navigation, an AbortSignal, view destruction,
success and failure close any owned sheet. Weak callbacks prevent an old
sheet from acting on a new request. PINs and QR secrets are never logged
or written to a profile by this code.

A plain headless or detached view cannot present this UI and rejects a
real request. A human-facing off-screen host can explicitly opt in:

```c
zebkit_view_set_authentication_ui(view, true);
zebkit_view_set_focus(view, true);  /* keep synchronized with the active tab */
```

The Rust equivalent is `view.set_authentication_ui(true)`. Off-screen views
start with this policy disabled. After opt-in, a focused view with recent
user activation may show a temporary native authentication window; it is
closed with the request. Hosts must use `LSUIElement` (agent), rather than
`LSBackgroundOnly`, when their browser process needs this UI. A background,
unfocused or non-activated off-screen page cannot open such a window.
Generic headless automation remains windowless. Popups inherit the opener's
policy, but still need their own foreground state and user activation.

The Host integration opts its off-screen views into this policy and keeps
its existing focus messages. Its build script checks for both the new Rust
API and C header before enabling the `webauthn` feature, so older engines
remain buildable. Its app and helper declare Bluetooth use, and the helper
remains an agent without a second Dock icon.

The browser reports platform availability from actual OS support and signing,
rather than from the existence of JavaScript methods. Passkey autofill, ambient UI and conditional credential
upgrades are not provided: the content patch allows the embedder to
report conditional get/create as unavailable instead of inheriting
Chrome's unconditional advertisement. The virtual authenticator testing
API remains available without real native UI.

macOS platform passkeys belong to the OS credential provider and are
shared according to its rules, rather than isolated by a Zebkit cookie
profile. Cookie encryption/persistence remains a separate feature.
The native request UI in this change is macOS-specific; Linux's existing
authentication behavior is unchanged.

## Source basis and validation

The integration uses Chromium's public `WebAuthenticationDelegate` and
`AuthenticatorRequestClientDelegate` interfaces, with the existing
`device::FidoDiscoveryFactory`:

- [Default embedder behavior](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/content/public/browser/web_authentication_delegate.cc)
  does not enable real resident keys or provide macOS authenticator
  configuration for a content-shell host.
- [macOS platform discovery](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/device/fido/fido_discovery_factory.cc)
  needs a native window to discover iCloud Keychain.
- [Native API availability](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/device/fido/mac/icloud_keychain_sys.mm)
  checks the browser entitlement on the process.
- [Chrome's request delegate](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/chrome/browser/webauthn/chrome_authenticator_request_delegate.cc)
  supplies the window, hybrid discovery data and UI missing from a content
  embedder. Zebkit supplies those pieces without linking that delegate.

`tests/engine/webauthn.py` uses its own localhost page, temporary profile
and CDP virtual authenticator. It covers real capability reporting,
windowless rejection, resident registration, discoverable sign-in,
origin/challenge/RP binding, signature verification, and abort. Virtual
credentials are test fixtures and do not establish that a production
Apple signature, phone or physical security key has been exercised.

### Verified on 2026-10-27

Environment: macOS arm64, Chromium 155.0.8059.84, profile
`slim-macos-native`.

- Native engine build and packaging passed. The C example and the Host
  release helper built successfully; the default Host build selects the
  native package and enables the API only when both bindings and headers
  support it. The packaged test applications passed deep, strict code
  signature verification.
- The full engine suite passed, including page, browser, media, encrypted
  cookie persistence, off-screen rendering and idle-frame checks. Rust
  workspace tests and documentation tests passed.
- WebAuthn tests passed in three off-screen modes: UI disabled, opted in
  but unfocused, and opted in/focused without recent user activation. All
  three stayed windowless for real requests, and synthetic resident
  registration/sign-in preserved origin, RP hash, challenge, presence,
  verification and ECDSA signature validity.
- An ad hoc signed, sandbox-enabled agent application, using its own
  localhost page and profile, displayed the actual phone/security-key QR
  sheet. Cancel returned `NotAllowedError`; AbortSignal returned
  `AbortError`; navigating away destroyed the request. A fresh request
  could display a new sheet after either operation. No Bluetooth access
  was granted during these checks.

The off-screen regression executable uses a disposable per-run Keychain
identity. Re-signing a test executable under its old name can otherwise
wait for authorization to read a previous binary's cookie key. The suite
cleans only its own new test key; production cookie encryption and existing
Keychain items are unchanged.

The `slim.gn`, `slim-macos.gn`, `slim-macos-native.gn` and shared optimization
profiles were not modified. This change does not link Chrome's browser
layer or add idle polling. The stripped packaged engine is 226,423,040
bytes (about 216 MiB). The previous local package also predates unrelated
upstream media changes, so its total size delta is not an isolated measure
of the Passkey implementation. No new comparative performance benchmark
is claimed.

### Test build and remaining real-device validation

The local test application is `dist/passkeys/Host.app`; quit the running
Host before opening it, since it uses the normal Host profile. The
installed application was not overwritten. `dist/release` and the native
profile package contain the rebuilt engine. See
[the validation record](reports/passkeys-macos.json) for the test-copy hashes.

This machine has no provisioning profile granting Apple's browser Passkey
entitlement. The native iCloud discovery path is compiled and preserves
Apple's checks, but a successful iCloud/Touch ID authentication was not
tested. Successful phone or physical USB-key authentication and the final
Google account sign-in also remain unverified. The synthetic protocol tests
and native UI checks must not be presented as a completed real-account
sign-in. Passkey autofill and new security-key biometric enrollment remain
outside this implementation.
