"""Summarize Plutonium x86 access violations from local minidumps."""
from __future__ import annotations

import io
import struct
import sys
from pathlib import Path

from minidump.minidumpfile import MinidumpFile
from minidump.streams.ContextStream import WOW64_CONTEXT


for path in map(Path, sys.argv[1:]):
    dump = MinidumpFile.parse(str(path))
    exception = dump.exception.exception_records[0]
    handle = dump.file_handle
    handle.seek(exception.ThreadContext.Rva)
    context = WOW64_CONTEXT.parse(io.BytesIO(handle.read(exception.ThreadContext.DataSize)))
    stack = struct.unpack("<8I", dump.get_reader().read(context.Esp, 32))
    print(path.name)
    print(f"  EIP={context.Eip:#x} EDI={context.Edi:#x} ESI={context.Esi:#x} ECX={context.Ecx:#x}")
    print("  stack:", " ".join(f"{value:#x}" for value in stack))
