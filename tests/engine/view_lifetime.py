"""How views, contexts and their callbacks end (<zebkit/zebkit.h>): an attached
view closes with its parent, a context closed from a page's own callback, the
C API's rule for a closed view and the wrong thread, zebkit_post_task()'s
answer, the window's "title" option, the page's process going
(zebkit_view_on_process_gone()) and the view loading again after it,
zebkit_call_free(), and zebkit.pathForFile() for two dropped files of one name. The rule is tried on every exported
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
TOLD = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p)
lib.zebkit_view_on_process_gone.argtypes = [ctypes.c_void_p, TOLD, ctypes.c_void_p]
lib.zebkit_view_reload.argtypes = [ctypes.c_void_p]
BIND = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p)
lib.zebkit_view_bind.argtypes = [ctypes.c_void_p, ctypes.c_char_p, BIND, ctypes.c_void_p]
lib.zebkit_call_free.argtypes = [ctypes.c_void_p]
DRAG = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p)
lib.zebkit_view_drag_enter.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_float, ctypes.c_float,
                                       ctypes.c_uint, ctypes.c_uint]
lib.zebkit_view_drag_over.argtypes = [ctypes.c_void_p, ctypes.c_float, ctypes.c_float, ctypes.c_uint,
                                      ctypes.c_uint, DRAG, ctypes.c_void_p]
lib.zebkit_view_drop.argtypes = [ctypes.c_void_p, ctypes.c_float, ctypes.c_float, ctypes.c_uint, DRAG,
                                 ctypes.c_void_p]
DRAG_COPY = 1
ALLOW_APP = 1

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
    # Under --headless too: the view is in another view's window.
    check(owner == "view", "its window's owner is \"view\"", owner)
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


CONSOLE = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p)


def observers_kept_off_thread():
    """The sweep above calls the void observers (on_console,
    on_window_open_event) with NULL, which cannot show whether another thread's
    call was refused: here each has an observer, another thread tries to
    replace it, and the engine's own still gets the reports. (Through the
    library itself: the Python binding's own thread guard would stop the call
    before the engine sees it.)"""
    live = opened()
    handle = live._check()
    lib.zebkit_view_on_console.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    lib.zebkit_view_on_window_open_event.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    told = {"console": [], "console-other": [], "open": [], "open-other": []}

    def keep(key):
        return CONSOLE(lambda view, json_text, data: told[key].append(json_text.decode() if json_text else None))
    callbacks = {key: keep(key) for key in told}
    as_pointer = {key: ctypes.cast(function, ctypes.c_void_p) for key, function in callbacks.items()}
    lib.zebkit_view_on_console(handle, as_pointer["console"], None)
    lib.zebkit_view_on_window_open_event(handle, as_pointer["open"], None)

    def elsewhere():
        lib.zebkit_view_on_console(handle, as_pointer["console-other"], None)
        lib.zebkit_view_on_window_open_event(handle, as_pointer["open-other"], None)
    thread = threading.Thread(target=elsewhere)
    thread.start()
    thread.join()
    ev(live, "console.log('zebkit-kept-off-thread'), 1")
    check(wait(lambda: any(t and "zebkit-kept-off-thread" in t for t in told["console"]), 10)
          and not told["console-other"],
          "on_console from another thread: refused, the engine's observer kept", told)
    ev(live, "window.open('about:blank#zebkit-kept', '_blank'), 1")
    check(wait(lambda: told["open"], 10) and not told["open-other"],
          "on_window_open_event from another thread: refused, the engine's observer kept", told)
    lib.zebkit_view_on_console(handle, None, None)
    lib.zebkit_view_on_window_open_event(handle, None, None)
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


def process_gone():
    view = opened()
    handle = view._check()
    told = []
    on_gone = TOLD(lambda h, text, data: told.append((h, json.loads(text.decode()))))
    lib.zebkit_view_on_process_gone(handle, on_gone, None)
    for url, reason in ((b"chrome://crash", "crashed"), (b"chrome://kill", "killed")):
        told.clear()
        lib.zebkit_view_load_url(handle, url)
        wait(lambda: told, 20)
        event = told[0][1] if told else {}
        check(len(told) == 1 and told[0][0] == handle and event.get("reason") == reason
              and isinstance(event.get("exitCode"), int),
              "the page's process went: zebkit_view_on_process_gone() says " + reason, told)
        check(not view._closed, "and the view stays open")
        loads = []
        view.on_load(lambda phase, u: loads.append(phase))
        lib.zebkit_view_load_url(handle, PAGE.encode())
        wait(lambda: zebkit.LOAD_FINISHED in loads, 20)
        check(zebkit.LOAD_FINISHED in loads and ev(view, "document.title") == "page",
              "a page loads in it again, in a new process", loads)
        loads.clear()
        lib.zebkit_view_reload(handle)
        wait(lambda: zebkit.LOAD_FINISHED in loads, 20)
        check(zebkit.LOAD_FINISHED in loads, "and reloads", loads)
    lib.zebkit_view_on_process_gone(handle, TOLD(), None)
    told.clear()
    lib.zebkit_view_load_url(handle, b"chrome://crash")
    pump()
    wait(lambda: told, 2)
    check(not told, "with NULL nothing is told", told)
    view.close()
    wait(lambda: view._closed)


def call_freed():
    view = opened()
    handle = view._check()
    view.grant("*", ALLOW_APP)
    on_call = BIND(lambda h, origin, args, call, data: lib.zebkit_call_free(call))
    lib.zebkit_view_bind(handle, b"dropped", on_call, None)
    view.load(PAGE)
    pump()
    wait(lambda: ev(view, "typeof zebkit") == "object")
    result = ev(view, "zebkit.app.dropped(1).then(() => 'resolved', (e) => 'rejected: ' + e.message)")
    check(result == "rejected: the application dropped the call",
          "zebkit_call_free(): the page's promise is rejected", result)
    view.close()
    wait(lambda: view._closed)


def dropped_same_names():
    base = os.environ.get("ZEBKIT_TEST_FILES") or os.environ.get("TMPDIR") or "/tmp"
    root = os.path.join(base, "view-lifetime-drop-%d" % os.getpid())
    paths = []
    for folder in ("one", "two"):
        os.makedirs(os.path.join(root, folder), exist_ok=True)
        path = os.path.join(root, folder, "same.txt")
        with open(path, "w") as file:
            file.write(folder)
        paths.append(path)
    page = ("data:text/html,<body style='margin:0'><div id=zone style='width:400px;height:300px'></div><script>"
            "window.got=null;zone.ondragover=(e)=>e.preventDefault();"
            "zone.ondrop=(e)=>{e.preventDefault();const f=[...e.dataTransfer.files];"
            "window.got=f.slice().reverse().map((x)=>zebkit.pathForFile(x)).concat(f.map((x)=>zebkit.pathForFile(x)));};"
            "</script>")
    view = opened()
    view.grant("*", ALLOW_APP)
    loads = []
    view.on_load(lambda phase, u: loads.append(phase))
    view.load(page)
    wait(lambda: zebkit.LOAD_FINISHED in loads)
    handle = view._check()
    told = []
    over = DRAG(lambda h, operation, data: told.append(("over", operation)))
    dropped = DRAG(lambda h, operation, data: told.append(("drop", operation)))
    lib.zebkit_view_drag_enter(handle, json.dumps({"files": paths}).encode(), 100, 100, DRAG_COPY, 0)
    lib.zebkit_view_drag_over(handle, 110, 110, DRAG_COPY, 0, over, None)
    wait(lambda: told)
    lib.zebkit_view_drop(handle, 110, 110, 0, dropped, None)
    wait(lambda: any(kind == "drop" for kind, _ in told))
    wait(lambda: ev(view, "window.got !== null"))
    got = ev(view, "window.got")
    # Asked for the second first: by name it would get the first's path.
    check(got == paths[::-1] + paths,
          "two dropped files of one name: each File its own path, whatever the order asked", got)
    view.close()
    wait(lambda: view._closed)
    for path in paths:
        os.remove(path)
        os.rmdir(os.path.dirname(path))
    os.rmdir(root)


def main():
    if zebkit.init(["view-lifetime"] + sys.argv[1:]) != 0:
        print("FAIL zebkit.init()")
        return 1
    try:
        for test in (post_task, attached_closes_with_parent, closed_view_rule, every_view_function,
                     observers_kept_off_thread, context_closed_from_callback, title_option, process_gone, call_freed,
                     dropped_same_names):
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
