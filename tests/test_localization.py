import json
import tempfile
import unittest
import zipfile
from pathlib import Path

from waw2bo2 import localization, weapons


class LocalizationTests(unittest.TestCase):
    def test_nested_flame_tables_are_not_read_as_weapon_display_names(self):
        table = self.project / 'content_source/weapons/waw_flametable/flame'
        table.parent.mkdir(parents=True)
        table.write_text('FLAMETABLEFILE\\name\\waw_flametable/flame\\flameVar_streamChunkSpeed\\700')
        self.script.write_text('f(){x=&"ZOMBIE_HINT";}')
        self.strings(self.root, 'map', {'ZOMBIE_HINT': 'Authored prompt'})
        report = self.stage()
        self.assertEqual(report['entries'][0]['value'], 'Authored prompt')
        self.assertEqual(report['errors'], [])

    def test_waw_use_hint_translates_button_but_not_hud_arguments(self):
        self.script.write_text('f(){self SetHintString(&"ZOMBIE_PERK_JUGGERNAUT");x=&"HUD_SCORE";}')
        self.strings(self.root, 'map', {
            'ZOMBIE_PERK_JUGGERNAUT': 'Press &&1 to buy Jugger-Nog [Cost: 2500]',
            'HUD_SCORE': 'Score: &&1'})
        report = self.stage()
        values = {e['key']: e['value'] for e in report['entries']}
        self.assertEqual(values['ZOMBIE_PERK_JUGGERNAUT'],
                         'Press ^3[{+activate}]^7 to buy Jugger-Nog [Cost: 2500]')
        self.assertEqual(values['HUD_SCORE'], 'Score: &&1')

    def test_a_hint_the_map_formats_itself_keeps_its_arguments(self):
        # wall buys: SetHintString( &"ZOMBIE_WEAPONCOSTAMMO", cost, ammo_cost )
        self.script.write_text('f(){self SetHintString(&"ZOMBIE_WEAPONCOSTAMMO", cost, ammo_cost);}')
        self.strings(self.root, 'map', {'ZOMBIE_WEAPONCOSTAMMO': 'For Weapon [Cost: &&1], For Ammo [Cost: &&2]'})
        report = self.stage()
        self.assertEqual(report['entries'][0]['value'], 'For Weapon [Cost: &&1], For Ammo [Cost: &&2]')

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.project = self.base / 'project'
        self.script = self.project / 'maps/mp/waw/map.gsc'
        self.script.parent.mkdir(parents=True)
        self.root = self.base / 'source'

    def strings(self, root, name, values):
        path = root / 'english/localizedstrings' / (name + '.str')
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('VERSION "1"\n' + ''.join(
            'REFERENCE ' + k + '\nLANG_ENGLISH ' + localization.quote(v) + '\n'
            for k, v in values.items()) + 'ENDMARKER\n')

    def stage(self, **kwargs):
        return localization.stage(self.project, [self.root], [], **kwargs)

    def test_parser_preserves_quotes_newlines_placeholders_and_button_tokens(self):
        text = 'Hold ^3[{+activate}]^7 for "Power" [Cost: &&1]\nNext line // literal'
        self.assertEqual(localization.parse('/* ignored */ REFERENCE ZOMBIE_HINT\nLANG_FRENCH "autre"\nLANG_ENGLISH ' + localization.quote(text)), {'ZOMBIE_HINT': text})

    def test_repeated_reference_uses_final_language_value_and_reports_only_conflicts(self):
        text = ('REFERENCE MUT_11_DESC\nLANG_ENGLISH "First &&1"\n'
                'REFERENCE MUT_11_DESC\nLANG_ENGLISH "First &&1"\n'
                'REFERENCE MUT_11_DESC\nLANG_FRENCH "Autre"\nLANG_ENGLISH "Final \\"quoted\\" &&1"\n')
        conflicts = []
        self.assertEqual(localization.parse(text, conflicts=conflicts), {'MUT_11_DESC': 'Final "quoted" &&1'})
        self.assertEqual(len(conflicts), 1)
        self.assertEqual(conflicts[0]['previous_line'], 4)
        self.assertEqual(conflicts[0]['line'], 7)
        self.assertEqual(localization.parse(text, 'french'), {'MUT_11_DESC': 'Autre'})

    def test_iwd_duplicate_keys_do_not_abort_or_override_higher_priority_zone(self):
        self.script.write_text('a(){x=&"MUT_11_DESC";y=&"ZONE_HINT";}')
        self.strings(self.root, 'map', {'ZONE_HINT': 'Zone text'})
        archive = self.base / 'map.iwd'
        with zipfile.ZipFile(archive, 'w') as output:
            output.writestr('localizedstrings/custom.str',
                'REFERENCE MUT_11_DESC\nLANG_ENGLISH "Earlier description"\n'
                'REFERENCE MUT_11_DESC\nLANG_ENGLISH "Final description &&1"\n'
                'REFERENCE ZONE_HINT\nLANG_ENGLISH "IWD text"\n'
                'REFERENCE ZONE_HINT\nLANG_ENGLISH "Final IWD text"\nENDMARKER\n')
        report = localization.stage(self.project, [self.root, self.root], [archive, archive])
        entries = {e['key']: e['value'] for e in report['entries']}
        self.assertEqual(entries, {'MUT_11_DESC': 'Final description &&1', 'ZONE_HINT': 'Zone text'})
        self.assertEqual(report['errors'], [])
        self.assertEqual(report['missing'], [])
        self.assertEqual(len(report['conflicts']), 2)
        self.assertEqual(len(report['warnings']), 2)
        self.assertIn('map.iwd:localizedstrings/custom.str', report['warnings'][0])
        self.assertIn('&"WAW2BO2_MUT_11_DESC"', self.script.read_text())
        generated = (self.project / 'english/localizedstrings/waw_project.str').read_text()
        self.assertEqual(generated.count('REFERENCE WAW2BO2_MUT_11_DESC'), 1)
        self.assertEqual(report['entries'], localization.stage(self.project, [self.root], [archive])['entries'])

    def test_conflicting_unused_fallback_does_not_block_source_text(self):
        self.script.write_text('a(){x=&"ZOMBIE_HINT";}')
        self.strings(self.root, 'map', {'ZOMBIE_HINT': 'Authored source text'})
        bo2 = self.base / 'bo2'
        self.strings(bo2 / 'raw', 'native', {'ZOMBIE_HINT': 'Native text'})
        path = bo2 / 'raw/english/localizedstrings/native.str'
        path.write_text(path.read_text().replace('ENDMARKER',
            'REFERENCE MUT_11_DESC\nLANG_ENGLISH "Earlier"\n'
            'REFERENCE MUT_11_DESC\nLANG_ENGLISH "Later"\nENDMARKER'))
        report = self.stage(bo2_root=bo2)
        self.assertEqual(report['entries'][0]['value'], 'Authored source text')
        self.assertEqual(report['entries'][0]['kind'], 'WaW')
        self.assertEqual(report['errors'], [])
        self.assertEqual(report['conflicts'][0]['key'], 'MUT_11_DESC')

    def test_source_namespaced_in_scripts_and_weapon_fields_without_touching_comments(self):
        self.strings(self.root, 'source', {'ZOMBIE_OFF': 'Turn on the power first', 'WEAPON_GUN': 'Original gun'})
        self.script.write_text('// &"ZOMBIE_OFF"\na() { self sethintstring(&"ZOMBIE_OFF"); level.hint = "ZOMBIE_OFF"; }')
        weapon = self.project / 'content_source/weapons/gun'
        weapon.parent.mkdir(parents=True)
        weapon.write_text(weapons.write_info({'displayName': 'WEAPON_GUN', 'damage': '100'}))
        report = self.stage()
        self.assertEqual(report['missing'], [])
        self.assertIn('// &"ZOMBIE_OFF"', self.script.read_text())
        self.assertIn('&"WAW2BO2_ZOMBIE_OFF"', self.script.read_text())
        self.assertIn('"WAW2BO2_ZOMBIE_OFF";', self.script.read_text())
        fields = weapons.read_info(weapon.read_text())
        self.assertEqual(fields, {'displayName': 'WAW2BO2_WEAPON_GUN', 'damage': '100'})

    def test_source_wins_over_raw_and_bo2_fallback(self):
        self.script.write_text('a() { self sethintstring(&"ZOMBIE_HINT"); }')
        self.strings(self.root, 'map', {'ZOMBIE_HINT': 'Map text &&1'})
        raw = self.base / 'raw'
        bo2 = self.base / 'bo2'
        self.strings(raw, 'raw', {'ZOMBIE_HINT': 'Raw text'})
        self.strings(bo2 / 'raw', 'native', {'ZOMBIE_HINT': 'Native text'})
        report = self.stage(raw_roots=[raw], bo2_root=bo2)
        self.assertEqual(report['entries'][0]['value'], 'Map text ^3[{+activate}]^7')
        self.assertEqual(report['entries'][0]['kind'], 'WaW')

    def test_unknown_key_is_reported_and_shown_as_its_key(self):
        self.script.write_text('a() { self sethintstring(&"custom_hint_missing"); }')
        report = self.stage()
        self.assertEqual(report['missing'], ['custom_hint_missing'])
        self.assertEqual(report['errors'], [])
        self.assertTrue(report['warnings'])
        self.assertEqual(report['entries'][0]['value'], 'custom_hint_missing')
        self.assertEqual(report['entries'][0]['kind'], 'MISSING_SHOWN_AS_KEY')

    def test_reference_lookup_ignores_case_like_waw(self):
        self.script.write_text('a() { self sethintstring(&"ZOMBIE_WEAPON_Galil_1200"); }')
        self.strings(self.root, 'zombie', {'WEAPON_GALIL_1200': 'Buy Galil [Cost: 1200]'})
        report = self.stage()
        self.assertEqual(report['missing'], [])
        self.assertEqual(report['entries'][0]['value'], 'Buy Galil [Cost: 1200]')
        self.assertIn('&"WAW2BO2_ZOMBIE_WEAPON_Galil_1200"', self.script.read_text())

    def test_custom_door_prices_use_source_wording_and_keep_authored_override(self):
        self.script.write_text('a() { x = &"ZOMBIE_BUTTON_BUY_OPEN_DOOR_15000"; '
                              'y = &"ZOMBIE_BUTTON_BUY_OPEN_DOOR_7500"; }')
        self.strings(self.root, 'zombie', {
            'BUTTON_BUY_OPEN_DOOR_100': 'Press & hold &&1 to Open Door [Cost: 100]',
            'BUTTON_BUY_OPEN_DOOR_200': 'Press & hold &&1 to Open Door [Cost: 200]'})
        first = self.stage()
        self.assertEqual(first['missing'], [])
        entries = {e['key']: e for e in first['entries']}
        self.assertEqual(entries['ZOMBIE_BUTTON_BUY_OPEN_DOOR_15000']['value'],
                         'Press & hold &&1 to Open Door [Cost: 15000]')
        self.assertEqual(entries['ZOMBIE_BUTTON_BUY_OPEN_DOOR_7500']['kind'], 'WAW_PRICE_VARIANT')
        self.strings(self.root, 'map', {'ZOMBIE_BUTTON_BUY_OPEN_DOOR_7500': 'Custom vault prompt'})
        second = self.stage()
        entries = {e['key']: e for e in second['entries']}
        self.assertEqual(entries['ZOMBIE_BUTTON_BUY_OPEN_DOOR_7500']['value'], 'Custom vault prompt')

    def test_mule_kick_shared_key_uses_bo2_only_when_waw_is_absent(self):
        self.script.write_text('a() { x = &"ZOMBIE_PERK_ADDITIONALWEAPONPERK"; }')
        bo2 = self.base / 'bo2'
        self.strings(bo2 / 'raw', 'en_patch_zm', {
            'ZOMBIE_PERK_ADDITIONALPRIMARYWEAPON': 'Hold &&1 for Mule Kick [Cost: &&2]'})
        report = self.stage(bo2_root=bo2)
        self.assertEqual(report['missing'], [])
        self.assertEqual(report['entries'][0]['kind'], 'BO2_SHARED_FALLBACK')
        self.strings(self.root, 'source', {'ZOMBIE_PERK_ADDITIONALWEAPONPERK': 'Original custom Mule Kick'})
        self.assertEqual(self.stage(bo2_root=bo2)['entries'][0]['value'], 'Original custom Mule Kick')

    def test_literal_weapon_name_and_repeated_staging_are_stable(self):
        self.script.write_text('a() {}')
        weapon = self.project / 'content_source/weapons/gun'
        weapon.parent.mkdir(parents=True)
        weapon.write_text(weapons.write_info({'displayName': "Amazon's Wild Seahorse"}))
        first = self.stage()
        before = weapon.read_bytes()
        second = self.stage()
        self.assertEqual(first['entries'], second['entries'])
        self.assertEqual(second['missing'], [])
        self.assertEqual(before, weapon.read_bytes())

    def test_hash_strings_and_unrelated_text_are_untouched(self):
        self.strings(self.root, 'source', {'ZOMBIE_OFF': 'Power first'})
        self.script.write_text('a() { hash = #"ZOMBIE_OFF"; x = "Press &&1"; self sethintstring(&"ZOMBIE_OFF"); }')
        self.stage()
        self.assertIn('#"ZOMBIE_OFF"', self.script.read_text())
        self.assertIn('"Press &&1"', self.script.read_text())

    def test_plain_value_matching_a_short_raw_reference_stays_literal(self):
        # stock menu.str has REFERENCE 30 ("30%"); a dvar value "30" is not a reference
        self.strings(self.root, 'menu', {'30': '30%'})
        self.script.write_text('a() { setsaveddvar( "player_laststandbleedouttime", "30" ); }')
        self.stage()
        self.assertIn('"30" )', self.script.read_text())

    def test_shared_fallback_never_overrides_present_source(self):
        self.script.write_text('a() { x = &"PATCH_ZOMBIE_MONKEY"; }')
        self.strings(self.root, 'source', {'PATCH_ZOMBIE_MONKEY': 'Custom monkey name'})
        report = self.stage()
        self.assertEqual(report['entries'][0]['kind'], 'WaW')
        self.assertEqual(report['entries'][0]['value'], 'Custom monkey name')

    def test_file_scoped_raw_references_resolve(self):
        self.script.write_text('a() { x = &"ZOMBIE_HINT"; }')
        self.strings(self.root, 'zombie', {'HINT': 'Original &&1 prompt'})
        report = self.stage()
        self.assertEqual(report['missing'], [])
        self.assertEqual(report['entries'][0]['value'], 'Original &&1 prompt')
