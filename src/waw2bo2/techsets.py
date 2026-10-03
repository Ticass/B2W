"""WaW (T4) -> Black Ops II (T6) technique-set selection.

Both games encode world-material features in the technique-set name, e.g.

    T4  wc_l_sm_r0c0n0s0            T6  wpc_lit_sm_r0c0n0s0_1zzj1138
    T4  l_sm_r0c0_b1c1   (layered)  T6  lit_sm_r0c0_b1c1n1x1

Each ``<blend><layer>`` token is one texture layer: blend ``r`` replace,
``b`` blend, ``t`` alpha-test, ``m`` multiply, ``a`` add; followed by the
per-layer maps ``c`` colour, ``n`` normal, ``s`` specular, ``d`` detail, and on
T6 also ``x``/``v``/``o`` shader variants that take no extra texture.

Instead of inventing T6 material state, every translated material is cloned
from a *donor*: a stock BO2 material (dumped from ``zm_nuked.ff``) that already
uses the selected T6 technique set. The donor supplies the technique set name,
state bits, sort key, camera region and sampler layout; the WaW material
supplies the images. That keeps the result consistent with what the T6
renderer expects for that technique.
"""
from __future__ import annotations

import copy
import json
import re
from dataclasses import dataclass, field
from pathlib import Path

LAYER_RE = re.compile(r"^([rbtam])(\d)((?:[a-z]\d)*)$")
FEATURE_RE = re.compile(r"([a-z])\d")
HASH_RE = re.compile(r"^[0-9a-z]{8}$")

# Maps whose absence in the source can be filled with a neutral code image.
FILLABLE = {"n": "normalMap"}
NEUTRAL_IMAGE = {"normalMap": "$identitynormalmap"}
# Shader variants that do not add texture slots in T6 (observed in zm_nuked).
TEXTURELESS_EXTRAS = {"x": 2, "v": 4}

UNLIT_MAP = {
    "unlit_blend": ("unlit_blend", []),
    "unlit": ("unlit_replace", []),
    "unlit_distfalloff": ("unlit_blend", ["distance falloff dropped"]),
    # WaW tool surfaces that reach the render world are shadow casters
    # (e.g. wc/caulk_shadow); T6 has a dedicated technique for that.
    "tools": ("shadowcaster", ["tools technique mapped to shadowcaster"]),
}


class TechsetError(ValueError):
    pass


@dataclass
class Techset:
    raw: str
    family: str  # "world", "layered", "model", "other"
    lit: bool
    unlit_kind: str | None
    layers: list[tuple[str, int, frozenset[str]]]
    modifiers: list[str] = field(default_factory=list)


def parse(name: str) -> Techset:
    raw = name
    family = "other"
    # mlv_ (static-model vertex-lit) is kept apart from mc_: the bridge cannot
    # bake per-vertex static model lighting, so only light-grid mc_ is a target.
    for prefix, fam in (("wpc_", "world"), ("wc_", "world"), ("mlv_", "model_vertex"), ("mc_", "model")):
        if name.startswith(prefix):
            family = fam
            name = name[len(prefix):]
            break
    tokens = name.split("_")
    if tokens and HASH_RE.match(tokens[-1]) and not LAYER_RE.match(tokens[-1]):
        tokens = tokens[:-1]  # T6 per-techset hash suffix
    lit = False
    if len(tokens) >= 2 and tokens[0] in ("l", "lit") and tokens[1] in ("sm", "hsm"):
        lit = True
        tokens = tokens[2:]
        if family == "other":
            family = "layered"
    if not lit:
        return Techset(raw, family, False, "_".join(tokens), [], [])
    layers, modifiers = [], []
    for tok in tokens:
        m = LAYER_RE.match(tok)
        if not m:
            modifiers.append(tok)
            continue
        feats = frozenset(FEATURE_RE.findall(m.group(3)))
        layers.append((m.group(1), int(m.group(2)), feats))
    return Techset(raw, family, True, None, layers, modifiers)


@dataclass
class Match:
    source: str
    target: str
    cost: int
    notes: list[str]


