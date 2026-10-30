"""Chromium NQE estimates reach existing and newly created renderers."""
import json
import sys
import time
import zebkit

kind = sys.argv[1]
assert zebkit.init([sys.argv[0], *sys.argv[2:], '--force-effective-connection-type=' + kind],
                   settings={'logging': {'destination': 'none'}}) == 0

def read(view):
    result = []
    view.evaluate('({rtt:navigator.connection.rtt,downlink:navigator.connection.downlink,type:navigator.connection.effectiveType})',
                  lambda value, error: result.append((value, error)))
    until = time.monotonic() + 10
    while not result and time.monotonic() < until:
        assert zebkit.pump()
        time.sleep(.001)
    assert result and not result[0][1], result
    return result[0][0]

def page():
    view = zebkit.View()
    loaded = []
    view.on_load(lambda event, url: loaded.append(True) if event == zebkit.LOAD_FINISHED and url.startswith('data:') else None)
    view.load('data:text/html,<title>network quality</title>')
    until = time.monotonic() + 10
    while not loaded and time.monotonic() < until:
        assert zebkit.pump()
        time.sleep(.001)
    assert loaded, 'network quality document did not load'
    return view


try:
    first = page()
    until = time.monotonic() + 10
    expected = '3g' if kind == '3G' else 'slow-2g'
    while True:
        value = read(first)
        if value['type'] == expected or time.monotonic() >= until:
            break
        time.sleep(.01)
    assert value['type'] == expected, value
    for view in (first, page()):
        value = read(view)
        assert value['type'] == expected, value
        if kind == '3G':
            assert 300 <= value['rtt'] <= 600 and .25 <= value['downlink'] <= .55, value
        else:
            # Chromium's typical HTTP RTT is 3600 ms, capped to 3000 for web exposure.
            assert value['rtt'] == 3000 and .025 <= value['downlink'] <= .075, value
    print('ok Chromium NQE ' + kind + ': ' + json.dumps(value), flush=True)
finally:
    assert zebkit.shutdown()
