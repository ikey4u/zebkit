"""How views, contexts and their callbacks end (<zebkit/zebkit.h>): an attached
view closes with its parent, a context closed from a page's own callback, the
C API's rule for a closed view and the wrong thread, zebkit_post_task()'s
answer, and the window's "title" option. The rule is tried on every exported
function that takes a zebkit_view* (read from the headers): with NULL, with a
closed view, and with a live view from another thread, each does nothing and
returns its failure value.

    python3 -I tests/engine/view_lifetime.py [engine switches...]

Exit 0 when every check passes.
"""

import ctypes
import glob
import json
import os
import re
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import check, ev, finish, pump, wait, zebkit  # noqa: E402

lib = zebkit._lib
TASK = ctypes.CFUNCTYPE(None, ctypes.c_void_p)
lib.zebkit_post_task.restype = ctypes.c_bool
lib.zebkit_post_task.argtypes = [TASK, ctypes.c_void_p]
lib.zebkit_view_attach.restype = ctypes.c_bool
lib.zebkit_view_attach.argtypes = [ctypes.c_void_p, ctypes.c_void_p] + [ctypes.c_int] * 4
lib.zebkit_view_get_url.restype = ctypes.c_void_p
lib.zebkit_view_get_url.argtypes = [ctypes.c_void_p]
lib.zebkit_view_load_url.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
lib.zebkit_free.argtypes = [ctypes.c_void_p]
lib.zebkit_context_new.restype = ctypes.c_void_p
lib.zebkit_context_close.argtypes = [ctypes.c_void_p]
lib.zebkit_view_new_in.restype = ctypes.c_void_p
lib.zebkit_view_new_in.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
LOAD = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_void_p)
VIEW = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p)
lib.zebkit_view_on_load.argtypes = [ctypes.c_void_p, LOAD, ctypes.c_void_p]
lib.zebkit_view_on_close.argtypes = [ctypes.c_void_p, VIEW, ctypes.c_void_p]

PAGE = "data:text/html,<title>page</title>"


def opened(url=PAGE, **options):
    view = zebkit.View(options=options) if options else zebkit.View()
    loads = []
    view.on_load(lambda phase, u: loads.append(phase))
    view.load(url)
    wait(lambda: zebkit.LOAD_FINISHED in loads)
    return view


def text(pointer):
    if not pointer:
        return None
    value = ctypes.string_at(pointer).decode()
    lib.zebkit_free(pointer)
    return value


def post_task():
    ran = []
    task = TASK(lambda data: ran.append(threading.get_ident()))
    check(lib.zebkit_post_task(task, None) is True, "zebkit_post_task() says the task is posted")
    wait(lambda: ran)
    check(ran == [threading.get_ident()], "and runs it on the engine's thread", ran)
    check(lib.zebkit_post_task(TASK(), None) is False, "a NULL function is refused, not posted")
    told = []
    other = TASK(lambda data: told.append(threading.get_ident()))

    def elsewhere():
        told.append(lib.zebkit_post_task(other, None))
    thread = threading.Thread(target=elsewhere)
    thread.start()
    thread.join()
    wait(lambda: len(told) == 2)
    check(told[:1] == [True] and told[1:] == [threading.get_ident()],
          "posted from another thread, it runs on the engine's", told)


def attached_closes_with_parent():
    parent = opened()
    child = opened()
    closed = []
    child.on_close(lambda: closed.append("child"))
    ok = lib.zebkit_view_attach(child._check(), parent._check(), 10, 20, 300, 200)
    check(ok is True, "zebkit_view_attach() says the view is attached", ok)
    owner = (child.window or {}).get("owner")
    # Without windows (--headless) there is no window to own.
    check(owner == ("none" if zebkit.is_headless() else "view"), "its window's owner is \"view\"", owner)
    check(lib.zebkit_view_attach(child._check(), child._check(), 0, 0, 10, 10) is False,
          "a view is not attached to itself")
    parent.close()
    wait(lambda: closed)
    check(closed == ["child"], "the attached view closes with its parent, on_close told", closed)


def closed_view_rule():
    view = opened()
    handle = view._check()
    url = text(lib.zebkit_view_get_url(handle))
    check(url == PAGE, "zebkit_view_get_url() of a live view", url)
    got = []

    def elsewhere():
        got.append(lib.zebkit_view_get_url(handle))
        got.append(lib.zebkit_view_attach(handle, handle, 0, 0, 1, 1))
    thread = threading.Thread(target=elsewhere)
    thread.start()
    thread.join()
    check(got == [None, False], "from another thread a view function does nothing and returns its failure", got)
    view.close()
    wait(lambda: view._closed)
    check(lib.zebkit_view_get_url(handle) is None, "a closed view's URL is NULL, not a crash")
    check(lib.zebkit_view_attach(handle, None, 0, 0, 1, 1) is False, "nor is attaching one")


# The headers: the package's (a test bundle has no source tree), else the source's.
HEADERS = os.path.join(os.environ.get("ZEBKIT_DIR", ""), "include", "zebkit")
if not os.path.isfile(os.path.join(HEADERS, "zebkit.h")):
    HEADERS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src", "engine", "include", "zebkit")
NUMBERS = {"bool": ctypes.c_bool, "int": ctypes.c_int, "unsigned": ctypes.c_uint, "uint32_t": ctypes.c_uint32,
           "uint64_t": ctypes.c_uint64, "size_t": ctypes.c_size_t, "uintptr_t": ctypes.c_size_t,
           "float": ctypes.c_float, "double": ctypes.c_double}
