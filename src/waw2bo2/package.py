"""Assemble the Plutonium T6 mod folder for the converted map.

Layout matches what the BO2 mod tools launcher's "Copy -> Pluto mods" does:
``%LOCALAPPDATA%\\Plutonium\\storage\\t6\\mods\\<map>\\`` holding the map
fastfile + ipak, mod.ff (gameplay assets) and its sound banks. Launch with::

    plutonium-bootstrapper-win32.exe t6zm "<BO2>" -lan +set fs_game mods/<map> +devmap <map>
"""
from __future__ import annotations

import json
import os
import shutil
from pathlib import Path


def plutonium_mods_dir() -> Path:
    return Path(os.environ["LOCALAPPDATA"]) / "Plutonium" / "storage" / "t6" / "mods"


def package(project: str, map_out: Path, mod_out: Path, dest: Path | None = None) -> Path:
    dest = dest or plutonium_mods_dir() / project
    files = [map_out / f"{project}.ff", map_out / f"{project}.ipak", mod_out / "mod.ff"]
    files += sorted((mod_out / "sound").glob("*.sab?")) if (mod_out / "sound").exists() else []
    # mod.ff's own image pack (converted weapon textures), when it has one
    files += sorted(mod_out.glob("*.ipak"))
    missing = [f for f in files if not f.exists()]
    if missing:
        raise FileNotFoundError(f"cannot package, missing: {missing}")
    dest.mkdir(parents=True, exist_ok=True)
    for f in files:
        shutil.copy2(f, dest / f.name)
    (dest / "mod.json").write_text(json.dumps({
        "name": "Nuketown Remastered (WaW -> BO2)",
        "description": f"{project}: World at War custom map converted by waw2bo2",
        "version": "0.1.0",
    }, indent=2) + "\n", encoding="utf-8")
    return dest


def launch_command(project: str, bo2: Path) -> list[str]:
    boot = Path(os.environ["LOCALAPPDATA"]) / "Plutonium" / "bin" / "plutonium-bootstrapper-win32.exe"
    return [str(boot), "t6zm", str(bo2), "-lan", "+set", "fs_game", f"mods/{project}", "+devmap", project]
