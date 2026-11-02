import csv
import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from PIL import Image
from waw2bo2 import wawhud, iwi


class WawHudTests(unittest.TestCase):
    def test_lui_live_events_and_native_map_fallback(self):
        try:
            from lupa import LuaRuntime
        except ImportError:
            self.skipTest('lupa is required for the LUI event harness')
        LuaRuntime().execute(Path('tests/waw_hud_runtime.lua').read_text())

    def test_wrapper_matches_checked_in_source(self):
        for wrapper in ('waw_hud', 'waw_hud_entry', 'waw_ammoarea', 'waw_scoreboard', 'waw_otherammo'):
            source = Path(wawhud.__file__).parent / f'compat/{wrapper}.lua'
            hashes = json.loads(source.with_suffix('.json').read_text())
            self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(), hashes['source_sha256'])
            self.assertEqual(hashlib.sha256(source.with_suffix('.luac').read_bytes()).hexdigest(), hashes['bytecode_sha256'])

    def test_missing_source_is_reported_instead_of_using_bo2(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)
            with self.assertRaisesRegex(ValueError, 'WaW HUD source assets missing'):
                wawhud.stage_assets(root, [])

    def test_source_font_alpha_metrics_and_map_weapon_metadata(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); source=root/'source'; project=root/'project'
            project.mkdir(); (project/'content_source').mkdir()
            for folder in ('materials','images','fonts'):(source/folder).mkdir(parents=True)
            atlas=Image.new('RGBA',(8,8),(255,255,255,0));atlas.putpixel((2,2),(255,255,255,123))
            atlas.save(source/'images/atlas.dds')
            for name in (*wawhud.ASSETS,'font'):
                (source/'materials'/f'{name}.json').write_text(json.dumps({'textures':[{'name':'colorMap','image':'atlas'}]}))
            glyph={'letter':'?', 'x0':2,'y0':-4,'dx':5,'pixelWidth':2,'pixelHeight':3,'s0':.25,'t0':.25}
            for name in ('objectiveFont','normalFont'):
                (source/'fonts'/f'{name}.json').write_text(json.dumps({'pixelHeight':8,'material':'font','glyphs':[glyph]}))
            data=wawhud.stage_assets(project,[source])
            self.assertEqual(data['glyphs'],2)
            with (project/wawhud.GLYPHS).open() as f: rows=list(csv.reader(f))
            self.assertEqual(rows[0][2:5],['2','-4','5'])
            image=project/'content_source/images/waw_hud/objective_003f.iwi'
            self.assertEqual(iwi.read_iwi_header(image.read_bytes())['width'],4)
            # The alpha byte survives a lossless atlas crop and IWI conversion.
            self.assertIn(bytes([255,255,255,123]),image.read_bytes())
            bo2=root/'bo2'; stock=bo2/'raw'/wawhud.RUNTIME;stock.parent.mkdir(parents=True)
            compiled=Path(wawhud.__file__).parent/'compat/waw_hud.luac'
            original=compiled.read_bytes()[:16]+b'stock';stock.write_bytes(original)
            ammo=bo2/'raw'/wawhud.AMMO;ammo.parent.mkdir(parents=True);ammo.write_bytes(original)
            for name in (wawhud.SCORE,wawhud.OTHER):
                (bo2/'raw'/name).write_bytes(original+b'beingAnimation\0pluse_low\0')
            entries=wawhud.stage_runtime(bo2,project,'zm_example')
            self.assertIn('stringtable,'+wawhud.MAPS,entries)
            self.assertEqual((project/wawhud.AMMO_BASE).read_bytes(),original)
            self.assertEqual((project/wawhud.MAPS).read_text(),'zm_example,1\n')


if __name__=='__main__': unittest.main()
