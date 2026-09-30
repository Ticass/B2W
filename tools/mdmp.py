"""Minimal minidump reader: exception address -> module + offset, plus a raw
stack scan for return addresses inside known modules."""
import struct
import sys

d = open(sys.argv[1], "rb").read()
sig, ver, nstreams, rva = struct.unpack_from("<IIII", d, 0)
streams = {}
for i in range(nstreams):
    t, size, srva = struct.unpack_from("<III", d, rva + i * 12)
    streams.setdefault(t, (size, srva))


def mdstring(r):
    n = struct.unpack_from("<I", d, r)[0]
    return d[r + 4:r + 4 + n].decode("utf-16-le", "replace")


mods = []
size, r = streams[4]
n = struct.unpack_from("<I", d, r)[0]
for i in range(n):
    base, msize, _cs, _ts, name_rva = struct.unpack_from("<QIIII", d, r + 4 + i * 108)
    mods.append((base, msize, mdstring(name_rva)))


def where(addr):
    for base, msize, name in mods:
        if base <= addr < base + msize:
            return f"{name.split(chr(92))[-1]}+0x{addr - base:X}"
    return None


size, r = streams[6]
tid = struct.unpack_from("<I", d, r)[0]
code, flags, rec, addr = struct.unpack_from("<IIQQ", d, r + 8)
nparams = struct.unpack_from("<I", d, r + 8 + 24)[0]
params = struct.unpack_from("<15Q", d, r + 8 + 32)
print(f"thread {tid} code 0x{code:X} at 0x{addr:X} = {where(addr)}; params {[hex(p) for p in params[:nparams]]}")
ctx_size, ctx_rva = struct.unpack_from("<II", d, r + 8 + 152)
# x86 CONTEXT: Esp at 0xC4, Ebp at 0xB4, Eip at 0xB8
esp = struct.unpack_from("<I", d, ctx_rva + 0xC4)[0]
eip = struct.unpack_from("<I", d, ctx_rva + 0xB8)[0]
regs = {n: struct.unpack_from("<I", d, ctx_rva + o)[0] for n, o in
        (("edi", 0x9C), ("esi", 0xA0), ("ebx", 0xA4), ("edx", 0xA8), ("ecx", 0xAC), ("eax", 0xB0), ("ebp", 0xB4))}
print(f"eip 0x{eip:X} ({where(eip)}) esp 0x{esp:X}", {k: hex(v) for k, v in regs.items()})

# memory list (stream 5) or memory64 (stream 9) to scan the stack
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


stack = read(esp, 0x800)
if stack:
    for i in range(0, len(stack), 4):
        v = struct.unpack_from("<I", stack, i)[0]
        w = where(v)
        if w and not w.lower().startswith(("ntdll", "kernel", "ucrt", "msvcp", "vcruntime", "win32u", "user32", "kernelbase")):
            print(f"  [esp+0x{i:X}] 0x{v:X} {w}")
else:
    print("stack memory not in dump")
