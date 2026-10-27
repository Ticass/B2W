import json
import tempfile
import unittest
from pathlib import Path

from waw2bo2 import dynents


def dynent(**change):
    entry = dict(type=1, quat=[0, 0, 0, 1], origin=[1, 2, 3], xModel='com_bottle1', brushModel=0,
                 physicsBrushModel=0, destroyFx='', destroyPieces=None,
                 physPreset={'name': ',bottle_plastic'}, physConstraints=[255, 255, 255, 255],
                 health=0, centerOfMass=[0, 0, 0], flags=0, contents=1, targetname='')
    entry.update(change)
    return entry


def empty():
    return dynent(type=0, xModel='', physPreset=None)


class DynentTests(unittest.TestCase):
    def source(self, root, models, brushes=(), count=None):
        path = root / 'map.dynents.json'
        path.write_text(json.dumps({'version': 1, 'originalDynEntCount': len(models) if count is None else count,
                                    'dynEntCount': [len(models) + 256, len(brushes), 0, 0],
                                    'lists': [[*models, *[empty()] * 256], list(brushes)]}))
        return path

    def test_map_dynents_keep_source_order_and_their_assets(self):
        # Abandoned School: 23 clutter props, every build dropped them
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            entries = dynents.read(self.source(root, [dynent(), dynent(xModel='school_basketball',
                                   physPreset={'name': 'default'}, destroyFx='props/glass', type=2,
                                   physConstraints=[0, 1, 255, 255], targetname='pf_ball')]))
            self.assertEqual([e['xModel'] for e in entries], ['com_bottle1', 'school_basketball'])
            self.assertEqual(dynents.preset_name(entries[0]), 'bottle_plastic')
            report = dynents.write(root / 'project', entries,
                                   {'bottle_plastic': 'waw_physpreset/bottle_plastic',
                                    'default': 'waw_physpreset/default'}, 'waw/')
            self.assertEqual(report['count'], 2)
            self.assertEqual((report['clutter'], report['destructible']), (1, 1))
            out = json.loads((root / 'project/BSP/dynents.json').read_text())['dynents']
            self.assertEqual(out[0]['physPreset'], 'waw_physpreset/bottle_plastic')
            self.assertEqual(out[0]['physConstraints'], [-1, -1, -1, -1])
            self.assertEqual(out[1]['physConstraints'], [0, 1, -1, -1])
            self.assertEqual(out[1]['destroyFx'], 'waw/props/glass')
            self.assertEqual(out[1]['targetname'], 'pf_ball')

    def test_what_is_not_converted_yet_fails_explicitly(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with self.assertRaisesRegex(ValueError, 'brush-model dynents'):
                dynents.read(self.source(root, [], [dynent(xModel='', brushModel=3)]))
            with self.assertRaisesRegex(ValueError, 'destroy pieces'):
                dynents.read(self.source(root, [dynent(destroyPieces={'name': 'p', 'pieces': []})]))
            with self.assertRaisesRegex(ValueError, 'invalid quat'):
                dynents.read(self.source(root, [dynent(quat=[0, 0, 1])]))
            self.assertEqual(dynents.read(root / 'absent.json'), [])

    def test_a_map_without_dynents_removes_a_stale_manifest(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            dynents.write(root, [dynent()], {}, 'waw/')
            self.assertTrue((root / 'BSP/dynents.json').exists())
            self.assertEqual(dynents.write(root, [], {}, 'waw/'), {'count': 0})
            self.assertFalse((root / 'BSP/dynents.json').exists())


if __name__ == '__main__':
    unittest.main()
