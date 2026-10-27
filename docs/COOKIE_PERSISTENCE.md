# Login cookies across engine restarts

The original slim engine used content-shell's network configuration without
setting a cookie database. Chromium's [NetworkContext file-path contract](https://chromium.googlesource.com/chromium/src/+/main/services/network/public/mojom/network_context.mojom)
explicitly selects an in-memory cookie store when the database name or data
directory is empty. Having a `--user-data-dir` alone was insufficient.

A localhost comparison reproduced the issue with the same profile and
clean engine shutdowns:

| State | Before | After |
|---|---|---|
| Persistent JavaScript cookie after restart | Missing | Restored |
| Persistent HttpOnly authentication cookie | Missing | Sent on the first request after restart |
| localStorage | Restored | Restored |
| Cookie database | Absent | `Network/Cookies` |
| Cookie values on disk | No database | Empty plaintext column, nonempty encrypted value |
| Session cookie without an expiry | Removed on exit | Removed on exit |

## Scope of the fix

The browser client first retains its existing UA, client hints, language
and third-party-cookie configuration. Normal disk-backed storage partitions
then receive a cookie database under:

```text
<user-data-dir>/<relative-partition-path>/Network/Cookies
```

Chromium's own SQLite cookie store and asynchronous encryption provider do
the storage work. In-memory and off-the-record contexts receive no disk
cookie configuration. Expiry and session-cookie semantics remain Chromium's
defaults; the patch does not turn a website's session cookie into a permanent
cookie. Another profile does not inherit the first profile's cookies.

By default on macOS, the engine generates a random AES-256-GCM key in the system
Keychain. The item is scoped by the host's bundle identifier, with its
executable name as the fallback for an unbundled program. It uses its own
`Zebkit Cookies: <host identity>` item, rather than another browser's key.
Keychain operations run on a worker thread. The host identity should stay
stable across updates.

On Linux with D-Bus, the provider uses Chromium's desktop Secret Service /
KWallet implementation. The well-known basic fallback key is rejected for
new cookie encryption. A usable, unlocked OS key store is required; failure
does not enable plaintext cookie storage. The Linux branch has not been
runtime-tested on the Mac used for this repair.

Use the same `--user-data-dir` when restarting. Opening an application from
Finder after testing it with an explicit temporary profile selects its
default profile instead. That is separate from the original missing-database
bug. Cookies lost by an older build cannot be recovered from disk: those
sites require one new login after updating.

## Validation on 2026-10-26

`tests/engine/cookies.py` uses only its own localhost fixture, temporary
profiles and synthetic authentication cookies. It checks:

- Encrypted persistent and HttpOnly cookies on disk after a clean shutdown.
- Authentication on the first HTTP request after restart, and session-cookie expiry.
- Isolation from another profile, including localStorage.
- Logout/deletion remaining effective after a further restart.

It passed against a signed, sandbox-enabled C application on macOS, and
against the command-line runner using the same invocation as the engine
suite. No real site's authentication cookie was inspected or exported. The final
framework (`19cd172b24b537c92babddbc010d150f824d07d637968877cf02d3c985f2ebf4`)
was copied into `dist/release` and the default C example; both hashes match.
The full engine suite passed, and the default C application also passed
all four persistence checks with its sandbox enabled. Google email/Next
still reached the password page after the change; no password was entered.

```sh
python3 -I tests/engine/cookies.py /path/to/engine-runner --headless
# For the Linux suite, opt in when an unlocked desktop keyring is available:
ENGINE_TEST_PERSISTENT_COOKIES=1 scripts/engine-test.sh slim
```

The suite runs this integration test on macOS by default. Headless Linux
builders without a desktop keyring can still run the other engine tests.

The stripped native Mac engine grew from 226,273,408 bytes (the hosted-app
login fix) to 226,306,416 bytes: 33,008 bytes. The historical slim optimization
profiles are unchanged. This adds a cookie store and OS key provider, with
no full Chromium browser layer, password manager or background timer.

