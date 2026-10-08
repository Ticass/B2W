import json
from pathlib import Path
import tempfile
import unittest

from waw2bo2 import all2raw, paths, t6bridge


def node():
    return dict(type=1, spawnflags=0, targetname='', script_linkname='', script_noteworthy='',
                target='', animscript='', animscriptfunc=0, origin=[0, 0, 0], angle=0,
                forward=[1, 0], radius=0, minUseDistSq=0, overlap=[-1, -1], links=[])


class BuildRecoveryTests(unittest.TestCase):
    def test_visibility_partial_final_byte_is_preserved(self):
        # The reported Bank Job count: 530*529 bits = 35046 bytes + 2 bits.
        count = 530
        for size in (35046, 35047):
            with self.subTest(size=size):
                vis = 'ab' * size
                result, _, summary = paths.convert_paths(dict(nodeCount=count, nodes=[node() for _ in range(count)],
                                                              visBytes=size, pathVis=vis))
                self.assertEqual(result['pathVis'], vis)
                self.assertEqual(summary.vis_bytes, size)
        with self.assertRaises(paths.PathError):
            paths.convert_paths(dict(nodeCount=count, nodes=[node() for _ in range(count)],
                                    visBytes=35048, pathVis='ab' * 35048))
        with self.assertRaises(paths.PathError):
            paths.convert_paths(dict(nodeCount=count, nodes=[node() for _ in range(count)],
                                    visBytes=35047, pathVis='ab' * 35046))

    def test_entities_resolve_from_the_ordinary_dump_with_source_priority(self):
        self.assertIn('mapents', all2raw.WAW_ASSETS.split(','))
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            maproot, modroot = root / 'map', root / 'mod'
            gfx = maproot / 'waw2bo2/maps/bank_job.d3dbsp.gfx.bin'
            expected = maproot / 'maps/bank_job.d3dbsp.ents'
            for owner in (maproot, modroot):
                p = owner / 'maps/bank_job.d3dbsp.ents'
                p.parent.mkdir(parents=True)
                p.write_text('{"classname" "worldspawn"}')
            self.assertEqual(t6bridge.source_entities(gfx, root / 'stage', [maproot, modroot]), expected)
            expected.unlink()
            self.assertEqual(t6bridge.source_entities(gfx, root / 'stage', [maproot, modroot]),
                             modroot / 'maps/bank_job.d3dbsp.ents')

    def test_template_without_server_ambient_is_complete(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            template, output = root / 'template', root / 'output'
            for folder, pattern in t6bridge.SCRIPT_FILES:
                if folder == 'maps/mp' and pattern == '{p}_amb.gsc':
                    continue  # Matches the shipped BO2 zm_test template.
                p = template / folder / pattern.format(p='bridge')
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text('main() {}')
            report = t6bridge.StageReport('zm_fixture')
            t6bridge.stage_scripts(report, 'zm_fixture', output, template, 'bridge')
            self.assertEqual(report.errors, [])
            self.assertTrue((output / 'maps/mp/zm_fixture_amb.gsc').is_file())
