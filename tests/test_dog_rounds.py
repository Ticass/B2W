import re
import tempfile
import unittest
from pathlib import Path

from waw2bo2 import gsc, gscport, t6bridge, zombieappearance
from waw2bo2.t6api import T6Api

WAW_DOG = '''main()
{
	self.animTree = "dog.atr";
	self.team = "axis";
	self.type = "dog";
	switch( codescripts\\character::get_random_character(2) )
	{
	case 0:
		character\\character_sp_zombie_dog::main();
		break;
	}
}
spawner() { self setspawnerteam("axis"); }
precache() { character\\character_sp_zombie_dog::precache(); }
'''

T6_DOG = '''#include codescripts\\character;
#include character\\character_sp_zombie_dog;
#using_animtree("zm_nuked_dog");
main()
{
    self.type = "zombie_dog";
    randchar = codescripts\\character::get_random_character( 2 );
    switch ( randchar )
    {
        case 0:
            character\\character_sp_zombie_dog::main();
            break;
    }
    self setcharacterindex( randchar );
}
precache( ai_index )
{
    precacheanimstatedef( ai_index, #animtree, "zm_nuked_dog" );
    character\\character_sp_zombie_dog::precache();
}
'''

DOGS = '''init()
{
	if ( !IsDefined( level.dogs_enabled ) ) return;
	level._effect[ "lightning_dog_spawn" ] = Loadfx( "maps/zombie/fx_zombie_dog_lightning_buildup" );
	level thread dog_round_tracker();
}
dog_round_spawning()
{
	spawn_point = dog_spawn_sumpf_logic( level.enemy_dog_spawns, favorite_enemy );
	dog_health_increase();
}
dog_spawn_sumpf_logic( dog_array, favorite_enemy ) { return dog_array[0]; }
dog_health_increase()
{
	if( level.dog_round_count == 1 ) level.dog_health = 350;
	if( level.dog_health > 1250 ) level.dog_health = 1250;
}
dog_round_tracker() {}
'''


