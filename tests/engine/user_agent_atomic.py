"""Rejected CDP client hints must preserve both renderer and network overrides."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import sys
import threading
from cdp_page import Cdp, launch_engine

received = []
class Server(BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def do_GET(self):
        received.append((self.path, self.headers.get("User-Agent"), self.headers.get("Accept-Language")))
        body = json.dumps(dict(self.headers)).encode() if self.path == '/headers' else b'<title>UA atomicity</title>'
        self.send_response(200)
        self.send_header('Content-Type', 'application/json' if self.path == '/headers' else 'text/html')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers(); self.wfile.write(body)

server = ThreadingHTTPServer(('127.0.0.1', 0), Server)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
engine = launch_engine([sys.argv[1], *sys.argv[2:]])
passed = False
try:
    assert engine.url, 'the engine published no CDP endpoint in time'
    cdp = Cdp(engine.url)
    target = cdp.call('Target.createTarget', {'url':'about:blank'})['targetId']
    session = cdp.call('Target.attachToTarget', {'targetId':target,'flatten':True})['sessionId']
    cdp.call('Page.enable', session=session)
    cdp.call('Runtime.enable', session=session)
    origin = f'http://127.0.0.1:{server.server_port}'
    cdp.call('Page.navigate', {'url':origin}, session=session)
    assert cdp.event('Page.loadEventFired'), 'initial page did not load'
    def evaluate(expression):
        result = cdp.call('Runtime.evaluate', {'expression':expression,'returnByValue':True,'awaitPromise':True},session=session)
        assert not result.get('exceptionDetails'), result
        return result['result'].get('value')
    valid = {'userAgent':'Zebkit-UA-Before/1','acceptLanguage':'en-US', 'userAgentMetadata': {
        'brands':[{'brand':'Before','version':'1'}], 'fullVersionList':[{'brand':'Before','version':'1.0'}],
        'platform':'Linux','platformVersion':'1','architecture':'x86','model':'','mobile':False}}
    cdp.call('Emulation.setUserAgentOverride',valid,session=session)
    def state():
        return evaluate("Promise.all([fetch('/headers').then(r=>r.json()), navigator.userAgentData.getHighEntropyValues(['platformVersion'])]).then(([h,m])=>({headers:h,ua:navigator.userAgent,metadata:m}))")
    before = state()
    assert before['headers']['User-Agent'] == before['ua'] == valid['userAgent'], before
    invalid_metadata = [
        {'platform':'bad\nplatform'},
        {'brands':[{'brand':'bad\nbrand','version':'2'}]},
        {'fullVersionList':[{'brand':'After','version':'bad\x00version'}]},
        {'platformVersion':'bad\nversion'}, {'architecture':'bad\narchitecture'},
        {'model':'bad\nmodel'}, {'fullVersion':'bad\nversion'},
        {'bitness':'bad\nbitness'}, {'formFactors':['bad\nfactor']},
    ]
    for index, changes in enumerate(invalid_metadata + [{}]):
        invalid = {**valid,'userAgent':'Zebkit-UA-After/2' if changes else '', 'acceptLanguage':'fr-FR',
                   'userAgentMetadata':{**valid['userAgentMetadata'],**changes}}
        try: cdp.call('Emulation.setUserAgentOverride', invalid, session=session)
        except RuntimeError as error: print('expected rejection:', error, flush=True)
        else: raise AssertionError('invalid client hints accepted')
        after = state()
        assert after['ua'] == before['ua'] and after['metadata'] == before['metadata'], after
        path = f'/after-rejection-{index}'
        cdp.call('Page.navigate', {'url':origin + path}, session=session)
        assert cdp.event('Page.loadEventFired'), 'navigation after rejection did not load'
        request = next(item for item in received if item[0] == path)
        print('navigation after rejection:', request, flush=True)
        assert request[1:] == ('Zebkit-UA-Before/1', 'en-US'), 'rejected metadata changed a later navigation'
        navigated = state()
        assert navigated['ua'] == before['ua'] and navigated['metadata'] == before['metadata'], 'rejected metadata changed a later document'
        for header in ('User-Agent', 'Accept-Language', 'sec-ch-ua', 'sec-ch-ua-platform', 'sec-ch-ua-mobile'):
            assert navigated['headers'].get(header) == before['headers'].get(header), (header, navigated)
    print('ok CDP user-agent metadata rejection is atomic for network and renderer',flush=True)
    passed = True
finally:
    engine.stop(not passed)
    server.shutdown(); server.server_close(); thread.join()
