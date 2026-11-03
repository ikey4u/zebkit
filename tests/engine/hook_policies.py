"""One program, one mode, one engine: tests/engine/hook_policies.py <language> <command...>.
The program does what the mode says through its language's binding; this checks what
the servers saw and gives the input a person would, over CDP."""
import atexit
import base64
import json
import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from hook_fixture import HookFixture

MODES = {
    'external': ['external-told', 'external-none', 'external-close'],
    'download': ['download-save', 'download-cancel', 'download-timeout', 'download-close',
                 'download-pending-close', 'download-replace', 'download-default'],
    'certificate': ['cert-provide', 'cert-none', 'cert-cancel', 'cert-timeout', 'cert-close',
                    'cert-pending-close', 'cert-replace', 'cert-default'],
    'world': ['world'],
    # The system's client certificate stores (clientCertificateStores), Python only.
    'certstore': ['cert-store', 'cert-store-off'],
    'page': ['file-open', 'file-multiple', 'file-folder', 'file-cancel', 'file-timeout', 'file-close',
             'file-pending-close', 'file-replace', 'media-allow', 'media-deny', 'media-timeout',
             'media-close', 'media-pending-close', 'media-replace', 'keys', 'menu', 'fullscreen-allow',
             'fullscreen-deny'],
    'popup': ['popup-inherit', 'popup-orphan', 'popup-own', 'two-views', 'cancel-running', 'cdp-order', 'workers-latch'],
    'network': ['net-none', 'net-block', 'net-headers', 'net-redirect', 'net-fulfill', 'net-navigation',
                'net-response', 'net-response-block', 'net-observe', 'net-timeout', 'net-close',
                'net-pending-close', 'net-replace', 'net-filter', 'net-server-redirect', 'net-worker', 'net-post',
                'net-redirect-manual', 'net-redirect-error',
                'net-ws-none', 'net-ws-nofunc', 'net-ws-filter', 'net-ws-block', 'net-ws-continue', 'net-ws-timeout',
                'net-ws-close', 'net-ws-abort', 'net-ws-replace',
                'net-sw-none', 'net-sw-block', 'net-sw-running', 'net-sw-ws',
                'net-shared-none', 'net-shared-block', 'net-shared-running',
                'net-body', 'net-body-none', 'net-body-big', 'net-body-backpressure', 'net-body-error',
                'net-body-close', 'net-body-abort', 'net-body-replace',
                'net-file', 'net-file-missing', 'net-file-close',
                'net-stream', 'net-stream-full', 'net-stream-fail', 'net-stream-close'],
}


class CDP:
    """A small client of the page's target, for Input events."""
    def __init__(self, url):
        host, port = re.match(r'ws://([^:/]+):(\d+)', url).groups()
        for _ in range(200):
            targets = json.load(urllib.request.urlopen(f'http://{host}:{port}/json/list'))
            pages = [t for t in targets if t['type'] == 'page' and t.get('title') == 'ready']
            if pages: break
            time.sleep(.05)
        else:
            raise AssertionError('the page did not get ready: %r' % targets)
        path = re.sub(r'^ws://[^/]+', '', pages[0]['webSocketDebuggerUrl'])
        self.socket = socket.create_connection((host, int(port)), timeout=20)
        key = base64.b64encode(os.urandom(16)).decode()
        self.socket.sendall((f'GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n'
                             f'Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n').encode())
        answer = b''
        while b'\r\n\r\n' not in answer:
            answer += self.socket.recv(4096)
        assert b' 101 ' in answer, answer
        self.next = 0
        self.events = []

    def send(self, method, **params):
        self.next += 1
        data = json.dumps({'id': self.next, 'method': method, 'params': params}).encode()
        mask = os.urandom(4)
        head = b'\x81' + (bytes([0x80 | len(data)]) if len(data) < 126 else b'\xfe' + struct.pack('>H', len(data)))
        self.socket.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))
        while True:
            message = self.message()
            if message.get('id') == self.next:
                assert 'error' not in message, message
                return message
            self.events.append(message)

    def message(self):
        head = self.read(2)
        size = head[1] & 127
        if size == 126: size = struct.unpack('>H', self.read(2))[0]
        elif size == 127: size = struct.unpack('>Q', self.read(8))[0]
        return json.loads(self.read(size))

    def event(self, method):
        while True:
            for message in self.events:
                if message.get('method') == method:
                    self.events.remove(message)
                    return message['params']
            self.events.append(self.message())

    def read(self, size):
        data = b''
        while len(data) < size:
            data += self.socket.recv(size - len(data))
        return data


