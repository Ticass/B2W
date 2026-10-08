import json
import struct
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from waw2bo2 import hulls, t6bridge


class ScriptModelBoxTests(unittest.TestCase):
    def test_box_triangles_follow_waw_convention(self):
        surf = hulls.bounds_box_collsurf((-10.0, -20.0, 0.0), (10.0, 20.0, 50.0), "tag_origin")
        self.assertEqual(surf["contents"], 0x2080)
        self.assertEqual(len(surf["tris"]), 12)
        centre = (0.0, 0.0, 25.0)
        for tri in surf["tris"]:
            a, b, c = hulls.collision_triangle(tuple(tri["plane"]) + tuple(tri["svec"]) + tuple(tri["tvec"]))
            normal = tri["plane"][:3]
            # outward, and (c - a) x (b - a) like every measured WaW collSurf triangle
            self.assertGreater(hulls._dot(normal, hulls._sub(a, centre)), 0)
            self.assertGreater(hulls._dot(hulls._cross(hulls._sub(c, a), hulls._sub(b, a)), normal), 0)
            for p in (a, b, c):
                self.assertTrue(-10.01 <= p[0] <= 10.01 and -20.01 <= p[1] <= 20.01 and -0.01 <= p[2] <= 50.01)

    def test_flat_model_box_gets_minimum_thickness(self):
        surf = hulls.bounds_box_collsurf((-10.0, 5.0, 0.0), (10.0, 5.0, 50.0), "tag_origin")
        self.assertEqual(len(surf["tris"]), 12)
        self.assertEqual((surf["mins"][1], surf["maxs"][1]), (4.5, 5.5))

    def test_gltf_bounds_use_measured_t4_export_axes(self):
        gltf = {"meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
                "accessors": [{"min": [-1.0, 0.0, -3.0], "max": [2.0, 10.0, 4.0]}]}
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "m.gltf"
            path.write_text(json.dumps(gltf), encoding="utf-8")
            mins, maxs = t6bridge._gltf_game_bounds(path)
        self.assertEqual(mins, (-1.0, -4.0, 0.0))
        self.assertEqual(maxs, (2.0, 3.0, 10.0))


class WalkableEdgeTests(unittest.TestCase):
    def test_source_edge_bits_follow_their_triangle(self):
        # triangle 0 has no material (dropped); triangle 1 keeps edges 0 and 2
        clip = SimpleNamespace(
            vertices=[(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (5.0, 5.0, 5.0)],
            indices=[0, 1, 2, 1, 3, 2], triangle_materials=[0xFFFF, 0],
            materials=[SimpleNamespace(content_flags=1)],
            edge_walkable=bytes([0b00101111, 0, 0, 0]))
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "edges.bin"
            summary = t6bridge.write_collision_edges(clip, path)
            data = path.read_bytes()
        self.assertEqual(struct.unpack_from("<I", data)[0], 1)
        record = struct.unpack_from("<9fB", data, 4)
        self.assertEqual(record[:3], (1.0, 0.0, 0.0))
        self.assertEqual(record[9], 0b101)  # bits 3 and 5 of the table
        self.assertEqual(summary["collision_edges_walkable"], 2)

    def test_pre_v6_dump_reports_missing_bits(self):
        clip = SimpleNamespace(edge_walkable=b"")
        with tempfile.TemporaryDirectory() as tmp:
            summary = t6bridge.write_collision_edges(clip, Path(tmp) / "edges.bin")
        self.assertIn("none", summary["collision_edges_source"])


if __name__ == "__main__":
    unittest.main()
