import csv
import subprocess
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from waw2bo2 import modzone


class ModZoneTests(unittest.TestCase):
    def test_authored_branding_survives_stock_table_rebuilds(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            project = root / 'project'
            project.mkdir()
            (project / 'menu.json').write_text(json.dumps({'title': 'Test Town',
                'description': 'A ruined town.', 'icon': 'custom_atom', 'blit': 'custom_cutout'}))
            stock = root / 'maps.csv'
            base = ['zm_stock', 'cdc', 'cia', 'STOCK', 'signpost', '0', 'DESC', 'compass', 'SMALL',
                    'NO', 'YES', '0', 'CDC', 'CIA', 'faction_cdc', 'faction_cia', '110', '40', '0', 'top']
            with stock.open('w', newline='') as stream:
                csv.writer(stream).writerows([[f'c{i}' for i in range(20)], ['maxnum_map', '1'], base,
                                              ['default', 'cdc', 'cia'] + [''] * 17])
            for _ in range(2):
                output = modzone.stage_lobby_map_table(stock, project, 'zm_custom')
                with output.open() as stream:
                    row = next(r for r in csv.reader(stream) if r[0] == 'zm_custom')
                self.assertEqual((row[3], row[4], row[6]),
                    ('WAW_MENU_ZM_CUSTOM_TITLE', 'custom_atom', 'WAW_MENU_ZM_CUSTOM_DESC'))
                self.assertEqual(row[11], '0')
            modes = root / 'modes.csv'
            with modes.open('w', newline='') as stream:
                csv.writer(stream).writerows([[f'c{i}' for i in range(23)], ['maxnum_startloc', '1'],
                    ['5', '0', 'zm_stock', 'town'] + [''] * 19, ['startloc_gamemode_map', '1'],
                    ['6', '0', 'zm_stock', 'town', 'zstandard', '2', 'left', 'YES'] + [''] * 15])
            output = modzone.stage_lobby_gametype_table(modes, project, 'zm_custom')
            with output.open() as stream:
                row = next(r for r in csv.reader(stream) if r[0] == '5' and r[2] == 'zm_custom')
            self.assertEqual(row[4:7], ['WAW_MENU_ZM_CUSTOM_CAPS', 'WAW_MENU_ZM_CUSTOM_DESC', 'custom_cutout'])
            self.assertEqual(row[7:12], ['1', '0', '0', '180', '-50'])
            self.assertEqual(row[17:19], ['100', '-90'])

    def test_authored_menu_requires_all_runtime_materials_and_images(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'menu.json').write_text(json.dumps({'title': 'A "Town"',
                'description': 'Custom text', 'icon': 'icon', 'blit': 'blit'}))
            with self.assertRaises(FileNotFoundError):
                modzone.stage_menu_assets(root, 'zm_custom')
            names = ['icon', 'blit', 'menu_zm_custom_map', 'menu_zm_custom_map_blur',
                'menu_zm_custom_zclassic_default', 'loadscreen_zm_custom_zclassic_default',
                'loadscreen_zm_custom_zclassic_']
            (root / 'materials').mkdir()
            (root / 'images').mkdir()
            for name in names:
                (root / 'materials' / (name + '.json')).write_text(json.dumps({'textures': [{'image': name}]}))
                (root / 'images' / (name + '.iwi')).write_bytes(b'image fixture')
            entries = modzone.stage_menu_assets(root, 'zm_custom')
            self.assertTrue(all('material,' + name in entries for name in names))
            self.assertLess(entries.index('>ipak,zm_custom_menu'),
                            min(i for i, entry in enumerate(entries) if entry.startswith('material,')))
            streaming = json.loads((root / 'images/streaming.json').read_text())['streamingMode']
            self.assertEqual(streaming, dict.fromkeys(names, 2))
            metadata = json.loads((root / 'menu.json').read_text())
            metadata['image_pack'] = 'zm_custom_menu_revision2'
            (root / 'menu.json').write_text(json.dumps(metadata))
            revised = modzone.stage_menu_assets(root, 'zm_custom')
            self.assertEqual(revised[:2], ['>level.ipak_read,zm_custom_menu_revision2', '>ipak,zm_custom_menu_revision2'])
            from waw2bo2.localization import parse
            strings = parse((root / 'english/localizedstrings/menu_zm_custom.str').read_text())
            self.assertEqual(strings['WAW_MENU_ZM_CUSTOM_TITLE'], 'A "Town"')
            (root / 'images/icon.iwi').unlink()
            with self.assertRaisesRegex(FileNotFoundError, 'menu image missing'):
                modzone.stage_menu_assets(root, 'zm_custom')

    def test_linked_stock_table_cannot_pass_custom_lobby_verification(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            def extract(command, **kwargs):
                dump = Path(command[command.index('--output-folder') + 1])
                (dump / 'zm').mkdir(parents=True)
                (dump / 'zm/mapstable.csv').write_text('zm_stock,cdc,cia\n')
                return subprocess.CompletedProcess(command, 0, '', '')
            with patch.object(modzone.subprocess, 'run', side_effect=extract):
                with self.assertRaisesRegex(RuntimeError, 'custom lobby registration missing'):
                    modzone.verify_lobby_tables(root / 'mod.ff', root / 'Unlinker.exe', root, 'zm_custom')

    def test_custom_map_location_and_coop_defaults_are_registered_once(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            stock = root / "stock.csv"
            original_location = ["5", "3", "zm_stock", "town"] + [""] * 20
            original_mode = ["6", "7", "zm_stock", "town", "zstandard", "2", "right", "YES"] + [""] * 16
            with stock.open("w", newline="") as stream:
                csv.writer(stream).writerows([[f"c{i}" for i in range(24)],
                    ["maxnum_startloc", "4"], original_location,
                    ["startloc_gamemode_map", "8"], original_mode])
            output = modzone.stage_lobby_gametype_table(stock, root / "project", "zm_custom")
            with output.open() as stream:
                rows = list(csv.reader(stream))
            self.assertIn(original_location, rows)
            self.assertIn(original_mode, rows)
            custom = [r for r in rows if len(r) > 2 and r[2] == "zm_custom"]
            self.assertEqual(len(custom), 2)
            self.assertEqual(custom[0][:4], ["5", "4", "zm_custom", "default"])
            self.assertEqual(custom[1][:8], ["6", "8", "zm_custom", "default", "zclassic", "0", "left", "YES"])
            self.assertEqual(next(r[1] for r in rows if r[0] == "maxnum_startloc"), "5")
            self.assertEqual(next(r[1] for r in rows if r[0] == "startloc_gamemode_map"), "9")
            modzone.stage_lobby_gametype_table(output, root / "project", "zm_custom")
            with output.open() as stream:
                self.assertEqual(rows, list(csv.reader(stream)))

    def test_custom_map_has_persistent_numeric_globe_metadata(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            stock = root / "stock.csv"
            header = [f"column{i}" for i in range(20)]
            original = ["zm_stock", "cdc", "cia", "ZMUI_STOCK", "signpost", "0", "ZMUI_DESC", "compass", "SMALL",
                        "NO", "YES", "0", "CDC_SHORT", "CIA_SHORT", "faction_cdc", "faction_cia",
                        "110", "40", "0", "top"]
            with stock.open("w", newline="") as stream:
                csv.writer(stream).writerows([header, ["maxnum_map", "1"], original,
                    ["default", "cdc", "cia"] + [""] * 17])
            output = modzone.stage_lobby_map_table(stock, root / "project", "zm_custom")
            with output.open() as stream:
                rows = list(csv.reader(stream))
            self.assertIn(original, rows)
            self.assertEqual(rows[1][1], "2")
            custom = next(row for row in rows if row[0] == "zm_custom")
            self.assertEqual([float(custom[i]) for i in (16, 17, 18)], [0, 0, 0])
            # no empty frontend column (Lua reads them all): base-game values, own name/index
            self.assertTrue(all(v.strip() for v in custom))
            self.assertEqual(custom[3], "zm_custom")
            self.assertEqual(custom[5], "1")
            self.assertEqual(custom[11], "0")
            self.assertEqual(custom[4], "signpost")
            again = modzone.stage_lobby_map_table(output, root / "project", "zm_custom")
            with again.open() as stream:
                self.assertEqual(rows, list(csv.reader(stream)))

    def test_shader_baseline_restores_staging_after_link_failure(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            material = root/'materials/example.json'
            material.parent.mkdir()
            original = b'{"techniqueSet":"waw/runtime_test","textures":[{"image":"original"}]}'
            material.write_bytes(original)
            (root/'bridge_stage.report.json').write_text(json.dumps({'materials': [{
                'file': 'materials/example.json', 'shader_runtime': {
                    'active': [{'slot': 2}], 'native_techset': 'stock_unlit'}}]}))
            with self.assertRaisesRegex(RuntimeError, 'link failed'):
                with modzone.baseline_shader_materials(root, root) as active:
                    self.assertTrue(active)
                    self.assertEqual(json.loads(material.read_text())['techniqueSet'], 'stock_unlit')
                    self.assertEqual(json.loads(material.read_text())['textures'][0]['image'], 'original')
                    raise RuntimeError('link failed')
            self.assertEqual(material.read_bytes(), original)

    def test_required_converted_asset_is_not_silently_dropped(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            template = root / "bo2/mods/zm_test/zm_test.zone"
            template.parent.mkdir(parents=True)
            template.write_text(">game,T6\nweapon,stock_weapon\n")
            failure = subprocess.CompletedProcess([], 1,
                'ERROR: Could not load asset "custom" of type "weapon"\n')
            with patch.object(modzone, "stock_scripts", return_value=set()), \
                 patch.object(modzone.subprocess, "run", return_value=failure) as run:
                with self.assertRaisesRegex(RuntimeError, "will NOT be dropped"):
                    modzone.link_mod(root / "bo2", root / "build", root / "Unlinker.exe",
                                     ["weapon,custom"], root / "converted")
                self.assertIn(str((root / "converted").resolve()), run.call_args.args[0])
            self.assertIn("weapon,custom", (root / "build/zone_source/mod.zone").read_text())


if __name__ == "__main__":
    unittest.main()


class CharacterModelTests(unittest.TestCase):
    def test_alias_heads_of_listed_character_scripts_are_added(self):
        with tempfile.TemporaryDirectory() as temp:
            raw = Path(temp)
            (raw / "character").mkdir()
            (raw / "xmodelalias").mkdir()
            (raw / "xmodel").mkdir()
            (raw / "character/c_zombie.gsc").write_text(
                'main()\n{\n    self setmodel( "body" );\n    self attach( "listed_head", "", 1 );\n}\n')
            (raw / "xmodelalias/heads.gsc").write_text('main()\n{\n    a[0] = "head1";\n    a[1] = "gone";\n    return a;\n}\n')
            for m in ("body", "listed_head", "head1"):
                (raw / f"xmodel/{m}.json").write_text("{}")
            zone = "script,character/c_zombie.gsc\nscript,xmodelalias/heads.gsc\nxmodel,body\nxmodel,listed_head\n"
            self.assertEqual(modzone.character_models(zone, raw), ["head1"])
