"""Input right after a navigation, and what automation had wrong around it:
tests/engine/automation_input.py <switches...>; ZEBKIT_TEST_ROUNDS= how many navigations
(100; another number is said, as a "narrowed:" line).

A new page's widget takes no input until its first frame (paint holding): the browser
answers the event itself and the page never hears of it. Commands wait for that signal
(docs/AUTOMATION.md 4.4), and input that was dropped anyway fails its command."""
import http.server
import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import Server, check, finish, knob, zebkit  # noqa: E402
from zebkit.automation import Page, run  # noqa: E402

# A debugging option: fewer rounds, said ("narrowed:"), a failure under ENGINE_TEST_STRICT=1.
ROUNDS = int(knob('ROUNDS') or '100')
seen_paths = []
seen_auth = {}


LISTEN = """<script>
window.seen = [];
for (const type of ['mousemove', 'mousedown', 'mouseup', 'click', 'keydown', 'keyup', 'wheel'])
  addEventListener(type, e => seen.push(type), true);
</script><style>body{margin:0;height:3000px}#b{position:absolute;left:10px;top:10px;width:200px;height:100px;background:#8f8}</style><div id=b>x</div><input id=i type=file>"""
# The first frame comes late: the widget holds input for over half a second.
SLOW = LISTEN + "<script>const t = performance.now(); while (performance.now() - t < 700);</script>"


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def send(self, status, body=b'', headers=()):
        self.send_response(status)
        for name, value in headers:
            self.send_header(name, value)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split('?')[0]
        seen_paths.append(self.headers.get('Host', '') + self.path)
        seen_auth[path] = (self.headers.get('Authorization'), self.headers.get('X-Other'))
        other = f'http://localhost:{self.server.server_port}'
        if path == '/slow':
            self.send(200, SLOW.encode(), [('Content-Type', 'text/html')])
        elif path == '/other-origin':
            self.send(302, headers=[('Location', other + '/target')])
        elif path == '/same-origin':
            self.send(302, headers=[('Location', '/target')])
        elif path == '/twice':
            self.send(302, headers=[('Location', '/same-origin')])
        elif path == '/target':
            self.send(200, b'target', [('Content-Type', 'text/plain')])
        elif path == '/big':
            self.send(200, b'x' * (1024 * 1024), [('Content-Type', 'text/plain')])
        else:
            self.send(200, LISTEN.encode(), [('Content-Type', 'text/html')])


base = Server(Handler).base


async def seen(page):
    return json.loads(await page.evaluate('JSON.stringify(seen)'))


async def without_the_wait():
    # ZEBKIT_TEST_NO_INPUT_WAIT: what commands would do without it. Held input is
    # dropped by the browser, and the command says so instead of succeeding.
    page = Page(width=800, height=600)
    await page.goto(f'{base}/')
    await page.goto(f'{base}/slow', wait_until='commit')
    try:
        await page.mouse.click(50, 50)
        check(False, 'held input fails its command')
    except Exception as error:  # noqa: BLE001
        check('dropped the input' in str(error), 'held input fails its command', str(error))
    await page.close()


async def input_after_navigation():
    # One view, navigated again and again: each document has a new widget.
    page = Page(width=800, height=600)
    lost = {'click': [], 'press': [], 'mouse': []}
    for round in range(ROUNDS):
        await page.goto(f'{base}/?{round}')
        await page.locator('#b').click()
        if 'click' not in await seen(page):
            lost['click'].append(round)
        await page.goto(f'{base}/?{round}k')
        await page.keyboard.press('a')
        if 'keydown' not in await seen(page):
            lost['press'].append(round)
        await page.goto(f'{base}/?{round}m')
        await page.mouse.click(50, 50)
        if 'click' not in await seen(page):
            lost['mouse'].append(round)
    for kind, rounds in lost.items():
        check(not rounds, f'{kind} right after a navigation, {ROUNDS} times: none lost', rounds[:10])
    # The widget still holds input when the command comes: it has to wait.
    await page.goto(f'{base}/slow', wait_until='commit')
    await page.mouse.click(50, 50)
    names = await seen(page)
    check('click' in names, 'a click while the page has no first frame arrives', names)
    await page.goto(f'{base}/slow?2', wait_until='commit')
    await page.keyboard.press('a')
    names = await seen(page)
    check('keydown' in names, 'a key while the page has no first frame arrives', names)
    await page.close()