def _layer_cost(src, dst, notes: list[str]) -> int | None:
    sblend, sidx, sfeat = src
    dblend, _didx, dfeat = dst
    cost = 0
    if sblend != dblend:
        # alpha-test/add have no layered T6 equivalent in zm_nuked; blend is
        # the closest (keeps the alpha channel meaningful).
        if {sblend, dblend} <= {"t", "b", "a"}:
            cost += 6
            notes.append(f"layer {sidx}: blend '{sblend}' -> '{dblend}'")
        else:
            return None
    if "c" not in dfeat and "c" in sfeat:
        return None
    for f in sorted(sfeat - dfeat):
        if f == "d":
            cost += 3
            notes.append(f"layer {sidx}: detail map dropped")
        elif f == "n":
            cost += 12
            notes.append(f"layer {sidx}: normal map dropped")
        elif f == "s":
            cost += 10
            notes.append(f"layer {sidx}: specular map dropped")
        elif f == "c":
            return None
        else:
            cost += 5
            notes.append(f"layer {sidx}: '{f}' dropped")
    for f in sorted(dfeat - sfeat):
        if f in FILLABLE:
            cost += 1
        elif f in TEXTURELESS_EXTRAS:
            cost += TEXTURELESS_EXTRAS[f]
        else:
            return None  # would need a texture we do not have (s, d, o...)
    return cost


# Layered world techniques need T6 vd1 layer data; stage_geometry enables
# them when the render dump carries the WaW vertex layer buffer (dump v5).
LAYERED_VERTEX_DATA = False

# Self-illuminated model techsets with a native T6 model counterpart. Lighting
# them by the grid darkens bulbs, signs and glows that WaW draws unlit.
MODEL_UNLIT_MAP = {
    "unlit": "unlit_replace",
    "unlit_blend": "unlit_blend",
    "unlit_add": "unlit_add",
    "objective": "objective",
}
# Model techsets with no T6 equivalent in the donor zone. The replacement
# keeps the colour map and is lit by the light grid instead.
MODEL_FALLBACK = {
    "unlit": ("l_sm_r0c0", "unlit model technique lit by the light grid"),
    "sky_noncubemap": ("l_sm_r0c0", "non-cubemap sky model lit by the light grid"),
    "objective": ("l_sm_r0c0", "objective technique lit by the light grid"),
    "cooktorrance_sm": ("l_sm_r0c0n0s0", "cook-torrance approximated by lit_sm"),
    "projected_decal_unlit": ("l_sm_t0c0", "projected decal drawn as alpha-tested model"),
    "ambient_t0c0_sco": ("l_sm_t0c0", "ambient-only technique lit by the light grid"),
}


def _match_lit(src: Techset, source: str, candidates: list[str], want_family: str) -> Match | None:
    best: Match | None = None
    for cand in candidates:
        dst = parse(cand)
        if not dst.lit or dst.family != want_family or dst.modifiers:
            continue
        if len(dst.layers) != len(src.layers):
            continue
        notes = [f"modifier '{m}' dropped" for m in src.modifiers]
        cost = 2 * len(src.modifiers)
        ok = True
        for s, d in zip(src.layers, dst.layers):
            c = _layer_cost(s, d, notes)
            if c is None:
                ok = False
                break
            cost += c
        if ok and (best is None or (cost, cand) < (best.cost, best.target)):
            best = Match(source, cand, cost, notes)
    return best


