"""
Black Ops' exposure, and its exposure volumes, in Black Ops II's units.

Both games scale every lit pixel by one shader constant, `hdrControl0.x`, and both take it
from a per-view exposure that eases toward the value of the exposure volume the camera stands
in (or the sun's, outside all of them) at the same rates -- 15 brightening, 20 darkening, each
scaled by the volume -- from a feather of 50 units. What differs is the unit:

    Black Ops     hdrControl0.x = exposure / 8          (OpenBLOPS R_SetExposure)
    Black Ops II  hdrControl0.x = 1 / 2^(exposure + 2)  (t6zm.exe 0x7281e9; the constant is
                                                         code constant 0x7B, hdrControl0,
                                                         written at 0x72ac98)

So Black Ops' linear exposure `e` gives the same shading at Black Ops II's `log2(8 / e) - 2` stops
(zombie_coast's sun: 0.45 -> 2.15; carried across unchanged, as v38 did, it lit the map 3.3x
brighter than Black Ops). The lightmap and light-grid shaders both multiply their decoded light by
this constant (§3e/§3f and codport.bo2.lightgrid), and the lighting data is carried in Black Ops'
own units. Stock maps agree: Black Ops II's lightmaps sit 2-3x above Black Ops' and its exposures
(3.0-3.36) multiply by 4x less.

Every converted exposure then sits POST_OFFSET_STOPS darker. The frame is finished by a LUT
(codport.convert.bo1_grade), which multiplies it back; the offset exists so Black Ops' white point
lands on a LUT texel (29 of 31) rather than inside Black Ops II's shoulder, where the last texel
would span everything from x = 3.2 to white. With it the LUT reproduces Black Ops' final image
to under one 8-bit step (measured on zombie_coast_2); without it, up to 20. It also puts the
scene buffer where Black Ops II's own maps keep theirs (stock exposures 3.0-3.4; Coast: 3.9).

The volumes are laid out differently, which the engines' volume tests show
(OpenBLOPS R_CheckExposureVolumes, t6zm.exe 0x727f60):

    Black Ops     control: first plane in bits 0-15, plane count - 1 in bits 16-23;
                  inside where every plane's dot >= 0
    Black Ops II  control: plane count - 1 in bits 8-15, first plane in bits 16-31;
                  inside where every plane's dot <= 0

zombie_coast's eleven volumes confirm Black Ops' side: every one is a bounded region under
dot >= 0 and empty under dot <= 0. So the planes are negated and `control` repacked.

What does not cross exactly is the feather. Black Ops feathers on the distance to the nearest
plane; Black Ops II's test keeps the most negative dot, and adds `featherAdjust` in stops rather
than linear exposure. A volume with a feather is carried as the nearest equivalent and reported;
zombie_coast has none.

Written to `BSP/map_exposure.json`, which the OAT fork reads into GfxWorld (it used to drop the
volumes entirely).
"""

from __future__ import annotations

import json
import math
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

#: Black Ops: hdrControl0.x = exposure / T5_EXPOSURE_WHITE (the white point R_UpdateExposureValue sets).
T5_EXPOSURE_WHITE = 8.0
#: Black Ops II: hdrControl0.x = 2^-(exposure + T6_EXPOSURE_BIAS).
T6_EXPOSURE_BIAS = 2.0

#: Black Ops II's LUT texel that Black Ops' white point (curve input 8) lands on; see the module
#: docstring. hdr_bloom_apply looks up shoulder(g), g = sqrt(4 * scene^2), and Black Ops' curve
#: input is 8 * scene^2 with the scene buffer the same -- so g^2 = k * x, k = 0.5 * 2^-offset.
LUT_WHITE_TEXEL = 29
LUT_SIZE = 32
T6_SHOULDER_START = 0.75


def _post_offset_stops() -> float:
    s = LUT_WHITE_TEXEL / (LUT_SIZE - 1.0)
    g = T6_SHOULDER_START - math.log(1.0 - (s - T6_SHOULDER_START) / 0.25) / 4.0
    k = g * g / T5_EXPOSURE_WHITE
    return math.log2(0.5 / k)


POST_OFFSET_STOPS = _post_offset_stops()
#: g^2 = SCENE_SCALE * (Black Ops' curve input): what the LUT has to undo.
SCENE_SCALE = 0.5 * 2.0 ** -POST_OFFSET_STOPS

#: Black Ops' "use the exposure table" marker, `exposure == -1` (R_UpdateExposureValue). The table
#: is runtime state, not in the fastfile, so such a volume takes the sun's exposure.
T5_EXPOSURE_FROM_TABLE = -1.0

SIDE_CAR = "map_exposure.json"


def t6_exposure(t5_exposure: float, offset: float | None = None) -> float:
    """
    Black Ops' linear exposure in Black Ops II stops: the same hdrControl0.x, POST_OFFSET_STOPS
    darker (pass offset=0 for the bare equivalence).
    """
    if not t5_exposure > 0.0:
        raise ValueError(f"exposure {t5_exposure} has no Black Ops II equivalent")
    offset = POST_OFFSET_STOPS if offset is None else offset
    return math.log2(T5_EXPOSURE_WHITE / t5_exposure) - T6_EXPOSURE_BIAS + offset


