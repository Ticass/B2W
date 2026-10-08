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
        self.stock = self.root / 'stock'
        self.stock.mkdir()
        (self.stock / 't6api_cache.json').write_text('{}')
        self.addCleanup(patch.stopall)
        patch.object(builddriver.all2raw, 'ready', return_value=self.stock).start()
        patch.object(builddriver.all2raw, 'source_dumps', return_value={
            'map': self.root / 'source_map', 'mod': self.root / 'source_mod',
            'patch': self.root / 'source_patch'}).start()

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
        self.assertEqual(stage.count('--extra-root'), 4)
        # The only native Unlinker invocations during a build verify newly built
        # outputs. No installed stock FF is extracted or listed.
        unlinks = [c for c in commands if Path(c[0]).name == 'Unlinker.exe']
        self.assertEqual(len(unlinks), 2)
        self.assertTrue(all(str(paths.root) in c[-1] for c in unlinks))
        audits = [c for c in commands if any(Path(arg).name == 'audit_material_args.py' for arg in c)]
        self.assertEqual(len(audits), 2)
        self.assertEqual(struct.unpack_from('<H', self.linker.read_bytes(), 86)[0] & 0x20, 0x20)

    def test_first_native_failure_stops_pipeline_and_surfaces_log(self):
        with patch.object(builddriver, 'run', side_effect=RuntimeError('dump failed')) as run:
            with self.assertRaisesRegex(RuntimeError, 'dump failed'):
                builddriver.build(self.settings)
        self.assertEqual(run.call_count, 1)

    def test_failed_cache_preparation_stops_before_any_native_build(self):
        with patch.object(builddriver.all2raw, 'ready', side_effect=RuntimeError('Run Extract All')), \
             patch.object(builddriver.all2raw, 'prepare', side_effect=RuntimeError('Extraction failed')), \
             patch.object(builddriver, 'run') as run:
            with self.assertRaisesRegex(RuntimeError, 'Extraction failed'):
                builddriver.build(self.settings)
        run.assert_not_called()

    def test_stale_cache_is_prepared_once_and_build_continues(self):
        # WaW stays reusable; only the initially stale BO2 cache is refreshed.
        with patch.object(builddriver.all2raw, 'ready', side_effect=[self.stock, RuntimeError('stale')]), \
             patch.object(builddriver.all2raw, 'prepare', return_value=self.stock) as prepare, \
             patch.object(builddriver, '_build', return_value=0) as pipeline:
            self.assertEqual(builddriver.build(self.settings), 0)
        self.assertEqual(prepare.call_count, 1)
        self.assertEqual(prepare.call_args.kwargs['engine'], 'T6')
        pipeline.assert_called_once()

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


class WorldNameTests(unittest.TestCase):
    def test_map_world_found_inside_mod_ff(self):
        with tempfile.TemporaryDirectory() as tmp:
            maps = Path(tmp) / 'waw2bo2/maps'
            maps.mkdir(parents=True)
            (maps / 'nazi_zombie_cellar.d3dbsp.gfx.bin').write_bytes(b'')
            self.assertEqual(builddriver.world_name(Path(tmp), 'mod'), 'nazi_zombie_cellar')
            (maps / 'mod.d3dbsp.gfx.bin').write_bytes(b'')
            self.assertEqual(builddriver.world_name(Path(tmp), 'mod'), 'mod')    # the usual case wins

    def test_several_worlds_need_the_map_fastfile(self):
        with tempfile.TemporaryDirectory() as tmp:
            maps = Path(tmp) / 'waw2bo2/maps'
            maps.mkdir(parents=True)
            for name in ('a', 'b'):
                (maps / f'{name}.d3dbsp.gfx.bin').write_bytes(b'')
            with self.assertRaisesRegex(RuntimeError, 'several map worlds'):
                builddriver.world_name(Path(tmp), 'mod')
