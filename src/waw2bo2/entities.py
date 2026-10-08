"""WaW map entities -> T6 bridge ``BSP/entities.json`` + ``BSP/spawns.json``.

The entity string format is the same in both games. What cannot be carried
over as-is is reported, never silently dropped:

* ``node_*``  path nodes are kept (as in stock T6 zones); the graph itself is
  linked from BSP/paths.json (see paths.py)
* ``light``   WaW's baked-light entities have no meaning for the T6 runtime
* brush-model entities (``"model" "*N"``: zones, doors, triggers) are kept;
  the bridge links submodel N from BSP/submodels.json in the same order
* WaW zombie spawner classes are renamed to a T6 zombie actor class
"""
from __future__ import annotations

import json
import math
import re
from collections import Counter
from pathlib import Path

ENTITY_RE = re.compile(r"\{([^{}]*)\}")
KV_RE = re.compile(r'"([^"]*)"\s+"([^"]*)"')

DROP_CLASSES = {"light"}
WORLDSPAWN_KEYS = {"classname", "skyboxmodel", "sundirection", "suncolor", "sunlight", "_color", "ambient",
                   "diffusefraction"}
# BO2 zombie actor spawner class shipped with the zm_test template (zm_nuked AI)
ZOMBIE_ACTOR_CLASS = "actor_zm_nuked_basic_01"

# Wall buys keep their WaW weapon (zombie_weapon_upgrade); the converted
# weapons are carried under their WaW names and t6bridge maps renamed or
# uncarried ones once mod.ff's weapon set is known (map_wall_buys).
RISE_SUFFIX = "_rise"
ZBARRIER_ASSET ="zmcore_basicwoodbarrier"


def parse_entities(text: str) -> list[dict[str, str]]:
    return [dict(KV_RE.findall(body)) for body in ENTITY_RE.findall(text)]


def _origin(ent: dict) -> tuple[float, float, float]:
    return tuple(float(v) for v in ent.get("origin", "0 0 0").split())  # type: ignore[return-value]


def _unique_wall_buy_targets(entities: list[dict], renamed: Counter) -> None:
    """T6 uses getent(target) for each wall buy; that requires one model."""
    buys = [e for e in entities if e.get("targetname") == "weapon_upgrade" and e.get("target")]
    for target in sorted({e["target"] for e in buys}):
        triggers = [e for e in buys if e["target"] == target]
        models = [e for e in entities if e.get("targetname") == target]
        if len(models) <= 1:
            continue
        if len(triggers) != len(models) or any(m.get("classname") != "script_model" for m in models):
            raise ValueError(f"ambiguous wall-buy target {target}: {len(triggers)} triggers, {len(models)} models")
        remaining = list(models)
        for index, trigger in enumerate(triggers):
            tx, ty, tz = _origin(trigger)
            model = min(remaining, key=lambda m: sum((a - b) ** 2 for a, b in zip((tx, ty, tz), _origin(m))))
            remaining.remove(model)
            unique_name = f"waw2bo2_wallbuy_{target}_{index}"
            trigger["target"] = unique_name
            model["targetname"] = unique_name
            renamed[f"wall buy target {target} uniquely paired"] += 1


