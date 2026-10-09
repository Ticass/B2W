"""Remaster: draw a converted map's models with their Black Ops II materials.

Opt-in (stage-bridge --remaster-bo2-materials, launcher "Remaster with BO2
materials"). Community "remastered" WaW maps are built from Black Ops II props
ported back to WaW: their textures keep the BO2 pixels (Nuketown Remastered:
259 of 259 matched images have the BO2 resolution), but the WaW material only
keeps what WaW shaders can draw (48 of 239 matched materials lost maps such as
specular/gloss). With this mode a WaW model material whose name is a BO2
material's name, behind the remaster prefixes (``mc/bo2_mtl_x`` -> BO2
``mc/mtl_x``), is replaced by that native BO2 material: its technique set and
every image, extracted from the BO2 zones that define them.

Scope (measured on Nuketown Remastered):
- model materials only (class ``mc/``, 249 of 386 match a BO2 ``mc/``
  material). World (``wc/``) materials keep the translated WaW passes: native
  T6 world programs read the WaW-encoded lightmap pages wrongly (white
  surfaces, AGENT_HANDOFF session 30b). The BO2 class must match too: the
  33 ``mlv/`` (vertex-lit static props) BO2 counterparts are not used;
- every replacement is reported (REMASTER); a material whose BO2 images or
  technique set cannot be extracted keeps its WaW translation.

GUIDELINES 2a (WaW first) applies whenever this option is off.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

from . import techsets

SCHEMA = 2
MODEL_CLASS = "mc"
# prefixes remaster mappers put in front of the BO2 asset name
SOURCE_PREFIX = re.compile(r"^(bo2|bo1|t6|t5)_")
INDEXED = ("material", "techniqueset", "image")
# Image packs the game keeps loaded for every map (DLC content). OAT opens
# only base/mp/so|zm|sp and the packs a zone names (ipak_read); a DLC map
# names its own pack (mp_downhill.ipak), which is not installed: its pixels
# are in dlc1.ipak (measured). The extraction offers these under the names
# OAT could not open.
GLOBAL_PACKS = re.compile(r"^dlc(zm)?\d+$")
_TRIED = re.compile(r"Trying to load ipak '([^']+)' for zone")
_FOUND = re.compile(r"(?:Found and loaded ipak '([^']+)\.ipak'|Referencing loaded ipak '([^']+)')")
_NO_DATA = re.compile(r'Could not find data for image "([^"]+)"')


@dataclass(frozen=True)
class Match:
    waw: str     # WaW material name
    bo2: str     # BO2 material name
    zone: Path   # BO2 fastfile that defines it


def split_name(name: str) -> tuple[str, str]:
    """(class, rest) of a material name: ``mc/mtl_x`` -> (``mc``, ``mtl_x``)."""
    cls, _, rest = name.lower().rpartition("/")
    return cls, rest


def bo2_candidates(waw: str) -> list[str]:
    """BO2 names a WaW material can stand for: its own name, then the name
    behind the remaster prefix (``mc/bo2_mtl_x`` -> ``mc/mtl_x``)."""
    cls, rest = split_name(waw)
    stripped = SOURCE_PREFIX.sub("", rest)
    return [f"{cls}/{rest}"] + ([f"{cls}/{stripped}"] if stripped != rest else [])


def _stamp(path: Path) -> list[int]:
    stat = path.stat()
    return [stat.st_size, stat.st_mtime_ns]


def _zones(bo2_root: Path) -> list[Path]:
    # Zombies zones first: an asset several zones define comes from the
    # Zombies one (same order every run, so the choice is stable).
    return sorted((bo2_root / "zone" / "all").glob("*.ff"),
                  key=lambda p: (not p.stem.startswith(("zm_", "common_zm")), p.stem.lower()))


def _run(command: list[str], log: Path) -> None:
    with log.open("w", encoding="utf-8", errors="replace") as out:
        code = subprocess.run(command, stdout=out, stderr=subprocess.STDOUT,
                              creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)).returncode
    if code:
        raise RuntimeError(f"{command[0]} exited {code}; see {log}")


def asset_index(bo2_root: Path, tool: Path, cache: Path) -> dict[str, dict[str, tuple[str, str]]]:
    """kind -> {name (lower): (name, defining fastfile)} for materials,
    technique sets and images over every zone/all fastfile. Listing is cheap
    (about 22 s for 107 zones); cached until a fastfile or the tool changes."""
    from .all2raw import listing
    zones = _zones(bo2_root)
    inputs = {"schema": SCHEMA, "tool": _stamp(tool), "zones": {str(z): _stamp(z) for z in zones}}
    path = cache / "asset_index.json"
    try:
        cached = json.loads(path.read_text(encoding="utf-8"))
        if cached["inputs"] == inputs:
            return {kind: {k: tuple(v) for k, v in names.items()} for kind, names in cached["assets"].items()}
    except (OSError, ValueError, KeyError):
        pass
    cache.mkdir(parents=True, exist_ok=True)
    search = str(bo2_root / "zone" / "all")
    assets: dict[str, dict[str, tuple[str, str]]] = {kind: {} for kind in INDEXED}
    for zone in zones:
        log = cache / f"{zone.stem}.assets.log"
        _run([str(tool), "--no-color", "--list", "--search-path", search, str(zone)], log)
        listed = listing(log.read_text(encoding="utf-8", errors="replace"))
        for kind in INDEXED:
            for name in listed.get(kind, []):
                assets[kind].setdefault(name.lower(), (name, str(zone)))
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps({"inputs": inputs, "assets": assets}), encoding="utf-8")
    temporary.replace(path)
    return assets


def find_matches(names, materials: dict[str, tuple[str, str]]) -> list[Match]:
    """WaW model materials with a same-class BO2 material of the same name
    (after the remaster prefix)."""
    matches = []
    for waw in sorted(names):
        if split_name(waw)[0] != MODEL_CLASS:
            continue
        hit = next((materials[c] for c in bo2_candidates(waw) if c in materials), None)
        if hit is not None:
            matches.append(Match(waw, hit[0], Path(hit[1])))
    return matches


class _Extractor:
    """Zone dumps for one remaster run (scratch folders under ``work``)."""

    def __init__(self, bo2_root: Path, tool: Path, work: Path, log):
        self.bo2_root, self.tool, self.work, self.log = bo2_root, tool, work, log
        self.zone_dir = bo2_root / "zone" / "all"
        self.dumps: dict[tuple[str, str], Path] = {}
        self.logs: dict[Path, str] = {}
        self.unrecoverable: set[tuple[Path, str]] = set()
        self.packs = sorted(p for p in self.zone_dir.glob("*.ipak") if GLOBAL_PACKS.match(p.stem))

    def _unlink(self, zone: Path, assets: str, out: Path, aliases: dict[str, Path] | None = None) -> str:
        search = [str(self.zone_dir)]
        out.parent.mkdir(parents=True, exist_ok=True)
        if aliases:
            links = out.with_name(out.name + ".packs")
            shutil.rmtree(links, ignore_errors=True)
            links.mkdir(parents=True)
            for name, pack in aliases.items():
                os.link(pack, links / f"{name}.ipak")   # same volume as the game: no copy
            search.insert(0, str(links))
        log = out.with_name(out.name + ".log")
        try:
            _run([str(self.tool), "-v", "--no-color", "--search-path", ";".join(search), "--image-format", "IWI",
                  "--include-assets", assets, "--output-folder", str(out), str(zone)], log)
        finally:
            if aliases:
                shutil.rmtree(out.with_name(out.name + ".packs"), ignore_errors=True)
        return log.read_text(encoding="utf-8", errors="replace")

    def dump(self, zone: Path, assets: str) -> Path:
        key = (str(zone), assets)
        if key not in self.dumps:
            # short names: shader file names are long and Windows paths end at 260
            out = self.work / f"{zone.stem}.{len(self.dumps)}"
            shutil.rmtree(out, ignore_errors=True)
            self.log(f"[staging] Remaster: reading {assets.replace(',', ', ')} from {zone.name}")
            self.logs[out] = self._unlink(zone, assets, out)
            self.dumps[key] = out
        return self.dumps[key]

    def recover_images(self, zone: Path, out: Path, names: set[str]) -> None:
        """``names``: images of ``out`` (a dump of ``zone``) a remastered
        material needs whose pixels OAT did not find. Their pixels are in a
        DLC pack OAT did not open: extract the zone again with the global packs
        offered under the pack names it could not open, a few at a time."""
        text = self.logs.get(out, "")
        missing = {n for n in names if n in set(_NO_DATA.findall(text)) and (zone, n) not in self.unrecoverable}
        loaded = {a or b for a, b in _FOUND.findall(text)}
        slots = [n for n in dict.fromkeys(_TRIED.findall(text)) if n not in loaded and not n.endswith("_base")]
        packs = [p for p in self.packs if p.stem not in loaded]
        if not missing or not slots:
            return
        for start in range(0, len(packs), len(slots)):
            group = dict(zip(slots, packs[start:start + len(slots)]))
            retry = out.with_name(out.name + ".retry")
            shutil.rmtree(retry, ignore_errors=True)
            self.log(f"[staging] Remaster: {len(missing)} image(s) of {zone.name} in DLC packs: "
                     f"trying {', '.join(p.name for p in group.values())}")
            try:
                self._unlink(zone, "image", retry, group)
            except (OSError, RuntimeError):
                shutil.rmtree(retry, ignore_errors=True)
                return
            for name in sorted(missing):
                rel = techsets.oat_image_path(name)
                if (retry / rel).is_file():
                    (out / rel).parent.mkdir(parents=True, exist_ok=True)
                    shutil.move(str(retry / rel), str(out / rel))
                    missing.discard(name)
            shutil.rmtree(retry, ignore_errors=True)
            if not missing:
                return
        self.unrecoverable.update((zone, n) for n in missing)


def _gather(material: str, home: Path, zone_of_home: Path, ex: _Extractor, index,
            views: list[Path]) -> dict[str, Path] | str:
    """relative file -> source file for one native material: the material,
    its images, its technique set and shaders, from the dump of its own zone,
    the donor views, or the zone that defines a referenced asset. None if any
    is unavailable (then the reason)."""
    from .t6bridge import _techset_shaders
    rel = techsets.oat_material_path(material)
    if not (home / rel).is_file():
        return f"material {material} not in the {zone_of_home.name} dump"
    files = {rel.as_posix(): home / rel}
    data = json.loads((home / rel).read_text(encoding="utf-8"))

    def locate(kind: str, name: str, rel: Path) -> Path | None:
        for root in (home, *views):
            if (root / rel).is_file():
                return root / rel
        owner = index[kind].get(name.lower())
        zone, dump = (zone_of_home, home)
        if owner is not None and Path(owner[1]) != zone_of_home:
            zone = Path(owner[1])
            dump = ex.dump(zone, "techniqueset" if kind == "techniqueset" else "image")
        if kind == "image" and not (dump / rel).is_file():
            ex.recover_images(zone, dump, {name})
        return dump / rel if (dump / rel).is_file() else None

    for texture in data.get("textures", []):
        image = texture.get("image", "").removeprefix(",")
        if not image or image.startswith("$"):
            continue
        rel = techsets.oat_image_path(image)
        source = locate("image", image, rel)
        if source is None:
            return f"image {image} has no pixels in the installed packs"
        files[rel.as_posix()] = source
    ts = data.get("techniqueSet")
    if not ts:
        return "no technique set"
    rel = Path("techniquesets") / f"{ts}.json"
    source = locate("techniqueset", ts, rel)
    if source is None:
        return f"technique set {ts} not found"
    files[rel.as_posix()] = source
    for shader in sorted(_techset_shaders(json.loads(source.read_text(encoding="utf-8")))):
        if not (source.parent.parent / shader).is_file():
            long = len(str(source.parent.parent / shader)) >= 250
            return f"shader {shader} of {ts} not found" + (" (path too long for Windows)" if long else "")
        files[shader] = source.parent.parent / shader
    return files


def extract(matches: list[Match], bo2_root: Path, tool: Path, cache: Path, views: list[Path] = (),
            log=print) -> tuple[dict[str, Path], list[str]]:
    """Extract the matched BO2 materials with their images, technique sets
    and shaders into one cached folder per material set, keeping only those
    files. ``views`` are folders that already hold technique sets/shaders
    (the stock donor view). Returns ({WaW material: native material json},
    problems)."""
    if not matches:
        return {}, []
    index = asset_index(bo2_root, tool, cache)
    wanted = sorted({(m.bo2, str(m.zone)) for m in matches})
    key = hashlib.sha256(json.dumps([SCHEMA, wanted, _stamp(tool),
                                     sorted({z: _stamp(Path(z)) for _, z in wanted}.items())])
                         .encode()).hexdigest()[:12]
    folder = cache / "sets" / key
    receipt = folder / "remaster.json"
    kept: list[str] | None = None
    reasons: dict[str, str] = {}
    try:
        data = json.loads(receipt.read_text(encoding="utf-8"))
        kept, reasons = data["materials"], data.get("reasons", {})
    except (OSError, ValueError, KeyError):
        pass
    if kept is None:
        work = cache / "w" / key
        shutil.rmtree(work, ignore_errors=True)
        ex = _Extractor(bo2_root, tool, work, log)
        partial = folder.with_name(key + ".partial")
        shutil.rmtree(partial, ignore_errors=True)
        partial.mkdir(parents=True)
        kept = []
        reasons: dict[str, str] = {}
        needed: dict[Path, set[str]] = {}
        for material, zone in wanted:
            home = ex.dump(Path(zone), "material,techniqueset,image")
            rel = techsets.oat_material_path(material)
            if (home / rel).is_file():
                for texture in json.loads((home / rel).read_text(encoding="utf-8")).get("textures", []):
                    image = texture.get("image", "").removeprefix(",")
                    if image and not image.startswith("$"):
                        needed.setdefault(Path(zone), set()).add(image)
        # one batch of DLC-pack retries per zone, for every image it must supply
        for zone, images in sorted(needed.items()):
            ex.recover_images(zone, ex.dump(zone, "material,techniqueset,image"), images)
        for material, zone in wanted:
            home = ex.dump(Path(zone), "material,techniqueset,image")
            files = _gather(material, home, Path(zone), ex, index, list(views))
            if isinstance(files, str):
                reasons[material] = files
                continue
            for rel, source in files.items():
                if not (partial / rel).is_file():
                    (partial / rel).parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(source, partial / rel)
            kept.append(material)
        (partial / "remaster.json").write_text(json.dumps({"materials": kept, "reasons": reasons}),
                                               encoding="utf-8")
        shutil.rmtree(folder, ignore_errors=True)
        partial.rename(folder)
        shutil.rmtree(work, ignore_errors=True)
    found: dict[str, Path] = {}
    problems: list[str] = []
    for m in matches:
        if m.bo2 in kept:
            found[m.waw] = folder / techsets.oat_material_path(m.bo2)
        else:
            why = reasons.get(m.bo2, "not extractable from the installed game")
            problems.append(f"{m.waw} -> {m.bo2} ({m.zone.name}): {why}; WaW material kept")
    return found, problems
