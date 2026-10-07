"""Compiled WaW weapon infostrings -> T6 source assets and dependency graph.

Use the compiled-zone dump, not the sometimes out-of-date IWD source weapon.
Field names AND types are read from the two engines' measured OAT schemas.
Unknown fields remain in the report; no stock weapon serves as a template.
The official BO2 linker consumes these infostrings and WaW's raw v17 xanims.
"""
from __future__ import annotations

import json
import re
import shutil
from dataclasses import dataclass, field
from pathlib import Path


class WeaponError(ValueError):
    pass


def read_info(text: str) -> dict[str, str]:
    parts = text.split("\\")
    if parts[0] != "WEAPONFILE" or len(parts) % 2 != 1:
        raise WeaponError("invalid WEAPONFILE key/value stream")
    result = {}
    for key, value in zip(parts[1::2], parts[2::2]):
        if key in result:
            raise WeaponError(f"duplicate weapon field {key}")
        result[key] = value
    return result


def write_info(fields: dict[str, str]) -> str:
    if any("\\" in key or "\\" in value for key, value in fields.items()):
        raise WeaponError("infostring field contains a separator")
    return "WEAPONFILE" + "".join(f"\\{key}\\{value}" for key, value in fields.items())


def field_schema(header: Path) -> dict[str, str]:
    result = {}
    for line in header.read_text().splitlines():
        match = re.search(r'\{"([^"]+)".*?([A-Z_0-9]+)\s*\}', line)
        if match:
            result[match[1]] = match[2]
    if len(result) < 100:
        raise WeaponError(f"weapon field schema incomplete: {header}")
    return result


def output_name(kind: str, name: str) -> str:
    if not name:
        return ""
    name = name.replace("\\", "/")
    if name.startswith("/") or ".." in name.split("/") or any(c in name for c in ":,\r\n\0"):
        raise WeaponError(f"unsafe {kind} name: {name!r}")
    # Weapons keep their WaW names: scripts name them (giveweapon, wallbuys) and
    # no always-loaded BO2 zone carries a weapon of a WaW name (checked against
    # the stock asset lists in stage_runtime). Everything else is namespaced:
    # BO2 zones load same-named, different xmodels/xanims/materials/images.
    return name if kind == "weapon" else f"waw_{kind}/" + name


DEPENDENCY_TYPES = {"CSPFT_XMODEL": "xmodel", "CSPFT_MATERIAL": "material", "CSPFT_FX": "fx",
                    "CSPFT_SOUND": "sound", "WFT_ANIM_NAME": "xanim", "CSPFT_TRACER": "tracer",
                    "CSPFT_PHYS_PRESET": "physpreset"}


def bounce_aliases(prefix: str, roots: list[Path], stock=None) -> set[str]:
    """Existing surface aliases for a T4 weapon's bounce-sound prefix."""
    names = {prefix + '_default'}
    for root in roots:
        folder = root / 'soundaliases'
        if folder.is_dir():
            names.update(p.relative_to(folder).as_posix().removesuffix('.w2bsnd.json')
                         for p in folder.rglob('*.w2bsnd.json')
                         if p.relative_to(folder).as_posix().startswith(prefix + '_'))
    if stock is not None:
        names.update(n for kinds in stock.index.values() for n in kinds.get('sound', [])
                     if n.startswith(prefix + '_'))
    return names


# Values the BO2 mod tools linker accepts (its own error message lists them).
T6_PLAYER_ANIM_TYPES = {"none", "default", "other", "sniper", "m203", "hold", "briefcase", "reviver", "radio",
                        "dualwield", "remotecontrol", "crossbow", "minigun", "beltfed", "g11", "rearclip",
                        "handleclip", "rearclipsniper", "ballisticknife", "singleknife", "nopump", "hatchet",
                        "grimreaper", "zipline", "riotshield", "tablet", "turned", "screecher", "staff"}
# WaW class-named third-person profiles -> T6 "default", measured on the BO2
# raw *_zm weapons of the same weaponClass (counts at the time of measuring).
CLASS_ANIM_TYPES = {
    "autorifle": "BO2 rifles use default (M14/M16, 24 rifle weapons)",
    "pistol": "BO2 pistols use default (22 of 25 pistol weapons; rest dualwield/ballistic)",
    "smg": "BO2 SMGs use default (16 of 25 smg weapons; rest rearclip/g11)",
    "mg": "BO2 box-fed MGs use default (hamr, rpd); beltfed only for belt feeds",
    "rocketlauncher": "BO2 rocket launchers use default (usrpg)",
    "grenade": "BO2 grenades use default (13 grenade weapons incl. frag_grenade_zm)",
}


