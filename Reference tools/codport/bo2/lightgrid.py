"""
The source map's baked light grid, handed to the T6 world compiler.

The lightmap lights the *world surfaces*. The light grid lights everything else:
static models, players, dropped weapons, viewmodels. So a port with a real
lightmap and no real grid gives you correct architecture standing in correct
light, with every prop and every player lit by one flat colour -- which reads as
the props not belonging to the scene. On mp_area51 that is now 7,158 props.

The grid's *spatial index* crosses over unchanged. `rowDataStart`, `rawRowData`
and `GfxLightGridEntry` are declared identically in T5 and T6 -- same fields,
same sizes, and the zone definitions for the two arrays are byte-for-byte the
same text in both games -- so the structure that says "which sample applies at
this point in space" is carried verbatim rather than rebuilt.

**The payload, read out of both executables (2026-09-24).** Both games light a model
the same way: the renderer fills a 4x4x4 texel cube per model from the grid, the model
shader projects the surface normal onto the cube (divide by its largest component) and
samples it 1.5 cells from the centre, so only the 56 outer cells are ever read, and the
texel squared times the range is the light. The 56 grid slots are those outer cells, z
slowest, then y, then x, interior skipped -- Black Ops' R_SetLightGridColorsVec4 (OpenBLOPS)
and Black Ops II's direction table builder (t6zm.exe 0x75ef70) walk them identically.

    T5  char rgb[56][3]   each colour ONE packed 24-bit value: luminance Y (12 bits, stored
                          as its square root, range 31.875) and two 6-bit chroma shares
                          (OpenBLOPS R_DecodeLightGridColors) -- not three channels
    T6  colors  as T5's layout but each byte a channel: (b / 255)^2 * 32  (0x755810)
        coeffs  uint16 [9][3]: (u16 / 65535) * 32 - 16  (0x755740), on the plain basis
                1, x, y, z, zx, zy, xy, 3z^2 - 1, x^2 - y^2  (0x7556a0)
    T6 reads `colors` when coeffCount is 0 (0x7576d4) -- no stock map does, but the path is
    the engine's own.

So a Black Ops grid is carried **exactly**: each colour decoded as Black Ops decodes it,
re-encoded per slot as Black Ops II reads it, shipped through `colors` (side-car version
2). The earlier DC-only conversion averaged the packed bytes as if they were channels,
and wrote its coefficients as (u16 - 32768) / 1024 where T6 reads (u16 / 65535) * 32 - 16
-- half the intended value -- which is what a 5.5x "radiance peak" constant was compensating for.
Version 1 (DC only) is still written for a grid without colours.

Layout, little-endian throughout (version 2 replaces the 6-byte samples with 168-byte
T6 colour samples and sets FLAG_COLORS)::

    header    magic "CPLG", uint32 version, uint32 flags,
              uint16 mins[3], uint16 maxs[3],
              uint32 rowAxis, uint32 colAxis, uint32 sunPrimaryLightIndex,
              uint32 rowDataStartCount, uint32 rawRowDataSize,
              uint32 entryCount, uint32 sampleCount, 12 bytes reserved  (64 bytes)
    rowStart  uint16 each                                    (rowDataStartCount)
    rawRows   opaque bytes, verbatim                              (rawRowDataSize)
    entries   uint16 colorsIndex, uint8 primaryLightIndex,
              uint8 visibility                                  (4 each)
    samples   uint16 dc[3]  -- the DC term per channel, already
              in T6's offset-binary encoding                     (6 each)
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

MAGIC = b"CPLG"
VERSION = 1

HEADER = struct.Struct("<4s I I 3H 3H I I I I I I I 12x")
OUT_ROWSTART = struct.Struct("<H")
OUT_ENTRY = struct.Struct("<H B B")
OUT_SAMPLE = struct.Struct("<3H")

_EXPECTED = ((HEADER, 64), (OUT_ROWSTART, 2), (OUT_ENTRY, 4), (OUT_SAMPLE, 6))
for _layout, _size in _EXPECTED:
    if _layout.size != _size:
        raise AssertionError(
            f"light grid side-car layout is {_layout.size} bytes, expected {_size}"
        )

#: One T5 sample: 56 directions x 3 channels of unsigned byte radiance.
SOURCE_SAMPLE_DIRECTIONS = 56
SOURCE_SAMPLE_STRIDE = SOURCE_SAMPLE_DIRECTIONS * 3

#: `GfxLightGridEntry.colorsIndex` is 16-bit, so a grid with more samples than
#: this cannot be indexed and must be refused rather than silently wrapped.
MAX_SAMPLES = 0xFFFF

#: T6 decodes a coefficient as (u16 / 65535) * 32 - 16 (t6zm.exe 0x755740), so zero sits
#: at 32767.5 and the full range is +-16. Mirrors `encodeLightGridCoeff` in
#: GfxWorldLinker.cpp. (The encoding used until 2026-09-24 -- bias 0x8000, scale 1024 --
#: was read off stock data rather than the executable and came out at half the value.)
COEFF_OFFSET = 16.0
COEFF_RANGE = 32.0

#: `GfxLightGridRow`: colStart, colCount, zStart, zCount (uint16) then firstEntry
#: (uint32), followed by a variable-length lookup table. `rowDataStart` addresses
#: rows in **4-byte units**, which was read off the games rather than assumed:
#: under that reading every one of stock zm_nuked's 132 rows and zm_transit's 559
#: parses with its columns and entries inside the grid's own bounds, and under a
#: byte or 2-byte reading almost none do.
ROW_HEADER = struct.Struct("<4H I")
ROW_OFFSET_UNIT = 4

#: A `rowDataStart` of 0xFFFF marks a row the source stores nothing for. Black Ops
#: uses it (zombie_coast: 194 of 443 rows) and no stock Black Ops II map does, so
#: rather than trust T6 to honour a sentinel its own maps never exercise, those
#: rows are pointed at a real empty row record appended to the row data. An
#: unhandled 0xFFFF would have the engine read 262,140 bytes into a 14KB buffer.
ROW_START_NONE = 0xFFFF

#: Set on a version 1 side-car: only band 0 is carried and the eight directional bands
#: are zero. Recorded in the file so a later reader can tell "flat by necessity" from
#: "flat because the map really is".
FLAG_DC_ONLY = 1 << 0

#: `GfxLightGridEntry.primaryLightIndex` value meaning "no primary light applies".
#: Stock mp_nuketown_2020 uses it on 2,130 of 37,685 entries.
NO_PRIMARY_LIGHT = 255

#: Primary lights a compiled port ships: slot 0 (the reserved empty light) and
#: slot 1 (the sun). Matches `BSP_DEFAULT_LIGHT_COUNT` in the world compiler, which
#: adds placed lights on top only when the glTF carries some.
DEFAULT_PRIMARY_LIGHT_COUNT = 2


def encode_coeff(value: float) -> int:
    """
    One spherical-harmonic coefficient in T6's encoding: round((v + 16) / 32 * 65535).

    Clamped rather than allowed to wrap: a coefficient that overflows would read as its
    opposite extreme, turning a bright sample into a black one.
    """
    encoded = int(round((value + COEFF_OFFSET) / COEFF_RANGE * 65535.0))
    return max(0, min(0xFFFF, encoded))


def decode_coeff(encoded: int) -> float:
    """What T6 reads back from `encode_coeff` (before its 1/weight scale)."""
    return encoded / 65535.0 * COEFF_RANGE - COEFF_OFFSET


@dataclass(slots=True)
class LightGridSample:
    """One grid sample: its DC term per channel (version 1), its 56 T6 colours (version 2), or its
    nine encoded coefficients per channel as read back from a version 3 side-car."""

    dc: tuple[int, int, int]
    colors: bytes | None = None
    coeffs: bytes | None = None


@dataclass(slots=True)
class LightGridEntry:
    """Which sample applies at one grid point. Identical layout in both games."""

    colors_index: int
    primary_light_index: int
    visibility: int


@dataclass
class LightGrid:
    """A source light grid, converted and ready to write."""

    mins: tuple[int, int, int] = (0, 0, 0)
    maxs: tuple[int, int, int] = (0, 0, 0)
    row_axis: int = 0
    col_axis: int = 1
    sun_primary_light_index: int = 0
    row_data_start: list[int] = field(default_factory=list)
    raw_row_data: bytes = b""
    entries: list[LightGridEntry] = field(default_factory=list)
    samples: list[LightGridSample] = field(default_factory=list)
    flags: int = FLAG_DC_ONLY
    notes: list[str] = field(default_factory=list)
    remapped_primary_lights: int = 0
    """Entries whose primary light the port does not ship, now naming none."""
    remapped_empty_rows: int = 0
    """Rows the source marked absent, now pointing at a real empty row record."""

    @property
    def usable(self) -> bool:
        """
        Whether this grid can replace the fabricated uniform one.

        All three parts have to be present together. Samples with no entries
        cannot be located in space, and entries with no samples index nothing --
        either way the result is worse than the flat grid it would replace.
        """
        return bool(self.samples and self.entries and self.raw_row_data)

    def summary(self) -> dict[str, Any]:
        return {
            "samples": len(self.samples),
            "entries": len(self.entries),
            "row_starts": len(self.row_data_start),
            "raw_row_bytes": len(self.raw_row_data),
            "dc_only": bool(self.flags & FLAG_DC_ONLY),
            "remapped_primary_lights": self.remapped_primary_lights,
            "remapped_empty_rows": self.remapped_empty_rows,
            "notes": list(self.notes),
        }


#: Side-car version 2: whole T6 colour samples (see `encode_t6_colors`).
VERSION_COLORS = 2
FLAG_COLORS = 1 << 1
T6_COLOR_SAMPLE_SIZE = SOURCE_SAMPLE_DIRECTIONS * 3

#: Black Ops' grid radiance ceiling: the decoded luminance is Y^2 * 31.875.
T5_GRID_RANGE = 31.875
#: Black Ops II's: t6zm.exe decodes a colour byte as (b / 255)^2 * 32 (0x755810) and fills the
#: model-lighting cube with sqrt(min(c, 32) / 32) * 255 (0x755600).
T6_GRID_RANGE = 32.0


def decode_t5_colors(block: bytes | memoryview) -> list[tuple[float, float, float]]:
    """
    Black Ops' 56 grid colours as linear radiance, exactly as its renderer decodes them.

    Each colour is one packed 24-bit value, not three channels (OpenBLOPS rb_light.cpp,
    R_DecodeLightGridColors): packed = b2 << 16 | b1 << 8 | b0; luminance Y = bits 12-23,
    stored as its square root, so Y' = (Y / 4095)^2 * 31.875; u = bits 6-11 and w = bits 0-5
    are the red and blue shares of Y' = R/4 + G/2 + B/4. Read as three channels -- what this
    converter did until 2026-09-24 -- the bytes mix luminance and chroma bits, which is why
    their average looked like "normalised or log-encoded" data needing a 5.5x fudge.
    """
    out = []
    for i in range(len(block) // 3):
        b0, b1, b2 = block[i * 3], block[i * 3 + 1], block[i * 3 + 2]
        packed = (b2 << 16) | (b1 << 8) | b0
        y = ((packed >> 12) & 0xFFF) / 4095.0
        y = y * y * T5_GRID_RANGE
        u = ((packed >> 6) & 0x3F) / 63.0
        w = (packed & 0x3F) / 63.0
        red = u * y / 0.25
        blue = w * y / 0.25
        green = (y - red * 0.25 - blue * 0.25) / 0.5
        out.append((red, green, blue))
    return out


def encode_t6_colors(colors: list[tuple[float, float, float]]) -> bytes:
    """
    56 linear colours in T6's grid colour encoding: byte = round(255 * sqrt(c / 32)).

    T6 decodes (b / 255)^2 * 32, the same radiance Black Ops lit the model with (its shader
    multiplies the cube texel squared by 31.875, T6's by 32 -- this absorbs the difference).
    Negative green, which Black Ops' decode yields for saturated red/blue, is clamped at 0
    exactly as Black Ops' own cube fill clamps it through the square root.
    """
    out = bytearray()
    for rgb in colors:
        for value in rgb:
            value = max(0.0, min(T6_GRID_RANGE, value))
            out.append(max(0, min(255, int(round(255.0 * (value / T6_GRID_RANGE) ** 0.5)))))
    return bytes(out)


def _dc_from_source_sample(colors: list[tuple[float, float, float]]) -> tuple[int, int, int]:
    """
    The DC term of one sample, for the version 1 side-car: the mean of its 56 decoded
    colours, which is what T6's band 0 (basis 1) contributes in every direction.
    """
    return tuple(
        encode_coeff(sum(c[channel] for c in colors) / len(colors)) for channel in range(3)
    )


def collect_light_grid(
    package: Any, primary_light_count: int = DEFAULT_PRIMARY_LIGHT_COUNT
) -> LightGrid:
    """
    Read and convert the light grid out of a decoded world package.

    Returns an unusable grid rather than raising when the source has none: a map
    with no baked grid is normal, and the caller falls back to the flat one.

    `primary_light_count` is how many primary lights the compiled port will have;
    entries naming any other light are remapped to none (see below).
    """
    grid = LightGrid()
    section = (getattr(package, "gfx", None) or {}).get("lightGrid") or {}
    if not section:
        return grid

    directory = Path(package.directory)

    def _triple(key: str) -> tuple[int, int, int]:
        values = list(section.get(key) or (0, 0, 0))
        while len(values) < 3:
            values.append(0)
        return (int(values[0]), int(values[1]), int(values[2]))

    def _int(key: str, default: int) -> int:
        """
        A source value that may legitimately be zero.

        `int(section.get(key) or default)` reads a real 0 as the default, because 0 is falsy.
        That cost `colAxis`: Black Ops' zombie_coast has `colAxis 0`, the side-car recorded 1,
        and the grid then named axis 1 twice. No stock Black Ops II map does that -- measured
        on mp_nuketown_2020 (1/0), mp_dockside (0/1) and mp_raid (1/0) -- and the engine walks
        the grid by these two axes, so every lookup asked the wrong question. Static surfaces
        kept their baked lightmap and looked right; everything lit from the grid (zombies,
        players, dropped weapons, the mystery box) rendered black. Measured in game
        2026-09-17.
        """
        value = section.get(key)
        return default if value is None else int(value)

    grid.mins = _triple("mins")
    grid.maxs = _triple("maxs")
    grid.row_axis = _int("rowAxis", 0)
    grid.col_axis = _int("colAxis", 1)
    grid.sun_primary_light_index = _int("sunPrimaryLightIndex", 0)

    sample_count = int(section.get("colorCount") or 0)
    sample_stride = int(section.get("colorStride") or 0)
    entry_count = int(section.get("entryCount") or 0)

    if sample_count > MAX_SAMPLES:
        grid.notes.append(
            f"{sample_count} grid samples exceeds the 16-bit index limit of "
            f"{MAX_SAMPLES}; the grid was left flat rather than wrapped"
        )
        return LightGrid(notes=grid.notes)

    if sample_stride not in (0, SOURCE_SAMPLE_STRIDE):
        grid.notes.append(
            f"grid samples are {sample_stride} bytes, expected {SOURCE_SAMPLE_STRIDE}; "
            "this is not a layout this converter understands"
        )
        return LightGrid(notes=grid.notes)

    # -- the samples ------------------------------------------------------
    colors_path = directory / str(section.get("colorFile") or "")
    if sample_count and colors_path.is_file():
        blob = colors_path.read_bytes()
        expected = sample_count * SOURCE_SAMPLE_STRIDE
        if len(blob) < expected:
            grid.notes.append(
                f"{colors_path.name} holds {len(blob)} bytes, expected {expected}"
            )
            return LightGrid(notes=grid.notes)
        view = memoryview(blob)
        grid.samples = []
        for i in range(sample_count):
            block = view[i * SOURCE_SAMPLE_STRIDE:(i + 1) * SOURCE_SAMPLE_STRIDE]
            colors = decode_t5_colors(block)
            grid.samples.append(LightGridSample(_dc_from_source_sample(colors), encode_t6_colors(colors)))
        grid.flags = FLAG_COLORS

    # -- which sample applies where ---------------------------------------
    entries_path = directory / str(section.get("entryFile") or "")
    if entry_count and entries_path.is_file():
        blob = entries_path.read_bytes()
        expected = entry_count * OUT_ENTRY.size
        if len(blob) < expected:
            grid.notes.append(
                f"{entries_path.name} holds {len(blob)} bytes, expected {expected}"
            )
            return LightGrid(notes=grid.notes)
        grid.entries = [
            LightGridEntry(index, light, vis)
            for index, light, vis in OUT_ENTRY.iter_unpack(blob[:expected])
        ]

    # An entry naming a primary light the port does not ship crashes the renderer.
    # Black Ops II draws a model lit from a grid cell with that cell's primary
    # light, looks the light up by this byte and dereferences its `def` without a
    # check: `0x786960` passes GfxLight::def (+0x150) to `0x786210`, which faults
    # on `mov eax,[eax+4]` -- the "crashes when I walk into that area" crash. The
    # source grid names Black Ops' own 27 lights; the port ships `primary_light_count`.
    # 0 is the engine's "no primary light" slot and returns before any lookup.
    for entry in grid.entries:
        index = entry.primary_light_index
        if index != NO_PRIMARY_LIGHT and index >= primary_light_count:
            entry.primary_light_index = 0
            grid.remapped_primary_lights += 1
    if grid.remapped_primary_lights:
        grid.notes.append(
            f"{grid.remapped_primary_lights} grid entries named a primary light the port "
            f"does not ship (only {primary_light_count} exist); they now name none, so "
            "models there lose that light's dynamic contribution instead of crashing"
        )

    # An entry pointing past the sample array would read someone else's memory.
    out_of_range = sum(1 for e in grid.entries if e.colors_index >= len(grid.samples))
    if out_of_range:
        grid.notes.append(
            f"{out_of_range} grid entries index a sample that does not exist; "
            "the grid was left flat rather than shipped corrupt"
        )
        return LightGrid(notes=grid.notes)

    # -- the spatial index, carried verbatim ------------------------------
    raw_path = directory / str(section.get("rawRowFile") or "")
    if raw_path.is_file():
        grid.raw_row_data = raw_path.read_bytes()

    rows_path = directory / str(section.get("rowFile") or "")
    if rows_path.is_file():
        blob = rows_path.read_bytes()
        grid.row_data_start = [v for (v,) in OUT_ROWSTART.iter_unpack(
            blob[:len(blob) - (len(blob) % OUT_ROWSTART.size)]
        )]

    # The engine derives this count from the bounds, so a mismatch means the
    # bounds and the table disagree and lookups would land off the end.
    expected_starts = grid.maxs[grid.row_axis] - grid.mins[grid.row_axis] + 1
    if grid.row_data_start and len(grid.row_data_start) != expected_starts:
        grid.notes.append(
            f"the row table has {len(grid.row_data_start)} entries but the grid bounds "
            f"imply {expected_starts}; the grid was left flat"
        )
        return LightGrid(notes=grid.notes)

    if grid.row_data_start and grid.raw_row_data:
        grid = _resolve_empty_rows(grid)
        if not grid.usable:
            return grid

    if grid.usable:
        grid.notes.append(
            f"{len(grid.samples)} samples carried with all 56 directional colours, decoded as "
            "Black Ops decodes them and re-encoded as Black Ops II reads them"
        )
    return grid


def _resolve_empty_rows(grid: LightGrid) -> LightGrid:
    """
    Give every row a row record the engine can actually read.

    Rows the source marks absent carry `ROW_START_NONE` instead of an offset. The
    row is genuinely empty -- no columns, no entries -- so it is expressed as one
    appended record saying exactly that, shared by all of them, and the sentinel
    never reaches T6.

    Every other offset is checked while we are here: one pointing past the row
    data would have the engine read whatever follows it as a row header, and a
    grid that indexes wrongly is worse than the flat one it replaces.
    """
    raw = bytearray(grid.raw_row_data)
    empty_offset: int | None = None
    resolved: list[int] = []

    for index, start in enumerate(grid.row_data_start):
        if start == ROW_START_NONE:
            if empty_offset is None:
                while len(raw) % ROW_OFFSET_UNIT:
                    raw.append(0)
                empty_offset = len(raw) // ROW_OFFSET_UNIT
                # colStart, colCount, zStart, zCount, firstEntry -- all zero: an
                # empty row, which is what the sentinel meant.
                raw += ROW_HEADER.pack(0, 0, 0, 0, 0)
                if empty_offset > ROW_START_NONE - 1:
                    grid.notes.append(
                        "the row data is too large to address an appended empty row; "
                        "the grid was left flat"
                    )
                    return LightGrid(notes=grid.notes)
            resolved.append(empty_offset)
            grid.remapped_empty_rows += 1
            continue

        end = start * ROW_OFFSET_UNIT + ROW_HEADER.size
        if end > len(grid.raw_row_data):
            grid.notes.append(
                f"row {index} starts at byte {start * ROW_OFFSET_UNIT} of "
                f"{len(grid.raw_row_data)} bytes of row data, which is not a row; "
                "the grid was left flat"
            )
            return LightGrid(notes=grid.notes)
        resolved.append(start)

    grid.row_data_start = resolved
    grid.raw_row_data = bytes(raw)
    if grid.remapped_empty_rows:
        grid.notes.append(
            f"{grid.remapped_empty_rows} of {len(resolved)} rows hold nothing in the "
            "source and now point at an appended empty row record rather than at "
            "Black Ops' 0xFFFF sentinel"
        )
    return grid


def write_light_grid(grid: LightGrid, path: Path) -> Path | None:
    """Write the side-car. Returns None when there is nothing usable to write."""
    if not grid.usable:
        return None

    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    colors = bool(grid.flags & FLAG_COLORS) and all(s.colors is not None for s in grid.samples)
    # The colours go out as fitted coefficients (version 3), which light the model SH as well.
    with path.open("wb") as stream:
        stream.write(
            HEADER.pack(
                MAGIC,
                VERSION_COEFFS if colors else VERSION,
                (grid.flags & ~FLAG_COLORS) | FLAG_COEFFS if colors else grid.flags & ~FLAG_COLORS,
                *grid.mins,
                *grid.maxs,
                grid.row_axis,
                grid.col_axis,
                grid.sun_primary_light_index,
                len(grid.row_data_start),
                len(grid.raw_row_data),
                len(grid.entries),
                len(grid.samples),
            )
        )
        for value in grid.row_data_start:
            stream.write(OUT_ROWSTART.pack(value))
        stream.write(grid.raw_row_data)
        for entry in grid.entries:
            stream.write(
                OUT_ENTRY.pack(
                    entry.colors_index,
                    entry.primary_light_index & 0xFF,
                    entry.visibility & 0xFF,
                )
            )
        for sample in grid.samples:
            stream.write(fit_t6_coeffs(sample.colors) if colors else OUT_SAMPLE.pack(*sample.dc))
    return path


def read_light_grid(path: Path) -> LightGrid:
    """Read a side-car back. Used by the tests to prove the round trip."""
    path = Path(path)
    blob = path.read_bytes()
    if len(blob) < HEADER.size:
        raise ValueError(f"{path}: too short to hold a light grid header")

    (
        magic,
        version,
        flags,
        min_x,
        min_y,
        min_z,
        max_x,
        max_y,
        max_z,
        row_axis,
        col_axis,
        sun_index,
        row_start_count,
        raw_row_size,
        entry_count,
        sample_count,
    ) = HEADER.unpack_from(blob, 0)

    if magic != MAGIC:
        raise ValueError(f"{path}: not a light grid side-car")
    if version not in (VERSION, VERSION_COLORS, VERSION_COEFFS):
        raise ValueError(f"{path}: version {version}, expected {VERSION}, {VERSION_COLORS} or {VERSION_COEFFS}")
    colors = version == VERSION_COLORS and bool(flags & FLAG_COLORS)
    coeffs = version == VERSION_COEFFS and bool(flags & FLAG_COEFFS)

    grid = LightGrid(
        mins=(min_x, min_y, min_z),
        maxs=(max_x, max_y, max_z),
        row_axis=row_axis,
        col_axis=col_axis,
        sun_primary_light_index=sun_index,
        flags=flags,
    )

    offset = HEADER.size
    for _ in range(row_start_count):
        (value,) = OUT_ROWSTART.unpack_from(blob, offset)
        grid.row_data_start.append(value)
        offset += OUT_ROWSTART.size

    grid.raw_row_data = bytes(blob[offset:offset + raw_row_size])
    offset += raw_row_size

    for _ in range(entry_count):
        index, light, vis = OUT_ENTRY.unpack_from(blob, offset)
        grid.entries.append(LightGridEntry(index, light, vis))
        offset += OUT_ENTRY.size

    for _ in range(sample_count):
        if coeffs:
            sample = LightGridSample((0, 0, 0))
            sample.coeffs = bytes(blob[offset:offset + COEFF_SAMPLE_SIZE])
            grid.samples.append(sample)
            offset += COEFF_SAMPLE_SIZE
        elif colors:
            grid.samples.append(LightGridSample((0, 0, 0), bytes(blob[offset:offset + T6_COLOR_SAMPLE_SIZE])))
            offset += T6_COLOR_SAMPLE_SIZE
        else:
            grid.samples.append(LightGridSample(OUT_SAMPLE.unpack_from(blob, offset)))
            offset += OUT_SAMPLE.size

    return grid


#: Side-car version 3: every sample as Black Ops II's nine spherical-harmonic coefficients per
#: channel (`uint16 coeffs[9][3]`, encoded), fitted to the 56 directional colours.
#:
#: Why not the colours (version 2): on that path t6zm.exe fills the model-lighting cube but
#: memsets the model's lighting SH to zero (R_SetLightGridColorsFromIndex). The lit model shaders
#: scale their probe reflection by gridLightingSH(normal) / reflectionLightingSH(normal), clamped at
#: 0.1, so with a zero SH every model reflected at a tenth of its strength -- zombie_coast's M1911,
#: silver in Black Ops from its reflection alone, drew black (v83-v102). The coefficient path fills
#: both the cube and the SH from the same numbers, as every stock map does.
VERSION_COEFFS = 3
FLAG_COEFFS = 1 << 2
COEFF_SAMPLE_SIZE = 9 * 3 * 2


def grid_basis_dirs() -> "np.ndarray":
    """t6zm.exe's 56 light-grid directions: the outer cells of a 4x4x4 cube (z slowest, then y,
    then x, interior skipped), from the cube's centre -- the colour slots' order."""
    import numpy as np

    dirs = []
    for z in range(4):
        for y in range(4):
            for x in range(4):
                if 0 < x < 3 and 0 < y < 3 and 0 < z < 3:
                    continue
                v = np.array([x - 1.5, y - 1.5, z - 1.5])
                dirs.append(v / np.linalg.norm(v))
    return np.array(dirs)


def sh_basis(dirs: "np.ndarray") -> "np.ndarray":
    """R_CalculateLightGridColorFromCoeffs's basis: 1, x, y, z, zx, zy, xy, 3z^2 - 1, x^2 - y^2."""
    import numpy as np

    x, y, z = dirs[:, 0], dirs[:, 1], dirs[:, 2]
    return np.stack([np.ones_like(x), x, y, z, z * x, z * y, x * y, 3 * z * z - 1, x * x - y * y], axis=1)


def fit_t6_coeffs(t6_colors: bytes) -> bytes:
    """A version 2 colour sample (56 x RGB bytes, (b/255)^2 * 32) as nine encoded coefficients per
    channel, least-squares over the 56 directions; `uint16 coeffs[9][3]`, band-major."""
    import numpy as np

    light = (np.frombuffer(t6_colors, dtype=np.uint8).reshape(56, 3).astype(np.float64) / 255.0) ** 2 * T6_GRID_RANGE
    basis = _basis_pinv()
    coeffs = basis @ light                              # 9 x 3
    out = bytearray()
    for band in range(9):
        for channel in range(3):
            out += struct.pack("<H", encode_coeff(float(coeffs[band, channel])))
    return bytes(out)


_PINV = None


def _basis_pinv():
    global _PINV
    if _PINV is None:
        import numpy as np

        _PINV = np.linalg.pinv(sh_basis(grid_basis_dirs()))
    return _PINV
