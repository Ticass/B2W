"""
CODIR primitives.

CODIR is the neutral form every source game is translated into before anything
BO2-specific happens. Its job is to record *meaning*, not bytes: "this surface
uses a colour map, a normal map and is alpha tested" rather than "this is a
T5 material with techset mc_l_sm_b0c0n0s0".

Two rules hold everywhere in this package:

  1. Nothing is silently dropped. If a source concept has no CODIR field, it
     goes in `extras` so the converter can still see it and report on it.
  2. Units are normalised on the way in. CODIR is always in Call of Duty world
     units (inches), Z-up, right-handed -- the convention BO2 uses. Plugins for
     games that differ convert at parse time and say so.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from typing import Any, Iterable, Sequence

# COD world units are inches. Player height ~64, standard doorway ~80.
UNITS_PER_METRE = 39.37007874


@dataclass(slots=True, frozen=True)
class Vec3:
    x: float = 0.0
    y: float = 0.0
    z: float = 0.0

    def __iter__(self):
        yield self.x
        yield self.y
        yield self.z

    def __add__(self, o: "Vec3") -> "Vec3":
        return Vec3(self.x + o.x, self.y + o.y, self.z + o.z)

    def __sub__(self, o: "Vec3") -> "Vec3":
        return Vec3(self.x - o.x, self.y - o.y, self.z - o.z)

    def __mul__(self, s: float) -> "Vec3":
        return Vec3(self.x * s, self.y * s, self.z * s)

    @property
    def length(self) -> float:
        return math.sqrt(self.x * self.x + self.y * self.y + self.z * self.z)

    def normalised(self) -> "Vec3":
        n = self.length
        return Vec3(self.x / n, self.y / n, self.z / n) if n > 1e-9 else Vec3()

    def to_list(self) -> list[float]:
        return [self.x, self.y, self.z]

    @classmethod
    def parse(cls, text: str | Sequence[float] | None) -> "Vec3":
        """Accepts '1 2 3', '1,2,3', [1,2,3] or None."""
        if text is None:
            return cls()
        if isinstance(text, (list, tuple)):
            vals = list(text) + [0.0, 0.0, 0.0]
            return cls(float(vals[0]), float(vals[1]), float(vals[2]))
        # Numbers as the engine's sscanf reads them: Black Ops' own zombie_coast writes its sun
        # colour ".567 .613.653", which is .567, .613, .653 -- split on spaces it was two values.
        import re

        parts = re.findall(r"[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?", str(text))
        vals = [_safe_float(p) for p in parts] + [0.0, 0.0, 0.0]
        return cls(vals[0], vals[1], vals[2])

    def to_str(self) -> str:
        return f"{self.x:g} {self.y:g} {self.z:g}"


def _safe_float(text: str) -> float:
    try:
        return float(text)
    except (TypeError, ValueError):
        return 0.0


@dataclass(slots=True)
class Bounds:
    """Axis-aligned bounding box in CODIR units."""

    mins: Vec3 = field(default_factory=Vec3)
    maxs: Vec3 = field(default_factory=Vec3)

    @property
    def size(self) -> Vec3:
        return self.maxs - self.mins

    @property
    def centre(self) -> Vec3:
        return Vec3(
            (self.mins.x + self.maxs.x) * 0.5,
            (self.mins.y + self.maxs.y) * 0.5,
            (self.mins.z + self.maxs.z) * 0.5,
        )

    @property
    def valid(self) -> bool:
        return (
            self.maxs.x >= self.mins.x
            and self.maxs.y >= self.mins.y
            and self.maxs.z >= self.mins.z
        )

    @property
    def degenerate(self) -> bool:
        s = self.size
        return s.x <= 1e-6 and s.y <= 1e-6 and s.z <= 1e-6

    def expand(self, point: Vec3) -> None:
        self.mins = Vec3(min(self.mins.x, point.x), min(self.mins.y, point.y), min(self.mins.z, point.z))
        self.maxs = Vec3(max(self.maxs.x, point.x), max(self.maxs.y, point.y), max(self.maxs.z, point.z))

    @classmethod
    def around(cls, points: Iterable[Vec3]) -> "Bounds":
        it = iter(points)
        try:
            first = next(it)
        except StopIteration:
            return cls()
        b = cls(mins=first, maxs=first)
        for p in it:
            b.expand(p)
        return b

    def to_dict(self) -> dict[str, Any]:
        return {"mins": self.mins.to_list(), "maxs": self.maxs.to_list()}

    @classmethod
    def from_dict(cls, d: dict | None) -> "Bounds":
        if not d:
            return cls()
        return cls(Vec3.parse(d.get("mins")), Vec3.parse(d.get("maxs")))


@dataclass(slots=True)
class Transform:
    """Position, Euler angles (pitch yaw roll, degrees) and uniform-ish scale."""

    origin: Vec3 = field(default_factory=Vec3)
    angles: Vec3 = field(default_factory=Vec3)
    scale: Vec3 = field(default_factory=lambda: Vec3(1.0, 1.0, 1.0))

    @property
    def uniform_scale(self) -> bool:
        s = self.scale
        return abs(s.x - s.y) < 1e-4 and abs(s.y - s.z) < 1e-4

    def to_dict(self) -> dict[str, Any]:
        return {
            "origin": self.origin.to_list(),
            "angles": self.angles.to_list(),
            "scale": self.scale.to_list(),
        }

    @classmethod
    def from_dict(cls, d: dict | None) -> "Transform":
        if not d:
            return cls()
        return cls(
            Vec3.parse(d.get("origin")),
            Vec3.parse(d.get("angles")),
            Vec3.parse(d.get("scale")) if d.get("scale") else Vec3(1.0, 1.0, 1.0),
        )


@dataclass(slots=True, frozen=True)
class AssetRef:
    """
    A typed pointer to another CODIR asset.

    Using a struct rather than a bare string means the dependency graph can be
    built mechanically, and a dangling reference is detectable rather than
    being an empty string nobody noticed.
    """

    kind: str
    name: str

    def __str__(self) -> str:
        return f"{self.kind}:{self.name}"

    def to_dict(self) -> dict[str, str]:
        return {"kind": self.kind, "name": self.name}

    @classmethod
    def from_dict(cls, d: dict) -> "AssetRef":
        return cls(kind=d["kind"], name=d["name"])


@dataclass
class CodirAsset:
    """
    Base for everything in CODIR.

    `source_game` and `source_path` are kept so that every converted asset can
    be traced back to the exact file it came from -- the differential report
    and every finding depend on it.
    """

    name: str = ""
    kind: str = ""
    source_game: str = ""
    source_path: str = ""
    references: list[AssetRef] = field(default_factory=list)
    extras: dict[str, Any] = field(default_factory=dict)
    """Source data with no CODIR home. Preserved, never silently discarded."""
    notes: list[str] = field(default_factory=list)

    def ref(self, kind: str, name: str) -> AssetRef:
        r = AssetRef(kind=kind, name=name)
        if r not in self.references:
            self.references.append(r)
        return r

    def base_dict(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "kind": self.kind,
            "source_game": self.source_game,
            "source_path": self.source_path,
            "references": [r.to_dict() for r in self.references],
            "extras": self.extras,
            "notes": self.notes,
        }

    def load_base(self, d: dict[str, Any]) -> None:
        self.name = d.get("name", "")
        self.kind = d.get("kind", self.kind)
        self.source_game = d.get("source_game", "")
        self.source_path = d.get("source_path", "")
        self.references = [AssetRef.from_dict(r) for r in d.get("references", [])]
        self.extras = dict(d.get("extras") or {})
        self.notes = list(d.get("notes") or [])
