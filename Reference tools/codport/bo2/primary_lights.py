"""
The source map's own primary lights -- its lamps, spotlights and gobos -- in Black Ops II.

Both games light static geometry from primary lights at runtime rather than baking them: a
surface names one light, its lightmap's visibility channel says where that light reaches, and a
lit-spot or lit-omni technique adds it (Black Ops' wc_l_sm techniques 13/15, Black Ops II's 7/13).
A port that ships only the sun -- as ports did until 2026-09-24 -- leaves every lamp in the map
dark: zombie_coast has 90 of them.

`ComPrimaryLight` is nearly the same struct in both games, and the light setup code of each
(OpenBLOPS r_draw_shadowablelight.cpp; t6zm.exe 0x784740) builds the same shader constants from the
same fields -- the falloff edges from `falloff`, the cone from `aAbB`, the cone control from
Black Ops' `attenuation.w`, which Black Ops II calls `roundness`. Three things differ:

* **Type numbers.** Black Ops: 1 sun, 2 spot, 3 omni. Black Ops II: 1 sun, 2 spot, 3 square
  spot, 4 round spot, 5 omni. Copied as they are, every omni light would be a square spot.
* **Distance attenuation.** Black Ops multiplies by `attenuation.x + .y d + .z d^2`; Black Ops II
  by `saturate(dAttenuation / d^2)` (lmap_6f38c918 vs lmap_b0662fa9). A constant Black Ops
  attenuation -- all 90 of zombie_coast's are (1, 0, 0) -- is exact: dAttenuation large enough to
  saturate everywhere, and the constant folded into the colour. A distance-dependent one is
  approximated and reported.
* **The sun.** Black Ops puts the sun's colour times its intensity on its primary light; Black
  Ops II keeps that in sunParse and leaves the light white (every stock map surveyed). It keeps
  Black Ops' direction.

Light definitions -- white_light, gobo_grate, ... -- are the source's own, with their attenuation
images: every Black Ops II map embeds its own (mp_bridge: white_light, gobo_*), and none of
Black Ops' is in a zone Black Ops II loads.

Written to `BSP/map_primary_lights.json`, which the OAT fork builds ComWorld from.
"""

from __future__ import annotations

import json
import struct
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

SIDE_CAR = "map_primary_lights.json"

#: Black Ops light type -> Black Ops II light type (GfxLightType in each game).
T5_TO_T6_TYPE = {0: 0, 1: 1, 2: 2, 3: 5}

#: Large enough that saturate(dAttenuation / d^2) is 1 anywhere in a map (d < 30,000 units).
SATURATED_ATTENUATION = 1.0e9

#: The definition Black Ops II draws a light through when it names none.
DEFAULT_DEF = "white_light"

_COPIED = (
    "canUseShadowMap", "exponent", "priority", "cullDist",
    "radius", "cosHalfFovOuter", "cosHalfFovInner", "cosHalfFovExpanded",
    "rotationLimit", "translationLimit", "mipDistance",
)
_VEC4 = ("falloff", "angle", "aAbB", "cookieControl0", "cookieControl1", "cookieControl2")


@dataclass
class LightDef:
    name: str
    image: str
    sampler_state: int = 115
    lmap_lookup_start: int = 0

    def to_dict(self) -> dict[str, Any]:
        return {"name": self.name, "image": self.image, "samplerState": self.sampler_state,
                "lmapLookupStart": self.lmap_lookup_start}


@dataclass
class PrimaryLights:
    lights: list[dict[str, Any]] = field(default_factory=list)
    defs: list[LightDef] = field(default_factory=list)
    sun_index: int = 1
    notes: list[str] = field(default_factory=list)

    @property
    def count(self) -> int:
        return len(self.lights)

    def images(self) -> list[str]:
        return sorted({d.image for d in self.defs if d.image})

    def to_dict(self) -> dict[str, Any]:
        return {"codportPrimaryLights": 1, "sunIndex": self.sun_index,
                "lights": self.lights, "defs": [d.to_dict() for d in self.defs]}


def _vec(values: Any, n: int) -> list[float]:
    v = [float(x) for x in (values or [])][:n]
    return v + [0.0] * (n - len(v))


def convert_light(source: dict[str, Any], is_sun: bool = False) -> tuple[dict[str, Any], str | None]:
    """One Black Ops ComPrimaryLight in Black Ops II's fields, and a note when it is approximate."""
    note = None
    t5_type = int(source.get("type", 0))
    out: dict[str, Any] = {"type": T5_TO_T6_TYPE.get(t5_type, t5_type)}
    for key in _COPIED:
        out[key] = source.get(key, 0)
    for key in _VEC4:
        out[key] = _vec(source.get(key), 4)
    out["dir"] = _vec(source.get("dir"), 3)
    out["origin"] = _vec(source.get("origin"), 3)
    colour = _vec(source.get("color"), 3)
    diffuse = _vec(source.get("diffuseColor"), 4)

    attenuation = _vec(source.get("attenuation"), 4)
    out["roundness"] = attenuation[3]
    if t5_type == 2 and out["roundness"] == 0.0:
        # Black Ops II's map load (gfx_d3d_r_bsp_load_obj) makes a spot with roundness 0 a *square*
        # spot (type 3), and one with roundness 1 and a symmetric aAbB a round one (type 4). Black
        # Ops has no square spot and stores 0 in attenuation.w for every light (zombie_coast: all 90):
        # carried as-is, every spot drew as a hard-edged square projector -- v79's white wedges on
        # the boat. Its entities say roundness 1.
        out["roundness"] = 1.0
    out["dAttenuation"] = 0.0
    if t5_type in (2, 3):
        out["dAttenuation"] = SATURATED_ATTENUATION
        constant = attenuation[0]
        if attenuation[1] != 0.0 or attenuation[2] != 0.0:
            note = (f"attenuation {attenuation[:3]} falls off with distance, which Black Ops II's "
                    "inverse-square term cannot follow; kept at its value at the light")
        if constant > 0.0 and constant != 1.0:
            colour = [c * constant for c in colour]
            diffuse = [c * constant for c in diffuse[:3]] + [diffuse[3]]

    if is_sun:
        colour, diffuse = [1.0, 1.0, 1.0], [1.0, 1.0, 1.0, 0.0]
    out["color"] = colour
    out["diffuseColor"] = diffuse
    def_name = str(source.get("defName") or "")
    out["defName"] = def_name
    out["useCookie"] = 255 if def_name and def_name != DEFAULT_DEF and t5_type in (2, 3) else 0
    out["shadowmapVolume"] = 0
    return out, note