@dataclass
class ConvertedWeapon:
    name: str
    fields: dict[str, str]
    dependencies: dict[str, set[str]] = field(default_factory=dict)
    unsupported: dict[str, dict] = field(default_factory=dict)
    translations: dict[str, dict] = field(default_factory=dict)

    def report(self) -> dict:
        return {"name": self.name, "dependencies": {k: sorted(v) for k, v in self.dependencies.items()},
                "unsupported_fields": self.unsupported, "translations": self.translations}


def convert(name: str, source: dict[str, str], t4_schema: dict[str, str], t6_schema: dict[str, str]) -> ConvertedWeapon:
    out = ConvertedWeapon(output_name("weapon", name), {})
    for key, value in source.items():
        src_type, dst_type = t4_schema.get(key), t6_schema.get(key)
        target_key = key
        if key == "adsZoomFov" and src_type == "CSPFT_FLOAT":
            target_key = "adsZoomFov1"
            dst_type = t6_schema.get(target_key)
        # T4 stores alias pointers; T6 stores their names. This is a semantic
        # conversion, not a generic permission to coerce arbitrary fields.
        sound_name = src_type == "CSPFT_SOUND" and dst_type == "CSPFT_STRING"
        if dst_type is None or (src_type != dst_type and not sound_name):
            if value not in ("", "0", "0.0"):
                out.unsupported[key] = {"value": value, "source_type": src_type, "target_type": dst_type}
            continue
        if key == "playerAnimType" and value in CLASS_ANIM_TYPES:
            # T6 has no class-named third-person profiles: the stock BO2 weapons
            # of these classes use "default" and T6 picks the family from
            # weaponClass. This is NOT a replacement of the original
            # first-person animation tracks.
            out.translations[key] = {"source": value, "target": "default", "reason": CLASS_ANIM_TYPES[value]}
            value = "default"
        elif key == "playerAnimType" and value and value not in T6_PLAYER_ANIM_TYPES:
            out.unsupported[key] = {"value": value, "source_type": src_type, "target_type": dst_type,
                                    "reason": "no measured T6 playerAnimType"}
            continue
        kind = DEPENDENCY_TYPES.get(src_type)
        if value and src_type == "WFT_BOUNCE_SOUND":
            # Both loaders append surface suffixes to this prefix. The aliases
            # live in the WaW bank namespace, just like ordinary sound fields.
            output_name("sound", value)
            out.dependencies.setdefault("sound", set()).add(value + "_default")
            out.translations[key] = {"source": value, "target": "waw/" + value,
                "reason": "T4 surface bounce aliases -> namespaced T6 sound prefix"}
            value = "waw/" + value
        if value and kind:
            output_name(kind, value)  # validate all asset paths, including sound/FX
            out.dependencies.setdefault(kind, set()).add(value)
            # Effects already use the shared FX namespace.
            value = "waw/" + value if kind in ("fx", "sound") else output_name(kind, value)
        if value and src_type == "WFT_NOTETRACKSOUNDMAP":
            pairs = []
            for line in value.splitlines():
                tokens = line.split()
                if len(tokens) != 2:
                    raise WeaponError(f"{name}: malformed notetrackSoundMap {line!r}")
                notify, alias = tokens
                output_name("sound", alias)
                out.dependencies.setdefault("sound", set()).add(alias)
                pairs.append(f"{notify} waw/{alias}")
            value = "\n".join(pairs)
        if value and key == "altWeapon":
            out.dependencies.setdefault("weapon", set()).add(value)
            value = output_name("weapon", value)
        if value and key in ("aiVsAiAccuracyGraph", "aiVsPlayerAccuracyGraph"):
            graph_kind = "accuracy_aivsai" if key == "aiVsAiAccuracyGraph" else "accuracy_aivsplayer"
            out.dependencies.setdefault(graph_kind, set()).add(value)
            value = output_name("accuracy", value)
        # Internal clip/ammo pools must not collide with another game's gun.
        if value and key in ("ammoName", "clipName"):
            value = "waw_" + value
        out.fields[target_key] = value
        if key == "adsZoomFov" and target_key == "adsZoomFov1":
            # T6 keeps one ADS FOV per zoom level and stock weapons repeat the
            # same value in all three (m1911_zm: 65/65/65). A level left at 0
            # zooms to a 0-degree FOV (measured in game: over-zoomed ADS).
            out.fields["adsZoomFov2"] = out.fields["adsZoomFov3"] = value
            out.translations[key] = {"source": value, "target": "adsZoomFov1/2/3",
                                     "reason": "T6 per-zoom-level ADS FOV; WaW has one"}
    # WaW selects offhands by class. T6 additionally needs a slot; leaving its
    # new field at None produces an offhand the grenade inventory cannot own.
    slot = {"Frag Grenade": "Lethal grenade", "Smoke Grenade": "Tactical grenade",
            "Flash Grenade": "Tactical grenade"}.get(source.get("offhandClass"))
    if slot and "offhandSlot" not in source:
        out.fields["offhandSlot"] = slot
        out.translations["offhandSlot"] = {"source": source["offhandClass"], "target": slot,
            "reason": "T4 offhand class -> T6 required inventory slot"}
    return out


