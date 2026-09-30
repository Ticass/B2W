"""Collision for the T6 clipmap.

World brushes come straight from the WaW clipmap. WaW static-model collision
stays per model: T6 links ``clipMap_t.staticModelList`` into its world
sectors at load and line-traces each entry against the xmodel's own collSurfs
(measured in t6zm: sub_886940 links, sub_886E50/886BF0 -> sub_6DB8A0/69ACC0 ->
sub_40DFD0 walks XModel collSurfs), exactly like WaW. Static models must NOT
become brushes: pmove/missile traces gather nearby brushes into a list capped
at 512 entries (sub_6A5CE0/5C7760 drop the rest silently), and one brush per
collision triangle overflowed it next to every model (walls missed, then the
uncapped position test pushed players back out).
"""
from __future__ import annotations

import math

Vec = tuple[float, float, float]



def _dot(a, b) -> float:
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _sub(a, b) -> Vec:
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _cross(a, b) -> Vec:
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _unit(a) -> Vec:
    length = math.sqrt(_dot(a, a))
    return (a[0] / length, a[1] / length, a[2] / length)


def solve3(rows, rhs) -> Vec | None:
    det = _dot(rows[0], _cross(rows[1], rows[2]))
    if abs(det) < 1e-12:
        return None
    c12, c20, c01 = _cross(rows[1], rows[2]), _cross(rows[2], rows[0]), _cross(rows[0], rows[1])
    return tuple((rhs[0] * c12[i] + rhs[1] * c20[i] + rhs[2] * c01[i]) / det for i in range(3))  # type: ignore[return-value]


def collision_triangle(tri: tuple[float, ...]) -> tuple[Vec, Vec, Vec] | None:
    """Model-space corners of one XModelCollTri_s (s,t = (0,0), (1,0), (0,1))."""
    n, d = tri[0:3], tri[3]
    sv, sw = tri[4:7], tri[7]
    tv, tw = tri[8:11], tri[11]
    rows = (n, sv, tv)
    corners = []
    for s, t in ((0.0, 0.0), (1.0, 0.0), (0.0, 1.0)):
        # s = svec.xyz . p - svec.w (verified: every corner lands in the surf bounds)
        p = solve3(rows, (d, s + sw, t + tw))
        if p is None:
            return None
        corners.append(p)
    return corners[0], corners[1], corners[2]


class Placement:
    """World transform of a WaW static model from its clipmap entry.

    The engine maps world to model space with ``local = transpose(A) (p - o)``
    for A = invScaledAxis (row-major), so ``p = o + inverse(transpose(A)) local``.
    """

    def __init__(self, origin: Vec, inv_scaled_axis: tuple[float, ...]):
        a = inv_scaled_axis
        m = ((a[0], a[3], a[6]), (a[1], a[4], a[7]), (a[2], a[5], a[8]))  # transpose(A)
        det = _dot(m[0], _cross(m[1], m[2]))
        if abs(det) < 1e-12:
            raise ValueError("degenerate static model transform")
        # inverse of m: columns are the cross products of its rows
        c0, c1, c2 = _cross(m[1], m[2]), _cross(m[2], m[0]), _cross(m[0], m[1])
        self.inv = tuple(tuple(c[i] / det for c in (c0, c1, c2)) for i in range(3))
        self.origin = origin

    def apply(self, p: Vec) -> Vec:
        return tuple(self.origin[i] + _dot(self.inv[i], p) for i in range(3))  # type: ignore[return-value]


def world_brush(brush, materials) -> dict:
    """WaW world brush -> bridge brush record (first six planes are axial)."""

    def flags(index: int) -> list[int]:
        if index < len(materials):
            return [materials[index].content_flags, materials[index].surface_flags]
        return [brush.contents, 0]

    axial = [flags(mat) for _, _, mat in brush.planes[:6]]
    sides = [[*n, d, *flags(mat)] for n, d, mat in brush.planes[6:]]
    return {"mins": list(brush.mins), "maxs": list(brush.maxs), "contents": brush.contents,
            "axial": axial, "sides": sides, "verts": [list(v) for v in brush.verts]}


