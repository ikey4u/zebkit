"""Public console callbacks may unregister or close their view during delivery."""
import sys
import time
import zebkit

def wait(predicate, label):
    deadline = time.monotonic() + 15
    while not predicate() and time.monotonic() < deadline:
        assert zebkit.pump()
        time.sleep(.001)
    assert predicate(), label

def evaluate(view, code):
    result = []
    view.evaluate(code, lambda value, error: result.append((value, error)))
    wait(lambda: bool(result), code)
    assert result[0][1] is None, result
    return result[0][0]

assert zebkit.init([sys.argv[0], *sys.argv[1:]], settings={'logging': {'destination': 'none'}}) == 0
try:
    view = zebkit.View()
    assert evaluate(view, '1+2') == 3
    for invalid in [False, 1, 'callback']:
        try: view.on_console(invalid)
        except TypeError: pass
        else: raise AssertionError('invalid observer accepted')
    events = []
    def event(metadata):
        events.append(metadata)
        view.on_console(None)
    view.on_console(event)
    evaluate(view, "console.warn('public console'); true")
    wait(lambda: bool(events), 'public console callback')
    assert events[0]['message'] == 'public console' and events[0]['level'] == 'warning'
    evaluate(view, "console.log('removed observer'); true")
    assert len(events) == 1
    view.on_console(lambda metadata: view.close())
    view.evaluate("console.log('close from console'); true")
    wait(lambda: view._closed, 'close from public console observer')
    try: view.on_console(None)
    except zebkit.Error: pass
    else: raise AssertionError('closed view accepted a console observer')
    print('ok Python console: metadata, validation, self-removal and closing during callback', flush=True)
finally:
    assert zebkit.shutdown()