The broad GN header checker still reports pre-existing dependency/visibility
issues in the user-agent and PDFium integration. No cookie-source include
errors were reported. Those unrelated issues are outside this patch.


## Host-owned keys without OS key-store dialogs

A host can now opt out of the OS key provider before starting the engine:

```c
/* Generate once using zebkit_random_bytes() from <zebkit/services.h>,
 * save in the application's own storage, and reuse on every restart. */
uint8_t key[32];
if (!host_load_or_create_key(key) ||
    !zebkit_set_cookie_encryption_key(key, sizeof(key))) {
    return 1; /* The application implements host_load_or_create_key(). */
}
return zebkit_main(argc, argv, ready, NULL);
```

The C function copies exactly 32 bytes for AES-256-GCM; NULL/0 restores the
OS default before startup. Configuration is main-thread-only and locks at
`zebkit_main()`. Invalid sizes and later changes return false. Rust exposes
`zebkit::set_cookie_encryption_key(Some(&key))`; Python exposes
`set_cookie_encryption_key(key)`. These are trusted-host interfaces, never
webpage bindings. No key is passed through command-line arguments or CDP.

In host-key mode, Zebkit does not construct its Keychain/Secret Service
provider. Chromium still encrypts the cookie values. It uses
`Network/Cookies.host` with the `zbh` encryption tag, independently of the
OS-encrypted `Network/Cookies`. Existing OS cookies and keys are neither
read nor overwritten. In-memory/off-the-record contexts still have no disk
cookie store. This C interface has no OS-specific storage calls; the host
chooses how to retain the key on Linux, macOS or Windows.

Host's helper opts in when both the package C header and Rust binding expose
the interface. Its host storage is `<profile>/Secrets/cookie.key`: a versioned
53-byte file containing a 32-byte random key. It is generated using the
engine's existing secure random service. POSIX uses a private 0700 directory
and 0600 file, checks ownership, rejects symlinks/nonregular files/hard links,
and publishes a fully written, flushed temporary file without overwriting
an existing key. Windows uses an owner-only protected DACL and rejects
reparse points. The final key is reused, never derived from a user name or
compiled into the program. A missing key with an existing host cookie database,
a malformed key or unacceptable permissions stop helper startup; they do
not trigger key rotation, OS-provider fallback or plaintext cookie storage.

File storage protects against other OS users through file permissions. It
does **not** provide Keychain's application-identity/unlock protection: a
process running as the same user, or a copied profile together with its
Secrets file, can access these cookies. Back up/restore the key and cookie
database together. A host needing stronger protection can supply a key from
its own secure store through the same API.

Switching from OS keys cannot silently decrypt cookies whose key requires
OS authorization. Host starts a separate host cookie store and may require
one new sign-in after switching. The original database is retained so the
change is reversible; changing modes does not merge the two sets of cookies.
This setting does not suppress Passkey, Bluetooth or biometric system UI.

### Validation on 2026-10-27

The owned localhost C fixture verifies host-key persistence across restarts,
encrypted/HttpOnly cookies, first-request authentication, session-cookie
expiry, logout and profile isolation. It also verifies that a pre-existing
OS cookie database remains byte-identical and that changing a key after
startup is rejected. It passed as a command-line no-sandbox fixture and as
an owned signed application launched through LaunchServices with Chromium's
sandbox enabled. The test never uses real accounts or their cookies.
Host's real helper integration independently checks a stable key file and
Cookie authentication state after a helper restart. Storage unit tests cover
missing/corrupt keys, permissions and links.

The file-storage module passes compilation for macOS arm64, Linux x86-64
and Windows x86-64; runtime checks here were performed on macOS. This does
not add a Windows Chromium engine (which the project does not ship yet).

See [the measured validation record](reports/cookie-host.json) for
artifact hashes, size and runtime/compile boundaries.
