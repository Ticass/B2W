import tempfile
import unittest
from pathlib import Path

from waw2bo2 import gscport, zombiebehavior
from waw2bo2.t6api import T6Api


class SoundHelperTests(unittest.TestCase):
    def test_helpers_playing_a_callers_alias_run_from_source(self):
        # Shi No Numa's dog script: play_sound_2D( "ann_vox_dog_start" ) reached
        # BO2's play_sound_2d with WaW's bare alias and played nothing
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'maps').mkdir()
            (root / 'maps/_zombiemode_utility.gsc').write_text(
                'play_sound_2d( sound ) { level thread really_play_2d_sound( sound ); }\n'
                'really_play_2d_sound( sound ) { e = spawn( "script_origin", ( 0, 0, 0 ) ); e playsound( sound ); }\n'
                'play_sound_at_pos( ref, pos ) { playsoundatposition( level.zombie_sounds[ref], pos ); }\n'
                'add_sound( ref, alias ) { level.zombie_sounds[ref] = alias; }\n'
                'fixed_sting( pos ) { playsoundatposition( "sting", pos ); }\n')
            tr = gscport.Translator(gscport.Sources([root], [], None), T6Api({}, {}), gscport.PortReport())
            core = 'maps\\_zombiemode_utility'
            found = sorted(n for n in tr.sources.get(core).functions if tr.plays_argument_sound(core, n))
            self.assertEqual(found, ['play_sound_2d', 'play_sound_at_pos', 'really_play_2d_sound'])


class AnnouncerTests(unittest.TestCase):
    def test_powerup_cases_name_waws_announcer_line(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'maps').mkdir()
            (root / 'maps/_zombiemode_utility.gsc').write_text(
                'play_sound_2d( sound ) { e = spawn( "script_origin", ( 0, 0, 0 ) ); e playsound( sound ); }\n')
            (root / 'maps/_zombiemode_powerups.gsc').write_text(
                '#include maps\\_zombiemode_utility;\n'
                'powerup_grab() { switch ( self.powerup_name ) {\n'
                ' case "nuke": level thread nuke_powerup( self ); break;\n'
                ' case "insta_kill": level thread insta_kill_powerup( self ); break;\n'
                ' case "carpenter": level thread fix_windows(); break; } }\n'
                'nuke_powerup( item ) { play_sound_2d( "nuke_flash" ); level thread delay(); }\n'
                'delay() { wait 3; level thread play_devil_dialog( "nuke_vox" ); }\n'
                'insta_kill_powerup( item ) { level thread play_devil_dialog( "insta_vox" ); }\n'
                'fix_windows() { wait 1; }\n'
                'play_devil_dialog( line ) { play_sound_2d( line ); wait 2; }\n')
            tr = gscport.Translator(gscport.Sources([root], [], None), T6Api({}, {}), gscport.PortReport())
            self.assertEqual(tr.announcer_lines('maps\\_zombiemode_powerups', 'powerup_grab'),
                             {'nuke': ['nuke_flash', 'nuke_vox'], 'insta_kill': ['insta_vox']})
            calls = tr.announcer_hook(root)
            self.assertEqual(calls, ['maps\\mp\\waw\\_waw2bo2_core::waw2bo2_announcer_events();'])
            code = tr.level_state_parts[-1]
            # the first line the bank holds an announcer copy of wins
            self.assertLess(code.index('vox_zmba_waw_nuke_flash'), code.index('vox_zmba_waw_nuke_vox'))
            self.assertIn('game["zmbdialog"]["insta_kill"] = "waw_insta_vox";', code)


class MusicTableTests(unittest.TestCase):
    def test_map_music_states_are_ported_with_bank_names(self):
        # Shi No Numa's round start/end music lived only in its music table
        from waw2bo2 import sounds
        source = ('main()\n{\n declareMusicState("round_begin");\n musicAlias("chalk", 2);\n'
                  ' musicAliasloop("mx_zombie_wave_1", 0, 0.5);\n musicwaittilldone();\n'
                  ' declareMusicState("last_zombie");\n musicAliasloop("mx_last_zombie", 0, 0.5);\n}\n')
        music = sounds.port_music(source, {"chalk", "mx_zombie_wave_1"})
        self.assertEqual(music["lines"], ['declaremusicstate( "round_begin" );', 'musicalias( "waw/chalk", 2 );',
                                          'musicaliasloop( "waw/mx_zombie_wave_1", 0, 0.5 );', 'musicwaittilldone();',
                                          'declaremusicstate( "last_zombie" );', 'musicaliasloop( "null", 0, 0.5 );'])
        self.assertEqual(music["silent"], [{"state": "last_zombie", "alias": "mx_last_zombie"}])


