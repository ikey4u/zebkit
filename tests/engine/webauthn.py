"""WebAuthn through the actual content service, using CDP test authenticators.

Only localhost, temporary profiles and synthetic credentials are used. This
does not test a real account or claim to test Apple's biometric/signing gate.
Run with the same runner/switches as the engine suite.
"""
import base64
import hashlib
import http.server
import json
import pathlib
import re
import struct
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from cdp_page import Cdp


class Fixture(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b"<title>Owned WebAuthn fixture</title>"
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def main():
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Fixture)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    origin = f"http://localhost:{server.server_port}"
    with tempfile.TemporaryDirectory(prefix="zebkit-webauthn-") as profile:
        log = (pathlib.Path(profile) / "engine.log").open("w")
        engine = subprocess.Popen(
            [sys.argv[1], *sys.argv[2:], f"--user-data-dir={profile}",
             "--remote-debugging-port=0", "about:blank"],
            stdout=subprocess.DEVNULL, stderr=log, text=True)
        try:
            endpoint = None
            deadline = time.monotonic() + 30
            while engine.poll() is None and time.monotonic() < deadline:
                match = re.search(r"ws://\S+", (pathlib.Path(profile) / "engine.log").read_text())
                if match:
                    endpoint = match.group(0)
                    break
                time.sleep(0.1)
            assert endpoint, "engine did not start its local test endpoint"
            cdp = Cdp(endpoint)
            targets = cdp.call("Target.getTargets")["targetInfos"]
            pages = [target for target in targets if target["type"] == "page"]
            target = next((t["targetId"] for t in pages if t["url"].startswith(origin)),
                          pages[0]["targetId"])
            session = cdp.call("Target.attachToTarget", {"targetId": target, "flatten": True})["sessionId"]

            def call(method, params=None):
                return cdp.call(method, params, session=session)

            def evaluate(expression):
                result = call("Runtime.evaluate", {"expression": expression, "awaitPromise": True,
                                                   "returnByValue": True})
                assert not result.get("exceptionDetails"), result
                return result["result"].get("value")

            # Navigate only after attaching. Evaluation during the first
            # renderer's startup races creation of its default V8 context.
            call("Page.enable")
            call("Page.navigate", {"url": origin})
            assert cdp.event("Page.loadEventFired", timeout=15), "fixture did not load"
            for _ in range(100):
                if evaluate("document.title") == "Owned WebAuthn fixture":
                    break
                time.sleep(0.1)
            assert evaluate("isSecureContext && !!globalThis.PublicKeyCredential")
            if sys.platform == "darwin":
                assert evaluate("PublicKeyCredential.isConditionalMediationAvailable()") is False
                assert evaluate("PublicKeyCredential.isUserVerifyingPlatformAuthenticatorAvailable()") is False
                capabilities = evaluate("PublicKeyCredential.getClientCapabilities()")
                assert capabilities["conditionalGet"] is False, capabilities
                assert capabilities["conditionalCreate"] is False, capabilities
                print("ok no autofill or windowless platform capability is falsely advertised", flush=True)
                # Real windowless requests finish instead of waiting for native UI.
                result = evaluate("""(async () => {
                  const controller = new AbortController();
                  const timer = setTimeout(() => controller.abort(), 2500);
                  try {
                    await navigator.credentials.get({publicKey: {
                      challenge: new Uint8Array(32), rpId: 'localhost',
                      allowCredentials: [{type: 'public-key', id: new Uint8Array([1]), transports: ['internal']}]
                    }, signal: controller.signal});
                    return 'unexpected success';
                  } catch (error) { return error.name; }
                  finally { clearTimeout(timer); }
                })()""")
                assert result == "NotAllowedError", result
                print("ok a windowless real request rejects without hanging or fabricating credentials", flush=True)

            call("WebAuthn.enable", {"enableUI": False})
            authenticator = call("WebAuthn.addVirtualAuthenticator", {"options": {
                "protocol": "ctap2", "transport": "internal",
                "hasResidentKey": True, "hasUserVerification": True,
                "isUserVerified": True, "automaticPresenceSimulation": True,
            }})["authenticatorId"]
            assert evaluate("PublicKeyCredential.isUserVerifyingPlatformAuthenticatorAvailable()") is True
            created = evaluate("""(async () => {
              const challenge = Uint8Array.from({length: 32}, (_, i) => i + 1);
              const credential = await navigator.credentials.create({publicKey: {
                challenge, rp: {id: 'localhost', name: 'Owned fixture'},
                user: {id: new Uint8Array([4, 2]), name: 'owned', displayName: 'Owned fixture user'},
                pubKeyCredParams: [{type: 'public-key', alg: -7}],
                authenticatorSelection: {residentKey: 'required', userVerification: 'required'},
                attestation: 'none', timeout: 10000
              }});
              return {id: credential.id, rawId: Array.from(new Uint8Array(credential.rawId)),
                      client: Array.from(new Uint8Array(credential.response.clientDataJSON)),
                      publicKey: Array.from(new Uint8Array(credential.response.getPublicKey()))};
            })()""")
            client = json.loads(bytes(created["client"]))
            assert client["type"] == "webauthn.create" and client["origin"] == origin, client
            assert base64.urlsafe_b64decode(client["challenge"] + "==") == bytes(range(1, 33))
            print("ok resident credential creation preserves the challenge and actual origin", flush=True)
            assertion = evaluate("""(async () => {
              const credential = await navigator.credentials.get({publicKey: {
                challenge: new Uint8Array(32).fill(7), rpId: 'localhost',
                userVerification: 'required', timeout: 10000
              }});
              return {id: credential.id,
                      user: Array.from(new Uint8Array(credential.response.userHandle)),
                      auth: Array.from(new Uint8Array(credential.response.authenticatorData)),
                      client: Array.from(new Uint8Array(credential.response.clientDataJSON)),
                      signature: Array.from(new Uint8Array(credential.response.signature))};
            })()""")
            assert assertion["id"] == created["id"] and assertion["user"] == [4, 2], assertion
            auth = bytes(assertion["auth"])
            assert auth[:32] == hashlib.sha256(b"localhost").digest()
            assert auth[32] & 0x05 == 0x05, "user presence and verification must both be asserted"
            assert struct.unpack(">I", auth[33:37])[0] > 0
            client = json.loads(bytes(assertion["client"]))
            assert client["type"] == "webauthn.get" and client["origin"] == origin, client
            assert base64.urlsafe_b64decode(client["challenge"] + "==") == b"\x07" * 32
            signature_valid = evaluate("""(async () => {
              const key = await crypto.subtle.importKey('spki', new Uint8Array(%s),
                {name: 'ECDSA', namedCurve: 'P-256'}, false, ['verify']);
              const der = new Uint8Array(%s);
              // WebAuthn signs DER ECDSA; WebCrypto expects the two padded scalars.
              let p = 2;
              const raw = new Uint8Array(64);
              for (let i = 0; i < 2; i++) {
                if (der[p++] !== 2) return false;
                const n = der[p++];
                let scalar = der.slice(p, p + n); p += n;
                if (scalar.length === 33 && scalar[0] === 0) scalar = scalar.slice(1);
                if (scalar.length > 32) return false;
                raw.set(scalar, 32 * i + 32 - scalar.length);
              }
              const auth = new Uint8Array(%s);
              const hash = new Uint8Array(await crypto.subtle.digest('SHA-256', new Uint8Array(%s)));
              const signed = new Uint8Array(auth.length + hash.length);
              signed.set(auth); signed.set(hash, auth.length);
              return crypto.subtle.verify({name: 'ECDSA', hash: 'SHA-256'}, key, raw, signed);
            })()""" % tuple(json.dumps(value) for value in
                             (created["publicKey"], assertion["signature"], assertion["auth"], assertion["client"])))
            assert signature_valid is True
            print("ok discoverable login returns the correct account, RP hash, UV flags and valid signature", flush=True)
            wrong_rp = evaluate("""navigator.credentials.get({publicKey: {
              challenge: new Uint8Array(32), rpId: '127.0.0.1'
            }}).then(() => 'unexpected success', error => error.name)""")
            assert wrong_rp == "SecurityError", wrong_rp
            print("ok a site cannot request another relying party's credential", flush=True)
            call("WebAuthn.setAutomaticPresenceSimulation", {
                "authenticatorId": authenticator, "enabled": False})
            aborted = evaluate("""(async () => {
              const controller = new AbortController();
              const pending = navigator.credentials.get({publicKey: {
                challenge: new Uint8Array(32), rpId: 'localhost', timeout: 10000
              }, signal: controller.signal}).then(() => 'unexpected success', error => error.name);
              setTimeout(() => controller.abort(), 100);
              return pending;
            })()""")
            assert aborted == "AbortError", aborted
            call("WebAuthn.removeVirtualAuthenticator", {"authenticatorId": authenticator})
            print("ok abort cancels a pending authenticator request", flush=True)
            print("all WebAuthn checks passed", flush=True)
        except Exception:
            log.flush()
            print("Engine diagnostics:", flush=True)
            diagnostic = (pathlib.Path(profile) / "engine.log").read_text()
            print(diagnostic[:2500], flush=True)
            if len(diagnostic) > 2500:
                print(diagnostic[-5000:], flush=True)
            raise
        finally:
            engine.terminate()
            try:
                engine.wait(timeout=10)
            except subprocess.TimeoutExpired:
                engine.kill()
                engine.wait()
            log.close()
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    main()
