"""Native runtime preferences: isolation, frames, reload and atomic validation.

    python3 tests/engine/view_settings.py <base URL of tests/engine/pages> [engine switches...]

Exit 0 when every check passes.
"""
import copy
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import check, ev, finish, must_wait, part, zebkit  # noqa: E402

base = sys.argv[1]


def evaluate(view, code):
    value = ev(view, code)
    assert not (isinstance(value, tuple) and value[:1] in (('error',), ('timeout',))), value
    return value


def load(view, name):
    loaded = []
    view.on_load(lambda event, url: loaded.append(True) if event == zebkit.LOAD_FINISHED and url.startswith(base + '/settings.html') else None)
    view.load(base + '/settings.html?' + name)
    must_wait(lambda: bool(loaded), 'settings page load')


prefs = "({inline:!!window.inlineRan,dark:matchMedia('(prefers-color-scheme: dark)').matches,contrast:matchMedia('(prefers-contrast: more)').matches,motion:matchMedia('(prefers-reduced-motion: reduce)').matches,fontSize:getComputedStyle(document.body).fontSize,fixedSize:getComputedStyle(document.querySelector('#fixed')).fontSize,image:document.querySelector('#image').naturalWidth})"
def main():
    if zebkit.init([sys.argv[0], *sys.argv[2:], '--mute-audio'], settings={'logging': {'destination': 'none'}}) != 0:
        print('FAIL zebkit.init()')
        return 1
    try:
        run()
    finally:
        check(zebkit.shutdown(), 'shutdown')
    return finish()


def run():
    first, other = zebkit.View(), zebkit.View()
    initial = {'javaScript': False, 'images': False, 'remoteFonts': False, 'plugins': False,
               'spatialNavigation': True, 'smoothScrolling': True, 'textAutosizing': True,
               'defaultFontSize': 28, 'defaultFixedFontSize': 22, 'minimumFontSize': 10,
               'minimumLogicalFontSize': 10, 'defaultEncoding': 'utf8', 'autoplay': 'gesture',
               'colorScheme': 'dark', 'contrast': 'more', 'reducedMotion': True,
               'reducedTransparency': True, 'hideScrollbars': True, 'localStorage': True,
               'hyperlinkAuditing': False,
               'fonts': {'standard': 'serif', 'serif': {'Zyyy': 'serif', 'Hans': 'sans-serif'}}}
    expected = copy.deepcopy(initial)
    expected['defaultEncoding'] = 'UTF-8'
    with part('settings: none at first; set on one view, normalized, the other untouched', fatal=True):
        assert first.settings == other.settings == {}
        first.set_settings(initial)
        assert first.settings == expected and other.settings == {}, first.settings
    with part('scripts off: the inline script did not run; then on: colour scheme, contrast, motion, fonts, no images'):
        load(first, 'disabled')
        assert first.title == 'settings default', first.title
        first.set_settings({'javaScript': True})
        expected['javaScript'] = True
        found = evaluate(first, prefs)
        assert found == {'inline': False, 'dark': True, 'contrast': True, 'motion': True, 'fontSize': '28px', 'fixedSize': '22px', 'image': 0}, found
    baseline = {}
    with part('another view keeps the defaults', fatal=True):
        load(other, 'other')
        baseline = evaluate(other, prefs)
        assert baseline['inline'] and baseline['image'] == 1 and baseline['fontSize'] == '16px', baseline
    invalids = [None, [], {'unknown': None}, {'javaScript': 1}, {'images': 'false'},
                {'defaultFontSize': 0}, {'minimumFontSize': -1}, {'defaultFontSize': 1.5},
                {'defaultFixedFontSize': 513}, {'defaultEncoding': 'not-a-real-codepage'},
                {'autoplay': 'sometimes'}, {'colorScheme': 'blue'}, {'contrast': True},
                {'fonts': {'unknown': 'serif'}}, {'fonts': {'standard': {'bad': 'serif'}}},
                {'fonts': {'fixed': ''}}, {'fonts': {'standard': 'a\0b'}}]
    with part('each of %d invalid settings refused, nothing of it applied; {} changes nothing' % len(invalids)):
        for invalid in invalids:
            proposal = {'colorScheme': 'light', **invalid} if isinstance(invalid, dict) else invalid
            try:
                first.set_settings(proposal)
                raise AssertionError('invalid settings accepted: ' + repr(invalid))
            except zebkit.Error:
                assert first.settings == expected
        first.set_settings({})
        assert first.settings == expected
    with part('images turned on reach the document already loaded'):
        first.set_settings({'javaScript': True, 'images': True})
        must_wait(lambda: evaluate(first, 'document.querySelector("#image").naturalWidth===1'), 'images re-enabled in existing document')
    with part('autoplay: gesture refused, allow allowed, document-activation refused'):
        load(first, 'enabled')
        must_wait(lambda: evaluate(first, 'autoplayResult!=="waiting"'), 'autoplay gesture policy')
        assert evaluate(first, 'autoplayResult') == 'NotAllowedError'
        first.set_settings({'autoplay': 'allow'})
        load(first, 'autoplay-allowed')
        must_wait(lambda: evaluate(first, 'autoplayResult!=="waiting"'), 'autoplay allow policy')
        assert evaluate(first, 'autoplayResult') == 'allowed'
        first.set_settings({'autoplay': 'document-activation'})
        load(first, 'autoplay-blocked')
        must_wait(lambda: evaluate(first, 'autoplayResult!=="waiting"'), 'autoplay document policy')
        assert evaluate(first, 'autoplayResult') == 'NotAllowedError'
    with part("a cross-site frame, in a process of its own, has the view's settings"):
        must_wait(lambda: evaluate(first, '!!window.framePrefs'), 'cross-site frame report')
        assert evaluate(first, 'framePrefs.dark && framePrefs.contrast && framePrefs.motion && framePrefs.fontSize==="28px" && framePrefs.origin.includes("localhost")') is True
        found = evaluate(first, prefs)
        assert found['inline'] and found['image'] == 1, found
    with part('changed at runtime: in both processes, with change events; the other view unchanged'):
        first.set_settings({'colorScheme': 'light', 'contrast': 'no-preference', 'reducedMotion': False, 'defaultFontSize': 24})
        must_wait(lambda: evaluate(first, 'preferenceEvents.length>=3 && framePrefs.dark===false && framePrefs.motion===false && framePrefs.fontSize==="24px"'), 'runtime preferences in both processes')
        assert evaluate(other, prefs) == baseline
    with part('reset: no settings, the defaults on reload and in a new document'):
        first.set_settings({key: None for key in first.settings})
        assert first.settings == {}
        load(first, 'reset')
        assert evaluate(first, prefs) == baseline
        load(first, 'new-document')
        assert first.settings == {} and evaluate(first, prefs) == baseline


if __name__ == '__main__':
    sys.exit(main())
