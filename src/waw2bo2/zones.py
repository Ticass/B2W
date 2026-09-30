"""Carry the WaW zombie zone graph into the BO2 map script.

WaW (_zombiemode_zone_manager) and BO2 (_zm_zonemgr) share the model: zones
are ``info_volume`` brush entities named by targetname, linked with
``add_adjacent_zone( a, b, flag )`` where the flag is set by the door that
opens the connection, plus a list of zones active at start. The WaW map
script is plain GSC in the map zone, so both are read from it verbatim.
"""
from __future__ import annotations

import re
from pathlib import Path

ADJACENT_RE = re.compile(r"^\s*add_adjacent_zone\s*\(([^;]*)\)\s*;", re.MULTILINE)
INIT_ZONE_RE = re.compile(r'^\s*(?:zones|init_zones)\s*\[[^\]]*\]\s*=\s*"([^"]+)"\s*;', re.MULTILINE)


class ZoneError(ValueError):
    pass


def read_waw_zones(waw_gsc: str) -> tuple[list[str], list[str]]:
    """(initial zones, add_adjacent_zone argument lists) from a WaW map script."""
    adjacency = [m.strip() for m in ADJACENT_RE.findall(waw_gsc)]
    initial = list(dict.fromkeys(INIT_ZONE_RE.findall(waw_gsc)))
    if not adjacency:
        raise ZoneError("no add_adjacent_zone calls in the WaW map script")
    if not initial:
        initial = [re.findall(r'"([^"]+)"', adjacency[0])[0]]
    return initial, adjacency


def patch_bo2_script(bo2_gsc: str, project: str, initial: list[str], adjacency: list[str]) -> str:
    """Replace the template's single ``start_zone`` setup with the WaW zones."""
    init_block = "".join(f'    init_zones[{i}] = "{z}";\n' for i, z in enumerate(initial))
    out, n = re.subn(r'^\s*init_zones\[0\] = "start_zone";\n', init_block, bo2_gsc, flags=re.MULTILINE)
    if n != 1 and init_block not in bo2_gsc:
        raise ZoneError("template init_zones block not found in the map script")
    body = "".join(f"    add_adjacent_zone( {args} );\n" for args in adjacency)
    func = re.compile(rf"^{re.escape(project)}_zone_init\(\)\s*\{{.*?^\}}", re.MULTILINE | re.DOTALL)
    if not func.search(out):
        raise ZoneError(f"{project}_zone_init() not found in the map script")
    return func.sub(lambda _: f"{project}_zone_init()\n{{\n    // zone graph from the WaW map script\n{body}}}", out, count=1)


def apply(waw_gsc_path: Path, bo2_gsc_path: Path, project: str) -> tuple[list[str], int]:
    initial, adjacency = read_waw_zones(waw_gsc_path.read_text(encoding="utf-8", errors="replace"))
    text = bo2_gsc_path.read_text(encoding="utf-8", errors="replace")
    bo2_gsc_path.write_text(patch_bo2_script(text, project, initial, adjacency), encoding="utf-8")
    return initial, len(adjacency)