def cdp_fetch(url):
    # A CDP client intercepting beside the hook: it gets what the hook let go.
    cdp = CDP(url)
    cdp.send('Fetch.enable', patterns=[{'urlPattern': '*/held'}])
    cdp.send('Runtime.evaluate', expression="document.title = 'fetch-on'")
    paused = cdp.event('Fetch.requestPaused')
    assert paused['request']['headers'].get('X-Hook') == 'first', paused
    cdp.send('Fetch.fulfillRequest', requestId=paused['requestId'], responseCode=200,
             body=base64.b64encode(b'from-cdp').decode())


def give_input(mode, url, child):
    # Once: the page has drawn (its title is set after two frames), so it takes input.
    cdp = CDP(url)
    try:
        if mode == 'keys':
            for code, key, modifiers in (('KeyK', 'k', 2), ('KeyB', 'b', 0)):
                common = dict(code=code, key=key, modifiers=modifiers,
                              windowsVirtualKeyCode=ord(key.upper()), nativeVirtualKeyCode=ord(key.upper()))
                cdp.send('Input.dispatchKeyEvent', type='rawKeyDown', **common)
                cdp.send('Input.dispatchKeyEvent', type='keyUp', **common)
        else:
            for kind in ('mousePressed', 'mouseReleased'):
                cdp.send('Input.dispatchMouseEvent', type=kind, x=20, y=30, button='right', buttons=2, clickCount=1)
    except OSError:
        pass  # The program finished and closed the page.


def nss_import_program(work):
    """tests/engine/nss_import.c, built against the system's NSS (or the sysroot's headers)."""
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(os.path.dirname(here))
    sysroot = os.path.join(root, 'upstream', 'chromium', 'src', 'build', 'linux', 'debian_bullseye_amd64-sysroot')
    includes = []
    for base in ('/usr/include', os.path.join(sysroot, 'usr', 'include')):
        if os.path.exists(os.path.join(base, 'nss', 'pk11pub.h')):
            includes = ['-I' + os.path.join(base, 'nss'), '-I' + os.path.join(base, 'nspr')]
            break
    assert includes, 'no NSS headers'
    program = os.path.join(work, 'nss_import')
    subprocess.run(['cc', '-O1', *includes, os.path.join(here, 'nss_import.c'), '-o', program,
                    '-l:libnss3.so', '-l:libnssutil3.so', '-l:libnspr4.so'], check=True)
    return program


