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


# WaW grid cells (T4/T6 share the convention): 32 x 32 x 64 units, cell index
# = floor((position + 131072) / size). Measured: Der Riese static models land
# in the grid's points only with this offset.
GRID_CELL = (32.0, 32.0, 64.0)
GRID_OFFSET = 131072.0


# WaW drops a light grid corner whose trilinear weight is below this
# (sub_71C8E0, dword_8D03CC).
CORNER_MIN_WEIGHT = 0.001
# WaW light grid traces: point trace through world model 0, contents mask
# (sub_71C770 -> sub_58EA70(..., 0, 0x2001)).
GRID_TRACE_MASK = 0x2001
# ComPrimaryLight.type
LIGHT_NONE, LIGHT_SUN, LIGHT_SPOT, LIGHT_OMNI = 0, 1, 2, 3


def _corners(grid: LightGrid, points: dict, entries: list, origin) -> list[tuple]:
    """The eight grid corners around ``origin`` in WaW's order (sub_71C8E0):
    bit 4 steps the row axis, bit 2 the column axis, bit 1 z. Each is
    (corner bit index, weight, world position, entry or None)."""
    cell = [(origin[i] + GRID_OFFSET) / GRID_CELL[i] for i in range(3)]
    low = [math.floor(c) for c in cell]
    frac = [c - l for c, l in zip(cell, low)]
    axes = (grid.row_axis, grid.col_axis, 2)
    corners = []
    for corner in range(8):
        step = [0, 0, 0]
        for bit, axis in zip((4, 2, 1), axes):
            if corner & bit:
                step[axis] = 1
        key = tuple(l + o for l, o in zip(low, step))
        weight = math.prod(f if o else 1.0 - f for f, o in zip(frac, step))
        position = [key[i] * GRID_CELL[i] - GRID_OFFSET for i in range(3)]
        entry = points.get(key)
        corners.append((corner, weight, position, None if entry is None else entries[entry]))
    return corners


def _corner_visible(tracer, origin, corner) -> bool:
    """sub_71C770: trace from the lighting origin to 1% short of the corner."""
    end = [c + 0.01 * (o - c) for o, c in zip(origin, corner)]
    return tracer.visible(origin, end)


def _dominant_light(corners: list[tuple], tracer, origin) -> tuple[int, list[tuple]]:
    """sub_71C8E0: corners flagged in needsTrace whose trace is blocked drop
    out once any corner is reachable; the dominant primary light prefers a
    real light over 255 (none recorded) over 0, then the larger weight."""
    kept, found, light, best = [], False, 0, 0.0
    for corner, weight, position, entry in corners:
        if entry is None or weight < CORNER_MIN_WEIGHT:
            continue
        _, primary, needs_trace = entry
        blocked = bool(needs_trace & (1 << corner)) and not _corner_visible(tracer, origin, position)
        if blocked and found:
            continue
        if not blocked and not found:
            found, best, light = True, weight, primary
            kept = [(corner, weight, position, entry)]
            continue
        kept.append((corner, weight, position, entry))
        if light == 0 or (primary != 0 and (light == 255 or (primary != 255 and weight > best))):
            best, light = weight, primary
    return light, kept


def _in_light_volume(light: dict, point) -> bool:
    """sub_71D340 -> sub_5953C0: a corner without a light counts against a
    model's visibility when it lies inside the light: the sun always, a spot
    or omni within its radius and (spot) its cone. Forward is -dir."""
    if light["type"] == LIGHT_NONE:
        return False
    if light["type"] == LIGHT_SUN:
        return True
    to_light = [o - p for o, p in zip(light["origin"], point)]
    distance2 = sum(v * v for v in to_light)
    if distance2 >= light["radius"] ** 2:
        return False
    if light["type"] == LIGHT_OMNI:
        return True
    cos_outer = light["cosHalfFovOuter"]
    if not -cos_outer < light["rotationLimit"]:
        return True
    if light["rotationLimit"] != 1.0:
        raise LightingError(f"primary light {light.get('index')}: rotationLimit {light['rotationLimit']} "
                            "needs WaW's expanded cone (sub_595370), not ported")
    along = sum(d * v for d, v in zip(light["dir"], to_light))
    return along > 0.0 and cos_outer * cos_outer * distance2 <= along * along


