"""Optional scripts must link on stock maps even when installed globally."""
import unittest
from pathlib import Path

from waw2bo2 import gsc, gscport
from waw2bo2.t6api import T6Api, load_snapshot

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'extras/der_riese_casual/der_riese_waw_health_audit.gsc'


class CasualScriptTests(unittest.TestCase):
    # The casual fixes themselves are console dvars now (compat/waw_fixes.json).
    def test_health_audit_has_only_stock_dependencies(self):
        self.check_script(SCRIPT)

    def check_script(self, path):
        data = load_snapshot()
        api = T6Api(data['builtins'], data['scripts'], data['methods'])
        script = gsc.parse(path.read_text(), str(path))
        includes = [r.qualifier for r in gsc.references(script.tokens)
                    if r.qualifier]
        for ref in gsc.references(script.tokens):
            if ref.qualifier:
                self.assertTrue(api.defines(ref.qualifier, ref.name), str(ref))
            elif ref.name not in script.functions:
                self.assertTrue(api.builtin_range(ref.name, ref.method) or
                                api.defines('common_scripts\\utility', ref.name), str(ref))
                argc = gscport._argc(script.tokens, ref.index)
                if argc is not None and api.builtin_range(ref.name, ref.method):
                    self.assertTrue(api.builtin_accepts(ref.name, argc, ref.method), str(ref))
        self.assertFalse(any('waw' in p for p in includes))


if __name__ == '__main__':
    unittest.main()
