"""No sandbox, no start: tests/engine/sandbox_startup.py <zebkit-run>. What the engine says where
there is no sandbox, and that --no-sandbox is stated, not silent (docs/SANDBOX.md)."""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import harness  # noqa: E402
from harness import Engine, finish  # noqa: E402


def check(ok, what, detail=None):
    # What the engine said, only when it is not what was expected.
    harness.check(ok, what, None if ok else detail)


def run(*switches):
    """(None, its standard error) when it started (served CDP); (exit status,
    standard error) when it ended first."""
    with tempfile.TemporaryDirectory(prefix='zebkit-sandbox-') as profile:
        engine = Engine([sys.argv[1], '--headless', f'--user-data-dir={profile}', *switches])
        # Running after the wait without the line is running too.
        started = engine.url is not None or engine.poll() is None
        code = engine.stop()
        return (None if started else code), ''.join(engine.lines)


code, err = run('--no-sandbox')
check(code is None, 'starts with --no-sandbox', err[-400:])
check('without the sandbox (--no-sandbox)' in err, 'and says so on standard error', err[-400:])

code, err = run()
if code is None:
    # This system gives it a sandbox: nothing to refuse, and nothing said about none.
    check('without the sandbox' not in err, 'a sandbox here: started with it', err[-400:])
else:
    check(code != 0, 'without a sandbox it refuses to start', code)
    check('no sandbox for the page processes' in err or 'as root' in err, 'it says why', err[-800:])
    check('--no-sandbox' in err, 'and names --no-sandbox', err[-800:])
    if os.geteuid() != 0:
        check('zebkit-sandbox-install' in err, 'and the helper', err[-800:])
    check('Trace/breakpoint' not in err and 'FATAL' not in err, 'without a crash', err[-800:])

code, err = run('--disable-setuid-sandbox', '--disable-namespace-sandbox')
check(code not in (None, 0), 'told not to use either: refuses', code)
sys.exit(finish())
