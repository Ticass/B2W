"""WaW (T4) -> Black Ops II (T6) effect conversion.

Compiled effect to compiled effect: the T4 OAT unlinker writes every WaW
FxEffectDef as ``fx/<name>.w2bfx.json`` (vendor FxJsonDumperT4), this module
translates it into the T6 layout, and the bridge linker builds the T6
FxEffectDef from it (vendor JsonLoaderFxT6). Both runtimes play effects from
the same sampled data (velocity / visual-state samples, ranges, atlas), so
nothing is re-authored and nothing is replaced by a stock BO2 effect.

The layout differences below were measured, not assumed:

* element flags -- T4 uses the IW3 layout; T6 inserted one bit after the run
  mode. Learned from 2435 stock BO2 elements paired with their ``raw/fx``
  sources (tools/fx/efx_pair.py) and confirmed on 176 WaW elements of effects
  BO2 ships under the same name (every mapped bit agrees).
* element types -- T6 inserted SPRITE_ROTATED; the others shift by one or two.
* colours -- T4 samples are B,G,R,A; T6 samples are R,G,B,A.
* atlas -- T6 packs ``entryCount | indexRange << 9``; BO2's own conversions
  of WaW effects keep indexRange 1.
* effect flags -- T6 derives them from the elements (lit, omni light, model,
  sound, runner, trail); T4 only has "lit".
* sizes -- totalSize is the effect's allocation: T4 36 + 256/elem, T6 76 +
  292/elem, plus samples, visual arrays, trails and strings.
"""
from __future__ import annotations

import json
import struct
from dataclasses import dataclass, field
from pathlib import Path

# ---- element flags -------------------------------------------------------

# T4 bit -> T6 bit (same meaning). Bits 0x2..0xC0 (spawn relative, frustum
# cull, runner random rotation, spawn offset shape, run mode) are identical.
ELEM_FLAG_MAP = {
    0x00000002: 0x00000002,  # spawnRelative
    0x00000004: 0x00000004,  # spawnFrustumCull
    0x00000008: 0x00000008,  # runnerUsesRandRot
    0x00000010: 0x00000010,  # spawnOffsetSphere
    0x00000020: 0x00000020,  # spawnOffsetCylinder
    0x00000040: 0x00000040,  # runRelToSpawn  (0xC0 = runRelToOffset)
    0x00000080: 0x00000080,  # runRelToEffect
    0x00000100: 0x00000200,  # useCollision
    0x00000200: 0x00000400,  # dieOnTouch
    0x00000400: 0x00000800,  # drawPastFog
    0x00000800: 0x00001000,  # drawWithViewModel
    0x00001000: 0x00002000,  # blocksSight
    0x01000000: 0x01000000,  # has local velocity graph
    0x02000000: 0x02000000,  # has world velocity graph
    0x04000000: 0x04000000,  # hasGravity
    0x08000000: 0x08000000,  # useModelPhysics
    0x10000000: 0x10000000,  # nonUniformScale
    0x40000000: 0x40000000,  # hasReflection
    0x80000000: 0x80000000,  # isMatureContent
}

# T4 FxElemType -> T6 FxElemType. WaW's numbering (from the dumped data, not
# the vendored T4 header's enum names): 0 billboard, 1 oriented, 2 tail,
# 3 line, 4 trail (the only type with trail data), 5 cloud, 6 model, 7 omni
# light, 8 spot light, 9 sound, 10 decal, 11 runner. Pairs of same-named
# WaW/BO2 effects agree (3->4 line, 4->5 trail, 5->6 cloud, 7->8 omni light).
T4_SPOT_LIGHT = 8
ELEM_TYPE_MAP = {0: 0, 1: 1, 2: 3, 3: 4, 4: 5, 5: 6, 6: 7, 7: 8, 8: 9, 9: 10, 10: 11, 11: 12}
T6_TYPE_NAMES = ["sprite_billboard", "sprite_oriented", "sprite_rotated", "tail", "line", "trail", "cloud",
                 "model", "omni_light", "spot_light", "sound", "decal", "runner"]
