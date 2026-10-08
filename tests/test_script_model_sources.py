"""Runtime model requests must survive the full original WaW lookup."""
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock

from waw2bo2 import gscport, t6bridge, wawsource


class ScriptModelSourcesTest(unittest.TestCase):
    def test_models_named_through_helpers_are_found(self):
        # WaW _loadout: the player's arms reach precacheModel through a helper;
        # without them T6 draws no viewmodel (hands nor gun)
        source = '''set_player_viewmodel( viewmodel )
{
    precacheModel( viewmodel );
    level.player_viewmodel = viewmodel;
}
attach_second( tag, model )
{
    self attach( model, tag );
}
init_loadout()
{
    set_player_viewmodel( "viewmodel_usa_marine_arms");
    attach_second( "tag_weapon", "weapon_prop" );
    set_switch_weapon( "zombie_thompson" );
}
'''
        self.assertEqual(t6bridge.wrapped_model_literals([source]), {"viewmodel_usa_marine_arms", "weapon_prop"})

    def test_missing_swap_keeps_source_lights_and_notifications(self):
        source = '''main()
{
    // self setmodel("absent_on");
    self setmodel("absent_on");
    self setmodel("present_on");
    self setmodel(variable);
    maps\\mp\\waw\\_waw2bo2_compat::waw_precachemodel("absent_on");
    custom::setmodel("absent_on");
    playfxontag(level._effect["light"], self, "tag_origin");
    level notify("source_power_on");
}
'''
        result, guarded = gscport.guard_missing_model_calls(source, {'absent_on'})
        self.assertEqual(guarded, ['absent_on', 'absent_on'])
        self.assertIn('// self setmodel("absent_on");', result)
        self.assertIn('self setmodel("present_on");', result)
        self.assertIn('self setmodel(variable);', result)
        self.assertIn('custom::setmodel("absent_on");', result)
        self.assertIn('playfxontag(level._effect["light"], self, "tag_origin");', result)
        self.assertIn('level notify("source_power_on");', result)
        self.assertEqual(gscport.guard_missing_model_calls(result, {'absent_on'}), (result, []))

    def test_local_script_function_is_not_a_native_model_api(self):
        source = 'setmodel(name) { return name; }\nmain() { setmodel("absent_on"); }\n'
        self.assertEqual(gscport.guard_missing_model_calls(source, {'absent_on'}), (source, []))

    def test_map_definition_wins_over_stock_and_raw(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            model = root / 'xmodel/custom_switch_on.json'
            model.parent.mkdir()
            model.write_text('{}')
            stock, raw = Mock(), Mock()
            roots = [root]
            report = t6bridge.StageReport('arbitrary_map')
            self.assertEqual(t6bridge.recover_script_models(
                report, {'custom_switch_on'}, roots, stock, raw), {'custom_switch_on'})
            stock.root_for.assert_not_called()
            raw.compile.assert_not_called()

    def test_stock_and_raw_runtime_models_widen_staging_roots(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            stock_root, raw_root = root / 'stock', root / 'source'
            for dump, name in ((stock_root, 'stock_switch_on'), (raw_root, 'custom_switch_on')):
                model = dump / 'xmodel' / f'{name}.json'
                model.parent.mkdir(parents=True)
                model.write_text('{}')
            stock, raw = Mock(), Mock()
            stock.root_for.side_effect = lambda kind, name: (
                ('common', stock_root) if name == 'stock_switch_on' else None)
            raw.compile.side_effect = lambda kind, name: raw_root if name == 'custom_switch_on' else None
            roots = [root / 'map']
            report = t6bridge.StageReport('arbitrary_map')
            wanted = {'stock_switch_on', 'custom_switch_on', 'absent_switch_on'}
            self.assertEqual(t6bridge.recover_script_models(report, wanted, roots, stock, raw),
                             {'stock_switch_on', 'custom_switch_on'})
            self.assertIn(stock_root, roots)
            self.assertIn(raw_root, roots)
            self.assertLess(roots.index(stock_root), roots.index(raw_root))
            raw.compile.assert_any_call('xmodel', 'custom_switch_on')
            raw.compile.assert_any_call('xmodel', 'absent_switch_on')
            self.assertNotIn(unittest.mock.call('xmodel', 'stock_switch_on'), raw.compile.call_args_list)
            nodes = {n['name']: n for n in report.scripts['model_sources']}
            self.assertEqual(nodes['absent_switch_on']['status'], 'missing_waw_source')
            self.assertEqual(nodes['custom_switch_on']['provenance'], 'WAW_SOURCE_ASSET')
            self.assertEqual(report.scripts['requested_models'], sorted(wanted))

    def test_raw_xmodel_uses_binary_waw_definition(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'raw/fx').mkdir(parents=True)
            model = root / 'raw/xmodel/custom_switch_on'
            model.parent.mkdir()
            model.write_bytes(b'original WaW xmodel')
            source = wawsource.WawSourceAssets(root, root, root / 'unlinker.exe', root / 'work')
            self.assertEqual(source.source('xmodel', 'custom_switch_on'), model)
            self.assertIsNone(source.source('xmodel', 'absent_switch_on'))


if __name__ == '__main__':
    unittest.main()
