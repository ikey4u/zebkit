"""Real TLS errors exercise the document-scoped native certificate decision hook."""
import ctypes
import json
import sys
import threading
import time
import traceback
import zebkit

from tls_fixture import TLSFixture
fixture = TLSFixture()
server,base = fixture.server,fixture.url
P,V,S = zebkit._P,zebkit._V,zebkit._S
REQUEST = ctypes.CFUNCTYPE(None,V,P,S,P)
RESULT = ctypes.CFUNCTYPE(None,V,S,P)
on_request = zebkit._declare('zebkit_view_on_certificate',None,V,REQUEST,P)
on_result = zebkit._declare('zebkit_view_on_certificate_result',None,V,RESULT,P)
set_timeout = zebkit._declare('zebkit_view_set_certificate_timeout',ctypes.c_bool,V,ctypes.c_uint32)
request_info = zebkit._declare('zebkit_certificate_request_info',P,P)
respond = zebkit._declare('zebkit_certificate_respond',ctypes.c_bool,P,ctypes.c_bool)
stop = zebkit._declare('zebkit_view_stop',None,V)
requests, outcomes, errors, callbacks = [],[],[],[]
mode = 'deny'

def wait(predicate,label):
    deadline = time.monotonic()+20
    while not predicate() and time.monotonic()<deadline:
        assert not errors,errors
        assert zebkit.pump()
        time.sleep(.001)
    assert not errors,errors
    assert predicate(),label

def info(token):
    value=request_info(token)
    return json.loads(zebkit._string(value)) if value else None

def evaluate(view,script):
    answers=[]
    view.evaluate(script,lambda value,error:answers.append((value,error)))
    wait(lambda:bool(answers),script)
    assert answers[0][1] is None,answers
    return answers[0][0]

@REQUEST
def certificate(handle,token,payload,data):
    try:
        metadata=json.loads(payload)
        requests.append((handle,token,metadata))
        assert info(token)==metadata
        assert metadata['state']=='pending' and metadata['error']<0
        assert metadata['certificateChain'][0].startswith('-----BEGIN CERTIFICATE-----')
        assert metadata['subject']=='localhost' and metadata['issuer']=='localhost'
        assert metadata['validFromUnixMs']<metadata['validUntilUnixMs']
        assert not metadata['knownRoot'] and not metadata['chainTruncated']
        if mode=='allow':
            assert metadata['canIgnore'] and not metadata['strictEnforcement']
            assert respond(token,True) and not respond(token,False) and info(token) is None
        elif mode=='deny': assert respond(token,False)
        elif mode=='replace':
            on_request(handle,REQUEST(),None)
            assert not respond(token,True) and info(token) is None
        elif mode=='strict':
            assert metadata['strictEnforcement'] and not metadata['canIgnore']
            assert not respond(token,True) and info(token) is not None
            assert respond(token,False)
        elif mode=='close': zebkit._view_close(handle)
    except BaseException: errors.append(traceback.format_exc())

@RESULT
def result(handle,payload,data):
    try: outcomes.append((handle,json.loads(payload)))
    except BaseException: errors.append(traceback.format_exc())
callbacks.extend([certificate,result])

def watch(view):
    on_request(view._check(),certificate,None)
    on_result(view._check(),result,None)

def load_fail(view,url):
    events=[]
    view.on_load(lambda event,actual:events.append((event,actual)))
    view.load(url)
    wait(lambda:any(event==zebkit.LOAD_FAILED for event,_ in events),'TLS rejection')

assert zebkit.init([sys.argv[0],*sys.argv[1:],'--host-resolver-rules=MAP mail.google.com 127.0.0.1','--no-proxy-server'],
                   settings={'logging':{'destination':'none'}})==0
