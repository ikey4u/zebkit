"""Playwright's API for zebkit's pages, natively (docs/AUTOMATION.md).

The names are those of Playwright's async API for Python; each command runs
inside the engine (<zebkit/automation.h>), without CDP or a driver process.

    import zebkit
    from zebkit.automation import Page, expect, run

    async def test():
        page = Page()
        await page.goto("https://example.com/")
        await page.get_by_role("link", name="More information").click()
        await expect(page.locator("h1")).to_have_text("Example Domains")
        zebkit.quit()

    zebkit.main(lambda: run(test()))

Coroutines run on the engine's thread, driven by run(): an await is a
command (or a wait) in flight, and the engine goes on meanwhile.
"""

import base64
import ctypes
import json
import pathlib
import re

from . import _VIEW, Error, View, _P, _S, _V, _alive, _declare, _guard, _view_on_close, post

__all__ = ["Page", "Frame", "Locator", "FrameLocator", "ElementHandle", "JSHandle", "Route", "Request",
           "Response", "Dialog", "FileChooser", "Download", "BrowserContext", "Worker", "Tracing", "Pixels", "expect", "run",
           "new_context", "listen", "TimeoutError", "commands"]

_DONE = ctypes.CFUNCTYPE(None, _V, _S, _S, _P)
_EVENT = ctypes.CFUNCTYPE(None, _V, _S, _S, _P)
_call = _declare("zebkit_automation_call", None, _V, _S, _S, _DONE, _P)
_on_event = _declare("zebkit_automation_on_event", None, _V, _EVENT, _P)
_commands = _declare("zebkit_automation_commands", _P)
_page_view = _declare("zebkit_automation_page", _V, _S)
_LISTEN = ctypes.CFUNCTYPE(None, _S, _S, _P)
_listen = _declare("zebkit_automation_listen", None, ctypes.c_int, _LISTEN, _P)
_free = _declare("zebkit_free", None, _P)
_PIXELS = ctypes.CFUNCTYPE(None, _V, _P, ctypes.c_int, ctypes.c_int, ctypes.c_int, _S, _P)
_screenshot_pixels = _declare("zebkit_automation_screenshot_pixels", None, _V, _S, _S, _PIXELS, _P)


class TimeoutError(Error):  # noqa: A001
    """A command's timeout passed: Playwright's TimeoutError."""


def commands():
    """The commands the engine has, by Playwright's protocol names."""
    pointer = _commands()
    try:
        return json.loads(ctypes.string_at(pointer).decode())
    finally:
        _free(pointer)


# ---- Futures and coroutines on the engine's thread ----------------------

class Future:
    """A result to come, awaitable in a coroutine run()."""

    def __init__(self):
        self._done = False
        self._result = None
        self._error = None
        self._callbacks = []

    def set_result(self, value):
        self._finish(value, None)

    def set_error(self, error):
        self._finish(None, error)

    def _finish(self, value, error):
        if self._done:
            return
        self._done, self._result, self._error = True, value, error
        callbacks, self._callbacks = self._callbacks, []
        for callback in callbacks:
            callback(self)

    def done(self):
        return self._done

    def result(self):
        if self._error is not None:
            raise self._error
        return self._result

    def add_done_callback(self, callback):
        if self._done:
            callback(self)
        else:
            self._callbacks.append(callback)

    def __await__(self):
        if not self._done:
            yield self
        return self.result()


def run(coroutine):
    """Runs @coroutine on the engine's thread; a Future of its value."""
    future = Future()

    def step(value=None, error=None):
        try:
            awaited = coroutine.throw(error) if error is not None else coroutine.send(value)
        except StopIteration as stop:
            future.set_result(stop.value)
            return
        except BaseException as e:  # noqa: BLE001
            future.set_error(e)
            return
        awaited.add_done_callback(lambda done: post(lambda: resume(done)))

    def resume(done):
        try:
            value = done.result()
        except BaseException as e:  # noqa: BLE001
            step(error=e)
            return
        step(value)

    post(step)
    return future


def sleep(ms):
    future = Future()
    post(lambda: future.set_result(None), ms)
    return future


# ---- Commands --------------------------------------------------------------

def _command(view, method, params=None):
    future = Future()

    def done(handle, result, error, data):
        _alive.discard(callback)
        if error is not None:
            message = error.decode()
            future.set_error((TimeoutError if message.startswith("Timeout ") else Error)(message))
        else:
            future.set_result(json.loads(result.decode()) if result else {})

    callback = _DONE(_guard(done))
    _alive.add(callback)
    clean = {k: v for k, v in (params or {}).items() if v is not None}
    _call(view._check() if view is not None else None, method.encode(), json.dumps(clean).encode(), callback, None)
    return future


class Pixels(tuple):
    """A screenshot's pixels, unencoded (zebkit's own, not Playwright's):
    @height rows of @stride bytes, 4 bytes a pixel, R, G, B, A, not
    premultiplied (the pixels of the PNG page.screenshot() gives)."""

    __slots__ = ()

    def __new__(cls, width, height, stride, data):
        return tuple.__new__(cls, (width, height, stride, data))

    width = property(lambda self: self[0])
    height = property(lambda self: self[1])
    stride = property(lambda self: self[2])
    data = property(lambda self: self[3])


def _pixels(view, method, params):
    future = Future()

    def done(handle, pixels, width, height, stride, error, data):
        _alive.discard(callback)
        if error is not None:
            message = error.decode()
            future.set_error((TimeoutError if message.startswith("Timeout ") else Error)(message))
        else:
            future.set_result(Pixels(width, height, stride, ctypes.string_at(pixels, stride * height)))

    callback = _PIXELS(_guard(done))
    _alive.add(callback)
    clean = {k: v for k, v in (params or {}).items() if v is not None}
    _screenshot_pixels(view._check(), method.encode(), json.dumps(clean).encode(), callback, None)
    return future


# Page.screenshot()'s options, as the protocol names them.
def _screenshot_options(options):
    names = {"full_page": "fullPage", "omit_background": "omitBackground", "mask_color": "maskColor"}
    result = {}
    for key, value in options.items():
        if value is None or key == "path":
            continue
        if key == "timeout":
            value = float(value)
        if key == "mask":
            value = [{"selector": locator._selector} for locator in value]
        result[names.get(key, key)] = value
    return result


def _write(path, data):
    if path:
        pathlib.Path(path).write_bytes(data)
    return data


def _handle_of(value):
    if isinstance(value, JSHandle):
        return {"handle": value._id}
    if isinstance(value, (list, tuple)):
        return [_handle_of(v) for v in value]
    if isinstance(value, dict):
        return {k: _handle_of(v) for k, v in value.items()}
    return value


def _ms(timeout):
    return None if timeout is None else float(timeout)


# ---- Selectors, as Playwright's locatorUtils.ts makes them -----------------

def _text(text, exact):
    if isinstance(text, re.Pattern):
        flags = "i" if text.flags & re.IGNORECASE else ""
        return "/" + text.pattern.replace("/", "\\/") + "/" + flags
    return json.dumps(text) + ("s" if exact else "i")


def _attribute(value, exact):
    if isinstance(value, re.Pattern):
        return _text(value, exact)
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"' + ("s" if exact else "i")