def convert_entities(ents: list[dict[str, str]], project: str,
                     animscripts: dict[str, str] | None = None) -> tuple[list[dict], dict, dict]:
    out: list[dict] = []
    dropped: Counter = Counter()
    renamed: Counter = Counter()
    targetnames = {e.get("targetname") for e in ents}
    volume_targets = {e["target"] for e in ents if e.get("classname") == "info_volume" and e.get("target")}
    world = next((e for e in ents if e.get("classname") == "worldspawn"), {"classname": "worldspawn"})
    ws = {k: v for k, v in world.items() if k in WORLDSPAWN_KEYS}
    ws["classname"] = "worldspawn"
    ws["skyboxmodel"] = f"skybox_{project}"
    out.append(ws)
    for ent in ents:
        cls = ent.get("classname", "")
        if cls == "worldspawn":
            continue
        if cls.startswith("node_"):
            # Stock T6 zones keep every node entity in the entity string, in
            # PathData order (the bridge links the graph from BSP/paths.json).
            ent = dict(ent)
            if animscripts and ent.get("animscript") in animscripts:
                ent["animscript"] = animscripts[ent["animscript"]]
            out.append(ent)
            continue
        if cls in DROP_CLASSES:
            dropped[f"{cls} (baked lighting only)"] += 1
            continue
        ent = dict(ent)
        if cls == "script_struct" and ent.get("targetname") == "exterior_goal":
            # WaW goals target brush planks, a "clip" brush and a struct inside;
            # T6 requires a native zbarrier. The zbarrier joins the goal's
            # ORIGINAL target group: BO2 _zm_blockers then finds the zbarrier,
            # turns the WaW brushes (clip included) into chunks it deletes in
            # favour of the zbarrier (whose collision model blocks the window),
            # and uses the struct as trigger_location (player repair range).
            # Measured on zm_prison: goal -> {zbarrier, struct 48 in, neg node}.
            target = ent.get("target")
            if target and target in targetnames:
                linked = [e for e in ents if e.get("targetname") == target]
                if linked and not any(e.get("classname", "").startswith("zbarrier_") for e in linked):
                    native_target = target
                    out.append({
                        "classname": "zbarrier_zmcore_BasicWoodBarrier",
                        "type": "zmcore_BasicWoodBarrier",
                        "targetname": native_target,
                        "origin": ent.get("origin", "0 0 0"),
                        "angles": ent.get("angles", "0 0 0"),
                        "zbarriernumboards": "6",
                        **{f"zbarrierboardmodel{i}": f"p6_anim_zm_barricade_board_0{i}" for i in range(1, 7)},
                        **{f"zbarrierboardanim{i}": f"o_zombie_board_{i}_repair" for i in range(1, 7)},
                        **{f"zbarriertearanim{i}": f"o_zombie_board_{i}_pull" for i in range(1, 7)},
                    })
                    renamed["WaW brush barricades -> T6 zbarriers"] += 1
        if cls == "info_volume":
            # BO2 _zm kills any player not touching a "player_volume" in an
            # enabled zone (player_out_of_playable_area_monitor). WaW's zone
            # manager counts every info_volume named after a zone, whatever its
            # script_noteworthy ("player_zone", "player_spawners" or none, as on
            # initial_zone). Non-zone volumes stay harmless: the zone must be enabled.
            renamed[f"info_volume script_noteworthy {ent.get('script_noteworthy')} -> player_volume"] += 1
            ent["script_noteworthy"] = "player_volume"
        if cls == "trigger_multiple" and ent.get("targetname") == "playable_area" and "script_noteworthy" not in ent:
            # WaW marks where players may stand with "playable_area" triggers
            # and never kills outside zones. BO2 kills players outside enabled
            # zones unless they touch a "life_brush"; the WaW spawn house is in
            # no zone volume, only in these triggers.
            ent["script_noteworthy"] = "life_brush"
            renamed["trigger_multiple playable_area -> life_brush"] += 1
        if cls.startswith("actor_") and "zombie" in cls:
            renamed[f"{cls} -> {ZOMBIE_ACTOR_CLASS}"] += 1
            ent["classname"] = ZOMBIE_ACTOR_CLASS
            ent.pop("model", None)
            if ent.get("script_noteworthy") == "zombie_spawner" and ent.get("targetname"):
                # WaW (_zombiemode_spawner/_zone_manager): a zone volume targets
                # its spawner actors. A plain spawner's zombie spawns at the actor
                # and walks to one of the 3 nearest windows; a "riser" spawner's
                # zombie rises at the zone's rise structs instead (below), never
                # at the actor, which mappers park outside the playable space.
                # BO2's zone manager spawns at script_structs named volume.target;
                # a struct without script_string also picks the nearest windows.
                if ent.get("script_string") != "riser":
                    out.append({"classname": "script_struct", "targetname": ent["targetname"],
                                "origin": ent.get("origin", "0 0 0"), "angles": ent.get("angles", "0 0 0"),
                                "script_noteworthy": "spawn_location"})
                    renamed["WaW zombie spawner -> spawn_location struct"] += 1
                else:
                    renamed["WaW riser spawner (rises at its zone's _rise structs)"] += 1
        if cls == "script_struct" and ent.get("targetname", "").endswith(RISE_SUFFIX) \
                and ent["targetname"][:-len(RISE_SUFFIX)] in volume_targets:
            # WaW rise_locations = structs named volume.target + "_rise"; their
            # script_noteworthy ("find_flesh", "riser_door") is what "risen"
            # hands to zombie_think. BO2 riser_location structs named
            # volume.target pass their script_string the same way.
            rise = {"classname": "script_struct", "targetname": ent["targetname"][:-len(RISE_SUFFIX)],
                    "origin": ent.get("origin", "0 0 0"), "angles": ent.get("angles", "0 0 0"),
                    "script_noteworthy": "riser_location"}
            # WaW: "find_flesh" chases players; anything else ("riser_door",
            # stray values) goes to the nearest windows. BO2 reads any other
            # string as a barricade id and fails without a matching goal, and
            # its no-string default is exactly WaW's nearest-windows path.
            if ent.get("script_noteworthy") == "find_flesh":
                rise["script_string"] = "find_flesh"
            out.append(rise)
            renamed["WaW rise struct -> riser_location struct"] += 1
        out.append(ent)

    _unique_wall_buy_targets(out, renamed)

    starts = [e for e in ents if e.get("targetname") == "initial_spawn_points"] or \
             [e for e in ents if e.get("classname") == "info_player_start"]
    points = [{"origin": e.get("origin", "0 0 0"), "angles": e.get("angles", "0 0 0")} for e in starts]
    if not any(e.get("classname") == "mp_global_intermission" for e in out) and points:
        out.append({"classname": "mp_global_intermission", **points[0]})
    spawns = zombies_spawns(points)
    summary = {"entities": len(out), "dropped": dict(dropped), "renamed": dict(renamed),
               "spawn_points": len(points)}
    return out, spawns, summary


