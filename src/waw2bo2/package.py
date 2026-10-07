"""Assemble the Plutonium T6 mod folder for the converted map.

Layout matches what the BO2 mod tools launcher's "Copy -> Pluto mods" does:
``%LOCALAPPDATA%\\Plutonium\\storage\\t6\\mods\\<map>\\`` holding the map
fastfile + ipak, mod.ff (gameplay), mod_load.ff (frontend) and sound banks.
Launch into the Zombies lobby with::

    plutonium-bootstrapper-win32.exe t6zm "<BO2>" +set fs_game mods/<map>
"""
from __future__ import annotations

import json
import hashlib
import os
import shutil
from pathlib import Path
from . import __version__


def plutonium_mods_dir() -> Path:
    if os.name != 'nt':
        from .linuxruntime import configure_localappdata
        configure_localappdata()
    return Path(os.environ["LOCALAPPDATA"]) / "Plutonium" / "storage" / "t6" / "mods"


def package(project: str, map_out: Path, mod_out: Path, dest: Path | None = None) -> Path:
    dest = dest or plutonium_mods_dir() / project
    files = [map_out / f"{project}.ff", map_out / f"{project}.ipak", mod_out / "mod.ff",
             mod_out / "mod_load.ff"]
    files += sorted((mod_out / "sound").glob("*.sab?")) if (mod_out / "sound").exists() else []
    # mod.ff's own image pack (converted weapon textures), when it has one
    files += sorted(mod_out.glob("*.ipak"))
    missing = [f for f in files if not f.exists()]
    if missing:
        raise FileNotFoundError(f"cannot package, missing: {missing}")
    dest.mkdir(parents=True, exist_ok=True)
    for f in files:
        target = dest / f.name
        # A running map keeps its image packs open. Identical files need no
        # replacement, and skipping them also avoids copying large packs.
        if target.is_file() and target.stat().st_size == f.stat().st_size:
            with f.open('rb') as source_stream, target.open('rb') as target_stream:
                if hashlib.file_digest(source_stream, 'sha256').digest() == hashlib.file_digest(target_stream, 'sha256').digest():
                    continue
        shutil.copy2(f, target)
    metadata_path = mod_out / 'menu_metadata.json'
    metadata = json.loads(metadata_path.read_text(encoding='utf-8')) if metadata_path.is_file() else {}
    (dest / "mod.json").write_text(json.dumps({
        "name": metadata.get('title', f"{project} (WaW -> BO2)"),
        "description": metadata.get('description', f"{project}: World at War custom map converted by waw2bo2"),
        "version": __version__,
    }, indent=2) + "\n", encoding="utf-8")
    return dest


def launch_command(project: str, bo2: Path, *, direct_map: bool = False) -> list[str]:
    boot = Path(os.environ["LOCALAPPDATA"]) / "Plutonium" / "bin" / "plutonium-bootstrapper-win32.exe"
    command = [str(boot), "t6zm", str(bo2), "+set", "fs_game", f"mods/{project}"]
    if direct_map:
        command += ["-lan", "+devmap", project]
    return command