class RemasterMixTests(unittest.TestCase):
    def test_roles_take_the_stock_mix_and_announcer_lines_get_bo2_names(self):
        from waw2bo2 import audioremaster
        with tempfile.TemporaryDirectory() as temp:
            bo2 = Path(temp)
            (bo2 / 'raw/soundbank').mkdir(parents=True)
            head = 'Name,Bus,VolumeGroup,DuckGroup,Duck,PanType,IsMusic,ReverbSend,CenterSend,VolMax\n'
            (bo2 / 'raw/soundbank/zmb_x.all.aliases.csv').write_text(
                head + 'mus_a,bus_music,grp_music,snp_x3,,2d,yes,0,0,90\n'
                       'mus_b,bus_music,grp_music,snp_music,,2d,yes,0,0,88\n'
                       'mus_c,bus_music,grp_music,snp_x3,,2d,yes,0,0,92\n'
                       'mus_radio,bus_fx,grp_ambience,snp_x3,,3d,no,70,0,84\n'
                       'vox_zmba_x,bus_voice,grp_voice,snp_never_duck,,2d,no,0,0,84\n')
            profiles = audioremaster.stock_profiles(bo2)
            self.assertEqual(profiles['music']['mix']['Bus'], 'bus_music')
            self.assertEqual(profiles['music']['volume'], 91)
            self.assertEqual(audioremaster.role('music', False, 'waw/mx_x', set(), set()), 'music')
            self.assertEqual(audioremaster.role('full_vol', False, 'waw/chalk', {'chalk'}, set()), 'music')
            self.assertEqual(audioremaster.role('voice', False, 'waw/insta_vox', set(), set()), 'announcer')
            self.assertIsNone(audioremaster.role('voice', True, 'waw/plr_0_vox', set(), set()))
            rows = [{'Name': 'waw/mx_eggs', 'Bus': 'bus_fx', 'VolumeGroup': 'grp_foley', 'PanType': '3d',
                     'VolMin': '70', 'VolMax': '80'},
                    {'Name': 'waw/insta_vox', 'Bus': 'bus_voice', 'VolumeGroup': 'grp_voice', 'PanType': '3d',
                     'VolMin': '80', 'VolMax': '80'}]
            roles = {'waw/mx_eggs': 'music', 'waw/insta_vox': 'announcer'}
            made = audioremaster.announcer_copies(rows, roles, {'insta_kill': ['insta_vox']}, 'waw_')
            self.assertEqual(made, {'waw/insta_vox': 'vox_zmba_waw_insta_vox'})
            report = audioremaster.remaster(rows, roles, profiles)
            self.assertEqual((rows[0]['Bus'], rows[0]['PanType'], rows[0]['VolMax'], rows[0]['VolMin']),
                             ('bus_music', '2d', '91', '81'))
            self.assertEqual([r['Name'] for r in rows], ['waw/mx_eggs', 'waw/insta_vox', 'vox_zmba_waw_insta_vox'])
            self.assertEqual(rows[2]['DuckGroup'], 'snp_never_duck')
            self.assertEqual(report['roles']['announcer']['aliases'], 2)