def zombies_spawns(points: list[dict]) -> dict:
    """The bridge turns every spawns.json point into one entity per multiplayer
    spawn class (11 per point for attackers/defenders, 3 for FFA). Zombies
    spawns players at the initial_spawn_points structs instead, and those
    entities count against the 1024-entity limit ("G_Spawn: no free entities"
    once the map's scripts spawn their own). Keep one FFA point as a fallback."""
    return {"attackers": [], "defenders": [], "FFA": points[:1]}


def script_model_names(ents: list[dict]) -> set[str]:
    return {e["model"] for e in ents if e.get("classname") == "script_model" and e.get("model")
            and not e["model"].startswith("*")}


def point_in_volume(point, ent: dict, clip) -> bool:
    """Is a world point inside a brush-model entity? Brush-model brushes are
    stored relative to the entity origin (the entity's angles are ignored:
    WaW zone volumes are never rotated)."""
    model = ent.get("model", "")
    if not model.startswith("*") or clip is None:
        return False
    index = int(model[1:])
    if index >= len(clip.submodels):
        return False
    ox, oy, oz = _origin(ent)
    local = (point[0] - ox, point[1] - oy, point[2] - oz)
    for brush_id in clip.submodels[index].brushes:
        planes = clip.brushes[brush_id].planes
        if all(n[0] * local[0] + n[1] * local[1] + n[2] * local[2] <= d + 0.1 for n, d, _ in planes):
            return True
    return False


# WaW _zombiemode::coop_player_spawn_placement puts player i at
# getstructarray("initial_spawn_points")[i] (entity order), so with WaW's four
# co-op players only the first four structs are ever spawn points. Maps keep
# more for their own scripts (nuketown's teleporter sends players to [4..7]).
WAW_COOP_PLAYERS = 4
# BO2 _zm_gametype::onspawnplayer first takes script_noteworthy "initial_spawn"
# structs whose script_string has "<gametype>_<start location>" and only falls
# back to a random pick over every initial_spawn_points struct when none match.
# The zm_test template every map is built from sets zclassic + classic_spawn.
BO2_SPAWN_MATCH = "zclassic_classic_spawn"


