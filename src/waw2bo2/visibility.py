"""WaW cells and portals -> T6 world visibility.

The T4 dumper writes ``<bsp>.visibility.json``: the map's point-to-cell tree,
every cell with its portals, and the surfaces and static models each cell owns.
T6's renderer walks the same structures (cell stride 48, portal plane tested
negative from the viewer's side, camera in solid walks every cell), so a walled
off room costs nothing here, as in WaW.

A T6 surface lives in exactly one cell: ``sortedSurfIndex`` holds
``staticSurfaceCount`` entries and every cell owns a contiguous range of it.
WaW's surfaces already do, so merged meshes keep the cell in their key
(fbx.merge_surfaces). Static models may repeat across cells. Brush model
surfaces move and belong to no cell.

``BSP/visibility.json`` is read by the bridge's GfxWorldLinker. Without a dump
the world stays one cell, and that is reported.
"""
from __future__ import annotations

import json
from pathlib import Path

# byte_3436034 and byte_3436134 (cell visited flags) sit 256 bytes apart
T6_MAX_CELLS = 256


def read(path: Path) -> dict | None:
    if not path.is_file():
        return None
    return json.loads(path.read_text(encoding="utf-8"))


def cell_at(visibility: dict, point) -> int:
    """The cell holding ``point``, -1 in solid. A node is a plane (value above
    cellCount), its right child offset and its left child; a leaf is cell + 1."""
    nodes, planes, count = visibility["nodes"], visibility["planes"], visibility["cellCount"]
    i = 0
    while nodes[i] > count:
        plane = planes[nodes[i] - count - 1]
        front = point[0] * plane[0] + point[1] * plane[1] + point[2] * plane[2] - plane[3] > 0
        i += 2 if front else nodes[i + 1]
    return nodes[i] - 1


def problem(visibility: dict, world) -> str | None:
    cells = visibility.get("cells") or []
    if not cells or not visibility.get("nodes"):
        return "the dump holds no cells"
    if len(cells) > T6_MAX_CELLS:
        return f"{len(cells)} cells, T6 walks at most {T6_MAX_CELLS}"
    if any(p["cell"] not in range(len(cells)) for c in cells for p in c["portals"]):
        return "a portal leads outside the cell list"
    owners = [s for c in cells for s in c["surfaces"]]
    if len(owners) != len(set(owners)):
        return "a surface sits in two cells"
    if any(s >= len(world.surfaces) for s in owners):
        return "a cell lists a surface outside the world"
    return None


def assign(report, world, visibility: dict | None, source_probes=None, plan=None) -> None:
    """Give every static surface its cell and every cell its static models and
    T6 probes. Runs before surfaces merge; anything WaW left out of every cell
    goes to the cell around it."""
    if visibility is None:
        report.warnings.append("visibility: the render dump has no cells (re-extract gfxworld with the "
                               "visibility T4 dumper); the world links as one cell and walls hide nothing")
        return
    if reason := problem(visibility, world):
        report.warnings.append(f"visibility: {reason}; the world links as one cell and walls hide nothing")
        return
    cells = visibility["cells"]
    for index, cell in enumerate(cells):
        for surface in cell["surfaces"]:
            world.surfaces[surface].cell = index

    placed = 0
    for surface in world.surfaces:
        if surface.cell < 0 and not surface.brush_model:
            centre = [(surface.mins[k] + surface.maxs[k]) * .5 for k in range(3)]
            surface.cell = max(cell_at(visibility, centre), 0)
            placed += 1
    listed = {m for cell in cells for m in cell["smodels"]}
    for index, model in enumerate(world.static_models):
        if index not in listed:
            cells[max(cell_at(visibility, model.origin), 0)]["smodels"].append(index)
            placed += 1

    for index, cell in enumerate(cells):
        cell["probes"] = []
        if source_probes is not None and plan is not None and index < len(source_probes.cells):
            cell["probes"] = sorted({plan.t6_index(p) for p in source_probes.cells[index]} - {0})
    world.visibility = visibility
    report.content["visibility"] = {"cells": len(cells), "portals": sum(len(c["portals"]) for c in cells),
                                    "nodes": len(visibility["nodes"]), "placed_by_position": placed}


def write(report, world, mesh_cells: dict[int, int], path: Path) -> None:
    """BSP/visibility.json: the WaW structures with surfaces as FBX mesh indices."""
    if path.exists():
        path.unlink()
    visibility = world.visibility
    if visibility is None:
        return
    meshes: list[list[int]] = [[] for _ in visibility["cells"]]
    for mesh, cell in sorted(mesh_cells.items()):
        if cell >= 0:
            meshes[cell].append(mesh)
    cells = [{"mins": c["mins"], "maxs": c["maxs"], "meshes": meshes[i], "smodels": sorted(c["smodels"]),
              "probes": c["probes"], "portals": c["portals"]} for i, c in enumerate(visibility["cells"])]
    path.write_text(json.dumps({"planes": visibility["planes"], "nodes": visibility["nodes"], "cells": cells},
                               separators=(",", ":")) + "\n", encoding="utf-8")
    report.content["visibility"]["meshes"] = sum(len(m) for m in meshes)
