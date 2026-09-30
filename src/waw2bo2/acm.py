"""Offline Windows ACM reference decoding; never creates an audio device.

Uses Microsoft's installed codec, independently of the FFmpeg pipeline.
API reference: learn.microsoft.com/windows/win32/api/msacm/
"""
from __future__ import annotations

import ctypes as c
import os
import struct

from .sounds import SoundError


class StreamHeader(c.Structure):
    _fields_ = [("cbStruct", c.c_uint32), ("fdwStatus", c.c_uint32), ("dwUser", c.c_size_t),
                ("pbSrc", c.c_void_p), ("cbSrcLength", c.c_uint32), ("cbSrcLengthUsed", c.c_uint32),
                ("dwSrcUser", c.c_size_t), ("pbDst", c.c_void_p), ("cbDstLength", c.c_uint32),
                ("cbDstLengthUsed", c.c_uint32), ("dwDstUser", c.c_size_t), ("reserved", c.c_size_t * 10)]


def decode(fmt: bytes, data: bytes) -> bytes:
    if os.name != "nt":
        raise SoundError("Windows ACM reference decoder unavailable on this OS")
    if len(fmt) < 18:
        raise SoundError("ACM requires complete WAVEFORMATEX")
    _, channels, rate = struct.unpack_from("<HHI", fmt)
    destination = struct.pack("<HHIIHHH", 1, channels, rate, rate * channels * 2, channels * 2, 16, 0)
    dll = c.WinDLL("msacm32.dll")
    handle = c.c_void_p()
    source_format, target_format = c.create_string_buffer(fmt), c.create_string_buffer(destination)
    dll.acmStreamOpen.argtypes = [c.POINTER(c.c_void_p), c.c_void_p, c.c_void_p, c.c_void_p,
                                 c.c_void_p, c.c_size_t, c.c_size_t, c.c_uint32]
    dll.acmStreamSize.argtypes = [c.c_void_p, c.c_uint32, c.POINTER(c.c_uint32), c.c_uint32]
    for name in ("acmStreamPrepareHeader", "acmStreamConvert", "acmStreamUnprepareHeader"):
        getattr(dll, name).argtypes = [c.c_void_p, c.POINTER(StreamHeader), c.c_uint32]
    dll.acmStreamClose.argtypes = [c.c_void_p, c.c_uint32]

    def check(result, operation):
        if result:
            raise SoundError(f"Windows ACM {operation} failed: MMRESULT {result}")

    check(dll.acmStreamOpen(c.byref(handle), None, source_format, target_format, None, 0, 0, 4), "open")
    prepared = False
    try:
        size = c.c_uint32()
        check(dll.acmStreamSize(handle, len(data), c.byref(size), 0), "size")
        if not size.value:
            raise SoundError("Windows ACM returned zero destination size")
        source, output = c.create_string_buffer(data), c.create_string_buffer(size.value)
        header = StreamHeader(cbStruct=c.sizeof(StreamHeader), pbSrc=c.addressof(source), cbSrcLength=len(data),
                              pbDst=c.addressof(output), cbDstLength=size.value)
        check(dll.acmStreamPrepareHeader(handle, c.byref(header), 0), "prepare")
        prepared = True
        check(dll.acmStreamConvert(handle, c.byref(header), 0x10 | 0x20), "convert")
        if header.cbSrcLengthUsed != len(data) or header.cbDstLengthUsed > size.value:
            raise SoundError("Windows ACM did not consume the complete source")
        return output.raw[:header.cbDstLengthUsed]
    finally:
        if prepared:
            dll.acmStreamUnprepareHeader(handle, c.byref(header), 0)
        dll.acmStreamClose(handle, 0)