def discover(roots: list[Path]) -> dict[str, Path]:
    """Every compiled weapon in source priority order, including nested names."""
    found = {}
    for root in roots:
        directory = root / "weapons"
        for path in sorted(directory.rglob("*")) if directory.exists() else ():
            if path.is_file() and path.read_bytes().startswith(b"WEAPONFILE\\"):
                found.setdefault(path.relative_to(directory).as_posix(), path)
    return found


def stage_models(roots: list[Path], project: Path, names: set[str]) -> dict:
    """Preserve weapon skeletons and LODs; never use the static-prop skin stripper.

    Models/material names are isolated from stock BO2. Multi-root skeletons
    require a loader fix and are explicitly rejected, not made rigid. Materials
    and image dependencies are reported for the subsequent translation stage.
    """
    staged, missing, unsupported, materials = [], [], {}, set()
    physpresets: dict[str, dict] = {}
    reparented: dict[str, list[str]] = {}
    no_collision: list[str] = []
    for name in sorted(names):
        model_materials = set()
        renamed = output_name("xmodel", name)
        relative = Path("xmodel") / f"{name}.json"
        root = next((r for r in roots if (r / relative).is_file()), None)
        if root is None:
            missing.append(name)
            continue
        model = json.loads((root / relative).read_text(encoding="utf-8"))
        try:
            if model.get("flags", 0):
                raise WeaponError("nonzero T4 xmodel flags need measured T6 translation")
            if model.get("physPreset"):
                model["physPreset"] = stage_physpreset(roots, project, model["physPreset"], physpresets)
            lods = []
            for index, lod in enumerate(model.get("lods", [])):
                file = lod["file"]
                output_name("model_file", file)
                source = root / file
                data = json.loads(source.read_text(encoding="utf-8"))
                parents = {c: i for i, node in enumerate(data.get("nodes", []))
                           for c in node.get("children", [])}
                for skin in data.get("skins", []):
                    joints = set(skin.get("joints", []))
                    root_joints = sorted(j for j in joints if parents.get(j) not in joints)
                    if len(root_joints) > 1 and model.get("type") == "rigid" and \
                            _merge_equal_roots(data, root_joints):
                        reparented[name] = [data["nodes"][r].get("name") for r in root_joints]
                        continue
                    if len(root_joints) != 1:
                        raise WeaponError("multiple/absent skin roots: skeleton preserved at source, not stripped")
                # OAT resolves engine materials by the GLTF material NAME;
                # GLTF images/textures are exporter previews, not dependencies
                # of its xmodel reader (GltfLoader::LoadMaterials). Keep their
                # source metadata, but do not mistake DDS preview URIs for
                # skeleton buffers or reject otherwise valid animated models.
                for resource in data.get("buffers", []):
                    uri = resource.get("uri", "")
                    if uri and not uri.startswith("data:"):
                        raise WeaponError(f"external GLTF resource needs dependency staging: {uri}")
                for material in data.get("materials", []):
                    material_name = material["name"]
                    material["name"] = output_name("material", material_name)
                    materials.add(material_name)
                    model_materials.add(material_name)
                destination = Path("model_export") / f"{renamed}_lod{index}.gltf"
                lods.append((dict(lod, file=destination.as_posix()), destination, data))
            if not lods:
                raise WeaponError("xmodel has no LODs")
        except (WeaponError, OSError, KeyError, ValueError) as exc:
            unsupported[name] = str(exc)
            continue
        # Validate every LOD before producing any model with partial geometry.
        for _, destination, data in lods:
            dst = project / destination
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_text(json.dumps(data, separators=(",", ":")), encoding="utf-8")
        model["_game"] = "t6"
        # These models are linked by the BO2 mod tools linker, whose xmodel JSON
        # has its own, different "collSurfs" schema (measured: it rejects the
        # T4 plane/svec/tvec triangles). Model collision is for world entities
        # (map zone, our bridge linker); weapon models do not carry it here.
        if model.pop("collSurfs", None):
            no_collision.append(name)
        model.pop("contents", None)
        model["lods"] = [lod for lod, _, _ in lods]
        model["lightingOriginOffset"] = {"x": 0.0, "y": 0.0, "z": 0.0}
        model["lightingOriginRange"] = 0.0
        dst = project / "xmodel" / f"{renamed}.json"
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text(json.dumps(model, indent=2) + "\n", encoding="utf-8")
        staged.append({"name": renamed, "waw": name, "source": str(root / relative), "lods": len(lods),
                       "materials": sorted(model_materials)})
    return {"status": "source_models_staged_materials_not_translated", "models": staged, "missing": missing,
            "unsupported": unsupported, "materials": sorted(materials), "physpresets": physpresets,
            "reparented_roots": reparented, "collision_not_carried": no_collision}


