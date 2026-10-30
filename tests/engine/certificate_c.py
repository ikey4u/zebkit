"""Keep the HTTPS fixture on the host; optionally run only C with old libc."""
import os
import subprocess
import sys
import tempfile
from tls_fixture import TLSFixture

with TLSFixture() as server, tempfile.TemporaryDirectory(prefix='zebkit-certificate-c-') as profile:
    env = dict(os.environ, ZEBKIT_CERTIFICATE_TEST_URL=server.url)
    env.pop('LD_PRELOAD', None)
    if len(sys.argv) > 2:
        env['LD_LIBRARY_PATH'] = sys.argv[2]
    child = subprocess.run([sys.argv[1], '--no-sandbox', '--headless', '--disable-gpu',
                            '--no-proxy-server', f'--user-data-dir={profile}'],
                           env=env, capture_output=True, text=True, timeout=60)
    print(child.stdout, end='')
    print(child.stderr, end='', file=sys.stderr)
    assert child.returncode == 0, child.returncode
    assert 'ok C certificate policy:' in child.stdout
    assert '/c-allowed' in server.received, server.received
    assert '/c-denied' not in server.received, server.received
    print('ok C TLS denied request never reached the HTTPS server')
