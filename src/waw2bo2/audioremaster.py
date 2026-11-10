"""Remaster WaW audio as a stock BO2 zombies map would mix it.

WaW's own data says what each sound is for: its bus (music, voice, ambience),
whether it is authored 2D, the map's music state table (musicAlias calls) and
its ambient package. Each such role takes the mix BO2's own zombie maps give
the same kind of sound, measured from the stock alias tables
(raw/soundbank/zmb_*.aliases.csv): bus, volume group, duck group, ducking,
panning, music flag and sends. Loudness moves so WaW's typical level for the
role lands on BO2's, keeping every sound's offset from it.

Every other sound is mixed by class, "<VolumeGroup>|<PanType>": WaW's bus
names the BO2 volume group (gsc_api audio_remaster bus_groups, checked against
the engine sounds both games name alike), and the class takes from the stock
tables its bus and duck group, its falloff curve, and a transfer for how far
it carries, its priority and its occlusion. A transfer reads a value's rank
among stock WaW's values of the class and gives the stock BO2 value of the
same rank (quantile mapping): the map keeps its own order (what carries
farthest still does) on BO2's scale, with no constant of ours in between.
"""
from __future__ import annotations

import bisect
import csv
import functools
import re
from collections import Counter
from pathlib import Path

from . import sounds

# Stock BO2 alias families that hold each role (BO2's own naming).
STOCK_FAMILY = {"music": "mus_", "announcer": "vox_zmba_", "ambience": "amb_"}
# The columns a role takes from its stock profile.
MIX_COLUMNS = ("Bus", "VolumeGroup", "DuckGroup", "Duck", "PanType", "IsMusic", "ReverbSend", "CenterSend")


def _number(value: str) -> float | None:
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def _median(values: list[float]) -> float:
    values = sorted(values)
    middle = len(values) // 2
    return values[middle] if len(values) % 2 else (values[middle - 1] + values[middle]) / 2


@functools.lru_cache(maxsize=2)
def _stock_rows(bo2_root: Path) -> tuple:
    """Every row of BO2's stock zombie alias tables (read once per build)."""
    rows = []
    for path in sorted((bo2_root / "raw/soundbank").glob("zmb_*.aliases.csv")):
        with path.open(encoding="utf-8", errors="replace", newline="") as f:
            rows += list(csv.DictReader(f))
    return tuple(rows)


def stock_profiles(bo2_root: Path, stingers=(), twins=()) -> dict[str, dict]:
    """Role -> {mix: most common stock MIX_COLUMNS values, volume: median VolMax
    of the rows with that mix, rows: how many stock rows it was measured on},
    plus "groups" (stock_group_levels), "classes" (stock_classes) and "twins"
    (stock_twins of the stock alias names in ``twins``)."""
    rows: dict[str, list[dict]] = {role: [] for role in STOCK_FAMILY}
    for row in _stock_rows(bo2_root):
        name = (row.get("Name") or "").lower()
        for role, prefix in STOCK_FAMILY.items():
            if name.startswith(prefix):
                rows[role].append(row)
    profiles = {}
    for role, found in rows.items():
        if not found:
            continue
        # Bus, group and panning first: BO2's scored music (bus_music, 2D)
        # splits over two duck groups, and as a whole tuple lost to the
        # positional radio/jukebox music of the same family.
        heads = Counter((r.get("Bus", ""), r.get("VolumeGroup", ""), r.get("PanType", "")) for r in found)
        head = heads.most_common(1)[0][0]
        found = [r for r in found if (r.get("Bus", ""), r.get("VolumeGroup", ""), r.get("PanType", "")) == head]
        mixes = Counter(tuple(r.get(c, "") for c in MIX_COLUMNS) for r in found)
        mix, count = mixes.most_common(1)[0]
        volumes = [v for r in found if tuple(r.get(c, "") for c in MIX_COLUMNS) == mix
                   for v in [_number(r.get("VolMax"))] if v is not None]
        profiles[role] = {"mix": dict(zip(MIX_COLUMNS, mix)), "volume": _median(volumes) if volumes else None,
                          "rows": count}
    for name in stingers or ():
        profile = _stinger_profile(bo2_root, name)
        if profile is not None:
            profiles["stinger:" + name] = profile
    if rows.get("ambience"):
        profiles["ambience"]["emitters"] = stock_emitters(rows["ambience"])
    profiles["groups"] = stock_group_levels(bo2_root)
    profiles["classes"] = stock_classes(bo2_root)
    profiles["twins"] = stock_twins(bo2_root, twins)
    return profiles


