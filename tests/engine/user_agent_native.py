"""Native UA and client hints commit together and survive rejected updates."""
import ctypes
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import tempfile
import threading
import time
import zebkit

received = []
class Server(BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def do_GET(self):
        received.append((self.path, dict(self.headers)))
        if self.path == '/headers':
            source = json.dumps(dict(self.headers)); kind = 'application/json'
        elif self.path == '/worker.js':
            source = 'navigator.userAgentData.getHighEntropyValues(["architecture","platformVersion","model","bitness","wow64","formFactors","fullVersionList","uaFullVersion"]).then(h=>postMessage({ua:navigator.userAgent,hints:h}))'
            kind = 'application/javascript'
        else:
            source = '<title>native-ua</title><script>window.firstUA={ua:navigator.userAgent,brands:navigator.userAgentData.brands,platform:navigator.userAgentData.platform}</script>'
            kind = 'text/html'
        body = source.encode()
        self.send_response(200)
        self.send_header('Content-Type',kind)
        self.send_header('Content-Length',str(len(body)))
        self.send_header('Cache-Control','no-store')
        self.send_header('Accept-CH','Sec-CH-UA-Full-Version-List, Sec-CH-UA-Platform-Version, Sec-CH-UA-Arch, Sec-CH-UA-Model, Sec-CH-UA-Bitness, Sec-CH-UA-WoW64, Sec-CH-UA-Form-Factors')
        self.end_headers()
        try: self.wfile.write(body)
        except (BrokenPipeError,ConnectionResetError): pass

def wait(predicate,label):
    end = time.monotonic()+15
    while time.monotonic()<end:
        assert zebkit.pump()
        if predicate(): return
        time.sleep(.002)
    raise AssertionError(label)

server = ThreadingHTTPServer(('127.0.0.1',0),Server)
thread = threading.Thread(target=lambda:server.serve_forever(poll_interval=.02),daemon=True)
thread.start()
base = f'http://127.0.0.1:{server.server_port}'
with tempfile.TemporaryDirectory(prefix='zebkit-native-ua-') as profile:
    assert zebkit.init(['native-ua','--no-sandbox','--headless','--disable-gpu',
                        '--no-proxy-server',f'--user-data-dir={profile}']) == 0
    try:
        setter = zebkit._declare('zebkit_view_set_user_agent_override',ctypes.c_bool,
                                zebkit._V,zebkit._S,zebkit._RESULT,zebkit._P)
        view = zebkit.View(None,320,200)
        callbacks = []
        def submit(options):
            answers = []
            def done(v,payload,error,data):
                answers.append((json.loads(payload) if payload else None,error.decode() if error else None))
            callback = zebkit._RESULT(done); callbacks.append(callback)
            source = options if isinstance(options,bytes) else json.dumps(options).encode()
            accepted = setter(view._check(),source,callback,None)
            return accepted, answers
        def evaluate(source):
            answers = []
            view.evaluate(source,lambda value,error:answers.append((value,error)))
            wait(lambda:bool(answers),'evaluate')
            assert answers[0][1] is None,answers
            return answers[0][0]
        valid = {'userAgent':'zebkit-native-hints/1','acceptLanguage':'en-US','platform':'NativePlatform',
                 'userAgentMetadata':{'brands':[{'brand':'Native','version':'1'}],
                   'fullVersionList':[{'brand':'Native','version':'1.0'}],
                   'platform':'Linux','platformVersion':'1','architecture':'x86','model':'NativeModel','mobile':False,
                   'fullVersion':'1.0','bitness':'64','wow64':False,'formFactors':['Desktop']}}
        accepted,answers = submit(valid)
        assert accepted
        # Navigation must wait for the asynchronous validation acknowledgement.
        view.load(base+'/first')
        wait(lambda:bool(answers) and view.title=='native-ua','atomic first load')
        assert answers==[({},None)],answers
        assert next(h for p,h in received if p=='/first')['User-Agent']==valid['userAgent'],received
        first = evaluate('firstUA')
        assert first['ua']==valid['userAgent'] and first['brands']==valid['userAgentMetadata']['brands'],first
        def state():
            value = evaluate("Promise.all([fetch('/headers').then(r=>r.json()),navigator.userAgentData.getHighEntropyValues(['platformVersion','architecture','model','bitness','wow64','formFactors','fullVersionList','uaFullVersion'])]).then(([headers,hints])=>({headers,hints,ua:navigator.userAgent,platform:navigator.platform}))")
            # Referer changes after each navigation; compare only UA inputs.
            value['headers'] = {key:content for key,content in value['headers'].items()
                                if key.lower() in ('user-agent','accept-language') or key.lower().startswith('sec-ch-ua')}
            return value
        before = state()
        assert before['ua']==before['headers']['User-Agent']==valid['userAgent'],before
        assert before['platform']=='NativePlatform' and before['hints']['architecture']=='x86',before
        assert before['hints']['platformVersion']=='1',before
        for key in ('model','bitness','wow64','formFactors','fullVersionList'):
            assert before['hints'][key]==valid['userAgentMetadata'][key],(key,before)
        assert before['hints']['uaFullVersion']=='1.0',before
        headers = {key.lower():value for key,value in before['headers'].items()}
        assert headers['sec-ch-ua-arch']=='"x86"' and headers['sec-ch-ua-model']=='"NativeModel"',headers
        assert headers['sec-ch-ua-bitness']=='"64"' and headers['sec-ch-ua-wow64']=='?0',headers
        worker = evaluate("new Promise((resolve,reject)=>{const w=new Worker('/worker.js');w.onmessage=e=>{w.terminate();resolve(e.data)};w.onerror=reject})")
        assert worker['ua']==valid['userAgent'],worker
        for key,value in worker['hints'].items():
            assert before['hints'][key]==value,(key,worker,before)
        for index,options in enumerate([
                {**valid,'userAgent':'bad\r\nagent'},
                {**valid,'acceptLanguage':'bad\nlanguage'},
                {**valid,'userAgent':'changed','userAgentMetadata':{**valid['userAgentMetadata'],'platform':'bad\nplatform'}},
                {**valid,'userAgent':'','userAgentMetadata':valid['userAgentMetadata']},
                {'userAgent':42}, {'userAgent':'changed','unknown':True}, b'{', b'x'*65537]):
            accepted,answers = submit(options)
            wait(lambda:bool(answers),'rejected update')
            assert answers[0][0] is None and answers[0][1],(index,accepted,answers)
            assert state()==before,(index,state(),before)
            path='/after-rejection-'+str(index)
            view.load(base+path)
            wait(lambda:any(p==path for p,_ in received),'navigation after rejection')
            wait(lambda:evaluate('location.pathname')==path,'document after rejection')
            assert next(h for p,h in received if p==path)['User-Agent']==valid['userAgent'],received
        print('ok native UA/client hints: first script, network, worker and atomic rejection',flush=True)
        view.close()
        wait(lambda:view._closed,'native test view close')
        public = zebkit.View(None,320,200)
        completions = []
        assert public.set_user_agent_override(valid,
            lambda value,error:completions.append((value,error)))
        wait(lambda:bool(completions),'Python completion')
        assert completions==[({},None)],completions
        cleared = []
        assert public.set_user_agent_override({'userAgent':''},
            lambda value,error:cleared.append((value,error)))
        wait(lambda:bool(cleared),'clear native override')
        assert cleared==[({},None)],cleared
        identity = []
        public.evaluate('navigator.userAgent',lambda value,error:identity.append((value,error)))
        wait(lambda:bool(identity),'default UA after reset')
        assert identity[0][1] is None and identity[0][0]!=valid['userAgent'],identity
        public.close()
        wait(lambda:public._closed,'public close')
        redirected = zebkit.View(None,320,200)
        errors = []
        def navigate_from_completion(value,error):
            if error: errors.append(error)
            else: redirected.load(base+'/from-completion')
        assert redirected.set_user_agent_override(valid,navigate_from_completion)
        redirected.load(base+'/superseded-deferred')
        wait(lambda:any(path=='/from-completion' for path,_ in received) or bool(errors),'completion navigation')
        assert not errors,errors
        assert not any(path=='/superseded-deferred' for path,_ in received),received
        redirected.close()
        closing = zebkit.View(None,320,200)
        closing_answers = []
        def close_from_completion(value,error):
            closing_answers.append((value,error))
            closing.close()
        assert closing.set_user_agent_override(valid,close_from_completion)
        wait(lambda:closing._closed,'close inside completion')
        assert closing_answers==[({},None)],closing_answers
        cancelled = zebkit.View(None,320,200)
        cancelled_answers = []
        assert cancelled.set_user_agent_override(valid,
            lambda value,error:cancelled_answers.append((value,error)))
        cancelled.close()
        wait(lambda:cancelled._closed,'close with pending UA completion')
        assert len(cancelled_answers)==1,cancelled_answers
        assert cancelled_answers[0]==({},None) or (
            cancelled_answers[0][0] is None and cancelled_answers[0][1]),cancelled_answers
        print('ok Python UA completion, superseding navigation and callback closure',flush=True)
    finally:
        zebkit.shutdown()
        server.shutdown();server.server_close();thread.join()
