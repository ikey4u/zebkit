"""HTML in canvas (Chromium 157, https://github.com/WICG/html-in-canvas) in
zebkit's views, on tests/engine/pages/html-in-canvas.html: a button drawn into
a 2D canvas by drawElementImage() is in the view's pixels where it is drawn,
and stays a button there, for the view's own mouse and for Playwright's API.

    python3 tests/engine/html_in_canvas.py <base URL> [engine switches...]

Exit 0 when every check passes.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import check, finish, zebkit  # noqa: E402
from zebkit.automation import Page, run  # noqa: E402

# Where the page draws the button: at (60, 50) in the canvas, 120 by 80.
CANVAS = (0, 0)
DRAWN = (60, 50, 120, 80)


def rgb(pixels, x, y):
    i = y * pixels.stride + x * 4
    return tuple(pixels.data[i:i + 3])


def near(color, want, tolerance=6):
    return all(abs(a - b) <= tolerance for a, b in zip(color, want))


async def test(base):
    page = Page(width=400, height=300)
    response = await page.goto(base + "/html-in-canvas.html")
    check(response.status == 200, "goto", response.status)

    supported = await page.evaluate("""() => {
        const canvas = document.createElement('canvas');
        canvas.setAttribute('content', 'drawable');
        return canvas.content === 'drawable' &&
            typeof CanvasRenderingContext2D.prototype.drawElementImage === 'function';
    }""")
    check(supported, "the engine has HTML in canvas (content=drawable, drawElementImage)")

    await page.wait_for_function("() => window.painted > 0", timeout=10000)
    check(True, "the canvas's paint event came and drew the element")

    pixels = await page.screenshot_pixels()
    x, y, w, h = DRAWN
    centre = (CANVAS[0] + x + w // 2, CANVAS[1] + y + h // 2)
    check(near(rgb(pixels, *centre), (0, 160, 0)), "the drawn element is in the pixels where it is drawn",
          rgb(pixels, *centre))
    check(near(rgb(pixels, CANVAS[0] + 20, CANVAS[1] + 20), (128, 128, 128)), "the canvas around it is the canvas's",
          rgb(pixels, CANVAS[0] + 20, CANVAS[1] + 20))
    check(near(rgb(pixels, CANVAS[0] + x + w + 20, CANVAS[1] + y + h // 2), (128, 128, 128)),
          "and only that rectangle is drawn")

    hit = await page.evaluate("([x, y]) => document.elementFromPoint(x, y)?.id", list(centre))
    check(hit == "go", "hit testing finds the button where it is drawn", hit)

    await page.mouse.click(*centre)
    clicks = await page.evaluate("() => window.clicks")
    check(clicks == 1, "the view's mouse clicks the button where it is drawn", clicks)

    await page.locator("#go").click()
    clicks = await page.evaluate("() => window.clicks")
    check(clicks == 2, "Playwright's click() finds and clicks it", clicks)

    box = await page.locator("#go").bounding_box()
    check(box is not None and abs(box["x"] - (CANVAS[0] + x)) < 1 and abs(box["y"] - (CANVAS[1] + y)) < 1,
          "its bounding box is where it is drawn", box)

    # Drawn somewhere else, it is there, for the pixels and for input.
    await page.evaluate("""() => new Promise((resolve) => {
        canvas.addEventListener('paint', () => {
            const context = canvas.getContext('2d');
            context.reset();
            context.drawElementImage(card, 160, 100);
            resolve();
        }, {once: true});
        canvas.requestPaint();
    })""")
    await page.evaluate("() => new Promise((resolve) => requestAnimationFrame(() => requestAnimationFrame(resolve)))")
    pixels = await page.screenshot_pixels()
    moved = (CANVAS[0] + 160 + w // 2, CANVAS[1] + 100 + h // 2)
    check(near(rgb(pixels, *moved), (0, 160, 0)), "drawn elsewhere, it is in the pixels there", rgb(pixels, *moved))
    check(near(rgb(pixels, *centre), (128, 128, 128)), "and no more where it was", rgb(pixels, *centre))
    await page.mouse.click(*moved)
    clicks = await page.evaluate("() => window.clicks")
    check(clicks == 3, "and its clicks come there", clicks)


def main():
    base = sys.argv[1]

    def ready():
        future = run(test(base))

        def finished(done):
            if done._error is not None:
                check(False, "the test ran to the end", repr(done._error))
            zebkit.quit()
        future.add_done_callback(finished)

    code = zebkit.main(ready, ["python"] + sys.argv[2:])
    result = finish()
    sys.exit(1 if code else result)


if __name__ == "__main__":
    main()
