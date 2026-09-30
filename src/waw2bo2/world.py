from __future__ import annotations

import math
import struct
from dataclasses import dataclass, field
from pathlib import Path
from typing import BinaryIO

MAGIC = b"W2BSP001"


class FormatError(ValueError):
    pass


class Reader:
    def __init__(self, stream: BinaryIO):
        self.stream = stream

    def exact(self, size: int) -> bytes:
        data = self.stream.read(size)
        if len(data) != size:
            raise FormatError(f"truncated interchange file at offset {self.stream.tell()}")
        return data

    def unpack(self, fmt: str):
        values = struct.unpack("<" + fmt, self.exact(struct.calcsize("<" + fmt)))
        return values[0] if len(values) == 1 else values

    def string(self) -> str:
        length = self.unpack("I")
        if length > 1_048_576:
            raise FormatError(f"unreasonable string length {length}")
        return self.exact(length).decode("utf-8", errors="strict")


@dataclass(slots=True)
class Vertex:
    xyz: tuple[float, float, float]
    color: tuple[float, float, float, float]
    uv: tuple[float, float]
    lightmap_uv: tuple[float, float]
    normal: tuple[float, float, float]
    tangent: tuple[float, float, float]
    binormal_sign: float


@dataclass(slots=True)
class Surface:
    first_vertex: int
    vertex_count: int
    triangle_count: int
    base_index: int
    material: str
    lightmap_index: int
    reflection_probe_index: int
    primary_light_index: int
    flags: int
    mins: tuple[float, float, float]
    maxs: tuple[float, float, float]


@dataclass(slots=True)
class StaticModel:
    name: str
    origin: tuple[float, float, float]
    axis: tuple[float, ...]
    scale: float
    flags: int
    cull_dist: float = 0.0  # WaW per-instance draw distance (0 = unknown)


@dataclass(slots=True)
class GfxWorld:
    name: str
    base_name: str
    skybox_model: str
    vertices: list[Vertex]
    indices: list[int]
    surfaces: list[Surface]
    static_models: list[StaticModel]


def _color(packed: int) -> tuple[float, float, float, float]:
    return tuple(((packed >> shift) & 0xFF) / 255.0 for shift in (0, 8, 16, 24))  # type: ignore[return-value]


def _unit_vec_scale(packed: int) -> tuple[float, float, float]:
    octets = struct.pack("<I", packed)
    scale = (octets[3] + 192.0) / 32385.0
    result = tuple((component - 127.0) * scale for component in octets[:3])
    length = math.sqrt(sum(component * component for component in result))
    if not math.isfinite(length) or length < 1.0e-8:
        return (0.0, 0.0, 1.0)
    return tuple(component / length for component in result)  # type: ignore[return-value]


def read_gfx_world(path: Path) -> GfxWorld:
    with path.open("rb") as stream:
        r = Reader(stream)
        if r.exact(8) != MAGIC or (gfx_version := r.unpack("I")) not in (1, 2):
            raise FormatError(f"{path} is not a W2BSP001 render-world file")
        vertex_count, index_count, surface_count, model_count = r.unpack("IIII")
        if vertex_count > 20_000_000 or index_count > 60_000_000 or surface_count > 2_000_000:
            raise FormatError("world counts exceed defensive limits")
        name, base_name, skybox_model = r.string(), r.string(), r.string()
        vertices: list[Vertex] = []
        for _ in range(vertex_count):
            xyz = r.unpack("3f")
            color = _color(r.unpack("I"))
            uv = r.unpack("2f")
            lightmap_uv = r.unpack("2f")
            normal = _unit_vec_scale(r.unpack("I"))
            tangent = _unit_vec_scale(r.unpack("I"))
            binormal_sign = r.unpack("f")
            vertices.append(Vertex(xyz, color, uv, lightmap_uv, normal, tangent, binormal_sign))
        indices = list(r.unpack(f"{index_count}H")) if index_count else []
        surfaces: list[Surface] = []
        for _ in range(surface_count):
            first_vertex, count, tri_count, base_index = r.unpack("IHHI")
            # OAT marks referenced assets with a leading comma in the native
            # name table. JSON material dumps canonicalize that marker away.
            material = r.string()
            if material.startswith(","):
                material = material[1:]
            lightmap, probe, primary, flags = r.unpack("4B")
            mins, maxs = r.unpack("3f"), r.unpack("3f")
            if base_index + tri_count * 3 > len(indices) or first_vertex + count > len(vertices):
                raise FormatError(f"surface {len(surfaces)} references data outside the world buffers")
            local = indices[base_index : base_index + tri_count * 3]
            if local and max(local) >= count:
                raise FormatError(f"surface {len(surfaces)} has an out-of-range local vertex index")
            surfaces.append(Surface(first_vertex, count, tri_count, base_index, material, lightmap, probe, primary, flags, mins, maxs))
        models: list[StaticModel] = []
        for _ in range(model_count):
            model_name = r.string()
            origin = r.unpack("3f")
            axis = r.unpack("9f")
            scale, flags = r.unpack("fI")
            cull_dist = r.unpack("f") if gfx_version >= 2 else 0.0
            models.append(StaticModel(model_name, origin, axis, scale, flags, cull_dist))
        if stream.read(1):
            raise FormatError("render-world file has trailing data (schema mismatch)")
    return GfxWorld(name, base_name, skybox_model, vertices, indices, surfaces, models)


