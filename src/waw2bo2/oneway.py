"""Identify WaW one-sided traversal sheets for removal from T6 collision.

T6 makes these sheets block both directions. The user requested that their
collision be omitted entirely rather than emulated by player movement. This
is a global geometry rule: player-blocking, non-solid, steep triangles without
an opposing face. Floors and ordinary solid geometry remain collision assets.
"""
from __future__ import annotations

import math

# WaW player movement mask (sub_67C1B0 sets ent clipmask, sub_665C30 pmove)
WAW_PLAYER_MASK = 0x281C011
CONTENTS_SOLID = 0x1
STEEP_NZ = 0.7          # |normal.z| below this: a wall, not a floor/ceiling


def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _unit(v):
    length = math.sqrt(_dot(v, v))
    return None if length < 1e-6 else (v[0] / length, v[1] / length, v[2] / length)


def one_way_sheets(clip) -> list[dict]:
    """WaW one-sided traversal triangles, with geometry for export diagnostics."""
    if not clip.triangle_materials:
        return []
    verts, idx, mats = clip.vertices, clip.indices, clip.materials

    def key(p):
        return (round(p[0], 1), round(p[1], 1), round(p[2], 1))

    blocking: dict[frozenset, list[int]] = {}
    for t, m in enumerate(clip.triangle_materials):
        if m == 0xFFFF or not mats[m].content_flags & WAW_PLAYER_MASK:
            continue
        corners = frozenset(key(verts[idx[3 * t + k]]) for k in range(3))
        blocking.setdefault(corners, []).append(t)
    sheets = []
    for t, m in enumerate(clip.triangle_materials):
        if m == 0xFFFF:
            continue
        contents = mats[m].content_flags
        if not contents & WAW_PLAYER_MASK or contents & CONTENTS_SOLID:
            continue
        a, b, c = (tuple(verts[idx[3 * t + k]]) for k in range(3))
        n = _unit(_cross(_sub(c, a), _sub(b, a)))       # WaW front face
        if n is None or abs(n[2]) >= STEEP_NZ:
            continue
        twins = blocking.get(frozenset(key(p) for p in (a, b, c)), [])
        # Same-facing duplicates are still one-sided. Only an opposing face
        # makes the sheet block from both sides in WaW.
        opposite = False
        for twin in twins:
            if twin == t:
                continue
            ta, tb, tc = (tuple(verts[idx[3 * twin + k]]) for k in range(3))
            tn = _unit(_cross(_sub(tc, ta), _sub(tb, ta)))
            if tn is not None and _dot(n, tn) < -0.99:
                opposite = True
                break
        if opposite:
            continue
        edges = []
        for p, q in ((a, b), (b, c), (c, a)):
            e = _unit(_cross(n, _sub(q, p)))
            if e is None:
                break
            # inward: the opposite corner is on the positive side
            r = c if (p, q) == (a, b) else a if (p, q) == (b, c) else b
            if _dot(e, _sub(r, p)) < 0:
                e = (-e[0], -e[1], -e[2])
            edges.append((e, _dot(e, p)))
        if len(edges) != 3:
            continue
        sheets.append({"triangle": t, "material": mats[m].name, "normal": n, "dist": _dot(n, a),
                       "edges": edges, "corners": [a, b, c],
                       "mins": [min(p[i] for p in (a, b, c)) for i in range(3)],
                       "maxs": [max(p[i] for p in (a, b, c)) for i in range(3)]})
    return sheets



def oneway_source(sheets: list[dict]) -> str:
    """Compatibility entry point for existing generated map-main hooks.

    Collision export removes the sheets. No teleporting or movement watcher
    is generated, even when callers still provide the old sheet list.
    """
    return "// waw2bo2: one-sided traversal collision is omitted at export.\n\ninit()\n{\n}\n"
