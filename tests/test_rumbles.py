import json
from pathlib import Path
import tempfile
import unittest

from waw2bo2 import rumbles, weapons


class RumbleTests(unittest.TestCase):
    def test_weapon_profile_and_both_graphs_keep_map_priority(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            project, source, stock = root / 'out', root / 'map', root / 'stock'
            (project / 'content_source/weapons').mkdir(parents=True)
            (project / 'content_source/weapons.runtime.json').write_text(json.dumps({'weapons': ['custom']}))
            (project / 'content_source/weapons/custom').write_text(weapons.write_info({'fireRumble': 'flame'}))
            profile = r'RUMBLE\lowRumbleFile\lo.rmb\highRumbleFile\hi.rmb\duration\0.5'
            for base, graph in [(source, 'authored graph'), (stock, 'wrong graph')]:
                (base / 'rumble').mkdir(parents=True)
                (base / 'rumble/flame').write_text(profile)
                for name in ('lo.rmb', 'hi.rmb'):
                    (base / 'rumble' / name).write_text(graph)
            result = rumbles.stage(project, [source, stock])
            self.assertEqual(result['missing'], [])
            self.assertEqual(result['files'], ['rumble/flame', 'rumble/hi.rmb', 'rumble/lo.rmb'])
            self.assertEqual((project / 'rumble/hi.rmb').read_text(), 'authored graph')

    def test_missing_graph_is_reported_and_native_framework_profiles_are_available(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            project, native = root / 'out', root / 'native'
            (project / 'maps/mp/waw').mkdir(parents=True)
            (project / 'maps/mp/waw/example.gsc').write_text('f(){ precacheRumble("native"); }')
            (native / 'rumble').mkdir(parents=True)
            (native / 'rumble/native').write_text(r'RUMBLE\highRumbleFile\missing.rmb')
            result = rumbles.stage(project, [], native)
            self.assertEqual(result['missing'], ['rumble/missing.rmb'])
            self.assertEqual(result['native_profiles'], ['native'])
