#!/usr/bin/env python3
"""What the GPU process says it draws with, over CDP (SystemInfo.getInfo, what chrome://gpu
shows), and a page's WebGL renderer. For a machine with a GPU: on it, software is a failure.

  gpu_status.py <zebkit-run> <engine switches...>
"""
import re
import subprocess
import sys
import time

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from cdp_page import Cdp


def main():
    program, switches = sys.argv[1], sys.argv[2:]
    engine = subprocess.Popen([program, *switches, '--remote-debugging-port=0', 'about:blank'],
                              stderr=subprocess.PIPE, text=True)
    failed = 0
    try:
        url = None
        for line in engine.stderr:
            found = re.search(r'ws://\S+', line)
            if found:
                url = found.group(0)
                break
        assert url, 'no "DevTools listening" line'
        client = Cdp(url)
        info = {}
        status = {}
        # The GPU process's facts arrive once it has started and something has drawn.
        target = client.call('Target.createTarget', {'url': 'data:text/html,<body style="background:green">'})['targetId']
        until = time.monotonic() + 15
        while time.monotonic() < until and not str(status.get('gpu_compositing', '')).startswith('enabled'):
            info = client.call('SystemInfo.getInfo')['gpu']
            status = info.get('featureStatus', {})
            time.sleep(.25)
        devices = [(d.get('vendorString'), d.get('deviceString')) for d in info.get('devices', [])]
        print('     devices:', devices, flush=True)
        print('     driver bug workarounds:', len(info.get('driverBugWorkarounds', [])), flush=True)
        for feature in ('gpu_compositing', 'webgl', 'rasterization', '2d_canvas', 'video_decode', 'webgpu'):
            print(f'     {feature}: {status.get(feature)}', flush=True)
        for feature in ('gpu_compositing', 'webgl', 'rasterization'):
            good = str(status.get(feature, '')).startswith('enabled') and 'software' not in str(status.get(feature))
            print(('ok  ' if good else 'FAIL'), feature, 'on the GPU:', status.get(feature), flush=True)
            failed += not good
        session = client.call('Target.attachToTarget', {'targetId': target, 'flatten': True})['sessionId']
        renderer = client.call('Runtime.evaluate', {'returnByValue': True, 'expression': """
            (() => { const g = document.createElement('canvas').getContext('webgl');
                     if (!g) return 'no WebGL';
                     const d = g.getExtension('WEBGL_debug_renderer_info');
                     return d ? g.getParameter(d.UNMASKED_RENDERER_WEBGL) : 'no renderer info'; })()"""},
                               session=session)['result']['value']
        good = 'SwiftShader' not in renderer and 'Software' not in renderer and renderer != 'no WebGL'
        print(('ok  ' if good else 'FAIL'), 'WebGL renderer:', renderer, flush=True)
        failed += not good
    finally:
        engine.terminate()
        engine.wait(15)
    return 1 if failed else 0


sys.exit(main())
