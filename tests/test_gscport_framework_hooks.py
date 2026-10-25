import tempfile
import unittest
from pathlib import Path

from waw2bo2 import gscport
from waw2bo2.t6api import T6Api


def _api() -> T6Api:
    return T6Api(builtins={"getentarray": (2, 2), "getent": (2, 2), "spawn": (2, 5)},
                 scripts={"maps\\mp\\zombies\\_zm": {"init": 0}},
                 methods={"setperk": (1, 1), "hasperk": (1, 1), "unsetperk": (1, 1), "hide": (0, 0),
                          "linkto": (1, 4)})


class FrameworkHookTests(unittest.TestCase):
    def test_custom_tesla_initializer_is_not_also_started_as_a_framework_hook(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'maps').mkdir()
            (root / 'maps/_zombiemode.gsc').write_text('main(){ maps\\_zombiemode_tesla::init(); }')
            (root / 'maps/_zombiemode_tesla.gsc').write_text('init(){ level.custom_tesla = true; }')
            tr = gscport.Translator(gscport.Sources([root], [], None), _api(), gscport.PortReport())
            tr.extract_entry(r'maps\_zombiemode_tesla', 'init')
            self.assertEqual(tr.framework_hooks(), [])

    def test_tesla_source_chain_is_bound_to_shared_native_damage_dispatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'maps').mkdir()
            (root / 'maps/testmap.gsc').write_text('main() { maps\\_zombiemode::main(); }')
            (root / 'maps/_zombiemode_tesla.gsc').write_text(
                'init() { level._effect["tesla_bolt"] = loadfx("custom/arc"); }\n'
                'is_tesla_damage(mod) { return self.damageweapon == "tesla_gun"; }\n'
                'tesla_damage_init(loc, origin, player) { self.seen_name = player.playername; '
                'self.variants = maps\\_zombiemode_spawner::get_number_variants("custom"); self arc(player); }\n'
                'arc(player) { self.chain_reached = true; }')
            api = _api()
            api.scripts[r'maps\mp\zombies\_zm_spawner'] = {'register_zombie_damage_callback': 1}
            api.builtins.update({'isdefined': (1, 1), 'isplayer': (1, 1), 'min': (2, 2),
                                 'loadfx': (1, 1), 'wait_network_frame': (0, 0)})
            report = gscport.port_map(gscport.Sources([root], [], root), api, 'testmap', root / 'out')
            generated = root / 'out/maps/mp/waw'
            core = (generated / '_waw2bo2_core.gsc').read_text()
            main = (generated / 'testmap.gsc').read_text()
            self.assertIn('register_zombie_damage_callback( ::waw_tesla_damage )', core)
            self.assertIn('player.waw_tesla_chain_active = true;', core)
            self.assertIn('player thread waw_tesla_run_chain', core)
            self.assertIn('target zombiemode_tesla__tesla_damage_init( hit_location, hit_origin, self )', core)
            self.assertIn('self.waw_tesla_chain_active = false;', core)
            self.assertIn('self.chain_reached = true', core)
            self.assertIn('self.seen_name = player.name', core)
            self.assertNotIn('player.playername', core)
            self.assertIn('_waw2bo2_compat::waw_get_number_variants("custom")', core)
            self.assertIn('waw_tesla_network_choke()', core)
            self.assertIn('min( 10,', core)
            self.assertIn('waw_tesla_init();', main)
            self.assertIn('custom/arc', report.fx)
            self.assertIn(r'maps\_zombiemode_tesla::arc', report.extracted)

    def test_template_spawnfx_bootstrap_is_not_the_engine_builtin(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            files = {
                'maps/testmap.gsc': 'main() { maps\\dlc3_code::spawnFX(); }',
                'maps/dlc3_code.gsc': 'spawnFX() { [[level.DLC3.createFX]](); '
                                      'maps\\createfx\\dlc3_fx::main(); }',
                'maps/createfx/dlc3_fx.gsc': 'main() { loadfx("custom/teleporter"); }',
            }
            for name, text in files.items():
                p = root / name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text(text)
            tr = gscport.Translator(gscport.Sources([root], [], None), _api(), gscport.PortReport())
            main = tr.translate(r'maps\testmap')
            parts = []
            while tr.core_queue:
                parts.append(tr.translate_extracted(*tr.core_queue.pop(0)))
        core = '\n'.join(parts)
        self.assertIn(gscport.CORE + '::dlc3_code__spawnfx()', main)
        self.assertIn('[[level.DLC3.createFX]]()', core)
        self.assertIn(gscport.CORE + '::dlc3_fx__main()', core)
        self.assertIn('waw_loadfx("custom/teleporter")', core)
        self.assertNotIn('waw_spawnfx()', main + core)
        self.assertEqual(tr.report.unsupported, {})

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
        self.assertTrue(any("with_args takes arguments" in e for e in report.unsupported.get(gscport.FALLBACK, [])))
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

    def test_framework_effects_the_ported_scripts_read(self):
        # WaW's _zombiemode::init_fx registers the box glow BO2 never builds
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            files = {
                "maps/testmap.gsc": "main()\n{\n\tmaps\\_zombiemode::main();\n\tlevel._effect[\"own\"] = loadfx( \"own/fx\" );\n"
                                    "\tplayfx( level._effect[\"chest_light\"], (0,0,0) );\n\tplayfx( level._effect[\"own\"], (0,0,0) );\n}\n",
                "maps/_zombiemode.gsc": "main() {}\ninit_fx()\n{\n"
                                        "\tlevel._effect[\"chest_light\"] = LoadFx( \"env/light/fx_ray_sun_sm_short\" );\n"
                                        "\tlevel._effect[\"own\"] = LoadFx( \"framework/own\" );\n"
                                        "\tlevel._effect[\"eye_glow\"] = LoadFx( \"misc/fx_zombie_eye_single\" );\n}\n",
            }
            for name, text in files.items():
                (root / "src" / name).parent.mkdir(parents=True, exist_ok=True)
                (root / "src" / name).write_text(text, encoding="utf-8")
            api = _api()
            api.builtins.update({"loadfx": (1, 1), "playfx": (2, 4)})
            report = gscport.port_map(gscport.Sources([root / "src"], [], None), api, "testmap", root / "out")
            core = (root / "out/maps/mp/waw/_waw2bo2_core.gsc").read_text(encoding="utf-8")
        self.assertIn('level.waw_effect["chest_light"]', core)
        self.assertIn("env/light/fx_ray_sun_sm_short", report.fx)
        self.assertNotIn("framework/own", core)     # the map assigns its own
        self.assertNotIn("eye_glow", core)          # never read
        self.assertIn("zombiemode__init_fx_level_state();", core)

    def test_effect_only_client_listeners_run_on_the_server(self):
        # Shi No Numa: the trap's clientnotify drives arcs spawned by its client script
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            files = {
                "maps/testmap.gsc": "main()\n{\n\tmaps\\_zombiemode::main();\n\tclientnotify( \"trap_a\" );\n}\n",
                "clientscripts/testmap_fx.csc":
                    "main()\n{\n\tprecache();\n\tlevel thread monitor( \"trap_a\", \"side_a\" );\n"
                    "\tlevel thread monitor( \"trap_b\", \"side_b\" );\n"
                    "\tlevel thread fog();\n}\n"
                    "precache()\n{\n\tlevel._effect[\"zapper\"] = loadfx( \"misc/fx_zombie_electric_trap\" );\n}\n"
                    "monitor( name, side )\n{\n\twhile ( 1 )\n\t{\n\t\tlevel waittill( name );\n"
                    "\t\tpoints = getstructarray( name, \"targetname\" );\n"
                    "\t\tfor ( i = 0; i < points.size; i++ )\n\t\t\tpoints[i] thread arc( side );\n\t}\n}\n"
                    "arc( side )\n{\n\tforward = anglestoforward( self.angles );\n\tup = anglestoup( self.angles );\n"
                    "\tplayers = getlocalplayers();\n\tself.fx = [];\n"
                    "\tfor ( i = 0; i < players.size; i++ )\n\t{\n"
                    "\t\tself.fx[i] = SpawnFx( i, level._effect[\"zapper\"], self.origin, 0, forward, up );\n"
                    "\t\ttriggerfx( self.fx[i] );\n\t}\n\tlevel waittill( side + \"off\" );\n"
                    "\tfor ( i = 0; i < self.fx.size; i++ )\n\t\tself.fx[i] delete();\n}\n"
                    "fog()\n{\n\tlevel waittill( \"dog_start\" );\n\tSetVolFog( 1, 2, 3, 4, 0.5, 0.5, 0.5, 7 );\n}\n",
            }
            for name, text in files.items():
                (root / "src" / name).parent.mkdir(parents=True, exist_ok=True)
                (root / "src" / name).write_text(text, encoding="utf-8")
            api = _api()
            api.builtins.update({"clientnotify": (1, 1)})
            report = gscport.port_map(gscport.Sources([root / "src"], [], None), api, "testmap", root / "out")
            core = (root / "out/maps/mp/waw/_waw2bo2_core.gsc").read_text(encoding="utf-8")
            main = (root / "out/maps/mp/waw/testmap.gsc").read_text(encoding="utf-8")
        self.assertIn(f'{gscport.COMPAT}::waw_clientnotify( "trap_a" )', main)
        self.assertIn('level thread waw2bo2_testmap_fx__monitor( "trap_a", "side_a" );', core)
        self.assertIn('level thread waw2bo2_testmap_fx__monitor( "trap_b", "side_b" );', core)
        # one definition each, however many threads main() starts
        self.assertEqual(core.count("\nwaw2bo2_testmap_fx__monitor( name, side )"), 1)
        self.assertEqual(core.count("\nwaw2bo2_testmap_fx__arc( side )"), 1)
        self.assertIn('level waittill( "waw_client_" + name )', core)
        self.assertIn('level waittill( "waw_client_" + side + "off" )', core)
        self.assertIn('SpawnFx( level.waw_effect["zapper"], self.origin, forward, up )', core)
        self.assertIn(f'{gscport.COMPAT}::waw_server_local_players()', core)
        self.assertIn(f'level.waw_effect["zapper"] = {gscport.COMPAT}::waw_loadfx( "misc/fx_zombie_electric_trap" )', core)
        self.assertIn("misc/fx_zombie_electric_trap", report.fx)
        self.assertNotIn("SetVolFog", core)
        self.assertIn("waw2bo2_client_fx_0();", core)

    def test_entity_and_sound_client_listeners_run_on_the_server(self):
        # Der Riese: power-on lamp swaps, pad wire sparks with a fake sound
        # entity, client-internal notifies, an effect another client script loads
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            files = {
                "maps/testmap.gsc": "main()\n{\n\tmaps\\_zombiemode::main();\n\tclientnotify( \"pl1\" );\n}\n",
                "clientscripts/testmap_fx.csc":
                    "main()\n{\n\tlevel thread lamps( \"smodel_light\", \"lamp_on\" );\n"
                    "\tlevel thread wire( \"pw0\", \"t01\" );\n\tlevel thread lit( \"t01\" );\n"
                    "\tlevel thread hud();\n\tlevel thread ptr();\n\tlevel thread lit2();\n}\n"
                    "lamps( name, model )\n{\n\tlevel waittill( \"pl1\" );\n\tplayers = getlocalplayers();\n"
                    "\tfor ( p = 0; p < players.size; p++ )\n\t{\n\t\tlamps = GetEntArray( p, name, \"targetname\" );\n"
                    "\t\tfor ( i = 0; i < lamps.size; i++ )\n\t\t\tlamps[i] SetModel( model );\n\t}\n}\n"
                    "wire( msg, done )\n{\n\tlevel waittill( msg );\n\tmover = Spawn( 0, (1,2,3), \"script_model\" );\n"
                    "\tmover SetModel( \"tag_origin\" );\n\tPlayFxOnTag( 0, level._effect[\"spark\"], mover, \"tag_origin\" );\n"
                    "\tsnd = spawnfakeent( 0 );\n\tsetfakeentorg( 0, snd, mover.origin );\n"
                    "\tplayloopsound( 0, snd, \"spark_loop\" );\n\tplaysound( 0, \"spark_hit\", mover.origin );\n"
                    "\tlevel endon( \"stop\" );\n\tmover MoveTo( (4,5,6), 0.1 );\n\trealwait( 0.1 );\n"
                    "\tdeletefakeent( 0, snd );\n\tlevel notify( done );\n}\n"
                    "lit( msg )\n{\n\tlevel waittill( msg );\n\tprintln( glow() );\n}\n"
                    "lit2()\n{\n\tlevel waittill( \"pl1\" );\n\tprintln( glow() );\n}\n"
                    "glow()\n{\n\treturn \"lit\";\n}\n"
                    "hud()\n{\n\tlevel waittill( \"pl1\" );\n\tSetClientHudThing( 0 );\n}\n"
                    "ptr()\n{\n\tlevel waittill( \"tae\" );\n\tself thread [[ level.funcs[0] ]]( 0 );\n}\n",
                "clientscripts/testmap.csc":
                    "main()\n{\n\tlevel._effect[\"spark\"] = loadfx( \"maps/zombie/fx_spark\" );\n}\n",
            }
            for name, text in files.items():
                (root / "src" / name).parent.mkdir(parents=True, exist_ok=True)
                (root / "src" / name).write_text(text, encoding="utf-8")
            api = _api()
            api.builtins.update({"clientnotify": (1, 1)})
            report = gscport.port_map(gscport.Sources([root / "src"], [], None), api, "testmap", root / "out")
            core = (root / "out/maps/mp/waw/_waw2bo2_core.gsc").read_text(encoding="utf-8")
        compat = gscport.COMPAT
        self.assertIn('GetEntArray( name, "targetname" )', core)
        self.assertIn("lamps[i] SetModel( model );", core)
        self.assertIn(f'{compat}::waw_client_spawn( (1,2,3), "script_model" )', core)
        self.assertIn('PlayFxOnTag( level.waw_effect["spark"], mover, "tag_origin" )', core)
        self.assertIn(f"snd = {compat}::waw_client_spawnfakeent( );", core)
        self.assertIn(f"{compat}::waw_client_setfakeentorg( snd, mover.origin )", core)
        self.assertIn(f'{compat}::waw_client_playloopsound( snd, "spark_loop" )', core)
        self.assertIn(f'{compat}::waw_playsoundatposition( "spark_hit", mover.origin )', core)
        self.assertIn("wait( 0.1 );", core)
        # one client namespace: the internal t01 notify reaches the waiting listener
        self.assertIn('level notify( "waw_client_" + done )', core)
        self.assertIn('level endon( "waw_client_" + "stop" )', core)
        self.assertIn('level waittill( "waw_client_" + msg )', core)
        self.assertIn(f'level.waw_effect["spark"] = {compat}::waw_loadfx( "maps/zombie/fx_spark" )', core)
        self.assertIn('level thread waw2bo2_testmap_fx__lamps( "smodel_light", "lamp_on" );', core)
        # a client-only call keeps its listener on the client, reported
        self.assertNotIn("SetClientHudThing", core)
        self.assertIn("CSC SetClientHudThing in client listener hud (client-only)", report.unsupported)
        # the client-only entities these listeners name (stage networks them)
        self.assertIn("smodel_light", report.client_listener_strings)
        self.assertNotIn("SetClientHudThing", report.client_listener_strings)
        # a helper two listeners share is defined once (T6 rejects a duplicate)
        self.assertEqual(core.count("\nwaw2bo2_testmap_fx__glow()"), 1)
        self.assertIn("level thread waw2bo2_testmap_fx__lit2();", core)
        # a function pointer's target is unknown: that listener stays client-side too
        self.assertNotIn("waw2bo2_testmap_fx__ptr", core)
        self.assertIn("CSC function pointer call in client listener ptr (client-only)", report.unsupported)

    def test_utility_clientnotify_reaches_the_compat_wrapper(self):
        # Shi No Numa's trap script includes maps\_utility, which defines
        # clientNotify; BO2's maps\mp\_utility::clientnotify never raised the
        # "waw_client_" notify, so the trap's server-side arcs never played.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            files = {
                "maps/testmap.gsc": "#include maps\\_utility;\nmain()\n{\n\tmaps\\_zombiemode::main();\n"
                                    "\tclientNotify( self.target );\n\tlevel clientNotify( \"TGH\" );\n}\n",
                "maps/_utility.gsc": "clientNotify(event)\n{\n\tif(level.clientscripts)\n\t{\n"
                                     "\t\tmaps\\_utility::setClientSysState(\"levelNotify\", event);\n\t}\n}\n",
            }
            for name, text in files.items():
                (root / "src" / name).parent.mkdir(parents=True, exist_ok=True)
                (root / "src" / name).write_text(text, encoding="utf-8")
            api = _api()
            api.builtins.update({"clientnotify": (1, 1)})
            api.scripts["maps\\mp\\_utility"] = {"clientnotify": 1}
            report = gscport.port_map(gscport.Sources([root / "src"], [], None), api, "testmap", root / "out")
            main = (root / "out/maps/mp/waw/testmap.gsc").read_text(encoding="utf-8")
        self.assertIn(f'{gscport.COMPAT}::waw_clientnotify( self.target )', main)
        self.assertIn(f'level {gscport.COMPAT}::waw_clientnotify( "TGH" )', main)
        self.assertEqual(report.rewrites[r"maps\_utility::clientnotify -> compat wrapper"], 2)

    def test_framework_zombie_var_defaults_reach_bo2_init(self):
        # WaW's _zombiemode::init_levelvars: zombie_spawn_delay 3 (BO2 2.0)
        _, _, report = self.port({
            "maps/testmap.gsc": "main()\n{\n\tmaps\\_zombiemode::main();\n}\n",
            "maps/_zombiemode.gsc": "main() {}\ninit_levelvars()\n{\n"
                                    "\tset_zombie_var( \"zombie_spawn_delay\", 3 );\n"
                                    "\tset_zombie_var( \"below_world_check\", -1000 );\n"
                                    "\tset_zombie_var( \"zombie_use_failsafe\", true );\n"
                                    "\tset_zombie_var( \"computed\", level.x );\n}\n",
        })
        self.assertEqual(report.rewrites["WaW framework zombie_vars defaults"], 3)

    def test_box_chooser_keys_skip_bo2_only_weapons(self):
        tokens = gscport.gsc.tokenize("f()\n{\n\tkeys = GetArrayKeys( level.zombie_weapons );\n"
                                      "\tp = GetArrayKeys( level.zombie_powerups );\n}\n")
        self.assertEqual(gscport.lower_zombie_weapon_keys(tokens), 1)
        text = gscport.gsc.emit(tokens)
        self.assertIn(f"keys = {gscport.COMPAT}::waw_zombie_weapon_keys();", text)
        self.assertIn("GetArrayKeys( level.zombie_powerups )", text)
        registration = gscport.weapons_source({})
        for weapon in gscport.BO2_FRAMEWORK_WEAPONS:
            self.assertIn(f'waw_native_only_weapon( "{weapon}" );', registration)

    def test_box_weapon_keys_survive_map_and_framework_translation(self):
        # Exercise reference resolution, not just the token rewrite: a stub
        # returns undefined and leaves the real box's keys.size loop spinning.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'maps').mkdir()
            body = ('choose() { keys = GetArrayKeys( level.zombie_weapons ); '
                    'for (i = 0; i < keys.size; i++) { last = keys[i]; } return keys; }')
            (root / 'maps/custom.gsc').write_text(body)
            (root / 'maps/_zombiemode_weapons.gsc').write_text(body)
            tr = gscport.Translator(gscport.Sources([root], [], None), _api(), gscport.PortReport())
            mapped = tr.translate(r'maps\custom')
            tr.extract_entry(r'maps\_zombiemode_weapons', 'choose')
            extracted = tr.translate_extracted(*tr.core_queue.pop(0))
        for output in (mapped, extracted):
            parsed = gscport.gsc.parse(output, 'translated box chooser')
            calls = gscport.gsc.references(parsed.tokens)
            self.assertTrue(any(ref.qualifier == gscport.COMPAT and ref.name == 'waw_zombie_weapon_keys'
                                for ref in calls), output)
            self.assertNotIn('stub_', output)
            self.assertNotIn('GetArrayKeys', output)
        self.assertEqual(tr.report.unsupported, {})
        self.assertEqual(tr.stubs, {})

    def test_named_models_precache_in_the_first_frame(self):
        # T6: "precacheModel must be called before any wait statements"; WaW perk
        # scripts precache the machine's on-model only when the power comes on
        text = gscport.precache_source(["p6_zm_vending_electric_cherry_on", "a", "a"])
        self.assertEqual(text.count("waw_precachemodel"), 2)
        self.assertIn(f'{gscport.COMPAT}::waw_precachemodel( "p6_zm_vending_electric_cherry_on" );', text)
        self.assertIn("waw_precachemodel", gscport.API["compat_wrappers"] and
                      (gscport.COMPAT_DIR / "_waw2bo2_compat.gsc").read_text(encoding="utf-8"))
        self.assertIn("precachemodel", gscport.API["compat_wrappers"])

    def test_effects_of_extracted_template_functions_are_converted(self):
        # the DLC3 template's dlc3_fx loads the mystery box's sky marker
        _, _, report = self.port({
            "maps/testmap.gsc": "main()\n{\n\tmaps\\dlc3_code::dlc3_fx();\n\tmaps\\_zombiemode::main();\n}\n",
            "maps/dlc3_code.gsc": "dlc3_fx()\n{\n\tlevel._effect[\"lght_marker\"] = "
                                  "loadfx( \"maps/zombie/fx_zombie_factory_marker\" );\n}\n",
        })
        self.assertIn("maps/zombie/fx_zombie_factory_marker", report.fx)

    def test_no_override_main_no_hooks(self):
        main, _, _ = self.port({"maps/testmap.gsc": "main()\n{\n\tmaps\\_zombiemode::main();\n}\n"})
        self.assertNotIn("inits of the map's own scripts", main)


