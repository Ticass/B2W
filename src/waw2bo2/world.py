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
    brush_model: int = 0
    # v5: byte offset of this surface's extra-layer vertices in GfxWorld.layer_data
    # and its technique set's MaterialWorldVertexFormat (0: single layer)
    layer_data_offset: int = 0
    world_vert_format: int = 0
    index: int = -1  # position in the source surface array (shadow geometry refers to it)


@dataclass(slots=True)
class StaticModel:
    name: str
    origin: tuple[float, float, float]
    axis: tuple[float, ...]
    scale: float
    flags: int
    cull_dist: float = 0.0  # WaW per-instance draw distance (0 = unknown)
    primary_light_index: int = 1


@dataclass(slots=True)
class GfxWorld:
    name: str
    base_name: str
    skybox_model: str
    vertices: list[Vertex]
    indices: list[int]
    surfaces: list[Surface]
    static_models: list[StaticModel]
    brush_model_count: int | None = None
    layer_data: bytes = b""
    # v6: per primary light, the static geometry of its shadow map and its region
    shadow_lights: list = field(default_factory=list)


@dataclass(slots=True)
class LightRegionHull:
    kdop_mid_point: tuple[float, ...]
    kdop_half_size: tuple[float, ...]
    axes: list  # (dir xyz, mid point, half size)


@dataclass(slots=True)
class ShadowLight:
    surfaces: list[int]  # WaW surface indices drawn into the light's shadow map
    smodels: list[int]  # WaW static model indices
    hulls: list[LightRegionHull]


def layer_format(world_vert_format: int) -> tuple[int, int]:
    """(texcoord count, normal count) of MTL_WORLDVERT_TEX_<t>_NRM_<n>
    (t 1..5, n 1..min(t, 3)); the same enum in WaW and T6."""
    pairs = [(t, n) for t in range(1, 6) for n in range(1, min(t, 3) + 1)]
    if not 0 <= world_vert_format < len(pairs):
        raise FormatError(f"unknown world vertex format {world_vert_format}")
    return pairs[world_vert_format]


def layer_stride(world_vert_format: int) -> int:
    """Bytes per vertex in the WaW layer buffer: t-1 float2 texcoords then n-1
    packed (UBYTE4N, RGBA) normal transforms (verified: the per-surface ranges
    of the measured world end exactly at the buffer size)."""
    texcoords, normals = layer_format(world_vert_format)
    return 8 * (texcoords - 1) + 4 * (normals - 1)


