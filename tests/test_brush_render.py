import struct
import tempfile
import unittest
from pathlib import Path

from waw2bo2.world import FormatError, read_gfx_world
from waw2bo2.fbx import merge_surfaces, write_world_fbx


class BrushRenderTests(unittest.TestCase):
    def fixture(self, ranges):
        text = lambda value: struct.pack('<I', len(value)) + value
        data = b'W2BSP001' + struct.pack('<5I', 4, 0, 0, 3, 0)
        data += text(b'map') + text(b'map') + text(b'')
        for _ in range(3):
            data += struct.pack('<IHHI', 0, 0, 0, 0) + text(b'wall')
            data += struct.pack('<4B6f', 0, 0, 1, 0, 0, 0, 0, 1, 1, 1)
        return data + struct.pack('<I', len(ranges)) + b''.join(struct.pack('<II', *r) for r in ranges)

    def read(self, ranges):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'world.bin'
            path.write_bytes(self.fixture(ranges))
            return read_gfx_world(path)

    def test_brush_ownership_survives_and_prevents_static_merging(self):
        world = self.read([(0, 1), (1, 1), (0xffffffff, 0), (2, 1)])
        self.assertEqual([s.brush_model for s in world.surfaces], [0, 1, 3])
        self.assertEqual(len(merge_surfaces(world)), 3)

    def test_invalid_and_overlapping_ranges_are_rejected(self):
        for ranges in ([(0, 1), (3, 1)], [(0, 1), (1, 2), (2, 1)]):
            with self.subTest(ranges=ranges), self.assertRaises(FormatError):
                self.read(ranges)

    def test_shadow_helpers_keep_geometry_and_carry_no_draw_metadata(self):
        world = self.read([(0, 3)])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'world.fbx'
            ordinary = write_world_fbx(world, path)
            self.assertNotIn('_nd1_', path.read_text())
            hidden = write_world_fbx(world, path, shadow_only_materials=frozenset({'wall'}))
            self.assertEqual(hidden, ordinary)
            self.assertIn('_nd1_bm0_pl1_rp0_lm0', path.read_text())

    def test_occluders_of_other_lights_become_no_draw_copies(self):
        import dataclasses
        from waw2bo2.t6bridge import shadow_occluder_copies
        from waw2bo2.world import ShadowLight
        world = self.read([(0, 1), (0xffffffff, 0), (0xffffffff, 0), (1, 2)])
        world.surfaces = [dataclasses.replace(s, index=i, primary_light_index=1) for i, s in enumerate(world.surfaces)]
        world.surfaces[1].brush_model = 0
        # light 2 lists surface 0 (own-light caster of light 1), surface 1 and
        # a brush model surface; light 1 lists its own surface 0
        world.surfaces[2].brush_model = 1
        world.shadow_lights = [ShadowLight([], [], []), ShadowLight([0], [], []), ShadowLight([0, 1, 2], [], [])]
        copies = shadow_occluder_copies(world, {'wall'})
        self.assertEqual([(c.primary_light_index, c.shadow_copy, c.index) for c in copies], [(2, True, -1), (2, True, -1)])
        world.surfaces += copies
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'world.fbx'
            meshes = write_world_fbx(world, path)
            self.assertIn('_nd1_bm0_pl2_', path.read_text())
            self.assertNotIn(-1, meshes)