def match(source: str, candidates: list[str]) -> Match:
    src = parse(source)
    # Measured on the same reticle_side_small asset in T4 and T6: T4's 2d
    # is the single-color-map, alpha-blended HUD pass called trivial in T6.
    # Keep this apart from world unlit shaders (different draw interfaces).
    if src.family == "other" and src.unlit_kind == "2d":
        pool = [c for c in candidates if (p := parse(c)).family == "other" and p.unlit_kind == "trivial"]
        if not pool:
            raise TechsetError(f"no stock T6 HUD trivial technique set available for '{source}'")
        return Match(source, sorted(pool)[0], 0, ["T4 2d -> T6 trivial HUD pass (same-asset reticle measurement)"])
    if not src.lit and src.family == "model":
        kind = MODEL_UNLIT_MAP.get(src.unlit_kind)
        pool = [c for c in candidates if kind and (p := parse(c)).family == "model" and not p.lit and p.unlit_kind == kind]
        if pool:
            return Match(source, sorted(pool)[0], 0, [])
        if src.unlit_kind not in MODEL_FALLBACK:
            raise TechsetError(f"no T6 mapping rule for model technique set '{source}'")
        replacement, note = MODEL_FALLBACK[src.unlit_kind]
        m = match("mc_" + replacement, candidates)
        return Match(source, m.target, m.cost + 20, [note] + m.notes)
    if not src.lit:
        if src.unlit_kind not in UNLIT_MAP:
            raise TechsetError(f"no T6 mapping rule for unlit technique set '{source}'")
        kind, notes = UNLIT_MAP[src.unlit_kind]
        pool = [c for c in candidates if (p := parse(c)).family == "world" and not p.lit and p.unlit_kind == kind]
        if not pool:
            raise TechsetError(f"no stock T6 '{kind}' technique set available for '{source}'")
        return Match(source, sorted(pool)[0], 0 if not notes else 5, list(notes))

    if src.family == "layered" and not LAYERED_VERTEX_DATA:
        # T6 layered techniques read per-vertex layer data from the world's
        # second vertex stream (vd1). Until the WaW layer data is converted,
        # a layered surface would read past vd1 (invisible surface, GPU
        # crash); draw its base layer with the single-layer world technique.
        base = Techset(src.raw, "world", True, None, src.layers[:1], src.modifiers)
        best = _match_lit(base, source, candidates, "world")
        if best is None:
            raise TechsetError(f"no single-layer T6 world technique set for the base layer of '{source}'")
        dropped = [f"layer {l[1]} dropped (vertex layer data not converted)" for l in src.layers[1:]]
        return Match(source, best.target, best.cost + 15 * len(dropped), dropped + best.notes)

    want_family = {"world": "world", "model": "model", "model_vertex": "model"}.get(src.family, "layered")
    best = _match_lit(src, source, candidates, want_family)
    # T6 has fewer blend layers than T4 (zm_nuked tops out at three); keep the
    # base layers and drop the rest rather than refusing the material.
    trimmed = src
    while best is None and len(trimmed.layers) > 1:
        trimmed = Techset(trimmed.raw, trimmed.family, True, None, trimmed.layers[:-1], trimmed.modifiers)
        best = _match_lit(trimmed, source, candidates, want_family)
        if best is not None:
            gone = [f"layer {l[1]} dropped (no {len(src.layers)}-layer T6 technique)"
                    for l in src.layers[len(trimmed.layers):]]
            best = Match(source, best.target, best.cost + 15 * len(gone), gone + best.notes)
    if best is None:
        raise TechsetError(f"no compatible stock T6 technique set for '{source}'")
    return best


def world_vert_format(techset_name: str) -> int:
    """MaterialWorldVertexFormat of a WaW world technique set: TEX_<t>_NRM_<n>
    with t = layer count and n = layers with a normal map (at least 1, at most
    3). Verified against the dumped worldVertFormat of 638 materials."""
    parsed = parse(techset_name)
    if not parsed.layers:
        return 0
    texcoords = len(parsed.layers)
    normals = min(3, max(1, sum("n" in feats for _, _, feats in parsed.layers)))
    pairs = [(t, n) for t in range(1, 6) for n in range(1, min(t, 3) + 1)]
    return pairs.index((min(texcoords, 5), min(normals, texcoords)))


# ---------------------------------------------------------------------------
# Donor materials


ANIMATED_DONOR_RE = re.compile(r"moving|scroll|flow|pulse|spin|rotat|flicker|_pan|uvanim|water|lava", re.I)


def index_donors(stock_material_dir: Path) -> dict[str, Path]:
    """techniqueSet name -> one stock T6 material JSON that uses it."""
    # Several materials can share a technique set with different extra slots
    # (e.g. weapon camo detail maps); prefer the one needing the fewest textures.
    best: dict[str, tuple[tuple, Path]] = {}
    animated_techsets: set[str] = set()
    static_techsets: set[str] = set()
    order = {"wpc": 0, "generated": 1}
    for path in stock_material_dir.rglob("*.json"):
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        ts = data.get("techniqueSet")
        if not ts:
            continue
        animated = ANIMATED_DONOR_RE.search(path.stem) is not None or ANIMATED_DONOR_RE.search(ts) is not None
        if animated:
            animated_techsets.add(ts)
        else:
            static_techsets.add(ts)
        key = (len(data.get("textures", [])), order.get(path.relative_to(stock_material_dir).parts[0], 9), str(path))
        if ts not in best or key < best[ts][0]:
            best[ts] = (key, path)
    # A technique set only ever used by animated stock materials (scrolling
    # UVs are compiled into the shader, not material constants) would make
    # static source surfaces move.
    return {ts: path for ts, (_, path) in best.items() if not (ts in animated_techsets and ts not in static_techsets)}


NEUTRAL_CONSTANT_PREFIXES = ("colorTint", "occlusionAmount", "scaleRGB")


def as_reference(image: str) -> str:
    """Code images ($...) are resolved by the game, not linked from disk."""
    if image.startswith("$"):
        return "," + image
    return image