def system_store(fixture, work, env):
    """The fixture's client identity in the operating system's store the engine reads."""
    if sys.platform.startswith('linux'):
        # The user's NSS database, ~/.pki/nssdb, in a home of the test's own.
        home = os.path.join(work, 'home')
        database = os.path.join(home, '.pki', 'nssdb')
        os.makedirs(database)
        der, key = os.path.join(work, 'client.der'), os.path.join(work, 'client.pk8')
        subprocess.run(['openssl', 'x509', '-in', str(fixture.dir / 'client.pem'), '-outform', 'DER', '-out', der],
                       check=True)
        subprocess.run(['openssl', 'pkcs8', '-topk8', '-nocrypt', '-in', str(fixture.dir / 'client.key'),
                        '-outform', 'DER', '-out', key], check=True)
        subprocess.run([nss_import_program(work), database, der, key, 'zebkit-client'], check=True,
                       stdout=subprocess.DEVNULL)
        env['HOME'] = home
        return lambda: None
    pem, key = str(fixture.dir / 'client.pem'), str(fixture.dir / 'client.key')
    p12 = os.path.join(work, 'client.p12')
    # The older ciphers, which Keychain and every Windows import read.
    export = ['openssl', 'pkcs12', '-export', '-inkey', key, '-in', pem, '-out', p12, '-passout', 'pass:zebkit']
    if subprocess.run(export + ['-legacy'], capture_output=True).returncode != 0:
        subprocess.run(export + ['-keypbe', 'PBE-SHA1-3DES', '-certpbe', 'PBE-SHA1-3DES', '-macalg', 'sha1'],
                       check=True, capture_output=True)
    if sys.platform == 'darwin':
        # A keychain of the test's own, first in the user's search list for the
        # run; any application may use its key, so that no access prompt shows.
        keychain = os.path.join(work, 'zebkit-test.keychain-db')
        listed = subprocess.run(['security', 'list-keychains', '-d', 'user'], check=True,
                                capture_output=True, text=True).stdout
        before = [line.strip().strip('"') for line in listed.splitlines() if line.strip()]
        subprocess.run(['security', 'create-keychain', '-p', 'zebkit', keychain], check=True)
        subprocess.run(['security', 'unlock-keychain', '-p', 'zebkit', keychain], check=True)
        subprocess.run(['security', 'set-keychain-settings', keychain], check=True)
        subprocess.run(['security', 'import', p12, '-k', keychain, '-P', 'zebkit', '-f', 'pkcs12', '-A'],
                       check=True, capture_output=True)
        subprocess.run(['security', 'list-keychains', '-d', 'user', '-s', keychain, *before], check=True)
        done = []
        def undo():
            if not done:
                done.append(True)
                subprocess.run(['security', 'list-keychains', '-d', 'user', '-s', *before])
                subprocess.run(['security', 'delete-keychain', keychain], capture_output=True)
        return undo
    if sys.platform == 'win32':
        # The user's MY store, as the Windows certificate dialog fills it.
        script = ("$p = ConvertTo-SecureString -String 'zebkit' -AsPlainText -Force; "
                  f"(Import-PfxCertificate -FilePath '{p12}' -CertStoreLocation Cert:\\CurrentUser\\My "
                  "-Password $p).Thumbprint")
        thumbprint = subprocess.run(['powershell', '-NoProfile', '-Command', script], check=True,
                                    capture_output=True, text=True).stdout.strip().splitlines()[-1]
        def undo():
            subprocess.run(['powershell', '-NoProfile', '-Command',
                            f'Remove-Item Cert:\\CurrentUser\\My\\{thumbprint}'])
        return undo
    raise AssertionError('no system store set up for ' + sys.platform)


