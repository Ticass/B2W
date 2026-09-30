"""BO2 equivalents for WaW assets that would otherwise end up as nothing.

GUIDELINES 2a: WaW always has priority (map zones, stock WaW zones, Mod Tools
sources). Only an asset that whole lookup cannot supply may be replaced by a
BO2 asset, instead of leaving a hole. Two rules, in order:

1. same name: the BO2 mod tools' raw folder holds a BO2 asset of exactly the
   WaW name (mappers often ship BO2-ported content under its BO2 name);
2. compat/bo2_equivalents.json: a global, map-independent table of WaW asset
   names (stock or common community assets) and the BO2 asset with the same
   role.

Every use is reported as BO2_FALLBACK by the caller. The candidate must exist
in the BO2 raw folder (the mod linker loads it from there) and must not be an
asset of an always-loaded stock zone (a second copy in mod.ff would be an
asset override, which BO2 refuses at load).
"""
from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

TABLE = Path(__file__).parent / "compat" / "bo2_equivalents.json"
# asset kind -> (raw folder, file suffix) of the BO2 mod tools' raw layout
RAW_LAYOUT = {"xmodel": ("xmodel", ".json")}


@dataclass(frozen=True)
class Equivalent:
    name: str
    rule: str   # "same_name" | "table"
    dds_images: tuple[str, ...] = ()   # raw images only present as DDS; stage() converts them

    def describe(self, kind: str, waw: str, why: str) -> str:
        how = "same name in BO2" if self.rule == "same_name" else "compat/bo2_equivalents.json"
        return f"BO2_FALLBACK {kind} {waw} -> {self.name}: {why}; {how}"


class Bo2Equivalents:
    def __init__(self, bo2_root: Path | None, loaded: dict[str, set[str]] | None = None,
                 table: Path = TABLE):
        self.raw = bo2_root / "raw" if bo2_root is not None else None
        self.loaded = {k: {n.lower() for n in v} for k, v in (loaded or {}).items()}
        data = json.loads(table.read_text(encoding="utf-8")) if table.exists() else {}
        self.table = {k: {w.lower(): b for w, b in v.items()} for k, v in data.items() if not k.startswith("_")}

    def _available(self, kind: str, name: str) -> tuple[str, ...] | None:
        """None when not buildable, else the images that need a DDS -> IWI."""
        if self.raw is None or kind not in RAW_LAYOUT or name.lower() in self.loaded.get(kind, set()):
            return None
        folder, suffix = RAW_LAYOUT[kind]
        path = self.raw / folder / f"{name}{suffix}"
        if not path.is_file():
            return None
        return self._model_images(path) if kind == "xmodel" else ()

    def _model_images(self, path: Path) -> tuple[str, ...] | None:
        """The mod linker must find every LOD, material and image as a raw file:
        many stock materials name linker-packed images (``~...``) that exist
        only inside the stock ipaks, which a converted map does not load. The
        ipak writer reads images as .iwi files (measured: "Failed to open file
        for ipak: images/<name>.iwi" although <name>.dds is in raw/images), so
        DDS-only images are returned for conversion."""
        dds = set()
        try:
            model = json.loads(path.read_text(encoding="utf-8"))
            for lod in model.get("lods", []):
                for material in _gltf_materials(self.raw / lod["file"]):
                    mat = self.raw / "materials" / (material.replace("*", "_") + ".json")
                    if not mat.is_file():
                        return None
                    for texture in json.loads(mat.read_text(encoding="utf-8")).get("textures", []):
                        image = texture.get("image", "").removeprefix(",")
                        if image.lower() in self.loaded.get("image", set()):
                            return None    # a second copy in mod.ff would override a loaded asset
                        if not image or image.startswith("$") or (self.raw / "images" / f"{image}.iwi").is_file():
                            continue
                        if not (self.raw / "images" / f"{image}.dds").is_file():
                            return None
                        dds.add(image)
        except (OSError, ValueError, KeyError):
            return None
        return tuple(sorted(dds))

    def find(self, kind: str, waw_name: str) -> Equivalent | None:
        images = self._available(kind, waw_name)
        if images is not None:
            return Equivalent(waw_name, "same_name", images)
        mapped = self.table.get(kind, {}).get(waw_name.lower())
        if mapped and (images := self._available(kind, mapped)) is not None:
            return Equivalent(mapped, "table", images)
        return None

    def stage(self, found: Equivalent, project: Path) -> None:
        """Convert the equivalent's DDS-only raw images to IWI in ``project``."""
        from . import iwi, techsets

        for image in found.dds_images:
            dst = project / techsets.oat_image_path(image)
            if not dst.is_file():
                iwi.convert_file(self.raw / "images" / f"{image}.dds", dst)


def _gltf_materials(path: Path) -> list[str]:
    """Material names of a .gltf or binary .glb (its first chunk is the JSON)."""
    data = path.read_bytes()
    if data[:4] == b"glTF":
        length = int.from_bytes(data[12:16], "little")
        document = json.loads(data[20:20 + length])
    else:
        document = json.loads(data)
    return [m["name"] for m in document.get("materials", [])]