def submodel_records(clip) -> list[dict]:
    """Brush submodels *1..*N in WaW order (entities reference them by index).

    Each record keeps its own brushes, so doors/blockers/triggers/zones stay
    separate from the static world collision."""
    out = []
    for sub in clip.submodels[1:]:
        brushes = [world_brush(clip.brushes[i], clip.materials) for i in sub.brushes]
        for b in brushes:
            b["contents"] = _signed(b["contents"])
            b["axial"] = [[_signed(c), _signed(s)] for c, s in b["axial"]]
            b["sides"] = [[*s[:4], _signed(s[4]), _signed(s[5])] for s in b["sides"]]
        out.append({"mins": list(sub.mins), "maxs": list(sub.maxs), "brushes": brushes})
    return out


def _signed(value: int) -> int:
    return value - (1 << 32) if value & 0x80000000 else value


def collision_brushes(clip) -> tuple[list[dict], dict]:
    """All brushes for BSP/brushes.json plus a summary for the stage report."""
    out: list[dict] = []
    world_ids = list(clip.submodels[0].brushes) if clip.submodels else list(range(len(clip.brushes)))
    # WaW world leafs also reference brush-model brushes (stored relative to
    # their entity); those must stay out of the static world.
    owned = {b for sub in clip.submodels[1:] for b in sub.brushes}
    world_ids = [i for i in world_ids if i not in owned]
    # The leaf-brush walk that lists the world's brushes misses some of them
    # (measured: 2,792 of 16,650 on one map: missile/shot clips, detail floors
    # and walls, player clips). Every brush belongs to exactly one cmodel, and
    # brush-model brushes lie in their entity's LOCAL space, so an unlisted
    # brush outside every brush model's local bounds is a world brush.
    listed = set(world_ids) | owned
    recovered, ambiguous = [], 0
    for i in range(len(clip.brushes)):
        if i in listed:
            continue
        b = clip.brushes[i]
        if any(all(s.mins[k] - 1 <= b.mins[k] and b.maxs[k] <= s.maxs[k] + 1 for k in range(3))
               for s in clip.submodels[1:]):
            ambiguous += 1
            continue
        recovered.append(i)
    world_ids += recovered
    for i in world_ids:
        out.append(world_brush(clip.brushes[i], clip.materials))
    world_count = len(out)
    for b in out:
        b["contents"] = _signed(b["contents"])
        b["axial"] = [[_signed(c), _signed(s)] for c, s in b["axial"]]
        b["sides"] = [[*s[:4], _signed(s[4]), _signed(s[5])] for s in b["sides"]]
    summary = {"world_brushes": world_count,
               "entity_brushes_excluded": sum(len(s.brushes) for s in clip.submodels[1:]),
               "world_brushes_recovered_unlisted": len(recovered),
               "unlisted_brushes_ambiguous_skipped": ambiguous}
    return out, summary


def static_model_records(clip) -> tuple[list[dict], dict]:
    """WaW clipmap static models for BSP/staticmodels.json (T6 cStaticModel_s).

    Only entries WaW itself could hit are kept: non-zero contents and at least
    one collision surface. The transform is WaW's invScaledAxis verbatim; both
    engines compute ``local = transpose(A) (p - o)``. Collision corners outside
    the WaW bounds (checked through the same transform) mean a transform or
    bounds mismatch and are counted.
    """
    out, outside, tris, no_collision = [], 0, 0, 0
    for sm in clip.static_models:
        if not sm.contents or not sm.surfaces:
            no_collision += 1
            continue
        placement = Placement(sm.origin, sm.inv_scaled_axis)
        for surf in sm.surfaces:
            for tri in surf.triangles:
                tris += 1
                corners = collision_triangle(tri)
                if corners is None:
                    continue
                world = [placement.apply(p) for p in corners]
                if any(not (sm.absmin[k] - 2.0 <= v[k] <= sm.absmax[k] + 2.0) for v in world for k in range(3)):
                    outside += 1
        out.append({"name": sm.name, "contents": _signed(sm.contents), "origin": list(sm.origin),
                    "invScaledAxis": list(sm.inv_scaled_axis), "absmin": list(sm.absmin),
                    "absmax": list(sm.absmax)})
    summary = {"static_models": len(out), "static_model_collision_triangles": tris,
               "static_models_without_collision": no_collision,
               "static_model_triangles_outside_waw_bounds": outside}
    return out, summary
