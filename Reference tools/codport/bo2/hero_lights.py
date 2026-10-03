"""
The world's hero lights: the sun, standing in for Black Ops' lighting of the weapon in hand.

Black Ops II lights a hero-lit model -- the weapon in the player's hands, and Black Ops' own guns it
kept (mtl_ray_gun on `mc_lit_sm_r0c0n0s0_7426e277`) -- from the light grid plus the world's hero
lights: each one within its radius adds (1 - d / radius)^2 * colour from its direction into the
model's 56-direction grid sample (gfx_d3d_r_pointlights, the loop over g_heroLights; type 5 is an
omni light, anything else a spot). Stock maps place them; the OAT world linker wrote none, so a port's
held weapons were lit by the grid alone. Black Ops has no hero lights -- its `_hero` weapon shaders
take the sun directly -- which on zombie_coast, whose two reflection probes are a dark storm and a
darker lighthouse, left Black Ops' M1911 black where Black Ops draws it silver (v83-v94).

So the port gets one omni hero light: the map's sun, far out along the sun's direction with a radius
wide enough that its falloff is even across the map, in the sun's colour.
"""

from __future__ import annotations

import json
import math
from pathlib import Path
from typing import Any

#: How far out the light stands, and its radius: (1 - 20000 / 400000)^2 = 0.90 at the map's centre,
#: within a few percent anywhere on a map a few thousand units across.
SUN_DISTANCE = 20000.0
SUN_RADIUS = 400000.0
#: The light's strength, in the light grid's own units ((byte / 255)^2 * 32 per channel): about a
#: sunlit texel of the grid, so the weapon in hand takes the sun as a sunlit prop would.
SUN_SCALE = 1.0


def _vector(text: str | None) -> tuple[float, float, float] | None:
    from codport.bo2.native_world import _parse_vector

    return _parse_vector(str(text or ""))


def sun_direction(angles: tuple[float, float, float]) -> tuple[float, float, float]:
    """The unit vector toward the sun from `sundirection` (pitch, yaw, roll in degrees)."""
    pitch, yaw = math.radians(angles[0]), math.radians(angles[1])
    return (math.cos(pitch) * math.cos(yaw), math.cos(pitch) * math.sin(yaw), -math.sin(pitch))


def sun_hero_light(worldspawn: dict[str, str], centre: tuple[float, float, float]) -> dict[str, Any] | None:
    """The map's sun as a hero light, or None when the map names no sun."""
    angles = _vector(worldspawn.get("sundirection"))
    colour = _vector(worldspawn.get("suncolor"))
    if angles is None or colour is None:
        return None
    from codport.bo2.native_world import t5_sun_colour

    colour = t5_sun_colour(colour)   # the sun Black Ops' `_hero` shaders took (linear; the key is sRGB)
    toward = sun_direction(angles)
    origin = [c + t * SUN_DISTANCE for c, t in zip(centre, toward)]
    return {
        "type": 5,
        "color": [round(c * SUN_SCALE, 4) for c in colour],
        "dir": [round(-t, 5) for t in toward],
        "origin": [round(v, 1) for v in origin],
        "radius": SUN_RADIUS,
        "cosHalfFovOuter": -1.0,
        "cosHalfFovInner": -1.0,
        "exponent": 0,
    }


def write_hero_lights(zone_raw: Path, lights: list[dict[str, Any]]) -> Path | None:
    """`BSP/map_hero_lights.json` for the world linker; None (and no file) when there are none."""
    path = Path(zone_raw) / "BSP" / "map_hero_lights.json"
    if not lights:
        path.unlink(missing_ok=True)
        return None
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({"lights": lights}, indent=2), encoding="utf-8")
    return path


def source_hero_lights(gfx: dict[str, Any] | None) -> list[dict[str, Any]]:
    """The source world's hero-only lights (the world dump's `lighting.heroLights`), as Black Ops II
    reads them: the struct is the same in both games."""
    lights = ((gfx or {}).get("lighting") or {}).get("heroLights") or []
    out = []
    for light in lights:
        try:
            out.append({
                "type": int(light.get("type", 5)),
                "color": [float(v) for v in light["color"]],
                "dir": [float(v) for v in light.get("dir", [0, 0, 0])],
                "origin": [float(v) for v in light["origin"]],
                "radius": float(light["radius"]),
                "cosHalfFovOuter": float(light.get("cosHalfFovOuter", -1.0)),
                "cosHalfFovInner": float(light.get("cosHalfFovInner", -1.0)),
                "exponent": int(light.get("exponent", 0)),
            })
        except (KeyError, TypeError, ValueError):
            continue
    return out