@dataclass(slots=True)
class CollisionSummary:
    name: str
    material_count: int
    vertex_count: int
    triangle_count: int
    brush_count: int


@dataclass(slots=True)
class ClipMaterial:
    name: str
    surface_flags: int
    content_flags: int


@dataclass(slots=True)
class Brush:
    mins: tuple[float, float, float]
    maxs: tuple[float, float, float]
    contents: int
    # (normal, dist, clip material index); the first six are the axial planes
    planes: list[tuple[tuple[float, float, float], float, int]]
    verts: list[tuple[float, float, float]]


@dataclass(slots=True)
class SubModel:
    mins: tuple[float, float, float]
    maxs: tuple[float, float, float]
    brushes: list[int]


@dataclass(slots=True)
class CollSurf:
    contents: int
    surface_flags: int
    mins: tuple[float, float, float]
    maxs: tuple[float, float, float]
    bone: int
    # model-space triangles: plane (nx, ny, nz, d), svec (4), tvec (4). A point
    # p on the triangle has barycentrics s = svec.xyz . p - svec.w, t likewise.
    triangles: list[tuple[float, ...]]


@dataclass(slots=True)
class ClipStaticModel:
    name: str
    origin: tuple[float, float, float]
    # row-major; model-space = transpose(inv_scaled_axis) * (world - origin)
    inv_scaled_axis: tuple[float, ...]
    absmin: tuple[float, float, float]
    absmax: tuple[float, float, float]
    contents: int
    surfaces: list[CollSurf]


@dataclass(slots=True)
class CollisionWorld:
    summary: CollisionSummary
    vertices: list[tuple[float, float, float]]
    indices: list[int]
    materials: list[ClipMaterial] = field(default_factory=list)
    brushes: list[Brush] = field(default_factory=list)
    # index 0 is the world; brushes owned by 1.. belong to brush entities
    submodels: list[SubModel] = field(default_factory=list)
    static_models: list[ClipStaticModel] = field(default_factory=list)


def read_collision(path: Path) -> CollisionWorld:
    with path.open("rb") as stream:
        r = Reader(stream)
        if r.exact(8) != MAGIC or (version := r.unpack("I")) not in (2, 3):
            raise FormatError(f"{path} is not a W2BSP001 clip-world file")
        name = r.string()
        material_count = r.unpack("I")
        materials = []
        for _ in range(material_count):
            mat_name = r.string()
            surface_flags, content_flags = r.unpack("2i")
            materials.append(ClipMaterial(mat_name, surface_flags & 0xFFFFFFFF, content_flags & 0xFFFFFFFF))
        vertex_count = r.unpack("I")
        vertices = [r.unpack("3f") for _ in range(vertex_count)]
        triangle_count = r.unpack("I")
        indices = list(r.unpack(f"{triangle_count * 3}H")) if triangle_count else []
        brush_count = r.unpack("I")
        brushes = []
        for _ in range(brush_count):
            mins, maxs = r.unpack("3f"), r.unpack("3f")
            contents = r.unpack("i") & 0xFFFFFFFF
            side_count = r.unpack("I")
            planes = []
            for _ in range(side_count):
                nx, ny, nz, dist, mat = r.unpack("4fI")
                planes.append(((nx, ny, nz), dist, mat))
            brush_vertex_count = r.unpack("I")
            verts = [r.unpack("3f") for _ in range(brush_vertex_count)]
            brushes.append(Brush(mins, maxs, contents, planes, verts))
        submodels = []
        if version >= 3:
            for _ in range(r.unpack("I")):
                mins, maxs = r.unpack("3f"), r.unpack("3f")
                owned = r.unpack("I")
                brush_ids = list(struct.unpack(f"<{owned}H", r.exact(owned * 2)))
                if any(b >= brush_count for b in brush_ids):
                    raise FormatError("submodel references a brush outside the brush array")
                submodels.append(SubModel(mins, maxs, brush_ids))
        static_models = []
        if version >= 3:
            for _ in range(r.unpack("I")):
                model_name = r.string()
                origin = r.unpack("3f")
                inv_scaled_axis = r.unpack("9f")
                absmin, absmax = r.unpack("3f"), r.unpack("3f")
                model_contents = r.unpack("i") & 0xFFFFFFFF
                surfs = []
                for _ in range(r.unpack("I")):
                    surf_contents, surf_flags = r.unpack("2i")
                    smins, smaxs = r.unpack("3f"), r.unpack("3f")
                    bone = r.unpack("i")
                    tri_count = r.unpack("I")
                    tris = [r.unpack("12f") for _ in range(tri_count)]
                    surfs.append(CollSurf(surf_contents & 0xFFFFFFFF, surf_flags & 0xFFFFFFFF, smins, smaxs, bone, tris))
                static_models.append(ClipStaticModel(model_name, origin, inv_scaled_axis, absmin, absmax,
                                                     model_contents, surfs))
        if stream.read(1):
            raise FormatError("clip-world file has trailing data (schema mismatch)")
    summary = CollisionSummary(name, material_count, vertex_count, triangle_count, brush_count)
    return CollisionWorld(summary, vertices, indices, materials, brushes, submodels, static_models)


def inspect_collision(path: Path) -> CollisionSummary:
    """Validate the complete clip payload while retaining only counts."""
    return read_collision(path).summary

