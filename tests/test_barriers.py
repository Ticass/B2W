import json
import struct
import tempfile
import unittest
from pathlib import Path
from waw2bo2 import barriers


class BarrierTests(unittest.TestCase):
    def test_repaired_delta_is_read_from_final_key_not_a_guessed_offset(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'repair'
            # 4 frames, two translation keys at 0 and 3, no delta rotation.
            path.write_bytes(struct.pack('<3H2BH2H2BB6f6H', 19, 4, 1, 2, 2, 30,
                                         0, 2, 0, 3, 0, -10, 0, -20, 40, 0, 60,
                                         0, 0, 0, 65535, 0, 65535))
            self.assertEqual(barriers.repaired_translation(path), (30, 0, 40))
            path.write_bytes(struct.pack('<3H2BH2H3f', 19, 4, 1, 2, 2, 30, 0, 1, 33, 0, 44))
            self.assertEqual(barriers.repaired_translation(path), (33, 0, 44))

    def test_boards_match_source_plane_without_moving_goal_or_entry(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'raw/xanim').mkdir(parents=True)
            (root / 'raw/xanim/repair').write_bytes(
                struct.pack('<3H2BH2H3f', 19, 4, 1, 2, 2, 30, 0, 1, 30, 0, 40))
            goal = {'targetname': 'exterior_goal', 'target': 'win', 'origin': '10 20 0', 'angles': '0 90 0'}
            entry = {'classname': 'script_struct', 'targetname': 'win', 'origin': '10 68 0'}
            board = {'classname': 'script_brushmodel', 'targetname': 'win', 'script_noteworthy': '1',
                     'origin': '10 66 40'}
            barrier = {'classname': 'zbarrier_zmcore_BasicWoodBarrier', 'targetname': 'win',
                       'origin': '10 68 0', 'zbarriernumboards': '1', 'zbarrierboardanim1': 'repair'}
            path = root / 'entities.json'
            path.write_text(json.dumps({'entities': [goal, entry, board, barrier]}))
            result = barriers.align_native_boards(root, root)
            es = json.loads(path.read_text())['entities']
            self.assertEqual(es[0], goal)
            self.assertEqual(es[1], entry)
            origin = [float(x) for x in es[3]['origin'].split()]
            self.assertAlmostEqual(origin[1] + 30, 66)
            self.assertLess(origin[1], 66)  # native attack slots stay outside
            self.assertEqual(result['aligned'][0]['root_advance'], 16)
            self.assertEqual(result['repair_poses']['repair'], (30, 0, 40))
            barriers.align_native_boards(root, root)
            self.assertEqual(json.loads(path.read_text())['entities'], es)

    def test_unnamed_boards_align_and_barrier_stands_tear_height_above_floor(self):
        from waw2bo2.world import Brush, CollisionSummary, CollisionWorld, SubModel
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'raw/xanim').mkdir(parents=True)
            (root / 'raw/xanim/repair').write_bytes(
                struct.pack('<3H2BH2H3f', 19, 4, 1, 2, 2, 30, 0, 1, 30, 0, 40))
            (root / 'raw/xanim' / barriers.TEAR_ANIM).write_bytes(
                struct.pack('<3H2BH2H3f', 19, 4, 1, 2, 2, 30, 0, 1, 6, 0, -31))
            # Project X: boards carry no number, the goal floats 16 above the floor
            goal = {'targetname': 'exterior_goal', 'target': 'win', 'origin': '0 0 -552', 'angles': '0 0 0'}
            boards = [{'classname': 'script_brushmodel', 'targetname': 'win', 'origin': '50 0 -506'},
                      {'classname': 'script_brushmodel', 'targetname': 'win', 'script_noteworthy': 'window_south_1',
                       'origin': '50 0 -490'},
                      {'classname': 'script_brushmodel', 'targetname': 'win', 'script_noteworthy': 'clip',
                       'origin': '200 0 -508'}]
            barrier = {'classname': 'zbarrier_zmcore_BasicWoodBarrier', 'targetname': 'win',
                       'origin': '0 0 -552', 'zbarriernumboards': '1', 'zbarrierboardanim1': 'repair'}
            path = root / 'entities.json'
            path.write_text(json.dumps({'entities': [goal, *boards, barrier]}))
            # floor: one quad at -568, a sill brush at the wall the barrier must not land on
            sill = Brush((40, -32, -568), (56, 32, -536), 1,
                         [((-1, 0, 0), -40, 0), ((1, 0, 0), 56, 0), ((0, -1, 0), 32, 0),
                          ((0, 1, 0), 32, 0), ((0, 0, -1), 568, 0), ((0, 0, 1), -536, 0)], [])
            clip = CollisionWorld(CollisionSummary('m', 0, 4, 2, 1),
                                  [(-500, -500, -568), (500, -500, -568), (500, 500, -568), (-500, 500, -568)],
                                  [0, 1, 2, 0, 2, 3], brushes=[sill], submodels=[SubModel((0, 0, 0), (0, 0, 0), [])])
            clip_file = root / 'fixture.clip.bin'
            clip_data = struct.pack('<8sII', b'W2BSP001', 2, 1) + b'm'
            clip_data += struct.pack('<II', 0, len(clip.vertices))
            clip_data += b''.join(struct.pack('<3f', *vertex) for vertex in clip.vertices)
            clip_data += struct.pack('<I6H', 2, *clip.indices)
            clip_data += struct.pack('<I6fII', 1, *sill.mins, *sill.maxs, sill.contents, len(sill.planes))
            clip_data += b''.join(struct.pack('<4fI', *normal, distance, material) for normal, distance, material in sill.planes)
            clip_file.write_bytes(clip_data + struct.pack('<I', 0))
            result = barriers.align_native_boards(root, root, clip, clip_source=clip_file)
            es = json.loads(path.read_text())['entities']
            origin = [float(x) for x in es[4]['origin'].split()]
            self.assertAlmostEqual(origin[0], 20)  # boards (not the clip brush) at 50 - native 30
            self.assertAlmostEqual(origin[2], -568 + 31)
            self.assertAlmostEqual(result['aligned'][0]['lift'], 15)
            barriers.align_native_boards(root, root, clip, clip_source=clip_file)
            self.assertEqual(json.loads(path.read_text())['entities'], es)
            _, chunks = barriers.stage_native_boards(root, root, clip, clip_file)
            self.assertEqual(chunks, {'windows': 1, 'dropped_brushes': 3})
            self.assertEqual(json.loads(path.read_text())['entities'], [es[0], es[-1]])
