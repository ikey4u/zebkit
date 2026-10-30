"""Real native HTTP challenges exercise tokens, retries and cancellation."""
import ctypes
import json
import sys
import tempfile
import threading
import time
import traceback
import zebkit
from http_auth_fixture import AuthFixture

P,V,S = zebkit._P,zebkit._V,zebkit._S
AUTH = ctypes.CFUNCTYPE(None,V,P,S,P)
RESULT = ctypes.CFUNCTYPE(None,V,S,P)
on_auth = zebkit._declare('zebkit_view_on_http_auth',None,V,AUTH,P)
on_result = zebkit._declare('zebkit_view_on_http_auth_result',None,V,RESULT,P)
set_timeout = zebkit._declare('zebkit_view_set_http_auth_timeout',ctypes.c_bool,V,ctypes.c_uint32)
request_info = zebkit._declare('zebkit_http_auth_request_info',P,P)
respond = zebkit._declare('zebkit_http_auth_respond',ctypes.c_bool,P,S,S)
callbacks,requests,outcomes,errors = [],[],[],[]

def wait(condition,label,timeout=15):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        assert not errors,errors
        assert zebkit.pump()
        if condition(): return
        time.sleep(.002)
    assert not errors,errors
    raise AssertionError(label)

def info(token):
    value=request_info(token)
    return json.loads(zebkit._string(value)) if value else None

def evaluate(view,script):
    answers=[]
    view.evaluate(script,lambda value,error:answers.append((value,error)))
    wait(lambda:bool(answers),script)
    assert answers[0][1] is None,answers
    return answers[0][0]

def watch(view,fixture,mode='allow'):
    state={'mode':mode}
    def challenge(handle,token,payload,data):
        try:
            metadata=json.loads(payload)
            requests.append((handle,token,metadata))
            assert metadata['realm']==fixture.realm and metadata['scheme']=='basic',metadata
            assert metadata['proxy']==fixture.proxy and metadata['port']==fixture.server.server_port,metadata
            assert fixture.username not in payload.decode() and fixture.password not in payload.decode()
            assert info(token)['id']==metadata['id']
            mode=state['mode']
            if mode=='hold': return
            if mode=='replace':
                on_auth(handle,AUTH(),None)
                assert not respond(token,fixture.username.encode(),fixture.password.encode())
                return
            if mode=='close':
                view.close()
                return
            username,password=fixture.username.encode(),fixture.password.encode()
            if mode=='retry' and metadata['firstAttempt']: username,password=b'wrong-user',b'wrong-password'
            if mode=='deny': username=password=None
            assert respond(token,username,password)
            assert info(token) is None and not respond(token,username,password)
        except BaseException:
            errors.append(traceback.format_exc())
    def result(handle,payload,data):
        try:
            metadata=json.loads(payload)
            assert fixture.username not in payload.decode() and fixture.password not in payload.decode()
            outcomes.append((handle,metadata))
        except BaseException:
            errors.append(traceback.format_exc())
    callback,result_callback=AUTH(challenge),RESULT(result)
    callbacks.extend((callback,result_callback))
    on_auth(view._check(),callback,None)
    on_result(view._check(),result_callback,None)
    return state,callback

def denied(view,url):
    events=[]
    view.on_load(lambda event,url:events.append(event))
    view.load(url)
    wait(lambda:view.title=='auth-denied' or zebkit.LOAD_FAILED in events,'authentication cancelled')

