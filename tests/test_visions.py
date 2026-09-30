import json
import struct
import tempfile
import unittest
import zipfile
from pathlib import Path

from waw2bo2 import visions


class VisionsTest(unittest.TestCase):
    def test_lut_donor_uses_renderer_role(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root/'materials').mkdir()
            donor = root/'materials/any_map_grade.json'
            donor.write_text(json.dumps({'techniqueSet': 'hdr_create_lut2dv_native', 'textures': [{'image': 'original'}]}))
            self.assertEqual(visions.lut_donor(root), donor)

    def test_invalid_film_data_is_rejected(self):
        for fields in ({'r_filmcontrast': 'nan'}, {'r_filmlighttint': '1 2'}, {'r_filmbrightness': 'inf'}):
            with self.assertRaises(ValueError):
                visions.film((.5,.5,.5), fields)

    def test_stock_precedes_modtools_vision_source(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            project = root/'project'
            (project/'maps/mp/waw').mkdir(parents=True)
            (project/'maps/mp/waw/test.gsc').write_text('main(){ waw_visionsetnaked("stock",0); waw_visionsetnaked("raw",0); }')
            for folder in ('stock', 'modtools'):
                (root/folder/'vision').mkdir(parents=True)
            (root/'stock/vision/stock.vision').write_text('r_filmBrightness ".1"')
            (root/'modtools/vision/stock.vision').write_text('r_filmBrightness ".9"')
            (root/'modtools/vision/raw.vision').write_text('r_filmBrightness ".2"')
            class Stock:
                def root_for(self, kind, name):
                    return 'stock', root/'stock'
            report = visions.stage(project, [], [], Stock(), source_root=root/'modtools')
            self.assertEqual(report['missing'], [])
            self.assertEqual(report['visions']['stock']['source'], str(root/'stock/vision/stock.vision'))
            self.assertEqual(report['visions']['raw']['source'], str(root/'modtools/vision/raw.vision'))

    def test_native_film_contrast_and_invert(self):
        # Derived independently from WaW's native bias/scale constants.
        rgb = (.25, .25, .25)
        for channel in visions.film(rgb, {'r_filmcontrast': '2', 'r_filmbrightness': '.2'}):
            self.assertAlmostEqual(channel, .2)
        self.assertEqual(visions.film(rgb, {'r_filminvert': '1'}), (.75, .75, .75))

    def test_native_tint_and_luminance(self):
        fields = {'r_filmdesaturation': '1', 'r_filmdarktint': '.2 .3 .4', 'r_filmlighttint': '.8 .7 .6'}
        lum = .299
        expected = tuple(lum*(lo+(hi-lo)*lum) for lo,hi in zip((.2,.3,.4),(.8,.7,.6)))
        self.assertEqual(visions.film((1,0,0), fields), expected)

    def test_atlas_matches_native_bo2_identity_layout(self):
        image = visions.atlas([{'r_filmenable': '0'}])
        self.assertEqual(struct.unpack_from('<HH', image, 6), (1024,32))
        for x,y,rgb in [(31,0,(255,0,0)),(32,0,(0,0,8)),(0,31,(0,255,0)),(1023,31,(255,255,255))]:
            pos = 64+(y*1024+x)*4
            self.assertEqual(tuple(image[pos:pos+3]), rgb)

    def test_iwd_priority_discovery_and_absent_reporting(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            project = root/'project'
            (project/'maps/mp/waw').mkdir(parents=True)
            (project/'images').mkdir()
            (project/'BSP').mkdir()
            (project/'maps/mp/waw/test.gsc').write_text('main(){ waw_visionsetnaked("custom", 1); waw_visionsetnaked("absent", 0); }')
            (root/'vision').mkdir()
            (root/'vision/custom.vision').write_text('r_filmBrightness "0"')
            archive = root/'mod.iwd'
            with zipfile.ZipFile(archive, 'w') as z:
                z.writestr('vision/custom.vision', 'r_filmBrightness ".3"')
            report = visions.stage(project, [root], [archive])
            self.assertEqual(report['missing'], ['absent'])
            self.assertIn('mod.iwd:', report['visions']['custom']['source'])
            self.assertIn('r_filmLut "-1"', (project/'vision/waw/custom.vision').read_text())
            self.assertIn('vision_indices["custom"] = 1', (project/'maps/mp/waw/_waw2bo2_visions.gsc').read_text())
            self.assertIn('rawfile,vision/waw/custom.vision', (project/'mod_extra.zone').read_text())


if __name__ == '__main__':
    unittest.main()

