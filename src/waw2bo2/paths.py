"""WaW PathData (dumped as ``<map>.paths.json``) -> T6 bridge ``BSP/paths.json``.

WaW and BO2 share the path node graph layout: nodes, per-node links, and the
node-pair visibility table (n*(n-1) bits in both games, copied verbatim). What
differs, measured against stock ``zm_nuked``:

* node types: BO2 inserted PILLAR/AMBUSH/EXPOSED after COVER_LEFT, so WaW
  types >= CONCEALMENT_STAND shift by one; WaW's WIDE_RIGHT/LEFT become RIGHT/LEFT
* spawnflags 0x100000 / 0x200000 are BO2 compiler clearance classes, mirrored
  in the link flags as 0x8 / 0x20. WaW has no equivalent; every node and link
  gets both (open to all AI sizes)
* traversals: a negotiation node's ``animscript`` names the script
  ``maps/mp/animscripts/traverse/<animscript>.gsc``. WaW names have no BO2
  script, so each is mapped by kind and height onto a BO2 zombie traverse
  animation (zm_nuked's ``zm_traverse`` substates) through a generated script
"""
from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from pathlib import Path

# WaW nodeType -> T6 nodeType
T4_WIDE_RIGHT, T4_WIDE_LEFT, T4_CONCEALMENT_STAND = 8, 9, 10
T6_COVER_RIGHT, T6_COVER_LEFT = 6, 7
T4_NEGOTIATION_BEGIN, T4_NEGOTIATION_END = 16, 17

T6_CLEARANCE_SPAWNFLAGS = 0x100000 | 0x200000
T6_CLEARANCE_LINK_FLAGS = 0x8 | 0x20

# BO2 stock traversal kept as is (maps/mp/animscripts/traverse/zm_mantle_over_40, patch_zm)
MANTLE_SCRIPT = "zm_mantle_over_40"
MANTLE_MAX_HEIGHT = 48.0
# zm_traverse substates of the zm_nuked zombie (animstatedefs/zm_nuked_basic.asd)
JUMP_DOWN = (40, 72, 96, 127, 176)
JUMP_UP = (72, 96, 127, 154, 222)
JUMP_UP_TO_CLIMB = "jump_up_to_climb"
GENERATED_PREFIX = "waw2bo2_"


class PathError(RuntimeError):
    pass


def t6_node_type(t4_type: int) -> int:
    if t4_type == T4_WIDE_RIGHT:
        return T6_COVER_RIGHT
    if t4_type == T4_WIDE_LEFT:
        return T6_COVER_LEFT
    if t4_type >= T4_CONCEALMENT_STAND:
        return t4_type + 1
    return t4_type


def _nearest(value: float, options: tuple[int, ...]) -> int:
    return min(options, key=lambda option: abs(option - value))


ASD_STATE_RE = re.compile(r"^(\w+)\s*:[^\n]*\n\{(.*?)^\}", re.MULTILINE | re.DOTALL)


def traverse_substates(asd_text: str) -> set[str]:
    """Aliases of the zombie's zm_traverse state that also have a _crawl twin
    in zm_traverse_crawl (BO2's dosimpletraverse appends _crawl for legless
    zombies). Commented-out ('//') entries are not substates."""
    states = {}
    for name, body in ASD_STATE_RE.findall(asd_text):
        aliases = set()
        for line in body.splitlines():
            line = line.strip()
            if line and not line.startswith("//"):
                aliases.add(line.split()[0])
        states[name] = aliases
    walking = states.get("zm_traverse", set())
    crawling = states.get("zm_traverse_crawl", set())
    return {a for a in walking if a + "_crawl" in crawling}


def _heights(substates: set[str], prefix: str) -> dict[int, str]:
    found = {}
    for alias in substates:
        match = re.fullmatch(prefix + r"(\d+)", alias)
        if match:
            found[int(match.group(1))] = alias
    return found