class DogRoundTests(unittest.TestCase):
    def test_waw_ai_type_selects_the_native_actor_family(self):
        self.assertEqual(zombieappearance.source_actor_kind(WAW_DOG), "dog")
        self.assertEqual(zombieappearance.source_actor_kind("main() { self.type = \"human\"; }"), "human")
        self.assertEqual(zombieappearance.source_actor_kind("main() {}"), "human")
        self.assertEqual(zombieappearance.template_tree(T6_DOG), "zm_nuked_dog")
        client = zombieappearance.native_client_actor("zombie_dog", "zm_nuked_dog")
        self.assertIn('#using_animtree("zm_nuked_dog");', client)
        self.assertIn('precacheanimstatedef( ai_index, #animtree, "zm_nuked_dog" );', client)
        self.assertNotIn("zm_nuked_basic", client)

    def test_dog_actor_keeps_native_dog_ai_with_source_appearance(self):
        actor = zombieappearance.native_actor(T6_DOG, "maps\\mp\\waw\\aitype\\zombie_dog")
        self.assertIn('#using_animtree("zm_nuked_dog");', actor)
        self.assertIn("maps\\mp\\waw\\aitype\\zombie_dog::main();", actor)
        self.assertIn("maps\\mp\\waw\\aitype\\zombie_dog::precache();", actor)
        self.assertNotIn("character\\character_sp_zombie_dog", actor)

    def test_native_tree_lists_leaf_animations_only(self):
        atr = "body\n{\n\tzombie_dog_idle\n\tzombie_dog_run // comment\n\n} // end body\n"
        self.assertEqual(t6bridge.native_tree_xanims(atr), ["zombie_dog_idle", "zombie_dog_run"])

    def test_dog_rounds_start_like_the_source_framework(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "maps").mkdir()
            (root / "maps/_zombiemode.gsc").write_text("main() { maps\\_zombiemode_dogs::init(); }")
            (root / "maps/_zombiemode_dogs.gsc").write_text(DOGS)
            (root / "clientscripts").mkdir()
            (root / "clientscripts/testmap_fx.csc").write_text(
                'main() { level thread dog_start_monitor(); level thread dog_stop_monitor(); }\n'
                'dog_start_monitor() { while( 1 ) { level waittill( "dog_start" ); '
                'SetVolFog( 729.34, 971.99, 338.336, 398.623, 0.58, 0.60, 0.56, 7 );\n'
                '//VisionSetNaked(0, "zombie_sumpf_dogs", 7 );\n } }\n'
                'dog_stop_monitor() { while( 1 ) { level waittill( "dog_stop" ); '
                'SetVolFog( 404.39, 1543.52, 460.33, -244.014, 0.65, 0.84, 0.79, 6 ); } }\n')
            api = T6Api(builtins={}, scripts={"maps\\mp\\zombies\\_zm_ai_dogs": {
                "init": 0, "enable_dog_rounds": 0, "dog_spawn_sumpf_logic": 2}}, methods={})
            tr = gscport.Translator(gscport.Sources([root], [], None), api, gscport.PortReport())
            calls = tr.framework_hooks()
            # WaW drives the dog rounds: the source init is ported and called
            self.assertIn("maps\\mp\\waw\\_zombiemode_dogs::init();", calls)
            self.assertIn("maps\\_zombiemode_dogs", tr.queue)
            self.assertFalse(any("waw_dog_rounds" in c for c in calls))
            # the map's client fog monitors run on the server at the source moments
            self.assertIn("level thread maps\\mp\\waw\\_waw2bo2_core::waw2bo2_client_dog_start_0();", calls)
            fog = "\n".join(tr.level_state_parts)
            self.assertIn('level waittill( "dog_round_starting" );', fog)
            self.assertIn("waw_setvolfog( 729.34, 971.99, 338.336, 398.623, 0.58, 0.60, 0.56, 7 );", fog)
            self.assertIn('level waittill( "last_dog_down" );\n\t\twait 2;', fog)
            self.assertIn("waw_setvolfog( 404.39, 1543.52, 460.33, -244.014, 0.65, 0.84, 0.79, 6 );", fog)
            self.assertNotIn("VisionSetNaked", fog)

    def test_source_dog_init_gets_the_t6_actor_glue_first(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "maps").mkdir()
            (root / "maps/_zombiemode_dogs.gsc").write_text(
                'dog_init()\n{\n\tself.ignoreall = true;\n\tanimscripts\\dog_init::change_anim_set( "zombie" );\n}\n')
            tr = gscport.Translator(gscport.Sources([root], [], None), T6Api(builtins={}, scripts={}, methods={}),
                                    gscport.PortReport())
            text = tr.translate("maps\\_zombiemode_dogs")
            body = text[text.index("dog_init()"):]
            self.assertLess(body.index("self maps\\mp\\waw\\_waw2bo2_ai_dogs::waw2bo2_dog_actor_glue();"),
                            body.index("self.ignoreall = true;"))
            # WaW's raw-animation dog tables have no T6 consumer
            self.assertIn('maps\\mp\\waw\\_waw2bo2_compat::waw_dog_animation_set( "zombie" );', text)

    def test_owned_dog_runtime_plays_the_source_dog_sounds(self):
        bo2 = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Call of Duty Black Ops II")
        if not (bo2 / "raw/maps/mp/zombies/_zm_ai_dogs.gsc").is_file():
            self.skipTest("BO2 raw scripts not installed")
        text = gscport.owned_dogs_source(bo2)
        self.assertIn('playsoundatposition( "waw/pre_spawn"', text)
        self.assertIn('"waw/dog_trail_fire_breath"', text)
        self.assertIn('play_sound_2d( "waw/dark_sting" )', text)
        self.assertIn('play_sound_2d( "waw/bright_sting" )', text)
        self.assertNotIn('"zmb_hellhound_prespawn"', text)
        self.assertNotIn("maps\\mp\\zombies\\_zm_ai_dogs::", text)
        # the spawn lightning and other dog effects prefer WaW's converted ones
        self.assertIn('level._effect["lightning_dog_spawn"] = maps\\mp\\waw\\_waw2bo2_compat::waw_loadfx_native( '
                      '"maps/zombie/fx_zombie_dog_lightning_buildup" );', text)
        self.assertNotIn(" loadfx(", text)

    def test_owned_dog_tree_restarts_attacks_and_jumps_windows(self):
        bo2 = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Call of Duty Black Ops II")
        if not (bo2 / "raw/animstatedefs/zm_transit_dog.asd").is_file():
            self.skipTest("BO2 raw animation state graphs not installed")
        cfg = gscport.API["zombie_runtime"]["actor_templates"]["dog"]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "aitype").mkdir()
            (root / "aitype/waw_zombie_dog.gsc").write_text(
                '#using_animtree("zm_transit_dog");\nmain() { self.animstatedef = "zm_transit_dog.asd"; }\n')
            tree, atr = t6bridge.stage_owned_actor_tree(root, bo2, "zm_transit_dog", cfg)
            asd = (root / "animstatedefs" / f"{tree}.asd").read_text()
            # one bite and frozen: the attack state must restart (TranZit's shipped graph)
            self.assertIn("zm_combat_attack_player_close_range : notify attack_combat restart", asd)
            self.assertIn("zm_traverse_barrier : aliased restart notify traverse_anim", asd)
            self.assertIn("zombie_dog_run_jump_window_40", t6bridge.native_tree_xanims(atr))
            actor = (root / "aitype/waw_zombie_dog.gsc").read_text()
            self.assertIn('#using_animtree("waw2bo2_dog_actor");', actor)
            self.assertIn("dummy_anim_ref = %zombie_dog_run_jump_window_40;", actor)

    def test_compat_dog_runtime(self):
        script = gsc.parse((gscport.COMPAT_DIR / "_waw2bo2_compat.gsc").read_text(), "compat")
        for name in ("waw_dog_rounds", "waw_dog_spawn_location", "waw_dog_source_health"):
            self.assertIn(name, script.functions)
        fn = script.functions["waw_dog_spawn_location"]
        self.assertIn("level.enemy_dog_spawns", gsc.emit(script.tokens[fn.body_open:fn.body_close + 1]))

    def test_ai_trigger_contacts_include_dogs(self):
        # A WaW AI_AXIS trigger (the Betty's trigger_radius) fires for dogs;
        # T6 getaiarray( "axis" ) leaves the zombie_dog actor out (measured).
        script = gsc.parse((gscport.COMPAT_DIR / "_waw2bo2_compat.gsc").read_text(), "compat")
        body = lambda name: gsc.emit(script.tokens[script.functions[name].body_open:
                                                   script.functions[name].body_close + 1])
        helper = body("waw_trigger_ai")
        for team in ("axis", "allies", "neutral"):
            self.assertIn(f'getaispeciesarray( "{team}", "all" )', helper)
        self.assertNotIn("getaiarray", helper)
        for caller in ("waw_radius_ai_contacts", "waw_linked_trigger_contacts"):
            self.assertIn("waw_trigger_ai( flags )", body(caller))
            self.assertNotIn("getaiarray", body(caller))


if __name__ == "__main__":
    unittest.main()
