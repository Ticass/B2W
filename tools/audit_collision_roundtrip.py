"""Compare an extracted native T6 collision geometry dump with its WaW source.

Run T6 Unlinker with --include-assets mapents to obtain .collision.json.
Coordinates, one-sided winding and collision masks must survive conversion.
"""
from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path

from waw2bo2.world import read_collision


def triangle_key(corners, contents, surface_flags):
    # Cyclic rotations preserve winding; reversing a triangle does not.
    points = tuple(tuple(round(float(v), 2) for v in p) for p in corners)
    return min(points[i:] + points[:i] for i in range(3)), contents & 0xFFFFFFFF, surface_flags & 0xFFFFFFFF


def audit(source_path: Path, target_path: Path) -> dict:
    source = read_collision(source_path)
    if not source.triangle_materials:
        raise ValueError("source dump needs per-triangle materials (v4 or newer)")
    expected = Counter()
    for t, material_index in enumerate(source.triangle_materials):
        if material_index == 0xFFFF:
            continue
        material = source.materials[material_index]
        if not material.content_flags:
            continue
        corners = [source.vertices[i] for i in source.indices[t * 3:t * 3 + 3]]
        expected[triangle_key(corners, material.content_flags, material.surface_flags)] += 1

    target = json.loads(target_path.read_text(encoding="utf-8"))
    partition_materials = {}
    for tree in target["aabbs"]:
        if not tree["children"]:
            p, material = tree["index"], tree["material"]
            if p in partition_materials and partition_materials[p] != material:
                raise ValueError(f"partition {p} has conflicting materials")
            partition_materials[p] = material
    actual = Counter()
    seen = set()
    for p, (first, count) in enumerate(target["partitions"]):
        material = target["materials"][partition_materials[p]]
        for t in range(first, first + count):
            if t in seen:
                raise ValueError(f"triangle {t} belongs to multiple partitions")
            seen.add(t)
            corners = [target["vertices"][i] for i in target["triangles"][t]]
            actual[triangle_key(corners, material["contents"], material["surface_flags"])] += 1
    missing, extra = expected - actual, actual - expected
    return {"source_triangles": sum(expected.values()), "target_triangles": sum(actual.values()),
            "missing_or_changed_triangles": sum(missing.values()), "extra_or_changed_triangles": sum(extra.values()),
            "triangle_positions_winding_and_flags_match": not missing and not extra,
            "source_brush_ownership_complete": source.brush_ownership_complete}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("target", type=Path)
    args = parser.parse_args()
    result = audit(args.source, args.target)
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result["triangle_positions_winding_and_flags_match"] else 1)