proxy_mode=len(sys.argv)>1 and sys.argv[1]=='proxy'
engine_args=sys.argv[2:] if proxy_mode else sys.argv[1:]
proxy=AuthFixture(proxy=True) if proxy_mode else None
with tempfile.TemporaryDirectory(prefix='zebkit-http-auth-') as profile:
    argv=['http-auth',*engine_args,'--no-sandbox','--headless','--disable-gpu',f'--user-data-dir={profile}']
    if proxy:
        argv += [f'--proxy-server={proxy.url}','--proxy-bypass-list=<-loopback>',
                 '--host-resolver-rules=MAP auth-proxy.invalid ~NOTFOUND']
    else: argv.append('--no-proxy-server')
    assert zebkit.init(argv)==0
    try:
        assert request_info(None) is None and not respond(None,None,None)
        if proxy:
            view=zebkit.View()
            watch(view,proxy)
            view.load('http://auth-proxy.invalid/auth/proxy')
            wait(lambda:view.title=='auth-ok','real proxy credentials accepted')
            assert any(r['accepted'] for r in proxy.received)
            assert any(m['proxy'] and m['mainFrame'] and m['navigation'] for _,_,m in requests)
            view.close()
            print('ok native HTTP proxy challenge: realm, challenger, UTF-8 credentials and single-use reply')
        else:
            with AuthFixture() as fixture:
                view=zebkit.View()
                denied(view,fixture.url+'/auth/default')
                assert not any(r['accepted'] for r in fixture.received)
                view.close()
            print('ok HTTP authentication without a handler remains cancelled')
            for mode in ('allow','deny','retry','replace','close'):
                with AuthFixture() as fixture:
                    view=zebkit.View()
                    assert not set_timeout(view._check(),0) and not set_timeout(view._check(),300001)
                    state,_=watch(view,fixture,mode)
                    before=len(requests)
                    if mode in ('deny','replace'):
                        denied(view,fixture.url+'/auth/'+mode)
                    else:
                        view.load(fixture.url+'/auth/'+mode)
                        if mode=='close': wait(lambda:view._closed,'close in auth callback')
                        else: wait(lambda:view.title=='auth-ok',mode+' authenticated')
                    relevant=requests[before:]
                    assert relevant
                    if mode=='retry': assert any(not m['firstAttempt'] for _,_,m in relevant)
                    if mode!='close':
                        if mode=='replace': wait(lambda:any(m['reason']=='handler-changed' for _,m in outcomes),'replacement outcome')
                        view.close()
                    assert all(info(token) is None for _,token,_ in relevant)
                    assert all(not respond(token,None,None) for _,token,_ in relevant)
                print('ok native HTTP authentication '+mode)
            for mode in ('invalid','timeout','stop','navigation','handler-change'):
                with AuthFixture() as fixture:
                    view=zebkit.View()
                    state,callback=watch(view,fixture,'hold')
                    if mode=='timeout': assert set_timeout(view._check(),50)
                    before=len(requests)
                    view.load(fixture.url+'/auth/'+mode)
                    wait(lambda:len(requests)>before,'pending auth '+mode)
                    token=requests[-1][1]
                    if mode=='invalid':
                        assert not respond(token,b'user',None) and not respond(token,b'\xff',b'password')
                        assert not respond(token,b'x'*65537,b'password') and info(token)
                        off_thread=[]
                        thread=threading.Thread(target=lambda:off_thread.append((info(token),respond(token,None,None))))
                        thread.start();thread.join()
                        assert off_thread==[(None,False)] and info(token)
                        on_auth(view._check(),callback,None)
                        assert info(token), 'identical callback reset cancelled its request'
                        assert respond(token,fixture.username.encode(),fixture.password.encode())
                        wait(lambda:view.title=='auth-ok','valid reply after invalid replies')
                    elif mode=='timeout':
                        time.sleep(.08)
                        assert not respond(token,fixture.username.encode(),fixture.password.encode())
                        wait(lambda:any(m['id']==str(token) and m['reason']=='timeout' for _,m in outcomes),'monotonic deadline')
                    elif mode=='stop':
                        stop=zebkit._declare('zebkit_view_stop',None,V)
                        stop(view._check())
                    elif mode=='navigation':
                        view.load('about:blank')
                    else: on_auth(view._check(),AUTH(),None)
                    wait(lambda:info(token) is None,'old auth token cancelled '+mode)
                    assert not respond(token,None,None)
                    view.close()
                print('ok HTTP authentication lifetime '+mode)
            with AuthFixture() as fixture:
                view=zebkit.View()
                watch(view,fixture)
                view.load(fixture.url+'/public')
                wait(lambda:view.title=='public','public page')
                assert 'auth-ok' in evaluate(view,"fetch('/auth/fetch').then(r=>r.text())")
                assert any(not m['navigation'] and not m['mainFrame'] for _,_,m in requests if m['port']==fixture.server.server_port)
                view.close()
            with AuthFixture() as fixture:
                view=zebkit.View()
                watch(view,fixture)
                view.load(fixture.url+'/public-frame')
                wait(lambda:any(r['path']=='/auth/frame' and r['accepted'] for r in fixture.received),'subframe auth')
                wait(lambda:evaluate(view,'childAuthenticated') is True,'authenticated frame message')
                assert any(m['navigation'] and not m['mainFrame'] for _,_,m in requests if m['port']==fixture.server.server_port)
                view.close()
            print('ok native HTTP auth covers fetch and iframe challenges without forged frame metadata')
        assert not errors,errors
    finally:
        zebkit.shutdown()
        if proxy: proxy.close()
