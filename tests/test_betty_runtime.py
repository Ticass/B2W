import tempfile
import unittest
from pathlib import Path
from waw2bo2 import gscport
from waw2bo2.t6api import T6Api


class BettyRuntimeTests(unittest.TestCase):
    def test_source_betty_startup_runs_from_zombiemode_main(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'maps').mkdir()
            (root / 'maps/_zombiemode.gsc').write_text(
                'main(){maps\\_zombiemode_betty::init();}')
            (root / 'maps/_zombiemode_betty.gsc').write_text(
                'init(){level thread give_betties_after_rounds();}\n'
                'give_betties_after_rounds(){level waittill("between_round_over");}')
            tr = gscport.Translator(gscport.Sources([root], [], None),
                                    T6Api(builtins={}, scripts={}), gscport.PortReport())
            self.assertEqual(tr.framework_hooks(),
                             [r'maps\mp\waw\_zombiemode_betty::init();'])
            self.assertIn(r'maps\_zombiemode_betty', tr.queue)

    def test_spawned_radius_keeps_source_ai_mask_and_dimensions(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'maps').mkdir()
            (root / 'maps/test.gsc').write_text(
                'main(){t=spawn("trigger_radius",self.origin,9,80,64);'
                'm=spawn("script_model",self.origin);}')
            api = T6Api(builtins={'spawn': (2, 5)}, scripts={})
            tr = gscport.Translator(gscport.Sources([root], [], None), api, gscport.PortReport())
            text = tr.translate(r'maps\test')
            self.assertIn('waw_spawn("trigger_radius",self.origin,9,80,64)', text)
            self.assertIn('waw_spawn("script_model",self.origin)', text)
            self.assertFalse(tr.report.unsupported)

    def test_source_script_damage_modes_survive_same_named_native_handler(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'maps').mkdir()
            (root / 'maps/test.gsc').write_text(
                '#include maps\\_zombiemode_spawner;\n'
                'main(){self zombie_damage("MOD_ZOMBIE_BETTY","none",self.origin,self.owner);}')
            (root / 'maps/_zombiemode_spawner.gsc').write_text(
                'zombie_damage(mod,hit,origin,player){if(mod=="MOD_ZOMBIE_BETTY")'
                '{self dodamage(level.round_number*randomintrange(100,200),self.origin,player);}}')
            api = T6Api(builtins={'randomintrange': (2, 2)}, methods={'dodamage': (3, 6)},
                        scripts={r'maps\mp\zombies\_zm_spawner': {'zombie_damage': 6}})
            tr = gscport.Translator(gscport.Sources([root], [], None), api, gscport.PortReport())
            text = tr.translate(r'maps\test')
            # WaW drives the zombie AI: the map's whole _zombiemode_spawner is
            # ported and its own zombie_damage handles the Betty
            self.assertIn('#include maps\mp\waw\_zombiemode_spawner;', text)
            self.assertIn('self zombie_damage("MOD_ZOMBIE_BETTY"', text)
            body = tr.translate(r'maps\_zombiemode_spawner')
            self.assertIn('mod=="MOD_ZOMBIE_BETTY"', body)
            self.assertIn('level.round_number*randomintrange(100,200)', body)
