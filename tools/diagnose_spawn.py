"""Inspect source collision immediately below a WaW spawn position."""
from __future__ import annotations

import sys
from pathlib import Path

from waw2bo2.world import read_collision


def main() -> None:
    clip = read_collision(Path(sys.argv[1]))
    x, y, z = map(float, sys.argv[2:5])
    hits = []
    for i in range(0, len(clip.indices), 3):
        a, b, c = (clip.vertices[clip.indices[i + j]] for j in range(3))
        denom = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
        if abs(denom) < 1e-7:
            continue
        u = ((b[1] - c[1]) * (x - c[0]) + (c[0] - b[0]) * (y - c[1])) / denom
        v = ((c[1] - a[1]) * (x - c[0]) + (a[0] - c[0]) * (y - c[1])) / denom
        w = 1 - u - v
        if min(u, v, w) >= -1e-4:
            normal_z = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
            hits.append((u * a[2] + v * b[2] + w * c[2], i // 3, "+z" if normal_z > 0 else "-z"))
    print(f"source triangles under ({x}, {y}, {z}): {len(hits)}")
    print("nearest heights:", sorted(hits, key=lambda h: abs(h[0] - z))[:15])
    brushes = [
        (i, b.mins[2], b.maxs[2], b.contents)
        for i, b in enumerate(clip.brushes)
        if b.mins[0] <= x <= b.maxs[0] and b.mins[1] <= y <= b.maxs[1]
    ]
    print("source brush heights:", sorted(brushes, key=lambda h: abs(h[2] - z))[:15])


if __name__ == "__main__":
    main()
