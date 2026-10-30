import json
import tempfile
import unittest
from pathlib import Path

from waw2bo2 import modelisolation


class ModelIsolationTests(unittest.TestCase):
    def test_render_collision_entities_scripts_and_dynamic_table_agree(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ['xmodel', 'BSP', 'maps/mp/waw', 'fx']:
                (root/name).mkdir(parents=True)
            (root/'xmodel/shared.json').write_text('{"lods":[]}')
            (root/'xmodel/waw_xmodel').mkdir()
            (root/'xmodel/waw_xmodel/weapon.json').write_text('{}')
            (root/'BSP/models.json').write_text(json.dumps({'models':[{'name':'shared'}]}))
            (root/'BSP/staticmodels.json').write_text(json.dumps({'staticModels':[{'name':'shared'}]}))
            (root/'BSP/entities.json').write_text(json.dumps({'entities':[
                {'model':'shared','targetname':'shared'}, {'model':'*12'}]}))
            (root/'maps/mp/waw/test.gsc').write_text('main(){ names[0]="shared"; self setmodel(names[0]); if(self.model=="shared") return; }')
            (root/'fx/piece.efx').write_text('model "shared"')
            renamed = modelisolation.stage(root)
            self.assertEqual(renamed, {'shared':'waw_world_model/shared'})
            self.assertTrue((root/'xmodel/waw_world_model/shared.json').is_file())
            self.assertEqual(json.loads((root/'BSP/models.json').read_text())['models'][0]['name'],renamed['shared'])
            ents = json.loads((root/'BSP/entities.json').read_text())['entities']
            self.assertEqual(ents[0],{'model':renamed['shared'],'targetname':'shared'})
            self.assertEqual(ents[1]['model'],'*12')
            self.assertNotIn('"shared"',(root/'maps/mp/waw/test.gsc').read_text())
            self.assertIn('models["s" + "hared"] = "waw_world_model/shared";',
                          (root/'maps/mp/waw/_waw2bo2_models.gsc').read_text())
            self.assertEqual((root/'fx/piece.efx').read_text(),'model "waw_world_model/shared"')


if __name__ == '__main__':
    unittest.main()
