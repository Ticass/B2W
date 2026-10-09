from dataclasses import asdict
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from waw2bo2 import linuxruntime
from waw2bo2.launcher import BuildPaths, Settings


class LinuxRuntimeTests(unittest.TestCase):
    def test_bridge_converts_paths_and_preserves_the_frontend_workspace(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'wine_runtime').mkdir()
            (root / 'wine_runtime/WawConverter.CLI.exe').touch()
            settings = Settings(waw='/games/WaW', bo2='/games/BO2', work=directory,
                                fastfile='/maps/Bank Job/bank_job.ff', project='zm_bankjob',
                                menu_large='/art/image with spaces.png', menu_title='Bank Job')
            paths = BuildPaths.for_settings(settings)
            with patch.object(linuxruntime, 'resource_root', return_value=root), \
                 patch.object(linuxruntime, 'wine', return_value='/runner/bin/wine'), \
                 patch.object(linuxruntime, 'configure_localappdata'), \
                 patch.object(linuxruntime, 'windows_path', side_effect=lambda p: 'mapped:' + str(p)):
                command = linuxruntime.build_command(settings, paths, redump=True)
            self.assertEqual(command[0], '/runner/bin/wine')
            payload = json.loads(command[command.index('--settings-json') + 1])
            self.assertEqual(payload['fastfile'], 'mapped:' + settings.fastfile)
            self.assertEqual(payload['menu_large'], 'mapped:' + settings.menu_large)
            self.assertEqual(payload['menu_title'], 'Bank Job')
            self.assertEqual(command[command.index('--build-root') + 1], 'mapped:' + str(paths.root))
            self.assertIn('--redump', command)
            self.assertEqual(asdict(settings)['fastfile'], '/maps/Bank Job/bank_job.ff')

    def test_missing_wine_is_actionable(self):
        with patch.object(linuxruntime.shutil, 'which', return_value=None):
            with self.assertRaisesRegex(RuntimeError, 'Wine is required'):
                linuxruntime.wine()

    def test_unix_localappdata_is_not_passed_to_windows_tools(self):
        with patch.dict(os.environ, {'LOCALAPPDATA': '/unix/appdata', 'WINEPREFIX': '/prefix'}):
            environment = linuxruntime.wine_environment()
        self.assertNotIn('LOCALAPPDATA', environment)
        self.assertEqual(environment['WINEPREFIX'], '/prefix')

    def test_game_launch_converts_the_game_folder_only(self):
        with patch.object(linuxruntime, 'wine', return_value='wine'), \
             patch.object(linuxruntime, 'windows_path', return_value='Z:\\games\\BO2'):
            command = linuxruntime.launch_command(['/prefix/boot.exe', 't6zm', '/games/BO2',
                                                  '+set', 'fs_game', 'mods/zm_bankjob'])
        self.assertEqual(command, ['wine', '/prefix/boot.exe', 't6zm', 'Z:\\games\\BO2',
                                   '+set', 'fs_game', 'mods/zm_bankjob'])

    def test_wine_defaults_disable_only_fixme_and_preserve_debug_overrides(self):
        with patch.dict(os.environ, {}, clear=True):
            self.assertEqual(linuxruntime.wine_environment()['WINEDEBUG'], '-fixme')
            self.assertNotIn('WINEDEBUG', os.environ)
        for value in ('+file', '', '-all'):
            with self.subTest(value=value), patch.dict(os.environ, {'WINEDEBUG': value}):
                self.assertEqual(linuxruntime.wine_environment()['WINEDEBUG'], value)


if __name__ == '__main__':
    unittest.main()