# What a class moves from WaW's scale to BO2's by rank (transfer).
TRANSFER_COLUMNS = ("DistMin", "Falloff", "PriorityMin", "PriorityMax", "OcclusionLevel")
# Fewest reference values (stock WaW, else the map's own) for a transfer.
REFERENCE_MIN_SAMPLES = 20


def read_curves(path: Path) -> dict[str, list]:
    """BO2's driver curves (raw/soundbank/globals/curves.csv): name -> points."""
    if not path.is_file():
        return {}
    with path.open(encoding="utf-8", errors="replace", newline="") as f:
        return {row["name"]: [[float(row[f"x{i}"]), float(row[f"y{i}"])] for i in range(8)]
                for row in csv.DictReader(f)}


def percentiles(values: list[float]) -> list[float]:
    """The 0th to 100th percentile of ``values`` (nearest rank)."""
    values = sorted(values)
    return [values[round(i * (len(values) - 1) / 100)] for i in range(101)]


def transfer(value: float, source: list[float], target: list[float]) -> float:
    """The ``target`` value at the rank ``value`` holds in ``source`` (both
    percentile tables). A value many source rows share sits at the middle of
    their ranks, so WaW's one authored default lands on BO2's typical value;
    a value outside the source's range takes the target's end."""
    low, high = bisect.bisect_left(source, value), bisect.bisect_right(source, value) - 1
    if high >= low:
        rank = (low + high) / 2
    elif low == 0:
        rank = 0.0
    elif low >= len(source):
        rank = len(source) - 1.0
    else:
        rank = low - 1 + (value - source[low - 1]) / (source[low] - source[low - 1])
    index = min(int(rank), len(target) - 2)
    return target[index] + (target[index + 1] - target[index]) * (rank - index)


def stock_classes(bo2_root: Path) -> dict[str, dict]:
    """"<VolumeGroup>|<PanType>" -> how stock BO2 mixes that class: its most
    common bus and duck group, the falloff curve of its ordinary 3D rows
    (sounds.falloff_descriptor: full inside DistMin, falling beyond) with
    that curve's area, and the percentiles of TRANSFER_COLUMNS."""
    curves = read_curves(bo2_root / "raw/soundbank/globals/curves.csv")
    found: dict[str, list[dict]] = {}
    for row in _stock_rows(bo2_root):
        found.setdefault(f"{row.get('VolumeGroup', '')}|{row.get('PanType', '')}", []).append(row)
    classes = {}
    for key, rows in sorted(found.items()):
        if len(rows) < MIN_GROUP_ROWS:
            continue
        values: dict[str, list[float]] = {column: [] for column in TRANSFER_COLUMNS}
        dry, wet = Counter(), Counter()
        for row in rows:
            for column in ("PriorityMin", "PriorityMax", "OcclusionLevel"):
                value = _number(row.get(column))
                if value is not None:
                    values[column].append(value)
            low, high = curves.get(row.get("DryMinCurve")), curves.get(row.get("DryMaxCurve"))
            near, far = _number(row.get("DistMin")), _number(row.get("DistMaxDry"))
            if row.get("PanType") != "3d" or not low or not high or near is None or far is None:
                continue
            shape = sounds.falloff_descriptor(near, far, low, high)
            if shape["standard"]:
                values["DistMin"].append(near)
                values["Falloff"].append(shape["falloff"])
                dry[row["DryMaxCurve"]] += 1
                if sounds.is_falloff(curves.get(row.get("WetMaxCurve")) or [[0, 0]]):
                    wet[row["WetMaxCurve"]] += 1
        routing = Counter((row.get("Bus", ""), row.get("DuckGroup", "")) for row in rows).most_common(1)[0][0]
        entry = {"rows": len(rows), "routing": {"Bus": routing[0], "DuckGroup": routing[1]},
                 "quantiles": {c: percentiles(v) for c, v in values.items() if len(v) >= MIN_GROUP_ROWS}}
        if dry:
            curve = dry.most_common(1)[0][0]
            entry["falloff"] = {"DryMaxCurve": curve, "area": sounds.curve_area(curves[curve]),
                                "WetMaxCurve": wet.most_common(1)[0][0] if wet else curve,
                                "falloff_curves": sorted(n for n, p in curves.items() if sounds.is_falloff(p))}
        classes[key] = entry
    return classes