def add_bo2_initial_spawns(out: list[dict], clip, start_zones: list[str], summary: dict) -> list[dict]:
    """Give BO2 exactly WaW's start points as matching ``initial_spawn`` structs.

    The WaW ``initial_spawn_points`` structs are left untouched: map scripts
    index them, and renaming any of them shifts those indices. A chosen point
    outside every start zone / life brush is reported (BO2 kills players
    outside enabled zones unless they touch a life brush), not moved."""
    starts = [e for e in out if e.get("targetname") == "initial_spawn_points"]
    if not starts:
        raise ValueError("map has no initial_spawn_points")
    chosen = starts[:WAW_COOP_PLAYERS]
    volumes = [e for e in out if (e.get("classname") == "info_volume" and e.get("targetname") in start_zones)
               or e.get("script_noteworthy") == "life_brush"]
    outside = [s for s in chosen
               if clip is not None and volumes
               and not any(point_in_volume((lambda o: (o[0], o[1], o[2] + 32))(_origin(s)), v, clip) for v in volumes)]
    for s in chosen:
        out.append({"classname": "script_struct", "targetname": "waw2bo2_initial_spawn",
                    "script_noteworthy": "initial_spawn", "script_string": BO2_SPAWN_MATCH,
                    "origin": s.get("origin", "0 0 0"), "angles": s.get("angles", "0 0 0")})
    summary["spawn_points"] = len(chosen)
    summary["spawn_points_unused_by_waw_coop_placement"] = len(starts) - len(chosen)
    summary["spawn_points_outside_start_zones"] = len(outside)
    return chosen


# WaW maps built on the prototype/asylum/sumpf scripts have no zone graph:
# _zombiemode spawns from every "zombie_spawner_init" actor, and a door or
# debris pile adds the spawners its pieces target (target or script_string,
# _zombiemode_blockers::add_new_zombie_spawners) once opened. BO2 spawns only
# from enabled zones, so each spawner group becomes a zone whose volume spans
# the whole map (WaW never limits spawners by player position), and a group a
# door unlocks is connected by a flag that door sets when it opens.
SYNTH_ZONE = "waw2bo2_zone"
INIT_SPAWNERS = "zombie_spawner_init"
DOOR_TRIGGERS = ("zombie_door", "zombie_debris")
# measured on converted WaW info_volume brushes (contents, surface flags)
VOLUME_FLAGS = (134217729, 262272)


def _box_brush(mins, maxs, flags) -> dict:
    corners = [[x, y, z] for x in (mins[0], maxs[0]) for y in (mins[1], maxs[1]) for z in (mins[2], maxs[2])]
    return {"mins": list(mins), "maxs": list(maxs), "contents": flags[0],
            "axial": [list(flags)] * 6, "sides": [], "verts": corners}


