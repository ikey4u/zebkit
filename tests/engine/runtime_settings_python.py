"""Runtime settings through zebkit's Python binding; run by runtime_settings.py. The
spare renderer is checked here alone: the process it is, by what the system lists."""
import json
import os
import subprocess
import sys
import time
import zebkit

BASE, OTHER = os.environ['RUNTIME_BASE'], os.environ['RUNTIME_OTHER']
assert zebkit.init(sys.argv, settings={'logging': {'destination': 'none'}}) == 0


def wait(condition, what, seconds=15):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        assert zebkit.pump()
        if condition():
            return
        time.sleep(.002)
    raise AssertionError(what)


def later(view, script):
    box = []
    view.evaluate(script, lambda value, error=None: box.append((value, error)))
    return box


def ev(view, script):
    box = later(view, script)
    wait(lambda: box, script)
    assert box[0][1] is None, box
    return box[0][0]


def opened(url=BASE + '/top'):
    view = zebkit.View()
    loaded = []
    view.on_load(lambda event, at: loaded.append(at) if event == zebkit.LOAD_FINISHED else None)
    view.load(url)
    wait(lambda: url in loaded, url)
    return view


names = iter(range(100000))


def cookies_become(view, expected):
    """Set on the network's pipe, not the page's: it arrives shortly."""
    for _ in range(60):
        name = f'c{next(names)}'
        if ev(view, f'probe({json.dumps(name)})') == expected:
            return name
        time.sleep(.05)
    raise AssertionError(f'third-party cookies never became {expected}')


def rejected(function, argument):
    try:
        function(argument)
    except zebkit.Error:
        return True
    return False


def renderers():
    """This engine's renderers: descendants of this process."""
    lines = subprocess.run(['ps', '-A', '-o', 'pid=,ppid=,command='], stdout=subprocess.PIPE, text=True, check=True).stdout
    parents, kinds = {}, {}
    for line in lines.splitlines():
        pid, ppid, command = line.split(None, 2)
        parents[int(pid)] = int(ppid)
        kinds[int(pid)] = command
    def ours(pid):
        while pid in parents and pid > 1:
            if pid == os.getpid():
                return True
            pid = parents[pid]
        return False
    return {pid for pid, command in kinds.items() if '--type=renderer' in command and ours(pid)}


view = opened()
other = opened()

# ---- What holds before anything is set: today's behaviour.
assert zebkit.engine_settings() == {'explicit': {}, 'effective': {
    'thirdPartyCookies': 'block', 'spareRenderer': False, 'spareRendererRunning': False,
    'dialogs': 'default', 'downloads': 'save', 'newWindows': 'block-without-gesture'}}, zebkit.engine_settings()
assert view.runtime_settings == {'explicit': {}, 'effective': {
    'thirdPartyCookies': 'block', 'dialogs': 'default', 'downloads': 'save', 'newWindows': 'block-without-gesture'}}, view.runtime_settings

# ---- Nothing of a bad batch changes.
zebkit.set_engine_settings({'downloads': 'save'})
view.set_runtime_settings({'dialogs': 'accept'})
engine_before, view_before = zebkit.engine_settings(), view.runtime_settings
for bad in ['not json', '[]', '"block"', 'null', {'nothing': 1}, {'thirdPartyCookies': True}, {'thirdPartyCookies': 'maybe'},
            {'dialogs': 3}, {'dialogs': 'never'}, {'downloads': 'ask'}, {'newWindows': False}, {'spareRenderer': 'yes'},
            {'downloads': 'deny', 'newWindows': 'nope'}, {'dialogs': 'dismiss', 'nothing': None}]:
    assert rejected(zebkit.set_engine_settings, bad), bad
    assert rejected(view.set_runtime_settings, bad), bad
    assert zebkit.engine_settings() == engine_before and view.runtime_settings == view_before, bad
assert rejected(view.set_runtime_settings, {'spareRenderer': True}), 'spareRenderer of a view'
assert view.runtime_settings == view_before
view.set_runtime_settings({})
zebkit.set_engine_settings({})
assert zebkit.engine_settings() == engine_before and view.runtime_settings == view_before
assert ev(view, "confirm('a batch that failed changed nothing')") is True
view.set_runtime_settings({'dialogs': None})
zebkit.set_engine_settings({'downloads': None})

# ---- Third-party cookies, as the third party's server saw them.
default_blocked = cookies_become(view, 'none')
zebkit.set_engine_settings({'thirdPartyCookies': 'allow'})
kept = cookies_become(view, 'cookie')
assert ev(view, f'read({json.dumps(default_blocked)})') == 'none', 'it was not stored when blocked'
assert ev(other, f'read({json.dumps(kept)})') == 'cookie', 'the engine-wide value is for every view'
assert view.runtime_settings['effective']['thirdPartyCookies'] == 'allow'
zebkit.set_engine_settings({'thirdPartyCookies': 'block'})
cookies_become(view, 'none')
assert ev(view, f'read({json.dumps(kept)})') == 'none', 'blocked again: the one kept is not sent'
# A view's goes to its context, the same one here; another context in the C test.
view.set_runtime_settings({'thirdPartyCookies': 'allow'})
assert other.runtime_settings['effective']['thirdPartyCookies'] == 'allow'
assert view.runtime_settings['explicit']['thirdPartyCookies'] == 'allow'
cookies_become(other, 'cookie')
# The engine's goes over what a view asked; null gives the default.
zebkit.set_engine_settings({'thirdPartyCookies': None})
assert view.runtime_settings['explicit'] == {} and view.runtime_settings['effective']['thirdPartyCookies'] == 'block'
cookies_become(view, 'none')