def layer_formats_from_strides(world: "GfxWorld") -> dict[int, int]:
    """World vertex formats of surfaces dumped without one (0xFF), by surface
    index, from the WaW layer buffer: the surfaces' records are packed back to
    back, so the gap to the next surface's records over the vertex range is the
    stride (matches the recorded format on every surface of the measured map).
    A stride that two formats share (24: TEX_3_NRM_3, TEX_4_NRM_1) is left out."""
    layered = [s for s in world.surfaces if s.world_vert_format != 0]
    offsets = sorted({s.layer_data_offset for s in layered})
    following = {o: (offsets[i + 1] if i + 1 < len(offsets) else len(world.layer_data)) for i, o in enumerate(offsets)}
    by_offset: dict[int, list] = {}
    for surface in layered:
        by_offset.setdefault(surface.layer_data_offset, []).append(surface)
    formats_of_stride: dict[int, list[int]] = {}
    for fmt in range(1, 12):
        formats_of_stride.setdefault(layer_stride(fmt), []).append(fmt)
    result = {}
    for offset, group in by_offset.items():
        span = max(s.first_vertex + s.vertex_count for s in group) - min(s.first_vertex for s in group)
        gap = following[offset] - offset
        if not span or gap % span:
            continue
        candidates = formats_of_stride.get(gap // span, [])
        if len(candidates) == 1:
            for surface in group:
                if surface.world_vert_format == 0xFF:
                    result[surface.index] = candidates[0]
    return result


def _color(packed: int) -> tuple[float, float, float, float]:
    """RGBA of a WaW vertex colour. WaW binds it as D3DCOLOR (0xAARRGGBB,
    bytes B G R A); T6 reads its own vertex colour as R8G8B8A8_UNORM (input
    layout table byte_D1F7E0, built in sub_7314B0), so the order is fixed here."""
    return tuple(((packed >> shift) & 0xFF) / 255.0 for shift in (16, 8, 0, 24))  # type: ignore[return-value]


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
        if r.exact(8) != MAGIC or (gfx_version := r.unpack("I")) not in (1, 2, 3, 4, 5, 6):
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
            surfaces.append(Surface(first_vertex, count, tri_count, base_index, material, lightmap, probe, primary, flags, mins, maxs,
                                    index=len(surfaces)))
        models: list[StaticModel] = []
        for _ in range(model_count):
            model_name = r.string()
            origin = r.unpack("3f")
            axis = r.unpack("9f")
            scale, flags = r.unpack("fI")
            cull_dist = r.unpack("f") if gfx_version >= 2 else 0.0
            primary_light = r.unpack("I") if gfx_version >= 3 else 1
            if primary_light > 255:
                raise FormatError("static model primary-light index exceeds byte range")
            models.append(StaticModel(model_name, origin, axis, scale, flags, cull_dist, primary_light))
        brush_count = None
        if gfx_version >= 4:
            brush_count = r.unpack("I")
            if not 1 <= brush_count <= 65536:
                raise FormatError("unreasonable render brush model count")
            for owner in range(brush_count):
                start, count = r.unpack("II")
                if count and start + count > len(surfaces):
                    raise FormatError("brush model surface range exceeds world surfaces")
                if owner:
                    for surface in surfaces[start:start + count]:
                        if surface.brush_model:
                            raise FormatError("overlapping brush model surface ranges")
                        surface.brush_model = owner
        layer_data = b""
        if gfx_version >= 5:
            size = r.unpack("I")
            if size > 256 * 1024 * 1024:
                raise FormatError("unreasonable vertex layer buffer size")
            layer_data = r.exact(size)
            for surface in surfaces:
                surface.layer_data_offset, surface.world_vert_format = r.unpack("iB")
                stride = layer_stride(surface.world_vert_format) if surface.world_vert_format != 0xFF else 0
                if stride and not 0 <= surface.layer_data_offset <= size - stride * surface.vertex_count:
                    raise FormatError(f"surface layer data exceeds the layer buffer ({surface.material})")
        shadow_lights = []
        if gfx_version >= 6:
            for _ in range(r.unpack("I")):
                surface_count, smodel_count = r.unpack("HH")
                light_surfaces = list(struct.unpack(f"<{surface_count}H", r.exact(2 * surface_count)))
                light_smodels = list(struct.unpack(f"<{smodel_count}H", r.exact(2 * smodel_count)))
                if any(i >= len(surfaces) for i in light_surfaces) or any(i >= len(models) for i in light_smodels):
                    raise FormatError("shadow geometry references data outside the world")
                hulls = []
                for _ in range(r.unpack("I")):
                    mid, half = r.unpack("9f"), r.unpack("9f")
                    axes = [(r.unpack("3f"), *r.unpack("2f")) for _ in range(r.unpack("I"))]
                    hulls.append(LightRegionHull(mid, half, axes))
                shadow_lights.append(ShadowLight(light_surfaces, light_smodels, hulls))
        if stream.read(1):
            raise FormatError("render-world file has trailing data (schema mismatch)")
    return GfxWorld(name, base_name, skybox_model, vertices, indices, surfaces, models, brush_count, layer_data,
                    shadow_lights)


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
    # v4: clip material index per collision triangle (0xFFFF = none recorded)
    triangle_materials: list[int] = field(default_factory=list)
    # v5: shared leaf-brush branches are included in the ownership lists.
    brush_ownership_complete: bool = False
    # v6: compiled triEdgeIsWalkable, bit 3t+e for edge e of triangle t
    edge_walkable: bytes = b""
    # v7: terrain aabb tree (material, childCount, first child / partition),
    # collision leafs (first root, root count, terrainContents) and partitions
    # (first triangle, triangle count)
    aabb_trees: list[tuple[int, int, int]] = field(default_factory=list)
    leaf_roots: list[tuple[int, int, int]] = field(default_factory=list)
    partitions: list[tuple[int, int]] = field(default_factory=list)


def read_collision(path: Path) -> CollisionWorld:
    with path.open("rb") as stream:
        r = Reader(stream)
        if r.exact(8) != MAGIC or (version := r.unpack("I")) not in (2, 3, 4, 5, 6, 7):
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
        triangle_materials = []
        if version >= 4:
            if r.unpack("I") != triangle_count:
                raise FormatError("triangle material table does not match the triangle count")
            values = r.unpack(f"{triangle_count}H") if triangle_count else ()
            triangle_materials = [values] if isinstance(values, int) else list(values)
            if any(m != 0xFFFF and m >= material_count for m in triangle_materials):
                raise FormatError("collision triangle references a material outside the material table")
        edge_walkable = b""
        if version >= 6:
            size = r.unpack("I")
            if size != (3 * triangle_count + 31) // 32 * 4:
                raise FormatError("walkable edge table does not match the triangle count")
            edge_walkable = r.exact(size)
        aabb_trees, leaf_roots, partitions = [], [], []
        if version >= 7:
            aabb_trees = [r.unpack("2Hi") for _ in range(r.unpack("I"))]
            leaf_roots = [r.unpack("2Ii") for _ in range(r.unpack("I"))]
            partitions = [r.unpack("2I") for _ in range(r.unpack("I"))]
        if stream.read(1):
            raise FormatError("clip-world file has trailing data (schema mismatch)")
    summary = CollisionSummary(name, material_count, vertex_count, triangle_count, brush_count)
    return CollisionWorld(summary, vertices, indices, materials, brushes, submodels, static_models, triangle_materials,
                          version >= 5, edge_walkable, aabb_trees, leaf_roots, partitions)


def inspect_collision(path: Path) -> CollisionSummary:
    """Validate the complete clip payload while retaining only counts."""
    return read_collision(path).summary

