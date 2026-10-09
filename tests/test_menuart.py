from pathlib import Path
import json
import tempfile
import unittest
import wave
from PIL import Image
from waw2bo2.launcher import Settings
from waw2bo2.mapmenu import validate_settings
from waw2bo2.menuart import SIZES, read_art, stage_art, validate_art
from waw2bo2.modzone import stage_menu_assets
from waw2bo2.iwi import read_iwi_header


class MenuArtworkTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.settings = Settings(project='zm_example', menu_title='Example Town', menu_description='Survive the town.')

    def artwork(self):
        for role, size in SIZES.items():
            image = Image.new('RGBA', size, (23, 45, 67, 255))
            if role == 'blit':
                image.putpixel((0, 0), (23, 45, 67, 0))
            path = self.root / (role + '.png')
            image.save(path)
            setattr(self.settings, 'menu_' + role, str(path))

    def test_exports_pixels_materials_and_streaming_registration(self):
        self.artwork()
        project = self.root / 'project'
        stage_art(self.settings, project)
        entries = stage_menu_assets(project, self.settings.project)
        self.assertIn('>ipak,zm_example_menu', entries)
        for role, size in {**SIZES, 'icon': (256, 256)}.items():
            pixels = (project / 'images' / f'menu_zm_example_{role}.iwi').read_bytes()
            header = read_iwi_header(pixels)
            self.assertEqual((header['width'], header['height']), size)
            self.assertEqual(pixels[64:68], bytes((23, 45, 67, 0 if role == 'blit' else 255)))
        stream = json.loads((project / 'images/streaming.json').read_text())
        self.assertTrue(all(value == 2 for value in stream['streamingMode'].values()))
        self.assertIn('Example Town', (project / 'english/localizedstrings/menu_zm_example.str').read_text())

    def test_wrong_resolution_and_opaque_blit_are_rejected(self):
        path = self.root / 'bad.png'
        Image.new('RGBA', (256, 256)).save(path)
        with self.assertRaisesRegex(ValueError, '512 × 256'):
            read_art(str(path), 'blit')
        Image.new('RGB', SIZES['blit']).save(path)
        with self.assertRaisesRegex(ValueError, 'transparent'):
            read_art(str(path), 'blit')

    def test_partial_upload_is_rejected(self):
        self.settings.menu_large = 'large.png'
        with self.assertRaisesRegex(ValueError, 'all three'):
            validate_art(self.settings)

    def test_text_only_and_clear_keep_default_branding_without_art(self):
        self.artwork()
        project = self.root / 'project'
        stage_art(self.settings, project)
        for role in SIZES:
            setattr(self.settings, 'menu_' + role, '')
        stage_art(self.settings, project)
        self.assertEqual(stage_menu_assets(project, 'zm_example'), ['localize,menu_zm_example'])
        self.assertFalse((project / 'materials/menu_zm_example_map.json').exists())
        self.settings.menu_title = self.settings.menu_description = ''
        stage_art(self.settings, project)
        data = json.loads((project / 'menu.json').read_text())
        self.assertEqual((data['title'], data['description']), ('zm_example', 'zm_example: World at War custom map'))
        self.assertNotIn('icon', data)
        self.assertEqual(stage_menu_assets(project, 'zm_example'), ['localize,menu_zm_example'])

    def test_globe_coordinates_reach_menu_metadata_and_are_range_checked(self):
        project = self.root / 'project'
        self.settings.menu_longitude, self.settings.menu_latitude = '-73.5', '45.5'
        stage_art(self.settings, project)
        data = json.loads((project / 'menu.json').read_text())
        self.assertEqual((data['longitude'], data['latitude']), (-73.5, 45.5))
        self.settings.menu_latitude = '91'
        with self.assertRaisesRegex(ValueError, 'Latitude'):
            validate_settings(self.settings)
        self.settings.menu_latitude = 'nan'
        with self.assertRaisesRegex(ValueError, 'Latitude'):
            validate_settings(self.settings)

    def test_loading_song_stages_streamed_music_bank_and_clears_it(self):
        source = self.root / 'song.wav'
        with wave.open(str(source), 'wb') as stream:
            stream.setnchannels(2)
            stream.setsampwidth(2)
            stream.setframerate(48000)
            stream.writeframes(b'\x01\x00\x02\x00' * 480)
        project = self.root / 'project'
        self.settings.loading_song = str(source)
        stage_art(self.settings, project)
        bank = json.loads((project / 'menu.json').read_text())['loading_song']['bank']
        self.assertIn('soundbank,' + bank, stage_menu_assets(project, 'zm_example'))
        aliases = (project / 'soundbank' / (bank + '.aliases.csv')).read_text()
        self.assertIn('mus_load_zm_example_patch', aliases)
        self.settings.loading_song = ''
        stage_art(self.settings, project)
        self.assertNotIn('loading_song', json.loads((project / 'menu.json').read_text()))
        self.assertFalse((project / 'soundbank' / (bank + '.aliases.csv')).exists())