def _by_role(role, name=None, exact=False, checked=None, disabled=None, selected=None, expanded=None,
             include_hidden=None, level=None, pressed=None):
    props = []
    for key, value in (("checked", checked), ("disabled", disabled), ("selected", selected),
                       ("expanded", expanded), ("include-hidden", include_hidden), ("level", level)):
        if value is not None:
            props.append((key, str(value).lower() if isinstance(value, bool) else str(value)))
    if name is not None:
        props.append(("name", _attribute(name, exact)))
    if pressed is not None:
        props.append(("pressed", str(pressed).lower()))
    return "internal:role=" + role + "".join("[%s=%s]" % p for p in props)


class _Selectors:
    """get_by_*() and locator(), for pages, frames, locators."""

    def _selector_for(self, selector):
        raise NotImplementedError

    def locator(self, selector, has_text=None, has_not_text=None, has=None, has_not=None):
        return Locator(self._frame_ref(), self._selector_for(selector))._filter(has_text, has_not_text, has, has_not)

    def get_by_role(self, role, **options):
        return self.locator(_by_role(role, **options))

    def get_by_text(self, text, exact=False):
        return self.locator("internal:text=" + _text(text, exact))

    def get_by_label(self, text, exact=False):
        return self.locator("internal:label=" + _text(text, exact))

    def get_by_placeholder(self, text, exact=False):
        return self.locator("internal:attr=[placeholder=" + _attribute(text, exact) + "]")

    def get_by_alt_text(self, text, exact=False):
        return self.locator("internal:attr=[alt=" + _attribute(text, exact) + "]")

    def get_by_title(self, text, exact=False):
        return self.locator("internal:attr=[title=" + _attribute(text, exact) + "]")

    def get_by_test_id(self, test_id):
        return self.locator("internal:testid=[data-testid=" + _attribute(test_id, True) + "]")

    def frame_locator(self, selector):
        return FrameLocator(self._frame_ref(), self._selector_for(selector))


# ---- Frames and pages ------------------------------------------------------

class Frame(_Selectors):
    """A frame of a page: "frame" is its id, None for the main frame."""

    def __init__(self, page, frame_id=None, name="", url=""):
        self.page = page
        self._id = frame_id
        self.name = name
        self.url = url

    def _frame_ref(self):
        return self

    def _selector_for(self, selector):
        return selector

    def _do(self, method, **params):
        params["frame"] = self._id
        return _command(self.page._view, method, params)

    async def goto(self, url, wait_until=None, timeout=None, referer=None):
        answer = await self._do("Frame.goto", url=url, waitUntil=wait_until, timeout=_ms(timeout), referer=referer)
        return Response(answer.get("response")) if answer.get("response") else None

    async def evaluate(self, expression, arg=None):
        is_function = _looks_like_function(expression)
        answer = await self._do("Frame.evaluateExpression", expression=expression, isFunction=is_function,
                                arg=_handle_of(arg))
        return answer.get("value")

    async def evaluate_handle(self, expression, arg=None):
        answer = await self._do("Frame.evaluateExpressionHandle", expression=expression,
                                isFunction=_looks_like_function(expression), arg=_handle_of(arg))
        return _make_handle(self, answer.get("handle"))

    async def content(self):
        return (await self._do("Frame.content")).get("value")

    async def set_content(self, html, wait_until=None, timeout=None):
        await self._do("Frame.setContent", html=html, waitUntil=wait_until, timeout=_ms(timeout))

    async def title(self):
        return (await self._do("Frame.title")).get("value")

    async def wait_for_selector(self, selector, state=None, timeout=None, strict=None):
        answer = await self._do("Frame.waitForSelector", selector=selector, state=state, timeout=_ms(timeout),
                                strict=strict)
        return _make_handle(self, answer.get("element"))

    async def wait_for_function(self, expression, arg=None, polling=None, timeout=None):
        answer = await self._do("Frame.waitForFunction", expression=expression,
                                isFunction=_looks_like_function(expression), arg=_handle_of(arg),
                                pollingInterval=polling if isinstance(polling, (int, float)) else None,
                                timeout=_ms(timeout))
        return _make_handle(self, answer.get("handle"))

    async def wait_for_load_state(self, state=None, timeout=None):
        await self._do("Frame.waitForLoadState", state=state, timeout=_ms(timeout))

    async def wait_for_timeout(self, timeout):
        await self._do("Frame.waitForTimeout", waitTimeout=float(timeout))

    async def query_selector(self, selector):
        return _make_handle(self, (await self._do("Frame.querySelector", selector=selector)).get("element"))

    async def query_selector_all(self, selector):
        answer = await self._do("Frame.querySelectorAll", selector=selector)
        return [_make_handle(self, e) for e in answer.get("elements") or []]

    async def eval_on_selector(self, selector, expression, arg=None):
        answer = await self._do("Frame.evalOnSelector", selector=selector, expression=expression,
                                isFunction=_looks_like_function(expression), arg=_handle_of(arg), strict=True)
        return answer.get("value")

    async def eval_on_selector_all(self, selector, expression, arg=None):
        answer = await self._do("Frame.evalOnSelectorAll", selector=selector, expression=expression,
                                isFunction=_looks_like_function(expression), arg=_handle_of(arg))
        return answer.get("value")

    async def add_script_tag(self, url=None, path=None, content=None, type=None):  # noqa: A002
        if path:
            content = pathlib.Path(path).read_text()
        answer = await self._do("Frame.addScriptTag", url=url, content=content, type=type)
        return _make_handle(self, answer.get("element"))

    async def add_style_tag(self, url=None, path=None, content=None):
        if path:
            content = pathlib.Path(path).read_text()
        answer = await self._do("Frame.addStyleTag", url=url, content=content)
        return _make_handle(self, answer.get("element"))

    async def drag_and_drop(self, source, target, **options):
        await self._do("Frame.dragAndDrop", source=source, target=target, strict=True,
                       **_options(options))


def _looks_like_function(expression):
    text = expression.strip()
    return bool(re.match(r"^(async\s+)?(function\b|\(?[\w$\s,]*\)?\s*=>|\([^)]*\)\s*=>)", text)) or None


def _options(options):
    names = {"no_wait_after": "noWaitAfter", "click_count": "clickCount", "source_position": "sourcePosition",
             "target_position": "targetPosition"}
    result = {}
    for key, value in options.items():
        if value is None:
            continue
        if key == "timeout":
            value = float(value)
        result[names.get(key, key)] = value
    return result


class Keyboard:
    def __init__(self, page):
        self._page = page

    async def down(self, key):
        await _command(self._page._view, "Page.keyboardDown", {"key": key})

    async def up(self, key):
        await _command(self._page._view, "Page.keyboardUp", {"key": key})

    async def press(self, key, delay=None):
        await _command(self._page._view, "Page.keyboardPress", {"key": key, "delay": delay})

    async def type(self, text, delay=None):  # noqa: A003
        await _command(self._page._view, "Page.keyboardType", {"text": text, "delay": delay})

    async def insert_text(self, text):
        await _command(self._page._view, "Page.keyboardInsertText", {"text": text})


