"""Disassemble code bytes captured in a minidump: dis.py <dmp> <addr> [addr...]"""
import struct
import sys

import capstone

d = open(sys.argv[1], "rb").read()
sig, ver, nstreams, rva = struct.unpack_from("<IIII", d, 0)
streams = {}
for i in range(nstreams):
    t, size, srva = struct.unpack_from("<III", d, rva + i * 12)
    streams.setdefault(t, (size, srva))


def read(addr, length):
    if 5 in streams:
        size, r = streams[5]
        n = struct.unpack_from("<I", d, r)[0]
        for i in range(n):
            start, dsize, drva = struct.unpack_from("<QII", d, r + 4 + i * 16)
            if start <= addr and addr + length <= start + dsize:
                return d[drva + addr - start: drva + addr - start + length]
    if 9 in streams:
        size, r = streams[9]
        n, base_rva = struct.unpack_from("<QQ", d, r)
        off = base_rva
        for i in range(n):
            start, dsize = struct.unpack_from("<QQ", d, r + 16 + i * 16)
            if start <= addr and addr + length <= start + dsize:
                return d[off + addr - start: off + addr - start + length]
            off += dsize
    return None


md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
before = int(sys.argv[-1], 16) if sys.argv[-1].startswith("b") is False and False else 0x50
for a in [int(x, 16) for x in sys.argv[2:]]:
    start = a - before
    b = read(start, before + 0x30)
    print("==", hex(a), "" if b else "no memory")
    if not b:
        continue
    # resync: try offsets until an instruction lands exactly on a
    for skew in range(0, 16):
        ins = list(md.disasm(b[skew:], start + skew))
        if any(i.address == a for i in ins):
            break
    for i in ins:
        print(hex(i.address), i.mnemonic, i.op_str, "<==" if i.address == a else "")