T6_CLOUD, T6_MODEL, T6_OMNI, T6_SPOT, T6_SOUND, T6_DECAL, T6_RUNNER, T6_TRAIL = 6, 7, 8, 9, 10, 11, 12, 5

# T6 effect flags derived from the elements (fitted on 384 stock effects).
EF_LIT, EF_OMNI, EF_MODEL, EF_SOUND, EF_RUNNER, EF_TRAIL = 0x1, 0x10, 0x20, 0x40, 0x80, 0x100
T4_EF_LIT = 0x1

# T6 defaults for fields T4 does not have (the effects editor defaults).
BILLBOARD_TRIM_DEFAULT = (1.0, 1.0)
CLOUD_DENSITY_DEFAULT = (1024, 0)

T4_EFFECT_SIZE, T4_ELEM_SIZE = 36, 256
T6_EFFECT_SIZE, T6_ELEM_SIZE = 76, 292
VEL_SAMPLE_SIZE, VIS_SAMPLE_SIZE = 96, 48

# Converted effects are linked into the map zone. Stock BO2 zones every
# zombies map loads already hold effects with WaW names (e.g.
# misc/fx_zombie_eye_single) whose content differs; a map zone asset of the
# same name is an override error. Every converted effect therefore lives
# under this prefix and the scripts' references are rewritten to it.
OUTPUT_PREFIX = "waw/"


class FxConvertError(ValueError):
    pass


def output_name(waw_name: str) -> str:
    return OUTPUT_PREFIX + waw_name


def _float_bits(x: float) -> int:
    return struct.unpack("<I", struct.pack("<f", x))[0]


def _int_bits(x: int) -> int:
    return x & 0xFFFFFFFF


@dataclass
class Deps:
    effects: set[str] = field(default_factory=set)
    materials: set[str] = field(default_factory=set)
    models: set[str] = field(default_factory=set)
    sounds: set[str] = field(default_factory=set)


def dependencies(t4: dict) -> Deps:
    d = Deps()
    for e in t4["elemDefs"]:
        for k in ("effectOnImpact", "effectOnDeath", "effectEmitted"):
            if e.get(k):
                d.effects.add(e[k])
        for v in e["visuals"]:
            if v.get("effect"):
                d.effects.add(v["effect"])
            if v.get("material"):
                d.materials.add(v["material"])
            for m in v.get("materials", []):
                if m:
                    d.materials.add(m)
            if v.get("model"):
                d.models.add(v["model"])
            if v.get("sound"):
                d.sounds.add(v["sound"])
    return d


def _elem_flags(t4_flags: int, notes: list[str], where: str) -> int:
    out = 0
    rest = t4_flags
    for src, dst in ELEM_FLAG_MAP.items():
        if t4_flags & src:
            out |= dst
            rest &= ~src
    if rest:
        notes.append(f"{where}: unknown T4 element flag bits 0x{rest:08x} dropped")
    return out


def _color(c: list[int]) -> list[int]:
    b, g, r, a = c
    return [r, g, b, a]


def _vis_state(s: dict) -> dict:
    out = dict(s)
    out["color"] = _color(s["color"])
    return out