class Mouse:
    def __init__(self, page):
        self._page = page

    async def move(self, x, y, steps=None):
        await _command(self._page._view, "Page.mouseMove", {"x": x, "y": y, "steps": steps})

    async def down(self, button=None, click_count=None):
        await _command(self._page._view, "Page.mouseDown", {"button": button, "clickCount": click_count})

    async def up(self, button=None, click_count=None):
        await _command(self._page._view, "Page.mouseUp", {"button": button, "clickCount": click_count})

    async def click(self, x, y, button=None, click_count=None, delay=None):
        await _command(self._page._view, "Page.mouseClick",
                       {"x": x, "y": y, "button": button, "clickCount": click_count, "delay": delay})

    async def dblclick(self, x, y, button=None, delay=None):
        await self.click(x, y, button=button, click_count=2, delay=delay)

    async def wheel(self, delta_x, delta_y):
        await _command(self._page._view, "Page.mouseWheel", {"deltaX": delta_x, "deltaY": delta_y})


class Page(Frame):
    """A page: a zebkit view (off-screen unless @view is given) and its main
    frame, with Playwright's Page API."""

    def __init__(self, view=None, width=1280, height=720):
        self._view = view or View(None, width, height)
        super().__init__(self, None)
        self.main_frame = self
        self.keyboard = Keyboard(self)
        self.mouse = Mouse(self)
        self._listeners = {}
        self._routes = []
        self._workers = {}
        self.touchscreen = Touchscreen(self)
        self.tracing = Tracing(self)
        self._event_callback = _EVENT(_guard(self._event))
        _alive.add(self._event_callback)
        _on_event(self._view._check(), self._event_callback, None)

    @property
    def view(self):
        return self._view

    # Events: Playwright's names for Python.
    _EVENTS = {
        "BrowserContext.console": "console", "BrowserContext.dialog": "dialog",
        "Page.fileChooser": "filechooser", "Page.download": "download", "Page.request": "request",
        "Page.response": "response", "Page.requestFinished": "requestfinished",
        "Page.requestFailed": "requestfailed", "Frame.navigated": "framenavigated",
        "Page.frameAttached": "frameattached", "Page.frameDetached": "framedetached",
        "Page.crash": "crash", "Page.close": "close", "Frame.loadstate": "loadstate",
        "Page.worker": "worker",
    }

    def _event(self, view, event, params, data):
        name = event.decode()
        value = json.loads(params.decode()) if params else {}
        if name == "BrowserContext.route":
            self._route(value)
            return
        if name == "BrowserContext.bindingCall":
            self._binding_call(value)
            return
        if name == "Worker.close":
            worker = self._workers.pop(value.get("worker"), None)
            if worker:
                for listener in list(worker._listeners):
                    listener(worker)
            return
        kind = self._EVENTS.get(name)
        if not kind:
            return
        wrapped = {
            "dialog": lambda v: Dialog(self, v), "filechooser": lambda v: FileChooser(self, v),
            "download": lambda v: Download(self, v), "request": Request, "response": lambda v: Response(v, self),
            "requestfinished": Request, "requestfailed": Request,
            "console": lambda v: ConsoleMessage(v), "worker": self._worker,
        }.get(kind, lambda v: v)(value)
        for listener in list(self._listeners.get(kind, [])):
            listener(wrapped)

    def on(self, event, listener):
        """Calls listener(value) for each @event: dialog, filechooser,
        download, request, response, requestfinished, requestfailed,
        console, framenavigated, frameattached, framedetached, close."""
        listeners = self._listeners.setdefault(event, [])
        listeners.append(listener)
        if event in ("dialog", "filechooser") and len(listeners) == 1:
            # At once: commands run in the order they are given.
            _command(self._view, "Page.updateSubscription",
                     {"event": "dialog" if event == "dialog" else "fileChooser", "enabled": True})

    def remove_listener(self, event, listener):
        listeners = self._listeners.get(event, [])
        if listener in listeners:
            listeners.remove(listener)

    def wait_for_event(self, event, predicate=None):
        future = Future()

        def listener(value):
            if predicate is None or predicate(value):
                self.remove_listener(event, listener)
                future.set_result(value)
        self.on(event, listener)
        return future

    async def route(self, url, handler):
        """Calls handler(route, request) for requests to @url (a glob or a
        compiled regular expression)."""
        if isinstance(url, re.Pattern):
            pattern = {"regexSource": url.pattern, "regexFlags": "i" if url.flags & re.IGNORECASE else ""}
            matches = url.search
        else:
            pattern = {"glob": url}
            regex = re.compile(_glob_to_regex(url))
            matches = regex.match
        self._routes.append((pattern, matches, handler))
        await _command(self._view, "Page.setNetworkInterceptionPatterns", {"patterns": [r[0] for r in self._routes]})

    async def unroute(self, url):
        self._routes = [r for r in self._routes if r[0].get("glob") != url and r[0].get("regexSource") != getattr(url, "pattern", None)]
        await _command(self._view, "Page.setNetworkInterceptionPatterns", {"patterns": [r[0] for r in self._routes]})

    def _route(self, value):
        request = Request(value["request"])
        route = Route(self, value["route"], request)
        for _, matches, handler in reversed(self._routes):
            if matches(request.url):
                result = handler(route, request)
                if hasattr(result, "send"):
                    run(result)
                return
        route.continue_()

    async def expose_function(self, name, function):
        await self.expose_binding(name, lambda source, *args: function(*args))

    async def expose_binding(self, name, function):
        self._bindings = getattr(self, "_bindings", {})
        self._bindings[name] = function
        await _command(self._view, "Page.exposeBinding", {"name": name})

    def _binding_call(self, value):
        function = getattr(self, "_bindings", {}).get(value["name"])

        def answer(result=None, error=None):
            if error is None:
                _command(self._view, "BindingCall.resolve", {"binding": value["binding"], "result": result})
            else:
                _command(self._view, "BindingCall.reject", {"binding": value["binding"], "error": str(error)})
        try:
            result = function({"page": self, "frame": value.get("frame")}, *value.get("args", []))
        except Exception as e:  # noqa: BLE001
            answer(error=e)
            return
        if hasattr(result, "send"):
            run(result).add_done_callback(
                lambda done: answer(error=done._error) if done._error else answer(done._result))
        else:
            answer(result)

    async def frames(self):
        answer = await _command(self._view, "Page.frames")
        return [Frame(self, f["frame"], f.get("name", ""), f.get("url", "")) if f.get("parentFrame") else self
                for f in answer.get("frames", [])]

    def frame(self, name=None, url=None):
        """The frame named @name, or whose URL has @url, as a Frame of this
        page; its id is found when it is used."""
        return _LazyFrame(self, name, url)

    async def reload(self, wait_until=None, timeout=None):
        answer = await _command(self._view, "Page.reload", {"waitUntil": wait_until, "timeout": _ms(timeout)})
        return Response(answer["response"]) if answer.get("response") else None

    async def go_back(self, wait_until=None, timeout=None):
        answer = await _command(self._view, "Page.goBack", {"waitUntil": wait_until, "timeout": _ms(timeout)})
        return Response(answer["response"]) if answer.get("response") else None

    async def go_forward(self, wait_until=None, timeout=None):
        answer = await _command(self._view, "Page.goForward", {"waitUntil": wait_until, "timeout": _ms(timeout)})
        return Response(answer["response"]) if answer.get("response") else None

    async def screenshot(self, path=None, **options):
        """Playwright's options: type, quality, full_page, clip, omit_background,
        animations, caret, scale, mask (locators), mask_color, style, timeout."""
        answer = await _command(self._view, "Page.screenshot", _screenshot_options(options))
        return _write(path, base64.b64decode(answer["binary"]))

    async def screenshot_pixels(self, **options):
        """zebkit's own: the screenshot's pixels unencoded, as Pixels (RGBA);
        screenshot()'s options but type, quality and path."""
        return await _pixels(self._view, "Page.screenshot", _screenshot_options(options))

    async def pdf(self, path=None, **options):
        answer = await _command(self._view, "Page.pdf", options)
        data = base64.b64decode(answer["pdf"])
        if path:
            pathlib.Path(path).write_bytes(data)
        return data

    async def set_viewport_size(self, viewport_size):
        await _command(self._view, "Page.setViewportSize", {"viewportSize": viewport_size})

    async def emulate_media(self, media=None, color_scheme=None, reduced_motion=None, forced_colors=None,
                            contrast=None):
        await _command(self._view, "Page.emulateMedia",
                       {"media": media, "colorScheme": color_scheme, "reducedMotion": reduced_motion,
                        "forcedColors": forced_colors, "contrast": contrast})

    async def add_init_script(self, script=None, path=None):
        source = pathlib.Path(path).read_text() if path else script
        await _command(self._view, "Page.addInitScript", {"source": source})

    async def set_extra_http_headers(self, headers):
        await _command(self._view, "Page.setExtraHTTPHeaders",
                       {"headers": [{"name": k, "value": v} for k, v in headers.items()]})

    async def bring_to_front(self):
        await _command(self._view, "Page.bringToFront")

    async def close(self):
        await _command(self._view, "Page.close")

    def _worker(self, value):
        worker = Worker(self, value)
        self._workers[worker._id] = worker
        return worker

    @property
    def workers(self):
        return list(self._workers.values())

    # Video (recordVideo): WebM, VP8.
    async def start_video(self, path=None, size=None):
        """Records the page to @path (a temporary .webm when None); answers
        the path."""
        answer = await _command(self._view, "Page.startVideo", {"path": str(path) if path else None, "size": size})
        return answer.get("path")

    async def stop_video(self):
        answer = await _command(self._view, "Page.stopVideo")
        return answer.get("path")

    async def set_geolocation(self, geolocation):
        await _command(self._view, "BrowserContext.setGeolocation", {"geolocation": geolocation})

    async def set_http_credentials(self, http_credentials):
        await _command(self._view, "BrowserContext.setHTTPCredentials", {"httpCredentials": http_credentials})

    # BrowserContext's, for the page's profile.
    async def cookies(self, urls=None):
        answer = await _command(self._view, "BrowserContext.cookies",
                                {"urls": [urls] if isinstance(urls, str) else urls})
        return answer.get("cookies", [])

    async def add_cookies(self, cookies):
        await _command(self._view, "BrowserContext.addCookies", {"cookies": cookies})

    async def clear_cookies(self, name=None, domain=None, path=None):
        await _command(self._view, "BrowserContext.clearCookies", {"name": name, "domain": domain, "path": path})

    async def storage_state(self, path=None):
        state = await _command(self._view, "BrowserContext.storageState")
        if path:
            pathlib.Path(path).write_text(json.dumps(state))
        return state

    async def set_storage_state(self, state):
        if not isinstance(state, dict):
            state = json.loads(pathlib.Path(state).read_text())
        await _command(self._view, "BrowserContext.setStorageState", {"storageState": state})

    async def grant_permissions(self, permissions, origin=None):
        await _command(self._view, "BrowserContext.grantPermissions", {"permissions": permissions, "origin": origin})

    async def clear_permissions(self):
        await _command(self._view, "BrowserContext.clearPermissions")

    async def set_offline(self, offline):
        await _command(self._view, "BrowserContext.setOffline", {"offline": offline})

    @property
    def clock(self):
        return Clock(self)

    @property
    def request(self):
        return APIRequestContext(self)