def traverse_alias_measured(animscript: str, height: float | None, substates: set[str]) -> str | None:
    """BO2 zm_traverse alias for a WaW traversal, or None for a mantle.

    BO2's dotraverse plays the animation in noclip with no height warping, so
    the zombie ends where the animation's root motion puts it: the substate is
    chosen by the MEASURED height (end.z - begin.z of the negotiation pair)
    among the substates the zombie actually has (measured: WaW's
    'jump_up_to_climb' has no BO2 substate; zombies stalled at those nodes).
    The number in a WaW name is only used when the height is unknown."""
    name = animscript.lower()
    number = re.search(r"(\d+)", name)
    if "mantle" in name or "hop" in name or "window" in name or "barrier" in name:
        return None
    # BO2's zombie inherits WaW's traversal set: a same-named substate plays the
    # same animation WaW played (e.g. jump_up_to_climb = ai_zombie_jump_up_2_climb)
    same = name.removeprefix("zombie_")
    if same in substates:
        return same
    if height is None:
        if not number:
            return None
        height = float(number.group(1)) * (-1.0 if "down" in name else 1.0)
    if abs(height) <= MANTLE_MAX_HEIGHT and "down" not in name and "up" not in name and "climb" not in name:
        return None
    downs, ups = _heights(substates, "jump_down_"), _heights(substates, "jump_up_")
    if height < 0 and downs:
        return downs[_nearest(-height, tuple(downs))]
    if height >= 0 and ups:
        best = _nearest(height, tuple(ups))
        if abs(best - height) > 24 and "jump_up_2_climb" in substates:
            return "jump_up_2_climb"
        return ups[best]
    return None


def traverse_alias(animscript: str, height: float | None) -> str | None:
    """BO2 zm_traverse alias for a WaW traversal, or None for a mantle.

    ``height`` is end.z - begin.z of the negotiation pair; a number in the WaW
    name (``zombie_jump_down_127``) wins over the measured height."""
    name = animscript.lower()
    number = re.search(r"(\d+)", name)
    if "climb" in name:
        return JUMP_UP_TO_CLIMB
    if "down" in name:
        return f"jump_down_{_nearest(float(number.group(1)) if number else abs(height or 0.0), JUMP_DOWN)}"
    if "up" in name:
        value = float(number.group(1)) if number else abs(height or 0.0)
        return JUMP_UP_TO_CLIMB if value > JUMP_UP[-1] + 24 else f"jump_up_{_nearest(value, JUMP_UP)}"
    if "mantle" in name or "hop" in name or "window" in name or "barrier" in name:
        return None
    if height is None or abs(height) <= MANTLE_MAX_HEIGHT:
        return None
    if height < 0:
        return f"jump_down_{_nearest(-height, JUMP_DOWN)}"
    return JUMP_UP_TO_CLIMB if height > JUMP_UP[-1] + 24 else f"jump_up_{_nearest(height, JUMP_UP)}"


def traverse_script(alias: str) -> str:
    return (
        "// generated by waw2bo2: WaW traversal -> BO2 zm_traverse substate\n"
        "#include maps\\mp\\animscripts\\traverse\\shared;\n"
        "#include maps\\mp\\animscripts\\traverse\\zm_shared;\n\n"
        "main()\n{\n"
        f"    dosimpletraverse( \"{alias}\" );\n"
        "}\n"
    )


@dataclass
class PathSummary:
    nodes: int = 0
    links: int = 0
    vis_bytes: int = 0
    traversals: dict[str, str] = field(default_factory=dict)
    scripts: list[str] = field(default_factory=list)