# What a stock alias's own rows say about where and how it plays: everything
# but the audio (file, storage, pitch, looping) and its level, which stay
# WaW's, and the alias-triggered duck.
TWIN_COLUMNS = ("Bus", "VolumeGroup", "DuckGroup", "ReverbSend", "CenterSend", "DistMin", "DistMaxDry", "DistMaxWet",
                "DryMinCurve", "DryMaxCurve", "WetMinCurve", "WetMaxCurve", "LimitCount", "EntityLimitCount",
                "LimitType", "EntityLimitType", "PriorityMin", "PriorityMax", "PriorityThresholdMin",
                "PriorityThresholdMax", "PanType", "Pan", "OcclusionLevel", "DistanceLpf", "StopOnEntDeath")


def stock_twins(bo2_root: Path, names) -> dict[str, dict]:
    """Stock alias -> its most common TWIN_COLUMNS row over BO2's zombie maps,
    for the stock aliases a converted sound plays in place of (the engine's
    footsteps and impacts, the zombie vocal events)."""
    wanted = {name.lower() for name in names or ()}
    found: dict[str, Counter] = {}
    for row in _stock_rows(bo2_root) if wanted else ():
        name = (row.get("Name") or "").lower()
        if name in wanted:
            found.setdefault(name, Counter())[tuple(row.get(c, "") for c in TWIN_COLUMNS)] += 1
    return {name: dict(zip(TWIN_COLUMNS, mixes.most_common(1)[0][0])) for name, mixes in found.items()}


def stock_alias_groups(bo2_root: Path) -> dict[str, str]:
    """Stock alias -> its most common volume group (sounds.learn_bus_groups)."""
    found: dict[str, Counter] = {}
    for row in _stock_rows(bo2_root):
        found.setdefault((row.get("Name") or "").lower(), Counter())[row.get("VolumeGroup", "")] += 1
    return {name: groups.most_common(1)[0][0] for name, groups in found.items()}


def reference_quantiles(reference: dict[str, dict[str, list[float]]]) -> dict[str, dict[str, dict]]:
    """Percentile tables of sounds.waw_mix_reference, for remaster."""
    return {key: {column: {"q": percentiles(values), "samples": len(values)}
                  for column, values in columns.items() if len(values) >= REFERENCE_MIN_SAMPLES}
            for key, columns in reference.items()}


# A stinger plays to every speaker, as BO2's own (Pan music_all).
STINGER_COLUMNS = MIX_COLUMNS + ("Pan",)


def _stinger_profile(bo2_root: Path, name: str) -> dict | None:
    """Mix and level of one stock BO2 music stinger: its most common mix over
    the zombie maps and the loudest map's level. BO2 ships its round stingers
    at 89 (TranZit era) to 96 dB (Mob of the Dead, Buried); at the median
    WaW's round change music played too low under the game (user, 2026-11-02)."""
    found: dict[str, list[dict]] = {}
    for path in sorted((bo2_root / "raw/soundbank").glob("zmb_*.aliases.csv")):
        with path.open(encoding="utf-8", errors="replace", newline="") as f:
            found[path.name] = [r for r in csv.DictReader(f) if (r.get("Name") or "").lower() == name]
    rows = [r for group in found.values() for r in group]  # per map: the level is the loudest map's
    if not rows:
        return None
    mix = Counter(tuple(r.get(c, "") for c in STINGER_COLUMNS) for r in rows).most_common(1)[0][0]
    levels = [_median(v) for group in found.values()
              for v in [[x for r in group for x in [_number(r.get("VolMax"))] if x is not None]] if v]
    return {"mix": dict(zip(STINGER_COLUMNS, mix)), "volume": max(levels) if levels else None,
            "rows": len(rows)}


_STATE_CALL = re.compile(r'\b(declareMusicState|musicAliasloop|musicAlias)\s*\(\s*"([^"]+)"', re.IGNORECASE)


def music_stingers(amb_source: str | None, states: dict[str, str]) -> dict[str, str]:
    """Map alias -> stock BO2 stinger for the one-shot musicAlias of each music
    state in ``states`` (round_begin's chalk, end_of_game's game over)."""
    out, state = {}, None
    for m in _STATE_CALL.finditer(amb_source or ""):
        call, value = m.group(1).lower(), m.group(2)
        if call == "declaremusicstate":
            state = value
        elif call == "musicalias" and state in states:
            out.setdefault(value.lower(), states[state])
    return out


