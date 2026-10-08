import unittest
import tempfile
from pathlib import Path

from waw2bo2 import gsc, gscport
from waw2bo2.t6api import T6Api


class ReviveStateTests(unittest.TestCase):
    def test_buildable_checks_read_native_state_but_custom_writes_survive(self):
        source = '''collect() {
            if (players[i] UseButtonPressed() && !players[i].being_revived)
                take_part();
            while (player UseButtonPressed() && !player.being_revived) wait .3;
            self.being_revived = true;
            if (isdefined(self.being_revived)) self.being_revived = false;
        }'''
        tokens = gsc.tokenize(source)
        self.assertEqual(gscport.bridge_revive_reads(tokens), 2)
        result = gsc.emit(tokens)
        parsed = gsc.parse(result, 'collect')
        calls = [r for r in gsc.references(parsed.tokens)
                 if r.name == 'waw_being_revived']
        self.assertEqual(len(calls), 2)
        self.assertTrue(all(r.qualifier == gscport.COMPAT for r in calls))
        self.assertIn('waw_being_revived( players[i] )', result)
        self.assertIn('self.being_revived = true;', result)
        self.assertIn('isdefined(self.being_revived)', result)
        self.assertEqual(gscport.bridge_revive_reads(gsc.tokenize(result)), 0)

    def test_nested_receiver_and_parenthesized_condition(self):
        tokens = gsc.tokenize('f() { if (!(team.players[index + 1].being_revived)) ok(); }')
        self.assertEqual(gscport.bridge_revive_reads(tokens), 1)
        result = gsc.emit(tokens)
        self.assertIn('waw_being_revived( team.players[index + 1] )', result)
        gsc.parse(result, 'test')

    def test_unrelated_fields_and_strings_are_unchanged(self):
        source = 'f() { self.other = "being_revived"; if (!self.other) ok(); }'
        tokens = gsc.tokenize(source)
        self.assertEqual(gscport.bridge_revive_reads(tokens), 0)
        self.assertEqual(gsc.emit(tokens), source)


class PlayerStatTests(unittest.TestCase):
    def test_native_counters_and_indexed_player_reads(self):
        tokens = gsc.tokenize('f() { x = players[i].stats["headshots"]; y = self.stats["downs"]; }')
        self.assertEqual(gscport.bridge_player_stat_reads(tokens), 2)
        result = gsc.emit(tokens)
        self.assertIn('waw_player_stat( players[i], "headshots" )', result)
        self.assertIn('waw_player_stat( self, "downs" )', result)
        gsc.parse(result, 'stats')
        self.assertEqual(gscport.bridge_player_stat_reads(gsc.tokenize(result)), 0)

    def test_custom_keys_writes_and_defined_checks_survive(self):
        source = '''f() {
            self.stats["headshots"]++;
            self.stats["kills"] = 2;
            ++self.stats["kills"];
            self.stats["headshots"] %= 2;
            x = self.stats["boxRolls"];
            if (isdefined(self.stats["downs"])) ok();
        }'''
        tokens = gsc.tokenize(source)
        self.assertEqual(gscport.bridge_player_stat_reads(tokens), 0)
        self.assertEqual(gsc.emit(tokens), source)

    def test_all_stock_waw_keys_have_defaults(self):
        compat = (gscport.COMPAT_DIR / '_waw2bo2_compat.gsc').read_text()
        for key in ('kills', 'score', 'downs', 'revives', 'perks', 'headshots', 'zombie_gibs'):
            self.assertIn(f'self.stats["{key}"] = 0;', compat)
        self.assertIn('native_key = "perks_drank";', compat)
        self.assertIn('return player.score_total;', compat)

    def test_scoreboard_player_names_and_authored_font_scales(self):
        source = '''f() {
            hud.fontscale = 1.6;
            hud settext(players[i].playername);
            label = "playername";
        }'''
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'maps').mkdir()
            (root / 'maps/test.gsc').write_text(source)
            sources = gscport.Sources([root], [], None)
            report = gscport.PortReport()
            translator = gscport.Translator(sources, T6Api(builtins={}, scripts={}, methods={'settext': (1, 1)}), report)
            result = translator.translate('maps/test')
        self.assertIn('players[i].name', result)
        self.assertIn('hud.fontscale = 1.6;', result)
        self.assertIn('"playername"', result)
        self.assertEqual(report.rewrites['WaW playername field -> T6 name'], 1)


if __name__ == '__main__':
    unittest.main()
