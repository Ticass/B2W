import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

from waw2bo2 import builddriver
from waw2bo2.launcher import BuildPaths, Settings


class BuildDriverTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.settings = Settings(waw=str(self.root / 'WaW game'), bo2=str(self.root / 'BO2 game'),
            t4=str(self.root / 't4'), t6=str(self.root / 't6'), decoder=str(self.root / 'audio.exe'),
            work=str(self.root / 'builds'), fastfile=str(self.root / 'Map folder/bank_job.ff'),
            project='zm_bankjob')
        for filename in ('bank_job.ff', 'mod.ff', 'bank_job_patch.ff'):
            path = Path(self.settings.fastfile).with_name(filename)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()
        for name in ('common', 'code_post_gfx', 'nazi_zombie_factory'):
            path = Path(self.settings.waw) / 'zone/english' / (name + '.ff')
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()
        stock = Path(self.settings.bo2) / 'zone/all/zm_nuked.ff'
        stock.parent.mkdir(parents=True)
        stock.touch()
        self.linker = Path(self.settings.t6) / 'Linker.exe'
        self.linker.parent.mkdir()
        data = bytearray(128)
        data[:2] = b'MZ'
        struct.pack_into('<I', data, 0x3c, 64)
        data[64:68] = b'PE\0\0'
        self.linker.write_bytes(data)

    def test_pipeline_preserves_sources_runs_audits_and_never_installs(self):
        commands = []
        paths = BuildPaths.for_settings(self.settings)
        def fake_run(command, log=None):
            commands.append(command)
            if 'bridge-link' in command:
                ff = paths.stage / 'zone_out/zm_bankjob/zm_bankjob.ff'
                ff.parent.mkdir(parents=True)
                ff.touch()
                mod = paths.mod / 'out/mod.ff'
                mod.parent.mkdir(parents=True)
                mod.touch()
        with patch.object(builddriver, 'run', side_effect=fake_run):
            self.assertEqual(builddriver.build(self.settings), 0)
        actions = [name for name in ('stage-bridge', 'build-mod', 'compile-scripts', 'bridge-link')
                   if any(name in command for command in commands)]
        self.assertEqual(len(actions), 4)
        self.assertFalse(any('powershell.exe' in command or 'package' in command for command in commands))
        stage = next(command for command in commands if 'stage-bridge' in command)
        self.assertIn(str(Path(self.settings.fastfile).parent), stage)
        self.assertIn('--approximate-sound-curves', stage)
        self.assertNotIn('--waw-source-fx', stage)
        self.assertEqual(stage.count('--extra-root'), 6)
        audits = [c for c in commands if any(Path(arg).name == 'audit_material_args.py' for arg in c)]
        self.assertEqual(len(audits), 2)
        self.assertEqual(struct.unpack_from('<H', self.linker.read_bytes(), 86)[0] & 0x20, 0x20)

    def test_first_native_failure_stops_pipeline_and_surfaces_log(self):
        with patch.object(builddriver, 'run', side_effect=RuntimeError('dump failed')) as run:
            with self.assertRaisesRegex(RuntimeError, 'dump failed'):
                builddriver.build(self.settings)
        self.assertEqual(run.call_count, 1)

    def test_failed_cache_refresh_does_not_retain_completion_marker(self):
        paths = BuildPaths.for_settings(self.settings)
        marker = paths.waw_dumps / 'stock_scripts/.complete'
        marker.parent.mkdir(parents=True)
        marker.touch()
        def fake_run(command, log=None):
            if any('stock_scripts_' in str(arg) for arg in (log,) if arg):
                raise RuntimeError('stock script dump failed')
        with patch.object(builddriver, 'run', side_effect=fake_run):
            with self.assertRaisesRegex(RuntimeError, 'stock script dump failed'):
                builddriver.build(self.settings, redump=True)
        self.assertFalse(marker.exists())

    def test_real_failed_process_is_not_treated_as_success(self):
        log = self.root / 'failure.log'
        with self.assertRaisesRegex(RuntimeError, r'failed \(7\)'):
            builddriver.run([sys.executable, '-c', "print('native diagnostic'); raise SystemExit(7)"], log)
        self.assertIn('native diagnostic', log.read_text())

    def test_invalid_linker_header_is_rejected_without_modification(self):
        self.linker.write_bytes(b'not a PE')
        with self.assertRaises(ValueError):
            builddriver.ensure_laa(self.linker)
        self.assertEqual(self.linker.read_bytes(), b'not a PE')


if __name__ == '__main__':
    unittest.main()