def synthesize_zones(out: list[dict], clip, bsp_dir: Path, summary: dict) -> tuple[list[str], list[str]]:
    """Zones for a WaW map without add_adjacent_zone; returns (initial, adjacency)."""
    spawners = [e for e in out if e.get("classname") == ZOMBIE_ACTOR_CLASS and e.get("targetname")]
    groups = sorted({e["targetname"] for e in spawners}, key=lambda g: (g != INIT_SPAWNERS, g))
    if not groups:
        raise ValueError("map has no zone graph and no named zombie spawners")
    subs_path = bsp_dir / "submodels.json"
    subs = json.loads(subs_path.read_text(encoding="utf-8"))["submodels"]
    trigger_models = {e["model"] for e in out if e.get("model", "").startswith("*")
                      and e.get("classname", "").startswith(("info_volume", "trigger_"))}
    flags = next(((b["contents"], b["axial"][0][1]) for m in sorted(trigger_models)
                  if int(m[1:]) - 1 < len(subs) for b in subs[int(m[1:]) - 1]["brushes"][:1]), VOLUME_FLAGS)
    world = clip.submodels[0]
    mins = [v - 512 for v in world.mins]
    maxs = [v + 512 for v in world.maxs]
    by_name: dict[str, list[dict]] = {}
    for e in out:
        by_name.setdefault(e.get("targetname", ""), []).append(e)
    have_struct = {(e.get("targetname"), e.get("origin")) for e in out if e.get("classname") == "script_struct"}
    initial, adjacency, locked = [], [], []
    for index, group in enumerate(groups):
        zone = SYNTH_ZONE if index == 0 else f"{SYNTH_ZONE}_{index}"
        if index and group != INIT_SPAWNERS:
            # entities that hand this group to add_new_zombie_spawners, and the
            # door/debris triggers they belong to (themselves or their owner)
            pieces = [e for e in out if group in (e.get("target"), e.get("script_string"))
                      and e.get("classname") != ZOMBIE_ACTOR_CLASS]
            doors = []
            for piece in pieces:
                if piece.get("targetname") in DOOR_TRIGGERS:
                    doors.append(piece)
                doors += [t for t in out if t.get("targetname") in DOOR_TRIGGERS and piece.get("targetname")
                          and t.get("target") == piece["targetname"]]
            doors = list({id(d): d for d in doors}.values())
            if not doors:
                locked.append(group)
                continue
            flag = f"{zone}_open"
            for door in doors:
                door["script_flag"] = ",".join(filter(None, [door.get("script_flag"), flag]))
            adjacency.append(f'"{SYNTH_ZONE}", "{zone}", "{flag}"')
        else:
            initial.append(zone)
        subs.append({"mins": mins, "maxs": maxs, "brushes": [_box_brush(mins, maxs, flags)]})
        out.append({"classname": "info_volume", "targetname": zone, "target": group, "origin": "0 0 0",
                    "model": f"*{len(subs)}", "script_noteworthy": "player_volume"})
        for spawner in by_name[group]:
            if spawner.get("classname") == ZOMBIE_ACTOR_CLASS and (group, spawner.get("origin")) not in have_struct:
                out.append({"classname": "script_struct", "targetname": group,
                            "origin": spawner.get("origin", "0 0 0"), "angles": spawner.get("angles", "0 0 0"),
                            "script_noteworthy": "spawn_location"})
    subs_path.write_text(json.dumps({"submodels": subs}, separators=(",", ":")) + "\n", encoding="utf-8")
    summary["synthesized_zones"] = {"initial": initial, "adjacency": adjacency, "groups": groups,
                                    "never_unlocked": locked}
    return initial, adjacency


def write_entities(ents_file: Path, project: str, bsp_dir: Path, clip=None,
                   start_zones: list[str] | None = None,
                   animscripts: dict[str, str] | None = None) -> tuple[dict, set[str]]:
    ents = parse_entities(ents_file.read_text(encoding="utf-8", errors="replace"))
    out, spawns, summary = convert_entities(ents, project, animscripts)
    if start_zones == [] and clip is not None and (bsp_dir / "submodels.json").exists():
        start_zones = synthesize_zones(out, clip, bsp_dir, summary)[0]
    if any(e.get("targetname") == "initial_spawn_points" for e in out):
        chosen = add_bo2_initial_spawns(out, clip, start_zones or [], summary)
        spawns = zombies_spawns([{"origin": e.get("origin", "0 0 0"), "angles": e.get("angles", "0 0 0")}
                                 for e in chosen])
    bsp_dir.mkdir(parents=True, exist_ok=True)
    (bsp_dir / "entities.json").write_text(json.dumps({"entities": out}, indent=1) + "\n", encoding="utf-8")
    (bsp_dir / "spawns.json").write_text(json.dumps(spawns, indent=1) + "\n", encoding="utf-8")
    return summary, script_model_names(out)


