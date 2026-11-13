"""zebkit-js with an error nothing caught after its module ran (thrown from a
timer or a view's event handler, or a promise rejected with no handler): the
error is printed ("Uncaught ..."), the script runs on to its end, and
zebkit-js exits 1. Without one it exits 0. tests/engine/script/uncaught.mjs is
the script.

    python3 -I tests/engine/script_uncaught.py ZEBKIT_JS [engine switches...]

Exit 0 when every check passes.
"""

import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import check as _check, finish  # noqa: E402


def check(ok, what, detail):
    # The detail only for a failure: stderr is the engine's log too.
    return _check(ok, what, None if ok else detail)

SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "script", "uncaught.mjs")
CASES = {
    "throw": "Uncaught Error: thrown from a timer",
    "reject": "Uncaught (in promise) Error: rejected with no handler",
    "hook": "Uncaught Error: thrown from a load handler",
    "none": None,
}


def main():
    program, switches = sys.argv[1], sys.argv[2:]
    for case, said in CASES.items():
        try:
            done = subprocess.run([program, *switches, SCRIPT, case], capture_output=True, text=True, timeout=60)
        except subprocess.TimeoutExpired:
            check(False, case + ": zebkit-js ended", "timed out")
            continue
        ran_on = "ran on after it" in done.stdout
        if said is None:
            check(done.returncode == 0 and ran_on and "Uncaught" not in done.stderr,
                  "without an uncaught error zebkit-js exits 0", (done.returncode, done.stderr[-400:]))
        else:
            check(said in done.stderr, case + ": the error is printed", done.stderr[-400:])
            check(ran_on, case + ": the script runs on after it", done.stdout[-400:])
            check(done.returncode == 1, case + ": zebkit-js exits 1 as it ends", done.returncode)
    return finish()


if __name__ == "__main__":
    sys.exit(main())