def read_light_defs(extracted: Path) -> dict[str, LightDef]:
    """The light definitions the source dump carries (lights/<name>.json, from the OAT fork)."""
    out: dict[str, LightDef] = {}
    folder = Path(extracted) / "lights"
    if not folder.is_dir():
        return out
    for path in sorted(folder.glob("*.json")):
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        name = str(data.get("name") or path.stem)
        out[name] = LightDef(name=name, image=str(data.get("image") or ""),
                             sampler_state=int(data.get("samplerState", 115)),
                             lmap_lookup_start=int(data.get("lmapLookupStart", 0)))
    return out


def collect_primary_lights(package: Any, extracted: Path | None) -> PrimaryLights | None:
    """The source's lights converted for Black Ops II, or None when there are none to carry."""
    if package is None or str(getattr(package, "source_game", "")).upper() != "T5":
        return None
    source = list(getattr(package, "primary_lights", []) or [])
    if len(source) < 2:
        return None
    lighting = ((getattr(package, "gfx", None) or {}).get("lighting") or {})
    sun_index = int(lighting.get("sunPrimaryLightIndex", 1))
    if not 0 < sun_index < len(source) or len(source) > 254:
        return None

    result = PrimaryLights(sun_index=sun_index)
    approximate = 0
    for index, light in enumerate(source):
        converted, note = convert_light(light, is_sun=index == sun_index)
        result.lights.append(converted)
        if note:
            approximate += 1

    available = read_light_defs(extracted) if extracted else {}
    wanted = sorted({l["defName"] for l in result.lights if l["defName"]})
    missing = []
    for name in wanted:
        if name in available:
            result.defs.append(available[name])
        else:
            missing.append(name)
    if missing:
        result.notes.append(
            f"light definition(s) {', '.join(missing)} are not in the source dump; those lights "
            "draw through $white")
    if approximate:
        result.notes.append(f"{approximate} light(s) have distance attenuation Black Ops II cannot follow exactly")
    return result


def write_primary_lights(lights: PrimaryLights | None, zone_raw: Path) -> Path | None:
    path = Path(zone_raw) / "BSP" / SIDE_CAR
    if lights is None:
        if path.exists():
            path.unlink()
        return None
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(lights.to_dict(), indent=1), encoding="utf-8")
    return path


# ---- the definitions' images ---------------------------------------------------------------------

#: DDS pixel format -> (IWI v27 format, bytes per 4x4 block or per pixel, is block compressed).
_IWI_FORMATS = {"dxt1": (0x0B, 8, True), "dxt3": (0x0C, 16, True), "dxt5": (0x0D, 16, True),
                "a8": (0x05, 1, False)}


def dds_to_iwi(dds_path: Path) -> bytes | None:
    """
    A 2D DDS (DXT1/3/5 or A8) as an IWI v27 with all its mips, smallest first; None otherwise.

    Black Ops' gobo images are alpha-only (A8), which the block-compression path would change;
    IWI v27 carries A8 as it is (IMG_FORMAT_BITMAP_ALPHA).
    """
    from codport.formats.dds import read_dds_header

    info = read_dds_header(dds_path)
    fmt = str(getattr(info.format, "value", info.format)).lower()
    if not info.valid or info.is_cubemap or info.depth > 1 or fmt not in _IWI_FORMATS:
        return None
    code, unit, blocks = _IWI_FORMATS[fmt]
    data = Path(dds_path).read_bytes()
    offset = 128 + (20 if data[84:88] == b"DX10" else 0)

    def level_size(m: int) -> int:
        w, h = max(1, info.width >> m), max(1, info.height >> m)
        return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * unit if blocks else w * h * unit

    mips = max(1, info.mip_levels)
    levels, at = [], offset
    for m in range(mips):
        size = level_size(m)
        if at + size > len(data):
            mips = m
            break
        levels.append(data[at:at + size])
        at += size
    if not levels:
        return None
    body, sizes, total = b"", [0] * 8, 64
    for m in reversed(range(len(levels))):
        body += levels[m]
        total += len(levels[m])
        if m < 8:
            sizes[m] = total
    flags = 0x02 if len(levels) == 1 else 0x00
    header = (b"IWi" + bytes([27]) + struct.pack("<BBHHHf", code, flags, info.width, info.height, 1, 0.0)
              + bytes(16) + struct.pack("<8I", *sizes))
    return header + body
