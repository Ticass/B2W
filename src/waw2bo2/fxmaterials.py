"""WaW effect materials -> BO2 effect materials.

WaW names its effect technique sets by feature (``effect_zfeather_add_nofog``,
``particle_cloud_outdoor``, ``distortion_scale_zfeather``); BO2's are hashed
(``effect_w77q49e8``). The features of a BO2 technique set are read from the
mod tools' own sources: ``raw/techsets/<ts>.techset`` names its techniques and
``raw/techniques/<tech>.tech`` lists the shader inputs, e.g.
``material.featherParms`` (depth feathering), ``material.falloffParms``
(view-angle falloff), ``material.eyeOffsetParms``, ``sampler.outdoor``.

As for world materials (techsets.py), the BO2 material is cloned from a stock
*donor* using the chosen technique set; the WaW material supplies images,
constants, texture atlas layout and its blend state.
"""
from __future__ import annotations

import copy
import re
from dataclasses import dataclass
from pathlib import Path

from . import techsets

EFFECT_FAMILIES = ("effect", "particle_cloud", "distortion")
T6_FAMILY_PREFIXES = ("effect_", "particlecloud_", "distortion_")

# features the matcher must honour; anything else on a donor disqualifies it
FEATURES = {"zfeather", "falloff", "eyeoffset", "outdoor", "distortion", "particlecloud", "dissolve",
            "spotlight", "add"}
# WaW features BO2 can drop with a reported note (no T6 technique variant exists)
DROPPABLE = {
    "nofog": "nofog (BO2 effect techniques always fog)",
    "zfeather": "depth feathering (no feathered BO2 variant for this family)",
}

BLEND_FIELDS = ("srcBlendRgb", "dstBlendRgb", "blendOpRgb", "srcBlendAlpha", "dstBlendAlpha", "blendOpAlpha",
                "alphaTest", "cullFace", "colorWriteRgb", "colorWriteAlpha")


def is_effect_techset(t4_techset: str) -> bool:
    return t4_techset.startswith(EFFECT_FAMILIES)


def waw_features(t4_techset: str) -> tuple[set[str], set[str]]:
    """(required features, droppable features) of a WaW effect technique set."""
    tokens = set(t4_techset.split("_"))
    feats: set[str] = set()
    extra: set[str] = set()
    if t4_techset.startswith("particle_cloud"):
        feats.add("particlecloud")
    elif t4_techset.startswith("distortion"):
        feats.add("distortion")
    for t in ("zfeather", "falloff", "eyeoffset", "outdoor", "add"):
        if t in tokens:
            feats.add(t)
    if "nofog" in tokens:
        extra.add("nofog")
    unknown = tokens - {"effect", "particle", "cloud", "distortion", "scale", "zfeather", "falloff", "eyeoffset",
                        "outdoor", "add", "nofog", "blend"}
    if unknown:
        raise techsets.TechsetError(f"unknown WaW effect technique tokens {sorted(unknown)} in '{t4_techset}'")
    return feats, extra


def t6_features(bo2_raw: Path, techset: str) -> set[str] | None:
    path = bo2_raw / "techsets" / f"{techset}.techset"
    if not path.exists():
        return None
    text = path.read_text(encoding="utf-8", errors="replace")
    feats: set[str] = set()
    for tech in re.findall(r"(pimp_technique_[a-z0-9_]+);", text):
        if "wireframe" in tech or "debugperformance" in tech:
            continue
        if "effectadd" in tech:
            feats.add("add")
        src_path = bo2_raw / "techniques" / f"{tech}.tech"
        if not src_path.exists():
            return None
        src = src_path.read_text(encoding="utf-8", errors="replace")
        args = set(re.findall(r"(?:material|code|sampler)\.(\w+)", src))
        shaders = set(re.findall(r'Shader [\d.]+ "pimp_shader_([a-z_]+?)_[0-9a-f]+\.hlsl"', src))
        if "featherParms" in args:
            feats.add("zfeather")
        if "falloffParms" in args:
            feats.add("falloff")
        if "eyeOffsetParms" in args:
            feats.add("eyeoffset")
        if "outdoor" in args:
            feats.add("outdoor")
        if "alphaDissolveParms" in args:
            feats.add("dissolve")
        if "spotLightWeight" in args:
            feats.add("spotlight")
        if "distortion" in shaders:
            feats.add("distortion")
        if "particlecloud" in shaders:
            feats.add("particlecloud")
    return feats


