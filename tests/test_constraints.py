import copy
import json
import tempfile
import unittest
from pathlib import Path

from waw2bo2 import constraints, modzone


def rope():
    return dict(type=8, attach_point_type1=2, attach_point_type2=0,
                target_ent1='suspended_model', target_ent2='',
                target_index1=123, target_index2=-1, material=',rope_material',
                offset=[0, 0, 0], pos=[0, 0, -3], pos2=[100, 200, 300],
                dir=[0, 0, 0], scale=[0.15, 0, 0], distance=34,
                damp=0, power=0, spin_scale=0, minAngle=0, maxAngle=0)


class ConstraintTests(unittest.TestCase):
    def test_rope_engine_default_material_is_an_explicit_dependency(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.assertEqual(modzone.rope_asset_lines(root), [])
            self.stage([rope()], root)
            self.assertEqual(modzone.rope_asset_lines(root / 'project'), ['material,mc/rope'])

    def test_script_suspension_uses_authored_world_anchor_and_is_idempotent(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            entry = dict(rope(), material='', flags=0, timeout=0)
            self.stage([entry], root)
            project = root / 'project'
            assets = project / 'maps/mp/waw/_waw2bo2_assets.gsc'
            assets.parent.mkdir(parents=True)
            assets.write_text('init()\n{\n    level.existing = 1;\n}\n')
            entities = [{'classname': 'script_model', 'targetname': 'suspended_model',
                         'origin': '100 200 266', 'angles': '0 90 0'}]
            (project / 'BSP/entities.json').write_text(json.dumps({'entities': entities}))
            report = constraints.stage_script_suspensions(project)
            self.assertEqual(report, {'suspensions': 1, 'unsupported': []})
            first = assets.read_text()
            self.assertIn('suspension.anchor = (100, 200, 300);', first)
            self.assertIn('suspension.point = (0, 0, -3);', first)
            self.assertIn('suspension.length = 34;', first)
            self.assertIn('level.existing = 1;', first)
            constraints.stage_script_suspensions(project)
            self.assertEqual(assets.read_text(), first)
            entry['timeout'] = 1000
            self.stage([entry], root)
            report = constraints.stage_script_suspensions(project)
            self.assertEqual(report, {'suspensions': 0, 'unsupported': ['suspended_model']})
            self.assertNotIn('suspension.anchor', assets.read_text())

    def test_reversed_attachment_and_multiple_joints_are_not_confused(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            entry = dict(rope(), material='', flags=0, timeout=0,
                         attach_point_type1=0, attach_point_type2=2,
                         target_ent1='', target_ent2='suspended_model',
                         pos=[100, 200, 300], pos2=[0, 0, -3])
            self.stage([entry], root)
            project = root / 'project'
            assets = project / 'maps/mp/waw/_waw2bo2_assets.gsc'
            assets.parent.mkdir(parents=True)
            assets.write_text('init()\n{\n}\n')
            (project / 'BSP/entities.json').write_text(json.dumps({'entities': [
                {'classname': 'script_model', 'targetname': 'suspended_model'}]}))
            self.assertEqual(constraints.stage_script_suspensions(project)['suspensions'], 1)
            self.assertIn('suspension.anchor = (100, 200, 300);', assets.read_text())
            self.assertIn('suspension.point = (0, 0, -3);', assets.read_text())
            self.stage([entry, entry], root)
            self.assertEqual(constraints.stage_script_suspensions(project),
                             {'suspensions': 0, 'unsupported': ['suspended_model']})

    def stage(self, entries, root):
        source = root / 'source.json'
        source.write_text(json.dumps(dict(version=1, constraints=entries)))
        return constraints.stage(source, root / 'project')

    def test_named_attachment_rebinds_without_changing_rope_geometry(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            original = rope()
            report = self.stage([original], root)
            self.assertEqual(report, dict(count=1, ropes=1, materials=['rope_material'], exported=True))
            staged = json.loads((root / 'project/BSP/constraints.json').read_text())['constraints'][0]
            expected = copy.deepcopy(original)
            expected.update(target_index1=-1, material='rope_material')
            self.assertEqual(staged, expected)

    def test_unsupported_attachments_and_bad_geometry_fail_explicitly(self):
        changes = [dict(attach_point_type1=1), dict(target_ent1=''),
                   dict(pos=[0, 1]), dict(distance=float('inf'))]
        with tempfile.TemporaryDirectory() as tmp:
            for change in changes:
                with self.subTest(change=change), self.assertRaises(ValueError):
                    self.stage([dict(rope(), **change)], Path(tmp))

    def test_limits_and_missing_source_remove_stale_manifest(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with self.assertRaisesRegex(ValueError, 'rope count'):
                self.stage([rope() for _ in range(301)], root)
            self.stage([rope()], root)
            report = constraints.stage(root / 'missing.json', root / 'project')
            self.assertFalse(report['exported'])
            self.assertFalse((root / 'project/BSP/constraints.json').exists())

    def test_native_rope_reconstructs_removed_endpoint_and_keeps_source_material(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            entry = dict(rope(), targetname='suspension', flags=0, timeout=0)
            entry.update(pos=[1, 0, -3], pos2=[100, 200, 330])
            self.stage([entry], root)
            project = root / 'project'
            source = project / 'materials/rope_material.json'
            source.parent.mkdir()
            source.write_text('{"textures":[{"image":"source_rope"}]}')
            data = {'entities': [{'classname': 'script_model', 'targetname': 'suspended_model',
                                 'origin': '100 200 300', 'angles': '0 90 0'},
                                {'classname': 'rope', 'target': 'compiled_out_endpoint'}]}
            (project / 'BSP/entities.json').write_text(json.dumps(data))
            report = constraints.stage_entities(project)
            self.assertEqual(report, dict(ropes=1, endpoints=1, materials=['mc/waw_rope/rope_material'],
                                          unsupported_joints=[]))
            out = json.loads((project / 'BSP/entities.json').read_text())['entities']
            end = next(e for e in out if e['classname'] == 'info_null')
            self.assertEqual(end['origin'], '100 201 297')
            self.assertEqual(end['target'], 'suspended_model')
            native = next(e for e in out if e['classname'] == 'rope')
            self.assertEqual(native['origin'], '100 200 330')
            self.assertEqual(native['target'], end['targetname'])
            self.assertEqual(native['material'], 'waw_rope/rope_material')
            self.assertEqual((project / 'materials/mc/waw_rope/rope_material.json').read_bytes(), source.read_bytes())

    def test_rope_hanging_a_dynent_binds_by_index_and_targetname(self):
        # Afterwar's test_hanging_corpse / Collingwood: a rope from the world
        # to a dynent (attach_point_type 1, target_index = dynent index)
        corpse = dict(type=1, quat=[0, 0, 0.7071068, 0.7071068], origin=[10, 20, 30], xModel='corpse',
                      targetname='pf_corpse')
        entry = dict(rope(), attach_point_type1=1, target_ent1='', target_index1=0,
                     targetname='', flags=0, timeout=0, pos=[1, 0, 0], pos2=[10, 20, 100])
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with self.assertRaisesRegex(ValueError, 'outside the map dynents'):
                self.stage([entry], root)
            source = root / 'source.json'
            source.write_text(json.dumps(dict(version=1, constraints=[entry])))
            constraints.stage(source, root / 'project', [corpse])
            project = root / 'project'
            staged = json.loads((project / 'BSP/constraints.json').read_text())['constraints'][0]
            self.assertEqual((staged['attach_point_type1'], staged['target_index1']), (1, 0))
            (project / 'BSP/dynents.json').write_text(json.dumps({'version': 1, 'dynents': [corpse]}))
            (project / 'BSP/entities.json').write_text(json.dumps({'entities': []}))
            (project / 'materials').mkdir()
            (project / 'materials/rope_material.json').write_text('{}')
            constraints.stage_entities(project)
            out = json.loads((project / 'BSP/entities.json').read_text())['entities']
            end = next(e for e in out if e['classname'] == 'info_null')
            # yaw 90: the dynent's local +x is world +y
            self.assertEqual([round(float(v), 4) for v in end['origin'].split()], [10, 21, 30])
            self.assertEqual(end['target'], 'pf_corpse')


    def test_point_joint_on_a_hanging_dynent_is_reported_and_its_rope_kept(self):
        # Collingwood's body bags: a rope and a point joint from the same hook
        bag = dict(type=1, quat=[0, 0, 0, 1], origin=[0, 0, 1036], xModel='bodybag', targetname='pf_bag')
        hang = dict(rope(), attach_point_type1=1, target_ent1='pf_bag', target_index1=0,
                    targetname='', flags=0, timeout=0, pos=[0, 30, 0], pos2=[0, 0, 1156])
        pin = dict(hang, type=1, distance=0.0, pos=[0, 112, 0], pos2=[0, 0, 1148])
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / 'source.json'
            source.write_text(json.dumps(dict(version=1, constraints=[hang, pin])))
            constraints.stage(source, root / 'project', [bag])
            project = root / 'project'
            (project / 'BSP/dynents.json').write_text(json.dumps({'version': 1, 'dynents': [bag]}))
            (project / 'BSP/entities.json').write_text(json.dumps({'entities': []}))
            (project / 'materials').mkdir()
            (project / 'materials/rope_material.json').write_text('{}')
            report = constraints.stage_entities(project)
            self.assertEqual(report['ropes'], 1)
            self.assertEqual(report['unsupported_joints'], ['point joint 1 (pf_bag)'])
            out = json.loads((project / 'BSP/entities.json').read_text())['entities']
            self.assertEqual([e['classname'] for e in out].count('rope'), 1)


if __name__ == '__main__':
    unittest.main()
