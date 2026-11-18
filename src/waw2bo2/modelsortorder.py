"""Draw order of translucent model surfaces that share a WaW sort key.

WaW sorts translucent surfaces by material sort key; surfaces of one model
with the same key draw in the model's surface order. Der Riese's song-egg
jars carry the glass (surface 0) and the liquid (surface 1) both at WaW sort
key 43, so the liquid draws over the glass. The converted materials take
their T6 sort key from different donors (liquid 40, glass 41): the glass
then covered the liquid and washed its green out to a pale teal (playtest
2026-11-11). A later surface whose T6 key sorts before an earlier surface
with the same WaW key is raised just past it.
"""
import json
from pathlib import Path

from . import modelcolours, native

MAX_SORT_KEY = 63


def _blended(material: dict) -> bool:
    """Translucent: drawn in a T6 transparent camera region with blending."""
    if not material.get("cameraRegion", "").endswith("Trans"):
        return False
    return any(s.get("colorWriteRgb") and not s.get("polymodeLine")
               and s.get("dstBlendRgb", "zero") != "zero"
               for s in material.get("stateBits", []))


def surface_orders(project: Path) -> list[list[str]]:
    """Material names of every staged model glTF in surface order."""
    orders = []
    for path in sorted((project / "model_export").rglob("*.gltf")):
        gltf = json.loads(path.read_text(encoding="utf-8"))
        names = [m.get("name") for m in gltf.get("materials", [])]
        order = []
        for mesh in gltf.get("meshes", []):
            for prim in mesh.get("primitives", []):
                index = prim.get("material")
                if index is not None and names[index] not in order:
                    order.append(names[index])
        if len(order) > 1:
            orders.append(order)
    return orders


def raise_keys(orders, waw_keys: dict[str, int], t6_keys: dict[str, int]) -> dict[str, tuple[int, int]]:
    """New T6 sort keys {material: (old, new)} keeping WaW's surface order."""
    keys = dict(t6_keys)
    for _ in range(MAX_SORT_KEY):
        moved = False
        for order in orders:
            for i, later in enumerate(order):
                for earlier in order[:i]:
                    if (later in keys and earlier in keys and later in waw_keys
                            and waw_keys.get(earlier) == waw_keys[later]
                            and keys[later] < keys[earlier] < MAX_SORT_KEY):
                        keys[later] = keys[earlier] + 1
                        moved = True
        if not moved:
            break
    return {name: (t6_keys[name], key) for name, key in keys.items() if key != t6_keys[name]}


def stage(project: Path, materials) -> dict[str, list[int]]:
    """Apply raise_keys to the staged translucent model materials."""
    if native.tool() is not None:
        return modelcolours._native_surface_fixes(project, materials, colors=False, sort=True)['sortKeys']
    entries = {m.get("source", m["name"]): m for m in materials
               if "t4_sort_key" in m and m.get("t4_techset", "").startswith("mc_")}
    files, waw_keys, t6_keys = {}, {}, {}
    for name, entry in entries.items():
        path = project / entry["file"]
        if not path.is_file():
            continue
        material = json.loads(path.read_text(encoding="utf-8"))
        if "sortKey" not in material or not _blended(material):
            continue
        files[name] = (path, material)
        waw_keys[name] = entry["t4_sort_key"]
        t6_keys[name] = material["sortKey"]
    changed = raise_keys(surface_orders(project), waw_keys, t6_keys)
    for name, (_, new) in changed.items():
        path, material = files[name]
        material["sortKey"] = new
        path.unlink()  # staged files may be hard links into the shared asset cache
        path.write_text(json.dumps(material, indent=2) + "\n", encoding="utf-8")
    return {name: list(keys) for name, keys in changed.items()}
