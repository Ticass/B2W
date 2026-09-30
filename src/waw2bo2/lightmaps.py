"""WaW lightmap pages for the T6 world.

A WaW lightmap page is two textures (measured on the dumped world programs):
  * secondary, W x 2W RGBA8: top half colour A + direction x in alpha, bottom
    half colour B + direction y in alpha. ``lm_*`` pixel programs sample it at
    (u, v/2) and (u, v/2 + 1/2).
  * primary, 2W x 2W L8: sun visibility, sampled at (u, v) by sun programs.

A T6 page is one W x 3W RGBA8 texture of three stacked pages, bound as
``lightmapSamplerSecondary`` (measured on stock world lit programs):
  1. ambient, decoded rgb / (a + 1e-6), linear
  2. directional colour, decoded the same way
  3. direction * 0.5 + 0.5 in rgb, sun visibility in alpha
Final T6 colour = sqrt(hdr * (albedo^2 * lighting + ...)); WaW writes
albedo * lighting in gamma space.

Two encodings of each page are produced:
  * WaW-encoded: [secondary top, secondary bottom, primary] stacked in T6's
    shape; translated WaW programs sample it through an exact UV remap
    (``WAW_PAGE_UV``). The primary is box-filtered 2x2 to the page width
    (reported: sun shadow detail at half resolution).
  * T6-encoded, for surfaces still drawn by T6 donor programs: ambient =
    WaW's own lighting at a flat normal, squared into T6's linear units
    (hdrControl0.x assumed 1), directional colour 0, sun visibility from the
    primary. Bump-dependent WaW lighting is not representable there
    (reported as an approximation).
"""
from __future__ import annotations

import json
import math
import struct
from pathlib import Path

from . import iwi

# WaW sampler -> (scale, offset) applied to v inside the WaW-encoded page.
WAW_PAGE_UV = {
    'lightmapSamplerSecondary': (2.0 / 3.0, 0.0),
    'lightmapSamplerPrimary': (1.0 / 3.0, 2.0 / 3.0),
}
# WaW lm_* direction decode, per alpha pair (measured literal c1 of the programs)
_DIR_SCALE = (4.07999992, 4.06451607)
_DIR_BIAS = (-2.07999992, -2.06451607)


class LightmapError(ValueError):
    pass


def _rgba(path: Path) -> tuple[int, int, bytes]:
    img = iwi.read_dds(path.read_bytes())
    level = iwi._convert_pixels(img, iwi._split_levels(img)[0])
    if img.iwi_format == iwi.FMT_RGBA:
        return img.width, img.height, level
    if img.iwi_format == iwi.FMT_LUMINANCE:
        out = bytearray(len(level) * 4)
        for c in range(3):
            out[c::4] = level
        out[3::4] = level
        return img.width, img.height, bytes(out)
    raise LightmapError(f'{path.name}: unsupported lightmap pixel format {img.iwi_format}')


