import base64
import json
import struct
import tempfile
import unittest
from pathlib import Path

from waw2bo2 import modelcolours


def gltf_with(colours, liquid_indices, glass_indices):
    data = bytearray()
    for colour in colours:
        data += struct.pack("<4f", *colour)
    colour_len = len(data)
    liquid_at = len(data)
    data += struct.pack(f"<{len(liquid_indices)}H", *liquid_indices)
    glass_at = len(data)
    data += struct.pack(f"<{len(glass_indices)}H", *glass_indices)
    return {
        "buffers": [{"uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(data)).decode()}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": colour_len},
                        {"buffer": 0, "byteOffset": liquid_at, "byteLength": 2 * len(liquid_indices)},
                        {"buffer": 0, "byteOffset": glass_at, "byteLength": 2 * len(glass_indices)}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": len(colours), "type": "VEC4"},
                      {"bufferView": 1, "componentType": 5123, "count": len(liquid_indices), "type": "SCALAR"},
                      {"bufferView": 2, "componentType": 5123, "count": len(glass_indices), "type": "SCALAR"}],
        "materials": [{"name": "mc/glass"}, {"name": "mc/liquid"}],
        "meshes": [{"primitives": [{"attributes": {"COLOR_0": 0}, "indices": 2, "material": 0},
                                   {"attributes": {"COLOR_0": 0}, "indices": 1, "material": 1}]}],
    }


def colours_of(gltf):
    data = base64.b64decode(gltf["buffers"][0]["uri"].split(",", 1)[1])
    return [struct.unpack_from("<4f", data, i * 16) for i in range(gltf["accessors"][0]["count"])]


class AdditiveModelVertexAlphaTests(unittest.TestCase):
    def test_only_additive_model_materials_are_selected(self):
        materials = [{"name": "mc/liquid", "t4_techset": "mc_unlit_add"},
                     {"name": "mc/renamed", "source": "mc/orig", "t4_techset": "mc_unlit_add"},
                     {"name": "mc/glass", "t4_techset": "mc_l_sm_b0c0s0"},
                     {"name": "wc/add", "t4_techset": "wc_unlit_add"}]
        self.assertEqual(modelcolours.additive_materials(materials), {"mc/liquid", "mc/orig"})

    def test_liquid_vertices_draw_at_alpha_one_and_keep_their_colour(self):
        green = (0.918, 0.988, 0.302, 0.114)
        grey = (0.757, 0.745, 0.733, 0.5)
        gltf = gltf_with([grey, grey, grey, green, green, green], [3, 4, 5], [0, 1, 2])
        self.assertEqual(modelcolours.opaque_vertex_alpha(gltf, {"mc/liquid"}), 3)
        colours = colours_of(gltf)
        for vertex in (3, 4, 5):
            self.assertAlmostEqual(colours[vertex][3], 1.0)
            self.assertAlmostEqual(colours[vertex][1], 0.988, places=5)
        for vertex in (0, 1, 2):   # the glass shares the accessor and keeps its alpha
            self.assertAlmostEqual(colours[vertex][3], 0.5)

    def test_stage_rewrites_files_without_touching_hard_link_targets(self):
        root = Path(tempfile.mkdtemp())
        cache = root / "cache.gltf"
        cache.write_text(json.dumps(gltf_with([(1, 1, 1, 0.2)] * 3, [0, 1, 2], [])), encoding="utf-8")
        staged = root / "project/model_export/jar_lod0.gltf"
        staged.parent.mkdir(parents=True)
        try:
            staged.hardlink_to(cache)
        except OSError:
            self.skipTest("no hard links here")
        result = modelcolours.stage(root / "project", [{"name": "mc/liquid", "t4_techset": "mc_unlit_add"}])
        self.assertEqual(result, {"model_export/jar_lod0.gltf": 3})
        self.assertAlmostEqual(colours_of(json.loads(cache.read_text()))[0][3], 0.2, places=5)
        self.assertAlmostEqual(colours_of(json.loads(staged.read_text()))[0][3], 1.0)


if __name__ == "__main__":
    unittest.main()
