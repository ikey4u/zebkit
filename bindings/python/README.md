# zebkit for Python

Chromium's engine as a library: web views in your own windows or off-screen,
a full PDF API on PDFium, media, compression, crypto, Playwright's API run
inside the engine, and the C libraries inside it. Pure Python on `ctypes`,
nothing to compile.

```python
import zebkit

def ready():
    view = zebkit.View("https://example.com/")
    view.on_load(lambda event, url: event == zebkit.LOAD_FINISHED and
                 view.evaluate("document.title", lambda title, error: print(title)))

zebkit.main(ready)
```

The package is the binding; the engine is a separate download. Unpack the
archive for your system from the [releases](https://github.com/ikey4u/zebkit/releases)
of the same version (Linux x86-64, macOS on Apple silicon, Windows x64) and
point `ZEBKIT_DIR` at it:

```sh
pip install zebkit
export ZEBKIT_DIR=/path/to/zebkit-<version>-<target>
```

On Windows `python.exe` cannot be one of the sandbox's children
(`docs/WINDOWS.md`): run the engine with `--no-sandbox`, for example
`zebkit.main(ready, ["python", "--no-sandbox"])`.

Everything is for the thread that called `zebkit.main()` (or `zebkit.init()`),
and callbacks run on it; `zebkit.post()` runs a function there from any
thread. A call from another thread raises `zebkit.Error`.

Everything the binding offers is in
[docs/API.md](https://github.com/ikey4u/zebkit/blob/main/docs/API.md), and
Playwright's API from Python (`zebkit.automation`) in
[docs/AUTOMATION.md](https://github.com/ikey4u/zebkit/blob/main/docs/AUTOMATION.md).
