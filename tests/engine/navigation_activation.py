"""A cached document must pass policy again before becoming the active page:
the navigation hook's activation phase for documents from the back/forward
cache and for prerendered ones, each answered, timed out, replaced, closed and
superseded. The suite's variants (ZEBKIT_TEST_PRERENDER_*) give the prerendered
page's view a user agent, client hints, an evaluation, init scripts and a
removed one first, which the activated document must have as the view's.

    [ZEBKIT_TEST_PRERENDER_...=1] python3 tests/engine/navigation_activation.py

Exit 0 when every check passes.
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import sys
import tempfile
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import check, ev, finish, must_wait, part, pump, zebkit  # noqa: E402
from zebkit import automation  # noqa: E402

received = []
user_agents = {}
activate = set()
class Server(BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def do_GET(self):
        received.append(self.path)
        user_agents[self.path] = self.headers.get('User-Agent')
        source = ("<title>" + self.path + "</title><script>window.restored=false;"
                "window.wasPrerendered=document.prerendering;"
                "window.initializationSeen=globalThis.hostInitialization||null;"
                "window.userAgentSeen=navigator.userAgent;"
                "window.userAgentMetadataSeen=navigator.userAgentData?.toJSON();"
                "if(document.prerendering)fetch('/ready'+location.pathname);"
                "addEventListener('pageshow',e=>restored=e.persisted);</script>")
        if self.path.startswith('/initiator-'):
            target = self.path.replace('/initiator-', '/prerender-', 1)
            source += '<script type="speculationrules">'+json.dumps(
                {'prerender':[{'source':'list','urls':[target]}]})+'</script>'
            action = self.path.removeprefix('/initiator-')
            source += ("<script>let activationSent=false;const activationTimer=setInterval(async()=>{if(activationSent)return;if(await(await fetch("+
                       json.dumps('/activation-control-'+action)+
                       ",{cache:'no-store'})).text()==='1'&&!activationSent){activationSent=true;clearInterval(activationTimer);location.href="+
                       json.dumps(target)+"}},30)</script>")
        if self.path.startswith('/activation-control-'):
            source = '1' if self.path.removeprefix('/activation-control-') in activate else '0'
        if self.path == '/worker-native.js': source = 'postMessage(navigator.userAgent);'
        body = source.encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/javascript' if self.path=='/worker-native.js' else 'text/html')
        self.send_header('Cache-Control', 'max-age=3600')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        try: self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError): pass

def wait(condition, label):
    must_wait(condition, label)

def evaluate(view, script):
    """ev(), raising when the script threw or did not answer."""
    value = ev(view, script)
    assert not (isinstance(value, tuple) and value[:1] in (('error',), ('timeout',))), (script, value)
    return value

def automate(view, method, params):
    # A native automation command by its protocol name: Page.addInitScript with
    # a registration of the caller's and Page.removeRegistration are no method of
    # zebkit.automation.Page (automation.call, a Future).
    answer = automation.call(view, method, params)
    wait(answer.done, method)
    return answer.result()

def bfcache(base):
    with part('BFCache restoration passes navigation activation policy'):
        view = zebkit.View(None,320,200)
        events = []
        def policy(request):
            events.append(request.metadata)
            assert request.respond()
        view.on_navigation(policy)
        for path in ('/first','/second'):
            view.load(base+path)
            wait(lambda: view.title == path, path)
        events.clear()
        evaluate(view,'history.back(); true')
        wait(lambda: view.title == '/first', 'cached document restored')
        assert evaluate(view,'restored') is True, 'test did not restore from BFCache'
        assert received.count('/first') == 1, received
        assert any(e['phase']=='activation' and e.get('activationType')=='back-forward-cache'
                   for e in events), events
        view.close()


def bfcache_activation(base, action):
    with part('BFCache activation '+action):
            view = zebkit.View(None,320,200)
            pending, outcomes = [], []
            def decide(request):
                if request.metadata['phase'] == 'activation':
                    pending.append(request)
                else:
                    assert request.respond()
            view.on_navigation(decide)
            view.on_navigation_result(lambda event: outcomes.append(event))
            for path in ('/first-'+action,'/second-'+action):
                view.load(base+path)
                wait(lambda: view.title == path, path)
            view.set_navigation_timeout(200 if action == 'timeout' else 5000)
            evaluate(view,'history.back(); true')
            wait(lambda: bool(pending), 'activation '+action)
            request = pending[0]
            assert request.info is not None
            assert view.url == base+'/second-'+action, 'activation committed before decision'
            assert view.title == '/second-'+action
            if action in ('continue','cancel','redirect'):
                assert request.respond(action, base+'/replacement' if action=='redirect' else None)
            elif action == 'replace':
                view.on_navigation(None)
            elif action == 'close':
                view.close()
            elif action == 'supersede':
                view.load(base+'/superseding')
            if action in ('continue','timeout','replace'):
                wait(lambda: view.title == '/first-'+action, action+' restored')
                assert evaluate(view,'restored') is True
            elif action == 'cancel':
                wait(lambda: any(e['id']==request.metadata['id'] for e in outcomes), 'cancel result')
                assert view.url == base+'/second-'+action
                assert evaluate(view,'location.pathname') == '/second-'+action
            elif action == 'redirect':
                wait(lambda: view.title == '/replacement', 'replacement navigation')
            elif action == 'supersede':
                wait(lambda: view.title == '/superseding', 'superseding navigation')
            wait(lambda: request.info is None, 'settled activation token')
            assert not request.respond(), 'settled activation token accepted another reply'
            if action != 'close': view.close()


def prerender_activation(base, action):
    with part('prerender activation '+action):
            view = zebkit.View(None,320,200)
            pending = []
            if os.environ.get('ZEBKIT_TEST_PRERENDER_EVALUATE'):
                assert evaluate(view,'1+1') == 2
            if os.environ.get('ZEBKIT_TEST_PRERENDER_INIT_SCRIPT'):
                view.add_script('globalThis.hostInitialization=(globalThis.hostInitialization||0)+1;')
            if os.environ.get('ZEBKIT_TEST_PRERENDER_USER_AGENT'):
                view.set_user_agent('zebkit-prerender-native/1')
                if os.environ.get('ZEBKIT_TEST_PRERENDER_INVALID_USER_AGENT'):
                    # A rejected native override must not poison replay into
                    # a future prerender target.
                    view.set_user_agent('invalid\r\nuser-agent')
                if os.environ.get('ZEBKIT_TEST_PRERENDER_USER_AGENT_METADATA'):
                    def metadata_done(value,error):
                        assert error is None,(value,error)
                    view.set_user_agent_override({'userAgent':'zebkit-prerender-native/1',
                        'userAgentMetadata':{'brands':[{'brand':'NativePrerender','version':'1'}],
                          'platform':'Linux','platformVersion':'1','architecture':'x86',
                          'model':'','mobile':False}}, metadata_done)
            if os.environ.get('ZEBKIT_TEST_PRERENDER_DISPOSE_SCRIPT'):
                registration = 'removed-before-prerender-'+action
                automate(view,'Page.addInitScript',{'registration':registration,
                         'source':'globalThis.removedInitialization=true;'})
                # Keep a later script alive so removing the earlier script
                # changes the renderer-local identifiers on the next target.
                automate(view,'Page.addInitScript',{'registration':'retained-'+action,
                         'source':'globalThis.retainedInitialization=true;'})
                automate(view,'Page.removeRegistration',{'registration':registration})
            def decide_prerender(request):
                if request.metadata['phase']=='activation': pending.append(request)
                else: assert request.respond()
            view.on_navigation(decide_prerender)
            view.load(base+'/initiator-'+action)
            wait(lambda: view.title=='/initiator-'+action, 'prerender initiator')
            if os.environ.get('ZEBKIT_TEST_PRERENDER_INIT_SCRIPT'):
                initial_state = evaluate(view,'({seen:initializationSeen,current:globalThis.hostInitialization||null})')
                assert initial_state == {'seen':1,'current':1}, ('initiator',initial_state)
            # No legacy DevTools evaluation session before prerender starts:
            # Chromium disables prerender with clients lacking tab-target support.
            wait(lambda: '/ready/prerender-'+action in received, 'real prerender ready '+action)
            if os.environ.get('ZEBKIT_TEST_PRERENDER_USER_AGENT'):
                assert user_agents['/initiator-'+action]=='zebkit-prerender-native/1', user_agents
                assert user_agents['/prerender-'+action]=='zebkit-prerender-native/1', user_agents
            view.set_navigation_timeout(200 if action=='timeout' else 5000)
            activate.add(action)
            wait(lambda: bool(pending), 'prerender activation decision')
            request = pending[0]
            assert request.metadata['activationType']=='prerender', request.metadata
            assert request.metadata['referrerAvailable'] is False
            assert view.url==base+'/initiator-'+action, 'prerender committed before decision'
            if action in ('continue','cancel','redirect'):
                assert request.respond(action,base+'/replacement' if action=='redirect' else None)
            elif action=='replace': view.on_navigation(None)
            elif action=='close': view.close()
            elif action=='supersede': view.load(base+'/superseding')
            if action in ('continue','timeout','replace'):
                wait(lambda: view.title=='/prerender-'+action,'prerender activated')
                assert evaluate(view,'wasPrerendered') is True
                if os.environ.get('ZEBKIT_TEST_PRERENDER_USER_AGENT'):
                    assert evaluate(view,'navigator.userAgent')=='zebkit-prerender-native/1'
                    assert evaluate(view,'userAgentSeen')=='zebkit-prerender-native/1'
                    worker_agent = evaluate(view,"new Promise((resolve,reject)=>{const worker=new Worker('/worker-native.js');"
                        "worker.onmessage=e=>{worker.terminate();resolve(e.data)};"
                        "worker.onerror=e=>{worker.terminate();reject(e.message)}})")
                    assert worker_agent=='zebkit-prerender-native/1', worker_agent
                    assert user_agents['/worker-native.js']=='zebkit-prerender-native/1', user_agents
                    if os.environ.get('ZEBKIT_TEST_PRERENDER_USER_AGENT_METADATA'):
                        metadata = evaluate(view,'userAgentMetadataSeen')
                        assert metadata['brands']==[{'brand':'NativePrerender','version':'1'}],metadata
                        current = evaluate(view,"navigator.userAgentData.getHighEntropyValues(['platformVersion','architecture'])")
                        assert current['architecture']=='x86' and current['platformVersion']=='1',current
                if os.environ.get('ZEBKIT_TEST_PRERENDER_INIT_SCRIPT'):
                    initialization = evaluate(view,'({seen:initializationSeen,current:globalThis.hostInitialization||null})')
                    assert initialization == {'seen':1,'current':1}, initialization
                if os.environ.get('ZEBKIT_TEST_PRERENDER_DISPOSE_SCRIPT'):
                    assert evaluate(view,'typeof removedInitialization')=='undefined', 'disposed script replayed in prerender'
                    assert evaluate(view,'retainedInitialization') is True
                    automate(view,'Page.removeRegistration',{'registration':'retained-'+action})
                    view.load(base+'/post-dispose-'+action)
                    wait(lambda: view.title=='/post-dispose-'+action,'navigation after script removal')
                    assert evaluate(view,'typeof retainedInitialization')=='undefined', 'removal used an obsolete target-local identifier'
                    if action=='continue':
                        before = len(pending)
                        evaluate(view,'history.go(-2);true')
                        wait(lambda: len(pending)>before,'return to pre-activation BFCache document')
                        restored = pending[-1]
                        assert restored.metadata['activationType']=='back-forward-cache', restored.metadata
                        assert restored.respond()
                        wait(lambda: view.title=='/initiator-'+action,'initiator restored after prerender')
                        assert evaluate(view,'restored') is True
                        assert evaluate(view,'hostInitialization')==1, 'restored document initialization ran twice'
                        view.load(base+'/after-target-return')
                        wait(lambda: view.title=='/after-target-return','new document on restored target')
                        assert evaluate(view,'initializationSeen')==1, 'restored target lost future initialization'
                        assert evaluate(view,'typeof retainedInitialization')=='undefined', 'disposed script returned with cached target'
                        if os.environ.get('ZEBKIT_TEST_PRERENDER_USER_AGENT'):
                            assert evaluate(view,'userAgentSeen')=='zebkit-prerender-native/1'
            elif action=='redirect':
                wait(lambda: view.title=='/replacement','prerender redirected')
            elif action=='supersede':
                wait(lambda: view.title=='/superseding','prerender superseded')
            elif action=='cancel':
                pump()
                assert view.url==base+'/initiator-'+action
            wait(lambda: request.info is None,'settled prerender token')
            assert not request.respond()
            assert received.count('/prerender-'+action)==1, received
            if action!='close': view.close()


ACTIONS = ('continue','cancel','redirect','timeout','replace','close','supersede')


def main():
    server = ThreadingHTTPServer(('127.0.0.1',0),Server)
    thread = threading.Thread(target=server.serve_forever,daemon=True)
    thread.start()
    base = f'http://127.0.0.1:{server.server_port}'
    try:
        with tempfile.TemporaryDirectory(prefix='zebkit-navigation-activation-') as profile:
            if zebkit.init(['navigation-activation','--no-sandbox','--headless','--disable-gpu',
                            '--enable-features=BackForwardCache,Prerender2',
                            '--disable-features=Prerender2MemoryControls', f'--user-data-dir={profile}']) != 0:
                print('FAIL zebkit.init()')
                return 1
            try:
                bfcache(base)
                for action in ACTIONS:
                    bfcache_activation(base, action)
                for action in ACTIONS:
                    prerender_activation(base, action)
            finally:
                check(zebkit.shutdown(), 'zebkit.shutdown()')
    finally:
        server.shutdown();server.server_close();thread.join()
    return finish()


if __name__ == '__main__':
    sys.exit(main())