def build_material(t4: dict, donor: dict, notes: list[str]) -> dict:
    out = copy.deepcopy(donor)
    src_tex = {t["name"]: t for t in t4.get("textures", []) if t.get("name") and t.get("image")}
    used = set()
    textures = []
    for slot in donor.get("textures", []):
        name = slot.get("name")
        new = copy.deepcopy(slot)
        if name in src_tex:
            src = src_tex[name]
            new["image"] = as_reference(src["image"])
            ss = src.get("samplerState") or {}
            for key in ("clampU", "clampV", "clampW", "filter", "mipMap"):
                if key in ss:
                    new.setdefault("samplerState", {})[key] = ss[key]
            used.add(name)
        elif slot.get("semantic") in NEUTRAL_IMAGE:
            new["image"] = as_reference(NEUTRAL_IMAGE[slot["semantic"]])
        else:
            raise TechsetError(f"donor slot '{name}' ({slot.get('semantic')}) has no source texture")
        textures.append(new)
    dropped = [f"{t.get('name') or t.get('semantic')}:{t['image']}" for t in t4.get("textures", [])
               if t.get("image") and t.get("name") not in used
               and not (t.get("semantic") == "normalMap" and t["image"] == "$identitynormalmap")]
    if dropped:
        notes.append("source textures without a T6 slot: " + ", ".join(dropped))
    out["textures"] = textures

    src_consts = {c["name"]: c for c in t4.get("constants", []) if c.get("name")}
    consts = []
    for c in donor.get("constants", []):
        c = copy.deepcopy(c)
        name = c.get("name", "")
        if name in src_consts:
            c["literal"] = list(src_consts[name]["literal"])
        elif name.startswith(NEUTRAL_CONSTANT_PREFIXES):
            c["literal"] = [1.0, 1.0, 1.0, 1.0]
        consts.append(c)
    out["constants"] = consts
    source = parse(t4.get('techniqueSet', ''))
    if source.family == 'world' and source.unlit_kind in ('unlit', 'unlit_blend', 'unlit_distfalloff'):
        entries = t4.get('stateBitsEntry', [])
        index = entries[4] if len(entries) > 4 else -1
        states = t4.get('stateBits', [])
        if 0 <= index < len(states):
            main = states[index]
            # Preserve the source decal's blend/depth behavior while retaining
            # T6-only stencil and pass routing from the matching native donor.
            fields = ('srcBlendRgb', 'dstBlendRgb', 'blendOpRgb', 'srcBlendAlpha',
                      'dstBlendAlpha', 'blendOpAlpha', 'alphaTest', 'depthWrite',
                      'depthTest', 'cullFace', 'polygonOffset', 'colorWriteAlpha', 'colorWriteRgb')
            for state in out.get('stateBits', []):
                if state.get('colorWriteRgb') and not state.get('polymodeLine'):
                    state.update({k: main[k] for k in fields if k in main})
            if 'sortKey' in t4:
                out['sortKey'] = t4['sortKey']
    out["_game"] = "t6"
    out["_type"] = "material"
    return out


def material_techset(t4: dict) -> str:
    """Unlit shader names alone do not encode a material's custom blending."""
    name = t4.get('techniqueSet', '')
    parsed = parse(name)
    entries, states = t4.get('stateBitsEntry', []), t4.get('stateBits', [])
    index = entries[4] if len(entries) > 4 else -1
    if parsed.family in ('world', 'model') and parsed.unlit_kind == 'unlit' and 0 <= index < len(states):
        state = states[index]
        if state.get('blendOpRgb', 'disabled') != 'disabled':
            # T6 has a separate additive model technique; world keeps blend
            # and receives the source state in build_material.
            if parsed.family == 'model' and state.get('dstBlendRgb') == 'one':
                return name + '_add'
            return name + '_blend'
    return name


def oat_material_path(name: str) -> Path:
    """Exact mirror of OAT material::GetFileNameForAssetName."""
    if name.startswith("*"):
        s = name.replace("*", "_")
        if "(" in s:
            s = s[: s.index("(")]
        return Path("materials") / "generated" / f"{s}.json"
    parts = name.split("/")
    return Path("materials", *parts[:-1], parts[-1] + ".json")


def oat_image_path(name: str, ext: str = ".iwi") -> Path:
    """Exact mirror of OAT image::GetFileNameForAsset."""
    return Path("images") / (name.replace("*", "_") + ext)
