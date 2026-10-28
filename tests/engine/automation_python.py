"""zebkit.automation, Playwright's async API for Python on the native
automation (docs/AUTOMATION.md), on tests/engine/pages/automation.html:
locators by role, text and test id, actions, expect, handles, routes,
dialogs, exposed functions, the clock, contexts of their own, workers,
tracing, the Playwright endpoint.

    python3 tests/engine/automation_python.py <base URL> [engine switches...]

Exit 0 when every check passes.
"""

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
    trace = await page.tracing.stop(os.path.join(os.environ.get("TMPDIR", "/tmp"), "zebkit-python-%d.zip" % os.getpid()))
    check(trace and open(trace, "rb").read(2) == b"PK", "tracing.stop() writes the zip", trace)
    if trace:
        os.unlink(trace)

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
