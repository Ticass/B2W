"""Repair ipak block layouts the T6 image streamer cannot read.

t6zm streams an ipak entry in fixed 0x40000-byte windows counted from the
entry's first 0x8000 chunk. It looks for block headers only at 128-aligned
offsets inside a window and at each window start, so block data must never
cross a window end. The BO2 mod tools linker (an OpenAssetTools build older
than the vendored writer's fix) restarts its window after a block that ends on
a chunk boundary. A later block then crosses a window end and the streamer
overruns its buffer (0xC0000005 at 0x7118B3: School's
weapon_panzerfaust3_2k_nml in mod.ff's ipak).

Block headers hold decompressed offsets only, so a skip block placed before a
misplaced block moves it to the next window start without touching image data.
"""
from __future__ import annotations

import struct
from pathlib import Path

CHUNK = 0x8000
WINDOW = 0x40000
HEADER = 128
SKIP = 0xCF
INDEX, DATA = 1, 2


def _sections(data: bytearray) -> list[tuple[int, int]]:
    """(header position, section type) of every section."""
    count = struct.unpack_from('<I', data, 12)[0]
    return [(16 + 16 * i, struct.unpack_from('<I', data, 16 + 16 * i)[0]) for i in range(count)]


def _section(data: bytearray, kind: int) -> tuple[int, int, int]:
    position = next(p for p, t in _sections(data) if t == kind)
    return struct.unpack_from('<3I', data, position + 4)


def block_length(data: bytearray, position: int) -> int:
    count = data[position + 3]
    commands = struct.unpack_from(f'<{count}I', data, position + 4)
    return (HEADER + sum(c & 0xFFFFFF for c in commands) + HEADER - 1) & ~(HEADER - 1)


def first_violation(data: bytearray) -> tuple[int, int, int] | None:
    """(index entry number, block position, window end) of the first block
    that crosses a streaming window end."""
    index_offset, _, count = _section(data, INDEX)
    data_offset = _section(data, DATA)[0]
    entries = sorted((struct.unpack_from('<I', data, index_offset + 16 * i + 8)[0], i) for i in range(count))
    for relative, entry in entries:
        start = data_offset + relative
        end = start + struct.unpack_from('<I', data, index_offset + 16 * entry + 12)[0]
        grid = start & ~(CHUNK - 1)
        position = start
        while position < end:
            window_end = grid + ((position - grid) // WINDOW + 1) * WINDOW
            length = block_length(data, position)
            if position + length > window_end:
                return entry, position, window_end
            position += length
    return None


def repair(path: Path) -> list[int]:
    """Move every block that crosses a window end to the next window start.
    Returns the inserted padding sizes (none for a readable ipak)."""
    data = bytearray(path.read_bytes())
    inserted = []
    while (violation := first_violation(data)) is not None:
        entry, position, window_end = violation
        padding = window_end - position
        if padding % CHUNK:
            # later entries keep their chunk phase only for whole chunks
            raise ValueError(f'{path.name}: block at {position:#x} is not chunk aligned; cannot move it')
        skip = struct.pack('<II', 1 << 24, SKIP << 24 | (padding - HEADER)) + bytes(padding - 8)
        data[position:position] = skip
        for header, kind in _sections(data):
            section_offset, size = struct.unpack_from('<2I', data, header + 4)
            if kind == DATA:
                struct.pack_into('<I', data, header + 8, size + padding)
            elif section_offset > position:
                struct.pack_into('<I', data, header + 4, section_offset + padding)
        struct.pack_into('<I', data, 8, len(data))
        relative = position - _section(data, DATA)[0]
        index_offset, _, count = _section(data, INDEX)
        for i in range(count):
            at = index_offset + 16 * i
            offset = struct.unpack_from('<I', data, at + 8)[0]
            if offset >= relative:
                struct.pack_into('<I', data, at + 8, offset + padding)
            elif i == entry:
                struct.pack_into('<I', data, at + 12, struct.unpack_from('<I', data, at + 12)[0] + padding)
        inserted.append(padding)
    if inserted:
        path.write_bytes(data)
    return inserted
