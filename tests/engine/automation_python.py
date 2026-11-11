"""zebkit.automation, Playwright's async API for Python on the native
automation (docs/AUTOMATION.md), on tests/engine/pages/automation.html:
locators by role, text and test id, actions, expect, handles, routes,
dialogs, exposed functions, the clock, contexts of their own, workers,
tracing, the Playwright endpoint.

    python3 tests/engine/automation_python.py <base URL> [engine switches...]

Exit 0 when every check passes.
"""

import math
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "bindings", "python"))

import zebkit  # noqa: E402
from zebkit.automation import Page, TimeoutError, expect, listen, new_context, run  # noqa: E402

failures = []
checks = 0


def check(ok, what, detail=None):
    global checks
    checks += 1
    print(("ok   " if ok else "FAIL ") + what + (": %r" % (detail,) if detail is not None else ""), flush=True)
    if not ok:
        failures.append(what)


async def test(base):
    page = Page(width=1280, height=800)
    response = await page.goto(base + "/automation.html")
    check(response.status == 200, "goto", response.status)
    check(await page.title() == "Automation", "title")

    await page.get_by_role("button", name="Go").click()
    check(await page.locator("#out").text_content() == "clicked 1", "get_by_role().click()")
    await expect(page.locator("#out")).to_have_text("clicked 1")
    check(True, "expect().to_have_text()")
    try:
        await expect(page.locator("#out")).to_have_text("nope", timeout=300)
        check(False, "a failing expect raises")
    except AssertionError as e:
        check("timed out" in str(e), "a failing expect raises", str(e)[:80])

    await page.get_by_placeholder("Name").fill("Grace")
    check(await page.locator("#name").input_value() == "Grace", "get_by_placeholder().fill()")
    await page.locator("#name").press("End")
    await page.locator("#name").press_sequentially(" H")
    check(await page.locator("#name").input_value() == "Grace H", "press_sequentially()")

    check(await page.locator(".item").count() == 3, "count()")
    check(await page.locator(".item").all_text_contents() == ["one", "two", "three"], "all_text_contents()")
    check(await page.locator(".item").filter(has_text="two").text_content() == "two", "filter(has_text=)")
    check(await page.locator(".item").last.text_content() == "three", "last")
    check(await page.get_by_text("I agree").is_visible(), "get_by_text().is_visible()")
    await page.get_by_text("I agree").check()
    check(await page.locator("#agree").is_checked(), "check() through its label")

    values = await page.locator("#color").select_option(label="Blue")
    check(values == ["b"], "select_option(label=)", values)

    deep = page.frame_locator("#inner").locator("#deep")
    await deep.click()
    check(await deep.text_content() == "pressed", "frame_locator().click()")

    handle = await page.query_selector("#go")
    check(await handle.text_content() == "Go", "ElementHandle.text_content()")
    box = await handle.bounding_box()
    check(box and box["width"] > 0, "ElementHandle.bounding_box()", box)
    value = await page.evaluate("(b) => b.tagName", handle)
    check(value == "BUTTON", "evaluate() with a handle", value)

    try:
        await page.locator("#nothing").click(timeout=300)
        check(False, "a timeout raises TimeoutError")
    except TimeoutError as e:
        check("Timeout 300ms exceeded" in str(e), "a timeout raises TimeoutError")

    seen = []
    page.on("dialog", lambda dialog: (seen.append(dialog.message), dialog.accept()))
    answer = await page.evaluate("() => new Promise(r => setTimeout(() => r(confirm('Proceed?')), 0))")
    check(answer is True and seen == ["Proceed?"], "page.on('dialog') and accept()", (answer, seen))

    async def routed(route, request):
        await route.fulfill(json_body={"from": "route"})
    await page.route("**/data.json", routed)
    got = await page.evaluate("() => fetch('data.json').then(r => r.json())")
    check(got == {"from": "route"}, "page.route() and fulfill()", got)

    await page.expose_function("add", lambda a, b: a + b)
    total = await page.evaluate("() => add(20, 22)")
    check(total == 42, "expose_function()", total)

    await page.clock.install(time=0)
    await page.clock.set_fixed_time(5000)
    now = await page.evaluate("() => Date.now()")
    check(now == 5000, "clock.set_fixed_time()", now)

    snapshot = await page.locator("ul").aria_snapshot()
    check("- listitem: one" in snapshot, "aria_snapshot()", snapshot)
    await expect(page.locator("ul")).to_match_aria_snapshot("- list:\n  - listitem: one\n  - listitem: two\n  - listitem: three")
    check(True, "to_match_aria_snapshot()")

    png = await page.screenshot()
    check(png[:8] == b"\x89PNG\r\n\x1a\n", "screenshot()")
    jpeg = await page.screenshot(type="jpeg", quality=60)
    check(jpeg[:3] == b"\xff\xd8\xff", "screenshot(type='jpeg')")
    # zebkit's own: the pixels, which are the PNG's; the viewport taken
    # without a clip is the same picture as with the viewport's clip, also
    # scrolled.
    # Two pictures, each of a frame of its own: a page that changed in
    # between (a frame still to come of what came before) is taken again.
    for attempt in range(3):
        width, height, rgba = zebkit.image_decode(await page.screenshot())
        pixels = await page.screenshot_pixels()
        differ = [i for i in range(0, min(len(rgba), len(pixels.data)), 4) if rgba[i:i + 4] != pixels.data[i:i + 4]]
        if (pixels.width, pixels.height) == (width, height) and not differ:
            break
    check((pixels.width, pixels.height, pixels.stride) == (1280, 800, 1280 * 4) and pixels.data == rgba,
          "screenshot_pixels() are the PNG's",
          (pixels.width, pixels.height, width, height, attempt, len(differ),
           differ and divmod(differ[0] // 4, width)[::-1]))
    for scroll in (0, 120):
        await page.evaluate("(y) => { document.body.style.minHeight = '3000px'; scrollTo(0, y); }", scroll)
        whole = await page.screenshot_pixels()
        clipped = await page.screenshot_pixels(clip={"x": 0, "y": 0, "width": 1280, "height": 800})
        check(whole == clipped, "the viewport with no clip is the clipped one's picture, scrolled by %d" % scroll)
    await page.evaluate("() => { document.body.style.minHeight = ''; scrollTo(0, 0); }")
    element = await page.locator("#go").screenshot_pixels()
    box = await page.locator("#go").bounding_box()
    # The box's enclosing pixels, as screenshotter.ts takes an element's.
    enclosing = (math.ceil(box["x"] + box["width"]) - math.floor(box["x"]),
                 math.ceil(box["y"] + box["height"]) - math.floor(box["y"]))
    check((element.width, element.height) == enclosing,
          "Locator.screenshot_pixels() is the element's", ((element.width, element.height), box))

    workers = []
    page.on("worker", workers.append)
    await page.evaluate("() => { window.w = new Worker(URL.createObjectURL(new Blob(['self.x = 7']))); }")
    for _ in range(100):
        if workers:
            break
        await page.wait_for_timeout(20)
    check(bool(workers) and await workers[0].evaluate("self.x * 6") == 42, "the worker event, Worker.evaluate()")

    await page.tracing.start(screenshots=True)
    await page.locator("#go").click()
    zip_path = os.path.join(os.environ.get("TMPDIR", "/tmp"), "zebkit-python-%d.zip" % os.getpid())
    try:
        trace = await page.tracing.stop(zip_path)
        check(trace and open(trace, "rb").read(2) == b"PK", "tracing.stop() writes the zip", trace)
    finally:
        if os.path.exists(zip_path):
            os.unlink(zip_path)

    context = await new_context(viewport={"width": 640, "height": 480}, user_agent="zebkit-python-context")
    other = await context.new_page()
    agent = await other.evaluate("() => navigator.userAgent")
    check(agent == "zebkit-python-context", "new_context(user_agent=...)", agent)
    await context.add_cookies([{"name": "only", "value": "x", "url": base + "/"}])
    names = [c["name"] for c in await context.cookies(base + "/")]
    mine = [c["name"] for c in await page.cookies(base + "/")]
    check("only" in names and "only" not in mine, "a context's cookies are its own", (names, mine))
    await context.close()

    url = await listen(0)
    check(url.startswith("ws://127.0.0.1:"), "listen() serves Playwright's protocol", url)
    check(len(url.rsplit("/", 1)[1]) >= 32, "listen()'s url has its random token", url)


def main():
    base = sys.argv[1]

    def ready():
        future = run(test(base))

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
