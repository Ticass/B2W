"""Capture a process's mapped PE image for offline native-code inspection."""
import ctypes as c
from ctypes import wintypes as w
from pathlib import Path
import sys
import struct

k = c.WinDLL('kernel32', use_last_error=True)
k.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
k.OpenProcess.restype = w.HANDLE
k.ReadProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t)]
k.CloseHandle.argtypes = [w.HANDLE]
h = k.OpenProcess(0x410, False, int(sys.argv[1]))
if not h:
    raise c.WinError(c.get_last_error())
def read(addr, size):
    b = c.create_string_buffer(size)
    n = c.c_size_t()
    if not k.ReadProcessMemory(h, addr, b, size, c.byref(n)):
        raise c.WinError(c.get_last_error())
    return b.raw
base = int(sys.argv[2], 0)
try:
    head = bytearray(read(base, 4096))
    pe = struct.unpack_from('<I', head, 60)[0]
    sections = struct.unpack_from('<H', head, pe + 6)[0]
    opt = struct.unpack_from('<H', head, pe + 20)[0]
    table = pe + 24 + opt
    out = head
    for i in range(sections):
        pos = table + 40*i
        vs, va, rs, raw = struct.unpack_from('<IIII', head, pos+8)
        if not rs:
            continue
        if len(out) < raw+rs:
            out.extend(b'\0' * (raw+rs-len(out)))
        out[raw:raw+rs] = read(base+va, rs)
    Path(sys.argv[3]).write_bytes(out)
finally:
    k.CloseHandle(h)
