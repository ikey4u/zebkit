#!/usr/bin/env python3
"""What the GPU process says it draws with, over CDP (SystemInfo.getInfo, what chrome://gpu
shows), and a page's WebGL renderer. For a machine with a GPU: on it, software is a failure.

  gpu_status.py <zebkit-run> <engine switches...>
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Cdp, check, failures, finish, launch_engine, note  # noqa: E402


def main():
    program, switches = sys.argv[1], sys.argv[2:]
    engine = launch_engine([program, *switches])
    try:
        if not check(engine.url is not None, 'the engine serves CDP'):
            return finish()
        client = Cdp(engine.url)
        info = {}
        status = {}
        # The GPU process's facts arrive once it has started and something has drawn.
        target = client.call('Target.createTarget', {'url': 'data:text/html,<body style="background:green">'})['targetId']
        until = time.monotonic() + 15
        while time.monotonic() < until and not str(status.get('gpu_compositing', '')).startswith('enabled'):
            info = client.call('SystemInfo.getInfo')['gpu']
            status = info.get('featureStatus', {})
            time.sleep(.05)
        devices = [(d.get('vendorString'), d.get('deviceString')) for d in info.get('devices', [])]
        note(f'devices: {devices}')
        note(f"driver bug workarounds: {len(info.get('driverBugWorkarounds', []))}")
        for feature in ('gpu_compositing', 'webgl', 'rasterization', '2d_canvas', 'video_decode', 'webgpu'):
            note(f'{feature}: {status.get(feature)}')
        for feature in ('gpu_compositing', 'webgl', 'rasterization'):
            good = str(status.get(feature, '')).startswith('enabled') and 'software' not in str(status.get(feature))
            check(good, f'{feature} on the GPU', status.get(feature))
        session = client.call('Target.attachToTarget', {'targetId': target, 'flatten': True})['sessionId']
        renderer = client.call('Runtime.evaluate', {'returnByValue': True, 'expression': """
            (() => { const g = document.createElement('canvas').getContext('webgl');
                     if (!g) return 'no WebGL';
                     const d = g.getExtension('WEBGL_debug_renderer_info');
                     return d ? g.getParameter(d.UNMASKED_RENDERER_WEBGL) : 'no renderer info'; })()"""},
                               session=session)['result']['value']
        good = 'SwiftShader' not in renderer and 'Software' not in renderer and renderer != 'no WebGL'
        check(good, 'WebGL renderer', renderer)
    except (RuntimeError, TimeoutError, EOFError, KeyError) as error:
        check(False, 'the CDP session', repr(error))
    finally:
        engine.stop(bool(failures))
    return finish()


sys.exit(main())