def static_model_samples(grid: LightGrid, models: list[dict], tracer, lights: list[dict]) -> tuple[list[tuple], bytes, dict]:
    """Bake each static model's grid lighting for T6, as WaW computes it.

    T6 lights a static model only from its draw instance: colorsIndex picks
    the grid palette entry it samples (sub_75C4F0 -> sub_755CE0) and
    visibility is its primary light's visibility (sub_75BF90:
    gridLightingCoordsAndVis.w = visibility / 255). WaW computes them at
    runtime (sub_72D520 -> sub_71D440) at the model's lighting origin:
    - the light: the grid's dominant corner light (sub_71C8E0); 255 means
      the sun; on a grid with light regions any other light keeps the
      model's compiled primaryLightIndex;
    - every corner whose trace from the origin is clear (sub_71C770) blends
      its palette by weight;
    - visibility = weight of clear corners selecting the light / (that +
      weight of clear corners selecting none (0, or 255 for a real light)
      that lie inside the light's volume); 1 when no corner counts against,
      0 for light 0.
    Returns per-model (colorsIndex, visibility, primary light), the appended
    palette bytes and a report."""
    points = dict(grid.points())
    entries = list(struct.iter_unpack("<HBB", grid.entries))
    base = len(grid.colors) // 168
    palette = bytearray()
    samples, unreached, outside = [], 0, 0
    for model in models:
        origin = model["lightingOrigin"]
        corners = _corners(grid, points, entries, origin)
        if all(entry is None for *_, entry in corners):
            outside += 1
            samples.append((0, 0, model.get("primaryLightIndex", 0)))
            continue
        dominant, kept = _dominant_light(corners, tracer, origin)
        if dominant == 255:
            light = grid.sun_index
        elif grid.has_light_regions and dominant != grid.sun_index:
            light = model.get("primaryLightIndex", 0)
        else:
            light = dominant
        lit = against = total = 0.0
        blend = [0.0] * 168
        for corner, weight, position, (color, primary, _) in kept:
            if not _corner_visible(tracer, origin, position):
                continue
            if primary == light:
                lit += weight
            elif (primary == 0 or (primary == 255 and light != 0)) and _in_light_volume(lights[light], position):
                against += weight
            total += weight
            source = grid.colors[color * 168:(color + 1) * 168]
            for i in range(168):
                blend[i] += source[i] * weight
        if total == 0.0:
            # sub_71C360: no corner reached; WaW keeps the last palette entry,
            # visibility 255 and the sun.
            unreached += 1
            samples.append((base - 1, 255, grid.sun_index))
            continue
        if light == 0:
            visible = 0.0
        elif against == 0.0:
            visible = 1.0 if lit else 0.0
        else:
            visible = lit / (against + lit)
        palette.extend(min(255, round(v / total)) for v in blend)
        samples.append((base + len(palette) // 168 - 1, int(visible * 255.0 + 0.5), light))
    return samples, bytes(palette), {"static_models": len(models), "no_corner_reached": unreached,
                                     "outside_grid": outside}


def stage_grid(source: Path, destination: Path, static_models: Path | None = None,
               collision=None, primary_lights: Path | None = None, tracer=None) -> dict:
    """Stage T6's packed grid, preserving positions and directional samples.

    Native T6 sub_755810 decodes bytes as 32*(c/255)^2. WaW's model
    lighting program uses 2*c/255 in gamma space (see shaderruntime), so
    T6 needs c/sqrt(8) to reproduce its linear equivalent 4*(c/255)^2.
    T4 needsTrace is a corner trace mask, NOT T6's visibility byte.
    Primary-light selection already records which light reaches each point;
    visibility is full for that selected light. Runtime corner obstruction
    checks performed by T4 remain a separate compatibility requirement.
    ``static_models``: the BSP models.json; each placement with a
    lightingOrigin gets its colorsIndex and visibility (static_model_samples).
    """
    grid = read_grid(source)
    entries = bytearray()
    trace_points = 0
    for color, primary, needs_trace in struct.iter_unpack("<HBB", grid.entries):
        entries.extend(struct.pack("<HBB", color, primary, 255 if primary else 0))
        trace_points += bool(needs_trace)
    colors, model_report = grid.colors, None
    if static_models is not None:
        placements = json.loads(static_models.read_text(encoding="utf-8"))
        models = [m for m in placements["models"] if "lightingOrigin" in m]
        if (collision is None and tracer is None) or primary_lights is None:
            raise LightingError("static model lighting needs the WaW collision and primary lights (grid traces)")
        if tracer is None:
            from .cmtrace import WorldTracer
            tracer = WorldTracer(collision, GRID_TRACE_MASK)
        lights = json.loads(primary_lights.read_text(encoding="utf-8"))["lights"]
        samples, extra, model_report = static_model_samples(grid, models, tracer, lights)
        if (len(colors) + len(extra)) // 168 > 0xFFFF:
            raise LightingError("static model lighting exceeds the T6 palette index range")
        for model, (index, visibility, light) in zip(models, samples):
            if light != model.get("primaryLightIndex"):
                model["compiledPrimaryLightIndex"] = model.get("primaryLightIndex")
            model["colorsIndex"], model["visibility"], model["primaryLightIndex"] = index, visibility, light
        static_models.write_text(json.dumps(placements, indent=1) + "\n", encoding="utf-8")
        colors += extra
    palette = bytes(round(c / math.sqrt(8)) for c in colors)
    header = HEADER.pack(b"W2BT6LG1", 1, 0, grid.sun_index, *grid.mins, *grid.maxs,
                         grid.row_axis, grid.col_axis, len(grid.row_starts), len(grid.packed_rows),
                         len(entries) // 4, len(palette) // 168)
    destination.write_bytes(header + struct.pack(f"<{len(grid.row_starts)}H", *grid.row_starts)
                            + grid.packed_rows + entries + palette)
    return {**grid.report(), "status": "runtime_T6_grid", "palette_encoding": "WaW_gamma2_to_T6_linear32",
            "static_model_lighting": model_report,
            "source_trace_mask_points": trace_points,
            "corner_obstruction_checks": "T4_runtime_traces_not_representable_in_T6_visibility"}


def t6_light_fields(light: dict) -> dict:
    """T6 ComPrimaryLight precomputed fields for a WaW spot/omni light.

    T6 sub_782FA0 builds the light uniforms from them (falloff f, aAbB):
      lightFallOffA = (1/(aAbB.x-aAbB.y), 1/(aAbB.z-aAbB.w), 1/(f.z-f.x), 1/(min(f.w,f.y)-f.y))
      lightFallOffB = (-aAbB.y*A.x, -aAbB.w*A.y, -f.x*A.z, -f.y*A.w)
    and the T6 world lit programs light d units away by
    smoothstep(sat(d*A.z+B.z) * sat(d*A.w+B.w)) * sat(dAttenuation / d^2).
    falloff = (near, radius, near, 0): the window is 1 - d/radius out to the
    radius and A.w = -1/radius, which is WaW's lightPosition.w. aAbB carries
    WaW's spot factors (1/(cosIn-cosOut), -cosOut/(cosIn-cosOut), exponent)
    through A.x, B.x and A.y. ``near`` stays positive: the T6 spot projection
    (sub_658F90) uses falloff.x as its near plane. dAttenuation removes the
    inverse-square term WaW does not have. T6 sub_73AC60 turns a spot with
    roundness 0 into SPOT_SQUARE (technique 9) and roundness 1 into
    SPOT_ROUND (11); 0.5 keeps it a plain spot (technique 7, WaW's lit_spot)."""
    radius = float(light["radius"])
    near = min(1.0, radius * 0.01)
    exponent = float(light.get("exponent", 0))
    return {
        "falloff": [near, radius, near, 0.0],
        "aAbB": [float(light["cosHalfFovInner"]), float(light["cosHalfFovOuter"]),
                 1.0 / exponent if exponent > 0 else 1e30, 0.0],
        "dAttenuation": radius * radius * 1e4,
        "roundness": 0.5,
    }


def stage_primary_lights(source: Path, destination: Path, shadows_disabled: list | None = None,
                         keep_shadows: bool = False) -> set[str]:
    """Write the T6 primary-light table; returns the light defs it uses.
    ``shadows_disabled`` collects the indices of lights staged unshadowed;
    ``keep_shadows`` keeps WaW's canUseShadowMap (the world carries the WaW
    per-light shadow geometry, BSP/shadowgeom.json)."""
    data = json.loads(source.read_text(encoding="utf-8"))
    definitions = set()
    shadows_disabled = [] if shadows_disabled is None else shadows_disabled
    for light in data["lights"]:
        if light["type"] not in (0, 1, 2, 3):
            raise LightingError(f"unsupported WaW primary light type {light['type']}")
        if light["type"] == 3:  # WaW omni -> T6 omni, which moved from 3 to 5
            light["type"] = 5
        if light["type"] != 1 and "color" in light:
            # WaW lit_spot/lit_omni add colour * attenuation in gamma; T6 adds
            # it in linear before sqrt(hdr * sum) (hdr 1 on converted worlds),
            # as the converted sun. The sun light (type 1) is set from SunParse.
            light["color"] = [c * c for c in light["color"]]
        if light["type"] > 1:
            light.update(t6_light_fields(light))
            # Without the WaW shadow geometry (render dump before v6) a T6
            # spot/omni shadow map would hold no static geometry: such lights
            # draw unshadowed (reported).
            if light.get("canUseShadowMap") and not keep_shadows:
                light["canUseShadowMap"] = 0
                shadows_disabled.append(light.get("index"))
        if light["defName"]:
            definitions.add(light["defName"])
            light["defName"] = "waw_light/" + light["defName"]
    destination.write_text(json.dumps(data, indent=1) + "\n", encoding="utf-8")
    return definitions