def _main_state(material: dict) -> dict | None:
    """The first state that draws normally (not the wireframe/debug state)."""
    for s in material.get("stateBits", []):
        if not s.get("polymodeLine") and s.get("depthTest") != "disabled":
            return s
    return None


def blend_class(state: dict | None) -> str:
    if not state or state.get("blendOpRgb") == "disabled":
        return "opaque"
    src, dst = state.get("srcBlendRgb"), state.get("dstBlendRgb")
    if dst == "one":
        return "add"
    if dst == "invsrcalpha":
        return "premult" if src == "one" else "blend"
    if src in ("destcolor", "zero") or dst == "srccolor":
        return "multiply"
    return f"{src}/{dst}"


@dataclass
class Donor:
    techset: str
    path: Path
    material: dict
    features: set[str]
    blend: str


def index_effect_donors(stock_materials: Path, techset_dump: Path | None, bo2_raw: Path) -> list[Donor]:
    """Every stock BO2 effect material usable as a donor (its technique set is dumped)."""
    import json

    donors: list[Donor] = []
    feats_cache: dict[str, set[str] | None] = {}
    for path in sorted(stock_materials.rglob("*.json")):
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        ts = data.get("techniqueSet", "")
        if not ts.startswith(T6_FAMILY_PREFIXES):
            continue
        if techset_dump is not None and not (techset_dump / "techniquesets" / f"{ts}.json").exists():
            continue
        if ts not in feats_cache:
            feats_cache[ts] = t6_features(bo2_raw, ts)
        feats = feats_cache[ts]
        if feats is None:
            continue
        donors.append(Donor(ts, path, data, feats, blend_class(_main_state(data))))
    return donors


def choose_donor(t4: dict, donors: list[Donor]) -> tuple[Donor, list[str]]:
    feats, extra = waw_features(t4.get("techniqueSet", ""))
    want_blend = blend_class(_main_state(t4))
    slots = {t.get("name") for t in t4.get("textures", []) if t.get("image")}
    notes = [f"dropped: {DROPPABLE[f]}" for f in sorted(extra)]

    def usable(d: Donor, required: set[str]) -> bool:
        if d.features & (FEATURES - {"add"}) != required - {"add"}:
            return False
        donor_slots = {t.get("name") for t in d.material.get("textures", [])}
        return donor_slots <= slots | {None}

    for required, note in ((feats, None), (feats - {"zfeather"}, DROPPABLE["zfeather"])):
        pool = [d for d in donors if usable(d, required)]
        if not pool:
            continue
        # same blend class first (keeps the shader's premultiply convention),
        # then the additive technique variant when WaW's is additive
        pool.sort(key=lambda d: (d.blend != want_blend, ("add" in d.features) != ("add" in feats), d.techset,
                                 str(d.path)))
        best = pool[0]
        if note and required != feats:
            notes.append(f"dropped: {note}")
        if best.blend != want_blend:
            notes.append(f"donor blend {best.blend}, WaW blend {want_blend} (WaW blend state applied)")
        return best, notes
    raise techsets.TechsetError(f"no stock BO2 effect technique set with features {sorted(feats)} "
                                f"for '{t4.get('techniqueSet')}'")


def build_effect_material(t4: dict, donor: Donor, notes: list[str]) -> dict:
    out = techsets.build_material(t4, donor.material, notes)
    main = _main_state(t4)
    if main is not None:
        for state in out.get("stateBits", []):
            if state.get("polymodeLine"):
                continue
            for k in BLEND_FIELDS:
                if k in main:
                    state[k] = main[k]
    if "textureAtlas" in t4:
        out["textureAtlas"] = copy.deepcopy(t4["textureAtlas"])
    return out
