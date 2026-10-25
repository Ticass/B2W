import tempfile
import unittest
from pathlib import Path

from waw2bo2 import gscport
from waw2bo2.t6api import T6Api

ZOMBIEMODE = '''
round_think()
{
	for( ;; )
	{
		maxreward = 50 * level.round_number;
		add_later_round_spawners();
		chalk_one_up();
		level thread [[level.round_spawn_func]]();
		round_wait();
		level.first_round = false;
		level thread spectators_respawn();
		wait( level.zombie_vars["zombie_between_round_time"] );
		level.round_number++;
		level notify( "between_round_over" );
	}
}
round_wait()
{
	wait( 1 );
	while( level.zombie_total > 0 )
		wait( 0.5 );
}
add_later_round_spawners()
{
	level.later = 1;
}
chalk_one_up()
{
	level.chalk = level.round_number;
}
spectators_respawn()
{
	level.respawned = 1;
}
'''


def _api() -> T6Api:
    # BO2's _zm defines functions of the same names; the source must still win
    return T6Api(builtins={"wait": (1, 1)},
                 scripts={"maps\\mp\\zombies\\_zm": {"round_wait": 0, "spectators_respawn": 0, "round_think": 1}},
                 methods={})


class SourceRoundLoopTests(unittest.TestCase):
    def _translate(self, text):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "maps").mkdir()
            (root / "maps/_zombiemode.gsc").write_text(text)
            tr = gscport.Translator(gscport.Sources([root], [], None), _api(), gscport.PortReport())
            name = tr.extract_round_loop()
            while tr.core_queue:
                tr.level_state_parts.append(tr.translate_extracted(*tr.core_queue.pop(0)))
            return tr, name, "\n".join(tr.level_state_parts)

    def test_the_map_round_loop_drives_with_bo2_glue_at_its_anchors(self):
        tr, name, core = self._translate(ZOMBIEMODE)
        self.assertEqual(name, "waw2bo2_source_round_loop")
        self.assertIn("level.round_think_func = ::waw2bo2_source_round_think;", core)
        # the source helpers run, not BO2's functions of the same name
        self.assertIn("zombiemode__round_wait()", core)
        self.assertIn("zombiemode__add_later_round_spawners()", core)
        self.assertNotIn("maps\\mp\\zombies\\_zm::round_wait", core)
        # glue around the anchors, in order
        order = [core.index(s) for s in (
            "_waw2bo2_zm::waw2bo2_round_spawn_ready();",
            "level thread [[level.round_spawn_func]]();",
            "_waw2bo2_zm::waw2bo2_round_started();",
            "_waw2bo2_zm::waw2bo2_round_waited();",
            "level.round_number++;",
            "_waw2bo2_zm::waw2bo2_round_advanced();")]
        self.assertEqual(order, sorted(order))

    def test_engine_glue_functions_stay_bo2(self):
        _, _, core = self._translate(ZOMBIEMODE)
        self.assertIn("maps\\mp\\zombies\\_zm::spectators_respawn", core)
        self.assertNotIn("zombiemode__spectators_respawn()", core)

    def test_a_loop_without_the_anchors_keeps_bo2(self):
        tr, name, _ = self._translate("round_think() { for(;;) { wait( 1 ); } }")
        self.assertIsNone(name)
        self.assertTrue(any("round loop" in k for k in tr.report.unsupported))


if __name__ == "__main__":
    unittest.main()
