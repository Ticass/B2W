import tempfile
import unittest
from pathlib import Path

from waw2bo2 import gsc, gscport
from waw2bo2.t6api import T6Api

ROUND_SPAWNING = '''
round_spawning()
{
	ai_calculate_health();
	count = 0;
	players = get_players();
	max = level.zombie_vars["zombie_max_ai"];
	multiplier = level.round_number / 5;
	if( multiplier < 1 )
		multiplier = 1;
	max += int( ( ( get_players().size - 1 ) * level.zombie_vars["zombie_ai_per_player"] ) * multiplier );
	if(level.round_number < 3 && level.script == "nazi_zombie_asylum")
		max = 6;
	else if ( level.first_round )
		max = int( max * 0.2 );
	level.zombie_total = max;
	while( count < max )
		count++;
}
'''

HEALTH_STEP = '''
ai_calculate_health()
{
	if( level.round_number >= 10 )
	{
		level.zombie_health += Int( level.zombie_health * level.zombie_vars["zombie_health_increase_percent"] );
		return;
	}
	if( level.round_number > 1 )
		level.zombie_health = Int( level.zombie_health + level.zombie_vars["zombie_health_increase"] );
}
'''

PERKS = '''
vending_trigger_think()
{
	perk = self.script_noteworthy;
	for( ;; )
	{
		self waittill( "trigger", player );
		cost = level.zombie_vars["zombie_perk_cost"];
		switch( perk )
		{
		case "specialty_armorvest":
			cost = 2500;
			break;
		case "specialty_quickrevive":
			cost = 1500;
			break;
		}
		player SetPerk( perk );
	}
}
'''


def _api() -> T6Api:
    return T6Api(builtins={"int": (1, 1), "get_players": (0, 0), "getplayers": (0, 0), "newhudelem": (0, 0)},
                 scripts={"maps\\mp\\zombies\\_zm": {"init": 0},
                          "maps\\mp\\zombies\\_zm_utility": {"get_players": 0}},
                 methods={"setperk": (1, 1), "waittill": (1, 3), "setshader": (1, 3), "setvalue": (1, 1)})


def _port(root: Path, stock_map: bool, level: str = "testmap") -> tuple[str, str]:
    (root / "maps").mkdir(exist_ok=True)
    (root / f"maps/{level}.gsc").write_text(
        "main() { maps\\_zombiemode::main(); maps\\_zombiemode_perks::init(); }")
    (root / "maps/_zombiemode.gsc").write_text("main() {}\n" + ROUND_SPAWNING)
    (root / "maps/_zombiemode_perks.gsc").write_text("init() { thread vending_trigger_think(); }\n" + PERKS)
    gscport.port_map(gscport.Sources([root], [], root), _api(), level, root / "out", stock_map=stock_map)
    generated = root / "out/maps/mp/waw"
    perks = (generated / "_zombiemode_perks.gsc")
    return (generated / "_waw2bo2_core.gsc").read_text(), perks.read_text() if perks.exists() else ""


