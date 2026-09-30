"""Read lossless compiled WaW lighting; no invented flat-light replacement.

The source grid preserves T4 trace flags and palette samples byte for byte.
T6 grid visibility semantics and directional lightmap encoding still require
translation/roundtrip validation before these data can be used at runtime.
"""
from dataclasses import dataclass
from pathlib import Path
import struct


class LightingError(ValueError):
    pass


HEADER = struct.Struct("<8sIII3H3H6I")


@dataclass(frozen=True)
class LightGrid:
    has_light_regions: bool
    sun_index: int
    mins: tuple[int, ...]
    maxs: tuple[int, ...]
    row_axis: int
    col_axis: int
    row_starts: tuple[int, ...]
    packed_rows: bytes
    entries: bytes
    colors: bytes

    def report(self) -> dict:
        return {"bounds": [self.mins, self.maxs], "axes": [self.row_axis, self.col_axis],
                "rows": len(self.row_starts), "packed_row_bytes": len(self.packed_rows),
                "entries": len(self.entries) // 4, "colors": len(self.colors) // 168,
                "sun_index": self.sun_index, "status": "preserved_T4_not_runtime_translated"}


def read_grid(path: Path) -> LightGrid:
    data = path.read_bytes()
    if len(data) < HEADER.size:
        raise LightingError("truncated WaW light-grid header")
    magic, version, regions, sun, *values = HEADER.unpack_from(data)
    if magic != b"W2BLGRD1" or version != 1 or regions not in (0, 1):
        raise LightingError("unsupported WaW light-grid header")
    mins, maxs = tuple(values[:3]), tuple(values[3:6])
    row_axis, col_axis, rows, raw_size, entries, colors = values[6:]
    if row_axis > 2 or col_axis > 2 or maxs[row_axis] < mins[row_axis]:
        raise LightingError("invalid WaW light-grid axes/bounds")
    if rows not in (0, maxs[row_axis] - mins[row_axis] + 1):
        raise LightingError("light-grid row count disagrees with bounds")
    expected = HEADER.size + rows * 2 + raw_size + entries * 4 + colors * 168
    if len(data) != expected:
        raise LightingError(f"WaW light-grid length {len(data)} != {expected}")
    cursor = HEADER.size
    row_starts = struct.unpack_from(f"<{rows}H", data, cursor)
    cursor += rows * 2
    packed = data[cursor:cursor + raw_size]
    cursor += raw_size
    entry_data = data[cursor:cursor + entries * 4]
    cursor += entries * 4
    palette = data[cursor:]
    if any(index >= colors for index, _, _ in struct.iter_unpack("<HBB", entry_data)):
        raise LightingError("WaW light-grid entry references an absent palette")
    return LightGrid(bool(regions), sun, mins, maxs, row_axis, col_axis, row_starts, packed, entry_data, palette)
