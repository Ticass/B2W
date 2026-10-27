import tempfile
import unittest
from pathlib import Path
from collections import Counter
from waw2bo2 import entities, gsc, gscport, zombiebehavior
from waw2bo2.t6api import T6Api



# BO2 _zm_spawner's power-up drop contract (zombiebehavior.waw_drop_rules).
BO2_DEATH_POINTS = (
    'zombie_death_points( origin, mod, hit_location, attacker, zombie, team )\n{\n'
    + zombiebehavior._DROP_GATE + '\n    {\n' + zombiebehavior._DROP_CALL + '\n    }\n\n'
    + zombiebehavior._KILL_VOX + '\n    award();\n}\n')

class ZombieBehaviorTests(unittest.TestCase):
    def make_movement_callback(self, root):
        path = root / 'bo2/raw/maps/mp/animscripts/zm_run.gsc'
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('setanimstatefromspeed(){ self [[ self.setanimstatefromspeed ]]("move", undefined); }')

    def test_round_speed_inputs_follow_source_helpers_before_each_round(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            files = {
                'maps/_zombiemode.gsc': '#include maps\\rules;\n'
                    'round_spawning(){ players=get_players(); for(i=0;i<players.size;i++){ '
                    'players[i].timer=0; } count=prepare(level.round_number); '
                    'while(1){ spawn_zombie(); } }',
                'maps/rules.gsc': 'prepare(round){ switch(round){ case 1: level.low=80; '
                    'level.high=100; break; default: level.low=30; level.high=60; } return round*10; }\n'
                    'choose(){ return randomintrange(level.low,level.high); }',
                'maps/_zombiemode_spawner.gsc': '#include maps\\rules;\n'
                    'set_run_speed(){ value=choose(); if(value>70) self.zombie_move_speed="sprint"; '
                    'else self.zombie_move_speed="run"; }',
            }
            for name, text in files.items():
                path = root / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_text(text)
            sources = gscport.Sources([root], [], None)
            state = zombiebehavior.source_speed_state(sources, gscport.API['zombie_runtime'])
            self.assertIn('count=prepare(level.round_number)', state)
            self.assertNotIn('spawn_zombie', state)
            self.assertNotIn('players[i].timer', state)
            gsc.parse(state, 'state')
            runtime = zombiebehavior.runtime_source(gscport.API['zombie_runtime'], 'source::choose',
                                                    speed_state_function='source_state')
            self.assertLess(runtime.index('source_state::prepare_round'), runtime.index('self source::choose'))
            self.assertIn('level.waw_speed_round != level.round_number', runtime)

    def test_stock_round_speed_uses_source_initial_and_previous_round_values(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for name, text in {
                'maps/_zombiemode.gsc': 'round_think(){ while(1){ '
                    'level.zombie_move_speed=level.round_number*8; level.round_number++; } }',
                'maps/_zombiemode_spawner.gsc': 'init(){level.zombie_move_speed=1;} '
                    'set_run_speed(){self.zombie_move_speed=randomintrange(level.zombie_move_speed,35);}',
            }.items():
                p=root/name; p.parent.mkdir(parents=True,exist_ok=True); p.write_text(text)
            state = zombiebehavior.source_speed_state(gscport.Sources([root], [], None), gscport.API['zombie_runtime'])
            tokens = gsc.tokenize(state)
            gscport.rename_level_fields(tokens)
            text = gsc.emit(tokens)
            self.assertIn('level.waw_zombie_move_speed=1', text)
            self.assertIn('level.waw_zombie_move_speed=source_round*8', text)
            self.assertIn('source_round = level.round_number - 1', text)
            self.assertIn('if (level.round_number > 1)', text)
            self.assertNotIn('level.round_number++', text)

    def test_conditional_round_speed_writer_is_not_applied_unconditionally(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for name, text in {
                'maps/_zombiemode.gsc': 'round_think(){while(1){ '
                    'if(level.round_number>4) level.zombie_move_speed=100; level.round_number++;}}',
                'maps/_zombiemode_spawner.gsc': 'set_run_speed(){self.zombie_move_speed=level.zombie_move_speed;}',
            }.items():
                p=root/name; p.parent.mkdir(parents=True,exist_ok=True); p.write_text(text)
            with self.assertRaisesRegex(ValueError, 'conditional source speed state'):
                zombiebehavior.source_speed_state(gscport.Sources([root], [], None), gscport.API['zombie_runtime'])
            # Berlin Bank: the map's own loop runs and updates it; only the
            # selector script's initial value is kept
            (root / 'maps/_zombiemode_spawner.gsc').write_text(
                'init(){level.zombie_move_speed=1;}set_run_speed(){self.zombie_move_speed=level.zombie_move_speed;}')
            state = zombiebehavior.source_speed_state(gscport.Sources([root], [], None),
                                                      gscport.API['zombie_runtime'], rounds=False)
            self.assertIn('level.zombie_move_speed=1;', state.replace(' ', ''))
            self.assertNotIn('100', state)

    def test_source_actor_speed_writes_cannot_be_clobbered_by_native_field(self):
        tokens = gsc.tokenize('f(){self.zombie_move_speed="sprint"; '
                              'zombies[i].zombie_move_speed="run"; level.zombie_move_speed=100;}')
        gscport.rename_level_fields(tokens)
        self.assertIn('self.waw_zombie_move_speed="sprint"', gsc.emit(tokens))
        self.assertIn('zombies[i].waw_zombie_move_speed="run"', gsc.emit(tokens))
        runtime = zombiebehavior.runtime_source(gscport.API['zombie_runtime'], 'source::choose')
        self.assertIn('self.zombie_move_speed != self.waw_zombie_move_speed', runtime)
        self.assertIn('self.setanimstatefromspeed = ::source_animstate', runtime)
        self.assertIn('self.waw_previous_speed_callback', runtime)
        self.assertIn('native_request = self.waw_zombie_move_speed', runtime)

    def make_sources(self, root, text):
        path = root / 'maps/_zombiemode.gsc'
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        return gscport.Sources([root], [], None)

    def test_authored_movement_variants_keep_duplicate_weights(self):
        with tempfile.TemporaryDirectory() as temp:
            sources = self.make_sources(Path(temp), '#using_animtree("human");\n'
                'init_standard_zombie_anims(){ level.scr_anim["zombie"]["sprint1"]=%fast; '
                'level.scr_anim["zombie"]["sprint2"]=%fast; '
                'level.scr_anim["zombie"]["run1"]=%jog; }')
            tree, sets = zombiebehavior.movement_sets(sources, gscport.API['zombie_runtime'])
            self.assertEqual(tree, 'human')
            self.assertEqual(sets, {'sprint': ['fast', 'fast'], 'run': ['jog']})

    def _write(self, root, files):
        for name, text in files.items():
            path = root / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_text(text)
        return gscport.Sources([root], [], None)

    def test_movement_sets_keep_only_variants_the_source_picks(self):
        # Verruckt defines sprint4 (ai_zombie_sprint_v5) but picks randomintrange( 1, 4 )
        with tempfile.TemporaryDirectory() as temp:
            sources = self._write(Path(temp), {
                'maps/_zombiemode.gsc': '#using_animtree("human");\ninit_standard_zombie_anims(){ '
                    'level.scr_anim["zombie"]["sprint1"]=%a; level.scr_anim["zombie"]["sprint2"]=%b; '
                    'level.scr_anim["zombie"]["sprint3"]=%c; level.scr_anim["zombie"]["sprint4"]=%d; }',
                'maps/_zombiemode_spawner.gsc': 'f(){ switch(self.zombie_move_speed){ case "sprint": '
                    'var = randomintrange(1, 4); self set_run_anim( "sprint" + var ); break; } }'})
            _, sets = zombiebehavior.movement_sets(sources, gscport.API['zombie_runtime'])
            self.assertEqual(sets, {'sprint': ['a', 'b', 'c']})

    def test_last_zombie_rule_follows_the_source_round_loop(self):
        api = gscport.API['zombie_runtime']
        with tempfile.TemporaryDirectory() as temp:
            sumpf = self._write(Path(temp) / 'sumpf', {'maps/_zombiemode.gsc':
                'round_spawning(){ while(count < max){ spawn(); } if( level.round_number > 3 ) { '
                'zombies = getaiarray("axis"); while( zombies.size > 0 ) { if( zombies.size == 1 && '
                'zombies[0].has_legs == true ) { var = randomintrange(1, 4); '
                'zombies[0] set_run_anim( "sprint" + var ); } wait(0.5); } } }'})
            rule = zombiebehavior.source_last_zombie(sumpf, api)
            self.assertEqual(rule, {'after_round': 3, 'speed': 'sprint'})
            runtime = zombiebehavior.runtime_source(api, None, last_zombie=rule)
            self.assertIn('level.zombie_speed_up = ::source_last_zombie;', runtime)
            self.assertIn('if ( level.round_number <= 3 || level.zombie_total > 0 )', runtime)
            self.assertIn('self.waw_zombie_move_speed = "sprint";', runtime)
            asylum = self._write(Path(temp) / 'asylum', {'maps/_zombiemode.gsc':
                'round_spawning(){ while(count < max){ spawn(); } }'})
            self.assertIsNone(zombiebehavior.source_last_zombie(asylum, api))
            runtime = zombiebehavior.runtime_source(api, None)
            # BO2's own sprint is disabled too: the map keeps the last zombie's speed
            self.assertIn('level.zombie_speed_up = ::source_last_zombie;', runtime)
            self.assertNotIn('self.waw_zombie_move_speed = "sprint";', runtime)

    def test_movement_initializer_role_is_detected_from_table_assignments(self):
        with tempfile.TemporaryDirectory() as temp:
            sources = self.make_sources(Path(temp), '#using_animtree("human");\n'
                'custom_moves(){level.scr_anim["zombie"]["sprint1"]=%source_sprint; '
                'level.scr_anim["zombie"]["run1"]=%source_run;}')
            tree, sets = zombiebehavior.movement_sets(sources, gscport.API['zombie_runtime'])
            self.assertEqual(tree, 'human')
            self.assertEqual(sets, {'sprint': ['source_sprint'], 'run': ['source_run']})

    def test_conditional_movement_table_is_not_guessed(self):
        with tempfile.TemporaryDirectory() as temp:
            sources = self.make_sources(Path(temp), 'init_standard_zombie_anims(){ '
                'if(level.fast) { level.scr_anim["zombie"]["sprint1"]=%fast; } }')
            with self.assertRaisesRegex(ValueError, 'conditional movement table'):
                zombiebehavior.movement_sets(sources, gscport.API['zombie_runtime'])

    def test_source_map_condition_selects_authored_movement_overrides(self):
        with tempfile.TemporaryDirectory() as temp:
            sources = self.make_sources(Path(temp), 'init_standard_zombie_anims(){ '
                'level.scr_anim["zombie"]["run1"]=%base; '
                'if(level.script == "source_map") { level.scr_anim["zombie"]["run1"]=%special; '
                'level.scr_anim["zombie"]["run2"]=%special; } '
                'else { level.scr_anim["zombie"]["run1"]=%other; } }')
            _, sets = zombiebehavior.movement_sets(sources, gscport.API['zombie_runtime'], 'source_map')
            self.assertEqual(sets, {'run': ['special', 'special']})
            _, sets = zombiebehavior.movement_sets(sources, gscport.API['zombie_runtime'], 'another_map')
            self.assertEqual(sets, {'run': ['other']})
            with self.assertRaisesRegex(ValueError, 'conditional movement table'):
                zombiebehavior.movement_sets(sources, gscport.API['zombie_runtime'])

    def test_source_clips_bind_owned_server_and_client_states(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            sources = self.make_sources(root, '#using_animtree("human");\n'
                'init_standard_zombie_anims(){ level.scr_anim["zombie"]["sprint1"]=%fast; }')
            raw = root / 'bo2/raw'
            for path, text in {
                'animtrees/zm_nuked_basic.atr': 'body\n{\nstock\n}\n',
                'animstatedefs/zm_nuked_basic.asd': 'zm_move_sprint : notify move_anim\n{\nstock\n}\n'
                    'zm_traverse : aliased\n{\njump stock\n}\n',
            }.items():
                p = raw / path; p.parent.mkdir(parents=True, exist_ok=True); p.write_text(text)
            out = root / 'out'
            (out / 'xanim').mkdir(parents=True)
            (out / 'xanim/waw_anim_fast').write_bytes(b'original source clip')
            for name in ('aitype/waw_actor.gsc', 'aitype/clientscripts/waw_actor.csc'):
                p = out / name; p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text('#using_animtree("zm_nuked_basic"); precache(){ use("zm_nuked_basic"); }')
            result = zombiebehavior.stage_actor_movement(sources, out, root / 'bo2', gscport.API['zombie_runtime'])
            self.assertEqual(result['clips'], ['waw_anim_fast'])
            asd = (out / ('animstatedefs/' + zombiebehavior.ACTOR_TREE + '.asd')).read_text()
            self.assertIn('waw_anim_fast', asd)
            self.assertIn('zm_traverse : aliased\n{\njump stock\n}', asd)
            self.assertIn('waw_anim_fast', (out / ('animtrees/' + zombiebehavior.ACTOR_TREE + '.atr')).read_text())
            for p in (out / 'aitype').rglob('*.?sc'):
                self.assertIn(zombiebehavior.ACTOR_TREE, p.read_text())
                self.assertIn('dummy_anim_ref = %waw_anim_fast;', p.read_text())
            self.assertNotIn('waw_anim_fast', (raw / 'animstatedefs/zm_nuked_basic.asd').read_text())

    def test_movement_clip_absent_from_every_zone_is_left_out(self):
        # Yard: run1 names a clip no zone carries; the build used to stop.
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            sources = self.make_sources(root, '#using_animtree("human");\n'
                'init_standard_zombie_anims(){ level.scr_anim["zombie"]["run1"]=%absent; '
                'level.scr_anim["zombie"]["run2"]=%present; level.scr_anim["zombie"]["walk1"]=%gone; }')
            raw = root / 'bo2/raw'
            for path, text in {
                'animtrees/zm_nuked_basic.atr': 'body\n{\nstock\n}\n',
                'animstatedefs/zm_nuked_basic.asd': 'zm_move_run : notify move_anim\n{\nstock_run\n}\n'
                    'zm_move_walk : notify move_anim\n{\nstock_walk\n}\n',
            }.items():
                p = raw / path; p.parent.mkdir(parents=True, exist_ok=True); p.write_text(text)
            out = root / 'out'
            (out / 'xanim').mkdir(parents=True)
            (out / 'xanim/waw_anim_present').write_bytes(b'original source clip')
            result = zombiebehavior.stage_actor_movement(sources, out, root / 'bo2', gscport.API['zombie_runtime'])
            self.assertEqual(result['clips'], ['waw_anim_present'])
            self.assertEqual(result['states'], {'run': ['present']})
            self.assertEqual(result['missing'], ['absent', 'gone'])
            self.assertEqual(result['native_states'], ['walk'])
            asd = (out / ('animstatedefs/' + zombiebehavior.ACTOR_TREE + '.asd')).read_text()
            self.assertIn('zm_move_run : notify move_anim\n{\n\twaw_anim_present\n}', asd)
            self.assertIn('zm_move_walk : notify move_anim\n{\nstock_walk\n}', asd)
            self.assertNotIn('absent', asd)

    def test_source_attack_tables_replace_native_melee_clips(self):
        # BO2's attack clips carry "no pain" notes: a hit never cancels the swing
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            sources = self.make_sources(root, '#using_animtree("human");\n'
                'init_standard_zombie_anims(){ level.scr_anim["zombie"]["run1"]=%run; '
                'level._zombie_melee["zombie"][0]=%attack_a; level._zombie_melee["zombie"][1]=%attack_a; '
                'level._zombie_run_melee["zombie"][0]=%run_attack; '
                'level._zombie_walk_melee["zombie"][0]=%walk_attack; '
                'level._zombie_melee_crawl["zombie"][0]=%crawl_attack; '
                'level._zombie_stumpy_melee["zombie"][0]=%absent; }')
            raw = root / 'bo2/raw'
            states = ('zm_move_run', 'zm_walk_melee', 'zm_run_melee', 'zm_walk_melee_crawl',
                      'zm_run_melee_crawl', 'zm_stumpy_melee')
            for path, text in {
                'animtrees/zm_nuked_basic.atr': 'body\n{\nstock\n}\n',
                'animstatedefs/zm_nuked_basic.asd': ''.join(f'{s} : restart notify melee_anim\n{{\nstock\n}}\n'
                                                            for s in states),
            }.items():
                p = raw / path; p.parent.mkdir(parents=True, exist_ok=True); p.write_text(text)
            out = root / 'out'
            (out / 'xanim').mkdir(parents=True)
            for clip in ('run', 'attack_a', 'run_attack', 'walk_attack', 'crawl_attack'):
                (out / 'xanim' / ('waw_anim_' + clip)).write_bytes(b'original source clip')
            result = zombiebehavior.stage_actor_movement(sources, out, root / 'bo2', gscport.API['zombie_runtime'])
            self.assertEqual(result['melee']['zm_run_melee'], ['attack_a', 'attack_a', 'run_attack'])
            self.assertEqual(result['melee']['zm_walk_melee_crawl'], ['crawl_attack'])
            self.assertEqual(result['native_melee_states'], ['zm_stumpy_melee'])
            asd = (out / ('animstatedefs/' + zombiebehavior.ACTOR_TREE + '.asd')).read_text()
            self.assertIn('zm_walk_melee : restart notify melee_anim\n{\n\twaw_anim_attack_a\n\twaw_anim_attack_a\n'
                          '\twaw_anim_walk_attack\n}', asd)
            self.assertIn('zm_stumpy_melee : restart notify melee_anim\n{\nstock\n}', asd)
            atr = (out / ('animtrees/' + zombiebehavior.ACTOR_TREE + '.atr')).read_text()
            self.assertIn('waw_anim_run_attack', atr)

    def test_spawner_sampler_preserves_source_reroll_body(self):
        with tempfile.TemporaryDirectory() as temp:
            sources = self.make_sources(Path(temp), 'round_spawning(){ unrelated(); old_spawn=undefined; '
                'while(1){ spawn_point=level.enemy_spawns[randomint(level.enemy_spawns.size)]; '
                'if(isdefined(old_spawn) && spawn_point==old_spawn){ '
                'spawn_point=level.enemy_spawns[randomint(level.enemy_spawns.size)]; } '
                'old_spawn=spawn_point; spawn_zombie(spawn_point); } }')
            text = zombiebehavior.source_selection(sources, gscport.API['zombie_runtime'])
            gsc.parse(text, 'selection')
            self.assertEqual(text.count('randomint(candidates.size)'), 2)
            self.assertIn('spawn_point==old_spawn', text)
            self.assertNotIn('unrelated', text)
            self.assertNotIn('spawn_zombie', text)

    def test_spawner_sampler_without_history_keeps_single_source_draw(self):
        with tempfile.TemporaryDirectory() as temp:
            sources = self.make_sources(Path(temp), 'round_spawning(){ while(1){ '
                'spawn_point=level.enemy_spawns[randomint(level.enemy_spawns.size)]; '
                'spawn_zombie(spawn_point); } }')
            text = zombiebehavior.source_selection(sources, gscport.API['zombie_runtime'])
            self.assertEqual(text.count('randomint(candidates.size)'), 1)
            self.assertNotIn('spawn_zombie', text)
            self.assertNotIn('old_spawn=', text)
            gsc.parse(text, 'single source draw')

    def test_unknown_sampler_without_history_is_not_assumed_random(self):
        with tempfile.TemporaryDirectory() as temp:
            sources = self.make_sources(Path(temp), 'round_spawning(){ '
                'spawn_point=level.enemy_spawns[custom_index()]; spawn_zombie(spawn_point); }')
            self.assertIsNone(zombiebehavior.source_selection(sources, gscport.API['zombie_runtime']))

    def test_source_zone_manager_keeps_active_array_initializer(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for name, text in {
                'maps/test.gsc': 'main(){ maps\\_zombiemode::main(); level.enemy_spawns=update(level.enemy_spawns); } '
                                 'update(spawns){return spawns;}',
                'maps/_zombiemode.gsc': 'main(){level.enemy_spawns=getentarray("initial","targetname");} '
                    'round_spawning(){while(1){spawn_point=level.enemy_spawns[randomint(level.enemy_spawns.size)]; spawn_zombie(spawn_point);}}',
                'maps/_loadout.gsc': 'init_loadout(){} give_model(){}',
            }.items():
                p = root / name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text(text)
            gscport.port_map(gscport.Sources([root], [], None), T6Api({'getentarray': (2, 2)}, {}), 'test', root / 'out')
            core = (root / 'out/maps/mp/waw/_waw2bo2_core.gsc').read_text()
            runtime = (root / 'out/maps/mp/waw/_waw2bo2_zombie_behavior.gsc').read_text()
            self.assertIn('level.enemy_spawns=getentarray("initial","targetname")', core)
            self.assertIn('level.waw_source_spawn_membership = 1;', runtime)
            self.assertIn('return spawn_point.waw_source_spawner;', runtime)

    def test_source_team_respawn_anchors_do_not_enter_native_zone_list(self):
        anchor = {'classname': 'script_struct', 'targetname': 'player_respawn_point',
                  'target': 'player_slots', 'origin': '1 2 3'}
        native = dict(anchor, origin='4 5 6', script_noteworthy='yard')
        out, _, _ = entities.convert_entities([anchor, native], 'test', {})
        points = [e for e in out if e.get('targetname') == 'player_respawn_point']
        self.assertEqual(points[0]['target'], 'player_slots')
        self.assertEqual(points[0]['origin'], '1 2 3')
        self.assertEqual(points[0]['script_string'], 'waw_source_respawn')
        self.assertNotIn('script_string', points[1])
        runtime = zombiebehavior.runtime_source(gscport.API['zombie_runtime'], None,
                                               respawn_function='source::team_respawn')
        self.assertIn('level.check_for_valid_spawn_near_team_callback = ::source_respawn', runtime)
        parsed = gsc.parse(runtime, 'source respawn adapter')
        fn = parsed.functions['source_respawn']
        body = gsc.emit(parsed.tokens[fn.body_open:fn.body_close])
        self.assertIn('source::team_respawn( player )', body)
        self.assertIn('point.origin = origin', body)

    def test_native_spawner_picks_use_source_sampler(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.make_movement_callback(root)
            native = root / 'bo2/raw/maps/mp/zombies/_zm_spawner.gsc'
            native.parent.mkdir(parents=True)
            native.write_text('zombie_spawn_init(){ self init_zombie_run_cycle(); self thread zombie_think(); } '
                              'do_zombie_spawn(){spot=random(spots); if(isdefined(spot.target)) self.target=spot.target;} zombie_think(){}\n' + BO2_DEATH_POINTS)
            out = root / 'out/maps/mp/waw'
            out.mkdir(parents=True)
            (out / '_waw2bo2_zombie_behavior.gsc').write_text('init(){}')
            (out / '_waw2bo2_zm.gsc').write_text('round_spawning(){ '
                'while(level.zombie_spawn_locations.size <= 0) wait 0.1; '
                'spawn_point=level.zombie_spawn_locations[randomint(level.zombie_spawn_locations.size)]; '
                'old_spawn=spawn_point; '
                'spawner=random(level.zombie_spawners); other=random(other_array); '
                'ai=spawn_zombie(spawner,spawner.targetname,spawn_point); }\n'
                'init(){ maps\\mp\\zombies\\_zm_spawner::init(); }\n'
                'zombie_speed_up(){ self set_zombie_run_cycle("walk"); }')
            zombiebehavior.stage_native_support(root / 'out', root / 'bo2', gscport.API['zombie_runtime'])
            text = (out / '_waw2bo2_zm.gsc').read_text()
            self.assertIn('::choose_spawner(level.zombie_spawners, spawn_point )', text)
            self.assertIn('::spawn_zombie(spawner,spawner.targetname,spawn_point)', text)
            self.assertIn('other=random(other_array)', text)
            self.assertIn('maps\\mp\\waw\\_waw2bo2_spawner::init()', text)
            self.assertIn('::refresh_speed("walk")', text)
            self.assertIn('::spawn_locations_available()', text)
            self.assertIn('::choose_spawn_location()', text)
            self.assertLess(text.index('::choose_spawn_location()'), text.index('::choose_spawner('))
            gsc.parse(text, 'active source membership round loop')
            # the chosen location's target replaces the actor's, even when it has none
            owned = (root / 'out/maps/mp/waw/_waw2bo2_spawner.gsc').read_text()
            self.assertIn('self.target=spot.target;', owned)
            self.assertNotIn('isdefined(spot.target)', owned)

    def test_source_tesla_deaths_skip_native_duplicate_points_and_drops(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.make_movement_callback(root)
            native = root / 'bo2/raw/maps/mp/zombies/_zm_spawner.gsc'
            native.parent.mkdir(parents=True)
            native.write_text('zombie_spawn_init(){ self init_zombie_run_cycle(); self thread zombie_think(); } '
                              'do_zombie_spawn(){spot=random(spots); if(isdefined(spot.target)) self.target=spot.target;} zombie_think(){}\n' + BO2_DEATH_POINTS)
            out = root / 'out/maps/mp/waw'
            out.mkdir(parents=True)
            (out / '_waw2bo2_zombie_behavior.gsc').write_text('init(){}')
            (out / '_waw2bo2_core.gsc').write_text('waw_tesla_init(){}')
            zombiebehavior.stage_native_support(root / 'out', root / 'bo2', gscport.API['zombie_runtime'])
            text = (out / '_waw2bo2_spawner.gsc').read_text()
            script = gsc.parse(text, 'native Tesla death adapter')
            fn = script.functions['zombie_death_points']
            body = gsc.emit(script.tokens[fn.body_open:fn.body_close])
            self.assertLess(body.index('zombie.tesla_death'), body.index('award()'))
            self.assertIn('return;', body)
            # WaW drop rule: roll before the attacker check, WaW playable_area
            self.assertLess(body.index('waw_zombie_can_drop_powerups( zombie )'), body.index('isplayer( attacker )'))
            self.assertIn('_waw2bo2_compat::waw_powerup_drop( origin )', body)
            self.assertNotIn('_zm_powerups::powerup_drop', body)
            self.assertIn('zombie.marked_for_death', text)

    def test_chosen_active_spawn_location_is_bound_before_deferred_native_callbacks(self):
        runtime = zombiebehavior.runtime_source(gscport.API['zombie_runtime'], None)
        parsed = gsc.parse(runtime, 'runtime')
        spawn = parsed.functions['spawn_zombie']
        body = gsc.emit(parsed.tokens[spawn.body_open:spawn.body_close])
        self.assertLess(body.index('_zm_utility::spawn_zombie'), body.index('ai.spawn_point_override = spawn_point'))
        self.assertLess(body.index('ai.spawn_point_override = spawn_point'), body.index('return ai'))
        self.assertIn('ai forceteleport( spawn_point.origin )', body)
        bind = parsed.functions['bind_spawn_location']
        text = gsc.emit(parsed.tokens[bind.body_open:bind.body_close])
        self.assertLess(text.index('isdefined( self.spawn_point_override )'), text.index('spots ='))
        # a riser the source spawns itself (Shi No Numa's door-opening
        # <hut>_initial_zombies) rises at WaW's random( level.zombie_rise_spawners )
        self.assertLess(text.index('isdefined( self.spawn_point_override )'), text.index('self.script_string == "riser"'))
        self.assertIn('self.spawn_point_override = rise_point( level.zombie_rise_spawners[randomint( '
                      'level.zombie_rise_spawners.size )], self )', text)
        point = gsc.emit(parsed.tokens[parsed.functions['rise_point'].body_open:parsed.functions['rise_point'].body_close])
        self.assertIn('point.script_noteworthy = "riser_location"', point)
        self.assertIn('point.script_string = "find_flesh"', point)
        self.assertIn('matching[matching.size] = candidate', runtime)
        self.assertIn('if ( matching.size )', runtime)
        self.assertIn('level.round_spawn_func == maps\\mp\\zombies\\_zm::round_spawning', runtime)
        self.assertIn('level.round_spawn_func = maps\\mp\\waw\\_waw2bo2_zm::round_spawning', runtime)

    def test_source_speed_body_and_dependencies_are_translated(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            files = {
                'maps/test.gsc': 'main(){ maps\\_zombiemode::main(); }',
                'maps/_zombiemode.gsc': 'main(){}',
                'maps/_zombiemode_spawner.gsc':
                    'set_run_speed(){ if(level.round_number >= 1) self.zombie_move_speed = choose(); } '
                    'choose(){ return "sprint"; }',
                'maps/_loadout.gsc': 'init_loadout(){} give_model(){}',
            }
            for name, text in files.items():
                file = root / name
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_text(text)
            report = gscport.port_map(gscport.Sources([root], [], None), T6Api({}, {}), 'test', root / 'out')
            core = (root / 'out/maps/mp/waw/_waw2bo2_core.gsc').read_text()
            self.assertIn('if(level.round_number >= 1)', core)
            self.assertIn('return "sprint";', core)
            self.assertIn('maps\\_zombiemode_spawner::choose', report.extracted)
            runtime = (root / 'out/maps/mp/waw/_waw2bo2_zombie_behavior.gsc').read_text()
            self.assertIn('::zombiemode_spawner__set_run_speed()', runtime)
            self.assertIn('native_request = self.waw_zombie_move_speed', runtime)
            self.assertIn('self.waw_zombie_move_speed = zombiemode_spawner__choose()', core)
            main = (root / 'out/maps/mp/waw/test.gsc').read_text()
            self.assertLess(main.index('_waw2bo2_zombie_behavior::init()'), main.index('initial_players_connected'))
            self.assertFalse(report.errors)

    def test_owned_callback_adapts_location_and_speed_before_think(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.make_movement_callback(root)
            native = root / 'bo2/raw/maps/mp/zombies/_zm_spawner.gsc'
            native.parent.mkdir(parents=True)
            native.write_text('#include maps\\mp\\zombies\\_zm_spawner;\n'
                'zombie_spawn_init(animname_set){ self init_zombie_run_cycle(); self thread zombie_think(); }\n'
                'do_zombie_spawn(){ spot=random(spots); self.spawn_point = spot; if ( isdefined( spot.target ) ) self.target = spot.target; }\n'
                'zombie_think(){ wait 1; }\n' + BO2_DEATH_POINTS)
            runtime = root / 'out/maps/mp/waw/_waw2bo2_zombie_behavior.gsc'
            runtime.parent.mkdir(parents=True)
            runtime.write_text(zombiebehavior.runtime_source(gscport.API['zombie_runtime'], None))
            original = native.read_text()
            zombiebehavior.stage_native_support(root / 'out', root / 'bo2', gscport.API['zombie_runtime'])
            text = (runtime.parent / '_waw2bo2_spawner.gsc').read_text()
            gsc.parse(text, 'owned')
            self.assertLess(text.index('::bind_spawn_location()'), text.index('::select_speed()'))
            self.assertLess(text.index('::select_speed()'), text.index('self thread zombie_think()'))
            self.assertLess(text.index('spot = self.spawn_point_override'), text.index('self.spawn_point = spot'))
            self.assertIn('#include maps\\mp\\waw\\_waw2bo2_spawner;', text)
            self.assertEqual(native.read_text(), original)
            self.assertIn('level.zombie_spawn_locations', runtime.read_text())
            self.assertIn('self.script_string == "riser"', runtime.read_text())

    def test_native_contract_change_fails_explicitly(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            runtime = root / 'out/maps/mp/waw/_waw2bo2_zombie_behavior.gsc'
            runtime.parent.mkdir(parents=True)
            runtime.write_text('init(){}')
            native = root / 'bo2/raw/maps/mp/zombies/_zm_spawner.gsc'
            native.parent.mkdir(parents=True)
            native.write_text('zombie_spawn_init(){ changed_speed_init(); } do_zombie_spawn(){}')
            with self.assertRaisesRegex(ValueError, 'expected one speed initialization'):
                zombiebehavior.stage_native_support(root / 'out', root / 'bo2', gscport.API['zombie_runtime'])

    def test_spawner_target_survives_on_its_spawn_location(self):
        struct = entities.spawn_struct({'target': 'window_route', 'origin': '1 2 3'}, 'spawn_group', Counter())
        self.assertEqual(struct['target'], 'window_route')


class GuardedSpeedStateTests(unittest.TestCase):
    def test_braced_source_round_guard_survives_the_speed_slice(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for name, text in {
                'maps/_zombiemode.gsc': 'round_think(){for(;;){if(level.ending_active==false){'
                    'unrelated_side_effect();level.zombie_move_speed=level.round_number*8;level.round_number++;}}}',
                'maps/_zombiemode_spawner.gsc': 'set_run_speed(){self.zombie_move_speed=level.zombie_move_speed;}',
            }.items():
                path = root/name; path.parent.mkdir(parents=True,exist_ok=True); path.write_text(text)
            text = zombiebehavior.source_speed_state(gscport.Sources([root], [], None),gscport.API['zombie_runtime'])
            self.assertIn('if(level.ending_active==false)',text)
            self.assertIn('level.zombie_move_speed=source_round*8',text)
            self.assertNotIn('unrelated_side_effect',text)
            gsc.parse(text, 'guarded_slice')


if __name__ == '__main__':
    unittest.main()
