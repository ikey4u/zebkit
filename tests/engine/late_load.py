"""Load the library after host threads exist, including the engine owner."""
import mmap
import os
from pathlib import Path
import struct
import queue
import sys
import threading
import time
import traceback

mode = sys.argv[1]
ready = threading.Barrier(5)
loaded = threading.Event()
started = threading.Event()
stop = threading.Event()
errors = queue.Queue()
posted = []
local = threading.local()


def owner():
    assert zebkit.init([sys.argv[0], *sys.argv[2:]], settings={'logging': {'destination': 'none'}}) == 0
    view = zebkit.View()
    values = []
    view.on_load(lambda event, url: view.evaluate('21*2', lambda value, error: values.append((value, error)))
                 if event == zebkit.LOAD_FINISHED and url.startswith('data:') else None)
    view.load('data:text/html,<title>late loaded</title>')
    started.set()
    until = time.monotonic() + 15
    while time.monotonic() < until and (not values or len(posted) != (3 if mode == 'worker' else 4)):
        assert zebkit.pump()
        time.sleep(.001)
    assert values and values[0] == (42, None), values
    assert len(posted) == (3 if mode == 'worker' else 4), posted
    assert zebkit.shutdown()
    stop.set()


def worker(index):
    local.marker = index
    ready.wait()
    loaded.wait()
    try:
        if mode == 'worker' and index == 0:
            owner()
        else:
            assert started.wait(20)
            zebkit.post(lambda: posted.append(index))
            assert stop.wait(20)
        assert local.marker == index
    except BaseException:
        errors.put(traceback.format_exc())
        stop.set()


threads = [threading.Thread(target=worker, args=(i,), daemon=True) for i in range(4)]
for thread in threads:
    thread.start()
ready.wait()
# Deferred RELR cannot repair copies the loader already made for host
# threads. A library supporting late load must have no such TLS pointers.
if sys.platform.startswith('linux'):
    with (Path(os.environ['ZEBKIT_DIR']) / 'libzebkit.so.1').open('rb') as binary:
        with mmap.mmap(binary.fileno(), 0, access=mmap.ACCESS_READ) as elf:
            assert elf[:6] == b'\x7fELF\x02\x01'
            phoff = struct.unpack_from('<Q', elf, 32)[0]
            entry_size, count = struct.unpack_from('<HH', elf, 54)
            headers = [struct.unpack_from('<IIQQQQQQ', elf, phoff + i * entry_size) for i in range(count)]
            tls = next(h for h in headers if h[0] == 7)
            dynamic = next(h for h in headers if h[0] == 2)
            tags = dict(struct.unpack_from('<QQ', elf, at) for at in range(dynamic[2], dynamic[2] + dynamic[5], 16))
            address, size = tags[0x6fffe000], tags[0x6fffe001]
            segment = next(h for h in headers if h[0] == 1 and h[3] <= address < h[3] + h[5])
            offset = segment[2] + address - segment[3]
            where, deferred_tls = 0, []
            for at in range(offset, offset + size, 8):
                value = struct.unpack_from('<Q', elf, at)[0]
                if value & 1:
                    addresses = [where + i * 8 for i in range(63) if value & (1 << (i + 1))]
                    where += 63 * 8
                else:
                    addresses, where = [value], value + 8
                deferred_tls.extend(a - tls[3] for a in addresses if tls[3] <= a < tls[3] + tls[5])
            assert not deferred_tls, ('initialized TLS contains deferred relocations', deferred_tls)
import zebkit  # noqa: E402 -- deliberately after all host threads exist
loaded.set()
try:
    if mode == 'main':
        owner()
    for thread in threads:
        thread.join(25)
        assert not thread.is_alive()
    assert errors.empty(), list(errors.queue)
    print('ok late load owner=' + mode + ': pre-existing host threads, page evaluation, posted tasks and clean exit', flush=True)
finally:
    stop.set()
