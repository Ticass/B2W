"""Original compiled WaW dependency closure, including stock-zone definitions.

This resolver never consults BO2 for a substitute. Source roots have explicit
priority; newly fetched stock dumps are ordered deterministically. Unresolved
nodes retain their incoming edges so diagnostics show why they were required.
"""
from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from pathlib import Path
from .resources import resource_root

from . import fx, techsets, weapons


def asset_path(kind: str, name: str) -> Path:
    if kind == "loadedsound":
        # WaW joins "sound/" + name and collapses repeated separators
        name = re.sub("/+", "/", name.replace(chr(92), "/")).lstrip("/")
    weapons.output_name(kind, name)  # reject traversal/zone injection before I/O
    if kind == "weapon":
        return Path("weapons") / name
    if kind == "xmodel":
        return Path("xmodel") / f"{name}.json"
    if kind == "material":
        return techsets.oat_material_path(name)
    if kind == "image":
        return techsets.oat_image_path(name, ".dds")
    if kind == "xanim":
        return Path("xanim") / name
    if kind == "sound":
        return Path("soundaliases") / f"{name}.w2bsnd.json"
    if kind == "loadedsound":
        return Path("sound") / name
    if kind == "fx":
        return Path("fx") / f"{name}.w2bfx.json"
    if kind == "physpreset":
        return Path("physic") / name
    if kind.startswith("accuracy_"):
        return Path("accuracy") / kind.removeprefix("accuracy_") / name
    if kind == "rawfile":
        return Path(name)
    raise weapons.WeaponError(f"no compiled source path for asset kind {kind}")