class StingerTests(unittest.TestCase):
    def test_round_and_game_over_stingers_take_bo2s_stinger_mix(self):
        # Der Riese's round start chalk sat in BO2's scored-loop mix (snp_x3,
        # front pan) under the game; BO2's own round start is a stinger
        from waw2bo2 import audioremaster
        with tempfile.TemporaryDirectory() as temp:
            bo2 = Path(temp)
            (bo2 / 'raw/soundbank').mkdir(parents=True)
            head = 'Name,Bus,VolumeGroup,DuckGroup,Duck,PanType,IsMusic,ReverbSend,CenterSend,Pan,VolMax\n'
            row = 'mus_zombie_round_start,bus_music,grp_music,snp_music,zmb_mus_rounds,2d,yes,0,0,music_all,{}\n'
            (bo2 / 'raw/soundbank/zmb_a.all.aliases.csv').write_text(head + row.format(96) + row.format(96))
            (bo2 / 'raw/soundbank/zmb_b.all.aliases.csv').write_text(head + row.format(89))
            (bo2 / 'raw/soundbank/zmb_c.all.aliases.csv').write_text(head + row.format(89))
            profiles = audioremaster.stock_profiles(bo2, ['mus_zombie_round_start', 'mus_absent'])
            stinger = profiles['stinger:mus_zombie_round_start']
            self.assertEqual(stinger['volume'], 96)  # the loudest map's level
            self.assertEqual((stinger['mix']['DuckGroup'], stinger['mix']['Pan']), ('snp_music', 'music_all'))
            self.assertNotIn('stinger:mus_absent', profiles)
            amb = ('main()\n{\n declareMusicState("round_begin");\n musicAlias("chalk", 2);\n'
                   ' musicAliasloop("mx_zombie_wave_1", 0, 4);\n declareMusicState ("WAVE_1");\n'
                   ' musicAlias("other", 1);\n}\n')
            stingers = audioremaster.music_stingers(amb, {'round_begin': 'mus_zombie_round_start'})
            self.assertEqual(stingers, {'chalk': 'mus_zombie_round_start'})
            self.assertEqual(audioremaster.role('music', False, 'waw/chalk', {'chalk'}, set(), stingers),
                             'stinger:mus_zombie_round_start')
            self.assertEqual(audioremaster.role('music', False, 'waw/mx_zombie_wave_1', set(), set(), stingers),
                             'music')
            rows = [{'Name': 'waw/chalk', 'Bus': 'bus_music', 'VolumeGroup': 'grp_music', 'PanType': '2d',
                     'DuckGroup': 'snp_x3', 'Pan': 'front', 'VolMin': '96', 'VolMax': '96'}]
            audioremaster.remaster(rows, {'waw/chalk': 'stinger:mus_zombie_round_start'}, profiles)
            self.assertEqual((rows[0]['DuckGroup'], rows[0]['Duck'], rows[0]['Pan'], rows[0]['VolMax']),
                             ('snp_music', 'zmb_mus_rounds', 'music_all', '96'))


class EndGameMusicTests(unittest.TestCase):
    def test_maps_end_game_music_is_extracted_without_its_sp_ending(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'maps').mkdir()
            (root / 'maps/_zombiemode.gsc').write_text(
                'end_game()\n{\n level waittill( "end_game" );\n level.intermission = true;\n'
                ' setmusicstate( "end_of_game" );\n setbusstate( "default" );\n wait 2;\n MissionFailed();\n}\n')
            tr = gscport.Translator(gscport.Sources([root], [], None), T6Api({}, {}), gscport.PortReport())
            self.assertEqual(tr.extract_end_game_music(), 'zombiemode__end_game_music')
            body = tr.level_state_parts[-1]
            self.assertIn('end_of_game', body)
            self.assertNotIn('MissionFailed', body)
            self.assertNotIn('intermission', body)

    def test_patch_matches_bo2s_end_game(self):
        zm = Path(r'C:\Program Files (x86)\Steam\steamapps\common\Call of Duty Black Ops II\raw\maps\mp\zombies\_zm.gsc')
        if not zm.is_file():
            self.skipTest('BO2 raw scripts not installed')
        self.assertEqual(zm.read_text(encoding='utf-8').count(gscport.WAW_GAME_OVER_MUSIC[0]), 1)


class PowerupModelTests(unittest.TestCase):
    def test_map_powerup_models_replace_bo2s(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'maps').mkdir()
            (root / 'maps/_zombiemode_powerups.gsc').write_text(
                'init_powerups() { add_zombie_powerup( "nuke", "zombie_bomb", &"X", "fx" ); }\n')
            tr = gscport.Translator(gscport.Sources([root], [], None), T6Api({}, {}), gscport.PortReport())
            self.assertEqual(tr.powerup_models_hook(), ['maps\\mp\\waw\\_waw2bo2_core::waw2bo2_powerup_models();'])
            self.assertIn('level.zombie_powerups["nuke"].model_name = "zombie_bomb";', tr.level_state_parts[-1])