# What places a 3D sound in the level: BO2's values replace WaW's for ambience.
EMITTER_DISTANCE_COLUMNS = ("DistMin", "DistMaxDry", "DistMaxWet")
EMITTER_CURVE_COLUMNS = ("DryMinCurve", "DryMaxCurve", "WetMinCurve", "WetMaxCurve")


def stock_emitters(ambience_rows: list[dict]) -> dict[str, dict]:
    """Looping value -> BO2's 3D ambient emitter placement: median distances
    and most common curves of the stock amb_ rows. Measured (2026-10-25): loops
    full to 100 units and silent by 400, one-shots 75 / 550; WaW's Shi No Numa
    cicada drone was full to 3000 and audible to 8500 (buzzed over the map)."""
    emitters = {}
    for looping in ("looping", "nonlooping"):
        found = [r for r in ambience_rows if r.get("PanType") == "3d" and r.get("Looping") == looping]
        if not found:
            continue
        values = {c: _median([v for r in found for v in [_number(r.get(c))] if v is not None])
                  for c in EMITTER_DISTANCE_COLUMNS}
        values.update({c: Counter(r.get(c, "") for r in found).most_common(1)[0][0] for c in EMITTER_CURVE_COLUMNS})
        emitters[looping] = {"columns": {k: f"{v:g}" if isinstance(v, float) else v for k, v in values.items()},
                             "rows": len(found)}
    return emitters


# A group needs this many stock rows to give a level (BO2's 2D voice group
# holds 53 rows, all silent placeholders at 0).
MIN_GROUP_ROWS = 100


def stock_group_levels(bo2_root: Path) -> dict[str, dict]:
    """"<VolumeGroup>|<PanType>" -> median stock VolMax over BO2's zombie maps.

    Every other WaW sound keeps its converted category (sounds._category); its
    level should sit where BO2 mixes that category. Measured on the stock
    banks: converted WaW voice 3D sat at 100 against BO2's 82, foley 3D at 90
    against 69, so BO2's own sounds in the same map (zombie vocals, footsteps)
    played some 18 dB under WaW's."""
    values: dict[str, list[float]] = {}
    for row in _stock_rows(bo2_root):
        volume = _number(row.get("VolMax"))
        if volume is not None:
            values.setdefault(f"{row.get('VolumeGroup', '')}|{row.get('PanType', '')}", []).append(volume)
    return {key: {"volume": _median(found), "rows": len(found)} for key, found in values.items()
            if len(found) >= MIN_GROUP_ROWS and _median(found) > 0}


_MUSIC_CALL = re.compile(r'\bmusicAlias(?:loop)?\s*\(\s*"([^"]+)"', re.IGNORECASE)


def music_table_aliases(amb_source: str | None) -> set[str]:
    """Aliases the map's music state table plays (musicAlias / musicAliasloop)."""
    return {m.group(1).lower() for m in _MUSIC_CALL.finditer(amb_source or "")}


def role(bus: str | None, spatialized: bool, name: str, music: set[str], ambience: set[str],
         stingers: dict[str, str] | None = None) -> str | None:
    """The role WaW's own data gives an alias (None keeps its converted mix)."""
    short = name.lower().removeprefix("waw/")
    if stingers and short in stingers:
        return "stinger:" + stingers[short]
    if bus == "music" or short in music:
        return "music"
    if bus == "ambience" or short in ambience:
        return "ambience"
    if bus == "voice" and not spatialized:
        return "announcer"
    return None


def announcer_copies(rows: list[dict], roles: dict[str, str], events: dict[str, list[str]], prefix: str) -> dict:
    """Copies of WaW announcer lines under the names BO2's announcer plays
    (vox_zmba_<prefix><alias>), for the lines the map's power-up script says
    (gscport.Translator.announcer_hook). Only lines WaW authored as
    announcements (announcer role) get one."""
    wanted = {f"waw/{alias}": f"vox_zmba_{prefix}{re.sub(r'[^a-z0-9_]', '_', alias)}"
              for aliases in events.values() for alias in aliases}
    copies, made = [], {}
    for row in rows:
        name = row["Name"].lower()
        if name in wanted and roles.get(name) == "announcer":
            copy = dict(row, Name=wanted[name])
            copies.append(copy)
            made[name] = wanted[name]
            roles[wanted[name]] = "announcer"
    rows += copies
    return made


def _shift(rows: list[dict], shift: float) -> None:
    for row in rows:
        for column in ("VolMin", "VolMax"):
            value = _number(row.get(column))
            if value is not None:
                row[column] = f"{max(0.0, min(100.0, value + shift)):g}"


