"""On Windows: the integrity level of each child of a process, as win_host.c reads it.

    win_child_tokens.py <program name>

Prints one line per process of that name; the first has no parent of the name.

A diagnostic, run by hand on a Windows machine while a sandboxed program runs
(for instance win-host-sandbox in scripts/engine-test.sh's work directory, with
ENGINE_TEST_KEEP_WORK=1), not a test the suite runs: it checks nothing itself.
"""

import ctypes
import ctypes.wintypes as w
import sys

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
advapi32 = ctypes.WinDLL("advapi32", use_last_error=True)
kernel32.OpenProcess.restype = w.HANDLE
kernel32.CreateToolhelp32Snapshot.restype = w.HANDLE
advapi32.GetSidSubAuthorityCount.restype = ctypes.POINTER(ctypes.c_ubyte)
advapi32.GetSidSubAuthority.restype = ctypes.POINTER(w.DWORD)
advapi32.GetSidSubAuthorityCount.argtypes = [ctypes.c_void_p]
advapi32.GetSidSubAuthority.argtypes = [ctypes.c_void_p, w.DWORD]
advapi32.OpenProcessToken.argtypes = [w.HANDLE, w.DWORD, ctypes.POINTER(w.HANDLE)]
advapi32.GetTokenInformation.argtypes = [w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD, ctypes.POINTER(w.DWORD)]
kernel32.Process32FirstW.argtypes = kernel32.Process32NextW.argtypes = [w.HANDLE, ctypes.c_void_p]


class Entry(ctypes.Structure):
    _fields_ = [("size", w.DWORD), ("usage", w.DWORD), ("pid", w.DWORD), ("heap", ctypes.c_size_t),
                ("module", w.DWORD), ("threads", w.DWORD), ("parent", w.DWORD), ("priority", w.LONG),
                ("flags", w.DWORD), ("name", w.WCHAR * 260)]


def level(pid):
    process = kernel32.OpenProcess(0x1000, False, pid)
    token = w.HANDLE()
    if not process or not advapi32.OpenProcessToken(process, 8, ctypes.byref(token)):
        return None
    buffer = ctypes.create_string_buffer(256)
    size = w.DWORD()
    if not advapi32.GetTokenInformation(token, 25, buffer, 256, ctypes.byref(size)):
        return None
    sid = ctypes.cast(buffer, ctypes.POINTER(ctypes.c_void_p))[0]
    return advapi32.GetSidSubAuthority(sid, advapi32.GetSidSubAuthorityCount(sid)[0] - 1)[0]


snapshot = kernel32.CreateToolhelp32Snapshot(2, 0)
entry = Entry()
entry.size = ctypes.sizeof(Entry)
more = kernel32.Process32FirstW(snapshot, ctypes.byref(entry))
while more:
    if entry.name.lower() == sys.argv[1].lower():
        print(f"{entry.name} {entry.pid} parent {entry.parent}: integrity {level(entry.pid):#x}")
    more = kernel32.Process32NextW(snapshot, ctypes.byref(entry))
