"""Remaster WaW audio as a stock BO2 zombies map would mix it.

WaW's own data says what each sound is for: its bus (music, voice, ambience),
whether it is authored 2D, the map's music state table (musicAlias calls) and
its ambient package. Each such role takes the mix BO2's own zombie maps give
the same kind of sound, measured from the stock alias tables
(raw/soundbank/zmb_*.aliases.csv): bus, volume group, duck group, ducking,
panning, music flag and sends. Loudness moves so WaW's typical level for the
role lands on BO2's, keeping every sound's offset from it. WaW's distances and
curves stay: they place each emitter in the map.
"""
from __future__ import annotations

import csv
import re
from collections import Counter
from pathlib import Path

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


def stock_profiles(bo2_root: Path) -> dict[str, dict]:
    """Role -> {mix: most common stock MIX_COLUMNS values, volume: median VolMax
    of the rows with that mix, rows: how many stock rows it was measured on}."""
    rows: dict[str, list[dict]] = {role: [] for role in STOCK_FAMILY}
    for path in sorted((bo2_root / "raw/soundbank").glob("zmb_*.aliases.csv")):
        with path.open(encoding="utf-8", errors="replace", newline="") as f:
            for row in csv.DictReader(f):
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
    profiles["groups"] = stock_group_levels(bo2_root)
    return profiles


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
    for path in sorted((bo2_root / "raw/soundbank").glob("zmb_*.aliases.csv")):
        with path.open(encoding="utf-8", errors="replace", newline="") as f:
            for row in csv.DictReader(f):
                volume = _number(row.get("VolMax"))
                if volume is not None:
                    values.setdefault(f"{row.get('VolumeGroup', '')}|{row.get('PanType', '')}", []).append(volume)
    return {key: {"volume": _median(found), "rows": len(found)} for key, found in values.items()
            if len(found) >= MIN_GROUP_ROWS and _median(found) > 0}


_MUSIC_CALL = re.compile(r'\bmusicAlias(?:loop)?\s*\(\s*"([^"]+)"', re.IGNORECASE)


def music_table_aliases(amb_source: str | None) -> set[str]:
    """Aliases the map's music state table plays (musicAlias / musicAliasloop)."""
    return {m.group(1).lower() for m in _MUSIC_CALL.finditer(amb_source or "")}


def role(bus: str | None, spatialized: bool, name: str, music: set[str], ambience: set[str]) -> str | None:
    """The role WaW's own data gives an alias (None keeps its converted mix)."""
    short = name.lower().removeprefix("waw/")
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


def remaster(rows: list[dict], roles: dict[str, str], profiles: dict[str, dict]) -> dict:
    """Apply each role's stock mix to its rows in place; returns the report."""
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
        shift = stock["volume"] - _median(waw_volumes)
        _shift(members, shift)
        report["groups"][key] = {"aliases": len({row["Name"] for row in members}), "rows": len(members),
                                 "volume_shift": round(shift, 2), "stock_rows": stock["rows"]}
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
        for row in members:
            row.update(mix)
        _shift(members, shift)
        report["roles"][r] = {"aliases": len({row["Name"] for row in members}), "rows": len(members),
                              "stock_mix": profile["mix"], "volume_shift": round(shift, 2),
                              "stock_rows": profile["rows"]}
    return report
