"""Point traces through WaW world collision, as the WaW light grid uses them.

WaW's static model lighting (sub_71C770) traces a point (zero box, mins =
maxs = vec3_origin at 0x816F68) through world model 0 with contents mask
0x2001 (sub_58EA70 -> sub_58E750). Model 0 leaves hold brushes and terrain
triangles only (sub_58E0A0); static model collision is not part of it.
"""
import math
from collections import defaultdict

from . import fbx
from .world import CollisionWorld

CELL = 128.0
EPSILON = 1e-6


def _cells(mins, maxs):
    lo = [math.floor(v / CELL) for v in mins]
    hi = [math.floor(v / CELL) for v in maxs]
    for x in range(lo[0], hi[0] + 1):
        for y in range(lo[1], hi[1] + 1):
            for z in range(lo[2], hi[2] + 1):
                yield x, y, z


class WorldTracer:
    """Segment visibility through model 0 brushes and terrain triangles."""

    def __init__(self, collision: CollisionWorld, mask: int):
        self.brushes, self.triangles = [], []
        self.cells = defaultdict(list)
        owned = {b for model in collision.submodels[1:] for b in model.brushes}
        for index, brush in enumerate(collision.brushes):
            reach = collision.brush_reach[index] if collision.brush_reach else brush.contents
            if index in owned or not brush.contents & mask or not reach & mask:
                continue
            planes = [(n, d) for n, d, _material in brush.planes]
            self._add(("b", len(self.brushes)), brush.mins, brush.maxs)
            self.brushes.append(planes)
        owners = fbx.collision_triangle_models(collision)
        for tri in range(len(collision.indices) // 3):
            material = collision.triangle_materials[tri] if tri < len(collision.triangle_materials) else 0xFFFF
            if owners[tri] or material >= len(collision.materials):
                continue
            if not collision.materials[material].content_flags & mask:
                continue
            a, b, c = (collision.vertices[collision.indices[3 * tri + k]] for k in range(3))
            mins = [min(a[i], b[i], c[i]) for i in range(3)]
            maxs = [max(a[i], b[i], c[i]) for i in range(3)]
            self._add(("t", len(self.triangles)), mins, maxs)
            self.triangles.append((a, b, c))

    def _add(self, item, mins, maxs):
        for cell in _cells(mins, maxs):
            self.cells[cell].append(item)

    def _candidates(self, start, end):
        mins = [min(s, e) for s, e in zip(start, end)]
        maxs = [max(s, e) for s, e in zip(start, end)]
        seen = set()
        for cell in _cells(mins, maxs):
            for item in self.cells.get(cell, ()):
                if item not in seen:
                    seen.add(item)
                    yield item

    @staticmethod
    def _hits_brush(planes, start, end) -> bool:
        enter, leave = 0.0, 1.0
        for normal, dist in planes:
            d0 = sum(n * s for n, s in zip(normal, start)) - dist
            d1 = sum(n * e for n, e in zip(normal, end)) - dist
            if d0 > 0 and d1 > 0:
                return False
            if d0 <= 0 and d1 <= 0:
                continue
            t = d0 / (d0 - d1)
            if d0 > 0:
                enter = max(enter, t)
            else:
                leave = min(leave, t)
            if enter > leave:
                return False
        return True

    @staticmethod
    def _hits_triangle(tri, start, end) -> bool:
        a, b, c = tri
        direction = [e - s for s, e in zip(start, end)]
        e1 = [b[i] - a[i] for i in range(3)]
        e2 = [c[i] - a[i] for i in range(3)]
        p = [direction[1] * e2[2] - direction[2] * e2[1], direction[2] * e2[0] - direction[0] * e2[2],
             direction[0] * e2[1] - direction[1] * e2[0]]
        det = sum(e1[i] * p[i] for i in range(3))
        if abs(det) < EPSILON:
            return False
        inv = 1.0 / det
        s = [start[i] - a[i] for i in range(3)]
        u = sum(s[i] * p[i] for i in range(3)) * inv
        if u < 0.0 or u > 1.0:
            return False
        q = [s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]]
        v = sum(direction[i] * q[i] for i in range(3)) * inv
        if v < 0.0 or u + v > 1.0:
            return False
        t = sum(e2[i] * q[i] for i in range(3)) * inv
        return 0.0 <= t <= 1.0

    def visible(self, start, end) -> bool:
        for kind, index in self._candidates(start, end):
            if kind == "b" and self._hits_brush(self.brushes[index], start, end):
                return False
            if kind == "t" and self._hits_triangle(self.triangles[index], start, end):
                return False
        return True
