"""Perk names the map uses that T6's engine does not know.

T6 SetPerk/HasPerk/UnsetPerk raise "Unknown perk" for a name outside the
engine's perk table. Maps use such names: WaW-only perks (specialty_boost for
Electric Cherry, specialty_shades for Who's Who) and their own markers
(School's specialty_marathon). Nothing here is a list: the engine's table is
read from the installed T6 executable (each perk name is a NUL-terminated
specialty_ string there, and only perk names are), the map's names from its
ported scripts. The difference is written to _waw2bo2_perk_table.gsc, which
compat's perk wrappers read: those perks live in a script-side table.
"""
from __future__ import annotations

import re
from pathlib import Path

SCRIPT = "_waw2bo2_perk_table.gsc"
# a NUL-terminated name not continuing another identifier: adjacent table
# strings share their separators, and specialty_grenadepulldeath follows data
PERK_STRING = re.compile(rb"(?<![A-Za-z0-9_])(specialty_[a-z0-9_]+)(?=\0)")
PERK_LITERAL = re.compile(r'"(specialty_[A-Za-z0-9_]+)"')


def t6_executable() -> Path | None:
    from .package import plutonium_mods_dir
    executable = plutonium_mods_dir().parents[2] / "games" / "t6zm.exe"
    return executable if executable.is_file() else None


def engine_perks(executable: Path) -> set[str]:
    return {m.group(1).decode() for m in PERK_STRING.finditer(executable.read_bytes())}


def map_perks(project_root: Path) -> set[str]:
    """Perk names the map's ported server scripts and its entities name (a
    machine's trigger names its perk in script_noteworthy). Names that are no
    perk (a shader, a notify) are harmless: they never reach a perk builtin."""
    texts = [p.read_text(encoding="utf-8", errors="replace") for p in sorted((project_root / "maps/mp/waw").rglob("*.gsc"))]
    entities = project_root / "BSP/entities.json"
    if entities.is_file():
        texts.append(entities.read_text(encoding="utf-8", errors="replace"))
    return {m.group(1).lower() for text in texts for m in PERK_LITERAL.finditer(text)}


def source(emulated: set[str]) -> str:
    lines = "".join(f'    level.waw2bo2_emulated_perks["{perk}"] = 1;\n' for perk in sorted(emulated))
    return ("// waw2bo2: the map's perk names T6's perk table lacks (perktable.py)\n"
            "init()\n{\n    level.waw2bo2_emulated_perks = [];\n" + lines + "}\n")


def stage(project_root: Path, executable: Path | None = None) -> dict:
    """Write the table for the ported scripts under project_root."""
    script_dir = project_root / "maps/mp/waw"
    executable = executable or t6_executable()
    if executable is None:
        (script_dir / SCRIPT).write_text(source(set()), encoding="utf-8")
        return {"error": "T6 executable (Plutonium games/t6zm.exe) not found: perk names T6 lacks are not detected"}
    known = engine_perks(executable)
    if not known:
        raise ValueError(f"{executable}: no perk table found")
    used = map_perks(project_root)
    emulated = used - known
    (script_dir / SCRIPT).write_text(source(emulated), encoding="utf-8")
    return {"engine_perks": len(known), "map_perks": len(used), "emulated": sorted(emulated)}
