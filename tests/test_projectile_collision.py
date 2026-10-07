import base64
import copy
import json
import struct
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from waw2bo2 import hulls, projectilecollision, t6bridge


class ProjectilePropTests(unittest.TestCase):
    def test_visible_pickets_block_but_gap_and_movement_barrier_do_not(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'BSP').mkdir()
            (root / 'xmodel').mkdir()
            (root / 'materials').mkdir()
            # Two physical planks, leaving a real four-unit window between them.
            points = [(0, y, z) for lo, hi in ((-8, -2), (2, 8))
                      for y, z in ((lo, 0), (hi, 0), (hi, 12), (lo, 0), (hi, 12), (lo, 12))]
            raw = b''.join(struct.pack('<3f', x, z, -y) for x, y, z in points)
            gltf = {'buffers': [{'uri': 'data:application/octet-stream;base64,' + base64.b64encode(raw).decode()}],
                    'bufferViews': [{'buffer': 0, 'byteLength': len(raw)}],
                    'accessors': [{'bufferView': 0, 'componentType': 5126, 'type': 'VEC3', 'count': len(points)}],
                    'nodes': [{'mesh': 0}], 'materials': [{'name': 'plank'}],
                    'meshes': [{'primitives': [{'attributes': {'POSITION': 0}, 'material': 0}]}]}
            (root / 'mesh.gltf').write_text(json.dumps(gltf))
            (root / 'materials/plank.json').write_text(json.dumps({'surfaceTypeBits': 1 << 20}))
            model = {'lods': [{'file': 'mesh.gltf', 'distance': 100}], 'contents': 0,
                     'rootBoneName': 'tag_origin', 'type': 'rigid'}
            (root / 'xmodel/fence.json').write_text(json.dumps(model))
            existing = {'name': 'authored', 'contents': 1}
            (root / 'BSP/staticmodels.json').write_text(json.dumps({'staticModels': [existing]}))
            brush = SimpleNamespace(contents=0x8030200, mins=(-1, -10, 0), maxs=(1, 10, 10),
                                    planes=[((-1, 0, 0), 1, 0), ((1, 0, 0), 1, 0),
                                            ((0, -1, 0), 10, 0), ((0, 1, 0), 10, 0),
                                            ((0, 0, -1), 0, 0), ((0, 0, 1), 10, 0)])
            clip = SimpleNamespace(brushes=[brush], submodels=[SimpleNamespace(brushes=[0])],
                                   brush_ownership_complete=True)
            placement = SimpleNamespace(name='fence', origin=(20, 30, 40),
                                        axis=(0, 1, 0, -1, 0, 0, 0, 0, 1), scale=2)
            # First exercise a rotated, scaled placement against a world-space clip.
            brush.mins, brush.maxs = (0, 28, 40), (40, 32, 60)
            brush.planes = [((-1, 0, 0), 0, 0), ((1, 0, 0), 40, 0),
                            ((0, -1, 0), -28, 0), ((0, 1, 0), 32, 0),
                            ((0, 0, -1), -40, 0), ((0, 0, 1), 60, 0)]
            before = copy.deepcopy(brush.__dict__)
            world = SimpleNamespace(static_models=[placement])
            report = t6bridge.StageReport('example')
            names = projectilecollision.recover(report, world, clip, root, [root])
            self.assertEqual(len(names), 1)
            proxy = json.loads((root / 'xmodel' / (next(iter(names)) + '.json')).read_text())
            self.assertEqual(proxy['contents'], 0x80)
            self.assertEqual(proxy['contents'] & 0x2818011, 0)  # player collision unchanged
            self.assertEqual(proxy['collLod'], 0)
            surf = proxy['collSurfs'][0]
            self.assertAlmostEqual(surf['maxs'][2], 12)  # physical mesh extends above the player clip
            self.assertEqual(len(surf['tris']), 8)  # every sheet blocks from both sides
            triangles = [hulls.collision_triangle(tuple(t['plane'] + t['svec'] + t['tvec'])) for t in surf['tris']]
            def hit(start, end):
                # T6's one-sided plane crossing and barycentric narrow phase.
                for tri in surf['tris']:
                    n, distance = tri['plane'][:3], tri['plane'][3]
                    ds, de = hulls._dot(n, start) - distance, hulls._dot(n, end) - distance
                    if ds <= 0 or de >= 0:
                        continue
                    fraction = ds / (ds - de)
                    p = tuple(start[k] + fraction * (end[k] - start[k]) for k in range(3))
                    s = hulls._dot(tri['svec'][:3], p) - tri['svec'][3]
                    t = hulls._dot(tri['tvec'][:3], p) - tri['tvec'][3]
                    if s >= -1e-6 and t >= -1e-6 and s + t <= 1.000001:
                        return True
                return False
            for y in (-5, 5):
                for z in (1, 9, 11):
                    self.assertTrue(hit((-3, y, z), (3, y, z)))
                    self.assertTrue(hit((3, y, z), (-3, y, z)))
                    self.assertTrue(hit((-3, y-1, z), (3, y+1, z)))
            self.assertFalse(hit((-3, 0, 11), (3, 0, 11)))
            self.assertFalse(hit((3, 0, 11), (-3, 0, 11)))
            self.assertTrue(any(all(-8.01 <= p[1] <= -1.99 for p in tri) for tri in triangles))
            self.assertTrue(any(all(1.99 <= p[1] <= 8.01 for p in tri) for tri in triangles))
            self.assertTrue(all(not (min(p[1] for p in tri) < 0 < max(p[1] for p in tri)) for tri in triangles))
            records = json.loads((root / 'BSP/staticmodels.json').read_text())['staticModels']
            self.assertEqual(records[0], existing)
            self.assertEqual(records[1]['invScaledAxis'], [v / 2 for v in placement.axis])
            self.assertEqual(brush.__dict__, before)
            self.assertEqual(json.loads((root / 'xmodel/fence.json').read_text()), model)
            projectilecollision.recover(report, world, clip, root, [root])
            self.assertEqual(len(json.loads((root / 'BSP/staticmodels.json').read_text())['staticModels']), 2)
            # Batching preserves both rotated/scaled surfaces in world space.
            second = copy.copy(placement)
            second.origin = (20, 31, 40)
            world.static_models.append(second)
            batched_names = projectilecollision.recover(report, world, clip, root, [root])
            self.assertEqual(len(batched_names), 1)
            batch = json.loads((root / 'xmodel' / (next(iter(batched_names)) + '.json')).read_text())
            self.assertEqual(len(batch['collSurfs']), 2)
            records = json.loads((root / 'BSP/staticmodels.json').read_text())['staticModels']
            place = hulls.Placement(tuple(records[1]['origin']), tuple(records[1]['invScaledAxis']))
            for surface, y in zip(batch['collSurfs'], (30, 31)):
                for tri in surface['tris']:
                    points = [place.apply(p) for p in hulls.collision_triangle(tuple(tri['plane'] + tri['svec'] + tri['tvec']))]
                    self.assertTrue(all(abs(p[1] - y) < 1e-5 for p in points))
                    self.assertTrue(max(p[0] for p in points) <= 16.001 or min(p[0] for p in points) >= 23.999)
            # Two 64-placement chunks with identical source models in one cell
            # must retain different world-space geometry rather than overwrite.
            world.static_models = [copy.copy(placement) for _ in range(64)] + [copy.copy(second) for _ in range(64)]
            batch_names = projectilecollision.recover(report, world, clip, root, [root])
            self.assertEqual(len(batch_names), 2)
            self.assertEqual(len(json.loads((root / 'BSP/staticmodels.json').read_text())['staticModels']), 3)
            rows = report.content['projectile_prop_collision']
            self.assertNotEqual(rows[0]['name'], rows[64]['name'])
            # An invisible movement wall with no physical material cannot produce a proxy.
            (root / 'materials/plank.json').write_text(json.dumps({'surfaceTypeBits': 1 << 7}))  # foliage
            self.assertEqual(projectilecollision.recover(report, world, clip, root, [root]), set())
            # Imported opaque default surfaces qualify, but alpha cards do not.
            (root / 'materials/plank.json').write_text(json.dumps({'surfaceTypeBits': 0, 'sortKey': 4,
                                                                 'techniqueSet': 'mc_l_sm_r0c0n0'}))
            self.assertEqual(len(projectilecollision.hard_triangles(root / 'mesh.gltf', [root])), 4)
            (root / 'materials/plank.json').write_text(json.dumps({'surfaceTypeBits': 0, 'sortKey': 4,
                                                                 'techniqueSet': 'mc_l_sm_t0c0n0'}))
            self.assertEqual(projectilecollision.hard_triangles(root / 'mesh.gltf', [root]), [])
            model['collSurfs'] = [surf]
            (root / 'xmodel/fence.json').write_text(json.dumps(model))
            (root / 'materials/plank.json').write_text(json.dumps({'surfaceTypeBits': 1 << 20}))
            self.assertEqual(projectilecollision.recover(report, world, clip, root, [root]), set())

    def test_synthesized_entity_box_is_enabled_without_promoting_authored_collision(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source, output = root / 'source', root / 'output'
            (source / 'xmodel').mkdir(parents=True)
            (source / 'mesh.gltf').write_text(json.dumps({
                'meshes': [{'primitives': [{'attributes': {'POSITION': 0}}]}],
                'accessors': [{'min': [-1, 0, -2], 'max': [1, 4, 2]}]}))
            model = {'lods': [{'file': 'mesh.gltf', 'distance': 100}], 'contents': 0}
            (source / 'xmodel/door.json').write_text(json.dumps(model))
            authored = {**model, 'contents': 0x30000, 'collLod': 0, 'collSurfs': [{'contents': 0x30000}]}
            (source / 'xmodel/authored.json').write_text(json.dumps(authored))
            (root / 'partclassification.csv').write_text('')
            report = t6bridge.StageReport('example')
            with patch.object(t6bridge, 'OAT_T6_RAW', root):
                t6bridge.stage_models(report, SimpleNamespace(static_models=[]), 'example', source, output,
                                      {'door', 'authored'}, [source], {'door', 'authored'})
            door = json.loads((output / 'xmodel/door.json').read_text())
            self.assertEqual(door['collLod'], 0)
            self.assertEqual(door['contents'], 0x2080)
            preserved = json.loads((output / 'xmodel/authored.json').read_text())
            self.assertEqual(preserved['contents'], 0x30000)
            self.assertEqual(preserved['collSurfs'], authored['collSurfs'])


if __name__ == '__main__':
    unittest.main()