def _box_luminance(width: int, height: int, rgba: bytes, out_w: int, out_h: int) -> bytes:
    """Average the red channel down to out_w x out_h (integer factor)."""
    fx, fy = width // out_w, height // out_h
    if fx * out_w != width or fy * out_h != height:
        raise LightmapError('primary lightmap is not an integer multiple of the page size')
    red = rgba[0::4]
    out = bytearray(out_w * out_h)
    n = fx * fy
    for y in range(out_h):
        rows = [red[(y * fy + j) * width:(y * fy + j + 1) * width] for j in range(fy)]
        for x in range(out_w):
            s = 0
            for row in rows:
                s += sum(row[x * fx:(x + 1) * fx])
            out[y * out_w + x] = (s + n // 2) // n
    return bytes(out)


def _iwi_rgba(width: int, height: int, data: bytes) -> bytes:
    out = bytearray(b'IWi' + bytes([27]))
    out += struct.pack('<bbHHHf', iwi.FMT_RGBA, iwi.FLAG_NOMIPMAPS, width, height, 1, 0.0)
    out += bytes(16)
    out += struct.pack('<8I', 64 + len(data), *([0] * 7))
    return bytes(out + data)


def _flat_weight_table() -> list[float]:
    """0.6 * exp(-|d|^2) + 0.4 for every (top alpha, bottom alpha) byte pair."""
    table = [0.0] * 65536
    for a in range(256):
        dx = a / 255.0 * _DIR_SCALE[0] + _DIR_BIAS[0]
        for b in range(256):
            dy = b / 255.0 * _DIR_SCALE[1] + _DIR_BIAS[1]
            table[a * 256 + b] = 0.6 * math.exp(-(dx * dx + dy * dy)) + 0.4
    return table


def build_page(secondary: Path, primary: Path | None) -> tuple[int, bytes, bytes]:
    """(page width, WaW-encoded RGBA, T6-encoded RGBA) for one WaW page."""
    sw, sh, sec = _rgba(secondary)
    if sh != 2 * sw:
        raise LightmapError(f'{secondary.name}: expected a W x 2W secondary lightmap, got {sw}x{sh}')
    half = sw * sw * 4
    top, bottom = sec[:half], sec[half:]
    if primary is not None:
        pw, ph, pri = _rgba(primary)
        vis = _box_luminance(pw, ph, pri, sw, sw)
    else:
        vis = bytes([255]) * (sw * sw)
    third = bytearray(sw * sw * 4)
    for c in range(3):
        third[c::4] = vis
    third[3::4] = vis
    waw = top + bottom + bytes(third)

    weights = _flat_weight_table()
    ambient = bytearray(sw * sw * 4)
    ta, ba = top[3::4], bottom[3::4]
    for i in range(sw * sw):
        w = weights[ta[i] * 256 + ba[i]]
        p = i * 4
        light = [(top[p + c] + bottom[p + c] * w) / 255.0 for c in range(3)]
        linear = [v * v for v in light]  # T6 squares; WaW lighting is gamma
        peak = max(1.0, max(linear))
        for c in range(3):
            ambient[p + c] = min(255, int(round(linear[c] / peak * 255.0)))
        ambient[p + 3] = min(255, max(1, int(round(255.0 / peak))))
    directional = bytes([0, 0, 0, 255]) * (sw * sw)
    direction = bytearray(sw * sw * 4)
    direction[0::4] = bytes([128]) * (sw * sw)
    direction[1::4] = bytes([128]) * (sw * sw)
    direction[2::4] = bytes([255]) * (sw * sw)
    direction[3::4] = vis
    t6 = bytes(ambient) + directional + bytes(direction)
    return sw, waw, t6


def stage(world, image_roots: list[Path], project_root: Path, waw_materials: set[str]) -> dict:
    """Write both encodings of every lightmap page the world uses.

    ``waw_materials``: materials whose lit passes are translated WaW programs
    reading WaW-encoded pages. Writes images/<name>.iwi and BSP/lightmaps.json:
    {"pages": [{"waw": img, "t6": img}], "wawMaterials": [...]} - the linker
    gives a surface page i for WaW materials and page count + i otherwise.
    """
    report = {'pages': [], 'warnings': [], 'errors': []}
    pages = []

    def find(index, kind):
        name = f'_lightmap{index}_{kind}.dds'
        return next((r / name for r in image_roots if (r / name).exists()), None)

    # WaW lightmap pages are *lightmap0.. up to the world's lightmap count; a
    # surface index past them (31 on the measured map) means "no lightmap".
    count = 0
    while find(count, 'secondary') is not None:
        count += 1
    used = {s.lightmap_index for s in world.surfaces}
    unlit = sum(1 for s in world.surfaces if s.lightmap_index >= count)
    if unlit:
        report['warnings'].append(f'{unlit} surfaces have no lightmap page (WaW index >= {count}: '
                                  f'{sorted(i for i in used if i >= count)})')
    for index in range(count):
        secondary, primary = find(index, 'secondary'), find(index, 'primary')
        if primary is None:
            report['warnings'].append(f'lightmap page {index}: primary (sun visibility) absent; fully visible')
        width, waw, t6 = build_page(secondary, primary)
        names = {'waw': f'lightmap{index}_waw_secondary', 't6': f'lightmap{index}_t6_secondary'}
        for key, data in (('waw', waw), ('t6', t6)):
            dst = project_root / 'images' / f'{names[key]}.iwi'
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(_iwi_rgba(width, width * 3, data))
        pages.append(names)
        report['pages'].append({'index': index, 'width': width, **names})
    bsp = project_root / 'BSP'
    bsp.mkdir(parents=True, exist_ok=True)
    (bsp / 'lightmaps.json').write_text(json.dumps({'pages': pages, 'wawMaterials': sorted(waw_materials)}, indent=1) + '\n',
                                        encoding='utf-8')
    if pages:
        report['warnings'].append('lightmap sun visibility (WaW primary) box-filtered to the secondary page resolution')
        if len(waw_materials) < len({s.material for s in world.surfaces if s.lightmap_index < count}):
            report['warnings'].append('APPROXIMATED lightmaps for T6 donor materials: WaW flat-normal lighting, no '
                                      'directional term, hdrControl0.x assumed 1')
    return report
