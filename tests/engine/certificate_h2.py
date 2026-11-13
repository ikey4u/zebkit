"""A document's TLS errors over HTTP/2 (zebkit_view_on_certificate), on Node's
HTTP/2 server: what is allowed reaches it, what is refused never does, also
with two views on one connection at once. The client is
certificate_h2_client.py, through the Python binding.

    python3 tests/engine/certificate_h2.py <node command...>

Exit 0 when every check passes.
"""
import json
import os
import subprocess
import sys
import threading
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import check, finish, program  # noqa: E402
from tls_fixture import TLSFixture  # noqa: E402

TESTS = Path(__file__).parent


def main():
    node = subprocess.check_output([*sys.argv[1:], '-p', 'process.execPath'], text=True, timeout=30).strip()
    with TLSFixture() as fixture:
        work = Path(fixture.work.name)
        server = subprocess.Popen([node, str(TESTS / 'certificate_h2_server.mjs'), str(work / 'key.pem'),
                                   str(work / 'certificate.pem')], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  text=True)
        thread = None
        try:
            ready = json.loads(server.stdout.readline() or 'null')
            if not check(isinstance(ready, dict) and ready.get('kind') == 'ready', 'the HTTP/2 server is ready', ready):
                return finish()
            received = []

            def read():
                for line in server.stdout:
                    received.append(json.loads(line))
            thread = threading.Thread(target=read, daemon=True)
            thread.start()
            program([sys.executable, '-I', '-B', str(TESTS / 'certificate_h2_client.py'), '--no-sandbox', '--headless',
                     '--disable-gpu', '--user-data-dir=' + str(work / 'h2-profile')],
                    'the client, through the Python binding', r'^\d+ of \d+ checks passed$', 90,
                    dict(os.environ, ZEBKIT_CERTIFICATE_TEST_URL=f"https://localhost:{ready['port']}"))
            paths = {item['path']: item['session'] for item in received}
            check({'/allowed', '/fetch-allow', '/concurrent-allow'} <= paths.keys(),
                  'what was allowed reached the server', paths)
            check('/denied' not in paths and '/concurrent-deny' not in paths,
                  'what was refused, its headers and body, never did', paths)
        finally:
            server.terminate()
            try:
                server.wait(timeout=10)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait(timeout=10)
            if thread:
                thread.join(timeout=5)
    return finish()


if __name__ == '__main__':
    sys.exit(main())
