"""
The two helpers hero_lights.py takes from codport/bo2/native_world.py (the world builder), copied out
on their own so the lighting files load without the rest of the tool.
"""

from __future__ import annotations

import re


def _parse_vector(text: str) -> tuple[float, float, float] | None:
    """Parse a worldspawn `"x y z"` triple, or None if it is not one."""
    # Numbers as the engine's sscanf reads them: zombie_coast's own ".567 .613.653" is three values;
    # split on spaces it was two, and the port's sun went white (v1-v94).
    parts = re.findall(r"[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?", text or "")
    if len(parts) != 3:
        return None
    try:
        return (float(parts[0]), float(parts[1]), float(parts[2]))
    except ValueError:
        return None


def t5_sun_colour(colour: tuple[float, float, float]) -> tuple[float, float, float]:
    """
    Black Ops' worldspawn `suncolor` as the linear colour its renderer lights with: it reads the key as sRGB.
    Captured from zombie_coast's draws (tools/gfxcap): `suncolor` ".567 .613.653", `sunlight` 11 gave
    sunDiffuse (3.095, 3.673, 4.223) = srgb_to_linear(suncolor) * 11 exactly. Black Ops II lights with
    sunCd.xyz * sunCd.w as given.

    For World at War: check how its renderer reads `suncolor` (sRGB or linear) the same way -- a frame
    capture of a known sun -- before reusing this.
    """
    def linear(c: float) -> float:
        return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4

    return (linear(colour[0]), linear(colour[1]), linear(colour[2]))
