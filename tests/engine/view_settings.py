"""Native runtime preferences: isolation, frames, reload and atomic validation."""
import copy
import sys
import time
import zebkit

base = sys.argv[1]
assert zebkit.init([sys.argv[0], *sys.argv[2:], '--mute-audio'], settings={'logging': {'destination': 'none'}}) == 0


def wait(condition, message='condition', timeout=15):
    until = time.monotonic() + timeout
    while not condition() and time.monotonic() < until:
        assert zebkit.pump()
        time.sleep(.001)
    assert condition(), message


def evaluate(view, code):
    result = []
    view.evaluate(code, lambda value, error: result.append((value, error)))
    wait(lambda: bool(result), code)
    assert result[0][1] is None, result
    return result[0][0]


def load(view, name):
    loaded = []
    view.on_load(lambda event, url: loaded.append(True) if event == zebkit.LOAD_FINISHED and url.startswith(base + '/settings.html') else None)
    view.load(base + '/settings.html?' + name)
    wait(lambda: bool(loaded), 'settings page load')


prefs = "({inline:!!window.inlineRan,dark:matchMedia('(prefers-color-scheme: dark)').matches,contrast:matchMedia('(prefers-contrast: more)').matches,motion:matchMedia('(prefers-reduced-motion: reduce)').matches,fontSize:getComputedStyle(document.body).fontSize,fixedSize:getComputedStyle(document.querySelector('#fixed')).fontSize,image:document.querySelector('#image').naturalWidth})"
try:
    first, other = zebkit.View(), zebkit.View()
    assert first.settings == other.settings == {}
    initial = {'javaScript': False, 'images': False, 'remoteFonts': False, 'plugins': False,
               'spatialNavigation': True, 'smoothScrolling': True, 'textAutosizing': True,
               'defaultFontSize': 28, 'defaultFixedFontSize': 22, 'minimumFontSize': 10,
               'minimumLogicalFontSize': 10, 'defaultEncoding': 'utf8', 'autoplay': 'gesture',
               'colorScheme': 'dark', 'contrast': 'more', 'reducedMotion': True,
               'reducedTransparency': True, 'hideScrollbars': True, 'localStorage': True,
               'hyperlinkAuditing': False,
               'fonts': {'standard': 'serif', 'serif': {'Zyyy': 'serif', 'Hans': 'sans-serif'}}}
    first.set_settings(initial)
    expected = copy.deepcopy(initial)
    expected['defaultEncoding'] = 'UTF-8'
    assert first.settings == expected and other.settings == {}
    load(first, 'disabled')
    assert first.title == 'settings default', first.title
    first.set_settings({'javaScript': True})
    expected['javaScript'] = True
    found = evaluate(first, prefs)
    assert found == {'inline': False, 'dark': True, 'contrast': True, 'motion': True, 'fontSize': '28px', 'fixedSize': '22px', 'image': 0}, found
    load(other, 'other')
    baseline = evaluate(other, prefs)
    assert baseline['inline'] and baseline['image'] == 1 and baseline['fontSize'] == '16px', baseline
    for invalid in [None, [], {'unknown': None}, {'javaScript': 1}, {'images': 'false'},
                    {'defaultFontSize': 0}, {'minimumFontSize': -1}, {'defaultFontSize': 1.5},
                    {'defaultFixedFontSize': 513}, {'defaultEncoding': 'not-a-real-codepage'},
                    {'autoplay': 'sometimes'}, {'colorScheme': 'blue'}, {'contrast': True},
                    {'fonts': {'unknown': 'serif'}}, {'fonts': {'standard': {'bad': 'serif'}}},
                    {'fonts': {'fixed': ''}}, {'fonts': {'standard': 'a\0b'}}]:
        proposal = {'colorScheme': 'light', **invalid} if isinstance(invalid, dict) else invalid
        try:
            first.set_settings(proposal)
            raise AssertionError('invalid settings accepted: ' + repr(invalid))
        except zebkit.Error:
            assert first.settings == expected
    first.set_settings({})
    assert first.settings == expected
    first.set_settings({'javaScript': True, 'images': True})
    wait(lambda: evaluate(first, 'document.querySelector("#image").naturalWidth===1'), 'images re-enabled in existing document')
    load(first, 'enabled')
    wait(lambda: evaluate(first, 'autoplayResult!=="waiting"'), 'autoplay gesture policy')
    assert evaluate(first, 'autoplayResult') == 'NotAllowedError'
    first.set_settings({'autoplay': 'allow'})
    load(first, 'autoplay-allowed')
    wait(lambda: evaluate(first, 'autoplayResult!=="waiting"'), 'autoplay allow policy')
    assert evaluate(first, 'autoplayResult') == 'allowed'
    first.set_settings({'autoplay': 'document-activation'})
    load(first, 'autoplay-blocked')
    wait(lambda: evaluate(first, 'autoplayResult!=="waiting"'), 'autoplay document policy')
    assert evaluate(first, 'autoplayResult') == 'NotAllowedError'
    wait(lambda: evaluate(first, '!!window.framePrefs'), 'cross-site frame report')
    assert evaluate(first, 'framePrefs.dark && framePrefs.contrast && framePrefs.motion && framePrefs.fontSize==="28px" && framePrefs.origin.includes("localhost")') is True
    found = evaluate(first, prefs)
    assert found['inline'] and found['image'] == 1, found
    first.set_settings({'colorScheme': 'light', 'contrast': 'no-preference', 'reducedMotion': False, 'defaultFontSize': 24})
    wait(lambda: evaluate(first, 'preferenceEvents.length>=3 && framePrefs.dark===false && framePrefs.motion===false && framePrefs.fontSize==="24px"'), 'runtime preferences in both processes')
    assert evaluate(other, prefs) == baseline
    first.set_settings({key: None for key in first.settings})
    assert first.settings == {}
    load(first, 'reset')
    assert evaluate(first, prefs) == baseline
    load(first, 'new-document')
    assert first.settings == {} and evaluate(first, prefs) == baseline
    print('ok native view settings: validation, isolation, images/scripts, fonts, media preferences, OOPIF, runtime changes and reset', flush=True)
finally:
    assert zebkit.shutdown()
