import json
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path

from waw2bo2.package import package, launch_command


class PackageTests(unittest.TestCase):
    def test_default_launch_opens_online_lobby_and_direct_launch_is_explicit(self):
        with patch.dict('os.environ', {'LOCALAPPDATA': 'C:/example'}):
            command = launch_command('zm_example', Path('C:/BO2'))
            self.assertEqual(command[-3:], ['+set', 'fs_game', 'mods/zm_example'])
            self.assertNotIn('-lan', command)
            self.assertNotIn('+devmap', command)
            direct = launch_command('zm_example', Path('C:/BO2'), direct_map=True)
            self.assertEqual(direct[-3:], ['-lan', '+devmap', 'zm_example'])

    def test_frontend_zone_is_required_and_installed_for_any_map(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            maps, mod, dest = root / "map", root / "mod", root / "installed"
            maps.mkdir()
            mod.mkdir()
            for name in ("zm_example.ff", "zm_example.ipak"):
                (maps / name).write_bytes(name.encode())
            (mod / "mod.ff").write_bytes(b"gameplay")
            with self.assertRaises(FileNotFoundError):
                package("zm_example", maps, mod, dest)
            self.assertFalse(dest.exists())
            (mod / "mod_load.ff").write_bytes(b"frontend")
            package("zm_example", maps, mod, dest)
            self.assertEqual((dest / "mod_load.ff").read_bytes(), b"frontend")
            self.assertIn("zm_example", json.loads((dest / "mod.json").read_text())["name"])

    def test_authored_menu_is_packaged_without_replacing_identical_open_packs(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            maps, mod, dest = root / 'map', root / 'mod', root / 'installed'
            maps.mkdir()
            mod.mkdir()
            for name in ('zm_example.ff', 'zm_example.ipak'):
                (maps / name).write_bytes(name.encode())
            for name in ('mod.ff', 'mod_load.ff', 'zm_example_menu.ipak'):
                (mod / name).write_bytes(name.encode())
            (mod / 'menu_metadata.json').write_text(json.dumps({'title': 'Custom Town', 'description': 'Custom description'}))
            package('zm_example', maps, mod, dest)
            self.assertEqual((dest / 'zm_example_menu.ipak').read_bytes(), b'zm_example_menu.ipak')
            metadata = json.loads((dest / 'mod.json').read_text())
            self.assertEqual((metadata['name'], metadata['description']), ('Custom Town', 'Custom description'))
            with patch('waw2bo2.package.shutil.copy2', side_effect=PermissionError('open image pack')) as copy:
                package('zm_example', maps, mod, dest)
                copy.assert_not_called()
