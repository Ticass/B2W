"""Disassemble native D3D bytecode with the Windows D3D compiler."""
import ctypes as c
from pathlib import Path
import sys

data = Path(sys.argv[1]).read_bytes()
dll = c.WinDLL('d3dcompiler_47.dll')
fn = dll.D3DDisassemble
fn.argtypes = [c.c_void_p, c.c_size_t, c.c_uint, c.c_char_p, c.POINTER(c.c_void_p)]
fn.restype = c.c_long
blob = c.c_void_p()
hr = fn(data, len(data), 0, None, c.byref(blob))
if hr < 0:
    raise RuntimeError(hex(hr & 0xffffffff))
vt = c.cast(blob, c.POINTER(c.POINTER(c.c_void_p))).contents
ptr = c.WINFUNCTYPE(c.c_void_p, c.c_void_p)(vt[3])(blob)
size = c.WINFUNCTYPE(c.c_size_t, c.c_void_p)(vt[4])(blob)
Path(sys.argv[2]).write_bytes(c.string_at(ptr, size))
c.WINFUNCTYPE(c.c_ulong, c.c_void_p)(vt[2])(blob)