def _source_quantiles(key: str, column: str, members: list[dict], reference: dict | None):
    """(percentiles, where from) WaW's values of a class are read against:
    stock WaW's, else this map's own rows when there are enough."""
    stock = (reference or {}).get(key, {}).get(column)
    if stock is not None:
        return stock["q"], "stock_waw"
    if column == "Falloff":
        values = [row["_waw"]["falloff"] for row in members if _places(row)]
    elif column == "DistMin":
        values = [v for row in members if _places(row) for v in [_number(row.get(column))] if v is not None]
    else:
        values = [v for row in members for v in [_number(row.get(column))] if v is not None]
    return (percentiles(values), "map") if len(values) >= REFERENCE_MIN_SAMPLES else (None, None)


def _places(row: dict) -> bool:
    """A 3D row with WaW's ordinary falloff: the kind a class places."""
    return row.get("PanType") == "3d" and bool(row.get("_waw", {}).get("standard"))


def _mix_class(key: str, members: list[dict], stock: dict, reference: dict | None, reach: str = "waw") -> dict:
    """Route one class as stock BO2 does and move its priority and occlusion
    to BO2's scale (transfer); returns what changed.

    ``reach``: "waw" leaves a row's distances and falloff as WaW authored
    them (on the stock curve nearest its own, sounds.bind_curves): how far a
    sound is heard is the map's design, and a sound BO2 plays itself takes
    BO2's placement by name instead (twins). "stock" moves reach by rank onto
    BO2's values and falloff. The stock tables hold hundreds of short
    per-surface rows per class, so by rank a scripted event sound (board
    break, mystery box, teleporter) lost two thirds of its reach and nothing
    was heard a room away (user, 2026-11-09); only ambient emitters, which
    droned over the map at WaW's reach, keep it."""
    out = {"routing": stock["routing"], "transferred": {}}
    for row in members:
        row.update(stock["routing"])
    targets, falloff = stock["quantiles"], stock.get("falloff")
    ordered = [row for row in members
               if (_number(row.get("PriorityMin")) or 0.0) <= (_number(row.get("PriorityMax")) or 0.0)]
    for column in ("PriorityMin", "PriorityMax", "OcclusionLevel"):
        source, origin = _source_quantiles(key, column, members, reference)
        if source is None or column not in targets:
            continue
        before = _median([v for row in members for v in [_number(row.get(column))] if v is not None] or [0])
        for row in members:
            value = _number(row.get(column))
            if value is not None:
                moved = transfer(value, source, targets[column])
                row[column] = f"{moved:.3g}" if column == "OcclusionLevel" else str(round(moved))
        after = _median([v for row in members for v in [_number(row.get(column))] if v is not None] or [0])
        out["transferred"][column] = {"reference": origin, "median": [before, after]}
    for row in ordered:
        # the two ends move on their own scales: keep WaW's min <= max
        if (_number(row.get("PriorityMin")) or 0.0) > (_number(row.get("PriorityMax")) or 0.0):
            row["PriorityMin"] = row["PriorityMax"]
    near, near_origin = _source_quantiles(key, "DistMin", members, reference)
    far, far_origin = _source_quantiles(key, "Falloff", members, reference)
    placed = [row for row in members if _places(row)]
    if (reach == "stock" and falloff and placed and near is not None and far is not None
            and {"DistMin", "Falloff"} <= targets.keys()):
        before = _median([_number(row["DistMaxDry"]) or 0.0 for row in placed])
        for row in placed:
            start = transfer(_number(row["DistMin"]) or 0.0, near, targets["DistMin"])
            end = start + transfer(row["_waw"]["falloff"], far, targets["Falloff"]) / falloff["area"]
            row.update(DistMin=str(round(start)), DistMaxDry=str(round(end)),
                       DistMaxWet=str(round(end * row["_waw"].get("wet_ratio", 1.0))),
                       DryMaxCurve=falloff["DryMaxCurve"])
            if row.get("WetMaxCurve") in falloff["falloff_curves"]:
                row["WetMaxCurve"] = falloff["WetMaxCurve"]
        out["falloff"] = {"rows": len(placed), "kept_authored_shape": sum(
                              1 for row in members if row.get("PanType") == "3d") - len(placed),
                          "curve": falloff["DryMaxCurve"], "reach": reach, "reference": [near_origin, far_origin],
                          "median_dist_max": [before, _median([_number(row["DistMaxDry"]) or 0.0 for row in placed])]}
    return out


