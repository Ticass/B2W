import json
import re
import unittest
from pathlib import Path

from waw2bo2 import framework_coverage, gsc, gscport
from waw2bo2.t6api import T6Api, load_snapshot

COMPAT = Path(gscport.COMPAT_DIR)
ENABLED = re.compile(r'enabled\(\s*"(\w+)"\s*\)')


class OptionalFixTests(unittest.TestCase):
    """compat/waw_fixes.json is the only list of opt-in fixes."""

    def setUp(self):
        self.manifest = json.loads((COMPAT / 'waw_fixes.json').read_text(encoding='utf-8'))['fixes']
        self.fixes = (COMPAT / '_waw2bo2_fixes.gsc').read_text(encoding='utf-8')
        self.compat = (COMPAT / '_waw2bo2_compat.gsc').read_text(encoding='utf-8')

    def test_every_fix_is_off_by_default(self):
        self.assertEqual({name: fix['default'] for name, fix in self.manifest.items()},
                         dict.fromkeys(self.manifest, 0))

    def test_registered_dvars_are_the_manifest(self):
        register = self.fixes[self.fixes.index('register()'):self.fixes.index('enabled( name )')]
        self.assertEqual(set(re.findall(r'"(g_fix_\w+)"', register)), set(self.manifest))

    def test_every_check_names_a_listed_fix_and_every_fix_is_used(self):
        used = set(ENABLED.findall(self.fixes)) | set(ENABLED.findall(self.compat))
        self.assertEqual(used, set(self.manifest))

    def test_script_links_on_stock_t6(self):
        # every map stages it: only its own functions, common_scripts\utility,
        # BO2's _zm_laststand and T6 builtins (with their arity)
        data = load_snapshot()
        api = T6Api(data['builtins'], data['scripts'], data['methods'])
        script = gsc.parse(self.fixes, gscport.FIXES)
        for entry in ('register', 'enabled', 'wrap_player_damage', 'native_laststand', 'perk_granted'):
            self.assertIn(entry, script.functions)
        for ref in gsc.references(script.tokens):
            if ref.qualifier:
                self.assertTrue(api.defines(ref.qualifier, ref.name), str(ref))
            elif ref.name not in script.functions and not ref.pointer:
                if api.defines('common_scripts\\utility', ref.name):
                    continue
                argc = gscport._argc(script.tokens, ref.index)
                self.assertTrue(api.builtin_accepts(ref.name.lower(), argc or 0, ref.method), str(ref))

    def test_hooks_reach_the_fixes(self):
        # the tesla fix is a condition on WaW's health setter, nothing else
        setter = self.compat[self.compat.index('waw_set_health( value )'):]
        setter = setter[:setter.index('}')]
        self.assertIn('_waw2bo2_fixes::enabled( "g_fix_tesla_bug" )', setter)
        self.assertIn('setmaxhealth', setter)
        # last stand: the owned glue sends a fix's down to T6's last stand
        glue = gscport.WAW_SOURCE_PLAYER_LASTSTAND[1]
        self.assertIn('_waw2bo2_fixes::native_laststand()', glue)
        self.assertIn('_zm_laststand::playerlaststand', glue.split('native_laststand()')[1])
        # compat registers the dvars; the old optional script's hooks are gone
        self.assertIn('maps\\mp\\waw\\_waw2bo2_fixes::register();', self.compat)
        self.assertNotIn('waw2bo2_health_setter', self.compat)

    def test_coverage_lists_the_fixes(self):
        text = framework_coverage.markdown('zm_test', {})
        for name in self.manifest:
            self.assertIn(f'| {name} 0 |', text)


if __name__ == '__main__':
    unittest.main()