class SoundTableTests(unittest.TestCase):
    def test_map_sound_table_replaces_bo2s(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'maps').mkdir()
            (root / 'maps/_zombiemode.gsc').write_text(
                'init_sounds() { add_sound( "music_chest", "music_box" ); add_sound( "open_chest", "lid_open" ); }\n')
            (root / 'maps/nazi_zombie_test.gsc').write_text('main() { add_sound( "open_chest", "map_lid" ); }\n')
            tr = gscport.Translator(gscport.Sources([root], [], None), T6Api({}, {}), gscport.PortReport())
            self.assertEqual(tr.sound_table_hook(), ['maps\\mp\\waw\\_waw2bo2_core::waw2bo2_sound_table();'])
            body = tr.level_state_parts[-1]
            self.assertIn('level.zombie_sounds["music_chest"] = "waw/music_box";', body)
            # the map's own registration wins over the framework's
            self.assertIn('level.zombie_sounds["open_chest"] = "waw/map_lid";', body)
            self.assertNotIn('lid_open', body)


class QuoteTriggerTests(unittest.TestCase):
    def test_map_quotes_bind_to_bo2_events(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'maps').mkdir()
            (root / 'maps/_zombiemode_spawner.gsc').write_text(
                'zombie_death_event( zombie ) { zombie waittill( "death" ); }\n'
                'play_death_vo( hit_location, player, mod, zombie ) { }\n')
            (root / 'maps/_zombiemode_powerups.gsc').write_text('powerup_vo( type ) { }\n')
            (root / 'maps/_zombiemode.gsc').write_text('track_players_ammo_count() { }\n')
            api = T6Api({}, {'maps\\mp\\zombies\\_zm_audio': {'player_zombie_kill_vox': 4},
                             'maps\\mp\\zombies\\_zm_powerups': {'powerup_vo': 1}})
            tr = gscport.Translator(gscport.Sources([root], [], None), api, gscport.PortReport())
            self.assertEqual(tr.quote_hooks(), ['maps\\mp\\waw\\_waw2bo2_core::waw2bo2_quote_triggers();'])
            body = tr.level_state_parts[-1]
            self.assertIn('replacefunc( maps\\mp\\zombies\\_zm_audio::player_zombie_kill_vox, '
                          'maps\\mp\\waw\\_waw2bo2_core::zombiemode_spawner__play_death_vo );', body)
            self.assertIn('replacefunc( maps\\mp\\zombies\\_zm_powerups::powerup_vo, '
                          'maps\\mp\\waw\\_waw2bo2_core::zombiemode_powerups__powerup_vo );', body)
            self.assertIn('level thread maps\\mp\\waw\\_waw2bo2_core::zombiemode_spawner__zombie_death_event( zombie );', body)
            self.assertIn('level thread maps\\mp\\waw\\_waw2bo2_core::zombiemode__track_players_ammo_count();', body)

    def test_spawner_runs_the_maps_death_event(self):
        zb = zombiebehavior
        text = zb._DEATH_EVENT + zb._DEATH_VOCALS + zb._CLOSE_DAMAGE_VOX + zb._KILL_NOTIFY + '    level notify( "zom_kill" );\n'
        out = zb.waw_quote_rules(text, 'spawner', 'waw2bo2_zombie_death_event( zombie )\n')
        self.assertIn('level thread zombie_death_event( self );\n'
                      '    maps\\mp\\waw\\_waw2bo2_core::waw2bo2_zombie_death_event( self );', out)
        for gone in ('do_zombies_playvocals', 'create_and_play_dialog', 'zombie.attacker notify'):
            self.assertNotIn(gone, out)
        self.assertIn('level notify( "zom_kill" );', out)
        self.assertEqual(zb.waw_quote_rules(text, 'spawner', ''), text)

    def test_killstreak_timer_runs_the_maps(self):
        self.assertTrue(gscport.source_drives('maps\\_zombiemode_utility', 'player_killstreak_timer'))
        self.assertFalse(gscport.source_drives('maps\\_zombiemode_utility', 'add_spawner'))
        # the map's own spawn_zombie runs; compat creates the actor (StalingradSpawn)
        self.assertTrue(gscport.source_drives('maps\\_zombiemode_utility', 'spawn_zombie'))
        self.assertTrue(gscport.source_drives('maps\\_zombiemode', 'round_think'))
        self.assertFalse(gscport.source_drives('maps\\_zombiemode', 'end_game'))


if __name__ == '__main__':
    unittest.main()
