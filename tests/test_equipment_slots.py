import tempfile
import unittest
from pathlib import Path

from waw2bo2 import gscport
from waw2bo2.t6api import T6Api


class EquipmentSlotTranslationTests(unittest.TestCase):
    def test_repeated_native_hud_cleanup_is_safe_without_changing_local_helpers(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'maps').mkdir()
            (root / 'maps/test.gsc').write_text(
                '#include maps\\_hud_util;\nmain(){ hud destroyElem(); hud destroyElem(); '
                'player EnableWeaponCycling(); }')
            (root / 'maps/_hud_util.gsc').write_text('destroyElem(){}')
            api = T6Api({}, {r'maps\mp\gametypes_zm\_hud_util': {'destroyelem': 0}},
                        methods={'enableweaponcycling': (0, 0)})
            translator = gscport.Translator(gscport.Sources([root], [], None), api, gscport.PortReport())
            text = translator.translate(r'maps\test')
            self.assertEqual(text.count('waw_destroy_hud_elem( hud )'), 2)
            self.assertIn('EnableWeaponCycling()', text)
            (root / 'maps/local.gsc').write_text('main(){ hud destroyElem(); } destroyElem(){}')
            translator = gscport.Translator(gscport.Sources([root], [], None), api, gscport.PortReport())
            self.assertNotIn('waw_destroy_hud_elem', translator.translate(r'maps\local'))

    def test_weapon_slot_calls_translate_even_when_bo2_accepts_the_builtin(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'maps').mkdir()
            (root / 'maps/test.gsc').write_text(
                'main(){ player setActionSlot(3,"weapon",item); '
                'self setactionslot(3,"weapon"," "); self setactionslot(3,"altMode"); }')
            api = T6Api({}, {}, methods={'setactionslot': (2, 3)})
            translator = gscport.Translator(gscport.Sources([root], [], None), api, gscport.PortReport())
            text = translator.translate(r'maps\test')
            self.assertIn('waw_setactionslot(3,"weapon",item)', text)
            self.assertIn('waw_setactionslot(3,"weapon"," ")', text)
            self.assertIn('waw_setactionslot(3,"altMode")', text)


if __name__ == '__main__':
    unittest.main()