async def input_files():
    page = Page(width=800, height=600)
    with tempfile.TemporaryDirectory(prefix='zebkit-input-files-') as directory:
        path = os.path.join(directory, 'chosen.txt')
        with open(path, 'w') as file:
            file.write('the contents')
        await page.goto(f'{base}/files')
        await page.locator('#i').set_input_files(path)
        text = await page.evaluate("document.querySelector('#i').files[0].text()")
        check(text == 'the contents', 'a file set by its path can be read by the page', text)
        crashed = []
        page.on('crash', lambda *_: crashed.append(1))
        await page.goto(f'{base}/after')
        check(await page.evaluate('1 + 1') == 2 and not crashed, 'leaving the page does not kill the renderer', crashed)
    await page.close()


async def fetches():
    page = Page(width=800, height=600)
    secret = {'Authorization': 'Bearer secret', 'X-Other': 'kept'}
    seen_paths.clear()
    answer = await page.request.fetch(f'{base}/same-origin', max_redirects=0)
    check(answer.status == 302, 'maxRedirects 0 answers with the redirect', answer.status)
    check(not any('/target' in path for path in seen_paths), 'and the target hears nothing', seen_paths)
    try:
        await page.request.fetch(f'{base}/twice', max_redirects=1)
        check(False, 'over maxRedirects fails')
    except Exception as error:  # noqa: BLE001
        check('Max redirect count' in str(error), 'over maxRedirects fails', str(error))
    answer = await page.request.fetch(f'{base}/twice', max_redirects=2)
    check(answer.status == 200, 'within maxRedirects goes on', answer.status)
    seen_auth.clear()
    answer = await page.request.fetch(f'{base}/same-origin', headers=secret)
    check(seen_auth['/target'][0] == 'Bearer secret', 'Authorization goes along inside the origin', seen_auth)
    seen_auth.clear()
    answer = await page.request.fetch(f'{base}/other-origin', headers=secret)
    check(answer.status == 200 and seen_auth['/target'][0] is None, 'not to another origin', seen_auth)
    check(seen_auth['/target'][1] == 'kept', 'where other headers still go', seen_auth)
    try:
        await page.request.fetch(f'{base}/big', max_body_size=1024)
        check(False, 'a body over its limit fails')
    except Exception as error:  # noqa: BLE001
        check('larger than the limit' in str(error), 'a body over its limit fails', str(error))
    answer = await page.request.fetch(f'{base}/big', max_body_size=2 * 1024 * 1024)
    check(len(await answer.body()) == 1024 * 1024, 'and one within it is whole')

    seen_auth.clear()
    results = []
    zebkit.fetch(f'{base}/other-origin', results.append, headers={'Authorization': 'Bearer secret'})
    for _ in range(200):
        if results:
            break
        await page.wait_for_timeout(25)
    check(results and '/target' in seen_auth and seen_auth['/target'][0] is None,
          "zebkit_fetch leaves Authorization behind at another origin", seen_auth)
    await page.close()


async def viewport():
    page = Page(width=800, height=600)
    await page.goto(f'{base}/')
    await page.set_viewport_size({'width': 500, 'height': 400})
    sizes = []
    for _ in range(5):
        await page.wait_for_timeout(100)
        sizes.append(await page.evaluate('[innerWidth, innerHeight]'))
    check(all(size == [500, 400] for size in sizes), 'setViewportSize on an off-screen view lasts', sizes)
    await page.close()


async def test():
    if '--zebkit-test-no-input-wait' in sys.argv:
        await without_the_wait()
        return
    for part in (input_after_navigation, input_files, fetches, viewport):
        await part()


def main():
    def ready():
        future = run(test())

        def finished(done):
            if done._error is not None:
                check(False, 'the test ran to the end', repr(done._error))
            zebkit.quit()
        future.add_done_callback(finished)

    code = zebkit.main(ready, ['python'] + sys.argv[1:])
    result = finish()
    sys.exit(1 if code else result)


main()