STOCK_ZM = ("main()\n{\n\tlevel.custom_intermission = ::player_intermission;\n\tlevel thread end_game();\n}\n"
            "end_game()\n{\n\tlevel waittill ( \"end_game\" );\n\tintermission();\n"
            "\twait( level.zombie_vars[\"zombie_intermission_time\"] );\n\tlevel notify( \"stop_intermission\" );\n}\n"
            "player_intermission()\n{\n\torg = Spawn( \"script_origin\", self.origin );\n"
            "\tself LinkTo( org, \"\", ( 0, 0, -60 ), ( 0, 0, 0 ) );\n}\n")


class CustomIntermissionTests(unittest.TestCase):
    def port(self, override: str) -> tuple[str, str, gscport.PortReport]:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            files = {root / "src/maps/testmap.gsc": "main()\n{\n\tmaps\\_zombiemode::main();\n}\n",
                     root / "src/maps/_zombiemode.gsc": override,
                     root / "stock/maps/_zombiemode.gsc": STOCK_ZM}
            for path, text in files.items():
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text, encoding="utf-8")
            sources = gscport.Sources([root / "src"], [], root / "stock")
            report = gscport.port_map(sources, _api(), "testmap", root / "out")
            waw = root / "out/maps/mp/waw"
            return ((waw / "testmap.gsc").read_text(encoding="utf-8"),
                    (waw / "_waw2bo2_core.gsc").read_text(encoding="utf-8"), report)

    def test_map_end_game_camera_replaces_bo2_intermission(self):
        # Nuketown: the view flies down with the nuke (rocket linked to the camera mover)
        edited = STOCK_ZM.replace("\tself LinkTo(", "\trocket = getEnt( \"rocket\", \"targetname\" );\n"
                                  "\trocket linkTo( org );\n\tself LinkTo(")
        edited = edited.replace("\tlevel waittill ( \"end_game\" );",
                                "\trocket = getEnt( \"rocket\", \"targetname\" );\n\trocket hide();\n"
                                "\tlevel waittill ( \"end_game\" );")
        # the cutscene, not BO2's fixed timer, decides when the game exits
        edited = edited.replace("\twait( level.zombie_vars[\"zombie_intermission_time\"] );",
                                "\tlevel waittill( \"end_it_pls\" );\n\twait 2;")
        main, core, report = self.port(edited)
        post = main[main.index("waw_main_post()"):]
        self.assertIn(f"level.custom_intermission = {gscport.CORE}::zombiemode__player_intermission;", post)
        self.assertIn(f"level thread {gscport.CORE}::zombiemode__end_game_prelude();", post)
        self.assertIn(f"level thread {gscport.CORE}::zombiemode__end_game_intermission_wait();", post)
        wait = core[core.index("zombiemode__end_game_intermission_wait()"):]
        wait = wait[:wait.index("\n}\n")]
        order = ['level waittill( "end_game" );', 'level.zombie_vars["zombie_intermission_time"] = 30;',
                 'level waittill( "intermission" );', 'level waittill( "end_it_pls" );', "wait 2;",
                 'level notify( "stop_intermission" );', "maps\\mp\\zombies\\_zm::player_exit_level", "wait 1.5;",
                 "cameraactivate( 0 );", "exitlevel( 0 );"]
        self.assertEqual([wait.index(o) for o in order], sorted(wait.index(o) for o in order))
        self.assertLess(post.index("custom_intermission"), post.index("initial_players_connected"))
        prelude = core[core.index("zombiemode__end_game_prelude()"):]
        self.assertIn("rocket hide();", prelude)
        self.assertNotIn("waittill", prelude[:prelude.index("}")])
        fn = core[core.index("zombiemode__player_intermission()"):]
        # linkto runs through compat (linked trigger volumes keep WaW contacts)
        self.assertIn("rocket maps\mp\waw\_waw2bo2_compat::waw_linkto( org );", fn)
        # BO2 views intermissions through the script camera on a networked model
        self.assertIn("self.waw_intermission_cam linkto( org );", fn)
        self.assertIn("self camerasetposition( self.waw_intermission_cam );", fn)
        self.assertIn("self cameraactivate( 1 );", fn)
        self.assertEqual(report.rewrites["intermission player link -> T6 script camera"], 1)

    def test_stock_intermission_keeps_bo2s(self):
        main, core, _ = self.port(STOCK_ZM + "other() {}\n")
        self.assertNotIn("custom_intermission", main)
        self.assertNotIn("end_game_prelude", main)
        self.assertNotIn("intermission_wait", main)
        self.assertNotIn("zombiemode__player_intermission", core)


if __name__ == "__main__":
    unittest.main()
