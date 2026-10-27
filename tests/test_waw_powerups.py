import tempfile
import unittest
from pathlib import Path

from waw2bo2 import gsc, gscport
from waw2bo2.t6api import T6Api


def emit(source, rewrite):
    tokens = gsc.tokenize(source)
    count = rewrite(tokens)
    return gsc.emit(tokens), count


class TeamZombieVarTests(unittest.TestCase):
    def test_source_writes_reach_bo2_teams(self):
        out, count = emit('f() { level.zombie_vars["zombie_point_scalar"] = 2; wait 30; '
                          'level.zombie_vars["zombie_drop_item"] = 1; '
                          'set_zombie_var( "zombie_insta_kill", 0 ); '
                          'x = level.zombie_vars["zombie_insta_kill"] == 1; }',
                          gscport.mirror_team_zombie_vars)
        self.assertEqual(count, 2)
        self.assertIn('= 2; maps\\mp\\waw\\_waw2bo2_compat::waw_team_zombie_var( "zombie_point_scalar" );', out)
        self.assertIn('0 ); maps\\mp\\waw\\_waw2bo2_compat::waw_team_zombie_var( "zombie_insta_kill" );', out)
        self.assertNotIn('waw_team_zombie_var( "zombie_drop_item" )', out)   # BO2 keeps it global


class NativeArgumentTests(unittest.TestCase):
    def translator(self, root):
        return gscport.Translator(gscport.Sources([root], [], None), T6Api({}, {}), gscport.PortReport())

    def adapt(self, source, native, name):
        with tempfile.TemporaryDirectory() as temp:
            tr = self.translator(Path(temp))
            tokens = gsc.tokenize(source)
            ref = next(r for r in gsc.references(tokens) if r.name.lower() == name)
            tr.adapt_native_arguments(tokens, ref, native, gscport._argc(tokens, ref.index), 'test')
            return gsc.emit(tokens)

    def test_barrier_helpers_get_the_barrier(self):
        out = self.adapt('f() { if ( all_chunks_intact( windows[i].barrier_chunks ) ) return; }',
                         'maps\\mp\\zombies\\_zm_utility::all_chunks_intact', 'all_chunks_intact')
        self.assertIn('all_chunks_intact( windows [ i ], windows [ i ].barrier_chunks', out)

    def test_replace_chunk_runs_on_its_window(self):
        out = self.adapt('f() { windows thread maps\\_zombiemode_blockers_new::replace_chunk( chunk, false, true ); }',
                         'maps\\mp\\zombies\\_zm_blockers::replace_chunk', 'replace_chunk')
        # WaW's has_perk flag false: BO2 tests isdefined( perk )
        self.assertIn('replace_chunk( windows, chunk, undefined, 0, true', out)

    def test_bo2_arity_calls_stay(self):
        source = 'f() { if ( all_chunks_intact( self, self.barrier_chunks ) ) return; }'
        out = self.adapt(source, 'maps\\mp\\zombies\\_zm_utility::all_chunks_intact', 'all_chunks_intact')
        self.assertEqual(out, gsc.emit(gsc.tokenize(source)))


class NativeReplacementTests(unittest.TestCase):
    def test_map_power_ups_replace_bo2s(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'maps').mkdir()
            (root / 'maps/_zombiemode_powerups.gsc').write_text(
                'init() { }\npowerup_drop( drop_point ) { }\ncheck_for_instakill( player ) { }\n')
            api = T6Api({}, {'maps\\mp\\zombies\\_zm_powerups': {'init': 0, 'powerup_drop': 1,
                                                                 'check_for_instakill': 3}})
            tr = gscport.Translator(gscport.Sources([root], [], None), api, gscport.PortReport())
            calls = tr.native_replacements()
            self.assertIn('replacefunc( maps\\mp\\zombies\\_zm_powerups::powerup_drop, '
                          'maps\\mp\\waw\\_waw2bo2_core::waw2bo2_native_zm_powerups_powerup_drop );', calls)
            body = tr.level_state_parts[-1]
            # BO2's arity in, the map's arguments on
            self.assertIn('waw2bo2_native_zm_powerups_check_for_instakill( arg0, arg1, arg2 )', body)
            self.assertIn('return self maps\\mp\\waw\\_waw2bo2_core::zombiemode_powerups__check_for_instakill( arg0 );', body)
            # BO2's registrations run before the map's init
            init = body[body.index('waw2bo2_native_zm_powerups_init('):]
            self.assertLess(init.index('_zm_powerups::init_powerups()'), init.index('zombiemode_powerups__init('))


if __name__ == '__main__':
    unittest.main()
