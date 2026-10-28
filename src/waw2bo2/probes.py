"""WaW baked reflection probes -> T6 gfxworld reflection probes.

The T4 dumper writes ``<bsp>.reflectionprobes.json``: every probe's origin and
cube image, the probes each WaW cell offers dynamic models, and each static
model's probe (surfaces carry theirs in the ``.gfx.bin``).

WaW probe 0 is an unbaked placeholder (solid red); it stays the bridge's black
neutral probe. A T6 world addresses at most ``T6_MAX_PROBES`` probes: the draw
surface sort key holds the index in 5 bits (GfxDrawSurfFields), so a map with
more keeps the probes that cover its geometry best and every dropped probe is
drawn with its nearest kept one.

WaW programs read the probe as rgb * a in display units
(ps lp_*: ``texldl r2, s1; mul r2.xyz, r2.w, r2``). The staged cube holds that
product with alpha 1; translated programs read it unchanged
(shaderruntime: reflectionProbeSampler).
"""
from __future__ import annotations

import json
import math
from dataclasses import dataclass
from pathlib import Path

from . import iwi, techsets

T6_MAX_PROBES = 32
NEUTRAL = 0


@dataclass
class SourceProbes:
    origins: list[tuple[float, float, float]]
    images: list[str]
    cells: list[list[int]]
    smodel_probes: list[int]


@dataclass
class Plan:
    kept: list[int]  # source probe index of each T6 probe; kept[0] is the neutral probe
    remap: dict[int, int]  # source probe index -> T6 probe index

    def t6_index(self, source: int) -> int:
        return self.remap.get(source, NEUTRAL)


def read(path: Path) -> SourceProbes | None:
    if not path.is_file():
        return None
    data = json.loads(path.read_text(encoding="utf-8"))
    probes = data["probes"]
    return SourceProbes([tuple(p["origin"]) for p in probes], [p["image"].removeprefix(",") for p in probes],
                        [list(c["probes"]) for c in data.get("cells", [])], list(data.get("smodelProbes", [])))


def _distance(a, b) -> float:
    return math.dist(a, b)


def plan(source: SourceProbes, usage: dict[int, int], baked: set[int], limit: int = T6_MAX_PROBES) -> Plan:
    """Choose the T6 probes. ``usage``: source probe -> surfaces and static
    models drawn with it; ``baked``: source probes whose cube has pixel data.

    Within the limit every baked probe is kept in source order. Over it, a
    usage-weighted k-medoids over probe origins (seeded by weighted farthest
    point, deterministic) keeps ``limit - 1`` probes."""
    candidates = sorted(i for i in baked if i != NEUTRAL and i < len(source.origins))
    room = limit - 1
    if len(candidates) <= room:
        kept = candidates
    else:
        weight = {i: max(1, usage.get(i, 0)) for i in candidates}
        origin = source.origins
        kept = [max(candidates, key=lambda i: (weight[i], -i))]
        while len(kept) < room:
            kept.append(max((i for i in candidates if i not in kept),
                            key=lambda i: (weight[i] * min(_distance(origin[i], origin[k]) for k in kept), -i)))
        for _ in range(20):
            groups = {k: [] for k in kept}
            for i in candidates:
                groups[min(kept, key=lambda k: (_distance(origin[i], origin[k]), k))].append(i)
            moved = sorted(min(members, key=lambda m: (sum(weight[i] * _distance(origin[i], origin[m])
                                                            for i in members), m))
                           for members in groups.values())
            if moved == sorted(kept):
                break
            kept = moved
        kept = sorted(kept)
    remap = {NEUTRAL: NEUTRAL}
    remap.update({source_index: t6 for t6, source_index in enumerate(kept, start=1)})
    for i in candidates:
        if i not in remap:
            nearest = min(kept, key=lambda k: (_distance(source.origins[i], source.origins[k]), k))
            remap[i] = remap[nearest]
    return Plan([NEUTRAL, *kept], remap)


def _display_texels(level: bytes) -> bytes:
    """RGBA texels -> (rgb * a, 1): the value WaW programs use."""
    out = bytearray(level)
    for p in range(0, len(out), 4):
        a = out[p + 3]
        out[p] = (out[p] * a + 127) // 255
        out[p + 1] = (out[p + 1] * a + 127) // 255
        out[p + 2] = (out[p + 2] * a + 127) // 255
        out[p + 3] = 255
    return bytes(out)


def image_name(t6_index: int) -> str:
    return f"reflection_probe{t6_index}"


def stage(report, world, source: SourceProbes | None, image_roots: list[Path], project_root: Path) -> Plan | None:
    """Give every surface and static model its T6 probe, write the probe cubes
    and BSP/reflectionprobes.json. Without a probe dump the world keeps the
    single neutral probe."""
    if source is None:
        report.warnings.append("reflection probes: the render dump has none (re-extract gfxworld with the "
                               "reflection-probe T4 dumper); specular reflections use the black neutral probe")
        return None
    if source.smodel_probes and len(source.smodel_probes) != len(world.static_models):
        report.errors.append(f"reflection probes: {len(source.smodel_probes)} static model probes for "
                             f"{len(world.static_models)} static models")
        return None

    def find(stem):
        return next((r / f"{stem}.dds" for r in image_roots if (r / f"{stem}.dds").is_file()), None)

    files = {i: find(name.replace("*", "_")) for i, name in enumerate(source.images) if i != NEUTRAL and name}
    missing = sorted(i for i, f in files.items() if f is None)
    if missing:
        report.errors.append(f"reflection probes {missing}: cube images missing from every image root")
    usage: dict[int, int] = {}
    for surface in world.surfaces:
        usage[surface.reflection_probe_index] = usage.get(surface.reflection_probe_index, 0) + 1
    for index in source.smodel_probes:
        usage[index] = usage.get(index, 0) + 1
    result = plan(source, usage, {i for i, f in files.items() if f is not None})
    for surface in world.surfaces:
        surface.reflection_probe_index = result.t6_index(surface.reflection_probe_index)
    for model, index in zip(world.static_models, source.smodel_probes):
        model.reflection_probe_index = result.t6_index(index)
    entries = []
    for t6, source_index in enumerate(result.kept):
        name = image_name(t6)
        entries.append({"origin": list(source.origins[source_index]) if source_index < len(source.origins)
                        else [0.0, 0.0, 0.0], "image": name})
        if t6 == NEUTRAL:
            continue  # stage_images synthesizes it
        dst = project_root / techsets.oat_image_path(name)
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(iwi.dds_to_iwi(files[source_index].read_bytes(), rgba_texels=_display_texels))
        report.images.append({"name": name, "source": str(files[source_index]),
                              "reflection_probe": source.images[source_index]})
    bsp = project_root / "BSP"
    bsp.mkdir(parents=True, exist_ok=True)
    # dynamic models pick the nearest probe the cell lists: every baked one
    (bsp / "reflectionprobes.json").write_text(json.dumps(
        {"probes": entries, "cellProbes": list(range(1, len(result.kept))) or [NEUTRAL]}, indent=1) + "\n",
        encoding="utf-8")
    baked = len(files) - len(missing)
    if baked > len(result.kept) - 1:
        report.warnings.append(f"reflection probes: T6 addresses {T6_MAX_PROBES} probes, the map bakes {baked}; "
                               f"kept {len(result.kept) - 1}, each dropped probe drawn with its nearest kept one")
    report.content["reflection_probes"] = {"source": len(source.origins), "baked": baked,
                                           "t6": len(result.kept), "remap": {str(k): v for k, v in result.remap.items()}}
    return result
