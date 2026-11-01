"""On Windows: the visible top-level windows, by process, class and title."""

import ctypes
import ctypes.wintypes as w

user32 = ctypes.WinDLL("user32")
kernel32 = ctypes.WinDLL("kernel32")
kernel32.OpenProcess.restype = w.HANDLE
Enum = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)


def name_of(pid):
    process = kernel32.OpenProcess(0x1000, False, pid)
    buffer = ctypes.create_unicode_buffer(260)
    size = w.DWORD(260)
    if process and kernel32.QueryFullProcessImageNameW(process, 0, buffer, ctypes.byref(size)):
        return buffer.value.rsplit("\\", 1)[-1]
    return "?"


def show(hwnd, _):
    if user32.IsWindowVisible(hwnd):
        title = ctypes.create_unicode_buffer(200)
        kind = ctypes.create_unicode_buffer(100)
        user32.GetWindowTextW(hwnd, title, 200)
        user32.GetClassNameW(hwnd, kind, 100)
        pid = w.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if title.value:
            print(f"{name_of(pid.value)} [{kind.value}] {title.value!a}")
    return True


user32.EnumWindows(Enum(show), 0)
