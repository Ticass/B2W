import tempfile
import unittest
from pathlib import Path

from waw2bo2 import gscport
from waw2bo2.t6api import T6Api


def _api() -> T6Api:
    return T6Api(builtins={"getentarray": (2, 2)},
                 scripts={"maps\\mp\\zombies\\_zm": {"init": 0}},
                 methods={"setperk": (1, 1), "hasperk": (1, 1), "unsetperk": (1, 1)})


class FrameworkHookTests(unittest.TestCase):
    def port(self, files: dict[str, str]) -> tuple[str, str, gscport.PortReport]:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name, text in files.items():
                (root / "src" / name).parent.mkdir(parents=True, exist_ok=True)
                (root / "src" / name).write_text(text, encoding="utf-8")
            sources = gscport.Sources([root / "src"], [], None)
            report = gscport.port_map(sources, _api(), "testmap", root / "out")
            waw = root / "out" / "maps" / "mp" / "waw"
            perks = waw / "_zombiemode_perks_custom.gsc"
            return ((waw / "testmap.gsc").read_text(encoding="utf-8"),
                    perks.read_text(encoding="utf-8") if perks.exists() else "", report)

    def test_override_inits_of_map_scripts_run_before_the_player_wait(self):
        # Electric Cherry & co.: a custom perk pack only WaW's (overridden) _zombiemode::main initialises
        main, perks, report = self.port({
            "maps/testmap.gsc": "main()\n{\n\tmaps\\_zombiemode::main();\n\tafter();\n}\nafter() {}\n",
            "maps/_zombiemode.gsc": "main()\n{\n\tmaps\\_zombiemode_perks::init();\n"
                                    "\tmaps\\_zombiemode_perks_custom::init();\n\tmaps\\_zombiemode_perks_custom::init();\n"
                                    "\tmaps\\_zombiemode_perks_custom::with_args( 1 );\n}\n",
            "maps/_zombiemode_perks_custom.gsc": "init()\n{\n\tself SetPerk( \"specialty_boost\" );\n"
                                                 "\tif ( self HasPerk( \"specialty_rof\" ) ) {}\n}\n"
                                                 "with_args( a ) {}\n",
        })
        post = main[main.index("waw_main_post()"):]
        self.assertEqual(post.count("maps\\mp\\waw\\_zombiemode_perks_custom::init();"), 1)
        self.assertLess(post.index("_zombiemode_perks_custom::init();"), post.index("initial_players_connected"))
        self.assertNotIn("_zm_perks::init", post)     # framework scripts stay BO2's
        self.assertIn("maps\\_zombiemode_perks_custom", report.ported)
        self.assertTrue(any("with_args takes arguments" in e for e in report.errors))
        # WaW-only perk names never reach T6 SetPerk/HasPerk ("Unknown perk")
        self.assertIn(f"self {gscport.COMPAT}::waw_setperk( \"specialty_boost\" )", perks)
        self.assertIn(f"self {gscport.COMPAT}::waw_hasperk( \"specialty_rof\" )", perks)

    def test_deathanim_keeps_bo2_state(self):
        # BO2 deathanim is an ASD state; the WaW value (and its WaW table read) must go,
        # and an unbraced if/else branch must stay a statement
        tokens = gscport.gsc.tokenize("f()\n{\n\tif( z[k].has_legs )\n\t\tz[k].deathanim = random( level.t[ z[k].animname ] );\n"
                                      "\telse\n\t\tself.deathanim = %ai_death;\n\tx = a.deathanim == b;\n}\n")
        self.assertEqual(gscport.keep_bo2_deathanims(tokens), 2)
        text = gscport.gsc.emit(tokens)
        self.assertIn("if( z[k].has_legs )\n\t\tz[k].deathanim = z[k].deathanim;", text)
        self.assertIn("else\n\t\tself.deathanim = self.deathanim;", text)
        self.assertNotIn("level.t", text)
        self.assertIn("x = a.deathanim == b;", text)

    def test_framework_anim_tables_the_ported_scripts_read(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            files = {
                "maps/testmap.gsc": "main()\n{\n\tmaps\\_zombiemode::main();\n\tx = level._zombie_board_taunt[\"zombie\"][2];\n}\n",
                "maps/_zombiemode.gsc": "#using_animtree( \"generic_human\" );\nmain() {}\ninit_standard_zombie_anims()\n{\n"
                                        "\tlevel._zombie_board_taunt[\"zombie\"] = [];\n"
                                        "\tlevel._zombie_board_taunt[\"zombie\"][2] = %ai_zombie_taunts_9;\n"
                                        "\tlevel._zombie_melee[\"zombie\"] = [];\n}\n",
            }
            for name, text in files.items():
                (root / "src" / name).parent.mkdir(parents=True, exist_ok=True)
                (root / "src" / name).write_text(text, encoding="utf-8")
            sources = gscport.Sources([root / "src"], [], None)
            gscport.port_map(sources, _api(), "testmap", root / "out")
            core = (root / "out/maps/mp/waw/_waw2bo2_core.gsc").read_text(encoding="utf-8")
            main = (root / "out/maps/mp/waw/testmap.gsc").read_text(encoding="utf-8")
        self.assertIn('level._zombie_board_taunt["zombie"][2] = "ai_zombie_taunts_9";', core)
        self.assertNotIn("_zombie_melee", core)     # not read by the map
        self.assertIn("framework_level_state()\n{\n\tzombiemode__init_standard_zombie_anims_level_state();", core)
        self.assertIn(f"{gscport.CORE}::framework_level_state();", main)
        self.assertIn(f"{gscport.PRECACHE}::init();", main)

    def test_named_models_precache_in_the_first_frame(self):
        # T6: "precacheModel must be called before any wait statements"; WaW perk
        # scripts precache the machine's on-model only when the power comes on
        text = gscport.precache_source(["p6_zm_vending_electric_cherry_on", "a", "a"])
        self.assertEqual(text.count("waw_precachemodel"), 2)
        self.assertIn(f'{gscport.COMPAT}::waw_precachemodel( "p6_zm_vending_electric_cherry_on" );', text)
        self.assertIn("waw_precachemodel", gscport.API["compat_wrappers"] and
                      (gscport.COMPAT_DIR / "_waw2bo2_compat.gsc").read_text(encoding="utf-8"))
        self.assertIn("precachemodel", gscport.API["compat_wrappers"])

    def test_no_override_main_no_hooks(self):
        main, _, _ = self.port({"maps/testmap.gsc": "main()\n{\n\tmaps\\_zombiemode::main();\n}\n"})
        self.assertNotIn("inits of the map's own scripts", main)


if __name__ == "__main__":
    unittest.main()