class _LazyFrame(Frame):
    def __init__(self, page, name, url):
        super().__init__(page, None)
        self._want_name, self._want_url = name, url

    async def _resolve(self):
        answer = await _command(self.page._view, "Page.frames")
        for frame in answer.get("frames", []):
            if (self._want_name and frame.get("name") == self._want_name) or \
                    (self._want_url and self._want_url in frame.get("url", "")):
                self._id = frame["frame"]
                return
        raise Error("no frame %s" % (self._want_name or self._want_url))

    def _do(self, method, **params):
        future = Future()

        async def go():
            if self._id is None:
                await self._resolve()
            return await Frame._do(self, method, **params)
        run(go()).add_done_callback(lambda done: future._finish(done._result, done._error))
        return future


# ---- Locators --------------------------------------------------------------

class Locator(_Selectors):
    """Playwright's Locator: a selector in a frame, resolved at each use."""

    def __init__(self, frame, selector):
        self._frame = frame
        self._selector = selector

    def _frame_ref(self):
        return self._frame

    def _selector_for(self, selector):
        if isinstance(selector, Locator):
            return self._selector + " >> internal:chain=" + json.dumps(selector._selector)
        return self._selector + " >> " + selector

    def _filter(self, has_text=None, has_not_text=None, has=None, has_not=None, visible=None):
        selector = self._selector
        if has_text is not None:
            selector += " >> internal:has-text=" + _text(has_text, False)
        if has_not_text is not None:
            selector += " >> internal:has-not-text=" + _text(has_not_text, False)
        if has is not None:
            selector += " >> internal:has=" + json.dumps(has._selector)
        if has_not is not None:
            selector += " >> internal:has-not=" + json.dumps(has_not._selector)
        if visible is not None:
            selector += " >> visible=" + ("true" if visible else "false")
        return Locator(self._frame, selector)

    def filter(self, has_text=None, has_not_text=None, has=None, has_not=None, visible=None):  # noqa: A003
        return self._filter(has_text, has_not_text, has, has_not, visible)

    @property
    def first(self):
        return Locator(self._frame, self._selector + " >> nth=0")

    @property
    def last(self):
        return Locator(self._frame, self._selector + " >> nth=-1")

    def nth(self, index):
        return Locator(self._frame, self._selector + " >> nth=%d" % index)

    def and_(self, locator):
        return Locator(self._frame, self._selector + " >> internal:and=" + json.dumps(locator._selector))

    def or_(self, locator):
        return Locator(self._frame, self._selector + " >> internal:or=" + json.dumps(locator._selector))

    def _do(self, method, strict=True, **params):
        return self._frame._do(method, selector=self._selector, strict=strict, **params)

    async def click(self, **options):
        await self._do("Frame.click", **_options(options))

    async def dblclick(self, **options):
        await self._do("Frame.dblclick", **_options(options))

    async def hover(self, **options):
        await self._do("Frame.hover", **_options(options))

    async def tap(self, **options):
        await self._do("Frame.tap", **_options(options))

    async def check(self, **options):
        await self._do("Frame.check", **_options(options))

    async def uncheck(self, **options):
        await self._do("Frame.uncheck", **_options(options))

    async def set_checked(self, checked, **options):
        await (self.check(**options) if checked else self.uncheck(**options))

    async def fill(self, value, **options):
        await self._do("Frame.fill", value=value, **_options(options))

    async def clear(self, **options):
        await self.fill("", **options)

    async def press(self, key, **options):
        await self._do("Frame.press", key=key, **_options(options))

    async def press_sequentially(self, text, **options):
        await self._do("Frame.type", text=text, **_options(options))

    type = press_sequentially  # noqa: A003

    async def focus(self, timeout=None):
        await self._do("Frame.focus", timeout=_ms(timeout))

    async def blur(self, timeout=None):
        await self._do("Frame.blur", timeout=_ms(timeout))

    async def select_option(self, value=None, index=None, label=None, element=None, **options):
        def listed(v):
            return v if isinstance(v, (list, tuple)) else ([] if v is None else [v])
        chosen = [{"valueOrLabel": v} for v in listed(value)] + [{"index": i} for i in listed(index)] + \
            [{"label": v} for v in listed(label)]
        answer = await self._do("Frame.selectOption", options=chosen,
                                elements=[{"handle": e._id} for e in listed(element)], **_options(options))
        return answer.get("values", [])

    async def set_input_files(self, files, **options):
        items = files if isinstance(files, (list, tuple)) else [files]
        if all(isinstance(f, dict) for f in items):
            payloads = [{"name": f["name"], "mimeType": f.get("mimeType", "application/octet-stream"),
                         "buffer": base64.b64encode(f["buffer"]).decode()} for f in items]
            await self._do("Frame.setInputFiles", payloads=payloads, **_options(options))
        else:
            await self._do("Frame.setInputFiles", localPaths=[str(pathlib.Path(f).resolve()) for f in items],
                           **_options(options))

    async def dispatch_event(self, type, event_init=None, timeout=None):  # noqa: A002
        await self._do("Frame.dispatchEvent", type=type, eventInit=event_init, timeout=_ms(timeout))

    async def drag_to(self, target, **options):
        await self._frame._do("Frame.dragAndDrop", source=self._selector, target=target._selector, strict=True,
                              **_options(options))

    async def text_content(self, timeout=None):
        return (await self._do("Frame.textContent", timeout=_ms(timeout))).get("value")

    async def inner_text(self, timeout=None):
        return (await self._do("Frame.innerText", timeout=_ms(timeout))).get("value")

    async def inner_html(self, timeout=None):
        return (await self._do("Frame.innerHTML", timeout=_ms(timeout))).get("value")

    async def get_attribute(self, name, timeout=None):
        return (await self._do("Frame.getAttribute", name=name, timeout=_ms(timeout))).get("value")

    async def input_value(self, timeout=None):
        return (await self._do("Frame.inputValue", timeout=_ms(timeout))).get("value")

    async def is_visible(self):
        return (await self._do("Frame.isVisible")).get("value")

    async def is_hidden(self):
        return (await self._do("Frame.isHidden")).get("value")

    async def is_enabled(self, timeout=None):
        return (await self._do("Frame.isEnabled", timeout=_ms(timeout))).get("value")

    async def is_disabled(self, timeout=None):
        return (await self._do("Frame.isDisabled", timeout=_ms(timeout))).get("value")

    async def is_checked(self, timeout=None):
        return (await self._do("Frame.isChecked", timeout=_ms(timeout))).get("value")

    async def is_editable(self, timeout=None):
        return (await self._do("Frame.isEditable", timeout=_ms(timeout))).get("value")

    async def count(self):
        return (await self._do("Frame.queryCount", strict=False)).get("value")

    async def all(self):
        return [self.nth(i) for i in range(await self.count())]

    async def all_text_contents(self):
        return await self.evaluate_all("es => es.map(e => e.textContent || '')")

    async def all_inner_texts(self):
        return await self.evaluate_all("es => es.map(e => e.innerText)")

    async def evaluate(self, expression, arg=None, timeout=None):
        element = await self.element_handle(timeout=timeout)
        try:
            return await element.evaluate(expression, arg)
        finally:
            await element.dispose()

    async def evaluate_all(self, expression, arg=None):
        answer = await self._frame._do("Frame.evalOnSelectorAll", selector=self._selector, expression=expression,
                                       isFunction=True, arg=_handle_of(arg))
        return answer.get("value")

    async def element_handle(self, timeout=None):
        answer = await self._do("Frame.waitForSelector", state="attached", timeout=_ms(timeout))
        return _make_handle(self._frame, answer.get("element"))

    async def element_handles(self):
        return await self._frame.query_selector_all(self._selector)

    async def wait_for(self, state=None, timeout=None):
        await self._do("Frame.waitForSelector", state=state, timeout=_ms(timeout), omitReturnValue=True)

    async def aria_snapshot(self, timeout=None):
        return (await self._do("Frame.ariaSnapshot", timeout=_ms(timeout))).get("snapshot")

    async def bounding_box(self, timeout=None):
        element = await self.element_handle(timeout=timeout)
        try:
            return await element.bounding_box()
        finally:
            await element.dispose()

    async def screenshot(self, path=None, timeout=None, **options):
        element = await self.element_handle(timeout=timeout)
        try:
            return await element.screenshot(path=path, timeout=timeout, **options)
        finally:
            await element.dispose()

    async def screenshot_pixels(self, timeout=None, **options):
        element = await self.element_handle(timeout=timeout)
        try:
            return await element.screenshot_pixels(timeout=timeout, **options)
        finally:
            await element.dispose()

    async def scroll_into_view_if_needed(self, timeout=None):
        element = await self.element_handle(timeout=timeout)
        try:
            await element.scroll_into_view_if_needed()
        finally:
            await element.dispose()


