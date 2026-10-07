from dataclasses import replace
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

from waw2bo2.launcher import (BuildPaths, ProcessRunner, Settings, build_command, discover,
                             input_stamp, map_fastfiles, perform_build, preflight, project_name, validate_project)


class LauncherTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def settings(self):
        values = Settings(waw=str(self.root / 'waw'), bo2=str(self.root / 'bo2'),
            t4=str(self.root / 't4'), t6=str(self.root / 't6'), decoder=str(self.root / 'audio.exe'),
            work=str(self.root / 'build files'), fastfile=str(self.root / 'Map folder/map.ff'), project='zm_map_waw')
        # Create only preflight's actual prerequisite files; no game/native process is run.
        for check in preflight(values, include_map=False):
            if check.detail.startswith('Missing:'):
                field = {'World at War': 'waw', 'Black Ops II + Mod Tools': 'bo2',
                         'WaW extractor': 't4', 'BO2 bridge tools': 't6'}.get(check.name)
                if field:
                    for relative in check.detail.removeprefix('Missing: ').split(', '):
                        path = Path(getattr(values, field)) / relative
                        path.parent.mkdir(parents=True, exist_ok=True)
                        if relative == 'main':
                            path.mkdir()
                        else:
                            path.write_bytes(b'fixture')
        Path(values.decoder).write_bytes(b'fixture')
        Path(values.fastfile).parent.mkdir()
        Path(values.fastfile).write_bytes(b'fixture')
        Path(values.fastfile).with_name('mod.ff').write_bytes(b'fixture')
        from waw2bo2.all2raw import CachePaths
        caches = CachePaths.for_settings(values)
        for root in (caches.waw, caches.bo2):
            root.mkdir(parents=True)
            (root / 'all2raw.json').write_text('{}')
        return values

    def test_settings_roundtrip_and_corrupt_file(self):
        path = self.root / 'settings.json'
        settings = Settings(waw='D:/Games/WaW', source_fx=True)
        settings.save(path)
        self.assertEqual(Settings.load(path), settings)
        path.write_text('{broken')
        self.assertEqual(Settings.load(path).waw, '')
        path.write_text(json.dumps({'waw': 17, 'source_fx': 'false', 'unknown': 'ignored'}))
        self.assertEqual(Settings.load(path).waw, '')
        self.assertFalse(Settings.load(path).source_fx)

    def test_project_names_cannot_escape_build_directory(self):
        self.assertEqual(project_name('nazi_zombie_My Map.ff'), 'zm_my_map_waw')
        for name in ['../outside', 'zm_../../outside', 'zm_bad name', 'zm_bad;exit', '']:
            with self.assertRaises(ValueError):
                validate_project(name)

    def test_bundled_paths_are_rediscovered_after_portable_folder_moves(self):
        from waw2bo2.resources import resource_root
        settings = Settings(t4=str(resource_root() / 'vendor/OpenAssetTools/build/bin/Release_x86'))
        path = self.root / 'settings.json'
        settings.save(path)
        self.assertEqual(Settings.load(path).t4, '')
        settings.t4 = 'D:/Custom extractor'
        settings.save(path)
        self.assertEqual(Settings.load(path).t4, settings.t4)

    def test_map_picker_excludes_companion_fastfiles(self):
        for name in ['mod.ff', 'map_patch.ff', 'map.ff', 'mod_load.ff', 'other.ff']:
            (self.root / name).touch()
        self.assertEqual([p.name for p in map_fastfiles(self.root)], ['map.ff', 'other.ff'])

    def test_preflight_reports_missing_game_data_and_optional_source_tools(self):
        settings = self.settings()
        self.assertFalse([c for c in preflight(settings) if c.required and not c.ready])
        self.assertFalse(next(c for c in preflight(settings) if c.name == 'WaW source tools').required)
        Path(settings.fastfile).with_name('mod.ff').unlink()
        failures = [c.name for c in preflight(replace(settings, source_fx=True)) if c.required and not c.ready]
        self.assertIn('Companion mod.ff', failures)
        self.assertIn('WaW source tools', failures)

    def test_build_caches_isolate_source_maps_and_game_installations(self):
        settings = self.settings()
        paths = BuildPaths.for_settings(settings)
        self.assertNotEqual(paths.root, BuildPaths.for_settings(replace(settings, fastfile=str(self.root / 'other/map.ff'))).root)
        self.assertNotEqual(paths.root, BuildPaths.for_settings(replace(settings, bo2='D:/Other BO2')).root)
        self.assertEqual(paths.root.parent, Path(settings.work).resolve())

    def test_command_preserves_spaces_and_never_installs_during_build(self):
        settings = self.settings()
        command = build_command(settings)
        payload = json.loads(command[command.index('--settings-json') + 1])
        self.assertEqual(payload['fastfile'], settings.fastfile)
        self.assertFalse(payload['source_fx'])
        self.assertIn('build-map', command)
        self.assertNotIn('powershell.exe', command)
        self.assertNotIn('--redump', command)
        paths = BuildPaths.for_settings(settings)
        paths.root.mkdir(parents=True)
        (paths.root / 'cache.json').write_text(json.dumps(input_stamp(settings)))
        self.assertNotIn('--redump', build_command(settings))
        Path(settings.fastfile).write_bytes(b'changed source')
        self.assertNotIn('--redump', build_command(settings))
        self.assertIn('--redump', build_command(replace(settings, redump=True)))

    def test_real_child_process_streams_output_and_propagates_failure(self):
        events = []
        runner = ProcessRunner(lambda event, text: events.append((event, text)))
        log = self.root / 'run.log'
        code = runner.run([sys.executable, '-c', "print('native error'); raise SystemExit(7)"], log)
        self.assertEqual(code, 7)
        self.assertIn(('line', 'native error'), events)
        self.assertIn('native error', log.read_text())

    def test_cancel_before_start_does_not_spawn_process(self):
        runner = ProcessRunner(lambda *_: None)
        runner.cancel()
        self.assertEqual(runner.run(['nonexistent-tool'], self.root / 'run.log'), -1)

    def test_quiet_process_reports_activity_and_stops_monitor_when_done(self):
        events = []
        runner = ProcessRunner(lambda event, text: events.append(text), heartbeat_interval=0.05)
        log = self.root / 'quiet.log'
        code = runner.run([sys.executable, '-u', '-c',
                           "import time; print('Decoding audio'); time.sleep(0.3)"], log)
        self.assertEqual(code, 0)
        activity = [line for line in events if line.startswith('[activity]')]
        self.assertTrue(activity)
        self.assertIn('Last output: Decoding audio', activity[-1])
        self.assertIn(activity[-1], log.read_text())
        self.assertFalse(any(t.name == 'build-activity' for t in threading.enumerate()))

    def test_verbose_setting_reaches_worker_environment(self):
        events = []
        runner = ProcessRunner(lambda event, text: events.append(text), verbose=True)
        code = runner.run([sys.executable, '-c',
                           "import os; print(os.environ['WAW2BO2_VERBOSE'])"], self.root / 'verbose.log')
        self.assertEqual(code, 0)
        self.assertIn('1', events)

    def test_worker_preflight_uses_explicit_shared_cache_roots(self):
        from waw2bo2.all2raw import CachePaths
        settings = self.settings()
        paths = CachePaths(self.root / 'mapped_waw_cache', self.root / 'mapped_bo2_cache')
        for root in (paths.waw, paths.bo2):
            root.mkdir()
            (root / 'all2raw.json').write_text('{}')
        shared = next(c for c in preflight(settings, cache_paths=paths) if c.name == 'Shared game assets')
        self.assertTrue(shared.ready)

    def test_cancel_running_process_exits_and_stream_remains_responsive(self):
        started = threading.Event()
        codes = []
        runner = ProcessRunner(lambda *_: started.set())
        worker = threading.Thread(target=lambda: codes.append(runner.run(
            [sys.executable, '-u', '-c', "import time; print('started'); time.sleep(20)"], self.root / 'cancel.log')))
        worker.start()
        self.assertTrue(started.wait(5))
        runner.cancel()
        worker.join(5)
        self.assertFalse(worker.is_alive())
        self.assertNotEqual(codes, [0])

    def test_failed_rebuild_invalidates_previous_success(self):
        settings = self.settings()
        paths = BuildPaths.for_settings(settings)
        paths.root.mkdir(parents=True)
        (paths.root / 'build.json').write_text(json.dumps({'project': settings.project}))
        runner = ProcessRunner(lambda *_: None)
        with patch.object(runner, 'run', return_value=12):
            self.assertEqual(perform_build(settings, runner), 12)
        self.assertFalse((paths.root / 'build.json').exists())
        self.assertFalse(paths.complete(settings.project))

    def test_completion_requires_successful_package_and_required_outputs(self):
        settings = self.settings()
        paths = BuildPaths.for_settings(settings)
        runner = ProcessRunner(lambda *_: None)
        def fake_native(command, log):
            if 'package' in command:
                paths.output.mkdir()
                for name in (settings.project + '.ff', settings.project + '.ipak', 'mod.ff', 'mod_load.ff', 'mod.json'):
                    (paths.output / name).write_bytes(b'fixture')
            else:
                native_map = paths.stage / 'zone_out' / settings.project
                native_map.mkdir(parents=True)
                (paths.mod / 'out').mkdir(parents=True)
                for suffix in ('.ff', '.ipak'):
                    (native_map / (settings.project + suffix)).write_bytes(b'fixture')
                for name in ('mod.ff', 'mod_load.ff'):
                    (paths.mod / 'out' / name).write_bytes(b'fixture')
            return 0
        with patch.object(runner, 'run', side_effect=fake_native):
            self.assertEqual(perform_build(settings, runner), 0)
        self.assertTrue(paths.complete(settings.project))
        (paths.output / 'mod.ff').unlink()
        self.assertFalse(paths.complete(settings.project))

    def test_successful_noop_driver_does_not_attempt_packaging(self):
        settings = self.settings()
        paths = BuildPaths.for_settings(settings)
        runner = ProcessRunner(lambda *_: None)
        with patch.object(runner, 'run', return_value=0) as run:
            with self.assertRaisesRegex(ValueError, 'worker did not execute'):
                perform_build(settings, runner)
        self.assertEqual(run.call_count, 1)
        self.assertFalse((paths.root / 'build.json').exists())

    def test_partial_native_outputs_do_not_attempt_packaging(self):
        settings = self.settings()
        paths = BuildPaths.for_settings(settings)
        native_map = paths.stage / 'zone_out' / settings.project
        native_map.mkdir(parents=True)
        for suffix in ('.ff', '.ipak'):
            (native_map / (settings.project + suffix)).write_bytes(b'fixture')
        runner = ProcessRunner(lambda *_: None)
        with patch.object(runner, 'run', return_value=0) as run:
            with self.assertRaisesRegex(ValueError, 'mod_load.ff'):
                perform_build(settings, runner)
        self.assertEqual(run.call_count, 1)
        self.assertFalse((paths.root / 'build.json').exists())


if __name__ == '__main__':
    unittest.main()
