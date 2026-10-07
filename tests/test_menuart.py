from pathlib import Path
import json
import tempfile
import unittest
from PIL import Image
from waw2bo2.launcher import Settings
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

    def test_text_only_and_clear_remove_previous_generated_art(self):
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
        self.assertFalse((project / 'menu.json').exists())
