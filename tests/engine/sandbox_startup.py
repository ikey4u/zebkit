"""No sandbox, no start: tests/engine/sandbox_startup.py <zebkit-run>. What the engine says where
there is no sandbox, and that --no-sandbox is stated, not silent (docs/SANDBOX.md)."""
import os
import subprocess
import sys
import tempfile

checks = failed = 0


def check(ok, what, detail=''):
    global checks, failed
    checks += 1
    failed += not ok
    print(('ok   ' if ok else 'FAIL ') + what + (f': {detail!r}' if detail and not ok else ''), flush=True)


def run(*switches):
    with tempfile.TemporaryDirectory(prefix='zebkit-sandbox-') as profile:
        process = subprocess.Popen([sys.argv[1], '--headless', f'--user-data-dir={profile}', *switches, 'about:blank'],
                                   stderr=subprocess.PIPE, stdout=subprocess.DEVNULL, text=True)
        try:
            _, err = process.communicate(timeout=4)
            return process.returncode, err
        except subprocess.TimeoutExpired:  # It started: it stays until told.
            process.terminate()
            _, err = process.communicate(timeout=30)
            return None, err


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
print(f'{checks - failed} of {checks} checks passed', flush=True)
sys.exit(1 if failed else 0)