def run(language, command, mode):
    with HookFixture() as fixture, tempfile.TemporaryDirectory(prefix='zebkit-hook-run-') as work:
        env = dict(os.environ, **fixture.env(mode, work))
        env.pop('LD_PRELOAD', None)
        undo_store = system_store(fixture, work, env) if mode.startswith('cert-store') else (lambda: None)
        # Also when the run fails: the user's own store is left as it was.
        atexit.register(undo_store)
        # This program stays on the host's libraries; only the child gets others.
        if os.environ.get('ZEBKIT_HOOK_CHILD_LIBRARY_PATH'):
            env['LD_LIBRARY_PATH'] = os.environ['ZEBKIT_HOOK_CHILD_LIBRARY_PATH']
        switches = ['--no-sandbox', '--headless', '--disable-gpu', '--no-proxy-server',
                    '--use-fake-device-for-media-stream', '--remote-debugging-port=0',
                    f'--user-data-dir={work}/profile']
        # zebkit-js takes its switches before the script, Python's program after it.
        full = command + switches if language == 'python' else command[:1] + switches + command[1:]
        if language == 'javascript':
            full.append(json.dumps(dict(fixture.env(mode, work), cert=(fixture.dir / 'client.pem').read_text(),
                                        key=(fixture.dir / 'client.key').read_text(),
                                        otherKey=(fixture.dir / 'stranger.key').read_text())))
        child = subprocess.Popen(full, env=env, text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        errors, outputs = [], []
        # One reader for each pipe: communicate() would read standard error too, and
        # take the DevTools line from the thread that looks for it.
        def drain_out():
            outputs.append(child.stdout.read())
        out_thread = threading.Thread(target=drain_out, daemon=True)
        out_thread.start()
        url = threading.Event()
        found = {}
        def drain_err():
            for line in child.stderr:
                errors.append(line)
                match = re.search(r'ws://\S+', line)
                if match and not url.is_set():
                    found['url'] = match.group(0)
                    url.set()
        thread = threading.Thread(target=drain_err, daemon=True)
        thread.start()
        driver_error = []
        def driver():
            try:
                assert url.wait(20), 'no DevTools line'
                if mode == 'cdp-order':
                    cdp_fetch(found['url'])
                else:
                    give_input(mode, found['url'], child)
            except Exception as error:  # noqa: BLE001
                driver_error.append(repr(error))
        if mode in ('keys', 'menu', 'cdp-order'):
            threading.Thread(target=driver, daemon=True).start()
        try:
            child.wait(timeout=60)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait()
            out_thread.join(5)
            raise AssertionError(f'{language} {mode} hung:\n{"".join(outputs)}\n{"".join(errors[-20:])}')
        out_thread.join(5)
        thread.join(2)
        out = ''.join(outputs)
        problem = (child.returncode != 0 or f'ok {language} {mode}\n' not in out or driver_error)
        if problem:
            raise AssertionError(f'{language} {mode} exit {child.returncode} {driver_error}:\n{out}\n{"".join(errors[-20:])}')
        # What the servers saw, whatever the program believes.
        undo_store()
        if mode in ('cert-provide', 'cert-store'):
            assert 'zebkit-client' in fixture.peers, fixture.peers
        if mode == 'cert-store-off':
            assert fixture.peers and set(fixture.peers) == {'none'}, fixture.peers
        if mode in ('cert-none', 'cert-default'):
            assert fixture.peers and set(fixture.peers) == {'none'}, fixture.peers
        if mode in ('cert-cancel', 'cert-timeout', 'cert-close', 'cert-pending-close', 'cert-replace'):
            assert not fixture.peers, fixture.peers
        if mode in ('net-block', 'net-fulfill', 'net-navigation', 'net-redirect'):
            assert '/held' not in fixture.received, fixture.received
        if mode == 'net-flood':
            assert not any('/held' in path for path in fixture.received), fixture.received
        if mode.startswith(('net-file', 'net-stream')):
            assert '/held' not in fixture.received, fixture.received
        if mode in ('net-redirect-manual', 'net-redirect-error'):
            assert '/held' not in fixture.received and '/echo' not in fixture.received, fixture.received
        if mode in ('net-ws-block', 'net-ws-close', 'net-ws-abort', 'net-sw-ws'):
            assert '/ws' not in fixture.received, fixture.received
        if mode in ('net-ws-none', 'net-ws-nofunc', 'net-ws-filter', 'net-ws-continue', 'net-ws-timeout', 'net-ws-replace'):
            assert '/ws' in fixture.received, fixture.received
        if mode in ('net-ws-continue', 'net-headers'):
            assert any(h.get('X-Zebkit') == 'yes' for h in fixture.echoed), fixture.echoed
        # A refused worker's fetch reaches nobody; one that ran before the policy did, once.
        if mode in ('net-sw-block', 'net-shared-block'):
            assert '/worker-held' not in fixture.received, fixture.received
        if mode in ('net-sw-none', 'net-shared-none', 'net-sw-running', 'net-shared-running'):
            assert fixture.received.count('/worker-held') == 1, fixture.received
        # Through, refused with its view gone, through after a takeover and after off: each worker.
        if mode == 'workers-latch':
            assert fixture.received.count('/worker-held') == 6, fixture.received
        if mode == 'net-headers':
            assert any(h.get('X-Zebkit') == 'yes' for h in fixture.echoed), fixture.echoed
        print(f'ok {language} {mode}', flush=True)


language, group = sys.argv[1].split(':')
# More requests than may wait: what asks them is the same in every language.
if group == 'network' and language == 'c':
    MODES[group].append('net-flood')
for mode in (os.environ["ONLY"].split(",") if os.environ.get("ONLY") else MODES[group]):
    run(language, sys.argv[2:], mode)
