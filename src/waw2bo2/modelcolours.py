"""Vertex colours of additive WaW model surfaces.

WaW's model additive technique (mc_unlit_add: vs vertcol_simple_fog_dtex,
ps vertcol_simple_add_fog) computes rgb * vertex.rgb * (texture.a * vertex.a).
Der Riese's song-egg jar liquid carries vertex alpha 0.114 (29/255) on a
strongly green vertex colour; with that alpha the converted liquid is an
invisible tint, while players remember it as plainly toxic green (playtest
2026-11-11). Those surfaces keep their vertex colour and draw with vertex
alpha 1: the colour is the authored look, the alpha is not honoured.
"""
import base64
import json
import struct
from pathlib import Path

ADDITIVE_MODEL_TECHSETS = ("mc_unlit_add",)


def additive_materials(materials) -> set[str]:
    """glTF material names (WaW names) of additive model materials."""
    return {m.get("source", m["name"]) for m in materials
            if m.get("t4_techset", "").startswith(ADDITIVE_MODEL_TECHSETS)}


def _buffer(gltf: dict) -> tuple[str, bytearray] | None:
    uri = gltf.get("buffers", [{}])[0].get("uri", "")
    if not uri.startswith("data:") or "," not in uri:
        return None
    head, data = uri.split(",", 1)
    return head, bytearray(base64.b64decode(data))


def opaque_vertex_alpha(gltf: dict, materials: set[str]) -> int:
    """Set COLOR_0 alpha to 1 on the vertices drawn by ``materials``.
    Returns the number of vertices changed."""
    names = [m.get("name") for m in gltf.get("materials", [])]
    targets = [p for mesh in gltf.get("meshes", []) for p in mesh.get("primitives", [])
               if p.get("material") is not None and names[p["material"]] in materials
               and "COLOR_0" in p.get("attributes", {}) and p.get("indices") is not None]
    if not targets:
        return 0
    buffer = _buffer(gltf)
    if buffer is None:
        return 0
    head, data = buffer
    changed = 0
    for prim in targets:
        colour = gltf["accessors"][prim["attributes"]["COLOR_0"]]
        if colour.get("componentType") != 5126 or colour.get("type") != "VEC4":
            continue
        view = gltf["bufferViews"][colour["bufferView"]]
        base = view.get("byteOffset", 0) + colour.get("byteOffset", 0)
        stride = view.get("byteStride", 16)
        index = gltf["accessors"][prim["indices"]]
        iview = gltf["bufferViews"][index["bufferView"]]
        fmt = {5121: "B", 5123: "H", 5125: "I"}[index["componentType"]]
        ibase = iview.get("byteOffset", 0) + index.get("byteOffset", 0)
        for vertex in set(struct.unpack_from(f"<{index['count']}{fmt}", data, ibase)):
            at = base + vertex * stride + 12
            if struct.unpack_from("<f", data, at)[0] != 1.0:
                struct.pack_into("<f", data, at, 1.0)
                changed += 1
    if changed:
        gltf["buffers"][0]["uri"] = head + "," + base64.b64encode(bytes(data)).decode()
    return changed


def stage(project: Path, materials) -> dict[str, int]:
    """Apply opaque_vertex_alpha to every staged model glTF."""
    wanted = additive_materials(materials)
    result: dict[str, int] = {}
    if not wanted:
        return result
    for path in sorted((project / "model_export").rglob("*.gltf")):
        gltf = json.loads(path.read_text(encoding="utf-8"))
        changed = opaque_vertex_alpha(gltf, wanted)
        if changed:
            # staged files may be hard links into the shared asset cache
            path.unlink()
            path.write_text(json.dumps(gltf), encoding="utf-8")
            result[path.relative_to(project).as_posix()] = changed
    return result