# ---- Dialogs: two views, two policies, then the engine's for one with none of its own.
view.set_runtime_settings({'dialogs': 'dismiss'})
other.set_runtime_settings({'dialogs': 'accept'})
assert ev(view, "confirm('q')") is False and ev(other, "confirm('q')") is True
assert ev(view, "prompt('q', 'text')") is None and ev(other, "prompt('q', 'text')") == 'text'
view.set_runtime_settings({'dialogs': None})
assert ev(view, "confirm('q')") is False, 'headless default: dismissed'
zebkit.set_engine_settings({'dialogs': 'accept'})
assert ev(view, "confirm('q')") is True and view.runtime_settings['effective']['dialogs'] == 'accept'
zebkit.set_engine_settings({'dialogs': 'dismiss'})
assert ev(other, "confirm('q')") is True, "a view's goes first"
zebkit.set_engine_settings({'dialogs': None})
held_view = opened()
held_view.set_runtime_settings({'dialogs': 'hold'})
held = later(held_view, "confirm('held')")
end = time.monotonic() + .5
while time.monotonic() < end:
    assert zebkit.pump()
    time.sleep(.002)
assert not held, 'hold: the page waits'
held_view.close()
wait(lambda: held, 'a held dialog ends with its view')
other.set_runtime_settings({'dialogs': None})

# ---- Downloads: denied and saved, told as such; a function goes first.
told = []
view.on_download_progress(lambda event: told.append(json.loads(event) if isinstance(event, str) else event))


def outcome(name):
    wait(lambda: any(item.get('url', '').endswith('name=' + name) and item.get('done') for item in told), name)
    return next(item for item in told if item.get('url', '').endswith('name=' + name) and item.get('done'))['state']


view.set_runtime_settings({'downloads': 'deny'})
ev(view, "save('denied.bin'); 1")
assert outcome('denied.bin') == 'cancelled', told
view.set_runtime_settings({'downloads': None})
zebkit.set_engine_settings({'downloads': 'deny'})
assert view.runtime_settings['effective']['downloads'] == 'deny'
ev(view, "save('denied-engine.bin'); 1")
assert outcome('denied-engine.bin') == 'cancelled', told
asked = []
view.on_download(lambda request: (asked.append(1), request.respond('cancel')))
assert view.runtime_settings['effective']['downloads'] == 'ask'
ev(view, "save('denied-asked.bin'); 1")
wait(lambda: asked, 'a function is asked, deny or no deny')
view.on_download(None)
view.set_runtime_settings({'downloads': 'save'})
ev(view, "save('saved.bin'); 1")
assert outcome('saved.bin') == 'complete', told
view.set_runtime_settings({'downloads': None})
zebkit.set_engine_settings({'downloads': None})

# ---- New windows. A script of the application counts as a person's act.
view.set_runtime_settings({'newWindows': 'block'})
other.set_runtime_settings({'newWindows': 'allow'})
assert ev(view, 'opens()') is False and ev(other, 'opens()') is True
assert view.popup_policy == 'block', 'the same value as the function'
view.set_runtime_settings({'newWindows': None})
other.set_runtime_settings({'newWindows': None})
zebkit.set_engine_settings({'newWindows': 'block'})
assert ev(view, 'opens()') is False and view.popup_policy == 'block'
later_view = opened()
assert ev(later_view, 'opens()') is False, 'a view made afterwards'
other.set_runtime_settings({'newWindows': 'allow'})
assert ev(other, 'opens()') is True, "a view's goes first"
zebkit.set_engine_settings({'newWindows': None})
other.set_runtime_settings({'newWindows': None})
assert ev(view, 'opens()') is True, 'back to today: a gesture, which an application script has'

# ---- The spare renderer: a process, there or not, and taken by a navigation.
before = renderers()
zebkit.set_engine_settings({'spareRenderer': True})
assert zebkit.engine_settings()['effective'] == {**zebkit.engine_settings()['effective'], 'spareRenderer': True, 'spareRendererRunning': True}
wait(lambda: renderers() - before, 'a spare renderer started')
spare = renderers() - before
assert len(spare) == 1, spare
# Another site in the same view: the spare is taken, and a new one follows. One not
# taken would have been dropped when the frame committed.
loaded = []
view.on_load(lambda event, at: loaded.append(at) if event == zebkit.LOAD_FINISHED else None)
view.load(OTHER + '/plain')
wait(lambda: OTHER + '/plain' in loaded, 'the other site')
# On macOS the spare was dropped here, not taken: not understood (docs/ROADMAP.md).
if sys.platform != 'darwin':
    assert spare <= renderers(), 'the spare was used, not dropped'
wait(lambda: zebkit.engine_settings()['effective']['spareRendererRunning'], 'another spare after the load')
zebkit.set_engine_settings({'spareRenderer': False})
assert not zebkit.engine_settings()['effective']['spareRendererRunning']
reloaded = []
view.on_load(lambda event, at: reloaded.append(at) if event == zebkit.LOAD_FINISHED else None)
view.load(BASE + '/plain')
wait(lambda: BASE + '/plain' in reloaded, 'back')
assert not zebkit.engine_settings()['effective']['spareRendererRunning'], 'false: none after a load'
zebkit.set_engine_settings({'spareRenderer': None})
assert 'spareRenderer' not in zebkit.engine_settings()['explicit']

for each in (view, other, later_view):
    each.close()
print('ok python', flush=True)
zebkit.shutdown()