# Not failures: what these give for no view is the engine's (its policy, the
# permission state's "invalid").
ANSWER_ANYWAY = {"zebkit_view_get_popup_policy", "zebkit_view_get_dialog_policy", "zebkit_view_get_permission"}


def view_functions():
    """(name, ctypes function, the view parameters' positions, returns a pointer)
    of every exported function taking a zebkit_view*, from the headers."""
    declaration = re.compile(r"ZEBKIT_API\s+([^;(]*?)\b(zebkit_\w+)\s*\(([^)]*)\)\s*;", re.S)
    found = []
    for header in sorted(glob.glob(os.path.join(HEADERS, "*.h"))):
        source = re.sub(r"/\*.*?\*/", "", open(header).read(), flags=re.S)
        for match in declaration.finditer(source):
            result, name, parameters = match.group(1).strip(), match.group(2), match.group(3)
            if not re.search(r"\bzebkit_view\s*\*", parameters) or not hasattr(lib, name):
                continue
            types, views = [], []
            for index, parameter in enumerate(p.strip() for p in parameters.split(",")):
                kind = re.sub(r"\s*\b\w+$", "", parameter) if not parameter.endswith("*") else parameter
                kind = kind.replace("const ", "").strip()
                if re.fullmatch(r"zebkit_view\s*\*", kind):
                    views.append(index)
                    types.append(ctypes.c_void_p)
                elif "*" in kind or kind.endswith("_func"):
                    types.append(ctypes.c_void_p)
                elif kind in NUMBERS:
                    types.append(NUMBERS[kind])
                elif kind.startswith("zebkit_"):
                    types.append(ctypes.c_int)  # an enum
                else:
                    types = None
                    break
            if types is None:
                continue
            function = getattr(lib, name)
            function.argtypes = types
            pointer = "*" in result
            function.restype = (ctypes.c_void_p if pointer else None if result == "void"
                                else NUMBERS.get(result, ctypes.c_int))
            found.append((name, function, views, pointer, result))
    return found


def call(function, views, view):
    arguments = [view if i in views else None if t is ctypes.c_void_p else t(0)
                 for i, t in enumerate(function.argtypes)]
    return function(*arguments)


def failure_value(name, result, pointer, value):
    if name in ANSWER_ANYWAY or result == "void":
        return True
    return not value if pointer or result == "bool" else True


def every_view_function():
    functions = view_functions()
    check(len(functions) > 140, "the headers' view functions found", len(functions))
    for kind, view in (("NULL", None), ("a closed view", None)):
        if kind == "a closed view":
            closing = opened()
            view = closing._check()
            closing.close()
            wait(lambda: closing._closed)
        wrong = [name for name, function, views, pointer, result in functions
                 if not failure_value(name, result, pointer, call(function, views, view))]
        check(not wrong, "every view function with %s does nothing and returns its failure" % kind, wrong)
    live = opened()
    handle = live._check()
    wrong = []

    def elsewhere():
        for name, function, views, pointer, result in functions:
            if not failure_value(name, result, pointer, call(function, views, handle)):
                wrong.append(name)
    thread = threading.Thread(target=elsewhere)
    thread.start()
    thread.join()
    check(not wrong, "every view function from another thread does nothing and returns its failure", wrong)
    pump()
    check(not live._closed and text(lib.zebkit_view_get_url(handle)) == PAGE,
          "and the view is as it was, open", live._closed)
    live.close()
    wait(lambda: live._closed)


def context_closed_from_callback():
    context = lib.zebkit_context_new()
    check(bool(context), "zebkit_context_new()")
    view = lib.zebkit_view_new_in(context, b"about:blank", 0, 0)
    closed = []
    state = {"context": context}

    def loaded(handle, event, url, data):
        # Inside the page's own on_load: the context, and its view, go after it.
        if event == zebkit.LOAD_FINISHED and state["context"]:
            lib.zebkit_context_close(state["context"])
            state["context"] = None
            closed.append("asked")
    on_load = LOAD(loaded)
    on_close = VIEW(lambda handle, data: closed.append("closed"))
    lib.zebkit_view_on_close(view, on_close, None)
    lib.zebkit_view_on_load(view, on_load, None)
    lib.zebkit_view_load_url(view, PAGE.encode())
    wait(lambda: "closed" in closed)
    check(closed == ["asked", "closed"],
          "zebkit_context_close() from the page's callback closes its view after the callback", closed)
    pump()


def title_option():
    view = opened(title="from the options")
    window = view.window or {}
    explicit = window.get("options", {}).get("explicit", {})
    check(explicit.get("title") == "from the options", "the \"title\" option is the window's", explicit)
    view.set_title("from set_title")
    explicit = (view.window or {}).get("options", {}).get("explicit", {})
    check(explicit.get("title") == "from set_title", "zebkit_view_set_title() is that same option", explicit)
    view.set_title(None)
    explicit = (view.window or {}).get("options", {}).get("explicit", {})
    check("title" not in explicit, "NULL gives the page's title back", explicit)
    view.close()


def main():
    if zebkit.init(["view-lifetime"] + sys.argv[1:]) != 0:
        print("FAIL zebkit.init()")
        return 1
    try:
        for test in (post_task, attached_closes_with_parent, closed_view_rule, every_view_function,
                     context_closed_from_callback, title_option):
            try:
                test()
            except Exception as error:  # noqa: BLE001
                check(False, test.__name__ + " ran to the end", repr(error))
    finally:
        zebkit.shutdown()
    after = []
    check(lib.zebkit_post_task(TASK(lambda data: after.append(1)), None) is False,
          "once the engine has stopped, zebkit_post_task() refuses")
    return finish()


if __name__ == "__main__":
    sys.exit(main())