@dataclass
class Resolver:
    source_roots: list[Path]
    stock: object | None = None
    nodes: dict[tuple[str, str], dict] = field(default_factory=dict)
    edges: set[tuple[str, str, str, str]] = field(default_factory=set)
    stock_roots: dict[str, Path] = field(default_factory=dict)

    @property
    def roots(self) -> list[Path]:
        from .wawassets import _zone_rank
        return [*self.source_roots, *[self.stock_roots[z] for z in sorted(self.stock_roots, key=_zone_rank)]]

    def find(self, kind: str, name: str) -> Path | None:
        relative = asset_path(kind, name)
        for root in self.source_roots:
            path = root / relative
            if path.is_file():
                return path
            if kind == "loadedsound" and path.with_suffix(".xwma").is_file():
                return path.with_suffix(".xwma")
        # Accuracy graphs are owned by their parent weapon, not XAssets.
        if kind.startswith("accuracy_"):
            for root in self.stock_roots.values():
                path = root / relative
                if path.is_file():
                    return path
            return None
        if self.stock is None:
            return None
        result = self.stock.root_for(kind, name)
        if result is None:
            return None
        zone, root = result
        self.stock_roots[zone] = root
        path = root / relative
        if path.is_file():
            return path
        if kind == "loadedsound" and path.with_suffix(".xwma").is_file():
            return path.with_suffix(".xwma")
        return None

    def children(self, kind: str, path: Path, name: str | None = None) -> set[tuple[str, str]]:
        children = set()
        if kind == "weapon":
            source = weapons.read_info_file(path)
            base = resource_root()
            t4 = weapons.field_schema(base / "vendor/OpenAssetTools/src/ObjCommon/Game/T4/Weapon/WeaponFields.h")
            for key, value in source.items():
                if not value:
                    continue
                child_kind = weapons.DEPENDENCY_TYPES.get(t4.get(key))
                if child_kind:
                    children.add((child_kind, value))
                if t4.get(key) == "WFT_BOUNCE_SOUND":
                    children.update(("sound", n) for n in weapons.bounce_aliases(value, self.roots, self.stock))
                if key == "altWeapon":
                    children.add(("weapon", value))
                if key in ("aiVsAiAccuracyGraph", "aiVsPlayerAccuracyGraph"):
                    children.add(("accuracy_aivsai" if key == "aiVsAiAccuracyGraph" else "accuracy_aivsplayer", value))
                if t4.get(key) == "WFT_NOTETRACKSOUNDMAP":
                    for line in value.splitlines():
                        tokens = line.split()
                        if len(tokens) != 2:
                            raise weapons.WeaponError(f"invalid notetrack sound dependency in {path}: {line!r}")
                        children.add(("sound", tokens[1]))
        elif kind == "xmodel":
            model = json.loads(path.read_text())
            if model.get("physPreset"):
                children.add(("physpreset", model["physPreset"]))
            root = next(r for r in self.roots if path == r / asset_path(kind, name)) if name is not None else \
                next(r for r in self.roots if path.is_relative_to(r))
            files = [lod['file'] for lod in model.get('lods', [])]
            missing = [file for file in files if weapons.model_lod_source(self.roots, root, file) is None]
            if missing and self.stock is not None and name is not None:
                # Preserve source metadata; fetch only the exact named WaW
                # model's exported geometry, never a substitute weapon/model.
                result = self.stock.root_for('xmodel', name)
                if result is not None:
                    zone, stock_root = result
                    self.stock_roots[zone] = stock_root
            for lod in model.get("lods", []):
                file = lod["file"]
                source = weapons.model_lod_source(self.roots, root, file)
                if source is None:
                    node = self.nodes.get((kind, name))
                    if node is not None:
                        node['status'] = 'missing_model_geometry'
                        node.setdefault('missing_files', []).append(file)
                    continue
                if source != root / file and name is not None:
                    self.nodes[kind, name].setdefault('recovered_geometry', []).append(str(source))
                gltf = json.loads(source.read_text())
                children.update(("material", m["name"]) for m in gltf.get("materials", []))
        elif kind == "material":
            material = json.loads(path.read_text())
            children.update(("image", t["image"].removeprefix(",")) for t in material.get("textures", []) if t.get("image"))
        elif kind == "sound":
            sound = json.loads(path.read_text())
            for alias in sound.get("aliases", []):
                for field_name in ("secondaryAliasName", "chainAliasName"):
                    if alias.get(field_name):
                        children.add(("sound", alias[field_name]))
                file = alias.get("soundFile")
                if file and file.get("type") == 1 and file.get("name"):
                    children.add(("loadedsound", file["name"]))
                elif file and file.get("primedName"):
                    children.add(("loadedsound", file["primedName"]))
        elif kind == "fx":
            dependencies = fx.dependencies(json.loads(path.read_text()))
            for child_kind, values in (("material", dependencies.materials), ("xmodel", dependencies.models),
                                        ("sound", dependencies.sounds), ("fx", dependencies.effects)):
                children.update((child_kind, value) for value in values)
        return children

    def expand(self, requests: set[tuple[str, str]]) -> dict:
        pending = set(requests)
        while pending:
            kind, name = min(pending)
            pending.remove((kind, name))
            if (kind, name) in self.nodes:
                continue
            path = self.find(kind, name)
            node = {"kind": kind, "name": name, "status": "resolved" if path else "missing_compiled_source"}
            self.nodes[kind, name] = node
            if path is None:
                continue
            node["source"] = str(path)
            zone = next((z for z, root in self.stock_roots.items() if path.is_relative_to(root)), None)
            if zone:
                node.update(provenance="WAW_STOCK_ASSET", zone=zone)
            else:
                node["provenance"] = "map_or_companion"
            for child_kind, child_name in self.children(kind, path, name):
                asset_path(child_kind, child_name)
                self.edges.add((kind, name, child_kind, child_name))
                pending.add((child_kind, child_name))
        return {"nodes": [self.nodes[k] for k in sorted(self.nodes)],
                "edges": [list(e) for e in sorted(self.edges)],
                "roots": [str(r) for r in self.roots],
                "missing": [self.nodes[k] for k in sorted(self.nodes) if self.nodes[k]["status"] != "resolved"]}
