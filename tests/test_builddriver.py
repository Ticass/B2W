import os
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

from waw2bo2 import builddriver
from waw2bo2.launcher import BuildPaths, Settings


class BuildWorkspace:
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


class BuildDriverTests(BuildWorkspace, unittest.TestCase):
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
        # Windows temp paths may use an 8.3 alias (RUNNER~1); the driver
        # resolves the selected fastfile before passing its directory to staging.
        iwd_dirs = [stage[index + 1] for index, arg in enumerate(stage) if arg == '--iwd-dir']
        self.assertIn(str(Path(self.settings.fastfile).resolve().parent), iwd_dirs)
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

    def test_authored_hitbox_loss_fails_the_build_automatically(self):
        paths = BuildPaths.for_settings(self.settings)
        source = paths.stage / 'zone_raw/zm_bankjob/xmodel/body.json'
        source.parent.mkdir(parents=True)
        head = {'bone': 'j_head', 'mins': [-1, -2, -3], 'maxs': [1, 2, 3],
                'offset': [0, 0, 0], 'radiusSquared': 14, 'hitloc': 'head'}
        source.write_text(json.dumps({'type': 'animated', 'boneInfo': [head]}))
        commands = []
        def fake_run(command, log=None):
            commands.append(command)
            if 'bridge-link' in command:
                for ff in (paths.stage / 'zone_out/zm_bankjob/zm_bankjob.ff', paths.mod / 'out/mod.ff'):
                    ff.parent.mkdir(parents=True, exist_ok=True)
                    ff.touch()
            if Path(command[0]).name == 'Unlinker.exe':
                output = Path(command[command.index('--output-folder') + 1]) / 'xmodel/body.json'
                output.parent.mkdir(parents=True)
                output.write_text(json.dumps({'boneInfo': [dict(head, radiusSquared=0)]}))
        with patch.object(builddriver, 'run', side_effect=fake_run):
            with self.assertRaisesRegex(RuntimeError, 'Authored bone hitboxes changed'):
                builddriver.build(self.settings)
        result = json.loads((paths.stage / 'bone_hitbox_audit.json').read_text())
        self.assertIn('radiusSquared changed', result['errors'][0])
        unlinks = [c for c in commands if Path(c[0]).name == 'Unlinker.exe']
        self.assertEqual(len(unlinks), 1)
        self.assertIn('xmodel', unlinks[0][unlinks[0].index('--include-assets') + 1])

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


class ResumeTests(BuildWorkspace, unittest.TestCase):
    """Resume keeps the last staging (and the user's edits to it) and reruns
    the steps after it; it never reuses a staging from other inputs."""
    def _pipeline(self, settings, *, fail=None):
        commands = []
        paths = BuildPaths.for_settings(settings)
        script = paths.stage / 'zone_raw/zm_bankjob/maps/mp/waw/_zombiemode_spawner.gsc'
        def fake_run(command, log=None):
            commands.append(command)
            if fail and fail in command:
                raise RuntimeError(fail + ' failed')
            if 'stage-bridge' in command:
                script.parent.mkdir(parents=True, exist_ok=True)
                script.write_text('// translated from WaW\n')
            if 'bridge-link' in command:
                for ff in (paths.stage / 'zone_out/zm_bankjob/zm_bankjob.ff', paths.mod / 'out/mod.ff'):
                    ff.parent.mkdir(parents=True, exist_ok=True)
                    ff.touch()
        with patch.object(builddriver, 'run', side_effect=fake_run):
            if fail:
                with self.assertRaisesRegex(RuntimeError, fail):
                    builddriver.build(settings)
            else:
                self.assertEqual(builddriver.build(settings), 0)
        return [name for name in ('stage-bridge', 'build-mod', 'compile-scripts', 'bridge-link')
                if any(name in command for command in commands)], script

    def test_resume_keeps_edited_staged_scripts_and_reruns_later_steps(self):
        from dataclasses import replace
        steps, script = self._pipeline(self.settings, fail='compile-scripts')
        self.assertEqual(steps, ['stage-bridge', 'build-mod', 'compile-scripts'])
        script.write_text('// user commented out the failing line\n')
        resume = replace(self.settings, resume=True)
        with patch('sys.stdout') as out:
            steps, _ = self._pipeline(resume)
        self.assertEqual(steps, ['build-mod', 'compile-scripts', 'bridge-link'])
        self.assertEqual(script.read_text(), '// user commented out the failing line\n')
        printed = ''.join(str(c.args[0]) for c in out.write.call_args_list if c.args)
        self.assertIn('1 edited', printed)
        self.assertIn('maps/mp/waw/_zombiemode_spawner.gsc', printed)

    def test_default_build_always_restages(self):
        _, script = self._pipeline(self.settings)
        script.write_text('// edit\n')
        steps, _ = self._pipeline(self.settings)
        self.assertEqual(steps[0], 'stage-bridge')
        self.assertEqual(script.read_text(), '// translated from WaW\n')

    def test_changed_options_or_failed_staging_are_never_resumed(self):
        from dataclasses import replace
        self._pipeline(self.settings)
        # another build option changes the stage-bridge inputs
        steps, _ = self._pipeline(replace(self.settings, resume=True, remaster=True))
        self.assertEqual(steps[0], 'stage-bridge')
        # a staging that failed half-way leaves no receipt to resume from
        self._pipeline(self.settings, fail='stage-bridge')
        steps, _ = self._pipeline(replace(self.settings, resume=True))
        self.assertEqual(steps[0], 'stage-bridge')

    def test_converter_change_invalidates_the_staging(self):
        from dataclasses import replace
        self._pipeline(self.settings)
        with patch.object(builddriver, 'converter_digest', return_value='other converter'):
            steps, _ = self._pipeline(replace(self.settings, resume=True))
        self.assertEqual(steps[0], 'stage-bridge')

    def test_resume_setting_round_trips(self):
        path = self.root / 'settings.json'
        Settings(resume=True).save(path)
        self.assertTrue(Settings.load(path).resume)



class ModFrameworkTests(unittest.TestCase):
    def test_zct_and_ugx_maps_are_named_not_refused(self):
        # Yote Tribute ships zct_mod2_mw.iwd; its scripts port like any map's
        with tempfile.TemporaryDirectory() as tmp:
            mod = Path(tmp) / 'yote_tribute'
            mod.mkdir()
            (mod / 'Zct_mod2_mw.iwd').write_bytes(b'')
            (mod / 'other.iwd').write_bytes(b'')
            self.assertEqual(builddriver.mod_frameworks(mod / 'yote_tribute.ff'), ['ZCT'])
            (mod / 'ugx_mod.iwd').write_bytes(b'')
            self.assertEqual(builddriver.mod_frameworks(mod / 'yote_tribute.ff'), ['UGX', 'ZCT'])
            self.assertFalse(hasattr(Settings(), 'allow_zct'))