def map_wall_buys(entities_json: Path, table: dict[str, str]) -> list[str]:
    """Point WaW wall buys at the weapons mod.ff carries (``table``: WaW name ->
    BO2 name, "" = not carried). A wall buy for an uncarried weapon is removed:
    BO2's wall-buy setup requires a registered weapon. Returns report lines."""
    data = json.loads(entities_json.read_text(encoding="utf-8"))
    notes, kept = [], []
    for ent in data["entities"]:
        weapon = ent.get("zombie_weapon_upgrade")
        if ent.get("targetname") == "weapon_upgrade" and weapon in table:
            if not table[weapon]:
                notes.append(f"UNSUPPORTED_WALLBUY {weapon} at {ent.get('origin')}: weapon not carried in mod.ff; "
                             f"trigger removed")
                continue
            if table[weapon] != weapon:
                ent["zombie_weapon_upgrade"] = table[weapon]
                notes.append(f"wall buy {weapon} -> {table[weapon]} (renamed weapon)")
        kept.append(ent)
    data["entities"] = kept
    entities_json.write_text(json.dumps(data, indent=1) + "\n", encoding="utf-8")
    return notes


# T6 negotiation begin node type (WaW 16 shifted, see paths.t6_node_type)
T6_NEGOTIATION_BEGIN = 17
BARRIER_NODE_RADIUS = 48.0


def link_barrier_traversals(bsp_dir: Path) -> list[str]:
    """Give each window its traversal the way BO2 barriers expect it.

    BO2 _zm_blockers takes the goal's target group's "Begin" node as neg_start
    and disconnects neg_start -> neg_end while boarded, reconnecting it when
    the boards are gone (stock zm_prison: node at the barrier, end 80 inside).
    WaW places the window's wall_hop negotiation at the goal (measured: 12
    units away, end ~78 inward) but links nothing. The nearest begin node
    within BARRIER_NODE_RADIUS whose end lies inward joins the goal's target
    group, in the path data and in its node entity. Returns report lines."""
    entities_json, paths_json = bsp_dir / "entities.json", bsp_dir / "paths.json"
    data = json.loads(entities_json.read_text(encoding="utf-8"))
    paths = json.loads(paths_json.read_text(encoding="utf-8"))
    nodes = paths["nodes"]
    by_name = {n["targetname"]: n for n in nodes if n.get("targetname")}
    notes, used = [], set()
    for goal in data["entities"]:
        if goal.get("targetname") != "exterior_goal" or not goal.get("target"):
            continue
        origin = [float(v) for v in goal.get("origin", "0 0 0").split()]
        yaw = math.radians([float(v) for v in goal.get("angles", "0 0 0").split()][1])
        forward = (math.cos(yaw), math.sin(yaw))
        best = None
        for index, node in enumerate(nodes):
            end = by_name.get(node.get("target", ""))
            if node["type"] != T6_NEGOTIATION_BEGIN or index in used or end is None:
                continue
            dist = math.hypot(node["origin"][0] - origin[0], node["origin"][1] - origin[1])
            inward = (end["origin"][0] - node["origin"][0]) * forward[0] + (end["origin"][1] - node["origin"][1]) * forward[1]
            if dist <= BARRIER_NODE_RADIUS and inward > 0 and (best is None or dist < best[0]):
                best = (dist, index)
        if best is None:
            notes.append(f"UNSUPPORTED_BARRIER_TRAVERSAL {goal['target']} at {goal.get('origin')}: no inward "
                         f"negotiation within {BARRIER_NODE_RADIUS:g} units; path stays connected while boarded")
            continue
        node = nodes[best[1]]
        used.add(best[1])
        old = node["targetname"]
        for ent in data["entities"]:
            # path data holds float32 origins, entity strings the decimal text
            if ent.get("classname") == "node_negotiation_begin" and ent.get("targetname", "") == old and \
                    all(abs(float(a) - b) < 0.05 for a, b in zip(ent.get("origin", "0 0 0").split(), node["origin"])):
                ent["targetname"] = goal["target"]
                break
        else:
            notes.append(f"UNSUPPORTED_BARRIER_TRAVERSAL {goal['target']}: node entity at {node['origin']} not found")
            continue
        node["targetname"] = goal["target"]
    entities_json.write_text(json.dumps(data, indent=1) + "\n", encoding="utf-8")
    paths_json.write_text(json.dumps(paths, separators=(",", ":")) + "\n", encoding="utf-8")
    return notes