def convert_elem(e: dict, material_names: dict[str, str], notes: list[str], where: str) -> dict:
    t4_type = e["elemType"]
    if t4_type not in ELEM_TYPE_MAP:
        raise FxConvertError(f"{where}: unknown T4 element type {t4_type}")
    if t4_type == T4_SPOT_LIGHT:
        # T6 spot lights need a GfxLightDef and FxSpotLightDef that T4 data
        # does not contain; converting them is not implemented.
        raise FxConvertError(f"{where}: spot light elements are not converted yet")
    t6_type = ELEM_TYPE_MAP[t4_type]

    out = {k: e[k] for k in (
        "spawnLooping", "spawnOneShot", "spawnRange", "fadeInRange", "fadeOutRange", "spawnFrustumCullRadius",
        "spawnDelayMsec", "lifeSpanMsec", "spawnOrigin", "spawnOffsetRadius", "spawnOffsetHeight", "spawnAngles",
        "angularVelocity", "initialRotation", "gravity", "reflectionFactor", "windInfluence", "visualCount",
        "velIntervalCount", "visStateIntervalCount", "velSamples", "collMins", "collMaxs", "emitDist",
        "emitDistVariance", "sortOrder", "lightingFrac")}
    out["flags"] = _elem_flags(e["flags"], notes, where)
    out["elemType"] = t6_type
    out["rotationAxis"] = 0
    a = e["atlas"]
    count = a["entryCount"]
    out["atlas"] = {
        "behavior": a["behavior"], "index": a["index"], "fps": a["fps"], "loopCount": a["loopCount"],
        "colIndexBits": a["colIndexBits"], "rowIndexBits": a["rowIndexBits"],
        "entryCountAndIndexRange": (count | (1 << 9)) if count else 0,
    }
    out["visSamples"] = [{"base": _vis_state(s["base"]), "amplitude": _vis_state(s["amplitude"])}
                         for s in e["visSamples"]]

    visuals = []
    for v in e["visuals"]:
        if "materials" in v:
            visuals.append({"materials": [material_names.get(m, m) if m else "" for m in v["materials"]]})
        elif "material" in v:
            visuals.append({"material": material_names.get(v["material"], v["material"])})
        elif "effect" in v:
            visuals.append({"effect": output_name(v["effect"]) if v["effect"] else ""})
        elif v.get("sound"):
            # WaW sound aliases are not converted yet. Keep the WaW alias under
            # the converted-asset prefix so BO2 cannot silently play a stock
            # alias that happens to share the name.
            visuals.append({"sound": OUTPUT_PREFIX + v["sound"]})
            notes.append(f"UNSUPPORTED_SOUND {v['sound']} ({where} sound element): WaW sound aliases are not "
                         f"converted yet (silent)")
        else:
            visuals.append(dict(v))
    out["visuals"] = visuals
    for k in ("effectOnImpact", "effectOnDeath", "effectEmitted"):
        out[k] = output_name(e[k]) if e[k] else ""
    out["effectAttached"] = ""
    out["trail"] = e["trail"] if t6_type == T6_TRAIL else None
    if t6_type == T6_TRAIL and not e["trail"]:
        raise FxConvertError(f"{where}: trail element without trail data")
    out["spotLight"] = None
    out["unused"] = [0, 0]
    out["alphaFadeTimeMsec"] = 0
    out["maxWindStrength"] = 0
    out["spawnIntervalAtMaxWind"] = 0
    out["lifespanAtMaxWind"] = 0
    if t6_type == T6_CLOUD:
        out["u"] = [_int_bits(CLOUD_DENSITY_DEFAULT[0]), _int_bits(CLOUD_DENSITY_DEFAULT[1])]
    else:
        out["u"] = [_float_bits(BILLBOARD_TRIM_DEFAULT[0]), _float_bits(BILLBOARD_TRIM_DEFAULT[1])]
    out["spawnSound"] = ""
    out["billboardPivot"] = [0.0, 0.0]
    return out


def _strings(fx: dict) -> int:
    seen = {fx["name"]}
    for e in fx["elemDefs"]:
        for k in ("effectOnImpact", "effectOnDeath", "effectEmitted", "effectAttached", "spawnSound"):
            if e.get(k):
                seen.add(e[k])
        for v in e["visuals"]:
            for k in ("effect", "sound"):
                if v.get(k):
                    seen.add(v[k])
    return sum(len(s) + 1 for s in seen)


def t6_total_size(fx: dict) -> int:
    size = T6_EFFECT_SIZE
    for e in fx["elemDefs"]:
        size += T6_ELEM_SIZE + VEL_SAMPLE_SIZE * len(e["velSamples"]) + VIS_SAMPLE_SIZE * len(e["visSamples"])
        if e["elemType"] == T6_DECAL:
            size += 8 * e["visualCount"]
        elif e["visualCount"] > 1:
            size += 4 * e["visualCount"]
        t = e.get("trail")
        if t:
            size += 28 + 20 * len(t["verts"]) + 2 * len(t["inds"])
        if e.get("spotLight"):
            size += 12
    return size + _strings(fx)


