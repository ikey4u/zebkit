#!/usr/bin/env python3
"""The native hosts' PDF pane: wheeled through a long document with XTest.

    native_host_pdf_scroll.py <host> <document.pdf> [engine switches...]

A private Xvfb. What the pane did is its own report (NATIVE_HOST_PDF_REPORT=1):
positions, pages rendered and kept, a checksum of the pane's pixels.
"""
import ctypes as C
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

LINE = re.compile(r"pdf-pane: position=(\d+) total=(\d+) page=(\d+) pages=(\d+) visible=(\d+)-(\d+) "
                  r"renders=(\d+) kept=(\d+) bytes=(\d+) checksum=(\w+)")


def self_driven(host, document, switches):
    """On a Mac: the host wheels itself (--scroll-test); the person's input stays theirs."""
    result = subprocess.run([host, "--scroll-test", "--pdf=" + str(document), *switches],
                            env=dict(os.environ, NATIVE_HOST_PDF_REPORT="1"), capture_output=True, text=True, timeout=240)
    lines = [LINE.search(line) for line in result.stdout.splitlines() if "pdf-pane:" in line]
    failures = 0

    def check(condition, what, detail=""):
        nonlocal failures
        print(("ok   " if condition else "FAIL ") + what + ("" if condition else f": {detail!r}"), flush=True)
        failures += not condition

    check(lines and int(lines[-1][4]) >= 300, "the pane has the document", result.stdout[-400:] + result.stderr[-400:])
    if lines:
        positions = [int(m[1]) for m in lines]
        top = max(positions)
        steps = [abs(b - a) for a, b in zip(positions, positions[1:])]
        check(int(lines[positions.index(top)][3]) >= int(lines[-1][4]) - 1, "the wheel reaches the end", top)
        check(len(set(m[10] for m in lines)) > 100, "the pixels change as it goes", len(lines))
        check(sorted(steps)[len(steps) // 2] < 150, "in small steps", sorted(steps)[len(steps) // 2])
        check(positions[-1] < top, "and back up", positions[-1])
        check(int(lines[-1][8]) < int(lines[-1][4]) / 2 and int(lines[-1][9]) <= 160 << 20, "what is kept is bounded", lines[-1][0])
        print(f"note {lines[-1][7]} renders of {lines[-1][4]} pages; {lines[-1][8]} kept, {lines[-1][9]} bytes")
    check(result.returncode == 0, "the host ends cleanly", result.returncode)
    print("FAIL" if failures else "0 failures")
    return 1 if failures else 0


def main():
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from pdf_long import long_pdf
    self_drive = sys.argv[1] == "--self"
    if self_drive:
        del sys.argv[1]
    host, document = sys.argv[1:3]
    if document == "generated":
        generated = tempfile.NamedTemporaryFile(suffix=".pdf", delete=False)
        generated.write(long_pdf(300))
        generated.close()
        document = generated.name
    if self_drive:
        return self_driven(host, document, sys.argv[3:])
    switches = sys.argv[3:]
    x = C.CDLL("libX11.so.6")
    test = C.CDLL("libXtst.so.6")
    x.XOpenDisplay.argtypes = [C.c_char_p]
    x.XOpenDisplay.restype = C.c_void_p
    x.XFlush.argtypes = [C.c_void_p]
    x.XCloseDisplay.argtypes = [C.c_void_p]
    test.XTestFakeMotionEvent.argtypes = [C.c_void_p, C.c_int, C.c_int, C.c_int, C.c_ulong]
    test.XTestFakeButtonEvent.argtypes = [C.c_void_p, C.c_uint, C.c_int, C.c_ulong]
    server = subprocess.Popen(["Xvfb", "-displayfd", "1", "-screen", "0", "1400x900x24", "-nolisten", "tcp"],
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    process = None
    display = None
    failures = []

    def check(condition, what, detail=""):
        print(("ok   " if condition else "FAIL ") + what + ("" if condition else f": {detail!r}"), flush=True)
        if not condition:
            failures.append(what)

    try:
        name = ":" + server.stdout.readline().strip()
        display = x.XOpenDisplay(name.encode())
        with tempfile.TemporaryDirectory(prefix="zebkit-pdf-scroll-") as work:
            log_path = Path(work) / "host.log"
            with log_path.open("w") as log:
                process = subprocess.Popen(
                    [host, "--pdf=" + str(Path(document).resolve()), *switches, "--user-data-dir=" + work + "/profile"],
                    env=dict(os.environ, DISPLAY=name, GDK_BACKEND="x11", QT_QPA_PLATFORM="xcb", NATIVE_HOST_PDF_REPORT="1"),
                    stdout=log, stderr=subprocess.STDOUT)

            def lines():
                return [LINE.search(line) for line in log_path.read_text(errors="replace").splitlines() if "pdf-pane:" in line]

            end = time.monotonic() + 30
            while time.monotonic() < end and not lines():
                if process.poll() is not None:
                    raise RuntimeError(log_path.read_text())
                time.sleep(.1)
            check(bool(lines()), "the pane draws", log_path.read_text()[-500:])
            first = lines()[-1]
            pages = int(first[4])
            check(pages >= 300, f"{pages} pages in the document")
            # The pointer in the right half of the window, over the PDF pane.
            test.XTestFakeMotionEvent(display, 0, 900, 400, 0)
            x.XFlush(display)
            time.sleep(.3)

            def notch(down, count, gap=.02):
                for _ in range(count):
                    button = 5 if down else 4
                    test.XTestFakeButtonEvent(display, button, 1, 0)
                    test.XTestFakeButtonEvent(display, button, 0, 0)
                    x.XFlush(display)
                    time.sleep(gap)

            notch(True, 1)
            time.sleep(.6)
            after = lines()
            positions = [int(m[1]) for m in after]
            distinct = sorted(set(positions))
            check(positions[-1] > int(first[1]), "one notch moves the document", positions[-5:])
            steps = [b - a for a, b in zip(distinct, distinct[1:])]
            check(len(distinct) >= 4 and max(steps) < 110, "one notch is smooth: several small steps", distinct)
            check(lines()[-1][10] != first[10], "the pixels changed", first[10])

            # Down through the whole document, as far as it goes.
            end = time.monotonic() + 180
            last = lines()[-1]
            while time.monotonic() < end and int(last[1]) + 900 < int(last[2]) - 1:
                notch(True, 40, .005)
                time.sleep(.05)
                last = lines()[-1]
                total = int(last[2])
                if int(last[3]) >= pages:
                    break
            time.sleep(.8)
            last = lines()[-1]
            check(int(last[3]) >= pages - 1, "the wheel reaches the end of the document", last[0])
            renders = int(last[7])
            kept = int(last[8])
            bytes_kept = int(last[9])
            print(f"note {renders} renders of {pages} pages; {kept} kept, {bytes_kept} bytes", flush=True)
            check(kept < pages / 2 and bytes_kept <= 160 << 20, "what is kept is bounded", last[0])
            top = int(last[1])
            notch(False, 5)
            time.sleep(.6)
            check(int(lines()[-1][1]) < top, "back up", lines()[-1][0])
            process.terminate()
            try:
                process.wait(20)
            except subprocess.TimeoutExpired:
                process.kill()
                check(False, "the host stops")
    finally:
        if process and process.poll() is None:
            process.kill()
        if display:
            x.XCloseDisplay(display)
        server.terminate()
    print(("FAIL " + ", ".join(failures)) if failures else f"{0} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