class FrameLocator(_Selectors):
    """Playwright's FrameLocator: the frame of an <iframe> selector."""

    def __init__(self, frame, selector):
        self._frame = frame
        self._selector = selector

    def _frame_ref(self):
        return self._frame

    def _selector_for(self, selector):
        return self._selector + " >> internal:control=enter-frame >> " + selector

    @property
    def owner(self):
        return Locator(self._frame, self._selector)


# ---- Handles ---------------------------------------------------------------

def _make_handle(frame, value):
    if not value or not isinstance(value, dict) or "handle" not in value:
        return None
    kind = ElementHandle if value.get("element") else JSHandle
    return kind(frame, value["handle"], value.get("preview", ""))


class JSHandle:
    def __init__(self, frame, handle_id, preview=""):
        self._frame = frame
        self._id = handle_id
        self._preview = preview

    def __repr__(self):
        return self._preview or "JSHandle"

    def _do(self, method, **params):
        params["handle"] = self._id
        return _command(self._frame.page._view, method, params)

    async def evaluate(self, expression, arg=None):
        answer = await self._do("JSHandle.evaluateExpression", expression=expression,
                                isFunction=_looks_like_function(expression), arg=_handle_of(arg))
        return answer.get("value")

    async def evaluate_handle(self, expression, arg=None):
        answer = await self._do("JSHandle.evaluateExpressionHandle", expression=expression,
                                isFunction=_looks_like_function(expression), arg=_handle_of(arg))
        return _make_handle(self._frame, answer.get("handle"))

    async def get_property(self, name):
        return _make_handle(self._frame, (await self._do("JSHandle.getProperty", name=name)).get("handle"))

    async def get_properties(self):
        answer = await self._do("JSHandle.getPropertyList")
        return {p["name"]: _make_handle(self._frame, p["value"]) for p in answer.get("properties", [])}

    async def json_value(self):
        return (await self._do("JSHandle.jsonValue")).get("value")

    async def dispose(self):
        await self._do("JSHandle.dispose")

    def as_element(self):
        return None