def non_looping_life(fx: dict) -> int:
    """Longest one-shot element life (spawn delay + life span, maxima)."""
    start = fx["elemDefCountLooping"]
    end = start + fx["elemDefCountOneShot"] + fx["elemDefCountEmission"]
    best = 0
    for e in fx["elemDefs"][start:end]:
        best = max(best, sum(e["spawnDelayMsec"]) + sum(e["lifeSpanMsec"]))
    return best


def convert_effect(t4: dict, material_names: dict[str, str] | None = None) -> tuple[dict, list[str]]:
    """T4 w2bfx JSON -> (T6 w2bfx JSON, notes). Raises FxConvertError."""
    if t4.get("_type") != "waw2bo2_fx" or t4.get("_game") != "T4":
        raise FxConvertError(f"{t4.get('name')}: not a T4 waw2bo2_fx dump")
    notes: list[str] = []
    material_names = material_names or {}
    name = t4["name"]
    elems = [convert_elem(e, material_names, notes, f"{name} elem {i}") for i, e in enumerate(t4["elemDefs"])]
    flags = 0
    if t4["flags"] & T4_EF_LIT or any(e["lightingFrac"] > 0 for e in elems):
        flags |= EF_LIT
    for e in elems:
        flags |= {T6_OMNI: EF_OMNI, T6_MODEL: EF_MODEL, T6_SOUND: EF_SOUND, T6_RUNNER: EF_RUNNER,
                  T6_TRAIL: EF_TRAIL}.get(e["elemType"], 0)
    if t4["flags"] & ~T4_EF_LIT:
        notes.append(f"{name}: unknown T4 effect flag bits 0x{t4['flags'] & ~T4_EF_LIT:x} dropped")
    out = {
        "_type": "waw2bo2_fx", "_game": "T6", "_version": 1, "_source": {"game": "T4", "name": name},
        "name": output_name(name),
        "flags": flags,
        "efPriority": t4["efPriority"],
        "elemDefCountLooping": t4["elemDefCountLooping"],
        "elemDefCountOneShot": t4["elemDefCountOneShot"],
        "elemDefCountEmission": t4["elemDefCountEmission"],
        "totalSize": 0,
        "msecLoopingLife": t4["msecLoopingLife"],
        "msecNonLoopingLife": 0,
        "boundingBoxDim": [0.0, 0.0, 0.0],
        "boundingBoxCentre": [0.0, 0.0, 0.0],
        "occlusionQueryDepthBias": 0.0,
        "occlusionQueryFadeIn": 0,
        "occlusionQueryFadeOut": 0,
        "occlusionQueryScaleRange": [0.0, 0.0],
        "elemDefs": elems,
    }
    out["msecNonLoopingLife"] = non_looping_life(out)
    out["totalSize"] = t6_total_size(out)
    return out, notes


def fx_file(root: Path, name: str) -> Path:
    return root / "fx" / f"{name}.w2bfx.json"


def find_source(roots: list[Path], name: str) -> Path | None:
    for r in roots:
        p = fx_file(r, name)
        if p.exists():
            return p
    return None


@dataclass
class Closure:
    effects: dict[str, Path] = field(default_factory=dict)  # WaW name -> dump file
    absent: dict[str, str] = field(default_factory=dict)  # WaW name -> referenced by
    deps: Deps = field(default_factory=Deps)


def closure(names: list[str], roots: list[Path], resolve=None) -> Closure:
    """Every effect the scripts load plus everything those effects reference.
    ``resolve(name) -> Path | None`` is asked for effects no root defines."""
    c = Closure()
    stack = [(n, "script") for n in sorted(set(names))]
    while stack:
        name, parent = stack.pop()
        if name in c.effects or name in c.absent:
            continue
        src = find_source(roots, name)
        if src is None and resolve is not None:
            src = resolve(name)
        if src is None:
            c.absent[name] = parent
            continue
        c.effects[name] = src
        d = dependencies(json.loads(src.read_text(encoding="utf-8")))
        c.deps.materials |= d.materials
        c.deps.models |= d.models
        c.deps.sounds |= d.sounds
        stack += [(e, name) for e in sorted(d.effects)]
    return c
