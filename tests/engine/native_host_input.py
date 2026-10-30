"""Optional X11 integration: toolkit window input, observed through CDP.

python3 tests/engine/native_host_input.py /path/to/gtk-host [engine switches...]
Uses a private Xvfb server; never sends input to the user's display.
"""
import ctypes as C
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

from cdp_page import Cdp


def main():
    x = C.CDLL('libX11.so.6')
    test = C.CDLL('libXtst.so.6')
    x.XOpenDisplay.argtypes = [C.c_char_p]
    x.XOpenDisplay.restype = C.c_void_p
    x.XDefaultRootWindow.argtypes = [C.c_void_p]
    x.XDefaultRootWindow.restype = C.c_ulong
    x.XQueryTree.argtypes = [C.c_void_p, C.c_ulong, C.POINTER(C.c_ulong), C.POINTER(C.c_ulong),
                            C.POINTER(C.POINTER(C.c_ulong)), C.POINTER(C.c_uint)]
    x.XFetchName.argtypes = [C.c_void_p, C.c_ulong, C.POINTER(C.c_char_p)]
    x.XFree.argtypes = [C.c_void_p]
    x.XTranslateCoordinates.argtypes = [C.c_void_p, C.c_ulong, C.c_ulong, C.c_int, C.c_int,
                                       C.POINTER(C.c_int), C.POINTER(C.c_int), C.POINTER(C.c_ulong)]
    x.XKeysymToKeycode.argtypes = [C.c_void_p, C.c_ulong]
    x.XKeysymToKeycode.restype = C.c_uint
    x.XSetInputFocus.argtypes = [C.c_void_p, C.c_ulong, C.c_int, C.c_ulong]
    x.XFlush.argtypes = [C.c_void_p]
    x.XCloseDisplay.argtypes = [C.c_void_p]
    test.XTestFakeMotionEvent.argtypes = [C.c_void_p, C.c_int, C.c_int, C.c_int, C.c_ulong]
    test.XTestFakeButtonEvent.argtypes = [C.c_void_p, C.c_uint, C.c_int, C.c_ulong]
    test.XTestFakeKeyEvent.argtypes = [C.c_void_p, C.c_uint, C.c_int, C.c_ulong]
    server = subprocess.Popen(['Xvfb', '-displayfd', '1', '-screen', '0', '3000x1800x24', '-nolisten', 'tcp'],
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    process = None
    display = None
    try:
        display_name = ':' + server.stdout.readline().strip()
        display = x.XOpenDisplay(display_name.encode())
        if not display:
            raise RuntimeError('Cannot open the private Xvfb display')
        with tempfile.TemporaryDirectory(prefix='zebkit-native-input-') as work:
            log_path = Path(work) / 'host.log'
            with log_path.open('w') as log:
                process = subprocess.Popen([sys.argv[1], *sys.argv[2:], '--print-bounds', '--remote-debugging-port=0',
                                            '--user-data-dir=' + work + '/profile'],
                                           env=dict(os.environ, DISPLAY=display_name, GDK_BACKEND="x11", QT_QPA_PLATFORM="xcb", WINIT_UNIX_BACKEND="x11"),
                                           stdout=log, stderr=subprocess.STDOUT)
                endpoint = bounds = None
                until = time.monotonic() + 15
                while time.monotonic() < until:
                    text = log_path.read_text()
                    endpoint = re.search(r'ws://\S+', text)
                    bounds = re.search(r'host-web-bounds: (\d+) (\d+) ([\d.]+)', text)
                    if endpoint and bounds:
                        break
                    if process.poll() is not None:
                        raise RuntimeError(text)
                    time.sleep(.05)
                if not endpoint or not bounds:
                    raise RuntimeError('Host did not initialize: ' + log_path.read_text())
                cdp = Cdp(endpoint[0])
                target = None
                until = time.monotonic() + 5
                while time.monotonic() < until and target is None:
                    targets = cdp.call('Target.getTargets')['targetInfos']
                    target = next((t['targetId'] for t in targets if t['type'] == 'page'), None)
                    if target is None:
                        time.sleep(.05)
                if target is None:
                    raise RuntimeError('No web-view target appeared')
                session = cdp.call('Target.attachToTarget', {'targetId': target, 'flatten': True})['sessionId']

                def evaluate(expression):
                    result = cdp.call('Runtime.evaluate', {'expression': expression, 'returnByValue': True}, session=session)
                    if 'exceptionDetails' in result:
                        raise RuntimeError(result['exceptionDetails'])
                    return result['result'].get('value')

                def wait(expression, expected):
                    end = time.monotonic() + 5
                    value = None
                    while time.monotonic() < end:
                        value = evaluate(expression)
                        if value == expected:
                            return
                        time.sleep(.05)
                    raise AssertionError(f'{expression}: expected {expected!r}, got {value!r}')

                wait('!!document.querySelector("input")', True)
                root = x.XDefaultRootWindow(display)
                ancestor = C.c_ulong()
                parent = C.c_ulong()
                children = C.POINTER(C.c_ulong)()
                count = C.c_uint()
                x.XQueryTree(display, root, C.byref(ancestor), C.byref(parent), C.byref(children), C.byref(count))
                window = None
                for child in children[:count.value]:
                    name = C.c_char_p()
                    if x.XFetchName(display, child, C.byref(name)) and name.value:
                        if name.value.startswith(b'zebkit'):
                            window = child
                        x.XFree(name)
                x.XFree(children)
                if window is None:
                    raise RuntimeError('No toolkit-owned X11 window')
                origin_x, origin_y, child = C.c_int(), C.c_int(), C.c_ulong()
                x.XTranslateCoordinates(display, window, root, 0, 0, C.byref(origin_x), C.byref(origin_y), C.byref(child))
                offset_x, offset_y, scale = map(float, bounds.groups())

                def click(selector):
                    point = evaluate(f"(()=>{{const r=document.querySelector({selector!r}).getBoundingClientRect();return [r.x+r.width/2,r.y+r.height/2]}})()")
                    px = origin_x.value + round((offset_x + point[0]) * scale)
                    py = origin_y.value + round((offset_y + point[1]) * scale)
                    test.XTestFakeMotionEvent(display, -1, px, py, 0)
                    test.XTestFakeButtonEvent(display, 1, 1, 0)
                    test.XTestFakeButtonEvent(display, 1, 0, 0)
                    x.XFlush(display)

                def key(symbol):
                    code = x.XKeysymToKeycode(display, symbol)
                    test.XTestFakeKeyEvent(display, code, 1, 0)
                    test.XTestFakeKeyEvent(display, code, 0, 0)
                    x.XFlush(display)

                x.XSetInputFocus(display, window, 2, 0)
                x.XFlush(display)
                click('input')
                wait('document.activeElement.tagName', 'INPUT')
                for character in 'zebkit':
                    key(ord(character))
                wait('document.querySelector("input").value', 'zebkit')
                key(0xff08)  # BackSpace
                wait('document.querySelector("input").value', 'zebki')
                click('button')
                wait('document.querySelector("button").textContent', 'Clicked')
                click('select')
                key(0xff54)  # Down
                key(0xff0d)  # Return
                wait('document.querySelector("select").selectedIndex', 1)
                print('ok native toolkit X11 input: focus, typing, backspace, button, select popup', flush=True)
                cdp.sock.close()
    finally:
        if process and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        if display:
            x.XCloseDisplay(display)
        server.terminate()
        server.wait()


if __name__ == '__main__':
    main()
