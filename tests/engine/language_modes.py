"""What the drivers of a feature's cases in every language share: http_auth.py,
certificate.py, user_agent.py, console.py and popup.py (hook_policies.py is the
same for <zebkit/hooks.h>).

    python3 tests/engine/<driver>.py <language> <command...>

A driver has one list of modes, each with the languages it is not written for
and why; it runs each mode of the language through that language's program,
once a mode, in an engine and a profile of its own, against servers of its own,
and checks what the servers saw whatever the program believes. language is c,
python, javascript or rust (some drivers have cdp too); command is the
program: for javascript zebkit-js and the script, for python the interpreter
and the program. A mode the language lacks is a "note:" line with its reason,
neither a pass nor a skip: the list is where the gaps are kept.

The C program, Rust's and Python's read the mode and the servers from the
environment and take the engine's switches as their arguments; zebkit-js takes
the switches before the script and the script's arguments after it.
ZEBKIT_TEST_CHILD_LIBRARY_PATH, when set, is the program's LD_LIBRARY_PATH (the
driver stays on the machine's libraries: scripts/glibc-test.sh).
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import check, note, program  # noqa: E402,F401

NAMES = {'c': 'C', 'python': 'Python', 'javascript': 'JavaScript', 'rust': 'Rust', 'cdp': 'CDP'}
# The driver's whole run, all its modes: within scripts/engine-test.sh's limit
# for it (600 s), so that a stall is this driver's report, by mode, and not the
# suite killing it. ZEBKIT_TEST_DRIVER_BUDGET (seconds) changes it.
BUDGET = float(os.environ.get('ZEBKIT_TEST_DRIVER_BUDGET') or 540)
_deadline = time.monotonic() + BUDGET


def arguments(argv, languages=('c', 'python', 'javascript', 'rust')):
    """(language, command) from the driver's arguments; exits 2 on a wrong one."""
    if len(argv) < 3 or argv[1] not in languages:
        print('usage: %s <%s> <command...>' % (os.path.basename(argv[0]), '|'.join(languages)), file=sys.stderr)
        sys.exit(2)
    return argv[1], argv[2:]


def modes(table, language):
    """The modes of table, [(mode, {language: why not})], that language has; a
    note for each it has not."""
    for mode, missing in table:
        if language in missing:
            note('%s %s: not run: %s' % (NAMES[language], mode, missing[language]))
        else:
            yield mode


def run(language, command, mode, what, switches=(), args=(), env=None, expect=None, seconds=90):
    """One mode in one engine: the program with the engine's switches and a
    profile of its own; harness.program()'s one check; the completed process.
    Its time is seconds, or what is left of the driver's budget (BUDGET); a
    mode with none left fails without running, and returns None."""
    left = _deadline - time.monotonic()
    if left < 5:
        check(False, what, "not run: the driver's budget of %d s was spent by the modes before it" % BUDGET)
        return None
    seconds = min(seconds, left - 2)
    environment = dict(os.environ, **(env or {}))
    environment.pop('LD_PRELOAD', None)
    if os.environ.get('ZEBKIT_TEST_CHILD_LIBRARY_PATH'):
        environment['LD_LIBRARY_PATH'] = os.environ['ZEBKIT_TEST_CHILD_LIBRARY_PATH']
    with tempfile.TemporaryDirectory(prefix='zebkit-%s-' % mode) as work:
        full = ['--no-sandbox', '--headless', '--disable-gpu', *switches, '--user-data-dir=' + work + '/profile']
        if language == 'javascript':
            line = command[:1] + full + command[1:] + list(args)
        else:
            line = list(command) + full
        return program(line, what, expect, seconds, environment)
