"""Read lossless compiled WaW lighting; no invented flat-light replacement.

The source grid preserves T4 trace flags and palette samples byte for byte.
T6 grid visibility semantics and directional lightmap encoding still require
translation/roundtrip validation before these data can be used at runtime.
"""
from dataclasses import dataclass
from pathlib import Path
import struct
import math
import json


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

    def points(self):
        """Yield (cell xyz, entry index) from the shared T4/T6 packed rows.

        Native readers: T4 sub_71C3D0, T6 sub_7584D0. Row offsets are
        four-byte units; 0xFFFF denotes an empty row. Runs with zero height
        have only two bytes, so treating every run as three bytes corrupts
        the lookup after the first empty column span.
        """
        count = len(self.entries) // 4
        for row_index, start in enumerate(self.row_starts):
            if start == 0xFFFF:
                continue
            cursor = start * 4
            if cursor + 12 > len(self.packed_rows):
                raise LightingError("light-grid row offset lies outside packed data")
            col_start, col_count, z_start, z_count, entry = struct.unpack_from("<4HI", self.packed_rows, cursor)
            cursor += 12
            col = 0
            while col < col_count:
                if cursor + 2 > len(self.packed_rows):
                    raise LightingError("truncated light-grid run")
                span, height = self.packed_rows[cursor:cursor + 2]
                cursor += 2
                if not span or col + span > col_count:
                    raise LightingError("invalid light-grid column run")
                offset = 0
                if height:
                    size = 2 if z_count > 255 else 1
                    if cursor + size > len(self.packed_rows):
                        raise LightingError("truncated light-grid vertical offset")
                    offset = int.from_bytes(self.packed_rows[cursor:cursor + size], "little")
                    cursor += size
                    if offset + height > z_count or entry + span * height > count:
                        raise LightingError("light-grid run exceeds its vertical or entry bounds")
                for c in range(span):
                    for z in range(height):
                        xyz = [0, 0, z_start + offset + z]
                        xyz[self.row_axis] = self.mins[self.row_axis] + row_index
                        xyz[self.col_axis] = col_start + col + c
                        yield tuple(xyz), entry + c * height + z
                entry += span * height
                col += span

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
    if row_axis > 1 or col_axis > 1 or row_axis == col_axis or maxs[row_axis] < mins[row_axis]:
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
    grid = LightGrid(bool(regions), sun, mins, maxs, row_axis, col_axis, row_starts, packed, entry_data, palette)
    for _ in grid.points():
        pass
    return grid


def stage_grid(source: Path, destination: Path) -> dict:
    """Stage T6's packed grid, preserving positions and directional samples.

    Native T6 sub_755810 decodes bytes as 32*(c/255)^2. WaW's model
    lighting program uses 2*c/255 in gamma space (see shaderruntime), so
    T6 needs c/sqrt(8) to reproduce its linear equivalent 4*(c/255)^2.
    T4 needsTrace is a corner trace mask, NOT T6's visibility byte.
    Primary-light selection already records which light reaches each point;
    visibility is full for that selected light. Runtime corner obstruction
    checks performed by T4 remain a separate compatibility requirement.
    """
    grid = read_grid(source)
    entries = bytearray()
    trace_points = 0
    for color, primary, needs_trace in struct.iter_unpack("<HBB", grid.entries):
        entries.extend(struct.pack("<HBB", color, primary, 255 if primary else 0))
        trace_points += bool(needs_trace)
    palette = bytes(round(c / math.sqrt(8)) for c in grid.colors)
    header = HEADER.pack(b"W2BT6LG1", 1, 0, grid.sun_index, *grid.mins, *grid.maxs,
                         grid.row_axis, grid.col_axis, len(grid.row_starts), len(grid.packed_rows),
                         len(entries) // 4, len(palette) // 168)
    destination.write_bytes(header + struct.pack(f"<{len(grid.row_starts)}H", *grid.row_starts)
                            + grid.packed_rows + entries + palette)
    return {**grid.report(), "status": "runtime_T6_grid", "palette_encoding": "WaW_gamma2_to_T6_linear32",
            "source_trace_mask_points": trace_points,
            "corner_obstruction_checks": "T4_runtime_traces_not_representable_in_T6_visibility"}


def stage_primary_lights(source: Path, destination: Path) -> set[str]:
    data = json.loads(source.read_text(encoding="utf-8"))
    definitions = set()
    for light in data["lights"]:
        if light["type"] not in (0, 1, 2, 3):
            raise LightingError(f"unsupported WaW primary light type {light['type']}")
        if light["type"] == 3:  # WaW omni -> T6 omni, which moved from 3 to 5
            light["type"] = 5
        if light["defName"]:
            definitions.add(light["defName"])
            light["defName"] = "waw_light/" + light["defName"]
    destination.write_text(json.dumps(data, indent=1) + "\n", encoding="utf-8")
    return definitions
