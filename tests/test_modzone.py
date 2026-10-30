import csv
import subprocess
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from waw2bo2 import modzone


class ModZoneTests(unittest.TestCase):
    def test_template_arsenal_pruning_keeps_map_runtime_and_alternate_dependencies(self):
        from waw2bo2 import gsc
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            project, bo2 = root / 'project', root / 'bo2'
            (project / 'content_source').mkdir(parents=True)
            (project / 'content_source/weapons.runtime.json').write_text(json.dumps(
                {'weapons': ['source_gun'], 'table': {'source_gun': 'waw_source_gun'}}))
            server, client = project / 'maps/mp/map.gsc', project / 'clientscripts/mp/map.csc'
            server.parent.mkdir(parents=True)
            client.parent.mkdir(parents=True)
            server.write_text('main(){ maps\\mp\\waw\\weapons::include_weapons(); '
                              'precacheitem("custom_native"); }\n'
                              'include_weapons(){ precacheitem("unused_gun"); }\n'
                              'custom_add_weapons(){ precacheitem("unused_gun"); }')
            client.write_text('main(){ include_weapons(); }\n'
                              'include_weapons(){ include_weapon("unused_gun"); }')
            weapon = bo2 / 'raw/weapons/mp/custom_native'
            weapon.parent.mkdir(parents=True)
            weapon.write_text('WEAPONFILE\\altWeapon\\custom_alt')
            stats = bo2 / 'raw/zm/zm_statstable.csv'
            stats.parent.mkdir(parents=True)
            stats.write_text('a0,b1,c2,d3,e4\n'
                             '7,,weapon_pistol,m1911,m1911\n'
                             '22,,weapon_smg,unused_gun,unused_gun\n')
            zone = ('>game,T6\nweapon,mp/unused_gun\nweapon,mp/custom_native\n'
                    'weapon,mp/custom_alt\nweapon,mp/m1911_zm\nmaterial,keep\n')
            dropped = []
            result = modzone.prune_template_weapons(zone, project, bo2, dropped)
            self.assertEqual(dropped, ['weapon,mp/unused_gun'])
            self.assertIn('weapon,mp/custom_native', result)
            self.assertIn('weapon,mp/custom_alt', result)
            self.assertIn('material,keep', result)
            self.assertIn('waw_source_gun', client.read_text())
            self.assertNotIn('unused_gun', client.read_text())
            self.assertIn('custom_alt', client.read_text())
            with (project / 'zm/zm_statstable.csv').open(newline='') as stream:
                rows = list(csv.reader(stream))
            self.assertEqual(rows[1], ['7', '', 'weapon_pistol', 'm1911', 'm1911'])
            self.assertEqual(rows[2], ['22', '', '', 'unused_gun', 'unused_gun'])
            self.assertIn('stringtable,zm/zm_statstable.csv', result)
            gsc.parse(client.read_text(), 'client')
            self.assertEqual(modzone.prune_template_weapons(zone, project, bo2, []), result)
            (project / 'content_source/weapons.runtime.json').unlink()
            self.assertEqual(modzone.prune_template_weapons(zone, project, bo2, []), zone)

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
                # T6 localizes col3 + "_CAPS"/"_LOC": a key prefix, not the title
                self.assertEqual((row[3], row[4], row[6]),
                    ('WAW_ZM_CUSTOM', 'custom_atom', 'WAW_MENU_ZM_CUSTOM_DESC'))
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

    def test_engine_composed_menu_keys_are_localized(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'menu.json').write_text(json.dumps({'title': 'Test Town', 'description': 'Text',
                                                        'mode': 'Survival', 'location': 'Berlin, 1945'}))
            (root / 'zm').mkdir()
            with (root / 'zm/gametypestable.csv').open('w', newline='') as stream:
                csv.writer(stream).writerows([[f'c{i}' for i in range(10)],
                    ['0', 'zclassic', 'ZMUI_CLASSIC_CAPS', 'D', 'p', '0', 'YES', 'ZMUI_CLASSIC', 'YES', 'zclassic'],
                    ['0', 'zgrief', 'ZMUI_GRIEF_CAPS', 'D', 'p', '2', 'YES', 'ZMUI_GRIEF', 'YES', 'zencounter'],
                    ['6', '0', 'zm_custom', 'default', 'zclassic', '0', 'left', 'YES', '', '']])
            self.assertEqual(modzone.stage_menu_assets(root, 'zm_custom'), ['localize,menu_zm_custom'])
            from waw2bo2.localization import parse
            strings = parse((root / 'english/localizedstrings/menu_zm_custom.str').read_text())
            self.assertEqual(strings['WAW_ZM_CUSTOM_CAPS'], 'TEST TOWN')
            self.assertEqual(strings['WAW_ZM_CUSTOM_LOC'], 'Berlin, 1945')
            self.assertEqual(strings['ZMUI_CLASSIC_ZM_CUSTOM_CAPS'], 'SURVIVAL')
            self.assertEqual(strings['ZMUI_CLASSIC_WAW_ZM_CUSTOM_LOC_CAPS'], 'SURVIVAL')
            self.assertNotIn('ZMUI_GRIEF_ZM_CUSTOM_CAPS', strings)

    def test_unbranded_map_still_localizes_its_lobby_names(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.assertEqual(modzone.stage_menu_assets(root, 'zm_custom'), ['localize,menu_zm_custom'])
            from waw2bo2.localization import parse
            strings = parse((root / 'english/localizedstrings/menu_zm_custom.str').read_text())
            self.assertEqual(strings['WAW_ZM_CUSTOM_CAPS'], 'ZM_CUSTOM')
            self.assertEqual(strings['WAW_ZM_CUSTOM_LOC'], modzone.DEFAULT_MENU_LOCATION)

    def test_long_project_names_fit_the_engine_name_buffer(self):
        key = modzone.map_name_key('zm_a_very_long_custom_map_name_waw')
        self.assertLessEqual(len(key + '_CAPS'), 31)
        self.assertEqual(key, modzone.map_name_key('zm_a_very_long_custom_map_name_waw'))

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

    def test_custom_location_row_precedes_stock_rows_with_its_location_name(self):
        # SelectStartLocZombie reads location columns by (category 5, location
        # name) only; zm_bus_test's "default" row came first (playlist_ffa).
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            project = root / "project"
            project.mkdir()
            (project / "menu.json").write_text(json.dumps({"title": "T", "description": "D",
                "icon": "custom_icon", "blit": "custom_loc", "start_location": [99, 103]}))
            stock = root / "stock.csv"
            bus = ["5", "0", "zm_bus_test", "default", "C", "D", "playlist_ffa", "1", "-320", "-240"] + [""] * 14
            with stock.open("w", newline="") as stream:
                csv.writer(stream).writerows([[f"c{i}" for i in range(24)], ["maxnum_startloc", "1"], bus,
                    ["startloc_gamemode_map", "1"],
                    ["6", "0", "zm_bus_test", "default", "zclassic", "0", "left", "YES"] + [""] * 16])
            output = modzone.stage_lobby_gametype_table(stock, project, "zm_custom")
            with output.open() as stream:
                rows = list(csv.reader(stream))
            first = next(r for r in rows if r and r[0] == "5" and r[3] == "default")
            self.assertEqual(first[2], "zm_custom")
            self.assertEqual([r[2] for r in rows if r and r[0] in ("5", "6") and r[3] == "default"],
                             ["zm_custom", "zm_custom"])
            self.assertEqual({r[3] for r in rows if len(r) > 3 and r[2] == "zm_bus_test"}, {"zm_bus_test"})
            self.assertEqual(first[6], "custom_loc")
            self.assertEqual(first[8:12], ["99", "103", "279", "53"])
            self.assertEqual(first[17:19], ["199", "13"])
            self.assertEqual(next(r[1] for r in rows if r[0] == "maxnum_startloc"), "2")

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
            self.assertEqual(custom[3], "WAW_ZM_CUSTOM")
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

    def test_native_probe_only_material_uses_stock_baseline(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            material = root/'materials/native.json'
            material.parent.mkdir()
            original = '{"techniqueSet":"waw/runtime_probe"}'
            material.write_text(original)
            (root/'bridge_stage.report.json').write_text(json.dumps({'materials':[{
                'file':'materials/native.json', 'shader_runtime':{
                    'active':[], 'native_probe_passes':22, 'native_techset':'stock_lit'}}]}))
            with modzone.baseline_shader_materials(root,root) as active:
                self.assertTrue(active)
                self.assertEqual(json.loads(material.read_text())['techniqueSet'],'stock_lit')
            self.assertEqual(material.read_text(),original)

    def test_shader_relink_ignores_materials_left_by_earlier_builds(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            project, work = root/'project', root/'work'
            for name in ('current', 'stale'):
                path = project/'materials/mc'/(name + '.json')
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(json.dumps({'techniqueSet': 'waw/runtime_' + name}))
            (project/'bridge_stage.report.json').write_text(json.dumps({'materials': [
                {'file': 'materials/mc/current.json'}]}))
            (work/'zone_source').mkdir(parents=True)
            (work/'zone_source/mod.zone').write_text('material,mc/current\n')
            ff = root/'mod.ff'
            ff.write_bytes(b'ff')

            def tool(command, **kwargs):
                if '--output-folder' in command and 'material' in command:
                    dump = Path(command[command.index('--output-folder') + 1])
                    for name in ('current', 'stale'):  # both exist in the gameplay zone
                        out = dump/'materials/mc'/(name + '.json')
                        out.parent.mkdir(parents=True, exist_ok=True)
                        out.write_text(json.dumps({'techniqueSet': 'stock'}))
                    return subprocess.CompletedProcess(command, 0, '', '')
                return subprocess.CompletedProcess(command, 1, 'stop after overlay', '')

            with patch.object(modzone, 'captured', side_effect=tool):
                with self.assertRaisesRegex(RuntimeError, 'relink failed'):
                    modzone.activate_mod_shaders(ff, project, work, root/'Linker.exe', root/'Unlinker.exe', root/'stock')
            overlay = work/'shader_overlay/materials/mc'
            self.assertEqual(json.loads((overlay/'current.json').read_text())['techniqueSet'], 'waw/runtime_current')
            self.assertFalse((overlay/'stale.json').exists())

    def test_shader_relink_leaves_same_named_stock_material_alone(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            project, work = root/'project', root/'work'
            source = project/'materials/mc/mtl_prop_bear.json'
            source.parent.mkdir(parents=True)
            source.write_text(json.dumps({'techniqueSet': 'waw/runtime_bear', 'textures': [
                {'name': n} for n in ('colorMap', 'normalMap', 'specularMap')]}))
            (project/'bridge_stage.report.json').write_text(json.dumps({'materials': [
                {'file': 'materials/mc/mtl_prop_bear.json'}]}))
            (work/'zone_source').mkdir(parents=True)
            (work/'zone_source/mod.zone').write_text('material,mc/mtl_prop_bear\n')
            ff = root/'mod.ff'
            ff.write_bytes(b'ff')

            def tool(command, **kwargs):
                if '--output-folder' in command and 'material' in command:
                    out = Path(command[command.index('--output-folder') + 1])/'materials/mc/mtl_prop_bear.json'
                    out.parent.mkdir(parents=True)
                    # BO2's own teddy bear: no specular slot
                    out.write_text(json.dumps({'techniqueSet': 'mc_lit_sm_r0c0n0', 'textures': [
                        {'name': 'colorMap'}, {'name': 'normalMap'}]}))
                    return subprocess.CompletedProcess(command, 0, '', '')
                return subprocess.CompletedProcess(command, 1, 'stop after overlay', '')

            with patch.object(modzone, 'captured', side_effect=tool):
                with self.assertRaisesRegex(RuntimeError, 'relink failed'):
                    modzone.activate_mod_shaders(ff, project, work, root/'Linker.exe', root/'Unlinker.exe', root/'stock')
            self.assertFalse((work/'shader_overlay/materials/mc/mtl_prop_bear.json').exists())

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

    def test_ipak_failure_recovers_exact_stock_image_and_relinks(self):
        from waw2bo2 import all2raw, iwi
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            template = root / 'bo2/mods/zm_test/zm_test.zone'
            template.parent.mkdir(parents=True)
            template.write_text('>game,T6\nmaterial,custom\n')
            original = root / 'stock_dependency/images/custom.iwi'
            original.parent.mkdir(parents=True)
            payload = iwi.solid_rgba_iwi(2, 2, (1, 2, 3, 255))
            original.write_bytes(payload)
            calls = []
            def link(command, **kwargs):
                calls.append(command)
                if len(calls) == 1:
                    return subprocess.CompletedProcess(command, 0,
                        'ERROR: Failed to open file for ipak: images/custom.iwi\n')
                self.assertEqual((root / 'build/ipak_images/images/custom.iwi').read_bytes(), payload)
                ff = root / 'build/out/mod.ff'
                ff.parent.mkdir(parents=True)
                ff.write_bytes(b'linked')
                return subprocess.CompletedProcess(command, 0, '')
            with patch.object(modzone, 'stock_scripts', return_value=set()), \
                 patch.object(modzone, 'captured', side_effect=link), \
                 patch.object(all2raw, 'required_bo2_image', return_value=original) as extract:
                ff, unavailable = modzone.link_mod(root / 'bo2', root / 'build', root / 'Unlinker.exe',
                    stock_dump=root / 'stock')
            self.assertTrue(ff.is_file())
            self.assertEqual(unavailable, [])
            self.assertEqual(len(calls), 2)
            self.assertEqual(extract.call_args.args[1], 'custom')
            self.assertIn('material,custom', (root / 'build/zone_source/mod.zone').read_text())


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

    def test_loaded_weapons_come_from_the_final_link_log(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            self.assertEqual(modzone.loaded_weapons(work), set())
            (work / "linker.log").write_text('Loaded weapon "ray_gun_zm" (src: disk)\n'
                                             'Loaded xmodel "defaultweapon" (src: disk)\n'
                                             'Loaded weapon "gl_type95_zm" (src: disk)\n')
            self.assertEqual(modzone.loaded_weapons(work), {"ray_gun_zm", "gl_type95_zm"})


class ModIpakRangeTests(unittest.TestCase):
    def test_mod_ipak_range_opens_before_template_entries(self):
        template = ("// header\n>game,T6\n>level.ipak_read,common_zm\n"
                    "weapon,mp/m1911_zm\nxanim,a\n")
        extra = [">level.ipak_read,map_mod", ">ipak,map_mod", "image,waw_image/side_small"]
        lines = modzone.open_mod_ipak_first(template, extra).splitlines()
        self.assertEqual(lines[3:6], [">level.ipak_read,map_mod", ">ipak,map_mod", "weapon,mp/m1911_zm"])

    def test_zone_without_mod_ipak_is_unchanged(self):
        template = ">game,T6\nweapon,mp/m1911_zm\n"
        self.assertEqual(modzone.open_mod_ipak_first(template, ["stringtable,zm/mapstable.csv"]), template)
