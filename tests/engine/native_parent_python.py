"""View.attach_native() from Python, under Xvfb: a window of plain Xlib as the parent, the
tree and the pixels read back, resized, detached, attached again, the parent destroyed
first. tests/engine/native_parent.c has the rest. Run by scripts/engine-test.sh."""

import ctypes as C
import sys

import zebkit

x = C.CDLL("libX11.so.6")
x.XOpenDisplay.restype = C.c_void_p
x.XDefaultRootWindow.argtypes = [C.c_void_p]
x.XDefaultRootWindow.restype = C.c_ulong
x.XCreateSimpleWindow.argtypes = [C.c_void_p, C.c_ulong] + [C.c_int] * 2 + [C.c_uint] * 3 + [C.c_ulong] * 2
x.XCreateSimpleWindow.restype = C.c_ulong
x.XMapWindow.argtypes = [C.c_void_p, C.c_ulong]
x.XDestroyWindow.argtypes = [C.c_void_p, C.c_ulong]
x.XSync.argtypes = [C.c_void_p, C.c_int]
x.XQueryTree.argtypes = [C.c_void_p, C.c_ulong, C.POINTER(C.c_ulong), C.POINTER(C.c_ulong),
                         C.POINTER(C.POINTER(C.c_ulong)), C.POINTER(C.c_uint)]
x.XFree.argtypes = [C.c_void_p]
x.XGetImage.argtypes = [C.c_void_p, C.c_ulong, C.c_int, C.c_int, C.c_uint, C.c_uint, C.c_ulong, C.c_int]
x.XGetImage.restype = C.c_void_p
x.XGetPixel.argtypes = [C.c_void_p, C.c_int, C.c_int]
x.XGetPixel.restype = C.c_ulong
x.XDestroyImage.argtypes = [C.c_void_p]

failures = []
display = x.XOpenDisplay(None)
root = x.XDefaultRootWindow(display)
host = x.XCreateSimpleWindow(display, root, 700, 50, 640, 480, 0, 0, 0x202020)
x.XMapWindow(display, host)
x.XSync(display, 0)
PAGE_COLOR = 0x12a456
state = {"stage": 0, "tries": 0, "closed": False}


def check(ok, what, detail=None):
    print(f"{'ok  ' if ok else 'FAIL'} {what}" + (f": {detail}" if detail is not None else ""), flush=True)
    if not ok:
        failures.append(what)


def children():
    r, p, listed, count = C.c_ulong(), C.c_ulong(), C.POINTER(C.c_ulong)(), C.c_uint()
    x.XQueryTree(display, host, C.byref(r), C.byref(p), C.byref(listed), C.byref(count))
    found = [listed[i] for i in range(count.value)]
    if listed:
        x.XFree(listed)
    return found


def pixel(px, py):
    image = x.XGetImage(display, host, px, py, 1, 1, 0xffffffffffffffff, 2)
    if not image:
        return None
    value = x.XGetPixel(image, 0, 0) & 0xffffff
    x.XDestroyImage(image)
    return value


def attached_and_drawn():
    return len(children()) == 1 and pixel(150, 150) == PAGE_COLOR


def finish():
    print(f"{len(failures)} failures", flush=True)
    zebkit.quit()


STEPS = []


def step(what):
    def add(function):
        STEPS.append((what, function))
        return function
    return add


@step("attach_native: the host's child, the page's pixels")
def _attached():
    return attached_and_drawn()


@step("set_bounds: the page is where it was put")
def _moved():
    if state.get("moved") is None:
        state["moved"] = True
        view.set_bounds(100, 100, 200, 200)
    return pixel(50, 50) == 0x202020 and pixel(150, 150) == PAGE_COLOR


@step("detach_native: not the host's child any more")
def _detached():
    if state.get("detached") is None:
        state["detached"] = view.detach_native()
        check(state["detached"], "detach_native returned true")
        native = view.native_window()
        check(native and native["kind"] == "x11" and native["handle"] != host, "native_window: detaching restores own window")
    return not children()


@step("attach_native again")
def _again():
    if state.get("again") is None:
        state["again"] = True
        view.attach_native(host, 0, 0, 400, 300)
        check(view.native_window() == {"kind": "x11", "handle": host}, "native_window: reports containing host")
    return attached_and_drawn()


@step("the parent destroyed first: the view closes")
def _destroyed():
    if state.get("destroyed") is None:
        state["destroyed"] = True
        x.XDestroyWindow(display, host)
        x.XSync(display, 0)
    return state["closed"]


def tick():
    if state["stage"] == len(STEPS):
        finish()
        return
    what, function = STEPS[state["stage"]]
    x.XSync(display, 0)
    if function():
        check(True, what)
        state["stage"] += 1
        state["tries"] = 0
    else:
        state["tries"] += 1
        if state["tries"] > 150:
            check(False, what)
            finish()
            return
    zebkit.post(tick, 100)


def loaded(event, url):
    if event == zebkit.LOAD_FINISHED and not state.get("started"):
        state["started"] = True
        try:
            view.attach_native(0x7ffffff0, 0, 0, 100, 100)
            check(False, "a window that is not there: refused")
        except zebkit.Error as error:
            check("no X11 window" in str(error), "a window that is not there: refused", error)
        view.attach_native(host, 0, 0, 400, 300)
        tick()


def ready():
    global view
    view = zebkit.View("data:text/html,<!doctype html><body style='background:%2312a456'>", 640, 480)
    view.on_load(loaded)
    view.on_close(lambda: state.update(closed=True))


code = zebkit.main(ready, sys.argv)
sys.exit(1 if code or failures else 0)