class ElementHandle(JSHandle):
    def as_element(self):
        return self

    def _do(self, method, **params):
        return JSHandle._do(self, method.replace("JSHandle.", "ElementHandle."), **params)

    async def _act(self, name, **params):
        return await self._do("ElementHandle." + name, **params)

    async def click(self, **options):
        await self._act("click", **_options(options))

    async def dblclick(self, **options):
        await self._act("dblclick", **_options(options))

    async def hover(self, **options):
        await self._act("hover", **_options(options))

    async def tap(self, **options):
        await self._act("tap", **_options(options))

    async def check(self, **options):
        await self._act("check", **_options(options))

    async def uncheck(self, **options):
        await self._act("uncheck", **_options(options))

    async def fill(self, value, **options):
        await self._act("fill", value=value, **_options(options))

    async def press(self, key, **options):
        await self._act("press", key=key, **_options(options))

    async def type(self, text, **options):  # noqa: A003
        await self._act("type", text=text, **_options(options))

    async def focus(self):
        await self._act("focus")

    async def select_text(self, **options):
        await self._act("selectText", **_options(options))

    async def scroll_into_view_if_needed(self, timeout=None):
        await self._act("scrollIntoViewIfNeeded", timeout=_ms(timeout))

    async def text_content(self):
        return (await self._act("textContent")).get("value")

    async def inner_text(self):
        return (await self._act("innerText")).get("value")

    async def inner_html(self):
        return (await self._act("innerHTML")).get("value")

    async def get_attribute(self, name):
        return (await self._act("getAttribute", name=name)).get("value")

    async def input_value(self):
        return (await self._act("inputValue")).get("value")

    async def is_visible(self):
        return (await self._act("isVisible")).get("value")

    async def is_hidden(self):
        return (await self._act("isHidden")).get("value")

    async def is_enabled(self):
        return (await self._act("isEnabled")).get("value")

    async def is_checked(self):
        return (await self._act("isChecked")).get("value")

    async def bounding_box(self):
        return (await self._act("boundingBox")).get("value")

    async def screenshot(self, path=None, **options):
        answer = await self._act("screenshot", **_screenshot_options(options))
        return _write(path, base64.b64decode(answer["binary"]))

    async def screenshot_pixels(self, **options):
        params = _screenshot_options(options)
        params["handle"] = self._id
        return await _pixels(self._frame.page._view, "ElementHandle.screenshot", params)

    async def query_selector(self, selector):
        return _make_handle(self._frame, (await self._act("querySelector", selector=selector)).get("element"))

    async def query_selector_all(self, selector):
        answer = await self._act("querySelectorAll", selector=selector)
        return [_make_handle(self._frame, e) for e in answer.get("elements") or []]

    async def wait_for_element_state(self, state, timeout=None):
        await self._act("waitForElementState", state=state, timeout=_ms(timeout))

    async def content_frame(self):
        answer = await self._act("contentFrame")
        return Frame(self._frame.page, answer["frame"]) if answer.get("frame") else None

    async def set_input_files(self, files, **options):
        items = files if isinstance(files, (list, tuple)) else [files]
        await self._act("setInputFiles", localPaths=[str(pathlib.Path(f).resolve()) for f in items],
                        **_options(options))


# ---- Network, dialogs, files ----------------------------------------------

def _glob_to_regex(glob):
    """Playwright's globToRegexPattern(), for matching here what the engine
    matched."""
    out, in_group, i = ["^"], False, 0
    escaped = set("$^+.*()|\\?{}[]")
    while i < len(glob):
        c = glob[i]
        if c == "\\" and i + 1 < len(glob):
            i += 1
            out.append(("\\" if glob[i] in escaped else "") + glob[i])
        elif c == "*":
            before = glob[i - 1] if i else ""
            stars = 1
            while i + 1 < len(glob) and glob[i + 1] == "*":
                stars += 1
                i += 1
            if stars > 1:
                if i + 1 < len(glob) and glob[i + 1] == "/":
                    out.append("((.+/)|)" if before == "/" else "(.*/)")
                    i += 1
                else:
                    out.append("(.*)")
            else:
                out.append("([^/]*)")
        elif c == "{":
            in_group = True
            out.append("(")
        elif c == "}":
            in_group = False
            out.append(")")
        elif c == ",":
            out.append("|" if in_group else "\\,")
        else:
            out.append(("\\" if c in escaped else "") + c)
        i += 1
    return "".join(out) + "$"


class Request:
    def __init__(self, value):
        self._value = value
        self.url = value.get("url")
        self.method = value.get("method")
        self.resource_type = value.get("resourceType")
        self.headers = {h["name"].lower(): h["value"] for h in value.get("headers", [])}
        self.failure = value.get("failureText")

    def is_navigation_request(self):
        return bool(self._value.get("isNavigationRequest"))

    def __repr__(self):
        return "<Request %s %s>" % (self.method, self.url)


class Response:
    def __init__(self, value, page=None):
        self._value = value
        self._page = page
        self.url = value.get("url")
        self.status = value.get("status")
        self.status_text = value.get("statusText", "")
        self.headers = {h["name"].lower(): h["value"] for h in value.get("headers", [])}
        self.ok = self.status is not None and (self.status == 0 or 200 <= self.status <= 299)

    async def body(self):
        answer = await _command(self._page._view, "Response.body", {"request": self._value.get("request")})
        return base64.b64decode(answer["binary"])

    async def text(self):
        return (await self.body()).decode()

    async def json(self):
        return json.loads(await self.body())

    def __repr__(self):
        return "<Response %s %s>" % (self.status, self.url)


class Route:
    def __init__(self, page, route_id, request):
        self._page = page
        self._id = route_id
        self.request = request

    def continue_(self, url=None, method=None, headers=None, post_data=None):
        if isinstance(post_data, str):
            post_data = post_data.encode()
        return _command(self._page._view, "Route.continue", {
            "route": self._id, "url": url, "method": method,
            "headers": [{"name": k, "value": v} for k, v in headers.items()] if headers else None,
            "postData": base64.b64encode(post_data).decode() if post_data is not None else None})

    def fulfill(self, status=200, headers=None, body=None, content_type=None, json_body=None, path=None):
        if json_body is not None:
            body, content_type = json.dumps(json_body), content_type or "application/json"
        if path:
            body = pathlib.Path(path).read_bytes()
        data = body.encode() if isinstance(body, str) else (body or b"")
        listed = [{"name": k, "value": v} for k, v in (headers or {}).items()]
        if content_type:
            listed.append({"name": "content-type", "value": content_type})
        return _command(self._page._view, "Route.fulfill", {
            "route": self._id, "status": status, "headers": listed,
            "body": base64.b64encode(data).decode(), "isBase64": True})

    def abort(self, error_code="failed"):
        return _command(self._page._view, "Route.abort", {"route": self._id, "errorCode": error_code})


class Dialog:
    def __init__(self, page, value):
        self._page = page
        self._id = value["dialog"]
        self.type = value["type"]
        self.message = value["message"]
        self.default_value = value.get("defaultValue", "")

    def accept(self, prompt_text=None):
        return _command(self._page._view, "Dialog.accept", {"dialog": self._id, "promptText": prompt_text})

    def dismiss(self):
        return _command(self._page._view, "Dialog.dismiss", {"dialog": self._id})