try:
    first,other=zebkit.View(),zebkit.View()
    load_fail(other,base+'/default-deny')
    assert not requests
    watch(first)
    assert not set_timeout(first._check(),0) and not set_timeout(first._check(),300001)
    assert set_timeout(first._check(),5000)
    assert info(None) is None and not respond(None,True) and not respond(P(-1),False)
    mode='allow'
    first.load(base+'/allowed')
    wait(lambda:first.title=='certificate-ok','allowed certificate loads the actual TLS page')
    wait(lambda:any(event['state']=='continued' for _,event in outcomes),'continued result')
    assert requests[-1][2]['url']==base+'/allowed'
    # A once-only exception must not grant another view of the same profile.
    before=len(requests)
    load_fail(other,base+'/other-still-denied')
    assert len(requests)==before
    assert '/other-still-denied' not in fixture.received, 'denied request reached the TLS server'
    watch(other);mode='deny'
    load_fail(other,"data:text/html,<form method='post' action='"+base+"/private-post'><input name='value' value='private-payload'></form><script>document.forms[0].submit()</script>")
    assert '/private-post' not in fixture.received, 'denied POST reached the TLS server'
    assert any(handle==other._handle and metadata['url']==base+'/private-post' for handle,_,metadata in requests), 'POST did not reach the certificate policy'
    mode='allow'
    cached_events=[]
    first.on_load(lambda event,url:cached_events.append((event,url)))
    first.load(base+'/cached-shared')
    wait(lambda:(zebkit.LOAD_FINISHED,base+'/cached-shared') in cached_events,'first view cached TLS page')
    assert first.title=='certificate-ok'
    mode='deny'
    load_fail(other,base+'/cached-shared')
    assert any(handle==other._handle and metadata['url']==base+'/cached-shared' for handle,_,metadata in requests), 'cached-origin load bypassed certificate policy'
    assert fixture.received.count('/cached-shared')==1, 'denied view sent a cached-origin request'
    watch(other);mode='deny'
    load_fail(other,base+'/explicit-deny')
    wait(lambda:any(handle==other._handle and event['reason']=='answered' for handle,event in outcomes),'denial result')
    mode='hold';before=len(requests)
    first.load(base+'/pending-reply')
    wait(lambda:len(requests)>before,'deferred request')
    token=requests[-1][1]
    off_thread=[]
    worker=threading.Thread(target=lambda:off_thread.append((info(token),respond(token,True))))
    worker.start();worker.join()
    assert off_thread==[(None,False)] and info(token) is not None
    assert respond(token,False)
    wait(lambda:any(event['id']==str(token) and event['state']=='denied' for _,event in outcomes),'deferred denial')
    assert set_timeout(first._check(),200)
    before=len(requests);first.load(base+'/expired')
    wait(lambda:len(requests)>before,'expiring request')
    token=requests[-1][1]
    time.sleep(.25)
    assert not respond(token,True) and info(token) is None
    wait(lambda:any(event['id']==str(token) and event['reason']=='timeout' for _,event in outcomes),'monotonic timeout')
    assert set_timeout(first._check(),5000)
    before=len(requests);first.load(base+'/superseded')
    wait(lambda:len(requests)>before,'request before navigation')
    token=requests[-1][1];first.load('about:blank')
    wait(lambda:info(token) is None,'navigation cancels token')
    assert not respond(token,True)
    before=len(requests);first.load(base+'/stopped')
    wait(lambda:len(requests)>before,'request before stop')
    token=requests[-1][1];stop(first._check())
    wait(lambda:info(token) is None,'stopped navigation cancels token')
    assert not respond(token,True)
    mode='replace'
    load_fail(first,base+'/replace')
    wait(lambda:any(event['reason']=='handler-changed' for _,event in outcomes),'handler replacement denies pending')
    watch(first);mode='strict'
    load_fail(first,f'https://mail.google.com:{server.server_port}/strict')
    assert requests[-1][2]['strictEnforcement']
    victim=zebkit.View();watch(victim);mode='close'
    before=len(requests);victim.load(base+'/close')
    wait(lambda:victim._closed,'close from certificate callback')
    assert len(requests)>before and not respond(requests[-1][1],True)
    public=zebkit.View()
    public.set_certificate_timeout(5000)
    for invalid in [0,300001,True,1.5]:
        try: public.set_certificate_timeout(invalid)
        except ValueError: pass
        else: raise AssertionError('invalid Python certificate deadline accepted')
    public_requests,public_results=[],[]
    def public_certificate(request):
        try:
            public_requests.append(request)
            assert request.info==request.metadata
            try: request.respond(1)
            except TypeError: pass
            else: raise AssertionError('non-boolean Python reply accepted')
            assert request.respond(False) and not request.respond(True) and request.info is None
            public.on_certificate(None)
        except BaseException: errors.append(traceback.format_exc())
    def public_result(metadata):
        public_results.append(metadata)
        public.on_certificate_result(None)
        public.close()
    public.on_certificate(public_certificate)
    public.on_certificate_result(public_result)
    public.load(base+'/public-python')
    wait(lambda:public._closed,'Python certificate result can remove itself and close')
    assert len(public_requests)==len(public_results)==1
    assert public_requests[0].info is None and public_results[0]['reason']=='answered'
    for path in ['/default-deny','/explicit-deny','/pending-reply','/expired','/superseded','/stopped','/replace','/strict','/close','/public-python']:
        assert path not in fixture.received, ('denied request reached server',path)
    first.close();other.close()
    print('ok native certificate policy: real TLS, once-only isolation, metadata, strict errors, deferred reply, deadline, navigation, stop, replacement and closure',flush=True)
finally:
    assert zebkit.shutdown()
    fixture.close()
assert not errors,errors
