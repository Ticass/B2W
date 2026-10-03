import struct
import tempfile
import unittest
from pathlib import Path

from waw2bo2.world import FormatError, read_gfx_world
from waw2bo2.fbx import merge_surfaces


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