class FileChooser:
    def __init__(self, page, value):
        self.page = page
        self._id = value["fileChooser"]
        self._multiple = value.get("isMultiple", False)

    def is_multiple(self):
        return self._multiple

    def set_files(self, files):
        items = files if isinstance(files, (list, tuple)) else [files]
        return _command(self.page._view, "FileChooser.setFiles",
                        {"fileChooser": self._id, "localPaths": [str(pathlib.Path(f).resolve()) for f in items]})


class Download:
    def __init__(self, page, value):
        self.page = page
        self._id = value["download"]
        self.url = value["url"]
        self.suggested_filename = value["suggestedFilename"]

    async def path(self):
        return (await _command(self.page._view, "Artifact.pathAfterFinished", {"download": self._id})).get("value")

    async def save_as(self, path):
        await _command(self.page._view, "Artifact.saveAs", {"download": self._id, "path": str(path)})

    async def failure(self):
        return (await _command(self.page._view, "Artifact.failure", {"download": self._id})).get("error")

    async def cancel(self):
        await _command(self.page._view, "Artifact.cancel", {"download": self._id})

    async def delete(self):
        await _command(self.page._view, "Artifact.delete", {"download": self._id})


class ConsoleMessage:
    def __init__(self, value):
        self.type = value.get("type")
        self.text = value.get("text")
        self.location = value.get("location")

    def __repr__(self):
        return "<ConsoleMessage %s %s>" % (self.type, self.text)


class Touchscreen:
    def __init__(self, page):
        self._page = page

    async def tap(self, x, y):
        await _command(self._page._view, "Page.touchscreenTap", {"x": x, "y": y})


class Worker:
    """A dedicated worker of the page (page.workers, the "worker" event)."""

    def __init__(self, page, value):
        self._page = page
        self._id = value.get("worker")
        self.url = value.get("url")
        self._listeners = []

    async def evaluate(self, expression, arg=None):
        answer = await _command(self._page._view, "Worker.evaluateExpression",
                                {"worker": self._id, "expression": expression, "arg": arg,
                                 "isFunction": True if _looks_like_function(expression) else None})
        return answer.get("value")

    def on(self, event, listener):
        """"close": listener(worker) once the worker ends."""
        if event == "close":
            self._listeners.append(listener)

    def __repr__(self):
        return "<Worker %s>" % self.url


class Tracing:
    """page.tracing: Playwright's traces (the zip its trace viewer opens) and
    HAR files of the page."""

    def __init__(self, page):
        self._page = page

    async def start(self, name=None, title=None, screenshots=None, snapshots=None):
        await _command(self._page._view, "Tracing.tracingStart",
                       {"name": name, "screenshots": screenshots, "snapshots": snapshots})
        await _command(self._page._view, "Tracing.tracingStartChunk", {"title": title})

    async def start_chunk(self, title=None):
        await _command(self._page._view, "Tracing.tracingStartChunk", {"title": title})

    async def stop_chunk(self, path=None):
        if path is None:
            await _command(self._page._view, "Tracing.tracingStopChunk", {"mode": "discard"})
            return None
        answer = await _command(self._page._view, "Tracing.tracingStopChunk", {"mode": "archive", "path": str(path)})
        return answer.get("path")

    async def stop(self, path=None):
        written = await self.stop_chunk(path)
        await _command(self._page._view, "Tracing.tracingStop")
        return written

    async def start_har(self, path=None, content=None, mode=None, url_filter=None):
        answer = await _command(self._page._view, "Tracing.harStart",
                                {"options": {k: v for k, v in {"content": content, "mode": mode,
                                                               "urlGlob": url_filter}.items() if v is not None},
                                 "path": str(path) if path else None})
        return answer.get("harId")

    async def export_har(self, har_id, path=None):
        answer = await _command(self._page._view, "Tracing.harExport",
                                {"harId": har_id, "path": str(path) if path else None})
        return answer.get("path")


def _adopt_view(handle):
    """A View for a view the engine made (a context's page)."""
    view = View.__new__(View)
    view._handle = handle
    view._callbacks = {}
    view._on_close_user = None
    view._closed = False
    view._set("close", _VIEW(_guard(view._closed_by_engine)), _view_on_close)
    _alive.add(view)
    return view


class BrowserContext:
    """A context of its own (new_context()): a profile of its own, cookies,
    storage, permissions and cache, and its pages."""

    def __init__(self, context_id):
        self._id = context_id
        self.pages = []

    def _do(self, method, **params):
        return _command(None, method, {"context": self._id, **params})

    async def new_page(self):
        answer = await self._do("BrowserContext.newPage")
        page = Page(_adopt_view(_page_view(answer["page"].encode())))
        page.context = self
        self.pages.append(page)
        return page

    async def cookies(self, urls=None):
        answer = await self._do("BrowserContext.cookies", urls=[urls] if isinstance(urls, str) else urls)
        return answer.get("cookies", [])

    async def add_cookies(self, cookies):
        await self._do("BrowserContext.addCookies", cookies=cookies)

    async def clear_cookies(self, name=None, domain=None, path=None):
        await self._do("BrowserContext.clearCookies", name=name, domain=domain, path=path)

    async def grant_permissions(self, permissions, origin=None):
        await self._do("BrowserContext.grantPermissions", permissions=permissions, origin=origin)

    async def clear_permissions(self):
        await self._do("BrowserContext.clearPermissions")

    async def set_geolocation(self, geolocation):
        await self._do("BrowserContext.setGeolocation", geolocation=geolocation)

    async def set_http_credentials(self, http_credentials):
        await self._do("BrowserContext.setHTTPCredentials", httpCredentials=http_credentials)

    async def set_offline(self, offline):
        await self._do("BrowserContext.setOffline", offline=offline)

    async def set_extra_http_headers(self, headers):
        await self._do("BrowserContext.setExtraHTTPHeaders",
                       headers=[{"name": k, "value": v} for k, v in headers.items()])

    async def storage_state(self, path=None):
        state = await self._do("BrowserContext.storageState")
        if path:
            pathlib.Path(path).write_text(json.dumps(state))
        return state

    async def close(self):
        await self._do("BrowserContext.close")
        self.pages = []


async def new_context(**options):
    """browser.new_context(): Playwright's options (viewport, user_agent,
    locale, geolocation, permissions, http_credentials, offline,
    extra_http_headers, color_scheme, has_touch, java_script_enabled,
    storage_state, ...), named in Python or in Playwright's camelCase."""
    def camel(name):
        head, *rest = name.split("_")
        return head + "".join(part.capitalize() for part in rest)
    acronyms = {"extraHttpHeaders": "extraHTTPHeaders", "ignoreHttpsErrors": "ignoreHTTPSErrors"}
    params = {acronyms.get(camel(k), camel(k)): v for k, v in options.items() if v is not None}
    if isinstance(params.get("extraHTTPHeaders"), dict):
        params["extraHTTPHeaders"] = [{"name": k, "value": v} for k, v in params["extraHTTPHeaders"].items()]
    answer = await _command(None, "Browser.newContext", params)
    return BrowserContext(answer["context"])