class StockMapPolicyTests(unittest.TestCase):
    def test_the_maps_spawning_sets_its_own_count(self):
        # WaW drives: no BO2 count formula is installed beside the map's spawning
        with tempfile.TemporaryDirectory() as tmp:
            core, _ = _port(Path(tmp), stock_map=False)
            self.assertNotIn("max_zombie_func", core)
            self.assertFalse(hasattr(gscport.Translator, "extract_round_total"))

    def test_custom_maps_keep_their_own_quick_revive_logic(self):
        with tempfile.TemporaryDirectory() as tmp:
            core, perks = _port(Path(tmp), stock_map=False)
            self.assertNotIn("waw2bo2_stock_map_policies", core)
            self.assertNotIn("waw_stock_perk_cost", perks)
            self.assertIn("cost = 1500;", perks)

    def test_stock_maps_keep_waws_quick_revive(self):
        # WaW: Quick Revive solo does nothing; no BO-style solo life or price
        with tempfile.TemporaryDirectory() as tmp:
            core, perks = _port(Path(tmp), stock_map=True, level="nazi_zombie_sumpf")
            self.assertIn("cost = 1500;", perks)
            self.assertIn("cost = 2500;", perks)
            self.assertNotIn("waw_stock_perk_cost", perks)
            self.assertNotIn("waw2bo2_solo_revive", core)
            self.assertNotIn("waw2bo2_stock_map_policies", core)

    def test_health_comes_from_the_source_step_on_every_map(self):
        # WaW: round_spawning calls ai_calculate_health; dog rounds run
        # dog_round_spawning instead, so they never raise health.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "maps").mkdir()
            (root / "maps/_zombiemode.gsc").write_text(ROUND_SPAWNING + HEALTH_STEP)
            tr = gscport.Translator(gscport.Sources([root], [], None), _api(), gscport.PortReport())
            self.assertEqual(tr.extract_round_health(), "zombiemode__ai_calculate_health_register")
            while tr.core_queue:
                tr.level_state_parts.append(tr.translate_extracted(*tr.core_queue.pop(0)))
            core = "\n".join(tr.level_state_parts)
            self.assertIn("level.waw2bo2_round_health_func = ::zombiemode__ai_calculate_health;", core)
            self.assertIn('level.zombie_vars["zombie_health_increase_percent"]', core)

    def test_round_spawning_without_a_health_step_is_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "maps").mkdir()
            (root / "maps/_zombiemode.gsc").write_text("round_spawning() { count = 0; level.zombie_total = 4; }")
            tr = gscport.Translator(gscport.Sources([root], [], None), _api(), gscport.PortReport())
            self.assertIsNone(tr.extract_round_health())
            # reported, never a build error (Verruckt failed to build on it)
            self.assertFalse(tr.report.errors)
            self.assertTrue(any("no zombie health function" in k for k in tr.report.unsupported))

    def test_older_framework_health_step_in_the_round_loop(self):
        # Verruckt: round_think calls ai_calculate_health itself; the source
        # loop runs it, so BO2's round_spawning must not recompute health
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "maps").mkdir()
            (root / "maps/_zombiemode.gsc").write_text(
                "round_spawning() { count = 0; level.zombie_total = 4; }\n"
                "round_think() { for( ;; ) { ai_calculate_health(); level.round_number++; } }\n" + HEALTH_STEP)
            tr = gscport.Translator(gscport.Sources([root], [], None), _api(), gscport.PortReport())
            self.assertEqual(tr.extract_round_health(source_loop=True), "waw2bo2_health_in_round_loop_register")
            self.assertIn("level.waw2bo2_health_in_round_loop = 1;", "\n".join(tr.level_state_parts))
            self.assertFalse(tr.report.errors)
            # without the source loop nobody would call the step: BO2's stays, reported
            tr2 = gscport.Translator(gscport.Sources([root], [], None), _api(), gscport.PortReport())
            self.assertIsNone(tr2.extract_round_health())

    def test_zombie_var_divisor_is_kept(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "maps").mkdir()
            (root / "maps/_zombiemode.gsc").write_text(
                'init_levelvars() { set_zombie_var( "zombie_health_increase_percent", 10, 100 ); '
                'set_zombie_var( "zombie_health_start", 150 ); }')
            tr = gscport.Translator(gscport.Sources([root], [], None), _api(), gscport.PortReport())
            self.assertEqual(tr.framework_zombie_vars(), "zombiemode__init_zombie_vars_defaults")
            core = "\n".join(tr.level_state_parts)
            self.assertIn('level.waw2bo2_zombie_var_defaults["zombie_health_increase_percent"] = 10;', core)
            self.assertIn('level.waw2bo2_zombie_var_divisors["zombie_health_increase_percent"] = 100;', core)
            self.assertNotIn('level.waw2bo2_zombie_var_divisors["zombie_health_start"]', core)

    def test_developer_blocks_are_not_defaults(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "maps").mkdir()
            (root / "maps/_zombiemode.gsc").write_text(
                'init_levelvars() { set_zombie_var( "zombie_score_start", 500 );\n/#\n'
                'if( GetDvarInt( "zombie_cheat" ) >= 1 ) { set_zombie_var( "zombie_score_start", 100000 ); }\n#/\n'
                'set_zombie_var( "zombie_score_kill", 50 ); }')
            tr = gscport.Translator(gscport.Sources([root], [], None), _api(), gscport.PortReport())
            tr.framework_zombie_vars()
            core = "\n".join(tr.level_state_parts)
            self.assertIn('level.waw2bo2_zombie_var_defaults["zombie_score_start"] = 500;', core)
            self.assertIn('level.waw2bo2_zombie_var_defaults["zombie_score_kill"] = 50;', core)

    def test_compat_runtime_has_no_solo_revive_rule(self):
        script = gsc.parse((gscport.COMPAT_DIR / "_waw2bo2_compat.gsc").read_text(), "compat")
        for name in ("waw_linkto", "waw_linked_trigger_contacts"):
            self.assertIn(name, script.functions)
        for name in ("waw_stock_perk_cost", "waw_stock_solo_revive", "waw_stock_solo_revive_bought"):
            self.assertNotIn(name, script.functions)
        self.assertNotIn("waw_round_health", script.functions)

    def test_stock_maps_draw_the_source_round_counter(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "maps").mkdir()
            (root / "maps/_zombiemode.gsc").write_text(
                'precache_shaders() { PrecacheShader( "hud_chalk_1" ); PrecacheShader( "hud_chalk_2" ); '
                'PrecacheShader( "hud_other" ); }\n'
                # the HUD assignment directly follows a block statement, as in WaW
                'round_start() { level.round_number = 1; level.first_round = true; '
                'players = get_players(); for( i = 0; i < players.size; i++ ) { players[i].x = 0; } '
                'level.chalk_hud1 = create_chalk_hud(); level.chalk_hud2 = create_chalk_hud( 64 ); }\n'
                'create_chalk_hud( x ) { hud = newhudelem(); hud SetShader( "hud_chalk_1", 64, 64 ); return hud; }\n'
                'chalk_one_up() { hud = level.chalk_hud1; if( level.first_round ) hud.alpha = 1; '
                'hud SetShader( "hud_chalk_" + level.round_number, 64, 64 ); hud SetValue( level.round_number ); }\n'
                'chalk_round_hint() { level.chalk_hud2.alpha = 0; }\n')
            tr = gscport.Translator(gscport.Sources([root], [], None), _api(), gscport.PortReport())
            name = tr.extract_round_counter()
            self.assertEqual(name, "waw2bo2_waw_round_counter")
            while tr.core_queue:
                tr.level_state_parts.append(tr.translate_extracted(*tr.core_queue.pop(0)))
            core = "\n".join(tr.level_state_parts)
            self.assertIn("level.waw2bo2_waw_round_counter = 1;", core)
            self.assertIn('precacheshader( "hud_chalk_1" );', core)
            self.assertIn('precacheshader( "hud_chalk_2" );', core)
            self.assertNotIn("hud_other", core)
            self.assertEqual(tr.report.waw_hud_materials, ["hud_chalk_1", "hud_chalk_2"])
            self.assertIn("level.zombie_round_change_custom = ::waw2bo2_waw_round_start;", core)
            self.assertIn("level.round_think_func = maps\\mp\\waw\\_waw2bo2_zm::round_think;", core)
            self.assertIn('level waittill( "end_of_round" );', core)
            self.assertIn("zombiemode__chalk_round_hint", core)
            # only the HUD objects of the source start-of-game, never BO2's round state
            state = core[core.index("zombiemode__round_start_level_state()"):]
            state = state[:state.index("}")]
            self.assertIn("level.chalk_hud1 =", state)
            self.assertIn("level.chalk_hud2 =", state)
            self.assertNotIn("round_number", state)
            self.assertNotIn("first_round", state)
            self.assertIn("zombiemode__create_chalk_hud()", state)
            self.assertNotIn("stub_", core)
            # the run-time built material name goes through the converted table
            self.assertIn('waw_hud_material( "hud_chalk_" + level.round_number )', core)
            self.assertIn('SetShader( "hud_chalk_1", 64, 64 )', core)

    def test_hud_material_table_keys_survive_the_literal_rewrite(self):
        source = gscport.hud_material_table_source({"hud_chalk_1": "waw_world/hud_chalk_1"})
        self.assertIn('materials["h" + "ud_chalk_1"] = "waw_world/hud_chalk_1";', source)
        self.assertNotIn('"hud_chalk_1"', source)

    def test_live_ragdoll_dies_at_frame_end_unless_its_script_killed_it(self):
        script = gsc.parse((gscport.COMPAT_DIR / "_waw2bo2_compat.gsc").read_text(), "compat")
        start = script.functions["waw_startragdoll"]
        self.assertIn("waw_ragdoll_doomed", gsc.emit(script.tokens[start.body_open:start.body_close + 1]))
        doomed = script.functions["waw_ragdoll_doomed"]
        body = gsc.emit(script.tokens[doomed.body_open:doomed.body_close + 1])
        self.assertLess(body.index("waittillframeend"), body.index("dodamage"))
        self.assertIn("isalive( self )", body)
        guard = script.functions["waw_dodamage"]
        self.assertIn("if ( !isdefined( self ) )", gsc.emit(script.tokens[guard.body_open:guard.body_close + 1]))
        self.assertIn("dodamage", gscport.API["compat_wrappers"])

    def test_explicit_waw_damage_override_call_runs_the_maps_own(self):
        # Shi No Numa's flogger calls player_damage_override directly: the map's
        # own function runs (source-driven), no compat stand-in
        self.assertNotIn("player_damage_override", gscport.API["function_renames"])
        self.assertTrue(gscport.source_drives(gscport.ZOMBIEMODE, "player_damage_override"))

if __name__ == "__main__":
    unittest.main()
