import unittest
from types import SimpleNamespace

from waw2bo2 import fbx, oneway, t6bridge
import tempfile
import struct
from pathlib import Path


def _clip(tris, materials):
    verts, idx, mats = [], [], []
    for corners, mat in tris:
        for p in corners:
            idx.append(len(verts))
            verts.append(p)
        mats.append(mat)
    return SimpleNamespace(vertices=verts, indices=idx, triangle_materials=mats,
                           materials=[SimpleNamespace(name=n, content_flags=c) for n, c in materials])


# a vertical sheet at y = 0 whose WaW front, (v2 - v0) x (v1 - v0), faces +y
SHEET = ((0.0, 0.0, 0.0), (100.0, 0.0, 0.0), (0.0, 0.0, 100.0))


class OneWayTests(unittest.TestCase):
    def test_single_clip_sheet_is_one_way_with_waw_front(self):
        # Nuketown: the clip sheet south of the spawn lets players back in from the south only
        sheets = oneway.one_way_sheets(_clip([(SHEET, 0)], [("clip", 0x30200)]))
        self.assertEqual(len(sheets), 1)
        self.assertAlmostEqual(sheets[0]["normal"][1], 1.0)
        self.assertAlmostEqual(sheets[0]["dist"], 0.0)
        # inside test: the centroid is inside every edge plane
        centroid = tuple(sum(p[i] for p in SHEET) / 3 for i in range(3))
        for e, k in sheets[0]["edges"]:
            self.assertGreater(sum(e[i] * centroid[i] for i in range(3)) - k, 0)
        source = oneway.oneway_source(sheets)
        self.assertNotIn("watch", source)
        self.assertNotIn("setorigin", source)
        self.assertNotIn("add(", source)

    def test_two_sided_solid_and_floor_triangles_are_not_one_way(self):
        back = (SHEET[0], SHEET[2], SHEET[1])
        floor = ((0.0, 0.0, 0.0), (100.0, 0.0, 0.0), (0.0, 100.0, 0.0))
        clip = _clip([(SHEET, 0), (back, 0), (SHEET, 1), (floor, 0), (SHEET, 2)],
                     [("clip", 0x30200), ("solid", 0x1), ("missileclip", 0x2080)])
        self.assertEqual(oneway.one_way_sheets(clip), [])
        self.assertNotIn("add(", oneway.oneway_source([]))

    def test_export_removes_one_sided_sheet_and_its_edges_only(self):
        floor = ((0.0, 0.0, 0.0), (100.0, 0.0, 0.0), (0.0, 100.0, 0.0))
        clip = _clip([(SHEET, 0), (floor, 0)], [("clip", 0x30200)])
        clip.edge_walkable = bytes([0b111111])
        self.assertEqual(fbx.collision_material_slots(clip), ([None, 0], [0]))
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            fbx.write_collision_fbx(clip, path / 'collision.fbx')
            text = (path / 'collision.fbx').read_text()
            self.assertIn('PolygonVertexIndex: *3', text)
            result = t6bridge.write_collision_edges(clip, path / 'edges.bin')
            self.assertEqual(struct.unpack_from('<I', (path / 'edges.bin').read_bytes())[0], 1)
            self.assertEqual(result['collision_edges_total'], 3)
            self.assertEqual(result['collision_edges_walkable'], 3)

    def test_same_facing_duplicates_are_removed_but_opposing_faces_remain(self):
        clip = _clip([(SHEET, 0), (SHEET, 0)], [("clip", 0x30200)])
        self.assertEqual(fbx.collision_material_slots(clip), ([None, None], []))
        back = (SHEET[0], SHEET[2], SHEET[1])
        clip = _clip([(SHEET, 0), (back, 0)], [("clip", 0x30200)])
        self.assertEqual(fbx.collision_material_slots(clip), ([0, 0], [0]))


if __name__ == "__main__":
    unittest.main()
