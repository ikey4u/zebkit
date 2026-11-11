"""The hooks through zebkit's Python binding; run by hook_policies.py."""
import json
import os
import pathlib
import sys
import tempfile
import time
import zebkit
from harness import activated

env = os.environ
MODE, BASE, TLS = env['ZEBKIT_HOOK_MODE'], env['ZEBKIT_HOOK_BASE'], env['ZEBKIT_HOOK_TLS']
FILES, DOWNLOADS = pathlib.Path(env['ZEBKIT_HOOK_FILES']), pathlib.Path(env['ZEBKIT_HOOK_DOWNLOADS'])
CERT = pathlib.Path(env['ZEBKIT_HOOK_CERT']).read_text()
KEY = pathlib.Path(env['ZEBKIT_HOOK_KEY']).read_text()
OTHER_KEY = pathlib.Path(env['ZEBKIT_HOOK_OTHER_KEY']).read_text()
errors = []


def pump(seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        assert zebkit.pump()
        time.sleep(.002)


def wait(condition, label, seconds=15):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        assert not errors, errors
        assert zebkit.pump()
        if condition():
            return
        time.sleep(.002)
    raise AssertionError(label)


def guarded(function):
    def call(*args):
        try:
            function(*args)
        except BaseException as error:  # noqa: BLE001
            errors.append(repr(error))
    return call


def ev(view, script):
    box = []
    view.evaluate(script, lambda value, error=None: box.append((value, error)))
    wait(lambda: box, 'evaluate: ' + script)
    assert box[0][1] is None, box
    return box[0][0]


def opened(url=None):
    view = zebkit.View()
    state = {'closed': False, 'loads': []}
    view.on_close(lambda: state.update(closed=True))
    names = {zebkit.LOAD_FINISHED: 'finished', zebkit.LOAD_FAILED: 'failed'}
    view.on_load(lambda phase, u: state['loads'].append(names.get(phase, 'other')))
    if url:
        view.load(url)
        wait(lambda: view.url == url and 'finished' in state['loads'], 'load ' + url)
        # The first document's own "finished" may be what was seen.
        wait(lambda: ev(view, 'location.href + " " + document.readyState') == url + ' complete', 'loaded ' + url)
    return view, state


def stale(request):
    assert request.info is None, request.info


def external(mode):
    view, state = opened(BASE + '/page')
    events = []
    def told(event):
        events.append(event)
        if mode == 'external-close':
            view.close()
    if mode != 'external-none':
        view.on_external_protocol(guarded(told))
    ev(view, 'location.href = "zoommtg://join?x=1"; 1')
    if mode == 'external-none':
        pump(.3)
        assert ev(view, 'location.pathname') == '/page'
        view.close()
        return
    wait(lambda: events, 'told')
    event = events[0]
    assert event['scheme'] == 'zoommtg' and event['url'] == 'zoommtg://join?x=1' and event['mainFrame'], event
    assert event['initiatorOrigin'] == BASE, event
    if mode == 'external-close':
        wait(lambda: state['closed'], 'closed')
        return
    assert ev(view, 'location.pathname') == '/page'
    view.on_external_protocol(None)
    ev(view, 'location.href = "mailto:a@example.com"; 1')
    pump(.3)
    assert len(events) == 1, events
    view.close()


def download(mode):
    view, state = opened(BASE + '/page')
    target = DOWNLOADS / 'saved.bin'
    target.write_bytes(b'old')
    kept, progress = [], []
    view.on_download_progress(guarded(progress.append))
    def asked(request):
        kept.append(request)
        metadata = request.metadata
        assert metadata['suggestedName'] == 'report.bin' and metadata['totalBytes'] == 65536, metadata
        assert metadata['mimeType'] == 'application/x-zebkit' and request.info == metadata
        assert metadata['timeoutMs'] == (50 if mode == 'download-timeout' else 30000), metadata
        if mode == 'download-save':
            assert not request.respond('save', 'relative/path.bin')
            assert not request.respond('cancel', str(target))
            assert request.info is not None
            assert request.respond('save', str(target))
            assert not request.respond('cancel')
            stale(request)
        elif mode == 'download-cancel':
            assert request.respond('cancel') and not request.respond('cancel')
        elif mode == 'download-close':
            view.close()
    if mode != 'download-default':
        view.on_download(guarded(asked))
    if mode == 'download-timeout':
        for invalid in (0, 300001, True):
            try:
                view.set_download_timeout(invalid)
            except ValueError:
                pass
            else:
                raise AssertionError('deadline accepted')
        view.set_download_timeout(50)
    ev(view, 'location.href = "/download.bin"; 1')
    if mode == 'download-default':
        # Saved in the profile's Downloads, on the page nothing.
        wait(lambda: any(e.get('done') for e in progress), 'default ended')
        pump(.2)
        assert ev(view, 'location.pathname') == '/page'
        done = [e for e in progress if e.get('done')][-1]
        assert done['state'] == 'complete' and done['path'].replace('\\', '/').endswith('/Downloads/report.bin'), progress
        assert os.path.getsize(done['path']) == 65536
        # A file already there is kept, as Chrome keeps it.
        progress.clear()
        ev(view, 'location.href = "/download.bin"; 1')
        wait(lambda: any(e.get('done') for e in progress), 'second default ended')
        again = [e for e in progress if e.get('done')][-1]
        assert again['state'] == 'complete' and again['path'].replace('\\', '/').endswith('/Downloads/report (1).bin'), progress
        assert os.path.getsize(done['path']) == os.path.getsize(again['path']) == 65536
        view.close()
        return
    wait(lambda: kept, 'asked')
    if mode == 'download-save':
        wait(lambda: any(e.get('state') == 'complete' for e in progress), 'complete')
        assert target.read_bytes() == b'z' * 65536
        assert not list(DOWNLOADS.glob('*.crdownload'))
    elif mode == 'download-cancel':
        wait(lambda: any(e.get('state') == 'cancelled' and e.get('done') for e in progress), 'cancelled')
        assert target.read_bytes() == b'old'
    elif mode == 'download-timeout':
        wait(lambda: any(e.get('reason') == 'timeout' for e in progress), 'timeout told')
        stale(kept[0])
        assert not kept[0].respond('save', str(target))
        assert target.read_bytes() == b'old'
    elif mode == 'download-close':
        wait(lambda: state['closed'], 'closed')
        stale(kept[0])
        return
    elif mode == 'download-pending-close':
        view.close()
        wait(lambda: state['closed'], 'closed')
        stale(kept[0])
        assert not kept[0].respond('save', str(target))
        pump(.2)
        assert target.read_bytes() == b'old'
        return
    elif mode == 'download-replace':
        view.on_download(None)
        stale(kept[0])
        wait(lambda: any(e.get('reason') == 'handler-changed' for e in progress), 'replaced told')
        assert target.read_bytes() == b'old'
    view.close()


def certificate(mode):
    view, state = opened()
    # The server's own certificate is not what is being tested.
    view.on_certificate(guarded(lambda request: request.respond(True)))
    kept, results = [], []
    view.on_client_certificate_result(guarded(results.append))
    def asked(request):
        kept.append(request)
        assert request.metadata['host'] == 'localhost' and not request.metadata['proxy'], request.metadata
        assert request.info == request.metadata
        if mode == 'cert-provide':
            assert not request.respond('provide', 'not a certificate', KEY)
            assert not request.respond('provide', CERT, 'not a key')
            assert not request.respond('provide', CERT, OTHER_KEY)
            assert not request.respond('none', CERT, KEY)
            assert request.info is not None
            assert request.respond('provide', CERT, KEY) and not request.respond('none')
            stale(request)
        elif mode == 'cert-none':
            assert request.respond('none')
        elif mode == 'cert-cancel':
            assert request.respond('cancel')
        elif mode == 'cert-close':
            view.close()
        elif mode == 'cert-store':
            # The system's store's identity, chosen; its key stays there.
            found = [i for i in request.metadata['identities'] if i['subject'] == 'zebkit-client']
            assert found, request.metadata['identities']
            assert found[0]['certificate'].startswith('-----BEGIN CERTIFICATE-----')
            assert len(found[0]['sha256']) == 64 and found[0]['validFromUnixMs'] < found[0]['validUntilUnixMs']
            assert not request.choose(len(request.metadata['identities']) + 5)
            assert request.choose(found[0]['index'])
        elif mode == 'cert-store-off':
            # Without the engine setting, the store is not offered.
            assert request.metadata['identities'] == [], request.metadata
            assert request.respond('none')
    if mode == 'cert-store':
        zebkit.set_engine_settings({'clientCertificateStores': True})
    if mode != 'cert-default':
        view.on_client_certificate(guarded(asked))
    if mode == 'cert-timeout':
        view.set_client_certificate_timeout(50)
    view.load(TLS + '/secure')
    if mode in ('cert-provide', 'cert-store'):
        wait(lambda: view.title == 'cert:zebkit-client', 'with the certificate')
    elif mode in ('cert-none', 'cert-default', 'cert-store-off'):
        wait(lambda: view.title == 'cert:none', 'without a certificate')
    elif mode in ('cert-cancel', 'cert-timeout'):
        wait(lambda: 'failed' in state['loads'], 'failed')
        assert results and results[0]['state'] == 'cancelled', results
    elif mode == 'cert-close':
        wait(lambda: state['closed'], 'closed')
        stale(kept[0])
        return
    elif mode == 'cert-pending-close':
        wait(lambda: kept, 'asked')
        view.close()
        wait(lambda: state['closed'], 'closed')
        stale(kept[0])
        assert not kept[0].respond('none')
        return
    elif mode == 'cert-replace':
        wait(lambda: kept, 'asked')
        view.on_client_certificate(None)
        stale(kept[0])
        wait(lambda: 'failed' in state['loads'], 'failed')
        assert results and results[0]['reason'] == 'handler-changed', results
    if mode != 'cert-default':
        assert kept, 'not asked'
    view.close()


ALL = "var worldVar = 1; addEventListener('zkping', () => document.documentElement.setAttribute('data-all', typeof pageVar === 'undefined' ? 'isolated' : 'leak'));"
MAIN = "addEventListener('zkping', () => document.documentElement.setAttribute('data-main', typeof pageVar === 'undefined' ? 'isolated' : 'leak'));"


def world(mode):
    view, state = opened()
    view.add_world_script(ALL, world='zebkit-test')
    view.add_world_script(MAIN, world='zebkit-main', main_frame_only=True)
    view.add_world_script('window.mainWorldVar = 1;', main_frame_only=True)
    try:
        view.add_world_script('1', world='x' * 257)
    except zebkit.Error:
        pass
    else:
        raise AssertionError('long world name accepted')
    view.load(BASE + '/frames')
    wait(lambda: view.url == BASE + '/frames' and 'finished' in state['loads'], 'loaded')
    wait(lambda: ev(view, 'reports.length') == 2, 'frames reported')
    top = ev(view, 'report')
    assert top == {'all': 'isolated', 'main': 'isolated', 'global': 'undefined', 'mainWorld': 'number'}, top
    for frame in ev(view, 'reports'):
        # The second frame is of another site, in a process of its own.
        assert frame == {'all': 'isolated', 'main': None, 'global': 'undefined', 'mainWorld': 'undefined'}, frame
    view.close()


def chooser(mode):
    view, state = opened(BASE + '/page')
    kept, results = [], []
    one, two = str(FILES / 'one.txt'), str(FILES / 'two.txt')
    view.on_file_chooser_result(guarded(results.append))
    def asked(request):
        kept.append(request)
        metadata = request.metadata
        assert request.info == metadata and metadata['origin'] == BASE and metadata['mainFrame'], metadata
        if mode == 'file-open':
            assert metadata['mode'] == 'open', metadata
            assert not request.respond(['relative.txt']) and not request.respond([one, two])
            assert request.respond([one]) and not request.respond([])
            stale(request)
        elif mode == 'file-multiple':
            assert metadata['mode'] == 'open-multiple', metadata
            assert request.respond([one, two])
        elif mode == 'file-folder':
            assert metadata['mode'] == 'upload-folder', metadata
            assert request.respond([str(FILES / 'folder')])
        elif mode == 'file-cancel':
            assert request.respond([])
        elif mode == 'file-close':
            view.close()
    view.on_file_chooser(guarded(asked))
    if mode == 'file-timeout':
        view.set_file_chooser_timeout(50)
    element = {'file-multiple': 'm', 'file-folder': 'd'}.get(mode, 'i')
    click = f"{element}.addEventListener('cancel', () => window.cancelled = 1); {element}.click(); 1"
    if mode == 'file-close':
        # The handler's close can come before the reply to the click, which then fails as the
        # page goes, as CDP fails a detached target's commands.
        box = []
        view.evaluate(click, lambda value, error=None: box.append(error))
        wait(lambda: box, 'evaluate: ' + click)
        # What a call still waiting for its reply is told then (src/engine/view.cc).
        assert box[0] in (None, 'the page target was detached', 'the page was closed', 'the view was closed'), box
    else:
        ev(view, click)
    wait(lambda: kept, 'asked')
    if mode == 'file-open':
        wait(lambda: ev(view, 'i.files.length') == 1, 'file given')
        assert ev(view, 'i.files[0].text()') == 'first file'
    elif mode == 'file-multiple':
        wait(lambda: ev(view, 'm.files.length') == 2, 'files given')
        assert ev(view, 'm.files[1].text()') == 'second file'
    elif mode == 'file-folder':
        wait(lambda: ev(view, 'd.files.length') == 2, 'folder given')
        names = ev(view, '[...d.files].map(f => f.webkitRelativePath).sort()')
        assert names == ['folder/a.txt', 'folder/sub/b.txt'], names
    elif mode in ('file-cancel', 'file-timeout'):
        wait(lambda: results, 'told')
        assert results[0]['state'] == 'cancelled', results
        assert results[0]['reason'] == ('timeout' if mode == 'file-timeout' else 'answered'), results
        wait(lambda: ev(view, 'window.cancelled === 1'), 'the page told of the cancel')
    elif mode == 'file-close':
        wait(lambda: state['closed'], 'closed')
        stale(kept[0])
        return
    elif mode == 'file-pending-close':
        view.close()
        wait(lambda: state['closed'], 'closed')
        stale(kept[0])
        assert not kept[0].respond([one])
        return
    elif mode == 'file-replace':
        view.on_file_chooser(None)
        stale(kept[0])
        wait(lambda: results, 'told')
        assert results[0]['reason'] == 'handler-changed', results
    view.close()


CAPTURE = "navigator.mediaDevices.getUserMedia({video: true}).then(s => { window.tracks = s.getTracks().length; s.getTracks().forEach(t => t.stop()); return 'ok'; }, e => e.name)"


def media(mode):
    view, state = opened(BASE + '/page')
    kept, results, answer = [], [], []
    view.on_media_access_result(guarded(results.append))
    def asked(request):
        kept.append(request)
        metadata = request.metadata
        assert metadata['video'] and not metadata['audio'] and metadata['origin'] == BASE, metadata
        assert metadata['videoDevices'], metadata
        if mode == 'media-allow':
            assert request.respond(True) and not request.respond(False)
            stale(request)
        elif mode == 'media-deny':
            assert not request.respond(False, None, 'device')
            assert request.respond(False)
        elif mode == 'media-close':
            view.close()
    view.on_media_access(guarded(asked))
    if mode == 'media-timeout':
        view.set_media_access_timeout(50)
    view.evaluate(CAPTURE, lambda value, error=None: answer.append(value))
    wait(lambda: kept, 'asked')
    if mode == 'media-allow':
        wait(lambda: answer, 'answered')
        assert answer == ['ok'], answer
        assert ev(view, 'window.tracks') == 1
    elif mode in ('media-deny', 'media-timeout'):
        wait(lambda: answer, 'answered')
        assert answer == ['NotAllowedError'], answer
        assert results[0]['state'] == 'denied', results
    elif mode == 'media-close':
        wait(lambda: state['closed'], 'closed')
        stale(kept[0])
        return
    elif mode == 'media-pending-close':
        view.close()
        wait(lambda: state['closed'], 'closed')
        stale(kept[0])
        assert not kept[0].respond(True)
        return
    elif mode == 'media-replace':
        view.on_media_access(None)
        stale(kept[0])
        wait(lambda: answer, 'answered')
        assert answer == ['NotAllowedError'], answer
    view.close()


def keys(mode):
    view, state = opened()
    told = []
    for invalid in (['Ctrl+Nonsense'], 'x', [1], ['Hyper+KeyA']):
        try:
            view.set_reserved_keys(invalid)
        except (zebkit.Error, TypeError):
            pass
        else:
            raise AssertionError('invalid keys accepted: %r' % (invalid,))
    view.set_reserved_keys(['Ctrl+KeyK'])
    view.on_reserved_key(guarded(told.append))
    view.load(BASE + '/input')
    wait(lambda: told, 'told')
    assert told[0]['combo'] == 'Ctrl+KeyK' and told[0]['code'] == 'KeyK' and not told[0]['autoRepeat'], told
    wait(lambda: ev(view, 'keys.length') >= 1, 'the other key')
    assert set(ev(view, 'keys')) == {'KeyB'}, ev(view, 'keys')
    view.close()


def menu(mode):
    view, state = opened()
    told = []
    view.on_context_menu(guarded(told.append))
    view.load(BASE + '/input')
    wait(lambda: told, 'told')
    assert told[0]['x'] == 20 and told[0]['y'] == 30 and told[0]['linkUrl'] == BASE + '/page', told
    assert ev(view, 'window.menu') == 1
    view.close()


# Once the evaluation's user activation has reached the browser (harness.py).
FULL = activated("document.documentElement.requestFullscreen()") + ".then(() => 'in', e => e.name)"


def fullscreen(mode):
    view, state = opened(BASE + '/page')
    told = []
    view.on_fullscreen(guarded(told.append))
    # Since Chromium 157 only a view with focus goes fullscreen.
    view.focus()
    try:
        view.set_fullscreen_policy('sometimes')
    except ValueError:
        pass
    else:
        raise AssertionError('invalid policy accepted')
    if mode == 'fullscreen-deny':
        view.set_fullscreen_policy('deny')
        assert ev(view, FULL) == 'TypeError'
        assert ev(view, 'document.fullscreenElement === null')
        pump(.2)
        assert not told, told
        view.set_fullscreen_policy('allow')
    assert ev(view, FULL) == 'in'
    wait(lambda: told and told[-1]['entered'], 'entered')
    if mode == 'fullscreen-deny':
        # Denying takes a page out of it.
        view.set_fullscreen_policy('deny')
    else:
        ev(view, 'document.exitFullscreen().then(() => 1)')
    wait(lambda: not told[-1]['entered'], 'left')
    view.close()


FETCH = "fetch('%s').then(async r => JSON.stringify({status: r.status, url: r.url, redirected: r.redirected, added: r.headers.get('x-added'), body: await r.text()}), e => 'failed')"


def fetched(view, url, method=None):
    box = []
    script = FETCH % url
    if method:
        script = script.replace("')", "', {method: 'POST', body: 'posted'})", 1)
    view.evaluate(script, lambda value, error=None: box.append(value))
    return box


WS_SCRIPT = "new Promise(ok => { const s = window.s = new WebSocket(location.origin.replace('http', 'ws') + '/ws'); s.onmessage = e => { ok(e.data); s.close(); }; s.onerror = () => ok('failed'); })"
SW_SCRIPT = "navigator.serviceWorker.register('/net-sw.js').then(r => new Promise(ok => { const w = r.installing || r.waiting || r.active; if (w.state === 'activated') return ok(w); w.addEventListener('statechange', () => { if (w.state === 'activated') ok(w); }); })).then(w => new Promise(ok => { navigator.serviceWorker.onmessage = e => ok(e.data); w.postMessage('%s'); }))"
SHARED_SCRIPT = "new Promise(ok => { const w = window.w = window.w || new SharedWorker('/net-shared.js'); w.port.onmessage = e => ok(e.data); w.port.postMessage('%s'); })"
PAGE_READS = "fetch('%s').then(r => r.arrayBuffer()).then(b => b.byteLength, () => 'failed')"
TEXT_HEADERS = [{'name': 'Content-Type', 'value': 'text/plain'}, {'name': 'X-Added', 'value': 'by-host'}]


def later(view, script):
    box = []
    view.evaluate(script, lambda value, error=None: box.append(value))
    return box


def network_ws(mode):
    view, state = opened(BASE + '/page')
    kept, events = [], []
    view.on_network_event(guarded(events.append))
    rules = {'net-ws-none': [], 'net-ws-filter': [{'url': '**/ws', 'resourceTypes': ['fetch']}],
             'net-ws-continue': [{'url': '**/ws', 'resourceTypes': ['websocket']}]}.get(mode, [{'url': '**/ws'}])
    view.set_network_filter(rules)
    def asked(request):
        kept.append(request)
        metadata = request.metadata
        if mode == 'net-ws-block':
            assert metadata['resourceType'] == 'websocket' and metadata['url'].startswith('ws://') and metadata['stage'] == 'request', metadata
            assert not request.respond('redirect', url='http://127.0.0.1/') and not request.respond('fulfill')
            assert request.respond_stream() is None and not request.respond_file('/nothing')
            assert not request.respond('continue', headers=[{'name': 'Origin', 'value': 'http://x'}])
            assert request.respond('block')
        elif mode == 'net-ws-continue':
            assert request.respond('continue', headers=[{'name': 'X-Zebkit', 'value': 'yes'}])
        elif mode == 'net-ws-close':
            view.close()
    if mode != 'net-ws-nofunc':
        view.on_network(guarded(asked))
    if mode == 'net-ws-timeout':
        view.set_network_timeout(50)
    box = later(view, WS_SCRIPT)
    if mode == 'net-ws-close':
        wait(lambda: state['closed'], 'closed')
        stale(kept[0])
        return
    if mode == 'net-ws-abort':
        wait(lambda: kept, 'asked')
        ev(view, 's.close()')
        wait(lambda: kept[0].info is None, 'the page closed it')
        wait(lambda: box, 'the socket ended')
        view.close()
        return
    if mode == 'net-ws-replace':
        wait(lambda: kept, 'asked')
        view.on_network(None)
        stale(kept[0])
        wait(lambda: any(e.get('reason') == 'handler-changed' for e in events), 'replaced told')
    wait(lambda: box, 'the socket answered')
    if mode == 'net-ws-block':
        assert box == ['failed'] and len(kept) == 1, box
    else:
        assert box == ['hello'], box
    if mode == 'net-ws-timeout':
        wait(lambda: any(e.get('reason') == 'timeout' for e in events), 'timeout told')
    if mode in ('net-ws-none', 'net-ws-nofunc', 'net-ws-filter'):
        assert not kept, kept
    view.close()


def network_workers(mode):
    view, state = opened(BASE + '/page')
    kept = []
    assert not view.set_network_filter_for_workers(True), 'no filter to decide'
    view.set_network_filter([{'url': '**/ws'}] if mode == 'net-sw-ws' else [{'url': '**/worker-held'}])
    def asked(request):
        kept.append(request)
        metadata = request.metadata
        assert metadata['worker'] and metadata['frameId'] == '', metadata
        expected = {'net-sw-ws': 'shared-or-service', 'net-sw-block': 'service', 'net-sw-running': 'service'}.get(mode, 'shared')
        assert metadata['workerType'] == expected, metadata
        assert request.respond('block')
    view.on_network(guarded(asked))
    if mode in ('net-sw-block', 'net-sw-ws', 'net-shared-block'):
        assert view.set_network_filter_for_workers(True)
    script = (SHARED_SCRIPT if mode.startswith('net-shared') else SW_SCRIPT) % ('socket' if mode == 'net-sw-ws' else 'fetch')
    if mode in ('net-sw-running', 'net-shared-running'):
        assert ev(view, script) == 'through'
        assert not kept
        assert view.set_network_filter_for_workers(True)
    if mode in ('net-sw-none', 'net-shared-none'):
        assert ev(view, script) == 'through' and not kept
    else:
        assert ev(view, script) == 'failed' and len(kept) == 1, kept
    assert view.set_network_filter_for_workers(False)
    view.close()


def workers_latch(mode):
    # A policy outlives the view that held it: refused until a view takes over or says off.
    holder, holder_state = opened(BASE + '/page')
    view, view_state = opened(BASE + '/page')
    other, _ = opened()
    both = (SW_SCRIPT % 'fetch', SHARED_SCRIPT % 'fetch')
    assert holder.set_network_filter_for_workers(False), 'nothing to say off to'
    holder.set_network_filter([])
    assert holder.set_network_filter_for_workers(True)
    for script in both:
        assert ev(view, script) == 'through', script
    view.set_network_filter([])
    assert not view.set_network_filter_for_workers(False), "another view's, which is still there"
    holder.close()
    wait(lambda: holder_state['closed'], 'holder closed')
    for script in both:
        assert ev(view, script) == 'failed', script
    assert view.set_network_filter_for_workers(True)
    for script in both:
        assert ev(view, script) == 'through', script
    view.close()
    wait(lambda: view_state['closed'], 'second holder closed')
    assert other.set_network_filter_for_workers(False), 'off, with nobody holding it'
    page, _ = opened(BASE + '/page')
    for script in both:
        assert ev(page, script) == 'through', script
    page.close()
    other.close()


def network_body(mode):
    view, state = opened(BASE + '/page')
    events, chunks, ends, second = [], [], [], []
    view.on_network_event(guarded(events.append))
    for invalid in ([{'body': True}], [{'stage': 'response', 'mode': 'hold', 'body': True}]):
        try:
            view.set_network_filter(invalid)
        except zebkit.Error:
            pass
        else:
            raise AssertionError('invalid filter accepted: %r' % (invalid,))
    view.set_network_filter([{'url': '**' + path, 'stage': 'response', 'body': True} for path in ('/download.bin', '/big', '/cut', '/slow-text')])
    late = []
    def told(request_id, data, end, truncated):
        assert not ends
        chunks.append((request_id, len(data)))
        if data and mode == 'net-body':
            assert data[0:1] == b'z'
        if end:
            ends.append(truncated)
            if any(e.get('kind') in ('finished', 'failed') for e in events):
                late.append(end)
        if data and mode == 'net-body-close':
            view.close()
        if data and mode == 'net-body-replace':
            view.on_network_body(guarded(lambda *args: second.append(args)))
    if mode != 'net-body-none':
        view.on_network_body(guarded(told))
    size = lambda: sum(n for _, n in chunks)
    if mode in ('net-body', 'net-body-none'):
        assert ev(view, PAGE_READS % '/download.bin') == 65536
        wait(lambda: any(e.get('kind') == 'finished' for e in events), 'finished')
        response = next(e for e in events if e.get('kind') == 'response')
        if mode == 'net-body':
            assert size() == 65536 and ends == [False] and not late, (size(), ends)
            assert chunks[0][0] == response['requestId'], (chunks[0], response)
        else:
            assert not chunks
    elif mode == 'net-body-big':
        assert ev(view, PAGE_READS % '/big') == 65 << 20
        assert size() == 64 << 20 and ends == [True], (size(), ends)
    elif mode == 'net-body-backpressure':
        assert ev(view, "fetch('/big').then(r => { window.r = r; return 1; })") == 1
        pump(1)
        assert not ends, size()  # Blink's fetch reads ahead, so no bound can be seen from here.
        assert ev(view, 'r.arrayBuffer().then(b => b.byteLength)') == 65 << 20
        assert size() == 64 << 20 and ends == [True], (size(), ends)
    elif mode == 'net-body-error':
        assert ev(view, PAGE_READS % '/cut') == 'failed'
        wait(lambda: any(e.get('kind') == 'failed' for e in events), 'failed told')
        assert size() == 1000 and ends == [False] and not late, (size(), ends)
    elif mode == 'net-body-abort':
        box = later(view, "(window.a = new AbortController(), fetch('/slow-text', {signal: a.signal}).then(r => r.arrayBuffer()).then(b => b.byteLength, () => 'failed'))")
        wait(lambda: size() > 0, 'first chunk')
        ev(view, 'a.abort()')
        wait(lambda: box, 'aborted')
        assert box == ['failed'], box
        wait(lambda: any(e.get('kind') == 'failed' for e in events), 'failed told')
        assert ends == [False] and not late, ends
    elif mode == 'net-body-close':
        later(view, PAGE_READS % '/slow-text')
        wait(lambda: state['closed'], 'closed')
        at = size()
        pump(.2)
        assert size() == at
        return
    elif mode == 'net-body-replace':
        assert ev(view, PAGE_READS % '/big') == 65 << 20
        assert not second and not ends, (len(second), ends)
        assert ev(view, PAGE_READS % '/download.bin') == 65536
        assert len(second) >= 2
    view.close()


def network_answers(mode):
    view, state = opened(BASE + '/page')
    kept, events, streams, blocked = [], [], [], []
    view.on_network_event(guarded(events.append))
    view.set_network_filter([{'url': '**/held'}])
    def asked(request):
        kept.append(request)
        if mode in ('net-file', 'net-file-close'):
            path = str(FILES / 'big.bin')
            assert not request.respond_file('relative.bin') and not request.respond_file(path, status=302)
            assert request.respond_file(path, status=201, headers=[{'name': 'Content-Type', 'value': 'application/x-zebkit'}, {'name': 'X-Added', 'value': 'by-host'}])
            if mode == 'net-file-close':
                view.close()
        elif mode == 'net-file-missing':
            assert request.respond_file(str(FILES / 'nothing.bin'))
        else:
            assert request.respond_stream(status=302) is None
            stream = request.respond_stream(status=201, headers=TEXT_HEADERS)
            assert stream and stream.is_open and request.info is None
            streams.append(stream)
            if mode == 'net-stream':
                assert stream.write(b'ab') and stream.write(b'cd')
                stream.end()
                assert not stream.is_open and not stream.write(b'x')
                stream.end()
            elif mode == 'net-stream-fail':
                assert stream.write(b'ab')
                stream.end(failed=True)
            elif mode == 'net-stream-full':
                assert not stream.write(bytes((1 << 20) + 1))
                # Blocks of 1 MiB, the most one write takes, 16 MiB in all: with
                # smaller ones a fast reader drains the engine's 1 MiB buffer
                # between writes, and none is refused (hook_policies.c).
                left = [16 << 20]
                def pour():
                    while left[0] > 0:
                        if not stream.write(b'f' * (1 << 20)):
                            blocked.append(1)
                            return
                        left[0] -= 1 << 20
                    stream.end()
                stream.on_writable(guarded(pour))
                pour()
            else:
                view.close()
    view.on_network(guarded(asked))
    if mode == 'net-file':
        assert ev(view, "fetch('/held').then(async r => [r.status, r.headers.get('x-added'), r.headers.get('content-type'), (await r.arrayBuffer()).byteLength].join('|'))") == '201|by-host|application/x-zebkit|3145728'
    elif mode == 'net-file-missing':
        assert ev(view, PAGE_READS % '/held') == 'failed'
        wait(lambda: any(e.get('kind') == 'failed' and 'FILE_NOT_FOUND' in e.get('error', '') for e in events), 'failed told')
    elif mode in ('net-file-close', 'net-stream-close'):
        later(view, PAGE_READS % '/held')
        wait(lambda: state['closed'], 'closed')
        if streams:
            wait(lambda: not streams[0].write(b'x'), 'nobody reads')
            assert not streams[0].is_open
            streams[0].end()
        return
    elif mode == 'net-stream':
        assert ev(view, "fetch('/held').then(async r => [r.status, r.headers.get('x-added'), await r.text()].join('|'))") == '201|by-host|abcd'
    elif mode == 'net-stream-full':
        assert ev(view, PAGE_READS % '/held') == 16 << 20
        assert blocked
    elif mode == 'net-stream-fail':
        assert ev(view, PAGE_READS % '/held') == 'failed'
        wait(lambda: any(e.get('kind') == 'failed' for e in events), 'failed told')
    view.close()


def network_redirect_modes(mode):
    view, state = opened(BASE + '/page')
    kept = []
    view.set_network_filter([{'url': '**/held'}])
    def asked(request):
        kept.append(request)
        assert request.respond('redirect', url=BASE + '/echo?redirected')
    view.on_network(guarded(asked))
    which = 'manual' if mode == 'net-redirect-manual' else 'error'
    answer = ev(view, "fetch('/held', {redirect: '%s'}).then(r => r.type, () => 'failed')" % which)
    assert answer == ('opaqueredirect' if which == 'manual' else 'failed') and len(kept) == 1, answer
    view.close()


def network(mode):
    if mode.startswith('net-ws-'):
        return network_ws(mode)
    if mode.startswith(('net-sw-', 'net-shared-')):
        return network_workers(mode)
    if mode.startswith('net-body'):
        return network_body(mode)
    if mode.startswith(('net-file', 'net-stream')):
        return network_answers(mode)
    if mode in ('net-redirect-manual', 'net-redirect-error'):
        return network_redirect_modes(mode)
    view, state = opened(BASE + '/page')
    kept, events = [], []
    view.on_network_event(guarded(events.append))
    rules = [{'url': '**/held', 'mode': 'hold'}]
    if mode in ('net-response', 'net-response-block'):
        rules = [{'url': '**/echo', 'stage': 'response'}]
    elif mode == 'net-observe':
        rules = [{'url': '**/echo', 'mode': 'observe'}, {'url': '**/echo', 'mode': 'observe', 'stage': 'response'}]
    elif mode == 'net-none':
        rules = []
    elif mode == 'net-filter':
        rules = [{'url': '**/echo', 'resourceTypes': ['image']}, {'url': '**/echo', 'frames': 'sub'}]
    elif mode == 'net-server-redirect':
        rules = [{'url': '**/echo'}]
    elif mode == 'net-worker':
        rules = [{'url': '**/echo?worker'}]
    elif mode in ('net-post', 'net-headers'):
        rules = [{'url': '**/echo'}]
    for invalid in ('nope', [{'mode': 'sometimes'}], [{'stage': 'x'}], [{'frames': 'x'}], [1]):
        try:
            view.set_network_filter(invalid)
        except zebkit.Error:
            pass
        else:
            raise AssertionError('invalid filter accepted: %r' % (invalid,))
    view.set_network_filter(rules)
    def asked(request):
        kept.append(request)
        metadata = request.metadata
        assert request.info == metadata, metadata
        if mode == 'net-block':
            assert metadata['stage'] == 'request' and metadata['resourceType'] == 'fetch' and metadata['frameId'], metadata
            assert not request.respond('nonsense') and not request.respond('continue', body=b'x')
            assert not request.respond('redirect', url='file:///etc/passwd')
            assert request.respond('block') and not request.respond('continue')
            stale(request)
        elif mode == 'net-headers':
            headers = metadata['headers'] + [{'name': 'X-Zebkit', 'value': 'yes'}]
            assert not request.respond('continue', headers=[{'name': 'Trailer', 'value': 'x'}])
            assert not request.respond('continue', headers=[{'name': 'bad name', 'value': 'x'}])
            assert request.respond('continue', headers=headers)
        elif mode == 'net-redirect':
            assert request.respond('redirect', url=BASE + '/echo?redirected')
        elif mode == 'net-fulfill':
            assert not request.respond('fulfill', status=302)
            assert request.respond('fulfill', body=bytes(range(256)) * 3, status=201,
                                   headers=[{'name': 'Content-Type', 'value': 'application/octet-stream'},
                                            {'name': 'X-Added', 'value': 'by-host'}])
        elif mode == 'net-navigation':
            assert metadata['navigation'] and metadata['resourceType'] == 'document' and metadata['mainFrame'], metadata
            assert request.respond('fulfill', body=b'<title>from-host</title>',
                                   headers=[{'name': 'Content-Type', 'value': 'text/html'}])
        elif mode == 'net-response':
            assert metadata['stage'] == 'response' and metadata['status'] == 200, metadata
            assert any(h['name'].lower() == 'content-type' for h in metadata['responseHeaders']), metadata
            assert request.respond('continue', headers=metadata['responseHeaders'] + [{'name': 'X-Added', 'value': 'late'}])
        elif mode == 'net-response-block':
            assert request.respond('block', error='failed')
        elif mode == 'net-close':
            view.close()
        elif mode == 'net-server-redirect':
            assert request.respond('block')
        elif mode == 'net-worker':
            assert metadata['worker'], metadata
            assert request.respond('fulfill', body=b'from-host', headers=[{'name': 'Content-Type', 'value': 'text/plain'}])
        elif mode == 'net-post':
            import base64
            assert metadata['method'] == 'POST' and metadata['hasBody'] and metadata['bodySize'] == 6, metadata
            assert base64.b64decode(metadata['body']) == b'posted', metadata
            assert request.respond('continue')
    view.on_network(guarded(asked))
    if mode == 'net-timeout':
        view.set_network_timeout(50)
    if mode == 'net-navigation':
        view.set_network_filter([{'url': '**/held', 'frames': 'main', 'resourceTypes': ['document']}])
        state['loads'].clear()
        view.load(BASE + '/held')
        wait(lambda: view.title == 'from-host', 'the application answered the navigation')
        view.close()
        return
    url = {'net-none': '/echo', 'net-observe': '/echo', 'net-response': '/echo', 'net-response-block': '/echo',
           'net-filter': '/echo', 'net-headers': '/echo', 'net-server-redirect': '/server-redirect', 'net-post': '/echo'}.get(mode, '/held')
    if mode == 'net-worker':
        box = []
        view.evaluate("new Promise(ok => { const w = new Worker('/net-worker'); w.onmessage = e => ok(e.data); })",
                      lambda value, error=None: box.append(value))
    else:
        box = fetched(view, url, 'POST' if mode == 'net-post' else None)
    if mode == 'net-close':
        wait(lambda: state['closed'], 'closed')
        stale(kept[0])
        return
    if mode == 'net-pending-close':
        wait(lambda: kept, 'asked')
        view.close()
        wait(lambda: state['closed'], 'closed')
        stale(kept[0])
        assert not kept[0].respond('continue')
        return
    if mode == 'net-replace':
        wait(lambda: kept, 'asked')
        view.on_network(None)
        stale(kept[0])
    wait(lambda: box, 'the fetch ended')
    if mode == 'net-worker':
        assert box == ['from-host'], box
        view.close()
        return
    answer = json.loads(box[0]) if box[0] != 'failed' else None
    if mode == 'net-none':
        assert answer and answer['status'] == 200 and not kept
        pump(.1)
        assert not events, events
    elif mode in ('net-block', 'net-response-block', 'net-server-redirect'):
        assert answer is None and len(kept) == 1, (box, kept)
        if mode == 'net-server-redirect':
            assert kept[0].metadata['url'].endswith('/echo'), kept[0].metadata
    elif mode == 'net-headers':
        assert answer['status'] == 200 and json.loads(answer['body'])['x-zebkit'] == 'yes', answer
    elif mode == 'net-redirect':
        assert answer['status'] == 200 and answer['redirected'] and answer['url'].endswith('/echo?redirected'), answer
    elif mode == 'net-fulfill':
        assert answer['status'] == 201 and answer['added'] == 'by-host', answer
        assert ev(view, "fetch('/held').then(r => r.arrayBuffer()).then(b => b.byteLength)") == 768
    elif mode == 'net-response':
        assert answer['added'] == 'late' and answer['status'] == 200, answer
    elif mode == 'net-observe':
        assert answer['status'] == 200 and not kept
        wait(lambda: {'request', 'response', 'finished'} <= {e.get('kind') for e in events}, 'told')
    elif mode == 'net-timeout':
        wait(lambda: any(e.get('reason') == 'timeout' for e in events), 'timeout told')
        stale(kept[0])
        assert not kept[0].respond('block')
    elif mode == 'net-replace':
        wait(lambda: any(e.get('reason') == 'handler-changed' for e in events), 'replaced told')
    elif mode == 'net-filter':
        assert answer['status'] == 200 and not kept, kept
    view.close()


def popup(mode):
    view, state = opened(BASE + '/page')
    children, progress, asked_by = [], [], []
    view.on_window_open(guarded(lambda child, url: children.append(child)))
    view.on_download(guarded(lambda request: (asked_by.append(request), request.respond('cancel'))))
    view.set_network_filter([{'url': '**/held'}])
    view.on_network(guarded(lambda request: (asked_by.append(request), request.respond('block'))))
    ev(view, "window.open('/page'); 1")
    wait(lambda: children, 'popup')
    child = children[0]
    child_state = {'closed': False}
    child.on_close(lambda: child_state.update(closed=True))
    child.on_download_progress(guarded(progress.append))
    # The URL is there before the document: wait for that.
    wait(lambda: ev(child, 'document.title') == 'page', 'popup loaded')
    if mode == 'popup-inherit':
        # The opener's functions decide for the popup.
        ev(child, 'location.href = "/download.bin"; 1')
        wait(lambda: asked_by, 'the opener was asked about the popup')
        assert asked_by[0].metadata['suggestedName'] == 'report.bin'
        wait(lambda: any(e.get('state') == 'cancelled' and e.get('done') for e in progress), 'cancelled')
        del asked_by[:]
        assert ev(child, "fetch('/held').then(() => 'passed', () => 'failed')") == 'failed'
        assert asked_by, 'the opener was asked about the popup network'
    elif mode == 'popup-own':
        # A function of its own replaces its opener's.
        own = []
        child.on_download(guarded(lambda request: (own.append(request), request.respond('cancel'))))
        ev(child, 'location.href = "/download.bin"; 1')
        wait(lambda: own, 'the popup was asked')
        assert not [r for r in asked_by if 'suggestedName' in r.metadata], asked_by
    else:
        # With its opener gone, a popup without functions of its own has no policy left.
        view.close()
        wait(lambda: state['closed'], 'opener closed')
        assert ev(child, "fetch('/held').then(() => 'passed', () => 'failed')") == 'failed'
        assert not asked_by
        # Its navigations are refused too; a link with download attribute is not one.
        ev(child, "const a = document.createElement('a'); a.href = '/download.bin'; a.download = ''; a.click(); 1")
        wait(lambda: any(e.get('state') == 'cancelled' and e.get('done') for e in progress), 'refused')
        assert ev(child, FULL) == 'TypeError'
        child.close()
        wait(lambda: child_state['closed'], 'closed')
        return
    child.close()
    view.close()


def cancel_running(mode):
    view, state = opened(BASE + '/page')
    progress, target = [], DOWNLOADS / 'running.bin'
    view.on_download(guarded(lambda request: request.respond('save', str(target))))
    view.on_download_progress(guarded(progress.append))
    ev(view, 'location.href = "/slow.bin"; 1')
    wait(lambda: any(e.get('state') == 'in-progress' and e.get('receivedBytes', 0) > 0 for e in progress), 'running')
    download_id = next(e['downloadId'] for e in progress if 'downloadId' in e and e.get('state') == 'in-progress')
    assert not view.cancel_download('d999999')
    assert view.cancel_download(download_id)
    wait(lambda: any(e.get('state') == 'cancelled' and e.get('done') for e in progress), 'cancelled')
    assert not view.cancel_download(download_id)
    assert not target.exists()
    view.close()


def cdp_order(mode):
    view, state = opened(BASE + '/input')
    view.set_network_filter([{'url': '**/held'}])
    def asked(request):
        assert request.respond('continue', headers=request.metadata['headers'] + [{'name': 'X-Hook', 'value': 'first'}])
    view.on_network(guarded(asked))
    wait(lambda: view.title == 'fetch-on', 'the CDP client has Fetch on')
    assert ev(view, "fetch('/held').then(r => r.text())") == 'from-cdp'
    view.close()


def two_views(mode):
    first, first_state = opened(BASE + '/page')
    second, second_state = opened(BASE + '/page')
    target = DOWNLOADS / 'first.bin'
    asked = []
    first.on_download(guarded(lambda request: (asked.append('first'), request.respond('save', str(target)))))
    second.on_download(guarded(lambda request: (asked.append('second'), request.respond('cancel'))))
    done = []
    first.on_download_progress(guarded(lambda e: done.append(('first', e.get('state')))))
    second.on_download_progress(guarded(lambda e: done.append(('second', e.get('state')))))
    first.set_network_filter([{'url': '**/held'}])
    first.on_network(guarded(lambda request: request.respond('block')))
    first.set_fullscreen_policy('deny')
    ev(first, 'location.href = "/download.bin"; 1')
    wait(lambda: ('first', 'complete') in done, 'first saved')
    ev(second, 'location.href = "/download.bin"; 1')
    wait(lambda: ('second', 'cancelled') in done, 'second cancelled')
    assert asked == ['first', 'second'], asked
    assert target.stat().st_size == 65536
    assert ev(first, "fetch('/held').then(() => 'passed', () => 'failed')") == 'failed'
    assert ev(second, "fetch('/held').then(() => 'passed', () => 'failed')") == 'passed'
    second.focus()
    assert ev(first, FULL) == 'TypeError' and ev(second, FULL) == 'in'
    first.close()
    second.close()


def main():
    with tempfile.TemporaryDirectory(prefix='zebkit-hook-python-') as profile:
        assert zebkit.init(['hook-policies-python'] + sys.argv[1:]) == 0
        try:
            group = MODE.split('-')[0]
            {'popup': popup, 'workers': workers_latch, 'two': two_views, 'cancel': cancel_running, 'cdp': cdp_order, 'net': network, 'external': external, 'download': download, 'cert': certificate, 'world': world, 'file': chooser,
             'media': media, 'keys': keys, 'menu': menu, 'fullscreen': fullscreen}[group](MODE)
            assert not errors, errors
            pump(.1)
            assert not errors, errors
        finally:
            zebkit.shutdown()
    print('ok python ' + MODE, flush=True)


main()
