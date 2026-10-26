import tempfile
import unittest
from pathlib import Path

from waw2bo2 import gscport
from waw2bo2.t6api import T6Api


class BuiltinArgumentCountTest(unittest.TestCase):
    """WaW recovers from builtin argument errors; T6 refuses to link the call."""

    def translate(self, body: str) -> gscport.Translator:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "maps").mkdir()
            (root / "maps/test.gsc").write_text("main()\n{\n" + body + "\n}\n")
            translator = gscport.Translator(gscport.Sources([root], [], None),
                                            T6Api({}, {}, methods={"moveto": (2, 4), "movez": (2, 4)}),
                                            gscport.PortReport())
            self.source = translator.translate(r"maps\test")
            return translator

    def test_too_few_arguments_recovers_and_preserves_following_movement(self):
        translator = self.translate("\tmover MoveTo(start);\n\tmover MoveZ( 4600, 2000 );")
        self.assertIn("::stub_moveto_1args_T6_takes_2_4(start);", self.source)
        self.assertIn("mover MoveZ( 4600, 2000 );", self.source)
        stubs = translator.stub_source()
        body = stubs[stubs.index("stub_moveto_1args"):]
        self.assertIn('::report(', body.split("}")[0])
        self.assertNotIn('waittill', body.split("}")[0])
        self.assertNotIn('return ', body.split("}")[0])  # implicit undefined result

    def test_too_many_arguments_are_dropped_not_halted(self):
        translator = self.translate("\tcheese MoveZ( 1, 2, 0, 0, 5 );")
        self.assertIn("cheese MoveZ( 1, 2, 0, 0", self.source)
        self.assertNotIn("waw2bo2_never", translator.stub_source())


class Dlc3PowerSwitchTest(unittest.TestCase):
    def test_template_threadcalls2_runs_the_ported_power_switch(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "maps").mkdir()
            (root / "maps/test.gsc").write_text("main()\n{\n\tlevel thread maps\\dlc3_code::dlc3_threadcalls2();\n}\n")
            (root / "maps/dlc3_code.gsc").write_text(
                'dlc3_threadcalls2() { level thread power_electric_switch(); '
                'thread init_elec_trap_trigs(); teleporter_init(); thread intro_screen(); }\n'
                'power_electric_switch() { level notify("electricity_on"); }\n'
                'init_elec_trap_trigs() {}\nintro_screen() {}\n'
                'teleporter_init() { maps\\dlc3_teleporter::player_is_near_pad(self); }\n')
            (root / "maps/dlc3_teleporter.gsc").write_text(
                'player_is_near_pad(who) { return (who) IsTouching(self); }\n')
            translator = gscport.Translator(gscport.Sources([root], [], None), T6Api({}, {}), gscport.PortReport())
            translator.api.methods['istouching'] = (1, 1)
            source = translator.translate(r"maps\test")
            core = []
            while translator.core_queue:
                core.append(translator.translate_extracted(*translator.core_queue.pop(0)))
        self.assertIn(gscport.CORE + '::dlc3_code__dlc3_threadcalls2()', source)
        extracted = '\n'.join(core)
        for name in ('power_electric_switch', 'init_elec_trap_trigs', 'teleporter_init', 'intro_screen'):
            self.assertIn('dlc3_code__' + name + '()', extracted)
        self.assertIn('who IsTouching(self)', extracted)
        self.assertEqual(translator.report.unsupported, {})

    def test_map_zone_manager_is_ported_whole(self):
        # The map's own zone manager fills level.enemy_spawns for its own spawn
        # loop; it runs on WaW-owned zone fields beside BO2's _zm_zonemgr.
        sources = gscport.Sources([], [], None)
        self.assertFalse(gscport.is_core(r'maps\_zombiemode_zone_manager', sources))
        self.assertNotIn('manage_zones', gscport.API['function_renames'])
        tokens = gscport.gsc.tokenize('f() { level.zones[ "a" ] = 1; level.zone_manager_init_func = ::g; '
                                      'x = level.enemy_dog_locations; }')
        gscport.rename_level_fields(tokens)
        text = gscport.gsc.emit(tokens)
        for field in ('waw_zones', 'waw_zone_manager_init_func', 'waw_enemy_dog_locations'):
            self.assertIn('level.' + field, text)

    def test_template_callback_setup_preserves_native_lifecycle(self):
        translator = gscport.Translator(gscport.Sources([], [], None), T6Api({}, {}), gscport.PortReport())
        self.assertEqual(translator.resolve_core(r'maps\_callbacksetup', 'setupcallbacks',
                                                0, 'template', set()),
                         gscport.COMPAT + '::waw_setupcallbacks')
        self.assertEqual(translator.report.unsupported, {})

    def test_lazy_stock_animation_helper_is_extracted_without_porting_whole_framework(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'maps').mkdir()
            (root / 'animscripts').mkdir()
            (root / 'maps/dlc3_code.gsc').write_text(
                'dlc3_fx() { animscripts\\utility::setFootstepEffect("wood", 1); }')
            (root / 'animscripts/utility.gsc').write_text(
                '#include animscripts\\missing_campaign_dependency;\n'
                'setFootstepEffect(name, fx) { level._effect[name] = fx; }\n'
                'unrelated_ai() { missing_builtin(); }')
            # A map's copy still counts as an override of a raw stock script.
            sources = gscport.Sources([root], [], None, [root])
            translator = gscport.Translator(sources, T6Api({}, {}), gscport.PortReport())
            translator.extract(r'maps\dlc3_code', 'dlc3_fx')
            while translator.core_queue:
                translator.translate_extracted(*translator.core_queue.pop(0))
        self.assertIn((r'animscripts\utility', 'setfootstepeffect'), translator.core_funcs)
        self.assertEqual(translator.queue, [])
        self.assertEqual(translator.report.errors, [])
        self.assertEqual(translator.report.unsupported, {})


if __name__ == "__main__":
    unittest.main()