def remaster(rows: list[dict], roles: dict[str, str], profiles: dict[str, dict], reference: dict | None = None,
             twins: dict[str, str] | None = None) -> dict:
    """Apply each role's stock mix to its rows in place; returns the report.

    ``reference``: reference_quantiles of stock WaW (what a map's values are
    read against; the map's own rows where it is missing). ``twins``: row
    name -> the stock BO2 alias it plays in place of (profiles["twins"])."""
    report = {"roles": {}, "unmeasured": [], "groups": {}}
    by_role: dict[str, list[dict]] = {}
    by_group: dict[str, list[dict]] = {}
    for row in rows:
        r = roles.get(row["Name"].lower())
        if r is not None:
            by_role.setdefault(r, []).append(row)
        else:
            by_group.setdefault(f"{row.get('VolumeGroup', '')}|{row.get('PanType', '')}", []).append(row)
    for key, members in sorted(by_group.items()):
        stock = profiles.get("groups", {}).get(key)
        waw_volumes = [v for row in members for v in [_number(row.get("VolMax"))] if v is not None]
        if stock is None or not waw_volumes:
            report["unmeasured"].append(key)
            continue
        # WaW's typical level of the class: stock WaW's where measured, so a
        # map holding only its loud members (reloads without footsteps) is
        # not pulled down to the class's median
        level = (reference or {}).get(key, {}).get("VolMax")
        shift = stock["volume"] - (level["q"][50] if level else _median(waw_volumes))
        _shift(members, shift)
        report["groups"][key] = {"aliases": len({row["Name"] for row in members}), "rows": len(members),
                                 "volume_shift": round(shift, 2), "stock_rows": stock["rows"],
                                 "level_reference": "stock_waw" if level else "map"}
        if key in profiles.get("classes", {}):
            report["groups"][key].update(_mix_class(key, members, profiles["classes"][key], reference))
    for r, members in sorted(by_role.items()):
        profile = profiles.get(r)
        if profile is None:
            report["unmeasured"].append(r)
            continue
        waw_volumes = [v for row in members for v in [_number(row.get("VolMax"))] if v is not None]
        shift = (profile["volume"] - _median(waw_volumes)) if profile["volume"] is not None and waw_volumes else 0.0
        # Ambience keeps WaW's authored panning: a 2D bed (amb_spooky_2d) is
        # not a point emitter. Music and announcer lines are 2D by role.
        mix = {k: v for k, v in profile["mix"].items() if not (r == "ambience" and k == "PanType")}
        placed = 0
        for row in members:
            row.update(mix)
        # BO2 is king for the mix: a 3D ambient sound carries as far as BO2's
        # own emitters, not as far as WaW authored it. By rank where the
        # class is measured: the fire that carried farthest in WaW still
        # does, on BO2's scale. One reach for every emitter (BO2's median,
        # 400 units for loops) left the map empty between its emitters
        # (user, 2026-11-09).
        ambient = profiles.get("classes", {}).get("grp_ambience|3d") if r == "ambience" else None
        spread = None
        if ambient is not None and ambient.get("falloff"):
            spatial = [row for row in members if row.get("PanType") == "3d"]
            spread = _mix_class("grp_ambience|3d", spatial, {**ambient, "routing": {}}, reference,
                                reach="stock").get("falloff")
        for row in members if r == "ambience" and spread is None else ():
            emitter = profile.get("emitters", {}).get(row.get("Looping", ""))
            if emitter is not None and row.get("PanType") == "3d":
                row.update(emitter["columns"])
                placed += 1
        _shift(members, shift)
        report["roles"][r] = {"aliases": len({row["Name"] for row in members}), "rows": len(members),
                              "stock_mix": profile["mix"], "volume_shift": round(shift, 2),
                              "stock_rows": profile["rows"]}
        if placed:
            report["roles"][r]["stock_emitter_rows"] = placed
        if spread:
            report["roles"][r]["falloff"] = spread
    if twins:
        stock, placed, missing = profiles.get("twins", {}), {}, set()
        for row in rows:
            twin = twins.get(row["Name"].lower())
            if twin is None:
                continue
            if twin in stock:
                row.update(stock[twin])
                placed[row["Name"]] = twin
            else:
                missing.add(twin)
        report["twins"] = {"placed": placed, "not_in_stock": sorted(missing)}
    return report
