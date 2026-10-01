"""List collision a standing T6 player touches that a crouching one does not.

Reads the linked map's own collision (T6 Unlinker ``.collision.json`` for
terrain triangles, ``BSP/brushes.json`` for world brushes) and tests capsules
at a ``/viewpos`` position. The capsule is a segment from ``feet + r`` to
``feet + height - r`` with radius ``r`` (T6 traces players as capsules,
sub_881940); contents are filtered with the measured T6 player clipmask
0x2818011 (sub_61F390). Triangles are tested two-sided, like the T6 position
test (sub_882E40), and closer than ``r`` counts as touching.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

PLAYER_MASK = 0x2818011


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def closest_point_triangle(p, a, b, c):
    """Ericson, Real-Time Collision Detection 5.1.5."""
    ab, ac, ap = sub(b, a), sub(c, a), sub(p, a)
    d1, d2 = dot(ab, ap), dot(ac, ap)
    if d1 <= 0 and d2 <= 0:
        return a
    bp = sub(p, b)
    d3, d4 = dot(ab, bp), dot(ac, bp)
    if d3 >= 0 and d4 <= d3:
        return b
    vc = d1 * d4 - d3 * d2
    if vc <= 0 and d1 >= 0 and d3 <= 0:
        v = d1 / (d1 - d3)
        return (a[0] + v * ab[0], a[1] + v * ab[1], a[2] + v * ab[2])
    cp = sub(p, c)
    d5, d6 = dot(ab, cp), dot(ac, cp)
    if d6 >= 0 and d5 <= d6:
        return c
    vb = d5 * d2 - d1 * d6
    if vb <= 0 and d2 >= 0 and d6 <= 0:
        w = d2 / (d2 - d6)
        return (a[0] + w * ac[0], a[1] + w * ac[1], a[2] + w * ac[2])
    va = d3 * d6 - d5 * d4
    if va <= 0 and (d4 - d3) >= 0 and (d5 - d6) >= 0:
        w = (d4 - d3) / ((d4 - d3) + (d5 - d6))
        return (b[0] + w * (c[0] - b[0]), b[1] + w * (c[1] - b[1]), b[2] + w * (c[2] - b[2]))
    denom = 1.0 / (va + vb + vc)
    v, w = vb * denom, vc * denom
    return (a[0] + ab[0] * v + ac[0] * w, a[1] + ab[1] * v + ac[1] * w, a[2] + ab[2] * v + ac[2] * w)


def segment_triangle_distance(p0, p1, tri, steps=24):
    """Distance from a vertical segment to a triangle (sampled along the segment)."""
    best = math.inf
    for i in range(steps + 1):
        t = i / steps
        p = (p0[0], p0[1], p0[2] + (p1[2] - p0[2]) * t)
        q = closest_point_triangle(p, *tri)
        best = min(best, math.dist(p, q))
    return best


def capsule_hits_brush(feet, radius, height, brush):
    """Expand every plane by the radius (as sub_883EA0 does) and test the segment."""
    planes = [((-1.0, 0.0, 0.0), -brush["mins"][0]), ((1.0, 0.0, 0.0), brush["maxs"][0]),
              ((0.0, -1.0, 0.0), -brush["mins"][1]), ((0.0, 1.0, 0.0), brush["maxs"][1]),
              ((0.0, 0.0, -1.0), -brush["mins"][2]), ((0.0, 0.0, 1.0), brush["maxs"][2])]
    planes += [((s[0], s[1], s[2]), s[3]) for s in brush["sides"]]
    lo = feet[2] + radius
    hi = feet[2] + height - radius
    half = (hi - lo) * 0.5
    centre = (feet[0], feet[1], (lo + hi) * 0.5)
    return all(dot(n, centre) - d <= radius + abs(n[2]) * half for n, d in planes)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("x", type=float)
    parser.add_argument("y", type=float)
    parser.add_argument("z", type=float, help="viewpos z (eye); feet = z - --eye")
    parser.add_argument("--eye", type=float, default=60.0, help="standing view height")
    parser.add_argument("--radius", type=float, default=15.0)
    parser.add_argument("--stand", type=float, default=70.0)
    parser.add_argument("--crouch", type=float, default=50.0)
    parser.add_argument("--search", type=float, default=48.0, help="horizontal search radius")
    parser.add_argument("--collision", type=Path, required=True, help="T6 Unlinker .collision.json")
    parser.add_argument("--brushes", type=Path, required=True, help="BSP/brushes.json")
    args = parser.parse_args()

    feet = (args.x, args.y, args.z - args.eye)
    data = json.loads(args.collision.read_text(encoding="utf-8"))
    verts = data["vertices"]
    material_of = {}
    for tree in data["aabbs"]:
        if not tree["children"]:
            material_of[tree["index"]] = tree["material"]
    print(f"feet {feet}  radius {args.radius}  stand {args.stand}  crouch {args.crouch}")
    for p, (first, count) in enumerate(data["partitions"]):
        mat = data["materials"][material_of.get(p, 0)]
        if not (mat["contents"] & PLAYER_MASK):
            continue
        for t in range(first, first + count):
            tri = [tuple(verts[i]) for i in data["triangles"][t]]
            if all(math.dist(feet[:2], v[:2]) > args.search + 200 for v in tri):
                continue
            results = []
            for name, height in (("stand", args.stand), ("crouch", args.crouch)):
                p0 = (feet[0], feet[1], feet[2] + args.radius)
                p1 = (feet[0], feet[1], feet[2] + height - args.radius)
                results.append(segment_triangle_distance(p0, p1, tri) < args.radius)
            if results[0]:
                tag = "STAND-ONLY" if not results[1] else "both"
                print(f"tri {t} {tag} {mat['name']} contents {mat['contents'] & 0xFFFFFFFF:#x} "
                      f"{[[round(c, 1) for c in v] for v in tri]}")
    brushes = json.loads(args.brushes.read_text(encoding="utf-8"))["brushes"]
    for i, b in enumerate(brushes):
        if not (b["contents"] & PLAYER_MASK):
            continue
        stand = capsule_hits_brush(feet, args.radius, args.stand, b)
        if stand:
            crouch = capsule_hits_brush(feet, args.radius, args.crouch, b)
            tag = "STAND-ONLY" if not crouch else "both"
            print(f"brush {i} {tag} contents {b['contents'] & 0xFFFFFFFF:#x} "
                  f"mins {[round(v) for v in b['mins']]} maxs {[round(v) for v in b['maxs']]}")


if __name__ == "__main__":
    main()