def convert_paths(waw: dict, substates: set[str] | None = None) -> tuple[dict, dict[str, str], PathSummary]:
    """Returns (T6 paths json, {WaW animscript: T6 animscript}, summary).
    ``substates``: the zombie's zm_traverse aliases (traverse_substates); a
    traversal is then mapped per node by its measured height."""
    nodes = waw["nodes"]
    count = int(waw["nodeCount"])
    if count != len(nodes):
        raise PathError(f"nodeCount {count} but {len(nodes)} nodes")
    if count > 0xFFFF - 128:
        raise PathError(f"{count} path nodes exceed the uint16 node index range")
    vis_hex = waw.get("pathVis", "")
    bits = count * (count - 1)
    sizes = {bits // 8, (bits + 7) // 8}
    if waw.get("visBytes", 0) and waw["visBytes"] not in sizes:
        raise PathError(f"visBytes {waw['visBytes']} is outside {sorted(sizes)} for {count} nodes; unknown vis layout")
    if len(vis_hex) != 2 * waw.get("visBytes", 0):
        raise PathError("pathVis hex length does not match visBytes")

    by_targetname = {n["targetname"]: n for n in nodes if n.get("targetname")}
    renamed: dict[str, str] = {}
    per_node: dict[int, str] = {}
    for index, node in enumerate(nodes):
        script = node.get("animscript", "")
        if node["type"] != T4_NEGOTIATION_BEGIN or not script:
            continue
        end = by_targetname.get(node.get("target", ""))
        height = end["origin"][2] - node["origin"][2] if end else None
        if substates is not None:
            alias = traverse_alias_measured(script, height, substates)
            per_node[index] = MANTLE_SCRIPT if alias is None else GENERATED_PREFIX + alias
            renamed.setdefault(script, per_node[index])
            continue
        if script in renamed:
            continue
        alias = traverse_alias(script, height)
        renamed[script] = MANTLE_SCRIPT if alias is None else GENERATED_PREFIX + alias

    out_nodes = []
    links = 0
    for index, node in enumerate(nodes):
        node_links = []
        for target, dist, disconnect, negotiation in node["links"]:
            if not 0 <= target < count:
                raise PathError(f"node {index} links to node {target} of {count}")
            node_links.append({"node": target, "dist": dist, "disconnectCount": disconnect,
                               "negotiationLink": negotiation, "flags": T6_CLEARANCE_LINK_FLAGS})
        links += len(node_links)
        out_nodes.append({
            "type": t6_node_type(node["type"]),
            "spawnflags": node["spawnflags"] | T6_CLEARANCE_SPAWNFLAGS,
            "targetname": node["targetname"],
            "script_linkname": node["script_linkname"],
            "script_noteworthy": node["script_noteworthy"],
            "target": node["target"],
            "animscript": per_node.get(index, renamed.get(node["animscript"], node["animscript"])),
            "animscriptfunc": node["animscriptfunc"],
            "origin": node["origin"],
            "angle": node["angle"],
            "forward": node["forward"],
            "radius": node["radius"],
            "minUseDistSq": node["minUseDistSq"],
            "overlap": node["overlap"],
            "links": node_links,
        })
    summary = PathSummary(nodes=count, links=links, vis_bytes=len(vis_hex) // 2, traversals=dict(renamed))
    if per_node:
        summary.traversals = {}
        for index, script in per_node.items():
            key = f"{nodes[index]['animscript']} -> {script}"
            summary.traversals[key] = str(int(summary.traversals.get(key, "0")) + 1)
    renamed = {**renamed, **{f"#{i}": s for i, s in per_node.items()}}
    return {"version": 1, "nodeCount": count, "visBytes": len(vis_hex) // 2, "pathVis": vis_hex,
            "nodes": out_nodes}, renamed, summary


def stage_paths(paths_file: Path, bsp_dir: Path, project_root: Path,
                asd: Path | None = None) -> tuple[PathSummary, dict[str, str]]:
    """Write BSP/paths.json and the generated traversal scripts. ``asd``: the
    zombie actor's anim state definitions (traversals mapped by measured height)."""
    waw = json.loads(paths_file.read_text(encoding="utf-8"))
    substates = None
    if asd is not None:
        substates = traverse_substates(asd.read_text(encoding="utf-8", errors="replace"))
        if not substates:
            raise PathError(f"{asd}: no zm_traverse substates with _crawl twins")
    t6, renamed, summary = convert_paths(waw, substates)
    bsp_dir.mkdir(parents=True, exist_ok=True)
    (bsp_dir / "paths.json").write_text(json.dumps(t6, separators=(",", ":")) + "\n", encoding="utf-8")
    folder = project_root / "maps" / "mp" / "animscripts" / "traverse"
    for script in sorted(set(renamed.values())):
        if not script.startswith(GENERATED_PREFIX):
            continue
        folder.mkdir(parents=True, exist_ok=True)
        (folder / f"{script}.gsc").write_text(traverse_script(script.removeprefix(GENERATED_PREFIX)), encoding="utf-8")
        summary.scripts.append(f"maps/mp/animscripts/traverse/{script}.gsc")
    return summary, renamed


def traverse_scripts(project_root: Path) -> list[str]:
    folder = project_root / "maps" / "mp" / "animscripts" / "traverse"
    return [f"maps/mp/animscripts/traverse/{p.name}" for p in sorted(folder.glob(f"{GENERATED_PREFIX}*.gsc"))]
