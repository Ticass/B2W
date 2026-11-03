import json
import tempfile
import unittest
from pathlib import Path

from waw2bo2 import modelisolation


class ModelIsolationTests(unittest.TestCase):
    def test_model_names_do_not_rename_bones(self):
        source = '''main(){
            self setmodel("tag_origin");
            self attach("shared", "tag_origin");
            self gettagorigin("tag_origin");
            self linkto(other, "tag_origin");
            playfxontag(fx, self, "tag_origin");
            api\\compat::waw_playfxontag(fx, self, "tag_origin");
            self api\\compat::waw_attach("shared", "tag_origin");
        }'''
        result = modelisolation.rewrite_script(source, {
            'tag_origin': 'waw_world_model/tag_origin', 'shared': 'waw_world_model/shared'})
        self.assertEqual(result.count('"waw_world_model/tag_origin"'), 1)
        self.assertEqual(result.count('"tag_origin"'), 6)
        self.assertEqual(result.count('"waw_world_model/shared"'), 2)

    def test_bone_names_held_in_a_tag_variable_stay_bones(self):
        # compat waw_playfxontag: tag = "tag_origin" for the power-up glow
        # carrier; renamed, the glow played on a missing tag
        source = '''play(fx, ent, tag){
            if ( !has(ent, tag) ) { ent = carrier(ent); tag = "tag_origin"; }
            playfxontag(fx, ent, tag);
            model = "tag_origin";
            self.tag = "tag_origin";
        }'''
        result = modelisolation.rewrite_script(source, {'tag_origin': 'waw_world_model/tag_origin'})
        self.assertIn('tag = "tag_origin";', result)
        self.assertIn('model = "waw_world_model/tag_origin";', result)
        self.assertIn('self.tag = "waw_world_model/tag_origin";', result)

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