TRS_KEYS = ("translation", "rotation", "scale", "matrix")


def _merge_equal_roots(gltf: dict, roots: list[int]) -> bool:
    """T4/T6 xmodels may have several root bones; the BO2 linker's GLTF loader
    takes exactly one. When every root has the SAME bind transform under the
    same parent node, the secondary roots become identity children of the
    primary one (tag_origin when present): every bone keeps its exact global
    transform, so a rigid (unanimated) model is unchanged. Returns False and
    leaves the data untouched otherwise."""
    nodes = gltf["nodes"]
    parent = {c: i for i, n in enumerate(nodes) for c in n.get("children", [])}
    transforms = [{k: nodes[r][k] for k in TRS_KEYS if k in nodes[r]} for r in roots]
    if len({parent.get(r) for r in roots}) != 1 or any(t != transforms[0] for t in transforms):
        return False
    primary = next((r for r in roots if nodes[r].get("name") == "tag_origin"), roots[0])
    holder = parent.get(primary)
    for r in roots:
        if r == primary:
            continue
        for k in TRS_KEYS:
            nodes[r].pop(k, None)
        if holder is not None:
            nodes[holder]["children"].remove(r)
        nodes[primary].setdefault("children", []).append(r)
        for scene in gltf.get("scenes", []):
            if r in scene.get("nodes", []):
                scene["nodes"].remove(r)
    return True


def stage_physpreset(roots: list[Path], project: Path, name: str, staged: dict[str, dict]) -> str:
    """T4 and T6 PHYSIC infostrings share every T4 key (the T4 dumper writes
    mass*1000 and isFrictionInfinity exactly as the T6 loader reads them), so
    the original values carry over unchanged. The sound alias prefix names
    WaW aliases, which live under waw/ in the converted bank."""
    renamed = output_name("physpreset", name)
    if name in staged:
        return renamed
    relative = Path("physic") / name
    source = next((r / relative for r in roots if (r / relative).is_file()), None)
    if source is None:
        raise WeaponError(f"physics preset {name}: physic/{name} absent from the supplied WaW dumps")
    fields = source.read_text(encoding="utf-8").split("\\")
    if fields[0] != "PHYSIC" or len(fields) % 2 != 1:
        raise WeaponError(f"physics preset {name}: not a PHYSIC infostring")
    pairs = dict(zip(fields[1::2], fields[2::2]))
    if pairs.get("sndAliasPrefix"):
        pairs["sndAliasPrefix"] = "waw/" + pairs["sndAliasPrefix"]
    dst = project / "physic" / renamed
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text("PHYSIC" + "".join(f"\\{k}\\{v}" for k, v in pairs.items()), encoding="utf-8")
    staged[name] = {"name": renamed, "source": str(source)}
    return renamed


def plan(roots: list[Path], project: Path) -> tuple[dict[str, ConvertedWeapon], dict]:
    base = Path(__file__).resolve().parents[2]
    t4 = field_schema(base / "vendor/OpenAssetTools/src/ObjCommon/Game/T4/Weapon/WeaponFields.h")
    t6 = field_schema(base / "vendor/OpenAssetToolsT6/src/ObjCommon/Game/T6/Weapon/WeaponFields.h")
    converted = {}
    sources = discover(roots)
    for name, path in sources.items():
        output_name("weapon", name)
        converted[name] = convert(name, read_info(path.read_text(encoding="utf-8")), t4, t6)
        prefix = converted[name].fields.get('bounceSound', '').removeprefix('waw/')
        if prefix:
            converted[name].dependencies.setdefault('sound', set()).update(bounce_aliases(prefix, roots))
    report = {"weapons": [dict(w.report(), source=str(sources[name])) for name, w in sorted(converted.items())]}
    project.mkdir(parents=True, exist_ok=True)
    (project / "weapons.plan.json").write_text(json.dumps(report, indent=2) + "\n")
    return converted, report


