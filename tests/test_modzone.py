import subprocess
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from waw2bo2 import modzone


class ModZoneTests(unittest.TestCase):
    def test_shader_baseline_restores_staging_after_link_failure(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            material = root/'materials/example.json'
            material.parent.mkdir()
            original = b'{"techniqueSet":"waw/runtime_test","textures":[{"image":"original"}]}'
            material.write_bytes(original)
            (root/'bridge_stage.report.json').write_text(json.dumps({'materials': [{
                'file': 'materials/example.json', 'shader_runtime': {
                    'active': [{'slot': 2}], 'native_techset': 'stock_unlit'}}]}))
            with self.assertRaisesRegex(RuntimeError, 'link failed'):
                with modzone.baseline_shader_materials(root, root) as active:
                    self.assertTrue(active)
                    self.assertEqual(json.loads(material.read_text())['techniqueSet'], 'stock_unlit')
                    self.assertEqual(json.loads(material.read_text())['textures'][0]['image'], 'original')
                    raise RuntimeError('link failed')
            self.assertEqual(material.read_bytes(), original)

    def test_required_converted_asset_is_not_silently_dropped(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            template = root / "bo2/mods/zm_test/zm_test.zone"
            template.parent.mkdir(parents=True)
            template.write_text(">game,T6\nweapon,stock_weapon\n")
            failure = subprocess.CompletedProcess([], 1,
                'ERROR: Could not load asset "custom" of type "weapon"\n')
            with patch.object(modzone, "stock_scripts", return_value=set()), \
                 patch.object(modzone.subprocess, "run", return_value=failure) as run:
                with self.assertRaisesRegex(RuntimeError, "will NOT be dropped"):
                    modzone.link_mod(root / "bo2", root / "build", root / "Unlinker.exe",
                                     ["weapon,custom"], root / "converted")
                self.assertIn(str((root / "converted").resolve()), run.call_args.args[0])
            self.assertIn("weapon,custom", (root / "build/zone_source/mod.zone").read_text())


if __name__ == "__main__":
    unittest.main()


class CharacterModelTests(unittest.TestCase):
    def test_alias_heads_of_listed_character_scripts_are_added(self):
        with tempfile.TemporaryDirectory() as temp:
            raw = Path(temp)
            (raw / "character").mkdir()
            (raw / "xmodelalias").mkdir()
            (raw / "xmodel").mkdir()
            (raw / "character/c_zombie.gsc").write_text(
                'main()\n{\n    self setmodel( "body" );\n    self attach( "listed_head", "", 1 );\n}\n')
            (raw / "xmodelalias/heads.gsc").write_text('main()\n{\n    a[0] = "head1";\n    a[1] = "gone";\n    return a;\n}\n')
            for m in ("body", "listed_head", "head1"):
                (raw / f"xmodel/{m}.json").write_text("{}")
            zone = "script,character/c_zombie.gsc\nscript,xmodelalias/heads.gsc\nxmodel,body\nxmodel,listed_head\n"
            self.assertEqual(modzone.character_models(zone, raw), ["head1"])
