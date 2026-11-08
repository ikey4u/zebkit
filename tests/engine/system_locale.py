"""The page's locale is the system's, as in Chrome: Intl's default and
navigator.language follow LANG on Linux, en-US where the system has none.
Chromium 157's base begins with en-US until the embedder says otherwise
(patches/1083).

    LANG=ja_JP.UTF-8 python3 tests/engine/system_locale.py ja-JP [engine switches...]

Exit 0 when every check passes.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "bindings", "python"))

import zebkit  # noqa: E402
from zebkit.automation import Page, run  # noqa: E402

failures = []
checks = 0


def check(ok, what, detail=None):
    global checks
    checks += 1
    print(("ok   " if ok else "FAIL ") + what + (": %r" % (detail,) if detail is not None else ""), flush=True)
    if not ok:
        failures.append(what)


async def test(want):
    page = Page(width=200, height=100)
    await page.goto("data:text/html,<title>locale</title>")
    seen = await page.evaluate("""() => ({
        intl: new Intl.DateTimeFormat().resolvedOptions().locale,
        language: navigator.language,
        weekday: new Date(0).toLocaleDateString(undefined, {weekday: 'long', timeZone: 'UTC'}),
    })""")
    check(seen["intl"] == want, "Intl's default locale is the system's", seen["intl"])
    check(seen["language"] == want, "navigator.language is the system's", seen["language"])
    if want == "ja-JP":
        check(seen["weekday"] == "木曜日", "dates in the system's language", seen["weekday"])


def main():
    want = sys.argv[1]

    def ready():
        future = run(test(want))

        def finished(done):
            if done._error is not None:
                check(False, "the test ran to the end", repr(done._error))
            print("%d of %d checks passed" % (checks - len(failures), checks), flush=True)
            zebkit.quit()
        future.add_done_callback(finished)

    code = zebkit.main(ready, ["python"] + sys.argv[2:])
    sys.exit(1 if code or failures or not checks else 0)


if __name__ == "__main__":
    main()
