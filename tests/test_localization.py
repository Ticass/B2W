import json
import tempfile
import unittest
from pathlib import Path

from waw2bo2 import localization, weapons


class LocalizationTests(unittest.TestCase):
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
        self.assertEqual(report['entries'][0]['value'], 'Map text &&1')
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
