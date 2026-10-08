import json
from pathlib import Path
import tempfile
import unittest
import zipfile

from waw2bo2.diagnostics import create_bundle
from waw2bo2.launcher import BuildPaths, Settings


class DiagnosticsTests(unittest.TestCase):
    def test_bundle_includes_referenced_cache_logs_and_reports_without_game_assets(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            settings = Settings(work=tmp, project='zm_fixture', fastfile=str(root / 'map.ff'))
            paths = BuildPaths.for_settings(settings)
            paths.root.mkdir(parents=True)
            native = root / 'asset_cache/waw/dependencies/native.log'
            native.parent.mkdir(parents=True)
            native.write_text('missing mesh detail')
            (paths.root / 'build.log').write_text('[tool] Unlinker.exe; log: ' + str(native))
            report = paths.stage / 'zone_raw/zm_fixture/content_source/weapons.dependencies.json'
            report.parent.mkdir(parents=True)
            report.write_text('{"missing":["gun"]}')
            (paths.root / 'map.ff').write_bytes(b'private game content')
            outside = root / 'outside.log'
            outside.write_text('unrelated data')
            bundle = create_bundle(settings, 'failure\n[tool] Tool; log: ' + str(outside), 'failed')
            with zipfile.ZipFile(bundle) as archive:
                self.assertIn('build/build.log', archive.namelist())
                self.assertIn('asset_cache/waw/dependencies/native.log', archive.namelist())
                self.assertTrue(any(n.endswith('weapons.dependencies.json') for n in archive.namelist()))
                self.assertFalse(any(n.endswith('.ff') or n.endswith('outside.log') for n in archive.namelist()))
                self.assertEqual(json.loads(archive.read('summary.json'))['error'], 'failed')

    def test_extraction_failure_works_without_selected_map(self):
        with tempfile.TemporaryDirectory() as tmp:
            settings = Settings(work=tmp)
            log = Path(tmp) / 'asset_cache/extract.log'
            log.parent.mkdir(parents=True)
            log.write_text('extraction failed')
            with zipfile.ZipFile(create_bundle(settings, 'failed')) as archive:
                self.assertEqual(archive.read('asset_cache/extract.log'), b'extraction failed')
