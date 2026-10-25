import tempfile
import unittest
from pathlib import Path

from waw2bo2 import gscport
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


if __name__ == '__main__':
    unittest.main()