def stage(roots: list[Path], project: Path, names: set[str] | None = None, stock=None,
          extra_requests: set[tuple[str, str]] = frozenset()) -> dict:
    """Stage compiled weapon fields and *original* raw animation tracks.

    The resulting zone fragment is for the installed BO2 linker. It does not
    imply gameplay registration or complete model/sound/FX conversion. Every
    outstanding dependency and incompatible field is included in the report.
    Missing dependencies are never replaced with another game's asset.
    """
    graph = None
    if stock is not None:
        from .assetresolve import Resolver
        # Only the map/companion weapons are initial roots. A stock dump made
        # for one dependency must not add every unrelated stock gun to the mod.
        names = names if names is not None else set(discover(roots))
        resolver = Resolver(roots, stock)
        graph = resolver.expand({("weapon", n) for n in names} | set(extra_requests))
        roots = resolver.roots
    converted, report = plan(roots, project)
    if names is not None:
        missing = names - converted.keys()
        if missing:
            raise WeaponError(f"compiled weapons not found: {sorted(missing)}")
        pending = list(names)
        selected = set()
        while pending:
            name = pending.pop()
            if name in selected:
                continue
            if name not in converted:
                raise WeaponError(f"alternate weapon not found: {name}")
            selected.add(name)
            pending.extend(converted[name].dependencies.get("weapon", ()))
        converted = {n: w for n, w in converted.items() if n in selected}
        report["weapons"] = [r for r in report["weapons"] if r["name"] in {w.name for w in converted.values()}]
    dependencies: dict[str, set[str]] = {}
    for weapon in converted.values():
        for kind, values in weapon.dependencies.items():
            dependencies.setdefault(kind, set()).update(values)
        dst = project / "weapons" / weapon.name
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text(write_info(weapon.fields), encoding="utf-8")
    graphs, missing_graphs = [], []
    for kind, folder in (("accuracy_aivsai", "aivsai"), ("accuracy_aivsplayer", "aivsplayer")):
        for name in sorted(dependencies.get(kind, ())):
            renamed = output_name("accuracy", name)
            relative = Path("accuracy") / folder / name
            src = next((r / relative for r in roots if (r / relative).is_file()), None)
            if src is None:
                missing_graphs.append(relative.as_posix())
                continue
            dst = project / "accuracy" / folder / renamed
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
            graphs.append({"name": renamed, "folder": folder, "source": str(src)})
    animations, missing_animations = [], []
    for name in sorted(dependencies.get("xanim", ())):
        relative = Path("xanim") / name
        # Validate BEFORE using a source-controlled name as a file path.
        renamed = output_name("xanim", name)
        src = next((r / relative for r in roots if (r / relative).is_file()), None)
        if src is None:
            missing_animations.append(name)
            continue
        dst = project / "xanim" / renamed
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        animations.append({"source": str(src), "name": renamed})
    lines = [f"xanim,{a['name']}" for a in animations]
    lines.extend(f"weapon,{w.name}" for w in sorted(converted.values(), key=lambda w: w.name))
    (project / "weapons.zone.fragment").write_text("\n".join(lines) + "\n", encoding="utf-8")
    report.update(status="partial_source_stage", animations=animations, missing_animations=missing_animations,
                  accuracy_graphs=graphs, missing_accuracy_graphs=missing_graphs,
                  dependencies={k: sorted(v) for k, v in dependencies.items()},
                  remaining=["model/material conversion", "sound-bank conversion", "FX dependency linking",
                             "unsupported weapon field translation", "Zombies gameplay registration",
                             "native roundtrip and in-game validation"])
    # extra_requests (models the map scripts use) only widen the resolved
    # roots; those models belong to the map zone under their WaW names.
    report["models"] = stage_models(roots, project, dependencies.get("xmodel", set()))
    if graph is not None:
        report["dependency_graph"] = "weapons.dependencies.json"
        report["resolved_roots"] = graph["roots"]
        (project / "weapons.dependencies.json").write_text(json.dumps(graph, indent=2) + "\n", encoding="utf-8")
    (project / "weapons.stage.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def stage_visuals(roots: list[Path], project: Path, stock_materials: Path, techset_dump: Path,
                  wavelets=None, *, native_fallbacks: dict[str, Path] | None = None) -> dict:
    """Translate weapon materials and isolate their original images from BO2.

    Uses the same measured material translator as world/FX assets. Missing
    image data are errors here: no generic grey gun texture is generated.
    ``failed_materials`` (WaW name -> reasons) lets stage_runtime keep exactly
    the weapons whose visuals are complete. ``wavelets`` (wavelet.IwdRecovery)
    decodes WaW wavelet IWIs the zone dumps could not export as DDS.
    """
    from . import iwi, t6bridge, techsets

    stage_report = json.loads((project / "weapons.stage.json").read_text(encoding="utf-8"))
    names = set(stage_report["models"]["materials"])
    names.update(stage_report["dependencies"].get("material", []))
    report = t6bridge.StageReport("weapon_visuals")
    used = t6bridge.stage_materials(report, names, roots, stock_materials, project, techset_dump,
                                    {n: output_name("material", n) for n in names}, image_prefix='',
                                    native_fallbacks=native_fallbacks)
    translated = {e.get("source", e["name"]) for e in report.materials}
    failed: dict[str, list[str]] = {n: [e for e in report.errors if e.startswith(f"material {n}:")
                                        or e.startswith(f"material {n} (")] or ["not translated"]
                                    for n in names - translated}
    image_status: dict[str, str | None] = {}   # output image -> error (None = converted)
    material_images: dict[str, list[str]] = {}
    for entry in report.materials:
        waw_material = entry.get("source", entry["name"])
        path = project / entry["file"]
        material = json.loads(path.read_text())
        images = []
        for texture in material.get("textures", []):
            name = texture.get("image", "")
            if not name:
                continue
            if name.startswith((',', '$')):
                texture['image'] = name.removeprefix(',')
                images.append(',' + texture['image'])
                continue
            if entry.get('native_equivalent'):
                # stage_materials already copied the native IWI and namespaced
                # its binding. Looking for those pixels in WaW would lose them.
                images.append(name)
                continue
            output = output_name("image", name.removeprefix(","))
            if output not in image_status:
                image_status[output] = _stage_image(roots, project, name.removeprefix(","), output, wavelets,
                                                    report)
            status = image_status[output]
            code = ABSENT_IMAGE_SLOTS.get(texture.get("name", ""))
            if status is not None and status.startswith(ABSENT) and code:
                # GUIDELINES 2a: the pixels exist nowhere in WaW (WaW drew its
                # default texture); a BO2 code image of the slot's neutral value
                texture["image"] = code.removeprefix(",")
                images.append(code)   # zone line "image,,$x": a reference, never packed
                report.warnings.append(f"BO2_FALLBACK image {name} -> {code}: {texture['name']} of material "
                                       f"{waw_material}; {status.removeprefix(ABSENT)}")
                continue
            texture["image"] = output
            images.append(output)
            if status is not None:
                failed.setdefault(waw_material, []).append(status.removeprefix(ABSENT))
        material_images[entry["name"]] = images
        path.write_text(json.dumps(material, indent=2) + "\n", encoding="utf-8")
    for name in used:
        relative = Path("techniquesets") / f"{name}.json"
        source = project / relative if (project / relative).is_file() else techset_dump / relative
        if not source.is_file():
            report.errors.append(f"weapon technique set {name}: missing from {techset_dump}")
            continue
        destinations = {relative} | {Path(p) for p in t6bridge._techset_shaders(json.loads(source.read_text()))}
        for relative in destinations:
            dst = project / relative
            if dst.is_file():
                continue
            source = techset_dump / relative
            if not source.is_file():
                report.errors.append(f"weapon shader {relative}: missing from {techset_dump}")
                continue
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, dst)
    result = dict(status="staged_not_native_validated", failed_materials=failed, material_images=material_images,
                  **report.__dict__)
    (project / "weapons.visuals.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


# Status prefix of an image whose pixels exist nowhere in the WaW sources
# (a streamed image header without its .iwi), as opposed to a failed conversion.
ABSENT = "absent: "
# Texture slot -> BO2 code image with that slot's neutral value. Code images
# come from the always-loaded stock zones: the material names the image and
# the zone lists it as a reference ("image,,$white"), so it is never packed.
ABSENT_IMAGE_SLOTS = {"normalMap": ",$identitynormalmap", "specularMap": ",$black", "colorMap": ",$white"}


def _stage_image(roots: list[Path], project: Path, name: str, output: str, wavelets, report) -> str | None:
    from . import iwi, techsets

    relative = techsets.oat_image_path(name, ".dds")
    source = next((r / relative for r in roots if (r / relative).is_file()), None)
    if source is None and wavelets is not None:
        source = wavelets.recover(relative.stem)
    if source is None:
        error = f"weapon image {name}: original pixels absent from supplied zone dumps and IWDs"
        report.errors.append(error)
        return ABSENT + error
    try:
        header = iwi.convert_file(source, project / techsets.oat_image_path(output))
    except (iwi.IwiError, OSError) as exc:
        report.errors.append(f"weapon image {name}: {exc}")
        return f"weapon image {name}: {exc}"
    report.images.append({"name": output, "source": str(source), **header})
    return None


# Model-index variants (camo / script-selected model slots). An absent one
# leaves the weapon usable; the primary models do not.
VARIANT_MODEL_SLOT = re.compile(r"^(gunModel|worldModel)([2-9]|1[0-6])$")
ENGINE_WEAPONS = {"none"}
# User-authorized ordinary lethal replacement. Never classify unknown/special
# grenades by offhand class alone: custom equipment can share that class.
BO2_PRIMARY_FRAGS = {"fraggrenade", "stielhandgranate"}


def primary_frag_replacement(name: str, fields: dict[str, str]) -> str | None:
    if (name.lower() in BO2_PRIMARY_FRAGS and fields.get("weaponType") == "grenade"
            and fields.get("offhandClass") == "Frag Grenade"
            and fields.get("offhandSlot") == "Lethal grenade"):
        return "frag_grenade_zm"
    return None


# T6 weapon field types stored as raw char* (see stage_runtime)
POINTER_STRING_FIELDS = {"CSPFT_STRING", "WFT_ANIM_NAME"}
WAW_WEAPON_PREFIX = "waw_"
PREFIXES = {"xmodel": "waw_xmodel/", "material": "waw_material/", "xanim": "waw_xanim/",
            "accuracy": "waw_accuracy/"}


def stage_runtime(project: Path, ipak: str, equivalents=None, loaded: dict[str, set[str]] | None = None,
                  converted_fx: set[str] = frozenset(), reserved_weapons: set[str] = frozenset()) -> dict:
    """Choose the staged weapons mod.ff can carry and write their zone lines.

    A weapon is carried when every dependency it needs converted. Absent
    (not merely unconverted) models get a BO2 equivalent (GUIDELINES 2a) or,
    for a model-index variant slot, are cleared; UI materials, animations and
    effects that did not convert are cleared from their field. Everything is
    reported. Excluded weapons stay out of mod.ff entirely, never half-built.
    Standard WaW lethal frags use the user-requested native BO2 frag; their
    source files are parked so inventory cheats cannot expose broken copies.
    """
    base = Path(__file__).resolve().parents[2]
    t6_types = field_schema(base / "vendor/OpenAssetToolsT6/src/ObjCommon/Game/T6/Weapon/WeaponFields.h")
    stage_report = json.loads((project / "weapons.stage.json").read_text(encoding="utf-8"))
    visuals = json.loads((project / "weapons.visuals.json").read_text(encoding="utf-8"))
    models = {m["waw"]: m for m in stage_report["models"]["models"]}
    missing_models = set(stage_report["models"]["missing"])
    unsupported_models = stage_report["models"]["unsupported"]
    failed_materials = visuals["failed_materials"]
    missing_anims = set(stage_report["missing_animations"])
    missing_graphs = {Path(p).name for p in stage_report["missing_accuracy_graphs"]}
    # BO2 weapon names the mod linker can also find (always-loaded zones, the
    # template, the mod tools' raw weapons): a WaW weapon of such a name would
    # shadow or override BO2's, so it is carried under WAW_WEAPON_PREFIX.
    reserved = {n.lower() for n in reserved_weapons} | {n.lower() for n in (loaded or {}).get("weapon", ())}
    excluded: dict[str, list[str]] = {}
    notes: list[str] = []
    table: dict[str, str] = {}
    replacements: dict[str, str] = {}
    fields_by_weapon: dict[str, dict[str, str]] = {}
    for entry in stage_report["weapons"]:
        name = entry["name"]
        if name.lower() in ENGINE_WEAPONS:
            # both engines' null weapon; BO2 always provides its own
            table[name] = name
            continue
        table[name] = name if name.lower() not in reserved else WAW_WEAPON_PREFIX + name
        if table[name] != name:
            notes.append(f"WEAPON_RENAMED {name} -> {table[name]}: BO2 has a weapon of that name; scripts reach "
                         f"it through level.waw2bo2_weapons")
        fields = read_info((project / "weapons" / name).read_text(encoding="utf-8"))
        replacement = primary_frag_replacement(name, fields)
        if replacement:
            table[name] = replacements[name] = replacement
            notes.append(f"BO2_PRIMARY_FRAG {name} -> {replacement}: ordinary lethal replacement; "
                         "special grenades retain their source weapons")
            continue
        reasons = []
        if table[name].lower() in reserved:
            reasons.append(f"BO2 already has weapons named {name} and {table[name]}")
        for key, value in list(fields.items()):
            kind = next((k for k, p in PREFIXES.items() if value.startswith(p)), None)
            waw = value.removeprefix(PREFIXES[kind]) if kind else value
            if kind == "xmodel":
                if waw in models:
                    bad = [m for m in models[waw]["materials"] if m in failed_materials]
                    if bad:
                        reasons.append(f"{key} {waw}: materials not converted: {', '.join(bad)}")
                elif waw in unsupported_models:
                    reasons.append(f"{key} {waw}: {unsupported_models[waw]}")
                elif waw in missing_models:
                    found = equivalents.find("xmodel", waw) if equivalents is not None else None
                    if found is not None:
                        try:
                            equivalents.stage(found, project)
                        except (OSError, ValueError) as exc:
                            notes.append(f"{name}: BO2 equivalent {found.name} for {waw} unusable: {exc}")
                            found = None
                    if found is not None:
                        fields[key] = found.name
                        notes.append(f"{name}: " + found.describe("xmodel", waw, f"{key} model absent from WaW"))
                    elif VARIANT_MODEL_SLOT.match(key):
                        fields[key] = ""
                        notes.append(f"WAW_ASSET_ABSENT {name}: {key} {waw} cleared (no WaW model, no BO2 "
                                     f"equivalent; WaW showed its default model)")
                    else:
                        reasons.append(f"{key} {waw}: model absent from WaW and no BO2 equivalent")
            elif kind == "material" and waw in failed_materials:
                fields[key] = ""
                notes.append(f"UNSUPPORTED_MATERIAL {name}: {key} {waw} cleared: {failed_materials[waw][0]}")
            elif kind == "xanim" and waw in missing_anims:
                fields[key] = ""
                notes.append(f"WAW_ASSET_ABSENT {name}: {key} animation {waw} cleared")
            elif kind == "accuracy" and waw in missing_graphs:
                fields[key] = ""
                notes.append(f"WAW_ASSET_ABSENT {name}: {key} accuracy graph {waw} cleared")
            elif value.startswith("waw/") and t6_types.get(key) == "CSPFT_FX" and \
                    value.removeprefix("waw/") not in converted_fx:
                fields[key] = ""
                notes.append(f"UNSUPPORTED_FX {name}: {key} {value.removeprefix('waw/')} not converted, cleared")
        if reasons:
            excluded[name] = reasons
        fields_by_weapon[name] = fields
    # a weapon is unusable without its alternate (and the alternate's own)
    changed = True
    while changed:
        changed = False
        for name, fields in fields_by_weapon.items():
            alt = fields.get("altWeapon")
            carried = alt and (alt.lower() in ENGINE_WEAPONS or alt in replacements
                               or (alt in fields_by_weapon and alt not in excluded))
            if name not in excluded and alt and not carried:
                excluded[name] = [f"alternate weapon {alt} is not carried"]
                changed = True
    runtime = sorted(n for n in fields_by_weapon if n not in excluded)
    # content_source is on mod.ff's asset search path: an uncarried WaW weapon
    # file would still be found when a BO2 weapon of the same name is loaded
    parked = project / "weapons_not_carried"
    for entry in stage_report["weapons"]:
        name = entry["name"]
        if name not in runtime and (project / "weapons" / name).is_file():
            parked.mkdir(exist_ok=True)
            (project / "weapons" / name).replace(parked / name)
    images, anims, effects = set(), set(), set()
    material_images = visuals["material_images"]
    for name in runtime:
        fields = fields_by_weapon[name]
        # Stock BO2 weapon files write all 1,027 fields; an absent string or
        # anim-name field stays a NULL pointer, which T6 dereferences (measured
        # crash: fireIntroAnim, anim index 3, a T6-only field WaW lacks). The
        # linker turns "" into a valid empty string, as for stock weapons.
        for key, kind in t6_types.items():
            if kind in POINTER_STRING_FIELDS and key not in fields:
                fields[key] = ""
        if fields.get("altWeapon") in table:
            fields["altWeapon"] = table[fields["altWeapon"]]
        if table[name] != name:
            (project / "weapons" / name).unlink()
        (project / "weapons" / table[name]).write_text(write_info(fields), encoding="utf-8")
        for key, value in fields.items():
            if value.startswith(PREFIXES["xanim"]):
                anims.add(value)
            elif value.startswith("waw/") and t6_types.get(key) == "CSPFT_FX":
                effects.add(value)
            elif value.startswith(PREFIXES["material"]):
                images.update(material_images.get(value, ()))
            elif value.startswith(PREFIXES["xmodel"]):
                for material in models[value.removeprefix(PREFIXES["xmodel"])]["materials"]:
                    images.update(material_images.get(output_name("material", material), ()))
    lines = [f">level.ipak_read,{ipak}", f">ipak,{ipak}"] if images else []
    lines += [f"image,{n}" for n in sorted(images)]
    lines += [f"xanim,{n}" for n in sorted(anims)]
    # converted effects live in the map zone; mod.ff only references them
    lines += [f"fx,,{n}" for n in sorted(effects)]
    lines += [f"weapon,{table[n]}" for n in runtime]
    lines += [f"weapon,{n}" for n in sorted(set(replacements.values()))]
    # every WaW weapon: its BO2 name, or "" when mod.ff does not carry it
    table = {w: (b if w in runtime or w in replacements or w.lower() in ENGINE_WEAPONS else "")
             for w, b in table.items()}
    result = {"status": "runtime_zone_lines_not_native_validated", "ipak": ipak if images else None,
              "weapons": runtime, "table": table, "replacements": replacements,
              "excluded": excluded, "notes": notes, "zone_lines": lines,
              "images": len(images), "xanims": len(anims), "fx_references": sorted(effects)}
    (project / "weapons.runtime.json").write_text(json.dumps(result, indent=2) + chr(10), encoding="utf-8")
    return result