def listen(port=0):
    """Serves Playwright's own protocol on 127.0.0.1:@port (0 picks one):
    chromium.connect(url) of any Playwright client drives this engine. A
    Future of the url, which has a random token: only it connects, and no
    request with an Origin header, so it is the credential to keep."""
    future = Future()

    def done(url, error, data):
        _alive.discard(callback)
        if error is not None:
            future.set_error(Error(error.decode()))
        else:
            future.set_result(url.decode())
    callback = _LISTEN(_guard(done))
    _alive.add(callback)
    _listen(port, callback, None)
    return future


class Clock:
    def __init__(self, page):
        self._page = page

    def _time(self, value, ticks=False):
        key = ("ticks" if ticks else "time") + ("Number" if isinstance(value, (int, float)) else "String")
        if hasattr(value, "timestamp"):
            return {"timeNumber": value.timestamp() * 1000}
        return {key: value}

    async def install(self, time=None):
        await _command(self._page._view, "BrowserContext.clockInstall", self._time(time) if time is not None else {})

    async def fast_forward(self, ticks):
        await _command(self._page._view, "BrowserContext.clockFastForward", self._time(ticks, True))

    async def run_for(self, ticks):
        await _command(self._page._view, "BrowserContext.clockRunFor", self._time(ticks, True))

    async def pause_at(self, time):
        await _command(self._page._view, "BrowserContext.clockPauseAt", self._time(time))

    async def resume(self):
        await _command(self._page._view, "BrowserContext.clockResume")

    async def set_fixed_time(self, time):
        await _command(self._page._view, "BrowserContext.clockSetFixedTime", self._time(time))

    async def set_system_time(self, time):
        await _command(self._page._view, "BrowserContext.clockSetSystemTime", self._time(time))


class APIRequestContext:
    """page.request: requests through the engine's network, with its
    cookies."""

    def __init__(self, page):
        self._page = page

    async def fetch(self, url, method="GET", headers=None, data=None, timeout=None, max_redirects=None, max_body_size=None):
        if isinstance(data, (dict, list)):
            data = json.dumps(data)
            headers = {"content-type": "application/json", **(headers or {})}
        if isinstance(data, str):
            data = data.encode()
        answer = await _command(self._page._view, "APIRequestContext.fetch", {
            "url": url, "method": method,
            "headers": [{"name": k, "value": v} for k, v in (headers or {}).items()],
            "postData": base64.b64encode(data).decode() if data is not None else None,
            "timeout": _ms(timeout), "maxRedirects": max_redirects, "maxBodySize": max_body_size})
        return APIResponse(answer["response"])

    async def get(self, url, **options):
        return await self.fetch(url, "GET", **options)

    async def post(self, url, **options):
        return await self.fetch(url, "POST", **options)


class APIResponse(Response):
    def __init__(self, value):
        super().__init__(value)
        self._body = base64.b64decode(value.get("body", ""))

    async def body(self):
        return self._body

    async def text(self):
        return self._body.decode()

    async def json(self):
        return json.loads(self._body)


# ---- expect ----------------------------------------------------------------

class _Expect:
    """Playwright's assertions, retried in the page until they hold
    (Frame.expect)."""

    def __init__(self, target, is_not=False):
        self._target = target
        self._not = is_not

    @property
    def not_(self):
        return _Expect(self._target, not self._not)

    async def _expect(self, expression, timeout=None, message=None, **options):
        target = self._target
        params = {"expression": expression, "isNot": self._not, "timeout": float(timeout or 5000)}
        params.update({k: v for k, v in options.items() if v is not None})
        if isinstance(target, Locator):
            answer = await target._frame._do("Frame.expect", selector=target._selector, **params)
        else:
            answer = await target._do("Frame.expect", **params)
        if answer.get("matches") == self._not:
            received = answer.get("received")
            detail = answer.get("errorMessage") or ""
            raise AssertionError(message or "expect(%s).%s%s failed%s: received %r %s" % (
                getattr(target, "_selector", "page"), "not." if self._not else "", expression,
                " (timed out)" if answer.get("timedOut") else "", received, detail))

    @staticmethod
    def _text(values, **flags):
        items = values if isinstance(values, (list, tuple)) else [values]
        result = []
        for value in items:
            if isinstance(value, re.Pattern):
                entry = {"regexSource": value.pattern, "regexFlags": "i" if value.flags & re.IGNORECASE else ""}
            else:
                entry = {"string": value}
            entry.update({k: v for k, v in flags.items() if v is not None})
            result.append(entry)
        return result

    async def to_have_text(self, expected, use_inner_text=None, ignore_case=None, timeout=None):
        array = isinstance(expected, (list, tuple))
        await self._expect("to.have.text.array" if array else "to.have.text", timeout,
                           expectedText=self._text(expected, matchSubstring=False, normalizeWhiteSpace=True,
                                                   ignoreCase=ignore_case),
                           useInnerText=use_inner_text)

    async def to_contain_text(self, expected, use_inner_text=None, ignore_case=None, timeout=None):
        array = isinstance(expected, (list, tuple))
        await self._expect("to.contain.text.array" if array else "to.have.text", timeout,
                           expectedText=self._text(expected, matchSubstring=True, normalizeWhiteSpace=True,
                                                   ignoreCase=ignore_case),
                           useInnerText=use_inner_text)

    async def to_have_value(self, value, timeout=None):
        await self._expect("to.have.value", timeout, expectedText=self._text(value))

    async def to_have_attribute(self, name, value, timeout=None):
        await self._expect("to.have.attribute.value", timeout, expressionArg=name,
                           expectedText=self._text(value))

    async def to_have_class(self, expected, timeout=None):
        await self._expect("to.have.class", timeout, expectedText=self._text(expected))

    async def to_have_count(self, count, timeout=None):
        await self._expect("to.have.count", timeout, expectedNumber=count)

    async def to_have_title(self, title, timeout=None):
        await self._expect("to.have.title", timeout, expectedText=self._text(title, normalizeWhiteSpace=True))

    async def to_have_url(self, url, timeout=None):
        await self._expect("to.have.url", timeout, expectedText=self._text(url))

    async def to_match_aria_snapshot(self, expected, timeout=None):
        await self._expect("to.match.aria", timeout, expectedValue=expected)

    async def _state(self, name, timeout=None):
        await self._expect(name, timeout)

    async def to_be_visible(self, timeout=None):
        await self._state("to.be.visible", timeout)

    async def to_be_hidden(self, timeout=None):
        await self._state("to.be.hidden", timeout)

    async def to_be_enabled(self, timeout=None):
        await self._state("to.be.enabled", timeout)

    async def to_be_disabled(self, timeout=None):
        await self._state("to.be.disabled", timeout)

    async def to_be_checked(self, timeout=None):
        await self._state("to.be.checked", timeout)

    async def to_be_editable(self, timeout=None):
        await self._state("to.be.editable", timeout)

    async def to_be_focused(self, timeout=None):
        await self._state("to.be.focused", timeout)

    async def to_be_empty(self, timeout=None):
        await self._state("to.be.empty", timeout)

    async def to_be_attached(self, timeout=None):
        await self._state("to.be.attached", timeout)

    async def to_be_in_viewport(self, timeout=None):
        await self._state("to.be.in.viewport", timeout)


def expect(target):
    """expect(locator) and expect(page), as Playwright's."""
    return _Expect(target)