def t5_hdr_control(t5_exposure: float) -> float:
    return t5_exposure / T5_EXPOSURE_WHITE


def t6_hdr_control(t6_exposure_value: float) -> float:
    return 2.0 ** -(t6_exposure_value + T6_EXPOSURE_BIAS)


@dataclass
class ExposureVolume:
    """One volume in Black Ops II's layout."""

    control: int
    exposure: float
    luminance_increase_scale: float
    luminance_decrease_scale: float
    feather_range: float
    feather_adjust: float

    def to_dict(self) -> dict[str, Any]:
        return {
            "control": self.control,
            "exposure": self.exposure,
            "luminanceIncreaseScale": self.luminance_increase_scale,
            "luminanceDecreaseScale": self.luminance_decrease_scale,
            "featherRange": self.feather_range,
            "featherAdjust": self.feather_adjust,
        }


@dataclass
class Exposure:
    source_sun: float
    sun: float
    volumes: list[ExposureVolume] = field(default_factory=list)
    planes: list[tuple[float, float, float, float]] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)

    def to_dict(self) -> dict[str, Any]:
        return {
            "codportExposure": 1,
            "sourceSunExposure": self.source_sun,
            "sunExposure": self.sun,
            "volumes": [v.to_dict() for v in self.volumes],
            "planes": [list(p) for p in self.planes],
        }


def _float(value: Any) -> float | None:
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def collect_exposure(package: Any, worldspawn_keys: dict[str, str] | None = None) -> Exposure | None:
    """
    The source map's exposure converted for Black Ops II, or None when there is nothing to convert.

    Only a Black Ops source is converted: its exposure model is the one read out of both
    executables. Other source games keep whatever their worldspawn says.
    """
    if package is None or str(getattr(package, "source_game", "")).upper() != "T5":
        return None

    gfx = getattr(package, "gfx", None) or {}
    lighting = gfx.get("lighting") or {}
    sun_parse = lighting.get("sunParse") or {}

    source_sun = _float(sun_parse.get("exposure"))
    if source_sun is None and worldspawn_keys:
        source_sun = _float(worldspawn_keys.get("exposure"))
    if source_sun is None or source_sun <= 0.0:
        return None

    exposure = Exposure(source_sun=source_sun, sun=t6_exposure(source_sun))

    volumes = lighting.get("exposureVolumes") or []
    planes = lighting.get("exposureVolumePlanes") or []
    feathered = 0
    for index, volume in enumerate(volumes):
        control = int(volume.get("control", 0))
        first = control & 0xFFFF
        count = ((control >> 16) & 0xFF) + 1
        if first + count > len(planes):
            exposure.notes.append(
                f"exposure volume {index} names planes {first}..{first + count - 1} of {len(planes)}; "
                "the volumes were left out rather than read past the plane list"
            )
            return Exposure(source_sun=source_sun, sun=exposure.sun, notes=exposure.notes)

        value = _float(volume.get("exposure"))
        if value is None or value == T5_EXPOSURE_FROM_TABLE or value <= 0.0:
            exposure.notes.append(
                f"exposure volume {index} reads Black Ops' runtime exposure table; it takes the sun's exposure"
            )
            value = source_sun

        first_out = len(exposure.planes)
        for plane in planes[first:first + count]:
            exposure.planes.append(tuple(-float(c) for c in plane))

        feather_range = float(volume.get("featherRange", 0.0))
        feather_adjust = float(volume.get("featherAdjust", 0.0))
        if feather_range and feather_adjust:
            feathered += 1
            # Black Ops adds featherAdjust to linear exposure over a positive nearest-plane
            # distance; Black Ops II adds it in stops over a negative dot. The same edge
            # exposure, reached from the other side.
            edge = max(value + feather_adjust, 1e-3)
            feather_adjust = -(t6_exposure(edge) - t6_exposure(value))
            feather_range = -feather_range
        else:
            feather_range = feather_adjust = 0.0

        exposure.volumes.append(ExposureVolume(
            control=(first_out << 16) | ((count - 1) << 8),
            exposure=t6_exposure(value),
            luminance_increase_scale=float(volume.get("luminanceIncreaseScale", 1.0)),
            luminance_decrease_scale=float(volume.get("luminanceDecreaseScale", 1.0)),
            feather_range=feather_range,
            feather_adjust=feather_adjust,
        ))

    if feathered:
        exposure.notes.append(
            f"{feathered} exposure volume(s) feather their edge; Black Ops II feathers on a different "
            "distance, so the blend at those edges is approximate"
        )
    return exposure


def write_exposure(exposure: Exposure | None, zone_raw: Path) -> Path | None:
    """Write BSP/map_exposure.json, or remove a stale one when there is nothing to write."""
    path = Path(zone_raw) / "BSP" / SIDE_CAR
    if exposure is None:
        if path.exists():
            path.unlink()
        return None
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(exposure.to_dict(), indent=1), encoding="utf-8")
    return path
