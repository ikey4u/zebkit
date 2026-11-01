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


## Built-in storage backends

The file backend belongs to **Zebkit's C++ engine**. Hosts choose a backend;
they do not generate keys, implement permissions or maintain key files.
The default remains the OS key store. To select private-file storage:

```c
if (!zebkit_set_cookie_storage(ZEBKIT_COOKIE_STORAGE_FILE, NULL)) {
    return 1;
}
return zebkit_main(argc, argv, ready, NULL);
```

```rust
assert!(zebkit::set_cookie_storage(zebkit::CookieStorage::File, None));
// zebkit::run(...) or run_with_args(...) starts the engine as usual.
```

```python
zebkit.set_cookie_storage("file")
# zebkit.main(...) starts the engine as usual.
```

Normal applications/runners can choose it using switches alone:

```sh
browser --zebkit-cookie-storage=file --user-data-dir=/path/to/profile
# Optional, profile-specific key directory:
browser --zebkit-cookie-storage=file --user-data-dir=/path/to/profile \
  --zebkit-cookie-key-directory=/path/to/private-keys
```

C/Rust/Python also accept an optional absolute key directory. NULL/None
uses `<user-data-dir>/Secrets`, resolving the engine's ordinary default
profile if no explicit profile was passed. A custom key directory should
belong to one profile. `ZEBKIT_COOKIE_STORAGE_OS`, `CookieStorage::Os`,
`"os"` or `--zebkit-cookie-storage=os` selects the OS backend. Explicit
switches override the pre-startup selection. No key material is passed
through arguments, CDP or page bindings.

Configuration happens on the main thread before startup. Invalid mode/path
combinations and changes after startup return false. The file is opened
at `zebkit_main()` startup, after resolving the profile, before opening any
cookie database. If it cannot be safely loaded, startup returns 1 without
calling ready, rotating the key or falling back to an OS provider. Selecting
file mode neither constructs nor accesses the Cookie Keychain/Secret
Service provider. In-memory/off-the-record contexts still get no disk store.

### Shared implementation and compatibility

`src/engine/cookie_key_file.cc` implements the built-in backend for all
bindings and hosts. It generates a 32-byte AES-256-GCM key using the engine's
existing secure random source, then atomically publishes a fully written,
flushed private file. POSIX checks owner, 0700 directory/0600 file permissions,
regular-file type and link count, rejecting symlinks. Windows uses an
owner-only protected DACL and rejects reparse points/nonregular files/hard
links. Invalid-format files and missing keys for an existing `Cookies.host`
stop startup. Existing Windows ACLs are validated rather than rewritten.
Temporary publication never overwrites an existing key.

The default file is `<profile>/Secrets/cookie.key`. Its 53-byte version-1
format (`Zebkit Cookie Key v1` plus newline and 32 key bytes), `zbh`
encryption tag and `Network/Cookies.host` database are unchanged from the
previous host implementation. Existing file-backed sign-ins therefore
survive the move into Zebkit without key rotation or another sign-in.
The host now makes one `set_cookie_storage(File, None)` call and contains no
key-file implementation. Source/header detection only enables that call
when building against an engine package with the built-in interface.

The separate OS-encrypted `Network/Cookies` and its OS key are retained.
The initial switch from OS keys to file keys can still require a new sign-in:
keys requiring OS authorization cannot silently be extracted. Switching
backends does not merge their cookie stores.

File storage relies on filesystem permissions. It does not provide the
OS key store's application-identity/unlock protection: another process
running as the same user, or a copied profile together with its key file,
can access these cookies. Back up/restore the key and database together.
Passkey, Bluetooth and biometric system permissions remain independent.

### Host-supplied keys are still available

A host with its own vault, hardware-backed store or other key policy can
continue using `zebkit_set_cookie_encryption_key(key, 32)` before startup.
That API copies the key; the host is responsible for its persistence. NULL/0
restores the OS default. Selecting a built-in backend clears a previously
supplied key, and supplying a key replaces the built-in selection.
Rust exposes `set_cookie_encryption_key(Some(&key))` and Python exposes
`set_cookie_encryption_key(key)`. Ordinary file-backend users do not need it.

### Validation on 2026-10-27

The C integration fixture uses the library's built-in selection directly:
it contains no file-key implementation. It verifies automatic private-key
creation, encrypted/HttpOnly cookies, first-request authentication after
restart, session expiry, logout, separate per-profile keys and unchanged
OS cookie data. Another fixture creates a cookie store with the prior
host-supplied key API, places a compatible former host key file beside it,
and confirms the built-in backend restores that login without replacement.

A normal runner separately tests command-line selection, a Unicode custom
key directory and refusal to start with a missing/invalid key. Its existing
cookie database remains byte-identical on those failures. Standalone C++
backend tests exercise restart, legacy format, permissions and links. Rust
and Python expose the same engine selection. The host's real helper regression
checks file-key reuse and cookies after restart alongside its existing POST
popup/redirect/opener behavior.

The same C++ backend compiles/links for macOS arm64, Linux x86-64 (glibc 2.17)
and Windows x86-64. Runtime validation is performed on macOS; this does not
add a Windows Chromium engine. See the [built-in backend validation record](reports/cookie-file-builtin.json)
for measured artifact and test boundaries. Earlier [host-key validation](reports/cookie-host.json)
remains a historical record of the low-level API.
