import struct
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from waw2bo2 import fbx, world


class MovingTerrainTests(unittest.TestCase):
    def collision(self):
        return SimpleNamespace(
            vertices=[(0, 0, 0), (10, 0, 0), (0, 10, 0)],
            indices=[0, 1, 2, 0, 1, 2], triangle_materials=[0, 0],
            materials=[SimpleNamespace(content_flags=1)],
            submodels=[None, None],
            aabb_trees=[(0, 0, 0), (0, 0, 1)],
            partitions=[(0, 1), (1, 1)], leaf_roots=[(0, 1, 32)],
            model_roots=[(0, 0, 0), (1, 1, 32)])

    def test_same_material_and_coordinates_keep_distinct_world_and_mover_owners(self):
        clip = self.collision()
        self.assertEqual(fbx.collision_triangle_models(clip), [0, 1])
        self.assertEqual(fbx.collision_material_slots(clip), ([0, 1], [0, 0]))
        self.assertEqual(fbx.collision_slot_models(clip), [0, 1])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'collision.fbx'
            fbx.write_collision_fbx(clip, path)
            self.assertIn('Material::clip_0_bm1', path.read_text())
            self.assertIn('Material::clip_0"', path.read_text())

    def test_moving_water_keeps_its_triangles_world_water_is_dropped(self):
        clip = self.collision()
        clip.materials = [SimpleNamespace(content_flags=0x20)]
        # world water caps a WaW water brush (dropped); the mover's water is its only collision
        self.assertEqual(fbx.collision_material_slots(clip), ([None, 0], [0]))
        self.assertEqual(fbx.collision_slot_models(clip), [1])

    def test_nested_and_shared_roots_do_not_duplicate_or_infer_ownership(self):
        clip = self.collision()
        clip.aabb_trees.append((0, 1, 1))
        clip.model_roots[1] = (1, 2, 32)
        self.assertEqual(fbx.collision_triangle_models(clip), [0, 1])

    def test_invalid_tree_partition_and_conflicting_ownership_fail(self):
        for mutate in (
            lambda c: c.aabb_trees.__setitem__(1, (0, 1, 1)),
            lambda c: c.aabb_trees.__setitem__(1, (0, 0, 99)),
            lambda c: c.partitions.__setitem__(1, (2, 1)),
            lambda c: c.model_roots.append((1, 1, 32)),
        ):
            clip = self.collision()
            mutate(clip)
            with self.assertRaises(ValueError):
                fbx.collision_triangle_models(clip)

    def test_legacy_moving_terrain_requires_reextraction(self):
        clip = self.collision()
        clip.model_roots = None
        with self.assertRaisesRegex(ValueError, 're-extract'):
            fbx.collision_triangle_models(clip)

    def test_v9_reader_preserves_model_leaf_roots_and_checks_count(self):
        def payload(count):
            # Empty world, one cmodel, v6 edges, v7 trees/leafs/partitions,
            # v8 brush reach and v9 model leaf roots.
            return (world.MAGIC + struct.pack('<II', 9, 0) + struct.pack('<IIII', 0, 0, 0, 0)
                    + struct.pack('<I6fI', 1, -1, -2, -3, 1, 2, 3, 0)
                    + struct.pack('<IIIIIII', 0, 0, 0, 0, 0, 0, 0)
                    + struct.pack('<I', count) + struct.pack('<IIi', 7, 2, 32))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'clip.bin'
            path.write_bytes(payload(1))
            self.assertEqual(world.read_collision(path).model_roots, [(7, 2, 32)])
            path.write_bytes(payload(2))
            with self.assertRaises(world.FormatError):
                world.read_collision(path)
